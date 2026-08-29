/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <rex/input/input_driver.h>

#include <SDL3/SDL.h>

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace rex::input::sdl {

class SDLInputDriver final : public InputDriver, public rex::ui::WindowListener {
 public:
  explicit SDLInputDriver(rex::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  uint32_t GetForceFeedbackMaxEffectSlots(uint32_t user_index) override;
  X_RESULT SetForceFeedbackEffect(uint32_t user_index, const ForceFeedbackEffect& effect,
                                  bool update) override;
  X_RESULT SetForceFeedbackEffectRunning(uint32_t user_index, uint32_t effect_index, bool running,
                                         uint32_t iterations) override;
  X_RESULT ForceFeedbackDeviceControl(uint32_t user_index, uint32_t command) override;
  void OnWindowAvailable(rex::ui::Window* window) override;

 private:
  static constexpr size_t kWheelForceFeedbackEffectSlots = 64;

  struct WheelEffectState {
    SDL_HapticEffectID effect_id = -1;
    ForceFeedbackEffectType type = ForceFeedbackEffectType::kConstant;
    uint32_t update_count = 0;
    uint32_t iterations = 1;
    uint32_t duration_ms = 0;
    bool running = false;
    bool resume_pending = false;
  };

  struct ControllerState {
    SDL_Gamepad* gamepad;
    SDL_Joystick* joystick;
    SDL_Haptic* haptic;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    uint64_t last_activity_sequence;
    bool state_changed;
    bool is_active;
    bool is_wheel;
    std::array<WheelEffectState, kWheelForceFeedbackEffectSlots> force_feedback_effects;
    uint32_t force_feedback_total_update_count;
    uint32_t force_feedback_total_start_count;
    bool force_feedback_device_info_logged;
  };

  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  // WindowListener
  void OnClosing(rex::ui::UIEvent& e) override;
  void OnLostFocus(rex::ui::UISetupEvent& e) override;
  void OnGotFocus(rex::ui::UISetupEvent& e) override;

  void HandleEvent(const SDL_Event& event);
  std::unique_lock<std::mutex> DrainAndLock();
  void ProcessEventLocked(const SDL_Event& event);
  void OnControllerDeviceAddedLocked(const SDL_Event& event);
  void OnControllerDeviceRemovedLocked(const SDL_Event& event);
  void OnControllerDeviceAxisMotionLocked(const SDL_Event& event);
  void OnControllerDeviceButtonChangedLocked(const SDL_Event& event);
  void OnJoystickDeviceAddedLocked(const SDL_Event& event);
  void OnJoystickDeviceAxisMotionLocked(const SDL_Event& event);
  void OnJoystickDeviceButtonChangedLocked(const SDL_Event& event);
  void OnJoystickDeviceHatMotionLocked(const SDL_Event& event);

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  bool IsControllerConnected(const ControllerState& state) const;
  SDL_Joystick* GetControllerJoystick(const ControllerState& state) const;
  void StopControllerFeedback(ControllerState& state, bool preserve_running = false);
  void ResumeControllerFeedback(ControllerState& state, const char* reason);
  void CloseController(ControllerState& state);
  void UpdateWheelAxisLocked(ControllerState& state, uint8_t axis, int16_t value,
                             bool record_activity);
  std::optional<size_t> GetControllerIndexFromInstanceID(SDL_JoystickID instance_id);
  std::optional<size_t> ResolveControllerIndex(uint32_t user_index) const;
  ControllerState* GetControllerState(uint32_t user_index);
  uint32_t GetConnectedControllerMask() const;
  void SelectActiveControllerLocked(size_t controller_index, const char* reason);
  void SelectFallbackControllerLocked(const char* reason);
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state);
  void QueueControllerUpdate();

  rex::ui::Window* attached_window_ = nullptr;
  bool sdl_events_initialized_;
  bool SDL_Gamepad_initialized_;
  bool sdl_haptic_initialized_;
  std::atomic<int> sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::atomic<bool> has_focus_{true};
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::optional<size_t> active_controller_index_;
  uint64_t controller_activity_sequence_ = 0;
  X_INPUT_STATE slot_zero_state_ = {};
  std::optional<size_t> slot_zero_state_source_;
  bool slot_zero_is_active_ = false;
  std::mutex controllers_mutex_;
  std::mutex event_queue_mutex_;
  std::vector<SDL_Event> pending_events_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;
};

}  // namespace rex::input::sdl
