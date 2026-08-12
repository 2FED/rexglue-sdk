/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <optional>

namespace rex::input::sdl {

inline constexpr uint32_t kControllerSlotCount = 4;

// The first accepted device deterministically owns Xbox user slot 0. Once a
// controller is present, preserve a valid free player index reported by SDL;
// otherwise use the lowest free slot.
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

// SDL's standard South face button is enum value 0. Additional controls may
// intentionally alias Xbox A for gameplay, but they must not satisfy the
// physical face-button challenge used by the slot audit.
constexpr bool IsInputSlotAuditChallengeButton(uint32_t sdl_button) {
  return sdl_button == 0;
}

}  // namespace rex::input::sdl
