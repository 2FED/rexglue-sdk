/**
 * Regression coverage for raw SDL racing-wheel translation.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/input/sdl/wheel_mapping.h>
#include <rex/input/sdl/wheel_force_feedback.h>

using rex::input::sdl::ApplyWheelSignedTransientFloor;
using rex::input::sdl::ApplyWheelUnsignedTransientFloor;
using rex::input::sdl::BuildSdlWheelEffect;
using rex::input::sdl::ClampWheelEffectTime;
using rex::input::sdl::GetSdlWheelEffectType;
using rex::input::sdl::GetWheelEffectGainPercent;
using rex::input::sdl::GetWheelEffectLength;
using rex::input::sdl::GetWheelEffectTypeName;
using rex::input::sdl::IsSupportedWheel;
using rex::input::sdl::IsThrustmasterT300;
using rex::input::sdl::NormalizeWheelEffectIterations;
using rex::input::sdl::ScaleWheelEnvelopeLevel;
using rex::input::sdl::ScaleWheelForceCenter;
using rex::input::sdl::ScaleWheelForceCoefficient;
using rex::input::sdl::ScaleWheelForceDeadband;
using rex::input::sdl::ScaleWheelForceSaturation;
using rex::input::sdl::ScaleWheelPeriodicPhase;
using rex::input::sdl::ScaleWheelSignedMagnitude;
using rex::input::sdl::ScaleWheelUnsignedMagnitude;
using rex::input::sdl::ShouldPreserveWheelForceFeedback;
using rex::input::sdl::ShouldSubmitWheelEffectStart;
using rex::input::sdl::WheelButtonToXInput;
using rex::input::sdl::WheelHatToXInput;
using rex::input::sdl::WheelMapping;
using rex::input::sdl::WheelPedalToTrigger;
using rex::input::sdl::WheelSteeringValue;

TEST_CASE("T300 normal, PS4 and advanced modes are classified", "[input][sdl][wheel]") {
  CHECK(IsThrustmasterT300(0x044F, 0xB66D));
  CHECK(IsThrustmasterT300(0x044F, 0xB66E));
  CHECK(IsThrustmasterT300(0x044F, 0xB66F));
  CHECK_FALSE(IsThrustmasterT300(0x044F, 0xB65E));
  CHECK_FALSE(IsThrustmasterT300(0x046D, 0xB66E));
}

TEST_CASE("SDL wheel classification is model agnostic with narrow fallback",
          "[input][sdl][wheel]") {
  CHECK(IsSupportedWheel(0x346E, 0x0004, SDL_JOYSTICK_TYPE_WHEEL));
  CHECK(IsSupportedWheel(0x046D, 0xC24F, SDL_JOYSTICK_TYPE_WHEEL));
  CHECK(IsSupportedWheel(0x044F, 0xB66E, SDL_JOYSTICK_TYPE_UNKNOWN));
  CHECK_FALSE(IsSupportedWheel(0x1234, 0x5678, SDL_JOYSTICK_TYPE_UNKNOWN));
  CHECK_FALSE(IsSupportedWheel(0x045E, 0x028E, SDL_JOYSTICK_TYPE_GAMEPAD));
}

TEST_CASE("Released and fully pressed wheel pedals span the XInput trigger range",
          "[input][sdl][wheel]") {
  CHECK(WheelPedalToTrigger(32767) == 0);
  CHECK(WheelPedalToTrigger(0) == 127);
  CHECK(WheelPedalToTrigger(-32768) == 255);
  CHECK(WheelPedalToTrigger(-32768, false) == 0);
  CHECK(WheelPedalToTrigger(32767, false) == 255);
  CHECK(WheelSteeringValue(-32768, false) == -32768);
  CHECK(WheelSteeringValue(-32768, true) == 32767);
  CHECK(WheelSteeringValue(1234, true) == -1234);
}

TEST_CASE("Wheel POV hats preserve cardinal and diagonal D-pad input", "[input][sdl][wheel]") {
  CHECK(WheelHatToXInput(SDL_HAT_CENTERED) == 0);
  CHECK(WheelHatToXInput(SDL_HAT_UP) == rex::input::X_INPUT_GAMEPAD_DPAD_UP);
  CHECK(WheelHatToXInput(SDL_HAT_RIGHTDOWN) ==
        (rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT | rex::input::X_INPUT_GAMEPAD_DPAD_DOWN));
  CHECK(WheelHatToXInput(SDL_HAT_LEFTUP) ==
        (rex::input::X_INPUT_GAMEPAD_DPAD_LEFT | rex::input::X_INPUT_GAMEPAD_DPAD_UP));
}

TEST_CASE("T300 rim controls map to the Xbox wheel navigation surface", "[input][sdl][wheel]") {
  CHECK(WheelButtonToXInput(0) == rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER);
  CHECK(WheelButtonToXInput(1) == rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER);
  CHECK(WheelButtonToXInput(2) == rex::input::X_INPUT_GAMEPAD_Y);
  CHECK(WheelButtonToXInput(3) == rex::input::X_INPUT_GAMEPAD_X);
  CHECK(WheelButtonToXInput(4) == rex::input::X_INPUT_GAMEPAD_B);
  CHECK(WheelButtonToXInput(5) == rex::input::X_INPUT_GAMEPAD_A);
  CHECK(WheelButtonToXInput(6) == rex::input::X_INPUT_GAMEPAD_BACK);
  CHECK(WheelButtonToXInput(7) == rex::input::X_INPUT_GAMEPAD_START);
  CHECK(WheelButtonToXInput(8) == 0);
  CHECK(WheelButtonToXInput(9) == 0);
  CHECK(WheelButtonToXInput(10) == rex::input::X_INPUT_GAMEPAD_LEFT_THUMB);
  CHECK(WheelButtonToXInput(11) == rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB);
  CHECK(WheelButtonToXInput(12) == rex::input::X_INPUT_GAMEPAD_GUIDE);

  WheelMapping custom;
  custom.a_button = 20;
  custom.start_button = 30;
  custom.dpad_up_button = 40;
  custom.left_thumb_button = 50;
  custom.right_thumb_button = 60;
  custom.guide_button = 70;
  CHECK(WheelButtonToXInput(20, custom) == rex::input::X_INPUT_GAMEPAD_A);
  CHECK(WheelButtonToXInput(30, custom) == rex::input::X_INPUT_GAMEPAD_START);
  CHECK(WheelButtonToXInput(40, custom) == rex::input::X_INPUT_GAMEPAD_DPAD_UP);
  CHECK(WheelButtonToXInput(50, custom) == rex::input::X_INPUT_GAMEPAD_LEFT_THUMB);
  CHECK(WheelButtonToXInput(60, custom) == rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB);
  CHECK(WheelButtonToXInput(70, custom) == rex::input::X_INPUT_GAMEPAD_GUIDE);
}

TEST_CASE("Wheel force feedback scales to the configured bounded gain",
          "[input][sdl][wheel][ffb]") {
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kConstant) == 1);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kRamp) == 2);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSquare) == 3);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSine) == 4);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kTriangle) == 5);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSawtoothUp) == 6);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSawtoothDown) == 7);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSpring) == 8);
  CHECK(static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kDamper) == 9);

  CHECK(ScaleWheelForceCoefficient(127) == 32767);
  CHECK(ScaleWheelForceCoefficient(-127) == -32767);
  CHECK(ScaleWheelForceCoefficient(114) == 29412);
  CHECK(ScaleWheelForceCoefficient(50000) == 32767);

  CHECK(ScaleWheelForceSaturation(255, 25) == 16383);
  CHECK(ScaleWheelForceSaturation(500, 25) == 16383);
  CHECK(ScaleWheelForceSaturation(255, 100) == 65535);
  CHECK(ScaleWheelForceSaturation(255, 0) == 0);
  CHECK(ScaleWheelForceSaturation(25, 15) == 963);

  CHECK(ScaleWheelSignedMagnitude(127, 100) == 32767);
  CHECK(ScaleWheelSignedMagnitude(-127, 100) == -32767);
  CHECK(ScaleWheelSignedMagnitude(77, 100) == 19866);
  CHECK(ScaleWheelSignedMagnitude(500, 50) == 16383);
  CHECK(ScaleWheelUnsignedMagnitude(255, 100) == 32767);
  CHECK(ScaleWheelUnsignedMagnitude(76, 100) == 9765);
  CHECK(ScaleWheelUnsignedMagnitude(500, 50) == 16383);
  CHECK(ScaleWheelEnvelopeLevel(255, 100) == 32767);
  CHECK(ScaleWheelEnvelopeLevel(128, 25) == 4111);
  CHECK(ApplyWheelUnsignedTransientFloor(8994, 150, 100, 75) == 24575);
  CHECK(ApplyWheelSignedTransientFloor(-3096, 150, 100, 75) == -24575);
  CHECK(ApplyWheelSignedTransientFloor(0, 150, 100, 75) == 0);
  CHECK(ApplyWheelUnsignedTransientFloor(8994, UINT32_MAX, 100, 75) == 8994);
  CHECK(ScaleWheelPeriodicPhase(0) == 0);
  CHECK(ScaleWheelPeriodicPhase(64) == 9035);
  CHECK(ScaleWheelPeriodicPhase(128) == 18070);
  CHECK(ScaleWheelPeriodicPhase(255) == 0);
  CHECK(ScaleWheelPeriodicPhase(99999) == 0);
  CHECK(ClampWheelEffectTime(1234) == 1234);
  CHECK(ClampWheelEffectTime(99999) == UINT16_MAX);

  rex::input::ForceFeedbackEffect continuous_periodic;
  continuous_periodic.type = rex::input::ForceFeedbackEffectType::kSquare;
  continuous_periodic.duration_ms = UINT32_MAX;
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 40, 0) == 40);
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 75, 40, 0) == 30);
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 500, 0) == 100);
  continuous_periodic.duration_ms = 150;
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 40, 0) == 100);
  continuous_periodic.duration_ms = UINT32_MAX;
  continuous_periodic.type = rex::input::ForceFeedbackEffectType::kConstant;
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 40, 0) == 0);
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 40, 25) == 25);
  continuous_periodic.duration_ms = 150;
  CHECK(GetWheelEffectGainPercent(continuous_periodic, 100, 40, 0) == 100);

  CHECK(ScaleWheelForceCenter(-127) == -32767);
  CHECK(ScaleWheelForceCenter(127) == 32767);
  CHECK(ScaleWheelForceDeadband(255) == 65535);

  CHECK_FALSE(ShouldPreserveWheelForceFeedback(UINT32_MAX, false, false, true, false));
  CHECK_FALSE(ShouldPreserveWheelForceFeedback(UINT32_MAX, false, true, true, false));
  CHECK(ShouldPreserveWheelForceFeedback(UINT32_MAX, false, true, true, true));
  CHECK(ShouldPreserveWheelForceFeedback(UINT32_MAX, false, true, false, false));
  CHECK(ShouldPreserveWheelForceFeedback(UINT32_MAX, true, false, true, false));
  CHECK_FALSE(ShouldPreserveWheelForceFeedback(150, true, true, true, true));
  CHECK(NormalizeWheelEffectIterations(0) == 1);
  CHECK(NormalizeWheelEffectIterations(2) == 2);
  CHECK(ShouldSubmitWheelEffectStart(UINT32_MAX, false, true, false));
  CHECK(ShouldSubmitWheelEffectStart(UINT32_MAX, true, false, false));
  CHECK(ShouldSubmitWheelEffectStart(UINT32_MAX, true, true, false));
  CHECK_FALSE(ShouldSubmitWheelEffectStart(UINT32_MAX, true, true, true));
  CHECK(ShouldSubmitWheelEffectStart(150, true, true, true));
}

TEST_CASE("Xbox wheel effect words decode every MCLA payload family", "[input][sdl][wheel][ffb]") {
  const auto operation = rex::input::DecodeForceFeedbackOperationWords(17, 2);
  CHECK(operation.effect_index == 17);
  CHECK(operation.iterations == 2);
  CHECK(rex::input::DecodeForceFeedbackOperationWords(18, 0).iterations == 1);

  std::array<uint32_t, rex::input::kForceFeedbackEffectWordCount> words = {};
  words[0] = 17;
  words[1] = static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kConstant);
  words[4] = 1234;
  words[5] = 25;
  words[7] = 11;
  words[8] = 22;
  words[9] = 333;
  words[10] = 444;
  words[11] = static_cast<uint32_t>(-123);
  auto constant = rex::input::DecodeForceFeedbackEffectWords(words);
  CHECK(constant.effect_index == 17);
  CHECK(constant.type == rex::input::ForceFeedbackEffectType::kConstant);
  CHECK(constant.duration_ms == 1234);
  CHECK(constant.start_delay_ms == 25);
  CHECK(constant.attack_length_ms == 11);
  CHECK(constant.fade_length_ms == 22);
  CHECK(constant.attack_level == 333);
  CHECK(constant.fade_level == 444);
  CHECK(constant.constant_level == -123);

  words[1] = static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kRamp);
  words[11] = static_cast<uint32_t>(-1000);
  words[12] = 2000;
  auto ramp = rex::input::DecodeForceFeedbackEffectWords(words);
  CHECK(ramp.ramp_start == -1000);
  CHECK(ramp.ramp_end == 2000);

  words[1] = static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kSine);
  words[11] = 3000;
  words[12] = static_cast<uint32_t>(-4000);
  words[13] = 4500;
  words[14] = 16;
  auto periodic = rex::input::DecodeForceFeedbackEffectWords(words);
  CHECK(periodic.periodic_magnitude == 3000);
  CHECK(periodic.periodic_offset == -4000);
  CHECK(periodic.periodic_phase == 4500);
  CHECK(periodic.periodic_period_ms == 16);

  words[1] = static_cast<uint32_t>(rex::input::ForceFeedbackEffectType::kDamper);
  for (size_t axis = 0; axis < 3; ++axis) {
    const size_t word = 11 + axis * 6;
    words[word] = static_cast<uint32_t>(-10 - static_cast<int32_t>(axis));
    words[word + 1] = 20 + static_cast<uint32_t>(axis);
    words[word + 2] = static_cast<uint32_t>(-30 - static_cast<int32_t>(axis));
    words[word + 3] = 40 + static_cast<uint32_t>(axis);
    words[word + 4] = 50 + static_cast<uint32_t>(axis);
    words[word + 5] = 60 + static_cast<uint32_t>(axis);
  }
  auto damper = rex::input::DecodeForceFeedbackEffectWords(words);
  CHECK(damper.type == rex::input::ForceFeedbackEffectType::kDamper);
  CHECK(damper.conditions[0].center == -10);
  CHECK(damper.conditions[0].right_coefficient == 20);
  CHECK(damper.conditions[0].left_coefficient == -30);
  CHECK(damper.conditions[0].right_saturation == 40);
  CHECK(damper.conditions[0].left_saturation == 50);
  CHECK(damper.conditions[0].deadband == 60);
  CHECK(damper.conditions[1].center == -11);
  CHECK(damper.conditions[1].right_coefficient == 21);
  CHECK(damper.conditions[1].left_coefficient == -31);
  CHECK(damper.conditions[1].right_saturation == 41);
  CHECK(damper.conditions[1].left_saturation == 51);
  CHECK(damper.conditions[1].deadband == 61);
  CHECK(damper.conditions[2].center == -12);
  CHECK(damper.conditions[2].right_coefficient == 22);
  CHECK(damper.conditions[2].left_coefficient == -32);
  CHECK(damper.conditions[2].right_saturation == 42);
  CHECK(damper.conditions[2].left_saturation == 52);
  CHECK(damper.conditions[2].deadband == 62);
}

TEST_CASE("Every MCLA wheel force family translates to the matching SDL effect",
          "[input][sdl][wheel][ffb]") {
  const std::array<std::pair<rex::input::ForceFeedbackEffectType, SDL_HapticEffectType>, 9>
      mappings = {{{rex::input::ForceFeedbackEffectType::kConstant, SDL_HAPTIC_CONSTANT},
                   {rex::input::ForceFeedbackEffectType::kRamp, SDL_HAPTIC_RAMP},
                   {rex::input::ForceFeedbackEffectType::kSquare, SDL_HAPTIC_SQUARE},
                   {rex::input::ForceFeedbackEffectType::kSine, SDL_HAPTIC_SINE},
                   {rex::input::ForceFeedbackEffectType::kTriangle, SDL_HAPTIC_TRIANGLE},
                   {rex::input::ForceFeedbackEffectType::kSawtoothUp, SDL_HAPTIC_SAWTOOTHUP},
                   {rex::input::ForceFeedbackEffectType::kSawtoothDown, SDL_HAPTIC_SAWTOOTHDOWN},
                   {rex::input::ForceFeedbackEffectType::kSpring, SDL_HAPTIC_SPRING},
                   {rex::input::ForceFeedbackEffectType::kDamper, SDL_HAPTIC_DAMPER}}};
  for (const auto& [guest_type, sdl_type] : mappings) {
    CAPTURE(GetWheelEffectTypeName(guest_type));
    CHECK(GetSdlWheelEffectType(guest_type) == sdl_type);
    rex::input::ForceFeedbackEffect source;
    source.type = guest_type;
    source.duration_ms = 1000;
    SDL_HapticEffect translated = {};
    CHECK(BuildSdlWheelEffect(source, 100, 1, translated));
    CHECK(translated.type == sdl_type);
  }

  rex::input::ForceFeedbackEffect source;
  source.duration_ms = UINT32_MAX;
  source.start_delay_ms = 70000;
  source.attack_length_ms = 123;
  source.fade_length_ms = 234;
  source.attack_level = 64;
  source.fade_level = 128;
  source.constant_level = -64;
  SDL_HapticEffect translated = {};
  CHECK(GetWheelEffectLength(source) == SDL_HAPTIC_INFINITY);
  REQUIRE(BuildSdlWheelEffect(source, 50, 1, translated));
  CHECK(translated.constant.length == SDL_HAPTIC_INFINITY);
  CHECK(translated.constant.delay == UINT16_MAX);
  CHECK(translated.constant.level == -8256);
  CHECK(translated.constant.attack_length == 123);
  CHECK(translated.constant.attack_level == 4111);
  CHECK(translated.constant.fade_length == 234);
  CHECK(translated.constant.fade_level == 8223);
  CHECK(translated.constant.direction.type == SDL_HAPTIC_CARTESIAN);
  CHECK(translated.constant.direction.dir[0] == 1);

  source = {};
  source.type = rex::input::ForceFeedbackEffectType::kRamp;
  source.duration_ms = UINT32_MAX;
  source.ramp_start = -127;
  source.ramp_end = 127;
  REQUIRE(BuildSdlWheelEffect(source, 25, 1, translated));
  CHECK(translated.ramp.length == UINT16_MAX);
  CHECK(translated.ramp.start == -8191);
  CHECK(translated.ramp.end == 8191);

  source = {};
  source.type = rex::input::ForceFeedbackEffectType::kSquare;
  source.periodic_magnitude = 255;
  source.periodic_offset = -64;
  source.periodic_phase = 64;
  source.periodic_period_ms = 0;
  REQUIRE(BuildSdlWheelEffect(source, 50, 1, translated));
  CHECK(translated.periodic.magnitude == 16383);
  CHECK(translated.periodic.offset == -8256);
  CHECK(translated.periodic.phase == 9035);
  CHECK(translated.periodic.period == 1);

  source.duration_ms = 150;
  source.periodic_magnitude = 10;
  source.periodic_offset = 5;
  REQUIRE(BuildSdlWheelEffect(source, 100, 1, translated, 75));
  CHECK(translated.periodic.magnitude == 24575);
  CHECK(translated.periodic.offset == 1290);

  source = {};
  source.type = rex::input::ForceFeedbackEffectType::kDamper;
  source.conditions[0] = {.center = 127,
                          .right_coefficient = 64,
                          .left_coefficient = -64,
                          .right_saturation = 255,
                          .left_saturation = 128,
                          .deadband = 32};
  REQUIRE(BuildSdlWheelEffect(source, 50, 4, translated));
  CHECK(translated.condition.center[0] == 32767);
  CHECK(translated.condition.right_coeff[0] == 16512);
  CHECK(translated.condition.left_coeff[0] == -16512);
  CHECK(translated.condition.right_sat[0] == 32767);
  CHECK(translated.condition.left_sat[0] == 16448);
  CHECK(translated.condition.deadband[0] == 8224);

  source.type = static_cast<rex::input::ForceFeedbackEffectType>(99);
  CHECK_FALSE(BuildSdlWheelEffect(source, 100, 1, translated));
}

TEST_CASE("Wheel configuration rejects axes, buttons and force above safe bounds",
          "[input][sdl][wheel][config]") {
  CHECK(rex::cvar::GetFlagByName("wheel_force_feedback") == "true");
  CHECK(rex::cvar::GetFlagByName("wheel_force_feedback_gain") == "100");
  CHECK(rex::cvar::GetFlagByName("wheel_force_feedback_continuous_periodic_gain") == "40");
  CHECK(rex::cvar::GetFlagByName("wheel_force_feedback_continuous_constant_gain") == "0");
  CHECK(rex::cvar::GetFlagByName("wheel_force_feedback_minimum_transient_strength") == "75");
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_gain", "0"));
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_gain", "100"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_gain", "101"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_gain", "-1"));
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_periodic_gain", "0"));
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_periodic_gain", "100"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_periodic_gain", "101"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_periodic_gain", "-1"));
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_constant_gain", "0"));
  CHECK(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_constant_gain", "100"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_constant_gain", "101"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_constant_gain", "-1"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_force_feedback_minimum_transient_strength", "101"));

  CHECK(rex::cvar::SetFlagByName("wheel_steering_axis", "-1"));
  CHECK(rex::cvar::SetFlagByName("wheel_steering_axis", "31"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_steering_axis", "32"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_steering_axis", "0"));

  CHECK(rex::cvar::SetFlagByName("wheel_button_start", "-1"));
  CHECK(rex::cvar::SetFlagByName("wheel_button_start", "255"));
  CHECK_FALSE(rex::cvar::SetFlagByName("wheel_button_start", "256"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_button_start", "7"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_force_feedback_gain", "100"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_periodic_gain", "40"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_force_feedback_continuous_constant_gain", "0"));
  REQUIRE(rex::cvar::SetFlagByName("wheel_force_feedback_minimum_transient_strength", "75"));
}
