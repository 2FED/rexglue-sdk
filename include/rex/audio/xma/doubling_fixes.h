/**
 ******************************************************************************
 * ReXGlue - KI-071 XMA doubled-voice fixes: pure decision helpers            *
 ******************************************************************************
 */

// Pure parts of the three cvar-gated XMA decoder fixes for KI-071 (doubled
// cut-scene dialogue). The decoder (xma_context.cpp) applies them; they are
// kept free of decoder state so the unit tests can cover them directly.

#pragma once

#include <stdint.h>

#include <rex/cvar.h>

// C1: after an input-buffer swap, start at packet (next_packet_index - packet_count)
// of the new buffer instead of packet 0 (multi-stream carry-over).
REXCVAR_DECLARE(bool, xma_swap_carry_packet);
// C2: clear output_buffer_valid only when this Work wrote at least one block.
REXCVAR_DECLARE(bool, xma_valid_only_when_written);
// C3: re-check is_enabled after taking the context lock in Work, and do not
// write back the guest-owned output_buffer_read_offset.
REXCVAR_DECLARE(bool, xma_work_lock_recheck);
// Trace: paced INFO line at each input-buffer swap and a per-minute counter line.
REXCVAR_DECLARE(bool, xma_swap_trace);

namespace rex::audio::xma {

// C1. Where a context continues in the new input buffer after a swap.
struct SwapCarry {
  // Packets past the end of the old buffer that the packet skip chain points at.
  uint32_t carry = 0;
  // Packet of the new buffer to continue at (carry, or 0 on fallback).
  uint32_t start_packet = 0;
  // The carry was not inside the new buffer; start_packet fell back to 0.
  bool fallback = false;
};

// next_packet_index = packet_index + skip + 1 of the last packet decoded in the
// old buffer. A next index inside the old buffer (a swap forced because no later
// packet starts a frame) carries nothing. A carry that does not fit the new
// buffer (packet count 0 or carry >= count) falls back to packet 0.
constexpr SwapCarry ComputeSwapCarry(uint32_t next_packet_index, uint32_t old_packet_count,
                                     uint32_t new_packet_count) {
  SwapCarry result;
  if (next_packet_index <= old_packet_count) {
    return result;
  }
  result.carry = next_packet_index - old_packet_count;
  if (result.carry < new_packet_count) {
    result.start_packet = result.carry;
  } else {
    result.fallback = true;
  }
  return result;
}

// C2. The guest treats a clear output_buffer_valid as "the whole ring is
// readable" (sub_823ED670 returns output_buffer_block_count). read == write is
// therefore only "full" when this Work's writes closed the ring; with nothing
// written read == write means the guest drained it (empty), and valid must stay.
constexpr bool ShouldClearOutputValid(bool ring_read_equals_write, uint32_t blocks_written,
                                      bool only_when_written) {
  if (!ring_read_equals_write) {
    return false;
  }
  return !only_when_written || blocks_written > 0;
}

// C2 event: upstream would clear valid on a ring that this Work did not write.
constexpr bool IsStaleRingReplay(bool initial_valid, bool ring_read_equals_write,
                                 uint32_t blocks_written) {
  return initial_valid && ring_read_equals_write && blocks_written == 0;
}

// C3. Work checks is_enabled before taking the lock; a guest lock (Disable +
// Block) can land between the check and the lock. enabled_after_lock is the
// flag read once the lock is held.
constexpr bool ShouldRunWorkAfterLock(bool enabled_after_lock, bool recheck) {
  return enabled_after_lock || !recheck;
}

// C3. output_buffer_read_offset (dword 9) is written by the guest consume
// helper (sub_823ED700) and read by the decoder; Work never changes it. Storing
// the host snapshot back can only undo a guest consume made meanwhile.
constexpr uint32_t MergeOutputReadOffset(uint32_t guest_current, uint32_t host_snapshot,
                                         bool preserve_guest) {
  return preserve_guest ? guest_current : host_snapshot;
}

}  // namespace rex::audio::xma
