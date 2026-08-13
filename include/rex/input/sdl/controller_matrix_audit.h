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

void ArmControllerMatrixAudit(const char* phase);

namespace internal {

void RecordControllerMatrixDeviceAssigned(uint32_t slot);
void RecordControllerMatrixDeviceRemoved(uint32_t slot);
void RecordControllerMatrixDeviceRejected();
void RecordControllerMatrixStaleEvent(bool axis);
void RecordControllerMatrixSdlButton(uint32_t slot, uint16_t button, bool down);
void RecordControllerMatrixSdlAnalog(uint32_t slot, uint16_t analog_mask);
void RecordControllerMatrixFocus(bool focused);
void RecordControllerMatrixGuestState(uint32_t requested_user_index, uint32_t actual_user_index,
                                      uint32_t result, const X_INPUT_STATE* state);
bool ShouldSuppressControllerMatrixResumeRumble(bool stopping);
void RecordControllerMatrixResumeRumbleSuppressed(uint32_t slot, uint8_t motor_mask);
void RecordControllerMatrixRumble(uint32_t slot, uint8_t motor_mask, bool supported,
                                  uint32_t result);

}  // namespace internal
}  // namespace rex::input::sdl
