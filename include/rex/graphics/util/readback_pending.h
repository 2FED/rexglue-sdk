#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/graphics/readback_range.h>

namespace rex::graphics {

// M11-007c: the read-back copies that are recorded on the host GPU but not
// yet in guest RAM, shared between the command processor thread (owner of
// the read-back slots and submissions) and the threads that call
// rex::gpu::ReadbackSyncQueryRange / ReadbackSyncEnsureRange.
//
// One entry per read-back key (MakeReadbackResolveKey). The pending copy is
// always the newest recorded GPU copy of the key that nobody delivered:
// - BeginResolve (before a resolve allocates or records into its own slot
//   w): a pending copy in slot w is dropped (w is about to be rewritten or
//   reallocated, so it can no longer be read);
// - DeliverByCommandProcessor: the command processor's own guest memcpy
//   (fast delivery, the some cadence, a miss, a forced or full resolve)
//   runs under the table lock; a pending copy in the slot it reads is
//   marked delivered;
// - EndResolve: a recorded copy becomes the pending copy (or, when the
//   resolve read its own slot, is delivered at once);
// - Erase / Clear: eviction and shutdown, before the slot buffers are
//   released.
// A caller (DeliverPending) copies a pending slot only if it is still the
// same generation and not delivered, under the lock; the command processor
// never records into that slot or releases it while the lock is held, so
// the slot is not reused during the copy, and guest RAM never goes back to
// an older resolve (the command processor's own copies and the callers'
// copies are serialised and both only deliver the newest pending copy or
// a newer one).
//
// The lock is held only for map updates and at most one slot memcpy (no
// waits inside it). The fence waits happen outside the lock.

// kind=ensure INFO lines: the first this many ensure calls per key.
inline constexpr uint32_t kReadbackEnsureLinesPerKey = 4;

inline bool ReadbackRangesOverlap(uint32_t base_a, uint32_t length_a, uint32_t base_b,
                                  uint32_t length_b) {
  if (!length_a || !length_b) {
    return false;
  }
  uint64_t end_a = uint64_t(base_a) + length_a;
  uint64_t end_b = uint64_t(base_b) + length_b;
  return uint64_t(base_a) < end_b && uint64_t(base_b) < end_a;
}

// A snapshot of one pending copy (taken under the lock, used without it).
struct ReadbackPendingRef {
  uint64_t key = 0;
  uint64_t generation = 0;
  // The submission that holds the GPU copy (its fence value).
  uint64_t submission = 0;
  // The submission was not yet executed at the snapshot.
  bool unsubmitted = false;
  uint32_t base = 0;
  uint32_t length = 0;
  bool exempt = false;
};

// What an ensure call does for one pending copy.
enum class ReadbackEnsureStep : uint8_t {
  // The copy's fence was reached: copy now.
  kDeliverNow,
  // Submitted, not complete: wait on its fence.
  kWait,
  // Still in the open submission: ask the command processor to execute it,
  // then wait on its fence.
  kRequestSubmitThenWait,
};

inline ReadbackEnsureStep PlanReadbackEnsure(uint64_t submission, uint64_t completed,
                                             uint64_t next_submission) {
  if (completed >= submission) {
    return ReadbackEnsureStep::kDeliverNow;
  }
  // Submissions below next_submission were executed (their fence value is
  // signalled by the GPU alone).
  return submission < next_submission ? ReadbackEnsureStep::kWait
                                      : ReadbackEnsureStep::kRequestSubmitThenWait;
}

enum class ReadbackDeliverResult : uint8_t {
  // Copied by this call.
  kDelivered,
  // That copy (or a newer one) was already delivered (by the command
  // processor or another caller).
  kAlreadyDelivered,
  // A newer GPU copy replaced it before anybody delivered it (retry with
  // the new pending copy if time remains).
  kSuperseded,
  // The key was evicted or the table cleared.
  kGone,
};

class ReadbackPendingTable {
 public:
  struct KeyInfo {
    uint32_t base = 0;
    uint32_t length = 0;
    uint32_t slot_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    bool exempt = false;
  };

  // Command processor thread. The frame and the first submission not yet
  // executed (submission_current_ after the last EndSubmission).
  void SetFrame(uint64_t frame) { frame_.store(frame, std::memory_order_relaxed); }
  void SetNextSubmission(uint64_t next) { next_submission_.store(next, std::memory_order_release); }
  uint64_t next_submission() const { return next_submission_.load(std::memory_order_acquire); }

  void BeginResolve(uint64_t key, uint32_t write_slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.pending && it->second.pending_slot == (write_slot & 1)) {
      it->second.pending = false;
    }
  }

  // Runs copy() (the command processor's guest memcpy from `read_slot`)
  // under the lock and marks a pending copy in that slot delivered.
  template <typename Copy>
  void DeliverByCommandProcessor(uint64_t key, uint32_t read_slot, Copy&& copy) {
    std::lock_guard<std::mutex> lock(mutex_);
    copy();
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return;
    }
    Entry& e = it->second;
    if (e.pending && e.pending_slot == (read_slot & 1)) {
      e.pending = false;
      e.delivered_generation = e.generation;
      e.delivered_resolve_frame = e.pending_resolve_frame;
      e.delivered_ever = true;
    }
  }

  // After the resolve's guest copy step. `recorded`: the GPU copy into
  // `slot` was recorded into `submission` (mapped at `data`);
  // `delivered_own`: the resolve copied its own slot into guest RAM.
  void EndResolve(uint64_t key, const KeyInfo& info, bool recorded, uint32_t slot,
                  uint64_t submission, const void* data, bool delivered_own) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = entries_[key];
    e.info = info;
    uint64_t frame = frame_.load(std::memory_order_relaxed);
    e.last_resolve_frame = frame;
    if (!recorded) {
      return;
    }
    ++e.generation;
    e.pending_slot = slot & 1;
    e.pending_submission = submission;
    e.pending_data = data;
    e.pending_resolve_frame = frame;
    if (delivered_own) {
      e.pending = false;
      e.delivered_generation = e.generation;
      e.delivered_resolve_frame = frame;
      e.delivered_ever = true;
    } else {
      e.pending = data != nullptr;
    }
  }

  void Erase(uint64_t key) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(key);
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
  }

  // Any thread.
  bool Query(uint32_t phys, uint32_t len, rex::gpu::ReadbackRangeInfo* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry* best = FindNewestOverlapLocked(phys, len, false);
    if (!best) {
      return false;
    }
    if (out) {
      uint64_t frame = frame_.load(std::memory_order_relaxed);
      uint64_t next = next_submission_.load(std::memory_order_acquire);
      out->key_base = best->info.base;
      out->key_length = best->info.length;
      out->slot_size = best->info.slot_size;
      out->width = best->info.width;
      out->height = best->info.height;
      out->format = best->info.format;
      out->exempt = best->info.exempt;
      out->pending = best->pending;
      out->unsubmitted = best->pending && best->pending_submission >= next;
      out->copy_age_frames =
          best->delivered_ever ? FrameAge(frame, best->delivered_resolve_frame) : UINT32_MAX;
      out->resolve_age_frames = FrameAge(frame, best->last_resolve_frame);
    }
    return true;
  }

  // The newest overlapping pending copy (false: none).
  bool FindPending(uint32_t phys, uint32_t len, ReadbackPendingRef* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t key = 0;
    const Entry* best = FindNewestOverlapLocked(phys, len, true, &key);
    if (!best) {
      return false;
    }
    out->key = key;
    out->generation = best->generation;
    out->submission = best->pending_submission;
    out->unsubmitted = best->pending_submission >= next_submission_.load(std::memory_order_acquire);
    out->base = best->info.base;
    out->length = best->info.length;
    out->exempt = best->info.exempt;
    return true;
  }

  // Copies the pending slot of `ref` with copy(base, src, length) under the
  // lock if it is still that generation and undelivered. The caller must
  // have seen the copy's fence reached.
  template <typename Copy>
  ReadbackDeliverResult DeliverPending(const ReadbackPendingRef& ref, Copy&& copy) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(ref.key);
    if (it == entries_.end()) {
      return ReadbackDeliverResult::kGone;
    }
    Entry& e = it->second;
    if (e.delivered_generation >= ref.generation) {
      return ReadbackDeliverResult::kAlreadyDelivered;
    }
    if (!e.pending || e.generation != ref.generation) {
      return ReadbackDeliverResult::kSuperseded;
    }
    copy(e.info.base, e.pending_data, e.info.length);
    e.pending = false;
    e.delivered_generation = e.generation;
    e.delivered_resolve_frame = e.pending_resolve_frame;
    e.delivered_ever = true;
    return ReadbackDeliverResult::kDelivered;
  }

  // The kind=ensure line budget: true for the first `limit` calls per key.
  bool TakeEnsureLine(uint64_t key, uint32_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end() || it->second.ensure_lines >= limit) {
      return false;
    }
    ++it->second.ensure_lines;
    return true;
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

 private:
  struct Entry {
    KeyInfo info;
    uint64_t last_resolve_frame = 0;
    // Incremented by every recorded GPU copy.
    uint64_t generation = 0;
    bool pending = false;
    uint32_t pending_slot = 0;
    uint64_t pending_submission = 0;
    const void* pending_data = nullptr;
    uint64_t pending_resolve_frame = 0;
    uint64_t delivered_generation = 0;
    uint64_t delivered_resolve_frame = 0;
    bool delivered_ever = false;
    uint32_t ensure_lines = 0;
  };

  static uint32_t FrameAge(uint64_t now, uint64_t then) {
    uint64_t age = now > then ? now - then : 0;
    return age > UINT32_MAX - 1 ? UINT32_MAX - 1 : uint32_t(age);
  }

  const Entry* FindNewestOverlapLocked(uint32_t phys, uint32_t len, bool pending_only,
                                       uint64_t* key_out = nullptr) const {
    const Entry* best = nullptr;
    uint64_t best_key = 0;
    for (const auto& [key, e] : entries_) {
      if (pending_only && !e.pending) {
        continue;
      }
      if (!ReadbackRangesOverlap(phys, len, e.info.base, e.info.length)) {
        continue;
      }
      // Newest resolve first; ties by key (deterministic).
      if (!best || e.last_resolve_frame > best->last_resolve_frame ||
          (e.last_resolve_frame == best->last_resolve_frame && key > best_key)) {
        best = &e;
        best_key = key;
      }
    }
    if (best && key_out) {
      *key_out = best_key;
    }
    return best;
  }

  std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> entries_;
  std::atomic<uint64_t> frame_{0};
  std::atomic<uint64_t> next_submission_{1};
};

// Counters of the ensure calls (any thread; reported and reset on the
// MCLA_READBACK_SYNC minute line by the command processor).
struct ReadbackEnsureCounters {
  std::atomic<uint64_t> calls{0};
  // Calls that found a pending copy, delivered it (this call or meanwhile
  // by the command processor), timed out.
  std::atomic<uint64_t> pending{0};
  std::atomic<uint64_t> delivered{0};
  std::atomic<uint64_t> delivered_by_cp{0};
  std::atomic<uint64_t> timeouts{0};
  // End-submission requests raised by callers, and honoured by the command
  // processor.
  std::atomic<uint64_t> submit_requests{0};
  std::atomic<uint64_t> submits{0};
  std::atomic<uint64_t> wait_us{0};
  std::atomic<uint64_t> max_wait_us{0};

  void CountWait(uint64_t us) {
    wait_us.fetch_add(us, std::memory_order_relaxed);
    uint64_t max = max_wait_us.load(std::memory_order_relaxed);
    while (us > max && !max_wait_us.compare_exchange_weak(max, us, std::memory_order_relaxed)) {}
  }

  struct Snapshot {
    uint64_t calls = 0, pending = 0, delivered = 0, delivered_by_cp = 0, timeouts = 0,
             submit_requests = 0, submits = 0, wait_us = 0, max_wait_us = 0;
  };

  // Reads and resets (the minute line).
  Snapshot Take() {
    Snapshot s;
    s.calls = calls.exchange(0, std::memory_order_relaxed);
    s.pending = pending.exchange(0, std::memory_order_relaxed);
    s.delivered = delivered.exchange(0, std::memory_order_relaxed);
    s.delivered_by_cp = delivered_by_cp.exchange(0, std::memory_order_relaxed);
    s.timeouts = timeouts.exchange(0, std::memory_order_relaxed);
    s.submit_requests = submit_requests.exchange(0, std::memory_order_relaxed);
    s.submits = submits.exchange(0, std::memory_order_relaxed);
    s.wait_us = wait_us.exchange(0, std::memory_order_relaxed);
    s.max_wait_us = max_wait_us.exchange(0, std::memory_order_relaxed);
    return s;
  }

  Snapshot Peek() const {
    Snapshot s;
    s.calls = calls.load(std::memory_order_relaxed);
    s.pending = pending.load(std::memory_order_relaxed);
    s.delivered = delivered.load(std::memory_order_relaxed);
    s.delivered_by_cp = delivered_by_cp.load(std::memory_order_relaxed);
    s.timeouts = timeouts.load(std::memory_order_relaxed);
    s.submit_requests = submit_requests.load(std::memory_order_relaxed);
    s.submits = submits.load(std::memory_order_relaxed);
    s.wait_us = wait_us.load(std::memory_order_relaxed);
    s.max_wait_us = max_wait_us.load(std::memory_order_relaxed);
    return s;
  }
};

// Fields appended to the MCLA_READBACK_SYNC minute and mark lines: empty
// while no ensure call was made in the window (the line is unchanged then).
inline std::string ReadbackEnsureReport(const ReadbackEnsureCounters::Snapshot& s) {
  if (!s.calls && !s.submits) {
    return std::string();
  }
  return fmt::format(
      " ensure_calls={} ensure_pending={} ensure_delivered={} ensure_delivered_by_cp={} "
      "ensure_timeouts={} ensure_wait_ms_total={:.3f} ensure_wait_ms_max={:.3f} "
      "ensure_submit_requests={} ensure_submits={}",
      s.calls, s.pending, s.delivered, s.delivered_by_cp, s.timeouts, double(s.wait_us) / 1000.0,
      double(s.max_wait_us) / 1000.0, s.submit_requests, s.submits);
}

}  // namespace rex::graphics
