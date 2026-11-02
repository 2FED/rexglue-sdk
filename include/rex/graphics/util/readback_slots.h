#pragma once

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rex::graphics {

// M10-002: decision logic of the two-slot read-back resolve path
// (D3D12CommandProcessor::IssueCopy_ReadbackResolvePath), extracted so that it
// can be planned before the GPU copy is recorded and unit-tested.
//
// Every read-back key (guest address, length) has two READBACK-heap slots.
// Resolve N of a key writes slot w = current_index, then reads:
// - full: its own slot w after a full queue wait;
// - fast, some: the previous slot 1 - w (written by resolve N-1 of the key),
//   unless that slot is missing, too small or unmapped (a miss: full queue
//   wait, read slot w), or the l55 sync rules force it
//   (d3d12_readback_sync_composite: full queue wait, read slot w);
// - some only: the guest copy happens only on a miss, a forced resolve, or
//   when the per-key counter reaches 4 (every 4th resolve), else it is
//   deferred.
// Then current_index flips.
//
// Step 1 (d3d12_readback_elide_unread): the bytes the GPU copies into slot w
// at resolve N are read only by resolve N itself (miss, forced, full) or by
// the next resolve N+1 of the same key, when N+1 reads the previous slot. In
// some mode N+1 reads the previous slot only if it is neither a miss nor
// forced and its counter reaches the interval, that is only if
//   next_resolves_since_copy + 1 >= 4,
// where next_resolves_since_copy is the counter that N leaves behind. Whether
// N+1 is a miss or forced is unknown at N, but neither reads slot w, so the
// condition is exact and conservative. Later resolves (N+2 and on) never read
// the content N wrote: N+2 writes slot w again (or elides it, which the same
// argument covers by induction), and an eviction (60 frames, or the 256-key
// cap) removes both slots so the next resolve of the key is a miss.
// So in some mode the GPU copy of N is elided exactly when
//   !own_slot && next_resolves_since_copy + 1 < 4, own_slot = miss || forced.
// The slot allocation, the counters, the miss test (slot presence, size,
// mapping) and current_index are unchanged, so the guest receives identical
// bytes.
//
// Guard: if the mode is switched at run time from some to fast between two
// resolves of a key, the next resolve may read a previous slot whose copy was
// elided. The plan detects that (slot_elided) before recording and turns it
// into a miss (record, wait, read the own slot). It never fires in a constant
// some mode (unit-tested) and is counted (guard_misses).
//
// Pure (no globals, no OS or D3D12 calls); unit-tested in
// tests/unit/graphics/readback_slots_test.cpp.

inline constexpr uint32_t kReadbackSlotsSomeInterval = 4;
inline constexpr uint32_t kReadbackLineVersion = 1;

enum class ReadbackPlanMode : uint8_t {
  kFast,
  kSome,
  kFull,  // also used for a disabled mode seen inside the path (it waits)
};

inline const char* ReadbackPlanModeName(ReadbackPlanMode mode) {
  switch (mode) {
    case ReadbackPlanMode::kFast:
      return "fast";
    case ReadbackPlanMode::kSome:
      return "some";
    case ReadbackPlanMode::kFull:
      return "full";
  }
  return "unknown";
}

// Per-key slot state that the decision depends on (the D3D12 entry also holds
// the buffers, sizes, mappings and the last-used frame).
struct ReadbackSlotState {
  uint32_t current_index = 0;
  uint32_t resolves_since_copy = 0;
  // The last resolve that wrote this slot did not record its GPU copy (the
  // slot holds older bytes that nobody may read).
  bool slot_elided[2] = {false, false};
};

struct ReadbackPlanInput {
  ReadbackPlanMode mode = ReadbackPlanMode::kSome;
  // Slot 1 - current_index exists, is mapped and is large enough for the
  // written length (fast and some read it).
  bool previous_slot_usable = false;
  // The l55 sync rules force this resolve unless it is a miss
  // (ShouldForceReadbackSync with cache_miss = false).
  bool force_unless_miss = false;
  // d3d12_readback_elide_unread.
  bool elide_unread = false;
};

struct ReadbackPlan {
  uint32_t write_index = 0;
  uint32_t read_index = 0;
  // full: wait for the queue, read the own slot.
  bool wait_full = false;
  // The previous slot is unusable: wait for the queue, read the own slot.
  bool miss = false;
  // The previous slot would be read but its copy was elided (only after a
  // mode switch); handled as a miss.
  bool guard_miss = false;
  // The l55 sync rules force a wait and a read of the own slot.
  bool forced = false;
  // The guest copy happens now (should_copy).
  bool copies_now = true;
  // The GPU copy into slot write_index is recorded.
  bool record_gpu = true;
  // resolves_since_copy after this resolve (some mode; other modes keep it).
  uint32_t next_resolves_since_copy = 0;

  bool own_slot() const { return read_index == write_index; }
};

inline ReadbackPlan PlanReadbackResolve(const ReadbackSlotState& state,
                                        const ReadbackPlanInput& in) {
  ReadbackPlan plan;
  plan.write_index = state.current_index & 1;
  plan.next_resolves_since_copy = state.resolves_since_copy;
  if (in.mode == ReadbackPlanMode::kFull) {
    plan.wait_full = true;
    plan.read_index = plan.write_index;
    return plan;
  }
  const uint32_t previous_index = 1 - plan.write_index;
  plan.miss = !in.previous_slot_usable;
  plan.forced = in.force_unless_miss && !plan.miss;
  bool own = plan.miss || plan.forced;
  if (in.mode == ReadbackPlanMode::kSome) {
    plan.copies_now = own || state.resolves_since_copy + 1 >= kReadbackSlotsSomeInterval;
    plan.next_resolves_since_copy = plan.copies_now ? 0 : state.resolves_since_copy + 1;
  }
  // The previous slot is read now only by a guest copy that is not own-slot.
  if (!own && plan.copies_now && state.slot_elided[previous_index]) {
    plan.guard_miss = true;
    own = true;
  }
  plan.read_index = own ? plan.write_index : previous_index;
  if (in.mode == ReadbackPlanMode::kSome && in.elide_unread) {
    plan.record_gpu = own || plan.next_resolves_since_copy + 1 >= kReadbackSlotsSomeInterval;
  }
  return plan;
}

// What a resolve that reads the previous slot finds there (diagnostics:
// d3d12_livery_diag v1, d3d12_livery_diag2 v2/v3). A slot is written when the
// last resolve that used it as its own slot recorded its GPU copy; a slot
// whose last copy was elided (slot_elided) or that no recorded copy ever
// reached (submission_written == 0) holds no bytes of that resolve. With
// elision only a deferred resolve (no guest copy) can have read_index on such
// a slot: a guest copy from it is turned into a guard miss by the plan.
// - kUnwritten: the slot holds no bytes of its last resolve (nothing the
//   guest could be given; the elision artifact, not a hazard by itself);
// - kUnfenced: written, but its submission is not known complete;
// - kComplete: written and its submission is known complete.
enum class ReadbackPreviousSlot : uint8_t {
  kComplete,
  kUnfenced,
  kUnwritten,
};

inline ReadbackPreviousSlot ClassifyReadbackPreviousSlot(bool slot_elided,
                                                         uint64_t submission_written,
                                                         uint64_t submission_completed) {
  if (slot_elided || !submission_written) {
    return ReadbackPreviousSlot::kUnwritten;
  }
  return submission_written <= submission_completed ? ReadbackPreviousSlot::kComplete
                                                    : ReadbackPreviousSlot::kUnfenced;
}

// The GPU copy into slot `index` was recorded (or elided).
inline void NoteReadbackSlotWrite(ReadbackSlotState& state, uint32_t index, bool recorded) {
  state.slot_elided[index & 1] = !recorded;
}

// Applies the plan once the resolve went through its waits (the point at
// which the path updates the counter and flips the slots).
inline void CommitReadbackResolve(ReadbackSlotState& state, const ReadbackPlan& plan,
                                  ReadbackPlanMode mode) {
  if (mode == ReadbackPlanMode::kSome) {
    state.resolves_since_copy = plan.next_resolves_since_copy;
  }
  state.current_index = 1 - (state.current_index & 1);
}

// MCLA_READBACK v=1 counters (always on, command processor thread). A minute
// line resets them; an F10 mark line reports the minute so far.
struct ReadbackCounters {
  uint64_t frames = 0;
  // Read-back resolves that reached the slot decision.
  uint64_t resolves = 0;
  // Resolves that left the path after the decision without reaching the
  // guest copy step (buffer, descriptor or wait failures, unsupported
  // formats).
  uint64_t aborted = 0;
  uint64_t copies_now = 0;
  uint64_t copies_deferred = 0;
  uint64_t copies_skipped = 0;
  uint64_t misses = 0;
  uint64_t guard_misses = 0;
  uint64_t forced = 0;
  uint64_t full_waits = 0;
  uint64_t gpu_copies_recorded = 0;
  uint64_t gpu_copies_elided = 0;
  uint64_t bytes_gpu_copied = 0;
  uint64_t bytes_gpu_elided = 0;
  uint64_t bytes_cpu_copied = 0;
  uint64_t memcpy_ns = 0;
  uint64_t readback_wait_ns = 0;
  // GPU timestamps around the recorded read-back blocks (downscale or
  // shared-memory copy, slot copy and their barriers), summed when their
  // submission completes.
  uint64_t gpu_ticks = 0;
  uint64_t gpu_blocks = 0;
  uint64_t gpu_blocks_dropped = 0;

  void CountPlan(const ReadbackPlan& plan, uint32_t length) {
    ++resolves;
    misses += plan.miss ? 1 : 0;
    guard_misses += plan.guard_miss ? 1 : 0;
    full_waits += plan.wait_full ? 1 : 0;
    if (plan.record_gpu) {
      ++gpu_copies_recorded;
      bytes_gpu_copied += length;
    } else {
      ++gpu_copies_elided;
      bytes_gpu_elided += length;
    }
  }
};

inline std::string ReadbackReport(const ReadbackCounters& c, const char* kind, const char* reason,
                                  uint64_t frame, const char* mode_name, bool elide, bool timing,
                                  uint64_t timestamp_frequency) {
  double gpu_ms =
      timestamp_frequency ? double(c.gpu_ticks) * 1000.0 / double(timestamp_frequency) : 0.0;
  double frames = double(c.frames ? c.frames : 1);
  return fmt::format(
      "MCLA_READBACK v={} kind={} reason={} frame={} frames={} mode={} elide={} timing={} "
      "resolves={} aborted={} copies_now={} copies_deferred={} copies_skipped={} misses={} "
      "guard_misses={} forced={} full_waits={} gpu_copies_recorded={} gpu_copies_elided={} "
      "bytes_gpu_copied={} bytes_gpu_elided={} bytes_cpu_copied={} memcpy_ms={:.3f} "
      "readback_wait_ms={:.3f} gpu_readback_ms={:.3f} gpu_readback_ms_per_frame={:.3f} "
      "gpu_blocks={} gpu_blocks_dropped={}",
      kReadbackLineVersion, kind, reason, frame, c.frames, mode_name, elide ? 1 : 0, timing ? 1 : 0,
      c.resolves, c.aborted, c.copies_now, c.copies_deferred, c.copies_skipped, c.misses,
      c.guard_misses, c.forced, c.full_waits, c.gpu_copies_recorded, c.gpu_copies_elided,
      c.bytes_gpu_copied, c.bytes_gpu_elided, c.bytes_cpu_copied, double(c.memcpy_ns) / 1e6,
      double(c.readback_wait_ns) / 1e6, gpu_ms, gpu_ms / frames, c.gpu_blocks,
      c.gpu_blocks_dropped);
}

}  // namespace rex::graphics
