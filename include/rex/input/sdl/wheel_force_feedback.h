#pragma once

#include <algorithm>
#include <cstdint>

#include <rex/input/input.h>

#include <SDL3/SDL.h>

namespace rex::input::sdl {

inline constexpr uint32_t kMaximumWheelForceFeedbackGainPercent = 100;
inline constexpr uint32_t kDefaultContinuousPeriodicGainPercent = 40;
inline constexpr uint32_t kDefaultContinuousConstantGainPercent = 0;
inline constexpr uint32_t kDefaultMinimumTransientStrengthPercent = 75;
inline constexpr uint32_t kMclaWheelHapticFeatures =
    SDL_HAPTIC_CONSTANT | SDL_HAPTIC_RAMP | SDL_HAPTIC_SQUARE | SDL_HAPTIC_SINE |
    SDL_HAPTIC_TRIANGLE | SDL_HAPTIC_SAWTOOTHUP | SDL_HAPTIC_SAWTOOTHDOWN | SDL_HAPTIC_SPRING |
    SDL_HAPTIC_DAMPER;

constexpr int16_t ScaleWheelForceCoefficient(int32_t value) {
  // SDL's condition-effect contract uses positive coefficients for the
  // restoring/resisting spring and damper examples shipped with SDL. Preserve
  // the guest sign while expanding the signed-byte domain. Inverting this
  // value turns a damper into an anti-damper on DirectInput wheels.
  const int32_t bounded_value = std::clamp(value, -127, 127);
  return static_cast<int16_t>((bounded_value * 32767) / 127);
}

constexpr uint16_t ScaleWheelForceSaturation(uint32_t value, uint32_t gain_percent) {
  const uint32_t bounded_value = std::min(value, 255u);
  const uint32_t bounded_gain = std::min(gain_percent, kMaximumWheelForceFeedbackGainPercent);
  const uint32_t requested = (bounded_value * 65535u) / 255u;
  return static_cast<uint16_t>((requested * bounded_gain) / 100u);
}

constexpr int16_t ScaleWheelSignedMagnitude(int32_t value, uint32_t gain_percent) {
  // XInputdFF carries constant, ramp and periodic-offset force levels in the
  // HID PID signed-byte domain. Expand it to SDL's signed 16-bit domain.
  const int32_t bounded_value = std::clamp(value, -127, 127);
  const int32_t bounded_gain =
      static_cast<int32_t>(std::min(gain_percent, kMaximumWheelForceFeedbackGainPercent));
  return static_cast<int16_t>((bounded_value * 32767 * bounded_gain) / (127 * 100));
}

constexpr uint16_t ScaleWheelUnsignedMagnitude(uint32_t value, uint32_t gain_percent) {
  // HID PID periodic magnitude is an unsigned byte; SDL periodic magnitude is
  // a signed 16-bit peak whose positive maximum is 32767.
  const uint32_t bounded_value = std::min(value, 255u);
  const uint32_t bounded_gain = std::min(gain_percent, kMaximumWheelForceFeedbackGainPercent);
  return static_cast<uint16_t>((bounded_value * 32767u * bounded_gain) / (255u * 100u));
}

constexpr uint16_t ScaleWheelEnvelopeLevel(uint32_t value, uint32_t gain_percent) {
  const uint32_t bounded_value = std::min(value, 255u);
  const uint32_t bounded_gain = std::min(gain_percent, kMaximumWheelForceFeedbackGainPercent);
  return static_cast<uint16_t>((bounded_value * 32767u * bounded_gain) / (255u * 100u));
}

constexpr uint16_t ApplyWheelUnsignedTransientFloor(uint16_t value, uint32_t duration_ms,
                                                    uint32_t gain_percent,
                                                    uint32_t minimum_percent) {
  if (duration_ms == UINT32_MAX || value == 0) {
    return value;
  }
  const uint32_t bounded_gain = std::min(gain_percent, kMaximumWheelForceFeedbackGainPercent);
  const uint32_t bounded_minimum = std::min(minimum_percent, kMaximumWheelForceFeedbackGainPercent);
  const uint32_t floor = (32767u * bounded_gain * bounded_minimum) / 10000u;
  return static_cast<uint16_t>(std::max<uint32_t>(value, floor));
}

constexpr int16_t ApplyWheelSignedTransientFloor(int16_t value, uint32_t duration_ms,
                                                 uint32_t gain_percent, uint32_t minimum_percent) {
  if (duration_ms == UINT32_MAX || value == 0) {
    return value;
  }
  const uint32_t magnitude =
      static_cast<uint32_t>(value < 0 ? -static_cast<int32_t>(value) : value);
  const uint32_t floored = ApplyWheelUnsignedTransientFloor(
      static_cast<uint16_t>(magnitude), duration_ms, gain_percent, minimum_percent);
  return static_cast<int16_t>(value < 0 ? -static_cast<int32_t>(floored) : floored);
}

constexpr uint16_t ScaleWheelPeriodicPhase(uint32_t value) {
  // HID PID phase spans one revolution in an unsigned byte; SDL uses
  // hundredths of a degree. Map the inclusive endpoint back to zero.
  return static_cast<uint16_t>(((std::min(value, 255u) * 36000u) / 255u) % 36000u);
}

constexpr uint16_t ClampWheelEffectTime(uint32_t value) {
  return static_cast<uint16_t>(std::min(value, uint32_t(UINT16_MAX)));
}

constexpr bool IsWheelPeriodicEffect(ForceFeedbackEffectType type) {
  return type == ForceFeedbackEffectType::kSquare || type == ForceFeedbackEffectType::kSine ||
         type == ForceFeedbackEffectType::kTriangle ||
         type == ForceFeedbackEffectType::kSawtoothUp ||
         type == ForceFeedbackEffectType::kSawtoothDown;
}

constexpr uint32_t GetWheelEffectGainPercent(const ForceFeedbackEffect& effect,
                                             uint32_t overall_gain_percent,
                                             uint32_t continuous_periodic_gain_percent,
                                             uint32_t continuous_constant_gain_percent) {
  const uint32_t overall = std::min(overall_gain_percent, kMaximumWheelForceFeedbackGainPercent);
  if (effect.duration_ms != UINT32_MAX) {
    return overall;
  }
  if (effect.type == ForceFeedbackEffectType::kConstant) {
    const uint32_t constant =
        std::min(continuous_constant_gain_percent, kMaximumWheelForceFeedbackGainPercent);
    return (overall * constant) / 100u;
  }
  if (!IsWheelPeriodicEffect(effect.type)) {
    return overall;
  }
  const uint32_t periodic =
      std::min(continuous_periodic_gain_percent, kMaximumWheelForceFeedbackGainPercent);
  return (overall * periodic) / 100u;
}

constexpr bool ShouldPreserveWheelForceFeedback(uint32_t duration_ms, bool resume_pending,
                                                bool running, bool status_supported,
                                                bool status_playing) {
  return duration_ms == UINT32_MAX &&
         (resume_pending || (running && (!status_supported || status_playing)));
}

constexpr uint32_t NormalizeWheelEffectIterations(uint32_t iterations) {
  return std::max(iterations, 1u);
}

constexpr bool ShouldSubmitWheelEffectStart(uint32_t duration_ms, bool state_running,
                                            bool status_supported, bool status_playing) {
  // Each finite MCLA start is an event edge. Some DirectInput wheel drivers
  // keep reporting an expired downloaded effect as playing, so status cannot
  // be used to suppress later curb/collision starts for that slot.
  return duration_ms != UINT32_MAX || !state_running || !status_supported || !status_playing;
}

constexpr int16_t ScaleWheelForceCenter(int32_t value) {
  const int32_t bounded_value = std::clamp(value, -127, 127);
  return static_cast<int16_t>((bounded_value * 32767) / 127);
}

constexpr uint16_t ScaleWheelForceDeadband(uint32_t value) {
  return static_cast<uint16_t>((std::min(value, 255u) * 65535u) / 255u);
}

constexpr const char* GetWheelEffectTypeName(ForceFeedbackEffectType type) {
  switch (type) {
    case ForceFeedbackEffectType::kConstant:
      return "constant";
    case ForceFeedbackEffectType::kRamp:
      return "ramp";
    case ForceFeedbackEffectType::kSquare:
      return "square";
    case ForceFeedbackEffectType::kSine:
      return "sine";
    case ForceFeedbackEffectType::kTriangle:
      return "triangle";
    case ForceFeedbackEffectType::kSawtoothUp:
      return "sawtooth-up";
    case ForceFeedbackEffectType::kSawtoothDown:
      return "sawtooth-down";
    case ForceFeedbackEffectType::kSpring:
      return "spring";
    case ForceFeedbackEffectType::kDamper:
      return "damper";
  }
  return "unknown";
}

constexpr SDL_HapticEffectType GetSdlWheelEffectType(ForceFeedbackEffectType type) {
  switch (type) {
    case ForceFeedbackEffectType::kConstant:
      return SDL_HAPTIC_CONSTANT;
    case ForceFeedbackEffectType::kRamp:
      return SDL_HAPTIC_RAMP;
    case ForceFeedbackEffectType::kSquare:
      return SDL_HAPTIC_SQUARE;
    case ForceFeedbackEffectType::kSine:
      return SDL_HAPTIC_SINE;
    case ForceFeedbackEffectType::kTriangle:
      return SDL_HAPTIC_TRIANGLE;
    case ForceFeedbackEffectType::kSawtoothUp:
      return SDL_HAPTIC_SAWTOOTHUP;
    case ForceFeedbackEffectType::kSawtoothDown:
      return SDL_HAPTIC_SAWTOOTHDOWN;
    case ForceFeedbackEffectType::kSpring:
      return SDL_HAPTIC_SPRING;
    case ForceFeedbackEffectType::kDamper:
      return SDL_HAPTIC_DAMPER;
  }
  return SDL_HAPTIC_RESERVED1;
}

constexpr uint32_t GetWheelEffectLength(const ForceFeedbackEffect& effect) {
  if (effect.duration_ms == UINT32_MAX) {
    // SDL explicitly excludes ramp effects from the infinite-duration value.
    return effect.type == ForceFeedbackEffectType::kRamp ? UINT16_MAX : SDL_HAPTIC_INFINITY;
  }
  return std::max(effect.duration_ms, 1u);
}

inline void ConfigureWheelEffectDirection(SDL_HapticDirection& direction) {
  // MCLA creates one-axis steering effects. Cartesian X was physically
  // verified on the T300 and is also accepted by generic SDL wheel drivers.
  direction.type = SDL_HAPTIC_CARTESIAN;
  direction.dir[0] = 1;
}

inline void ConfigureWheelEffectEnvelope(uint16_t& attack_length, uint16_t& attack_level,
                                         uint16_t& fade_length, uint16_t& fade_level,
                                         const ForceFeedbackEffect& effect, uint32_t gain) {
  attack_length = ClampWheelEffectTime(effect.attack_length_ms);
  attack_level = ScaleWheelEnvelopeLevel(effect.attack_level, gain);
  fade_length = ClampWheelEffectTime(effect.fade_length_ms);
  fade_level = ScaleWheelEnvelopeLevel(effect.fade_level, gain);
}

inline bool BuildSdlWheelEffect(const ForceFeedbackEffect& effect, uint32_t gain, int haptic_axes,
                                SDL_HapticEffect& out_effect,
                                uint32_t minimum_transient_percent = 0) {
  out_effect = {};
  const SDL_HapticEffectType sdl_type = GetSdlWheelEffectType(effect.type);
  if (sdl_type == SDL_HAPTIC_RESERVED1) {
    return false;
  }

  const uint32_t length = GetWheelEffectLength(effect);
  const uint16_t delay = ClampWheelEffectTime(effect.start_delay_ms);
  switch (effect.type) {
    case ForceFeedbackEffectType::kConstant:
      out_effect.type = sdl_type;
      ConfigureWheelEffectDirection(out_effect.constant.direction);
      out_effect.constant.length = length;
      out_effect.constant.delay = delay;
      out_effect.constant.level =
          ApplyWheelSignedTransientFloor(ScaleWheelSignedMagnitude(effect.constant_level, gain),
                                         effect.duration_ms, gain, minimum_transient_percent);
      ConfigureWheelEffectEnvelope(
          out_effect.constant.attack_length, out_effect.constant.attack_level,
          out_effect.constant.fade_length, out_effect.constant.fade_level, effect, gain);
      return true;
    case ForceFeedbackEffectType::kRamp:
      out_effect.type = sdl_type;
      ConfigureWheelEffectDirection(out_effect.ramp.direction);
      out_effect.ramp.length = length;
      out_effect.ramp.delay = delay;
      out_effect.ramp.start = ScaleWheelSignedMagnitude(effect.ramp_start, gain);
      out_effect.ramp.end = ScaleWheelSignedMagnitude(effect.ramp_end, gain);
      ConfigureWheelEffectEnvelope(out_effect.ramp.attack_length, out_effect.ramp.attack_level,
                                   out_effect.ramp.fade_length, out_effect.ramp.fade_level, effect,
                                   gain);
      return true;
    case ForceFeedbackEffectType::kSquare:
    case ForceFeedbackEffectType::kSine:
    case ForceFeedbackEffectType::kTriangle:
    case ForceFeedbackEffectType::kSawtoothUp:
    case ForceFeedbackEffectType::kSawtoothDown:
      out_effect.type = sdl_type;
      ConfigureWheelEffectDirection(out_effect.periodic.direction);
      out_effect.periodic.length = length;
      out_effect.periodic.delay = delay;
      out_effect.periodic.period =
          std::max<uint16_t>(ClampWheelEffectTime(effect.periodic_period_ms), 1);
      out_effect.periodic.magnitude = static_cast<int16_t>(ApplyWheelUnsignedTransientFloor(
          ScaleWheelUnsignedMagnitude(effect.periodic_magnitude, gain), effect.duration_ms, gain,
          minimum_transient_percent));
      // Offset shifts the waveform's center and is directional, not an event
      // strength. Flooring it would turn a tiny bias into a strong one-sided
      // pull. Only the periodic magnitude receives the transient floor.
      out_effect.periodic.offset = ScaleWheelSignedMagnitude(effect.periodic_offset, gain);
      out_effect.periodic.phase = ScaleWheelPeriodicPhase(effect.periodic_phase);
      ConfigureWheelEffectEnvelope(
          out_effect.periodic.attack_length, out_effect.periodic.attack_level,
          out_effect.periodic.fade_length, out_effect.periodic.fade_level, effect, gain);
      return true;
    case ForceFeedbackEffectType::kSpring:
    case ForceFeedbackEffectType::kDamper:
      out_effect.type = sdl_type;
      ConfigureWheelEffectDirection(out_effect.condition.direction);
      out_effect.condition.length = length;
      out_effect.condition.delay = delay;
      for (int axis = 0; axis < std::clamp(haptic_axes, 0, 3); ++axis) {
        const auto& condition = effect.conditions[static_cast<size_t>(axis)];
        out_effect.condition.center[axis] = ScaleWheelForceCenter(condition.center);
        out_effect.condition.right_coeff[axis] =
            ScaleWheelForceCoefficient(condition.right_coefficient);
        out_effect.condition.left_coeff[axis] =
            ScaleWheelForceCoefficient(condition.left_coefficient);
        out_effect.condition.right_sat[axis] =
            ScaleWheelForceSaturation(condition.right_saturation, gain);
        out_effect.condition.left_sat[axis] =
            ScaleWheelForceSaturation(condition.left_saturation, gain);
        out_effect.condition.deadband[axis] = ScaleWheelForceDeadband(condition.deadband);
      }
      return true;
  }
  return false;
}

}  // namespace rex::input::sdl
