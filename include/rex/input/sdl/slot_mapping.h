/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace rex::input::sdl {

inline constexpr uint32_t kControllerSlotCount = 4;

// The first accepted device deterministically owns physical storage slot 0.
// Once a controller is present, preserve a valid free player index reported by
// SDL; otherwise use the lowest free physical slot.
constexpr std::optional<uint32_t> ChooseControllerSlot(int32_t reported_player_index,
                                                       uint32_t occupied_slot_mask) {
  if (occupied_slot_mask == 0)
    return 0;
  if (reported_player_index >= 0 &&
      reported_player_index < static_cast<int32_t>(kControllerSlotCount)) {
    const uint32_t reported_slot = static_cast<uint32_t>(reported_player_index);
    if (!(occupied_slot_mask & (uint32_t(1) << reported_slot)))
      return reported_slot;
  }
  for (uint32_t slot = 0; slot < kControllerSlotCount; ++slot) {
    if (!(occupied_slot_mask & (uint32_t(1) << slot))) {
      return slot;
    }
  }
  return std::nullopt;
}

// Xbox user slot zero follows the most recently active physical controller.
// The selected controller is hidden at its original physical index to avoid
// exposing the same device through two guest slots. Other physical slots keep
// their stable indices.
constexpr std::optional<uint32_t> ResolveLogicalControllerSlot(
    uint32_t logical_slot, std::optional<uint32_t> active_physical_slot,
    uint32_t connected_slot_mask) {
  if (logical_slot >= kControllerSlotCount)
    return std::nullopt;
  if (logical_slot == 0) {
    if (active_physical_slot && *active_physical_slot < kControllerSlotCount &&
        (connected_slot_mask & (uint32_t(1) << *active_physical_slot))) {
      return active_physical_slot;
    }
    return std::nullopt;
  }
  if (active_physical_slot && logical_slot == *active_physical_slot)
    return std::nullopt;
  return connected_slot_mask & (uint32_t(1) << logical_slot) ? std::optional<uint32_t>(logical_slot)
                                                             : std::nullopt;
}

// Select the connected controller with the newest meaningful input. Devices
// without activity are tied by their stable physical slot, which gives a
// deterministic fallback after disconnect.
constexpr std::optional<uint32_t> ChooseMostRecentlyActiveController(
    uint32_t connected_slot_mask,
    const std::array<uint64_t, kControllerSlotCount>& activity_sequences) {
  std::optional<uint32_t> selected;
  uint64_t selected_sequence = 0;
  for (uint32_t slot = 0; slot < kControllerSlotCount; ++slot) {
    if (!(connected_slot_mask & (uint32_t(1) << slot)))
      continue;
    if (!selected || activity_sequences[slot] > selected_sequence) {
      selected = slot;
      selected_sequence = activity_sequences[slot];
    }
  }
  return selected;
}

// SDL's standard South face button is enum value 0. Additional controls may
// intentionally alias Xbox A for gameplay, but they must not satisfy the
// physical face-button challenge used by the slot audit.
constexpr bool IsInputSlotAuditChallengeButton(uint32_t sdl_button) {
  return sdl_button == 0;
}

}  // namespace rex::input::sdl
