#pragma once

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rex::graphics {

// M10-CPU-F: wake policy of the D3D12 streaming replay worker.
//
// The command processor (CP) publishes its deferred command stream every 32
// commands. By default the worker spins 4096 PAUSE instructions after catching
// up before it sleeps, and the CP wakes a sleeping worker at every publish, so
// in practice the worker spins for as long as the CP produces a submission.
// With d3d12_replay_batched_wake on, the worker spins only 128 PAUSEs, and the
// CP wakes a sleeping worker only once 512 commands have been published since
// it was seen asleep, or at once after a block seal and when the submission
// finishes (FinishStreaming always wakes). Publishing itself (visibility
// granularity) is unchanged.
//
// Pure (no globals, no OS calls); unit-tested in
// tests/unit/graphics/replay_wake_test.cpp.

inline constexpr uint32_t kReplaySpinPausesDefault = 4096;
inline constexpr uint32_t kReplaySpinPausesBatched = 128;
inline constexpr uint32_t kReplayWakeBatchCommands = 512;
inline constexpr uint32_t kReplayWakeLineVersion = 1;

constexpr uint32_t ReplaySpinLimit(bool batched) {
  return batched ? kReplaySpinPausesBatched : kReplaySpinPausesDefault;
}

// Decides on the CP thread, after each publish, whether to notify the worker.
class ReplayWakeBatcher {
 public:
  enum class Decision : uint8_t {
    kNone,      // the worker is awake (it will see the publish by itself)
    kDeferred,  // the worker sleeps; not enough work pending yet
    kWake,      // notify: the batch is full
    kForced,    // notify: a forced wake (block seal, stream start or finish)
  };

  // Starts a submission. `batched` is fixed for the whole submission.
  void Begin(bool batched) {
    batched_ = batched;
    pending_ = 0;
    force_pending_ = false;
  }

  // The writer sealed a block: the next publish wakes a sleeping worker.
  void NoteSeal() { force_pending_ = true; }

  // `commands` were published (made visible); `worker_sleeping` is the
  // worker's flag read after the publish fence; `force` wakes regardless of
  // the batch.
  Decision OnPublish(uint32_t commands, bool worker_sleeping, bool force) {
    if (!worker_sleeping) {
      pending_ = 0;
      force_pending_ = false;
      return Decision::kNone;
    }
    if (!batched_) {
      return Decision::kWake;
    }
    pending_ += commands;
    if (force || force_pending_) {
      pending_ = 0;
      force_pending_ = false;
      return Decision::kForced;
    }
    if (pending_ >= kReplayWakeBatchCommands) {
      pending_ = 0;
      return Decision::kWake;
    }
    return Decision::kDeferred;
  }

  bool batched() const { return batched_; }
  uint32_t pending() const { return pending_; }

 private:
  bool batched_ = false;
  bool force_pending_ = false;
  uint32_t pending_ = 0;
};

// Per-minute worker idle counters.
struct ReplayWakeCounters {
  uint64_t submissions = 0;          // streamed submissions
  uint64_t batched_submissions = 0;  // of those, with the batched wake
  uint64_t idle_episodes = 0;        // times the worker caught up and spun
  uint64_t spin_pauses = 0;
  uint64_t spin_us = 0;
  uint64_t sleeps = 0;
  uint64_t wakes = 0;           // CP notifications at publishes (batch full or unbatched)
  uint64_t forced_wakes = 0;    // CP notifications forced by a seal or stream start
  uint64_t deferred_wakes = 0;  // publishes to a sleeping worker without a notification
};

// wake_batch=0 means every publish (the default policy).
// MCLA_REPLAY_WAKE v=1 batched= spin_limit= wake_batch= submissions=
// batched_submissions= idle_episodes= spin_pauses= spin_ms= sleeps= wakes=
// forced_wakes= deferred_wakes=
inline std::string FormatReplayWakeLine(bool batched, const ReplayWakeCounters& c) {
  return fmt::format(
      "MCLA_REPLAY_WAKE v={} batched={} spin_limit={} wake_batch={} submissions={} "
      "batched_submissions={} idle_episodes={} spin_pauses={} spin_ms={:.3f} sleeps={} wakes={} "
      "forced_wakes={} deferred_wakes={}",
      kReplayWakeLineVersion, batched ? 1 : 0, ReplaySpinLimit(batched),
      batched ? kReplayWakeBatchCommands : 0u, c.submissions, c.batched_submissions,
      c.idle_episodes, c.spin_pauses, double(c.spin_us) / 1000.0, c.sleeps, c.wakes, c.forced_wakes,
      c.deferred_wakes);
}

}  // namespace rex::graphics
