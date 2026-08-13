/**
 * Regression coverage for SDL controller matrix classification and SDL3
 * result semantics.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/input/input_system.h>
#include <rex/input/sdl/controller_matrix.h>

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_GAMEPAD;
using rex::input::sdl::AreControllerMatrixResumeModesValid;
using rex::input::sdl::ClassifyControllerMatrixAnalog;
using rex::input::sdl::ControllerMatrixExpectedCurrentRumbleRecords;
using rex::input::sdl::ControllerMatrixRumblePatternBit;
using rex::input::sdl::GetControllerMatrixHotplugResumeSeed;
using rex::input::sdl::GetControllerMatrixResumeSeed;
using rex::input::sdl::IsCausalControllerMatrixPair;
using rex::input::sdl::IsControllerInputActive;
using rex::input::sdl::IsControllerMatrixInputReady;
using rex::input::sdl::IsControllerMatrixSurfaceComplete;
using rex::input::sdl::IsSingleControllerMatrixBit;
using rex::input::sdl::SdlBoolToXResult;
using rex::input::sdl::ShouldIgnoreControllerMatrixFocusChurn;
using rex::input::sdl::ShouldSubmitRumble;
using rex::input::sdl::ShouldSuppressControllerMatrixRumble;
using rex::input::sdl::StandardSdlButtonToXInput;

namespace {
class SetStateResultDriver final : public rex::input::InputDriver {
 public:
  explicit SetStateResultDriver(X_RESULT result) : InputDriver(nullptr, 0), result_(result) {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t, uint32_t, rex::input::X_INPUT_CAPABILITIES*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetState(uint32_t, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT SetState(uint32_t, rex::input::X_INPUT_VIBRATION*) override {
    ++calls;
    return result_;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  uint32_t calls = 0;

 private:
  X_RESULT result_;
};
}  // namespace

TEST_CASE("SDL3 boolean rumble results preserve success and failure",
          "[input][sdl][controller-matrix]") {
  CHECK(SdlBoolToXResult(true) == X_ERROR_SUCCESS);
  CHECK(SdlBoolToXResult(false) == X_ERROR_FUNCTION_FAILED);
}

TEST_CASE("Runtime never installs the connected NOP fallback", "[input][sdl][controller-matrix]") {
  CHECK(rex::input::ShouldInstallNopInputDriver(true));
  CHECK_FALSE(rex::input::ShouldInstallNopInputDriver(false));
}

TEST_CASE("Vibration preserves the first connected backend result",
          "[input][sdl][controller-matrix]") {
  rex::input::InputSystem input(nullptr);
  auto disconnected = std::make_unique<SetStateResultDriver>(X_ERROR_DEVICE_NOT_CONNECTED);
  auto failed = std::make_unique<SetStateResultDriver>(X_ERROR_FUNCTION_FAILED);
  auto hidden_success = std::make_unique<SetStateResultDriver>(X_ERROR_SUCCESS);
  auto* disconnected_ptr = disconnected.get();
  auto* failed_ptr = failed.get();
  auto* hidden_success_ptr = hidden_success.get();
  input.AddDriver(std::move(disconnected));
  input.AddDriver(std::move(failed));
  input.AddDriver(std::move(hidden_success));
  rex::input::X_INPUT_VIBRATION vibration = {};

  CHECK(input.SetState(0, &vibration) == X_ERROR_FUNCTION_FAILED);
  CHECK(disconnected_ptr->calls == 1);
  CHECK(failed_ptr->calls == 1);
  CHECK(hidden_success_ptr->calls == 0);
}

TEST_CASE("Controller matrix distinguishes left right and combined rumble",
          "[input][sdl][controller-matrix]") {
  CHECK(ControllerMatrixRumblePatternBit(0) == 0);
  CHECK(ControllerMatrixRumblePatternBit(1) == 1);
  CHECK(ControllerMatrixRumblePatternBit(2) == 2);
  CHECK(ControllerMatrixRumblePatternBit(3) == 4);
  const bool phase_isolated =
      IsControllerMatrixInputReady(true, 7, 7, 0, 0, 0) &&
      !IsControllerMatrixInputReady(true, 7, 7, 4, 0, 0) &&
      !IsControllerMatrixInputReady(true, 7, 7, 0, rex::input::X_INPUT_GAMEPAD_A, 0) &&
      !IsControllerMatrixInputReady(true, 3, 3, 0, 0, 0);
  const bool focus_phase_isolated =
      IsControllerMatrixSurfaceComplete(0xF3FF, 0xF3FF, 0x03FF, 0x03FF, false) &&
      !IsControllerMatrixSurfaceComplete(0xF3FE, 0xF3FF, 0x03FF, 0x03FF, false) &&
      !IsControllerMatrixSurfaceComplete(0xF3FF, 0xF3FF, 0x03FF, 0x03FF, true);
  CHECK((phase_isolated && focus_phase_isolated));
}

TEST_CASE("Controller matrix causal pairs reject overlap and swapped guest controls",
          "[input][sdl][controller-matrix]") {
  CHECK(IsSingleControllerMatrixBit(0x0001));
  CHECK_FALSE(IsSingleControllerMatrixBit(0));
  CHECK_FALSE(IsSingleControllerMatrixBit(0x0003));
  CHECK(IsCausalControllerMatrixPair(0x1000, 0x1000));
  CHECK_FALSE(IsCausalControllerMatrixPair(0x1000, 0x2000));
  CHECK_FALSE(IsCausalControllerMatrixPair(0x3000, 0x3000));
}

TEST_CASE("Controller matrix continuation explicitly reuses completed digital and rumble state",
          "[input][sdl][controller-matrix]") {
  const auto full = GetControllerMatrixResumeSeed(false);
  CHECK(full.button_sdl_mask == 0);
  CHECK(full.button_guest_mask == 0);
  CHECK(full.rumble_pattern_mask == 0);
  CHECK(full.rumble_stop_mask == 0);
  CHECK(full.source_sequence == 0);
  CHECK_FALSE(full.input_ready);

  const auto resumed = GetControllerMatrixResumeSeed(true);
  CHECK(resumed.button_sdl_mask == rex::input::sdl::kControllerMatrixButtonMask);
  CHECK(resumed.button_guest_mask == rex::input::sdl::kControllerMatrixButtonMask);
  CHECK(resumed.rumble_pattern_mask == 7);
  CHECK(resumed.rumble_stop_mask == 7);
  CHECK(resumed.source_sequence == 14);
  CHECK(resumed.source_sequence + 1 == 15);
  CHECK(resumed.input_ready);
  CHECK(ControllerMatrixExpectedCurrentRumbleRecords(false) == 6);
  CHECK(ControllerMatrixExpectedCurrentRumbleRecords(true) == 0);
}

TEST_CASE("Controller matrix hotplug continuation reuses only completed prior phases",
          "[input][sdl][controller-matrix]") {
  const auto full = GetControllerMatrixHotplugResumeSeed(false);
  CHECK_FALSE(full.completed.input_ready);
  CHECK(full.analog_sdl_mask == 0);
  CHECK(full.source_sequence == 0);
  CHECK_FALSE(full.focus_gained);

  const auto resumed = GetControllerMatrixHotplugResumeSeed(true);
  CHECK(resumed.completed.button_sdl_mask == rex::input::sdl::kControllerMatrixButtonMask);
  CHECK(resumed.completed.rumble_pattern_mask == 7);
  CHECK(resumed.analog_sdl_mask == rex::input::sdl::kControllerMatrixAnalogMask);
  CHECK(resumed.analog_guest_mask == rex::input::sdl::kControllerMatrixAnalogMask);
  CHECK(resumed.source_sequence == 24);
  CHECK(resumed.focus_lost);
  CHECK(resumed.focus_neutral);
  CHECK(resumed.focus_gained);
  CHECK(AreControllerMatrixResumeModesValid(false, false));
  CHECK(AreControllerMatrixResumeModesValid(true, false));
  CHECK(AreControllerMatrixResumeModesValid(false, true));
  CHECK_FALSE(AreControllerMatrixResumeModesValid(true, true));
}

TEST_CASE("Rumble submission is focus-safe while stop commands remain effective",
          "[input][sdl][controller-matrix]") {
  CHECK(ShouldSubmitRumble(true, false));
  CHECK(ShouldSubmitRumble(true, true));
  CHECK_FALSE(ShouldSubmitRumble(false, false));
  CHECK(ShouldSubmitRumble(false, true));
  CHECK_FALSE(ShouldSuppressControllerMatrixRumble(false, false));
  CHECK_FALSE(ShouldSuppressControllerMatrixRumble(false, true));
  CHECK(ShouldSuppressControllerMatrixRumble(true, false));
  CHECK_FALSE(ShouldSuppressControllerMatrixRumble(true, true));
}

TEST_CASE("Controller input is inactive whenever the window loses focus",
          "[input][sdl][controller-matrix]") {
  CHECK(IsControllerInputActive(true, true));
  CHECK_FALSE(IsControllerInputActive(true, false));
  CHECK_FALSE(IsControllerInputActive(false, true));
  CHECK_FALSE(IsControllerInputActive(false, false));
}

TEST_CASE("Controller matrix ignores focus churn only after the canonical challenge",
          "[input][sdl][controller-matrix]") {
  CHECK_FALSE(ShouldIgnoreControllerMatrixFocusChurn(false, false, false));
  CHECK_FALSE(ShouldIgnoreControllerMatrixFocusChurn(true, false, false));
  CHECK_FALSE(ShouldIgnoreControllerMatrixFocusChurn(true, true, false));
  CHECK(ShouldIgnoreControllerMatrixFocusChurn(true, true, true));
  CHECK_FALSE(ShouldIgnoreControllerMatrixFocusChurn(false, true, true));
}

TEST_CASE("Controller matrix accepts only standard SDL face and navigation buttons",
          "[input][sdl][controller-matrix]") {
  CHECK(StandardSdlButtonToXInput(0) == rex::input::X_INPUT_GAMEPAD_A);
  CHECK(StandardSdlButtonToXInput(14) == rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT);
  CHECK(StandardSdlButtonToXInput(5) == 0);   // Guide is outside the standard F3FF matrix.
  CHECK(StandardSdlButtonToXInput(19) == 0);  // Elite paddle alias must not satisfy A.
  CHECK(StandardSdlButtonToXInput(21) == 0);
}

TEST_CASE("Controller matrix uses official XInput analog boundaries without mutating state",
          "[input][sdl][controller-matrix]") {
  X_INPUT_GAMEPAD gamepad = {};

  gamepad.thumb_lx = -7849;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0);
  gamepad.thumb_lx = -7850;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x001);
  gamepad.thumb_lx = 7849;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0);
  gamepad.thumb_lx = 7850;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x002);

  gamepad = {};
  gamepad.thumb_ly = -7850;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x004);
  gamepad.thumb_ly = 7850;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x008);

  gamepad = {};
  gamepad.thumb_rx = -8689;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0);
  gamepad.thumb_rx = -8690;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x010);
  gamepad.thumb_rx = 8689;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0);
  gamepad.thumb_rx = 8690;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x020);

  gamepad = {};
  gamepad.thumb_ry = -8690;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x040);
  gamepad.thumb_ry = 8690;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x080);

  gamepad = {};
  gamepad.left_trigger = 30;
  gamepad.right_trigger = 30;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0);
  gamepad.left_trigger = 31;
  gamepad.right_trigger = 31;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x300);

  gamepad.thumb_lx = -32768;
  gamepad.thumb_ly = 32767;
  gamepad.thumb_rx = -32768;
  gamepad.thumb_ry = 32767;
  CHECK(ClassifyControllerMatrixAnalog(gamepad) == 0x399);
}
