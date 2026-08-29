/**
 ******************************************************************************
 * Copyright 2026 ReXGlue contributors. All rights reserved.                  *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

#include <rex/input/input.h>

#include <SDL3/SDL_joystick.h>

namespace rex::input::sdl {

constexpr uint16_t kThrustmasterVendorId = 0x044F;

inline constexpr bool IsThrustmasterT300(uint16_t vendor_id, uint16_t product_id) {
  return vendor_id == kThrustmasterVendorId &&
         (product_id == 0xB66D || product_id == 0xB66E || product_id == 0xB66F);
}

inline constexpr bool IsSupportedWheel(uint16_t vendor_id, uint16_t product_id,
                                       SDL_JoystickType joystick_type) {
  // Device type is the model-agnostic contract. Keep narrow known-device
  // fallbacks for Windows drivers that expose a real wheel as UNKNOWN.
  return joystick_type == SDL_JOYSTICK_TYPE_WHEEL || IsThrustmasterT300(vendor_id, product_id);
}

struct WheelMapping {
  int steering_axis = 0;
  int brake_axis = 1;
  int accelerator_axis = 2;
  bool steering_inverted = false;
  bool brake_inverted = true;
  bool accelerator_inverted = true;
  int left_shoulder_button = 0;
  int right_shoulder_button = 1;
  int y_button = 2;
  int x_button = 3;
  int b_button = 4;
  int a_button = 5;
  int back_button = 6;
  int start_button = 7;
  int left_thumb_button = 10;
  int right_thumb_button = 11;
  int guide_button = 12;
  int dpad_up_button = -1;
  int dpad_down_button = -1;
  int dpad_left_button = -1;
  int dpad_right_button = -1;
};

inline constexpr int16_t WheelSteeringValue(int16_t value, bool inverted) {
  if (!inverted) {
    return value;
  }
  return value == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-value);
}

inline constexpr uint8_t WheelPedalToTrigger(int16_t value, bool inverted = true) {
  const int32_t distance_from_released =
      inverted ? 32767 - static_cast<int32_t>(value) : static_cast<int32_t>(value) - INT16_MIN;
  return static_cast<uint8_t>((distance_from_released * 255 + 32767) / 65535);
}

inline constexpr uint16_t WheelHatToXInput(uint8_t value) {
  uint16_t buttons = 0;
  if (value & SDL_HAT_UP) {
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  }
  if (value & SDL_HAT_DOWN) {
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  if (value & SDL_HAT_LEFT) {
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  }
  if (value & SDL_HAT_RIGHT) {
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  }
  return buttons;
}

inline constexpr uint16_t WheelButtonToXInput(uint8_t button, const WheelMapping& mapping = {}) {
  uint16_t result = 0;
  const int source = button;
  if (source == mapping.left_shoulder_button)
    result |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (source == mapping.right_shoulder_button)
    result |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (source == mapping.y_button)
    result |= X_INPUT_GAMEPAD_Y;
  if (source == mapping.x_button)
    result |= X_INPUT_GAMEPAD_X;
  if (source == mapping.b_button)
    result |= X_INPUT_GAMEPAD_B;
  if (source == mapping.a_button)
    result |= X_INPUT_GAMEPAD_A;
  if (source == mapping.back_button)
    result |= X_INPUT_GAMEPAD_BACK;
  if (source == mapping.start_button)
    result |= X_INPUT_GAMEPAD_START;
  if (source == mapping.left_thumb_button)
    result |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (source == mapping.right_thumb_button)
    result |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (source == mapping.guide_button)
    result |= X_INPUT_GAMEPAD_GUIDE;
  if (source == mapping.dpad_up_button)
    result |= X_INPUT_GAMEPAD_DPAD_UP;
  if (source == mapping.dpad_down_button)
    result |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (source == mapping.dpad_left_button)
    result |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (source == mapping.dpad_right_button)
    result |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  return result;
}

}  // namespace rex::input::sdl
