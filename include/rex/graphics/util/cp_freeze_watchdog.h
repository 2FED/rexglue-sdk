/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    MCLA-R, 2026 - KI-069 command-processor freeze watchdog
 */

#pragma once

// KI-069: a watchdog thread notices when the D3D12 command processor (CP)
// completes no frame for longer than d3d12_cp_freeze_watchdog_ms and logs one
// MCLA_CP_FREEZE begin line per episode (and an end line when frames resume).
// The episode logic and the line format are pure; unit-tested in
// tests/unit/graphics/cp_freeze_watchdog_test.cpp.

#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rex::graphics {

// What the CP thread was doing, as published by it.
enum class CpWaitState : uint8_t {
  kBusy = 0,        // executing packets
  kRingIdle = 1,    // waiting for the guest to write more commands
  kWaitRegMem = 2,  // inside a WAIT_REG_MEM block
};

inline const char* CpWaitStateName(uint8_t state) {
  switch (CpWaitState(state)) {
    case CpWaitState::kBusy:
      return "busy";
    case CpWaitState::kRingIdle:
      return "ring_idle";
    case CpWaitState::kWaitRegMem:
      return "wrm";
  }
  return "?";
}

class CpFreezeEpisodes {
 public:
  enum class Action : uint8_t { kNone, kBegin, kEnd };

  // `frames` counts completed frames; `last_frame_ns` is the time of the last
  // one (both published by the CP). threshold_ms 0 disables new episodes.
  Action Observe(uint64_t now_ns, uint64_t frames, uint64_t last_frame_ns, uint32_t threshold_ms) {
    if (!frames) {
      return Action::kNone;  // no frame yet (boot)
    }
    if (in_episode_) {
      if (frames != episode_frames_) {
        in_episode_ = false;
        episode_ms_ = last_frame_ns > episode_last_frame_ns_
                          ? (last_frame_ns - episode_last_frame_ns_) / 1000000ull
                          : 0;
        return Action::kEnd;
      }
      return Action::kNone;
    }
    if (!threshold_ms) {
      return Action::kNone;
    }
    const uint64_t stall_ns = now_ns > last_frame_ns ? now_ns - last_frame_ns : 0;
    if (stall_ns > uint64_t(threshold_ms) * 1000000ull) {
      in_episode_ = true;
      ++episodes_;
      episode_frames_ = frames;
      episode_last_frame_ns_ = last_frame_ns;
      stall_ms_ = stall_ns / 1000000ull;
      return Action::kBegin;
    }
    return Action::kNone;
  }

  bool in_episode() const { return in_episode_; }
  uint64_t episodes() const { return episodes_; }
  // Stall at the begin line.
  uint64_t stall_ms() const { return stall_ms_; }
  // Last frame before the episode to the first frame after it.
  uint64_t episode_ms() const { return episode_ms_; }
  uint64_t episode_frames() const { return episode_frames_; }

 private:
  bool in_episode_ = false;
  uint64_t episodes_ = 0;
  uint64_t episode_frames_ = 0;
  uint64_t episode_last_frame_ns_ = 0;
  uint64_t stall_ms_ = 0;
  uint64_t episode_ms_ = 0;
};

// Everything one begin line reports.
struct CpFreezeReport {
  uint64_t episode = 0;
  uint64_t stall_ms = 0;
  uint64_t frames = 0;
  uint32_t cp_tid = 0;
  uint8_t cp_state = 0;
  bool perf_split = false;
  std::string scope = "-";
  uint32_t scope_depth = 0;
  uint64_t scope_ms = 0;
  bool copy_active = false;
  uint64_t copy_seq = 0;
  uint64_t copy_ms = 0;
  uint32_t copy_address = 0;
  uint32_t copy_length = 0;
  bool log_active = false;
  bool log_async = false;
  uint64_t log_queue = 0;
  uint64_t log_dropped = 0;
  uint64_t log_op_ms = 0;
  uint32_t log_op_thread = 0;
  bool log_op_flush = false;
  uint32_t log_writer = 0;
};

inline std::string FormatCpFreezeBegin(const CpFreezeReport& r) {
  const char* log_mode = !r.log_active ? "none" : (r.log_async ? "async" : "sync");
  const char* log_op = !r.log_op_ms ? "none" : (r.log_op_flush ? "flush" : "write");
  return fmt::format(
      "MCLA_CP_FREEZE v=1 kind=begin episode={} stall_ms={} frames={} cp_tid={} cp_state={} "
      "perf_split={} scope={} scope_depth={} scope_ms={} copy_active={} copy_seq={} copy_ms={} "
      "copy_addr={:08X} copy_len={} log_mode={} log_queue={} log_dropped={} log_op={} "
      "log_op_ms={} log_op_thread={} log_writer={}",
      r.episode, r.stall_ms, r.frames, r.cp_tid, CpWaitStateName(r.cp_state), r.perf_split ? 1 : 0,
      r.scope, r.scope_depth, r.scope_ms, r.copy_active ? 1 : 0, r.copy_seq, r.copy_ms,
      r.copy_address, r.copy_length, log_mode, r.log_queue, r.log_dropped, log_op, r.log_op_ms,
      r.log_op_thread, r.log_writer);
}

inline std::string FormatCpFreezeEnd(uint64_t episode, uint64_t episode_ms, uint64_t frames) {
  return fmt::format("MCLA_CP_FREEZE v=1 kind=end episode={} episode_ms={} frames={}", episode,
                     episode_ms, frames);
}

}  // namespace rex::graphics
