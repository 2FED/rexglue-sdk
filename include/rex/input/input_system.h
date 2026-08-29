#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/system/interfaces/input.h>

namespace rex::ui {
class Window;
}

namespace rex::input {

class InputSystem : public system::IInputSystem {
 public:
  explicit InputSystem(rex::ui::Window* window);
  ~InputSystem() override;

  rex::ui::Window* window() const { return window_; }

  X_STATUS Setup() override;
  void Shutdown() override;

  void AddDriver(std::unique_ptr<InputDriver> driver);
  void AttachWindow(rex::ui::Window* window);
  void SetActiveCallback(std::function<bool()> callback);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags, X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags, X_INPUT_KEYSTROKE* out_keystroke);
  uint32_t GetForceFeedbackMaxEffectSlots(uint32_t user_index);
  X_RESULT SetForceFeedbackEffect(uint32_t user_index, const ForceFeedbackEffect& effect,
                                  bool update);
  X_RESULT SetForceFeedbackEffectRunning(uint32_t user_index, uint32_t effect_index, bool running,
                                         uint32_t iterations);
  X_RESULT ForceFeedbackDeviceControl(uint32_t user_index, uint32_t command);

 private:
  rex::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;
  std::mutex merged_states_mutex_;
  std::array<X_INPUT_STATE, 4> merged_states_ = {};
  std::array<bool, 4> merged_states_connected_ = {};
};

constexpr bool ShouldInstallNopInputDriver(bool tool_mode) {
  return tool_mode;
}

/// Create a default InputSystem. Tool mode uses only the deterministic NOP
/// driver; runtime preserves real backend disconnect and failure results.
std::unique_ptr<InputSystem> CreateDefaultInputSystem(bool tool_mode);

}  // namespace rex::input
