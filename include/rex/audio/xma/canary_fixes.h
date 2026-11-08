/**
 ******************************************************************************
 * ReXGlue - XMA decoder fixes ported from Xenia Canary (xma_canary_fixes)    *
 ******************************************************************************
 *
 * Ported from Xenia Canary, src/xenia/apu/xma_context_new.cc:
 *   Copyright 2024 Xenia Canary. All rights reserved.
 *   Released under the BSD license (BSD-3-Clause), see
 *   https://github.com/xenia-canary/xenia-canary/blob/canary_experimental/LICENSE
 *
 * Upstream commits (adapted to the ReXGlue XmaContext, not copied verbatim):
 *   6bef1fbed "[XMA] Drain the current frame before ending the XMA work loop"
 *   7015473a7 "Follow the sub-stream skip chain past frameless packets"
 *             (in rex/audio/xma/frame_walk.h FindNextPacketReadOffset)
 *   5dd1cdbbf "[XMA] Resolve XMA loop_start to a frame boundary"
 *             (PacketFrames::next_frame_start in frame_walk.h)
 *   ade7e610b "[XMA] Fix stall detection false positive in Work loop"
 *
 * The decoder (xma_context.cpp) applies these only with xma_canary_fixes.
 */

#pragma once

#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>

// Default false for the first owner A/B (l79).
REXCVAR_DECLARE(bool, xma_canary_fixes);

namespace rex::audio::xma {

// Canary 6bef1fbed: Work() must not leave its loop while the current frame is
// only partly handed to the output ring. Consume() moves at most
// subframe_decode_count blocks per pass, and Work() has already cleared
// is_enabled, so the remainder would wait for the next XMAEnableContext (a
// game polling for that remainder never re-enables the context).
inline bool ShouldEndWorkLoop(bool any_input_valid, uint32_t error_status,
                              uint32_t remaining_subframes, bool canary) {
  const bool input_done = !any_input_valid || error_status == 4;
  if (!input_done) {
    return false;
  }
  return !canary || remaining_subframes == 0;
}

// Canary ade7e610b: a pass in which Decode did not move the read offset and
// produced no frame (and that did not start with a partly delivered frame,
// where Decode skips on purpose while Consume drains) cannot make progress:
// end the loop instead of spinning with the context lock held.
inline bool IsNoProgressPass(uint32_t pre_read_offset, uint32_t pre_remaining_subframes,
                             uint32_t post_read_offset, uint32_t post_remaining_subframes) {
  return pre_remaining_subframes == 0 && post_read_offset == pre_read_offset &&
         post_remaining_subframes == 0;
}

// Canary 5dd1cdbbf: on a loop restart, a loop_start that is not a frame start
// (games write it one bit short) is moved to the first frame start at or after
// it. Returns the packet-relative offset to decode from.
inline uint32_t ResolveLoopStartOffset(bool loop_start_pending, uint32_t relative_offset,
                                       uint32_t next_frame_start) {
  if (!loop_start_pending || next_frame_start == relative_offset) {
    return relative_offset;
  }
  return next_frame_start;
}

// Per-minute counters (appended to MCLA_XMA kind=minute).
struct CanaryMinuteTotals {
  uint64_t drain_passes = 0;    // 6bef1fbed: passes kept to drain a partly delivered frame
  uint64_t skip_follows = 0;    // 7015473a7: next-packet searches that followed a skip chain
  uint64_t loop_resolved = 0;   // 5dd1cdbbf: loop starts moved to a frame boundary
  uint64_t no_progress = 0;     // ade7e610b: Work loops ended by a no-progress pass
};

inline std::string FormatCanaryMinuteFields(const CanaryMinuteTotals& c, bool fix) {
  return fmt::format(
      "canary_drain={} canary_skip_follow={} canary_loop_resolved={} canary_no_progress={} "
      "canary_fix={}",
      c.drain_passes, c.skip_follows, c.loop_resolved, c.no_progress, int(fix));
}

}  // namespace rex::audio::xma
