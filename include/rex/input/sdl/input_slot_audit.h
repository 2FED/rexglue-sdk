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

namespace rex::input {
struct X_INPUT_STATE;
}

namespace rex::input::sdl {

// Arms the opt-in input-slot audit after the title checkpoint. The audit is
// one-shot; repeated calls are ignored.
void ArmInputSlotAudit(const char* phase);

namespace internal {

// Internal hooks shared by the SDL driver and XAM input bridge. These expose
// no device identity or guest pointers and are not project-consumer APIs.
void RecordInputSlotDeviceAssigned(uint32_t slot);
void RecordInputSlotDeviceRemoved(uint32_t slot);
void RecordInputSlotDeviceRejected();
void RecordInputSlotSdlAEdge(uint32_t slot, bool down);
void RecordInputSlotGuestState(uint32_t requested_user_index, uint32_t actual_user_index,
                               uint32_t result, const X_INPUT_STATE* state);

}  // namespace internal
}  // namespace rex::input::sdl
