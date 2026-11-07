#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include <fmt/format.h>

namespace rex::graphics {

// M11-007e: deferred fence writes (d3d12_fence_write_deferred).
//
// On the Xenos, EVENT_WRITE_SHD makes the GPU store a value into guest memory
// when the pipeline reaches the event, that is after all the work before it
// in the ring has finished. The guest's fences (D3D InsertFence, the
// BlockOnFence ring-space and frame-throttle waits, resource Fence /
// ReadFence) are built on that word. The host command processor used to
// store the value when it parsed the packet, ahead of the host GPU, so a
// guest waiting for a fence could run ahead of the GPU work it waited for.
//
// With the deferred mode the command processor queues {address, value,
// submission} instead, where submission is the host submission whose
// completion means the GPU has passed the event (the open submission when
// the packet is parsed while one is open, else the last ended one), and
// stores the value once the submission fence has reached it. Submissions
// complete in order, so the queue is FIFO and flushes a prefix.
//
// Rules the command processor follows (deadlock analysis in the l77 notes):
// - a write is queued whenever the mode is on, or the queue is not empty
//   (a later fence never lands before an earlier one, also while the mode is
//   being turned off);
// - the queue is polled (non-blocking) at every packet boundary, in every
//   WAIT_REG_MEM poll, in every idle-loop iteration and after every
//   synchronous fence wait (CheckSubmissionFence);
// - the guest may spin on a queued word, so the command processor never
//   waits for the guest with an outstanding write whose submission is still
//   open: idle or in WAIT_REG_MEM it ends the open submission; while busy it
//   ends it at a draw boundary when the GPU has run out of work;
// - idle with outstanding writes, it waits on the submission fence or on new
//   ring data (whichever comes first), never on the ring alone.

// kind=fence_defer INFO lines: the first this many queued writes after each
// enable (one parse line and one write line each).
inline constexpr uint32_t kFenceDeferLines = 8;

struct DeferredFenceWrite {
  uint32_t address = 0;     // guest physical, 4-byte aligned
  uint32_t value = 0;       // as stored (already in guest byte order)
  uint64_t submission = 0;  // the store waits until the fence reaches this
  uint64_t frame = 0;       // command processor frame when parsed
  uint64_t parse_us = 0;    // steady clock when parsed
  uint64_t seq = 0;         // queued writes since the last enable
};

struct DeferredFenceCounters {
  uint64_t events = 0;         // fence events seen in the deferred mode
  uint64_t deferred = 0;       // writes queued
  uint64_t immediate = 0;      // writes stored at once (GPU already past)
  uint64_t written = 0;        // queued writes stored
  uint64_t max_frames = 0;     // frames between parse and store
  uint64_t max_us = 0;         // time between parse and store
  uint64_t flush_on_idle = 0;  // queued writes stored by an idle or WAIT_REG_MEM poll
  uint64_t early_submits = 0;  // submissions ended for an outstanding write
  uint64_t max_depth = 0;      // longest queue
};

// The submission whose completion means the GPU has passed an event parsed
// now: the open one, or (none open) the last ended one. 0 = nothing was ever
// submitted (fence initial value, already complete).
inline uint64_t DeferredFenceTargetSubmission(bool submission_open, uint64_t submission_current) {
  if (submission_open) {
    return submission_current;
  }
  return submission_current ? submission_current - 1 : 0;
}

// Off with an empty queue: the old parse-time store (byte-identical path).
inline bool ShouldDeferFenceWrite(bool enabled, bool queue_empty) {
  return enabled || !queue_empty;
}

enum class DeferredFenceAction : uint8_t {
  kNone,           // nothing to do now (flush or keep parsing)
  kEndSubmission,  // the head waits for the open submission: end it
  kWaitFence,      // the head is submitted: wait for the fence (idle only)
};

// What the command processor does for the queue head after a flush.
// waiting: the command processor is idle or in WAIT_REG_MEM (it would wait
// for the guest, which may wait for this write). gpu_idle: every ended
// submission has completed.
inline DeferredFenceAction PlanDeferredFenceProgress(uint64_t head_submission, bool submission_open,
                                                     uint64_t submission_current,
                                                     uint64_t completed, bool waiting) {
  if (head_submission <= completed) {
    return DeferredFenceAction::kNone;
  }
  if (head_submission >= submission_current) {
    // Only an open submission can hold it.
    if (!submission_open) {
      return DeferredFenceAction::kNone;
    }
    const bool gpu_idle = completed + 1 >= submission_current;
    return (waiting || gpu_idle) ? DeferredFenceAction::kEndSubmission : DeferredFenceAction::kNone;
  }
  return waiting ? DeferredFenceAction::kWaitFence : DeferredFenceAction::kNone;
}

class DeferredFenceWriteQueue {
 public:
  bool empty() const { return writes_.empty(); }
  size_t size() const { return writes_.size(); }
  uint64_t head_submission() const { return writes_.empty() ? 0 : writes_.front().submission; }
  const DeferredFenceWrite& head() const { return writes_.front(); }
  const DeferredFenceWrite& back() const { return writes_.back(); }

  // Counts the event. Returns false when the write should be stored now (the
  // queue is empty and the GPU is already past the submission); else queues
  // it (with its seq) and returns true.
  bool Push(DeferredFenceWrite write, uint64_t completed) {
    ++counters_.events;
    if (writes_.empty() && write.submission <= completed) {
      ++counters_.immediate;
      return false;
    }
    write.seq = next_seq_++;
    writes_.push_back(write);
    ++counters_.deferred;
    counters_.max_depth = std::max<uint64_t>(counters_.max_depth, writes_.size());
    return true;
  }

  // Stores (store(const DeferredFenceWrite&, frames, us)) every queued write
  // whose submission has completed, in order; stops at the first that has
  // not. Returns the number stored.
  template <typename Store>
  size_t Flush(uint64_t completed, uint64_t frame, uint64_t now_us, bool waiting, Store&& store) {
    size_t stored = 0;
    while (!writes_.empty() && writes_.front().submission <= completed) {
      const DeferredFenceWrite& w = writes_.front();
      const uint64_t frames = frame >= w.frame ? frame - w.frame : 0;
      const uint64_t us = now_us >= w.parse_us ? now_us - w.parse_us : 0;
      store(w, frames, us);
      counters_.max_frames = std::max(counters_.max_frames, frames);
      counters_.max_us = std::max(counters_.max_us, us);
      ++counters_.written;
      if (waiting) {
        ++counters_.flush_on_idle;
      }
      writes_.pop_front();
      ++stored;
    }
    return stored;
  }

  void NoteEarlySubmit() { ++counters_.early_submits; }

  // A new enable restarts the INFO line budget.
  void RestartLines() { next_seq_ = 0; }

  void Clear() { writes_.clear(); }

  const DeferredFenceCounters& counters() const { return counters_; }
  DeferredFenceCounters TakeCounters() {
    DeferredFenceCounters taken = counters_;
    counters_ = {};
    return taken;
  }

 private:
  std::deque<DeferredFenceWrite> writes_;
  DeferredFenceCounters counters_;
  uint64_t next_seq_ = 0;
};

// Fields appended to the MCLA readback minute/mark line while the mode is on
// or something was counted in the window (empty otherwise, so the line is
// unchanged with the mode off).
inline std::string FormatDeferredFenceFields(const DeferredFenceCounters& c, bool enabled,
                                             size_t depth) {
  if (!enabled && !c.events && !c.written && !c.early_submits && !depth) {
    return std::string();
  }
  return fmt::format(
      " fence_defer={} fence_events={} fence_deferred={} fence_immediate={} fence_written={} "
      "fence_deferred_max_frames={} fence_deferred_ms_max={:.3f} fence_flush_on_idle={} "
      "fence_early_submits={} fence_queue_max={} fence_queue_now={}",
      int(enabled), c.events, c.deferred, c.immediate, c.written, c.max_frames,
      double(c.max_us) / 1000.0, c.flush_on_idle, c.early_submits, c.max_depth, depth);
}

}  // namespace rex::graphics
