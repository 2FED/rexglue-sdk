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

#include <rex/input/input.h>
#include <rex/system/xtypes.h>

namespace rex::input::sdl {

constexpr uint16_t kControllerMatrixButtonMask = 0xF3FF;
constexpr int16_t kControllerMatrixLeftDeadzone = 7849;
constexpr int16_t kControllerMatrixRightDeadzone = 8689;
constexpr uint8_t kControllerMatrixTriggerThreshold = 30;
constexpr uint16_t kControllerMatrixAnalogMask = 0x03FF;
constexpr uint32_t kControllerMatrixButtonSourceCount = 14;
constexpr uint32_t kControllerMatrixAnalogSourceCount = 10;

struct ControllerMatrixResumeSeed {
  uint16_t button_sdl_mask;
  uint16_t button_guest_mask;
  uint8_t rumble_pattern_mask;
  uint8_t rumble_stop_mask;
  uint32_t source_sequence;
  bool input_ready;
};

struct ControllerMatrixHotplugResumeSeed {
  ControllerMatrixResumeSeed completed;
  uint16_t analog_sdl_mask;
  uint16_t analog_guest_mask;
  uint32_t source_sequence;
  bool focus_lost;
  bool focus_neutral;
  bool focus_gained;
};

constexpr ControllerMatrixResumeSeed GetControllerMatrixResumeSeed(bool resume) {
  return resume ? ControllerMatrixResumeSeed{kControllerMatrixButtonMask,
                                             kControllerMatrixButtonMask,
                                             7,
                                             7,
                                             kControllerMatrixButtonSourceCount,
                                             true}
                : ControllerMatrixResumeSeed{};
}

constexpr uint32_t ControllerMatrixExpectedCurrentRumbleRecords(bool resume) {
  return resume ? 0 : 6;
}

constexpr ControllerMatrixHotplugResumeSeed GetControllerMatrixHotplugResumeSeed(
    bool hotplug_resume) {
  return hotplug_resume ? ControllerMatrixHotplugResumeSeed{GetControllerMatrixResumeSeed(true),
                                                            kControllerMatrixAnalogMask,
                                                            kControllerMatrixAnalogMask,
                                                            kControllerMatrixButtonSourceCount +
                                                                kControllerMatrixAnalogSourceCount,
                                                            true,
                                                            true,
                                                            true}
                        : ControllerMatrixHotplugResumeSeed{};
}

constexpr bool AreControllerMatrixResumeModesValid(bool resume, bool hotplug_resume) {
  return !(resume && hotplug_resume);
}

// SDL3 returns true when rumble was accepted and false on failure.
constexpr X_RESULT SdlBoolToXResult(bool result) {
  return result ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED;
}

// A zero command is always submitted so focus loss can stop motors. Nonzero
// commands are acknowledged but suppressed while the title is unfocused.
constexpr bool ShouldSubmitRumble(bool has_focus, bool stopping) {
  return has_focus || stopping;
}

constexpr bool ShouldSuppressControllerMatrixRumble(bool resume, bool stopping) {
  return resume && !stopping;
}

constexpr bool IsControllerInputActive(bool driver_active, bool has_focus) {
  return driver_active && has_focus;
}

constexpr bool ShouldIgnoreControllerMatrixFocusChurn(bool focus_lost, bool focus_neutral,
                                                      bool focus_gained) {
  return focus_lost && focus_neutral && focus_gained;
}

constexpr bool IsSingleControllerMatrixBit(uint16_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

constexpr bool IsCausalControllerMatrixPair(uint16_t pending_source, uint16_t guest_value) {
  return IsSingleControllerMatrixBit(pending_source) && pending_source == guest_value;
}

constexpr uint8_t ControllerMatrixRumblePatternBit(uint8_t motor_mask) {
  return motor_mask == 1 ? 1 : motor_mask == 2 ? 2 : motor_mask == 3 ? 4 : 0;
}

constexpr bool IsControllerMatrixInputReady(bool armed, uint8_t rumble_pattern_mask,
                                            uint8_t rumble_stop_mask, uint8_t active_rumble_pattern,
                                            uint16_t buttons, uint16_t analog) {
  return armed && rumble_pattern_mask == 7 && rumble_stop_mask == 7 && active_rumble_pattern == 0 &&
         buttons == 0 && analog == 0;
}

constexpr bool IsControllerMatrixSurfaceComplete(uint16_t button_sdl_mask,
                                                 uint16_t button_guest_mask,
                                                 uint16_t analog_sdl_mask,
                                                 uint16_t analog_guest_mask, bool pending_input) {
  return button_sdl_mask == kControllerMatrixButtonMask &&
         button_guest_mask == kControllerMatrixButtonMask &&
         analog_sdl_mask == kControllerMatrixAnalogMask &&
         analog_guest_mask == kControllerMatrixAnalogMask && !pending_input;
}

// Classifies only the standard Xbox-compatible SDL button surface. Extended
// paddles, misc buttons and touchpads may still map for gameplay, but cannot
// satisfy the physical matrix audit.
constexpr uint16_t StandardSdlButtonToXInput(uint8_t button) {
  constexpr uint16_t buttons[] = {
      X_INPUT_GAMEPAD_A,
      X_INPUT_GAMEPAD_B,
      X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y,
      X_INPUT_GAMEPAD_BACK,
      0,
      X_INPUT_GAMEPAD_START,
      X_INPUT_GAMEPAD_LEFT_THUMB,
      X_INPUT_GAMEPAD_RIGHT_THUMB,
      X_INPUT_GAMEPAD_LEFT_SHOULDER,
      X_INPUT_GAMEPAD_RIGHT_SHOULDER,
      X_INPUT_GAMEPAD_DPAD_UP,
      X_INPUT_GAMEPAD_DPAD_DOWN,
      X_INPUT_GAMEPAD_DPAD_LEFT,
      X_INPUT_GAMEPAD_DPAD_RIGHT,
  };
  return button < sizeof(buttons) / sizeof(buttons[0]) ? buttons[button] : 0;
}

constexpr uint16_t ClassifyControllerMatrixAnalog(const X_INPUT_GAMEPAD& gamepad) {
  uint16_t mask = 0;
  mask |= gamepad.thumb_lx < -kControllerMatrixLeftDeadzone ? 0x001 : 0;
  mask |= gamepad.thumb_lx > kControllerMatrixLeftDeadzone ? 0x002 : 0;
  mask |= gamepad.thumb_ly < -kControllerMatrixLeftDeadzone ? 0x004 : 0;
  mask |= gamepad.thumb_ly > kControllerMatrixLeftDeadzone ? 0x008 : 0;
  mask |= gamepad.thumb_rx < -kControllerMatrixRightDeadzone ? 0x010 : 0;
  mask |= gamepad.thumb_rx > kControllerMatrixRightDeadzone ? 0x020 : 0;
  mask |= gamepad.thumb_ry < -kControllerMatrixRightDeadzone ? 0x040 : 0;
  mask |= gamepad.thumb_ry > kControllerMatrixRightDeadzone ? 0x080 : 0;
  mask |= gamepad.left_trigger > kControllerMatrixTriggerThreshold ? 0x100 : 0;
  mask |= gamepad.right_trigger > kControllerMatrixTriggerThreshold ? 0x200 : 0;
  return mask;
}

}  // namespace rex::input::sdl
