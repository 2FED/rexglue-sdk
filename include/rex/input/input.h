#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>

#include <rex/assert.h>
#include <rex/types.h>

namespace rex::input {

enum X_INPUT_CAPS {
  X_INPUT_CAPS_FFB_SUPPORTED = 0x0001,
  X_INPUT_CAPS_WIRELESS = 0x0002,
  X_INPUT_CAPS_VOICE_SUPPORTED = 0x0004,
  X_INPUT_CAPS_PMD_SUPPORTED = 0x0008,
  X_INPUT_CAPS_NO_NAVIGATION = 0x0010,
};

enum X_INPUT_DEVTYPE {
  XINPUT_DEVTYPE_GAMEPAD = 0x00000001,
  XINPUT_DEVTYPE_KEYBOARD = 0x00000002,
};

enum X_INPUT_FLAG {
  X_INPUT_FLAG_GAMEPAD = 0x00000001,
};

enum X_INPUT_GAMEPAD_BUTTON {
  X_INPUT_GAMEPAD_DPAD_UP = 0x0001,
  X_INPUT_GAMEPAD_DPAD_DOWN = 0x0002,
  X_INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
  X_INPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
  X_INPUT_GAMEPAD_START = 0x0010,
  X_INPUT_GAMEPAD_BACK = 0x0020,
  X_INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
  X_INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
  X_INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
  X_INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
  X_INPUT_GAMEPAD_GUIDE = 0x0400,
  X_INPUT_GAMEPAD_A = 0x1000,
  X_INPUT_GAMEPAD_B = 0x2000,
  X_INPUT_GAMEPAD_X = 0x4000,
  X_INPUT_GAMEPAD_Y = 0x8000,
};

// For VK_PAD, use ui::VirtualKey.

enum X_INPUT_KEYSTROKE_FLAGS {
  X_INPUT_KEYSTROKE_KEYDOWN = 0x0001,
  X_INPUT_KEYSTROKE_KEYUP = 0x0002,
  X_INPUT_KEYSTROKE_REPEAT = 0x0004,
};

struct X_INPUT_GAMEPAD {
  be<uint16_t> buttons;
  uint8_t left_trigger;
  uint8_t right_trigger;
  be<int16_t> thumb_lx;
  be<int16_t> thumb_ly;
  be<int16_t> thumb_rx;
  be<int16_t> thumb_ry;
};
static_assert_size(X_INPUT_GAMEPAD, 12);

struct X_INPUT_STATE {
  be<uint32_t> packet_number;
  X_INPUT_GAMEPAD gamepad;
};
static_assert_size(X_INPUT_STATE, sizeof(X_INPUT_GAMEPAD) + 4);

struct X_INPUT_VIBRATION {
  be<uint16_t> left_motor_speed;
  be<uint16_t> right_motor_speed;
};
static_assert_size(X_INPUT_VIBRATION, 4);

struct X_INPUT_CAPABILITIES {
  uint8_t type;
  uint8_t sub_type;
  be<uint16_t> flags;
  X_INPUT_GAMEPAD gamepad;
  X_INPUT_VIBRATION vibration;
};
static_assert_size(X_INPUT_CAPABILITIES, sizeof(X_INPUT_GAMEPAD) + sizeof(X_INPUT_VIBRATION) + 4);

enum class ForceFeedbackEffectType : uint32_t {
  kConstant = 1,
  kRamp = 2,
  kSquare = 3,
  kSine = 4,
  kTriangle = 5,
  kSawtoothUp = 6,
  kSawtoothDown = 7,
  kSpring = 8,
  kDamper = 9,
};

struct ForceFeedbackCondition {
  int32_t center = 0;
  int32_t right_coefficient = 0;
  int32_t left_coefficient = 0;
  uint32_t right_saturation = 0;
  uint32_t left_saturation = 0;
  uint32_t deadband = 0;
};

struct ForceFeedbackEffect {
  uint32_t effect_index = 0;
  ForceFeedbackEffectType type = ForceFeedbackEffectType::kConstant;
  uint32_t duration_ms = 0;
  uint32_t start_delay_ms = 0;
  uint32_t attack_length_ms = 0;
  uint32_t fade_length_ms = 0;
  uint32_t attack_level = 0;
  uint32_t fade_level = 0;
  int32_t constant_level = 0;
  int32_t ramp_start = 0;
  int32_t ramp_end = 0;
  uint32_t periodic_magnitude = 0;
  int32_t periodic_offset = 0;
  uint32_t periodic_phase = 0;
  uint32_t periodic_period_ms = 0;
  std::array<ForceFeedbackCondition, 3> conditions = {};
};

struct ForceFeedbackOperationEntry {
  uint32_t effect_index = 0;
  uint32_t iterations = 1;
};

inline ForceFeedbackOperationEntry DecodeForceFeedbackOperationWords(uint32_t effect_index,
                                                                     uint32_t iterations) {
  return {.effect_index = effect_index, .iterations = std::max(iterations, 1u)};
}

// XInputdFFEffectOperation passes an array of {effect_index, iterations}
// entries (two big-endian words each); r5 carries the entry count. Bound the
// count to the 64 guest effect slots; a zero count keeps the historical
// single-entry behaviour.
inline constexpr uint32_t kMaxForceFeedbackOperationEntries = 64;
inline constexpr size_t kForceFeedbackOperationEntryWords = 2;

constexpr uint32_t ClampForceFeedbackOperationCount(uint32_t count) {
  return std::clamp<uint32_t>(count, 1u, kMaxForceFeedbackOperationEntries);
}

// Applies every entry of a start (`start`) or stop operation. `load_word(i)`
// returns guest word i of the array; `apply(entry)` performs one start/stop
// and returns an X_RESULT-style code (0 = success). The first failure is
// returned, but every entry is still applied.
template <typename LoadWord, typename Apply>
uint32_t ApplyForceFeedbackOperationBatch(uint32_t raw_count, bool start, LoadWord&& load_word,
                                          Apply&& apply) {
  const uint32_t count = ClampForceFeedbackOperationCount(raw_count);
  uint32_t result = 0;
  for (uint32_t entry = 0; entry < count; ++entry) {
    const size_t word = size_t(entry) * kForceFeedbackOperationEntryWords;
    const auto operation =
        DecodeForceFeedbackOperationWords(load_word(word), start ? load_word(word + 1) : 1);
    const uint32_t entry_result = static_cast<uint32_t>(apply(operation));
    if (result == 0) {
      result = entry_result;
    }
  }
  return result;
}

inline constexpr size_t kForceFeedbackEffectWordCount = 29;

inline ForceFeedbackEffect DecodeForceFeedbackEffectWords(
    const std::array<uint32_t, kForceFeedbackEffectWordCount>& words) {
  ForceFeedbackEffect effect;
  effect.effect_index = words[0];
  effect.type = static_cast<ForceFeedbackEffectType>(words[1]);
  effect.duration_ms = words[4];
  effect.start_delay_ms = words[5];
  effect.attack_length_ms = words[7];
  effect.fade_length_ms = words[8];
  effect.attack_level = words[9];
  effect.fade_level = words[10];
  switch (effect.type) {
    case ForceFeedbackEffectType::kConstant:
      effect.constant_level = static_cast<int32_t>(words[11]);
      break;
    case ForceFeedbackEffectType::kRamp:
      effect.ramp_start = static_cast<int32_t>(words[11]);
      effect.ramp_end = static_cast<int32_t>(words[12]);
      break;
    case ForceFeedbackEffectType::kSquare:
    case ForceFeedbackEffectType::kSine:
    case ForceFeedbackEffectType::kTriangle:
    case ForceFeedbackEffectType::kSawtoothUp:
    case ForceFeedbackEffectType::kSawtoothDown:
      effect.periodic_magnitude = words[11];
      effect.periodic_offset = static_cast<int32_t>(words[12]);
      effect.periodic_phase = words[13];
      effect.periodic_period_ms = words[14];
      break;
    case ForceFeedbackEffectType::kSpring:
    case ForceFeedbackEffectType::kDamper:
      for (size_t axis = 0; axis < effect.conditions.size(); ++axis) {
        const size_t word = 11 + axis * 6;
        auto& condition = effect.conditions[axis];
        condition.center = static_cast<int32_t>(words[word]);
        condition.right_coefficient = static_cast<int32_t>(words[word + 1]);
        condition.left_coefficient = static_cast<int32_t>(words[word + 2]);
        condition.right_saturation = words[word + 3];
        condition.left_saturation = words[word + 4];
        condition.deadband = words[word + 5];
      }
      break;
  }
  return effect;
}

// https://msdn.microsoft.com/en-us/library/windows/desktop/microsoft.directx_sdk.reference.xinput_keystroke(v=vs.85).aspx
struct X_INPUT_KEYSTROKE {
  be<uint16_t> virtual_key;
  be<uint16_t> unicode;
  be<uint16_t> flags;
  uint8_t user_index;
  uint8_t hid_code;
};
static_assert_size(X_INPUT_KEYSTROKE, 8);

}  // namespace rex::input
