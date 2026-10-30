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

#include <algorithm>
#include <array>
#include <filesystem>
#include <mutex>
#include <string>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/input/flags.h>
#include <rex/input/sdl/controller_matrix.h>
#include <rex/input/sdl/controller_matrix_audit.h>
#include <rex/input/sdl/input_slot_audit.h>
#include <rex/input/sdl/sdl_input_driver.h>
#include <rex/input/sdl/slot_mapping.h>
#include <rex/input/sdl/wheel_force_feedback.h>
#include <rex/input/sdl/wheel_mapping.h>
#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

REXCVAR_DEFINE_STRING(hid_mappings_file, "gamecontrollerdb.txt", "Input",
                      "Path to SDL gamecontroller mappings file");
// KI-040 opt-in: let SDL see XInput-class pads that Windows could not give one
// of the four XInput user indices (extra or virtual pads). Off by default:
// RawInput without XInput correlation merges triggers and loses rumble.
REXCVAR_DEFINE_BOOL(hid_sdl_rawinput, false, "Input",
                    "Experimental: enumerate gamepads through SDL's Windows RawInput backend");
REXCVAR_DEFINE_INT32(wheel_steering_axis, 0, "Input/Wheel", "Raw steering axis (-1 disables)")
    .range(-1, 31);
REXCVAR_DEFINE_INT32(wheel_brake_axis, 1, "Input/Wheel", "Raw brake axis (-1 disables)")
    .range(-1, 31);
REXCVAR_DEFINE_INT32(wheel_accelerator_axis, 2, "Input/Wheel", "Raw accelerator axis (-1 disables)")
    .range(-1, 31);
REXCVAR_DEFINE_BOOL(wheel_steering_inverted, false, "Input/Wheel", "Invert steering axis");
REXCVAR_DEFINE_BOOL(wheel_brake_inverted, true, "Input/Wheel", "Invert brake axis");
REXCVAR_DEFINE_BOOL(wheel_accelerator_inverted, true, "Input/Wheel", "Invert accelerator axis");
REXCVAR_DEFINE_BOOL(wheel_force_feedback, true, "Input/Wheel",
                    "Enable the safety-bounded Xbox 360 wheel force-feedback route");
REXCVAR_DEFINE_INT32(wheel_force_feedback_gain, 100, "Input/Wheel",
                     "Safety-bounded wheel force-feedback gain percentage")
    .range(0, static_cast<int32_t>(rex::input::sdl::kMaximumWheelForceFeedbackGainPercent));
REXCVAR_DEFINE_INT32(wheel_force_feedback_continuous_periodic_gain,
                     rex::input::sdl::kDefaultContinuousPeriodicGainPercent, "Input/Wheel",
                     "Continuous engine/road periodic force percentage of overall wheel gain")
    .range(0, static_cast<int32_t>(rex::input::sdl::kMaximumWheelForceFeedbackGainPercent));
REXCVAR_DEFINE_INT32(wheel_force_feedback_continuous_constant_gain,
                     rex::input::sdl::kDefaultContinuousConstantGainPercent, "Input/Wheel",
                     "Continuous directional force percentage of overall wheel gain")
    .range(0, static_cast<int32_t>(rex::input::sdl::kMaximumWheelForceFeedbackGainPercent));
REXCVAR_DEFINE_INT32(
    wheel_force_feedback_minimum_transient_strength,
    rex::input::sdl::kDefaultMinimumTransientStrengthPercent, "Input/Wheel",
    "Minimum finite constant/periodic primary strength percentage of current wheel gain")
    .range(0, static_cast<int32_t>(rex::input::sdl::kMaximumWheelForceFeedbackGainPercent));
REXCVAR_DEFINE_INT32(wheel_button_left_shoulder, 0, "Input/Wheel", "Left paddle button")
    .range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_right_shoulder, 1, "Input/Wheel", "Right paddle button")
    .range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_y, 2, "Input/Wheel", "Y button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_x, 3, "Input/Wheel", "X button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_b, 4, "Input/Wheel", "B button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_a, 5, "Input/Wheel", "A button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_back, 6, "Input/Wheel", "Back button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_start, 7, "Input/Wheel", "Start button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_left_thumb, 10, "Input/Wheel", "Left-stick click button")
    .range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_right_thumb, 11, "Input/Wheel", "Right-stick click button")
    .range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_guide, 12, "Input/Wheel", "Guide button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_dpad_up, -1, "Input/Wheel", "D-pad Up button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_dpad_down, -1, "Input/Wheel", "D-pad Down button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_dpad_left, -1, "Input/Wheel", "D-pad Left button").range(-1, 255);
REXCVAR_DEFINE_INT32(wheel_button_dpad_right, -1, "Input/Wheel", "D-pad Right button")
    .range(-1, 255);

namespace rex::input::sdl {

namespace {

WheelMapping GetConfiguredWheelMapping() {
  return {
      .steering_axis = REXCVAR_GET(wheel_steering_axis),
      .brake_axis = REXCVAR_GET(wheel_brake_axis),
      .accelerator_axis = REXCVAR_GET(wheel_accelerator_axis),
      .steering_inverted = REXCVAR_GET(wheel_steering_inverted),
      .brake_inverted = REXCVAR_GET(wheel_brake_inverted),
      .accelerator_inverted = REXCVAR_GET(wheel_accelerator_inverted),
      .left_shoulder_button = REXCVAR_GET(wheel_button_left_shoulder),
      .right_shoulder_button = REXCVAR_GET(wheel_button_right_shoulder),
      .y_button = REXCVAR_GET(wheel_button_y),
      .x_button = REXCVAR_GET(wheel_button_x),
      .b_button = REXCVAR_GET(wheel_button_b),
      .a_button = REXCVAR_GET(wheel_button_a),
      .back_button = REXCVAR_GET(wheel_button_back),
      .start_button = REXCVAR_GET(wheel_button_start),
      .left_thumb_button = REXCVAR_GET(wheel_button_left_thumb),
      .right_thumb_button = REXCVAR_GET(wheel_button_right_thumb),
      .guide_button = REXCVAR_GET(wheel_button_guide),
      .dpad_up_button = REXCVAR_GET(wheel_button_dpad_up),
      .dpad_down_button = REXCVAR_GET(wheel_button_dpad_down),
      .dpad_left_button = REXCVAR_GET(wheel_button_dpad_left),
      .dpad_right_button = REXCVAR_GET(wheel_button_dpad_right),
  };
}

void LogWheelEffectParameters(const ForceFeedbackEffect& guest, const SDL_HapticEffect& host,
                              const char* action, uint32_t gain) {
  switch (guest.type) {
    case ForceFeedbackEffectType::kConstant:
      REXLOG_INFO(
          "SDL_WHEEL_FFB_PARAMS v=1 action={} slot={} type=constant duration={} delay={} "
          "guest_level={} host_level={} guest_attack={} host_attack={} guest_fade={} "
          "host_fade={} gain_percent={}",
          action, guest.effect_index, guest.duration_ms, guest.start_delay_ms, guest.constant_level,
          host.constant.level, guest.attack_level, host.constant.attack_level, guest.fade_level,
          host.constant.fade_level, gain);
      break;
    case ForceFeedbackEffectType::kRamp:
      REXLOG_INFO(
          "SDL_WHEEL_FFB_PARAMS v=1 action={} slot={} type=ramp duration={} delay={} "
          "guest_start={} guest_end={} host_start={} host_end={} gain_percent={}",
          action, guest.effect_index, guest.duration_ms, guest.start_delay_ms, guest.ramp_start,
          guest.ramp_end, host.ramp.start, host.ramp.end, gain);
      break;
    case ForceFeedbackEffectType::kSquare:
    case ForceFeedbackEffectType::kSine:
    case ForceFeedbackEffectType::kTriangle:
    case ForceFeedbackEffectType::kSawtoothUp:
    case ForceFeedbackEffectType::kSawtoothDown:
      REXLOG_INFO(
          "SDL_WHEEL_FFB_PARAMS v=1 action={} slot={} type={} duration={} delay={} "
          "guest_magnitude={} guest_offset={} guest_period={} host_magnitude={} "
          "host_offset={} host_period={} guest_phase={} host_phase={} guest_attack={} "
          "host_attack={} guest_fade={} host_fade={} gain_percent={}",
          action, guest.effect_index, GetWheelEffectTypeName(guest.type), guest.duration_ms,
          guest.start_delay_ms, guest.periodic_magnitude, guest.periodic_offset,
          guest.periodic_period_ms, host.periodic.magnitude, host.periodic.offset,
          host.periodic.period, guest.periodic_phase, host.periodic.phase, guest.attack_level,
          host.periodic.attack_level, guest.fade_level, host.periodic.fade_level, gain);
      break;
    case ForceFeedbackEffectType::kSpring:
    case ForceFeedbackEffectType::kDamper: {
      const auto& condition = guest.conditions[0];
      REXLOG_INFO(
          "SDL_WHEEL_FFB_PARAMS v=1 action={} slot={} type={} duration={} delay={} "
          "guest_center={} guest_right_coeff={} guest_left_coeff={} guest_right_sat={} "
          "guest_left_sat={} guest_deadband={} host_center={} host_right_coeff={} "
          "host_left_coeff={} host_right_sat={} host_left_sat={} host_deadband={} "
          "gain_percent={}",
          action, guest.effect_index, GetWheelEffectTypeName(guest.type), guest.duration_ms,
          guest.start_delay_ms, condition.center, condition.right_coefficient,
          condition.left_coefficient, condition.right_saturation, condition.left_saturation,
          condition.deadband, host.condition.center[0], host.condition.right_coeff[0],
          host.condition.left_coeff[0], host.condition.right_sat[0], host.condition.left_sat[0],
          host.condition.deadband[0], gain);
      break;
    }
  }
}

// KI-022: one coalesced host resync request, shared by all driver instances
// and consumed on the poll path of the active wheel.
std::mutex g_wheel_resync_request_mutex;
WheelResyncRequestSlot g_wheel_resync_request;

bool TakeWheelResyncRequest(uint64_t now_ms, std::string& reason) {
  std::lock_guard lock(g_wheel_resync_request_mutex);
  return g_wheel_resync_request.Take(now_ms, reason);
}

}  // namespace

void RequestWheelEffectResync(const char* reason) {
  std::string tag = NormalizeWheelResyncHostReason(reason);
  const uint64_t now_ms = SDL_GetTicks();
  std::lock_guard lock(g_wheel_resync_request_mutex);
  g_wheel_resync_request.Offer(std::move(tag), now_ms);
}

SDLInputDriver::SDLInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order),
      sdl_events_initialized_(false),
      SDL_Gamepad_initialized_(false),
      sdl_haptic_initialized_(false),
      sdl_events_unflushed_(0),
      sdl_pumpevents_queued_(false),
      controllers_(),
      controllers_mutex_(),
      keystroke_states_() {}

SDLInputDriver::~SDLInputDriver() {}

X_STATUS SDLInputDriver::Setup() {
  if (!TestSDLVersion()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

void SDLInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window && !attached_window_) {
    attached_window_ = window;
    window->AddListener(this);
    window->app_context().CallInUIThreadSynchronous([this]() {
      // Initialize SDL events subsystem
      if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        REXLOG_ERROR("SDL: Failed to init events subsystem: {}", SDL_GetError());
        return;
      }
      sdl_events_initialized_ = true;
      pending_events_.reserve(64);

      // With an event watch we will always get notified, even if the event queue
      // is full, which can happen if another subsystem does not clear its events.
      SDL_AddEventWatch(
          [](void* userdata, SDL_Event* event) -> bool {
            if (!userdata || !event) {
              assert_always();
              return false;
            }

            const auto type = event->type;
            if (type < SDL_EVENT_JOYSTICK_AXIS_MOTION || type >= SDL_EVENT_FINGER_DOWN) {
              return false;
            }

            // If another part of rex uses another SDL subsystem that generates
            // events, this may seem like a bad idea. They will however not
            // subscribe to controller events so we get away with that.
            const auto driver = static_cast<SDLInputDriver*>(userdata);
            driver->HandleEvent(*event);

            return false;
          },
          this);

      // Hints must be set before the joystick subsystem starts.
      if (REXCVAR_GET(hid_sdl_rawinput)) {
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
        REXLOG_INFO("SDL: RawInput joystick backend enabled (hid_sdl_rawinput).");
      }

      // Initialize game controller subsystem
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        REXLOG_ERROR("SDL: Failed to init gamecontroller subsystem: {}", SDL_GetError());
        return;
      }
      SDL_Gamepad_initialized_ = true;

      if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        REXLOG_WARN("SDL: Failed to init optional haptic subsystem: {}", SDL_GetError());
      } else {
        sdl_haptic_initialized_ = true;
      }

      // Load custom controller mappings if available
      if (!REXCVAR_GET(hid_mappings_file).empty()) {
        std::filesystem::path mappings_path(REXCVAR_GET(hid_mappings_file));
        if (!std::filesystem::exists(mappings_path)) {
          REXLOG_WARN("SDL GameControllerDB: file '{}' does not exist.",
                      REXCVAR_GET(hid_mappings_file));
        } else {
          auto mappings_result =
              SDL_AddGamepadMappingsFromFile(REXCVAR_GET(hid_mappings_file).c_str());
          if (mappings_result < 0) {
            REXLOG_ERROR("SDL GameControllerDB: error loading file '{}': {}.",
                         REXCVAR_GET(hid_mappings_file), mappings_result);
          } else {
            REXLOG_INFO("SDL GameControllerDB: loaded {} mappings.", mappings_result);
          }
        }
      }

      REXLOG_INFO("SDL input driver initialized successfully");
    });
  }
}

void SDLInputDriver::OnClosing(rex::ui::UIEvent&) {
  if (attached_window_) {
    attached_window_->RemoveListener(this);
    if (sdl_pumpevents_queued_) {
      attached_window_->app_context().CallInUIThreadSynchronous(
          [this]() { attached_window_->app_context().ExecutePendingFunctionsFromUIThread(); });
    }
    for (auto& controller : controllers_) {
      CloseController(controller);
    }
    active_controller_index_.reset();
    slot_zero_state_source_.reset();
    slot_zero_state_ = {};
    slot_zero_is_active_ = false;
    if (sdl_haptic_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_HAPTIC);
      sdl_haptic_initialized_ = false;
    }
    if (SDL_Gamepad_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
      SDL_Gamepad_initialized_ = false;
    }
    if (sdl_events_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_EVENTS);
      sdl_events_initialized_ = false;
    }
    attached_window_ = nullptr;
  }
}

void SDLInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  has_focus_ = false;
  {
    std::lock_guard lock(controllers_mutex_);
    for (auto& controller : controllers_) {
      StopControllerFeedback(controller, true);
    }
    for (auto& state : keystroke_states_) {
      state.repeat_state = RepeatState::Idle;
      state.repeat_time = 0;
    }
  }
  internal::RecordControllerMatrixFocus(false);
}

void SDLInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  has_focus_ = true;
  {
    std::lock_guard lock(controllers_mutex_);
    for (auto& controller : controllers_) {
      const auto controller_index = static_cast<size_t>(&controller - controllers_.data());
      if (active_controller_index_ == controller_index) {
        ResumeControllerFeedback(controller, "focus");
      }
    }
  }
  internal::RecordControllerMatrixFocus(true);
}

X_RESULT SDLInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Unfortunately drivers can't present all information immediately (e.g.
  // battery information) so this needs to be refreshed every time.
  UpdateXCapabilities(*controller);

  std::memcpy(out_caps, &controller->caps, sizeof(*out_caps));

  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  auto is_active = IsControllerInputActive(this->is_active(), has_focus_.load());

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  if (is_active && controller->is_wheel && controller->haptic) {
    const uint64_t now_ms = SDL_GetTicks();
    if (ShouldRunWheelRearmWatchdog(now_ms, controller->force_feedback_rearm_check_ms)) {
      controller->force_feedback_rearm_check_ms = now_ms;
      RearmStoppedWheelEffectsLocked(*controller, "watchdog");
    }
    ServiceWheelEffectResyncLocked(*controller, now_ms);
  }

  if (user_index == 0) {
    const bool source_changed = slot_zero_state_source_ != active_controller_index_;
    if (source_changed || is_active != slot_zero_is_active_ ||
        (is_active && controller->state_changed)) {
      slot_zero_state_.packet_number++;
    }
    slot_zero_state_.gamepad = controller->state.gamepad;
    slot_zero_state_source_ = active_controller_index_;
    slot_zero_is_active_ = is_active;
    controller->state_changed = false;
    std::memcpy(out_state, &slot_zero_state_, sizeof(*out_state));
    if (!is_active) {
      std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
    }
    return X_ERROR_SUCCESS;
  }

  // Make sure packet_number is only incremented by 1, even if there have been
  // multiple updates between GetState calls. Also track `is_active` to
  // increment the packet number if it changed.
  if ((is_active != controller->is_active) || (is_active && controller->state_changed)) {
    controller->state.packet_number++;
    controller->is_active = is_active;
    controller->state_changed = false;
  }
  std::memcpy(out_state, &controller->state, sizeof(*out_state));
  if (!is_active) {
    // Simulate an "untouched" controller. When we become active again the
    // pressed buttons aren't lost and will be visible again.
    std::memset(&out_state->gamepad, 0, sizeof(out_state->gamepad));
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  if (user_index >= HID_SDL_USER_COUNT) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  QueueControllerUpdate();

  auto guard = DrainAndLock();

  auto controller = GetControllerState(user_index);
  if (!controller) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

#if SDL_VERSION_ATLEAST(2, 0, 9)
  const bool stopping = static_cast<uint16_t>(vibration->left_motor_speed) == 0 &&
                        static_cast<uint16_t>(vibration->right_motor_speed) == 0;
  const uint8_t motor_mask = (static_cast<uint16_t>(vibration->left_motor_speed) ? 1 : 0) |
                             (static_cast<uint16_t>(vibration->right_motor_speed) ? 2 : 0);
  if (internal::ShouldSuppressControllerMatrixResumeRumble(stopping)) {
    internal::RecordControllerMatrixResumeRumbleSuppressed(user_index, motor_mask);
    return X_ERROR_FUNCTION_FAILED;
  }
  if (!ShouldSubmitRumble(has_focus_.load(), stopping)) {
    return X_ERROR_SUCCESS;
  }

  if (controller->is_wheel) {
    // Basic XInput motor vibration is not the Xbox 360 wheel force-feedback
    // API. Do not turn it into an unsigned steering force; the independently
    // bounded XInputdFF condition-effect route handles wheel forces.
    return X_ERROR_SUCCESS;
  }

  const bool supported = SDL_GetBooleanProperty(SDL_GetGamepadProperties(controller->gamepad),
                                                SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
  const X_RESULT result = SdlBoolToXResult(SDL_RumbleGamepad(
      controller->gamepad, vibration->left_motor_speed, vibration->right_motor_speed, 0));
  internal::RecordControllerMatrixRumble(user_index, motor_mask, supported, result);
  return result;
#else
  return X_ERROR_SUCCESS;
#endif
}

uint32_t SDLInputDriver::GetForceFeedbackMaxEffectSlots(uint32_t user_index) {
  if (!REXCVAR_GET(wheel_force_feedback) || user_index >= HID_SDL_USER_COUNT) {
    return 0;
  }
  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller || !controller->is_wheel || !controller->haptic) {
    return 0;
  }
  const uint32_t features = SDL_GetHapticFeatures(controller->haptic);
  const uint32_t effect_slots =
      (features & kMclaWheelHapticFeatures) == 0
          ? 0
          : static_cast<uint32_t>(std::clamp(SDL_GetMaxHapticEffects(controller->haptic), 0,
                                             static_cast<int>(kWheelForceFeedbackEffectSlots)));
  if (!controller->force_feedback_device_info_logged) {
    controller->force_feedback_device_info_logged = true;
    REXLOG_INFO(
        "SDL_WHEEL_FFB_DEVICE_INFO v=2 guest_slots={} host_effects={} host_playing={} "
        "features=0x{:08X}",
        effect_slots, SDL_GetMaxHapticEffects(controller->haptic),
        SDL_GetMaxHapticEffectsPlaying(controller->haptic), features);
  }
  return effect_slots;
}

X_RESULT SDLInputDriver::SetForceFeedbackEffect(uint32_t user_index,
                                                const ForceFeedbackEffect& effect, bool update) {
  if (user_index >= HID_SDL_USER_COUNT || effect.effect_index == 0 ||
      effect.effect_index > kWheelForceFeedbackEffectSlots) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!REXCVAR_GET(wheel_force_feedback)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller || !controller->is_wheel || !controller->haptic) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  const int max_effects = std::clamp(SDL_GetMaxHapticEffects(controller->haptic), 0,
                                     static_cast<int>(kWheelForceFeedbackEffectSlots));
  if (effect.effect_index > static_cast<uint32_t>(max_effects)) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  const uint32_t overall_gain = static_cast<uint32_t>(REXCVAR_GET(wheel_force_feedback_gain));
  const uint32_t gain = GetWheelEffectGainPercent(
      effect, overall_gain,
      static_cast<uint32_t>(REXCVAR_GET(wheel_force_feedback_continuous_periodic_gain)),
      static_cast<uint32_t>(REXCVAR_GET(wheel_force_feedback_continuous_constant_gain)));
  const int haptic_axes = std::clamp(SDL_GetNumHapticAxes(controller->haptic), 0, 3);
  SDL_HapticEffect sdl_effect = {};
  if (!BuildSdlWheelEffect(
          effect, gain, haptic_axes, sdl_effect,
          static_cast<uint32_t>(REXCVAR_GET(wheel_force_feedback_minimum_transient_strength)))) {
    REXLOG_WARN("SDL wheel FFB rejected unknown effect type {}.",
                static_cast<uint32_t>(effect.type));
    return X_ERROR_FUNCTION_FAILED;
  }

  if (!SDL_HapticEffectSupported(controller->haptic, &sdl_effect)) {
    REXLOG_WARN("SDL wheel does not support title-requested effect type {}.",
                GetWheelEffectTypeName(effect.type));
    return X_ERROR_FUNCTION_FAILED;
  }

  auto& effect_state = controller->force_feedback_effects[effect.effect_index - 1];
  bool submitted = false;
  bool recreated = false;
  if (effect_state.effect_id >= 0 && update && effect_state.type == effect.type) {
    submitted = SDL_UpdateHapticEffect(controller->haptic, effect_state.effect_id, &sdl_effect);
  } else {
    const bool restart = effect_state.running && has_focus_.load();
    if (IsWheelEffectRetype(update, effect_state.effect_id >= 0, effect_state.type,
                            effect.type)) {
      REXLOG_INFO("{}", FormatWheelRetypeLine(effect.effect_index, effect_state.type, effect.type,
                                              restart));
    }
    if (effect_state.effect_id >= 0) {
      SDL_DestroyHapticEffect(controller->haptic, effect_state.effect_id);
      effect_state.effect_id = -1;
      effect_state.running = false;
    }
    effect_state.effect_id = SDL_CreateHapticEffect(controller->haptic, &sdl_effect);
    submitted = effect_state.effect_id >= 0;
    recreated = true;
    if (submitted && restart) {
      submitted =
          SDL_RunHapticEffect(controller->haptic, effect_state.effect_id, effect_state.iterations);
      effect_state.running = submitted;
      if (submitted) {
        controller->force_feedback_total_start_count++;
      }
    }
  }
  if (!submitted) {
    REXLOG_ERROR("SDL wheel FFB {} failed for slot {} type {}: {}", recreated ? "create" : "update",
                 effect.effect_index, GetWheelEffectTypeName(effect.type), SDL_GetError());
    return X_ERROR_FUNCTION_FAILED;
  }

  effect_state.type = effect.type;
  effect_state.duration_ms = effect.duration_ms;
  {
    const bool neutral = IsWheelEffectNeutral(effect, gain);
    const bool finite = effect.duration_ms != UINT32_MAX;
    if (!update || recreated || effect_state.update_count == 0) {
      // Create, retype and the first update already log their parameters.
      ResetWheelUpdateChange(effect_state.update_change, neutral, finite);
    } else {
      switch (ObserveWheelUpdateChange(effect_state.update_change, neutral, finite,
                                       SDL_GetTicks())) {
        case WheelUpdateChangeLog::kLog:
          REXLOG_INFO("{}", FormatWheelUpdateChangeLine(effect.effect_index, effect.type, neutral,
                                                        effect.duration_ms, gain));
          LogWheelEffectParameters(effect, sdl_effect, "update-change", gain);
          break;
        case WheelUpdateChangeLog::kLimitReached:
          REXLOG_WARN("{}", FormatWheelUpdateChangeLimitLine(effect.effect_index, effect.type));
          break;
        case WheelUpdateChangeLog::kNone:
          break;
      }
    }
  }
  if (update && !recreated) {
    // An update never restarts a stopped DirectInput effect. Re-arm this slot
    // if it is a running infinite effect the device reports as stopped.
    RearmStoppedWheelEffectsLocked(*controller, "update", effect.effect_index - 1);
  }
  if (update) {
    if (effect_state.update_count == 0) {
      LogWheelEffectParameters(effect, sdl_effect, "first-update", gain);
    }
    effect_state.update_count++;
    controller->force_feedback_total_update_count++;
    if (effect_state.update_count == 1) {
      REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action=first-update slot={} type={} gain_percent={}",
                  effect.effect_index, GetWheelEffectTypeName(effect.type), gain);
    }
  } else {
    LogWheelEffectParameters(effect, sdl_effect, "create", gain);
    REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action=create slot={} type={} gain_percent={}",
                effect.effect_index, GetWheelEffectTypeName(effect.type), gain);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::SetForceFeedbackEffectRunning(uint32_t user_index, uint32_t effect_index,
                                                       bool running, uint32_t iterations) {
  if (user_index >= HID_SDL_USER_COUNT || effect_index == 0 ||
      effect_index > kWheelForceFeedbackEffectSlots) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!REXCVAR_GET(wheel_force_feedback)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller || !controller->is_wheel || !controller->haptic) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  auto& effect_state = controller->force_feedback_effects[effect_index - 1];
  if (effect_state.effect_id < 0) {
    return X_ERROR_FUNCTION_FAILED;
  }

  if (running) {
    effect_state.iterations = NormalizeWheelEffectIterations(iterations);
  }

  if (running && !has_focus_.load()) {
    if (effect_state.duration_ms != UINT32_MAX) {
      effect_state.running = false;
      effect_state.resume_pending = false;
      REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action=drop-start slot={} type={} reason=focus-finite",
                  effect_index, GetWheelEffectTypeName(effect_state.type));
      return X_ERROR_SUCCESS;
    }
    const bool first_deferred_start = !effect_state.resume_pending;
    effect_state.running = false;
    effect_state.resume_pending = true;
    if (first_deferred_start) {
      REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action=defer-start slot={} type={} reason=focus",
                  effect_index, GetWheelEffectTypeName(effect_state.type));
    }
    return X_ERROR_SUCCESS;
  }

  const bool status_supported =
      (SDL_GetHapticFeatures(controller->haptic) & SDL_HAPTIC_STATUS) != 0;
  const bool status_playing = running && effect_state.running && status_supported &&
                              SDL_GetHapticEffectStatus(controller->haptic, effect_state.effect_id);
  if (running && !ShouldSubmitWheelEffectStart(effect_state.duration_ms, effect_state.running,
                                               status_supported, status_playing)) {
    // Avoid re-articulating a genuinely running infinite condition or engine
    // effect. Finite event starts are never suppressed by status.
    if (ShouldLogWheelStartSuppressed(effect_state.start_suppressed_logged)) {
      REXLOG_INFO("{}", FormatWheelStartSuppressedLine(effect_index, effect_state.type));
    }
    return X_ERROR_SUCCESS;
  }

  const bool submitted = running ? SDL_RunHapticEffect(controller->haptic, effect_state.effect_id,
                                                       effect_state.iterations)
                                 : SDL_StopHapticEffect(controller->haptic, effect_state.effect_id);
  if (!submitted) {
    REXLOG_ERROR("SDL wheel FFB {} failed for slot {} type {}: {}", running ? "start" : "stop",
                 effect_index, GetWheelEffectTypeName(effect_state.type), SDL_GetError());
    return X_ERROR_FUNCTION_FAILED;
  }
  effect_state.running = running;
  effect_state.resume_pending = false;
  effect_state.start_suppressed_logged = false;
  if (running) {
    controller->force_feedback_total_start_count++;
    if (effect_state.duration_ms != UINT32_MAX) {
      // KI-022: a burst of finite event starts can make some wheel drivers
      // drop the infinite effects silently; resync after the burst ends.
      controller->force_feedback_burst.OnFiniteStart(SDL_GetTicks());
    }
  }
  REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action={} slot={} type={}", running ? "start" : "stop",
              effect_index, GetWheelEffectTypeName(effect_state.type));
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::ForceFeedbackDeviceControl(uint32_t user_index, uint32_t command) {
  if (!REXCVAR_GET(wheel_force_feedback)) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  QueueControllerUpdate();
  auto guard = DrainAndLock();
  auto* controller = GetControllerState(user_index);
  if (!controller || !controller->is_wheel || !controller->haptic) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  REXLOG_INFO("SDL_WHEEL_FFB_DEVICE_CONTROL v=1 command={}", command);
  return X_ERROR_SUCCESS;
}

X_RESULT SDLInputDriver::GetKeystroke(uint32_t users, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  // TODO(JoelLinn): Figure out the flags
  // https://github.com/evilC/UCR/blob/0489929e2a8e39caa3484c67f3993d3fba39e46f/Libraries/XInput.ahk#L85-L98
  assert(sdl_events_initialized_ && SDL_Gamepad_initialized_);
  bool user_any = users == 0xFF;
  if (users >= HID_SDL_USER_COUNT && !user_any) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (!out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }

  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<rex::ui::VirtualKey, 34> kVkLookup = {
      // 00 - True buttons from xinput button field
      rex::ui::VirtualKey::kXInputPadDpadUp,
      rex::ui::VirtualKey::kXInputPadDpadDown,
      rex::ui::VirtualKey::kXInputPadDpadLeft,
      rex::ui::VirtualKey::kXInputPadDpadRight,
      rex::ui::VirtualKey::kXInputPadStart,
      rex::ui::VirtualKey::kXInputPadBack,
      rex::ui::VirtualKey::kXInputPadLThumbPress,
      rex::ui::VirtualKey::kXInputPadRThumbPress,
      rex::ui::VirtualKey::kXInputPadLShoulder,
      rex::ui::VirtualKey::kXInputPadRShoulder,
      rex::ui::VirtualKey::kNone, /* Guide has no VK */
      rex::ui::VirtualKey::kNone, /* Unknown */
      rex::ui::VirtualKey::kXInputPadA,
      rex::ui::VirtualKey::kXInputPadB,
      rex::ui::VirtualKey::kXInputPadX,
      rex::ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      rex::ui::VirtualKey::kXInputPadLTrigger,
      rex::ui::VirtualKey::kXInputPadRTrigger,
      // 18
      rex::ui::VirtualKey::kXInputPadLThumbUp,
      rex::ui::VirtualKey::kXInputPadLThumbDown,
      rex::ui::VirtualKey::kXInputPadLThumbRight,
      rex::ui::VirtualKey::kXInputPadLThumbLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadLThumbUpRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownRight,
      rex::ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      rex::ui::VirtualKey::kXInputPadRThumbUp,
      rex::ui::VirtualKey::kXInputPadRThumbDown,
      rex::ui::VirtualKey::kXInputPadRThumbRight,
      rex::ui::VirtualKey::kXInputPadRThumbLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpLeft,
      rex::ui::VirtualKey::kXInputPadRThumbUpRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownRight,
      rex::ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  auto is_active = IsControllerInputActive(this->is_active(), has_focus_.load());

  if (is_active) {
    QueueControllerUpdate();
  }

  auto guard = DrainAndLock();

  for (uint32_t user_index = (user_any ? 0 : users);
       user_index < (user_any ? HID_SDL_USER_COUNT : users + 1); user_index++) {
    auto controller = GetControllerState(user_index);
    if (!controller) {
      if (user_any) {
        continue;
      } else {
        return X_ERROR_DEVICE_NOT_CONNECTED;
      }
    }

    // If input is not active (e.g. due to a dialog overlay), force buttons to
    // "unpressed". The algorithm will automatically send UP events when
    // `is_active()` goes low and DOWN events when it goes high again.
    const uint64_t curr_butts =
        is_active
            ? (controller->state.gamepad.buttons | AnalogToKeyfield(controller->state.gamepad))
            : uint64_t(0);
    KeystrokeState& last = keystroke_states_.at(user_index);

    // Handle repeating
    auto guest_now = rex::chrono::Clock::QueryGuestUptimeMillis();
    static_assert(HID_SDL_REPEAT_DELAY >= HID_SDL_REPEAT_RATE);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + HID_SDL_REPEAT_DELAY < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + HID_SDL_REPEAT_RATE < guest_now)) {
      last.repeat_time = guest_now;
      rex::ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != rex::ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    auto butts_changed = curr_butts ^ last.buttons;
    if (!butts_changed) {
      continue;
    }

    // First try to clear buttons with up events. This is to match xinput
    // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
    // up before THUMB_LEFT is down.
    for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2; clear_pass = false, i++) {
      for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
        auto fbutton = uint64_t(1) << i;
        if (!(butts_changed & fbutton)) {
          continue;
        }
        rex::ui::VirtualKey vk = kVkLookup.at(i);
        if (vk == rex::ui::VirtualKey::kNone) {
          continue;
        }

        out_keystroke->virtual_key = uint16_t(vk);
        out_keystroke->unicode = 0;
        out_keystroke->user_index = user_index;
        out_keystroke->hid_code = 0;

        bool is_pressed = curr_butts & fbutton;
        if (clear_pass && !is_pressed) {
          // up
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
          last.buttons &= ~fbutton;
          last.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && is_pressed) {
          // down
          out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
          last.buttons |= fbutton;
          last.repeat_state = RepeatState::Waiting;
          last.repeat_butt_idx = i;
          last.repeat_time = guest_now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

void SDLInputDriver::HandleEvent(const SDL_Event& event) {
  // This callback will likely run on the thread that posts the event, which
  // may be a dedicated thread SDL has created for the joystick subsystem.

  // Event queue should never be (this) full
  assert(SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) < 0xFFFF);

  // The queue could grow up to 3.5MB since it is never polled.
  if (++sdl_events_unflushed_ > 64) {
    SDL_FlushEvents(SDL_EVENT_JOYSTICK_AXIS_MOTION, SDL_EVENT_FINGER_DOWN - 1);
    sdl_events_unflushed_ = 0;
  }

  // Buffer only - no controllers_mutex_ acquisition here.
  // This breaks the lock ordering inversion between controllers_mutex_ and
  // SDL's internal joystick lock that caused deadlocks.
  std::lock_guard<std::mutex> guard(event_queue_mutex_);
  pending_events_.push_back(event);
}

std::unique_lock<std::mutex> SDLInputDriver::DrainAndLock() {
  std::vector<SDL_Event> events;
  {
    std::lock_guard<std::mutex> guard(event_queue_mutex_);
    events.swap(pending_events_);
  }
  std::unique_lock<std::mutex> guard(controllers_mutex_);
  for (const auto& event : events) {
    ProcessEventLocked(event);
  }
  return guard;
}

void SDLInputDriver::ProcessEventLocked(const SDL_Event& event) {
  switch (event.type) {
    case SDL_EVENT_JOYSTICK_ADDED:
      OnJoystickDeviceAddedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
      OnJoystickDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
      OnJoystickDeviceButtonChangedLocked(event);
      break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
      OnJoystickDeviceHatMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_ADDED:
      OnControllerDeviceAddedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_REMOVED:
      OnControllerDeviceRemovedLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      OnControllerDeviceAxisMotionLocked(event);
      break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      OnControllerDeviceButtonChangedLocked(event);
      break;
    default:
      break;
  }
}

void SDLInputDriver::OnJoystickDeviceAddedLocked(const SDL_Event& event) {
  const SDL_JoystickID instance_id = event.jdevice.which;
  const uint16_t vendor_id = SDL_GetJoystickVendorForID(instance_id);
  const uint16_t product_id = SDL_GetJoystickProductForID(instance_id);
  if (!IsSupportedWheel(vendor_id, product_id, SDL_GetJoystickTypeForID(instance_id))) {
    return;
  }
  if (GetControllerIndexFromInstanceID(instance_id)) {
    return;
  }

  SDL_Joystick* joystick = SDL_OpenJoystick(instance_id);
  if (!joystick) {
    REXLOG_ERROR("SDL wheel open failed for instance {}: {}", instance_id, SDL_GetError());
    return;
  }

  uint32_t occupied_slot_mask = GetConnectedControllerMask();
  const auto selected_slot =
      ChooseControllerSlot(SDL_GetJoystickPlayerIndex(joystick), occupied_slot_mask);
  if (!selected_slot) {
    SDL_CloseJoystick(joystick);
    REXLOG_WARN("SDL wheel ignored because all four physical slots are occupied.");
    return;
  }

  auto& state = controllers_.at(*selected_slot);
  state = {};
  state.joystick = joystick;
  state.is_wheel = true;
  state.state_changed = true;
  SDL_SetJoystickPlayerIndex(joystick, static_cast<int>(*selected_slot));

  if (sdl_haptic_initialized_) {
    state.haptic = SDL_OpenHapticFromJoystick(joystick);
    if (state.haptic && SDL_HapticRumbleSupported(state.haptic) &&
        !SDL_InitHapticRumble(state.haptic)) {
      REXLOG_WARN("SDL wheel rumble initialization failed: {}", SDL_GetError());
    }
  }

  const int axis_count = SDL_GetNumJoystickAxes(joystick);
  for (int axis = 0; axis < axis_count; ++axis) {
    int16_t initial_value = 0;
    if (SDL_GetJoystickAxisInitialState(joystick, axis, &initial_value)) {
      UpdateWheelAxisLocked(state, static_cast<uint8_t>(axis), initial_value, false);
    }
  }
  if (SDL_GetNumJoystickHats(joystick) > 0) {
    state.state.gamepad.buttons = static_cast<uint16_t>(state.state.gamepad.buttons) |
                                  WheelHatToXInput(SDL_GetJoystickHat(joystick, 0));
  }
  UpdateXCapabilities(state);

  internal::RecordInputSlotDeviceAssigned(static_cast<uint32_t>(*selected_slot));
  internal::RecordControllerMatrixDeviceAssigned(static_cast<uint32_t>(*selected_slot));
  REXLOG_INFO(
      "SDL_WHEEL_CONNECTED v=1 physical_slot={} vendor=0x{:04X} product=0x{:04X} "
      "axes={} buttons={} hats={} haptic={}",
      *selected_slot, vendor_id, product_id, axis_count, SDL_GetNumJoystickButtons(joystick),
      SDL_GetNumJoystickHats(joystick), state.haptic ? 1 : 0);
  if (!active_controller_index_) {
    SelectActiveControllerLocked(*selected_slot, "first-connected-wheel");
  }
}

void SDLInputDriver::OnControllerDeviceAddedLocked(const SDL_Event& event) {
  const uint16_t vendor_id = SDL_GetJoystickVendorForID(event.gdevice.which);
  const uint16_t product_id = SDL_GetJoystickProductForID(event.gdevice.which);
  if (IsSupportedWheel(vendor_id, product_id, SDL_GetJoystickTypeForID(event.gdevice.which))) {
    // SDL normally emits JOYSTICK_ADDED before GAMEPAD_ADDED, but do not rely
    // on event ordering: T300 must stay on the raw wheel path in every mode.
    OnJoystickDeviceAddedLocked(event);
    return;
  }
  if (GetControllerIndexFromInstanceID(event.gdevice.which)) {
    return;
  }
  // Open the controller.
  const auto controller = SDL_OpenGamepad(event.gdevice.which);
  if (!controller) {
    assert_always();
    return;
  }
  REXLOG_INFO(
      "SDL OnControllerDeviceAdded: \"{}\", "
      "JoystickType({}), "
      "GameControllerType({}), "
      "VendorID(0x{:04X}), "
      "ProductID(0x{:04X}), "
      "Instance({}), "
      "Backend({}), "
      "Path(\"{}\")",
      SDL_GetGamepadName(controller),
      static_cast<int>(SDL_GetJoystickType(SDL_GetGamepadJoystick(controller))),
      static_cast<int>(SDL_GetGamepadType(controller)), SDL_GetGamepadVendor(controller),
      SDL_GetGamepadProduct(controller), static_cast<uint32_t>(event.gdevice.which),
      SdlJoystickBackendName(SDL_GetJoystickGUIDForID(event.gdevice.which).data[14]),
      PrivacySafeSdlDevicePath(SDL_GetGamepadPath(controller) ? SDL_GetGamepadPath(controller)
                                                              : ""));
  uint32_t occupied_slot_mask = 0;
  for (uint32_t slot = 0; slot < controllers_.size(); ++slot) {
    if (IsControllerConnected(controllers_.at(slot)))
      occupied_slot_mask |= uint32_t(1) << slot;
  }
  const auto selected_slot =
      ChooseControllerSlot(SDL_GetGamepadPlayerIndex(controller), occupied_slot_mask);
  const int user_id = selected_slot ? static_cast<int>(*selected_slot) : -1;
  if (user_id >= 0) {
    auto& state = controllers_.at(user_id);
    state = {};
    state.gamepad = controller;
    // XInput seems to start with packet_number = 1 .
    state.state_changed = true;
    UpdateXCapabilities(state);
    SDL_SetGamepadPlayerIndex(controller, user_id);
    internal::RecordInputSlotDeviceAssigned(static_cast<uint32_t>(user_id));
    internal::RecordControllerMatrixDeviceAssigned(static_cast<uint32_t>(user_id));

    REXLOG_INFO("SDL OnControllerDeviceAdded: Added at index {}.", user_id);
    if (!active_controller_index_) {
      SelectActiveControllerLocked(static_cast<size_t>(user_id), "first-connected");
    }
  } else {
    // No more controllers needed, close it.
    internal::RecordInputSlotDeviceRejected();
    internal::RecordControllerMatrixDeviceRejected();
    SDL_CloseGamepad(controller);
    REXLOG_WARN("SDL OnControllerDeviceAdded: Ignored. No free slots.");
  }
}

void SDLInputDriver::OnControllerDeviceRemovedLocked(const SDL_Event& event) {
  // Find the disconnected gamecontroller and close it.
  const SDL_JoystickID instance_id =
      event.type == SDL_EVENT_JOYSTICK_REMOVED ? event.jdevice.which : event.gdevice.which;
  auto idx = GetControllerIndexFromInstanceID(instance_id);
  if (idx) {
    const bool removed_active = active_controller_index_ == idx;
    internal::RecordInputSlotDeviceRemoved(static_cast<uint32_t>(*idx));
    internal::RecordControllerMatrixDeviceRemoved(static_cast<uint32_t>(*idx));
    const bool was_wheel = controllers_.at(*idx).is_wheel;
    CloseController(controllers_.at(*idx));
    keystroke_states_.at(*idx) = {};
    REXLOG_INFO("SDL {} removed from physical slot {}.", was_wheel ? "wheel" : "gamepad", *idx);
    if (removed_active) {
      active_controller_index_.reset();
      SelectFallbackControllerLocked("active-disconnected");
    }
  } else {
    // Can happen in case all slots where full previously.
    REXLOG_DEBUG("SDL device removal ignored because the instance is already unavailable.");
  }
}

void SDLInputDriver::OnControllerDeviceAxisMotionLocked(const SDL_Event& event) {
  auto idx = GetControllerIndexFromInstanceID(event.gaxis.which);
  if (!idx) {
    static std::atomic<bool> logged = false;
    if (!logged.exchange(true))
      REXLOG_WARN("SDL HID: Ignored stale axis event for an unavailable controller.");
    internal::RecordControllerMatrixStaleEvent(true);
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (controller.is_wheel) {
    // A wheel may also have an SDL gamepad mapping. Its raw joystick events
    // are authoritative; accepting both streams can remap the same physical
    // axis twice and overwrite the configured wheel state.
    return;
  }
  const X_INPUT_GAMEPAD previous_pad = controller.state.gamepad;
  auto& pad = controller.state.gamepad;
  switch (event.gaxis.axis) {
    case SDL_GAMEPAD_AXIS_LEFTX:
      pad.thumb_lx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFTY:
      pad.thumb_ly = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
      pad.thumb_rx = event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_RIGHTY:
      pad.thumb_ry = ~event.gaxis.value;
      break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
      pad.left_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
      pad.right_trigger = static_cast<uint8_t>(event.gaxis.value >> 7);
      break;
    default:
      assert_always();
      break;
  }
  controller.state_changed = true;
  const bool wheel_is_active =
      active_controller_index_ && controllers_.at(*active_controller_index_).is_wheel;
  const bool deliberate_takeover = wheel_is_active && IsDeliberateGamepadTakeoverActivity(pad);
  if (IsMeaningfulControllerAnalogActivity(previous_pad, pad) || deliberate_takeover) {
    controller.last_activity_sequence = ++controller_activity_sequence_;
    controller.last_activity_timestamp_ms = SDL_GetTicks();
    if (!wheel_is_active || deliberate_takeover) {
      SelectActiveControllerLocked(*idx, "analog");
    }
  }
  internal::RecordControllerMatrixSdlAnalog(
      static_cast<uint32_t>(*idx), ClassifyControllerMatrixAnalog(controller.state.gamepad));
}

void SDLInputDriver::UpdateWheelAxisLocked(ControllerState& controller, uint8_t axis, int16_t value,
                                           bool record_activity) {
  const X_INPUT_GAMEPAD previous_pad = controller.state.gamepad;
  auto& pad = controller.state.gamepad;
  const WheelMapping mapping = GetConfiguredWheelMapping();
  if (axis == mapping.steering_axis) {
    pad.thumb_lx = WheelSteeringValue(value, mapping.steering_inverted);
  } else if (axis == mapping.brake_axis) {
    pad.left_trigger = WheelPedalToTrigger(value, mapping.brake_inverted);
  } else if (axis == mapping.accelerator_axis) {
    pad.right_trigger = WheelPedalToTrigger(value, mapping.accelerator_inverted);
  } else {
    return;
  }
  if (std::memcmp(&previous_pad, &pad, sizeof(pad)) == 0) {
    return;
  }
  controller.state_changed = true;
  const auto idx = record_activity
                       ? GetControllerIndexFromInstanceID(SDL_GetJoystickID(controller.joystick))
                       : std::nullopt;
  const bool wheel_is_inactive = idx && active_controller_index_ != idx;
  if (record_activity &&
      (IsMeaningfulControllerAnalogActivity(previous_pad, pad) ||
       (wheel_is_inactive && IsMeaningfulInactiveWheelActivity(previous_pad, pad)))) {
    controller.last_activity_sequence = ++controller_activity_sequence_;
    controller.last_activity_timestamp_ms = SDL_GetTicks();
    if (idx) {
      SelectActiveControllerLocked(*idx, "wheel-axis");
    }
  }
}

void SDLInputDriver::OnJoystickDeviceAxisMotionLocked(const SDL_Event& event) {
  const auto idx = GetControllerIndexFromInstanceID(event.jaxis.which);
  if (!idx || !controllers_.at(*idx).is_wheel) {
    return;
  }
  auto& controller = controllers_.at(*idx);
  UpdateWheelAxisLocked(controller, event.jaxis.axis, event.jaxis.value, true);
}

void SDLInputDriver::OnJoystickDeviceButtonChangedLocked(const SDL_Event& event) {
  const auto idx = GetControllerIndexFromInstanceID(event.jbutton.which);
  if (!idx || !controllers_.at(*idx).is_wheel) {
    return;
  }
  const uint16_t xbutton = WheelButtonToXInput(event.jbutton.button, GetConfiguredWheelMapping());
  if (!xbutton) {
    return;
  }

  auto& controller = controllers_.at(*idx);
  const uint16_t previous = controller.state.gamepad.buttons;
  uint16_t buttons = previous;
  if (event.jbutton.down) {
    buttons |= xbutton;
  } else {
    buttons &= ~xbutton;
  }
  if (buttons == previous) {
    return;
  }
  controller.state.gamepad.buttons = buttons;
  controller.state_changed = true;
  if (event.jbutton.down) {
    controller.last_activity_sequence = ++controller_activity_sequence_;
    controller.last_activity_timestamp_ms = SDL_GetTicks();
    SelectActiveControllerLocked(*idx, "wheel-button-down");
  }
}

void SDLInputDriver::OnJoystickDeviceHatMotionLocked(const SDL_Event& event) {
  const auto idx = GetControllerIndexFromInstanceID(event.jhat.which);
  if (!idx || !controllers_.at(*idx).is_wheel || event.jhat.hat != 0) {
    return;
  }

  auto& controller = controllers_.at(*idx);
  uint16_t buttons = controller.state.gamepad.buttons;
  constexpr uint16_t kDpadMask = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN |
                                 X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_RIGHT;
  const uint16_t previous = buttons;
  buttons = static_cast<uint16_t>((buttons & ~kDpadMask) | WheelHatToXInput(event.jhat.value));
  if (buttons == previous) {
    return;
  }
  controller.state.gamepad.buttons = buttons;
  controller.state_changed = true;
  if ((buttons & kDpadMask) != 0) {
    controller.last_activity_sequence = ++controller_activity_sequence_;
    controller.last_activity_timestamp_ms = SDL_GetTicks();
    SelectActiveControllerLocked(*idx, "wheel-hat");
  }
}

void SDLInputDriver::OnControllerDeviceButtonChangedLocked(const SDL_Event& event) {
  // Define a lookup table to map between SDL and XInput button codes.
  // These need to be in the order of the SDL_GamepadButton enum.
  static constexpr std::array<std::underlying_type<X_INPUT_GAMEPAD_BUTTON>::type, 21>
      xbutton_lookup = {
          // Standard buttons:
          X_INPUT_GAMEPAD_A,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_BACK,
          X_INPUT_GAMEPAD_GUIDE,
          X_INPUT_GAMEPAD_START,
          X_INPUT_GAMEPAD_LEFT_THUMB,
          X_INPUT_GAMEPAD_RIGHT_THUMB,
          X_INPUT_GAMEPAD_LEFT_SHOULDER,
          X_INPUT_GAMEPAD_RIGHT_SHOULDER,
          X_INPUT_GAMEPAD_DPAD_UP,
          X_INPUT_GAMEPAD_DPAD_DOWN,
          X_INPUT_GAMEPAD_DPAD_LEFT,
          X_INPUT_GAMEPAD_DPAD_RIGHT,
          // There are additional buttons only available on some controllers.
          // For now just assign sensible defaults
          // Misc:
          X_INPUT_GAMEPAD_GUIDE,
          // Xbox Elite paddles:
          X_INPUT_GAMEPAD_Y,
          X_INPUT_GAMEPAD_B,
          X_INPUT_GAMEPAD_X,
          X_INPUT_GAMEPAD_A,
          // PS touchpad button
          X_INPUT_GAMEPAD_GUIDE,
      };
  static_assert(SDL_GAMEPAD_BUTTON_SOUTH == 0);
  static_assert(SDL_GAMEPAD_BUTTON_DPAD_RIGHT == 14);

  auto idx = GetControllerIndexFromInstanceID(event.gbutton.which);
  if (!idx) {
    static std::atomic<bool> logged = false;
    if (!logged.exchange(true))
      REXLOG_WARN("SDL HID: Ignored stale button event for an unavailable controller.");
    internal::RecordControllerMatrixStaleEvent(false);
    return;
  }
  auto& controller = controllers_.at(*idx);
  if (controller.is_wheel) {
    // Raw wheel button/hat events own this device for the same reason as axes.
    return;
  }

  uint16_t xbuttons = controller.state.gamepad.buttons;
  // Lookup the XInput button code.
  if (event.gbutton.button >= xbutton_lookup.size()) {
    // A newer SDL Version may have added new buttons.
    REXLOG_INFO("SDL HID: Unknown button was pressed: {}.", event.gbutton.button);
    return;
  }
  auto xbutton = xbutton_lookup.at(event.gbutton.button);
  const bool was_pressed = (xbuttons & xbutton) != 0;
  // Pressed or released?
  if (event.gbutton.down) {
    if (xbutton == X_INPUT_GAMEPAD_GUIDE && !REXCVAR_GET(guide_button)) {
      return;
    }
    xbuttons |= xbutton;
  } else {
    xbuttons &= ~xbutton;
  }
  controller.state.gamepad.buttons = xbuttons;
  controller.state_changed = true;
  const bool is_pressed = (xbuttons & xbutton) != 0;
  if (!was_pressed && is_pressed) {
    controller.last_activity_sequence = ++controller_activity_sequence_;
    controller.last_activity_timestamp_ms = SDL_GetTicks();
    SelectActiveControllerLocked(*idx, "button-down");
  }
  if (IsInputSlotAuditChallengeButton(event.gbutton.button) && was_pressed != is_pressed) {
    internal::RecordInputSlotSdlAEdge(static_cast<uint32_t>(*idx), is_pressed);
  }
  const uint16_t standard_button = StandardSdlButtonToXInput(event.gbutton.button);
  if (standard_button && was_pressed != is_pressed) {
    internal::RecordControllerMatrixSdlButton(static_cast<uint32_t>(*idx), standard_button,
                                              is_pressed);
  }
}

std::optional<size_t> SDLInputDriver::GetControllerIndexFromInstanceID(SDL_JoystickID instance_id) {
  // Loop through our controllers and try to match the given ID.
  for (size_t i = 0; i < controllers_.size(); i++) {
    SDL_Joystick* joystick = GetControllerJoystick(controllers_.at(i));
    if (!joystick) {
      continue;
    }
    auto joy_instance_id = SDL_GetJoystickID(joystick);
    if (joy_instance_id == instance_id) {
      return i;
    }
  }
  return std::nullopt;
}

bool SDLInputDriver::IsControllerConnected(const ControllerState& state) const {
  return state.gamepad || state.joystick;
}

SDL_Joystick* SDLInputDriver::GetControllerJoystick(const ControllerState& state) const {
  if (state.gamepad) {
    return SDL_GetGamepadJoystick(state.gamepad);
  }
  return state.joystick;
}

void SDLInputDriver::StopControllerFeedback(ControllerState& state, bool preserve_running) {
  if (state.gamepad) {
    SDL_RumbleGamepad(state.gamepad, 0, 0, 0);
  }
  if (state.haptic) {
    std::array<bool, kWheelForceFeedbackEffectSlots> preserve_effect = {};
    const bool status_supported = (SDL_GetHapticFeatures(state.haptic) & SDL_HAPTIC_STATUS) != 0;
    if (preserve_running) {
      for (size_t effect_slot = 0; effect_slot < state.force_feedback_effects.size();
           ++effect_slot) {
        const auto& effect = state.force_feedback_effects[effect_slot];
        const bool status_playing = effect.running && status_supported && effect.effect_id >= 0 &&
                                    SDL_GetHapticEffectStatus(state.haptic, effect.effect_id);
        preserve_effect[effect_slot] =
            ShouldPreserveWheelForceFeedback(effect.duration_ms, effect.resume_pending,
                                             effect.running, status_supported, status_playing);
      }
    }
    if (SDL_HapticRumbleSupported(state.haptic)) {
      SDL_StopHapticRumble(state.haptic);
    }
    SDL_StopHapticEffects(state.haptic);
    for (size_t effect_slot = 0; effect_slot < state.force_feedback_effects.size(); ++effect_slot) {
      auto& effect = state.force_feedback_effects[effect_slot];
      if (preserve_running) {
        effect.resume_pending = preserve_effect[effect_slot];
      } else {
        effect.resume_pending = false;
      }
      effect.running = false;
    }
  }
}

void SDLInputDriver::ResumeControllerFeedback(ControllerState& state, const char* reason) {
  if (!state.haptic || !has_focus_.load()) {
    return;
  }
  for (size_t effect_slot = 0; effect_slot < state.force_feedback_effects.size(); ++effect_slot) {
    auto& effect = state.force_feedback_effects[effect_slot];
    if (!effect.resume_pending || effect.effect_id < 0) {
      continue;
    }
    if (!SDL_RunHapticEffect(state.haptic, effect.effect_id, effect.iterations)) {
      REXLOG_ERROR("SDL wheel FFB {} resume failed for slot {}: {}", reason, effect_slot + 1,
                   SDL_GetError());
      continue;
    }
    effect.running = true;
    effect.resume_pending = false;
    effect.start_suppressed_logged = false;
    state.force_feedback_total_start_count++;
    REXLOG_INFO("SDL_WHEEL_FFB_EFFECT v=2 action=resume slot={} type={} reason={}", effect_slot + 1,
                GetWheelEffectTypeName(effect.type), reason);
  }
}

void SDLInputDriver::RearmStoppedWheelEffectsLocked(ControllerState& state, const char* reason,
                                                    std::optional<size_t> only_slot) {
  if (!state.is_wheel || !state.haptic || !has_focus_.load() ||
      !REXCVAR_GET(wheel_force_feedback)) {
    return;
  }
  const auto controller_index = static_cast<size_t>(&state - controllers_.data());
  if (active_controller_index_ != controller_index) {
    return;
  }
  if ((SDL_GetHapticFeatures(state.haptic) & SDL_HAPTIC_STATUS) == 0) {
    // Without status the host cannot tell a stopped effect from a playing one;
    // re-running blindly would re-articulate centering, so do nothing.
    return;
  }
  const uint64_t now_ms = SDL_GetTicks();
  auto status_playing = [&](SDL_HapticEffectID effect_id) {
    return SDL_GetHapticEffectStatus(state.haptic, effect_id);
  };
  auto run = [&](SDL_HapticEffectID effect_id, uint32_t iterations) {
    return SDL_RunHapticEffect(state.haptic, effect_id, iterations);
  };
  auto on_event = [&](WheelRearmEvent event, size_t slot_number, ForceFeedbackEffectType type) {
    switch (event) {
      case WheelRearmEvent::kRearmed:
        state.force_feedback_total_start_count++;
        REXLOG_INFO("{}", FormatWheelRearmLine(slot_number, type, reason));
        break;
      case WheelRearmEvent::kRunFailed:
        REXLOG_ERROR("SDL wheel FFB {} rearm failed for slot {} type {}: {}", reason, slot_number,
                     GetWheelEffectTypeName(type), SDL_GetError());
        break;
      case WheelRearmEvent::kLimitReached:
        REXLOG_WARN("{}", FormatWheelRearmLimitLine(slot_number, type, reason));
        break;
    }
  };
  if (only_slot) {
    if (*only_slot < state.force_feedback_effects.size()) {
      RearmStoppedWheelEffect(state.force_feedback_effects[*only_slot], *only_slot + 1, now_ms,
                              status_playing, run, on_event);
    }
    return;
  }
  RearmStoppedWheelEffects(state.force_feedback_effects, now_ms, status_playing, run, on_event);
}

void SDLInputDriver::ResyncWheelEffectsLocked(ControllerState& state, WheelResyncTrigger trigger,
                                              const std::string& reason) {
  if (!state.is_wheel || !state.haptic || !has_focus_.load() ||
      !REXCVAR_GET(wheel_force_feedback)) {
    return;
  }
  const auto controller_index = static_cast<size_t>(&state - controllers_.data());
  if (active_controller_index_ != controller_index) {
    return;
  }
  const uint64_t now_ms = SDL_GetTicks();
  // Same device sequence as a focus loss and regain (stop, then run), which
  // restores an effect the driver dropped while still reporting it playing.
  auto stop = [&](SDL_HapticEffectID effect_id) {
    return SDL_StopHapticEffect(state.haptic, effect_id);
  };
  auto run = [&](SDL_HapticEffectID effect_id, uint32_t iterations) {
    return SDL_RunHapticEffect(state.haptic, effect_id, iterations);
  };
  auto on_event = [&](WheelResyncEvent event, size_t slot_number, ForceFeedbackEffectType type) {
    switch (event) {
      case WheelResyncEvent::kResynced:
        state.force_feedback_total_start_count++;
        state.force_feedback_effects[slot_number - 1].start_suppressed_logged = false;
        REXLOG_INFO("{}", FormatWheelResyncLine(slot_number, type, reason));
        break;
      case WheelResyncEvent::kRunFailed:
        REXLOG_ERROR("SDL wheel FFB {} resync failed for slot {} type {}: {}", reason, slot_number,
                     GetWheelEffectTypeName(type), SDL_GetError());
        break;
      case WheelResyncEvent::kLimitReached:
        REXLOG_WARN("{}", FormatWheelResyncLimitLine(slot_number, type, reason));
        break;
    }
  };
  ResyncWheelEffects(state.force_feedback_effects, now_ms, trigger, stop, run, on_event);
}

void SDLInputDriver::ServiceWheelEffectResyncLocked(ControllerState& state, uint64_t now_ms) {
  const auto controller_index = static_cast<size_t>(&state - controllers_.data());
  if (active_controller_index_ != controller_index || !state.is_wheel || !state.haptic ||
      !has_focus_.load() || !REXCVAR_GET(wheel_force_feedback)) {
    return;
  }
  std::string host_reason;
  const bool host_request = TakeWheelResyncRequest(now_ms, host_reason);
  const bool burst_end = state.force_feedback_burst.ConsumeBurstEnd(now_ms);
  if (!AnyWheelEffectResyncCandidate(state.force_feedback_effects)) {
    // Nothing believed running: keep the periodic timer armed from now.
    state.force_feedback_resync_ms = now_ms;
    return;
  }
  if (host_request) {
    // A burst end at the same poll is coalesced into the host pass.
    ResyncWheelEffectsLocked(state, WheelResyncTrigger::kHost, "host:" + host_reason);
  } else if (burst_end) {
    ResyncWheelEffectsLocked(state, WheelResyncTrigger::kBurst, "burst");
  } else if (ShouldRunWheelPeriodicResync(now_ms, state.force_feedback_resync_ms)) {
    ResyncWheelEffectsLocked(state, WheelResyncTrigger::kPeriodic, "periodic");
  } else {
    return;
  }
  state.force_feedback_resync_ms = now_ms;
}

void SDLInputDriver::CloseController(ControllerState& state) {
  StopControllerFeedback(state);
  if (state.haptic) {
    uint32_t created_effects = 0;
    for (auto& effect : state.force_feedback_effects) {
      if (effect.effect_id >= 0) {
        SDL_DestroyHapticEffect(state.haptic, effect.effect_id);
        effect.effect_id = -1;
        created_effects++;
      }
    }
    if (created_effects || state.force_feedback_total_start_count ||
        state.force_feedback_total_update_count) {
      REXLOG_INFO(
          "SDL_WHEEL_FFB_SUMMARY v=2 created_effects={} starts={} updates={} gain_percent={}",
          created_effects, state.force_feedback_total_start_count,
          state.force_feedback_total_update_count, REXCVAR_GET(wheel_force_feedback_gain));
    }
    SDL_CloseHaptic(state.haptic);
  }
  if (state.gamepad) {
    SDL_CloseGamepad(state.gamepad);
  } else if (state.joystick) {
    SDL_CloseJoystick(state.joystick);
  }
  state = {};
}

SDLInputDriver::ControllerState* SDLInputDriver::GetControllerState(uint32_t user_index) {
  const auto controller_index = ResolveControllerIndex(user_index);
  if (!controller_index) {
    return nullptr;
  }
  auto controller = &controllers_.at(*controller_index);
  if (!IsControllerConnected(*controller)) {
    return nullptr;
  }
  return controller;
}

std::optional<size_t> SDLInputDriver::ResolveControllerIndex(uint32_t user_index) const {
  const auto active =
      active_controller_index_
          ? std::optional<uint32_t>(static_cast<uint32_t>(*active_controller_index_))
          : std::nullopt;
  const auto resolved =
      ResolveLogicalControllerSlot(user_index, active, GetConnectedControllerMask());
  return resolved ? std::optional<size_t>(static_cast<size_t>(*resolved)) : std::nullopt;
}

uint32_t SDLInputDriver::GetConnectedControllerMask() const {
  uint32_t mask = 0;
  for (uint32_t slot = 0; slot < controllers_.size(); ++slot) {
    if (IsControllerConnected(controllers_.at(slot))) {
      mask |= uint32_t(1) << slot;
    }
  }
  return mask;
}

void SDLInputDriver::SelectActiveControllerLocked(size_t controller_index, const char* reason) {
  if (controller_index >= controllers_.size() ||
      !IsControllerConnected(controllers_.at(controller_index)) ||
      active_controller_index_ == controller_index) {
    return;
  }

  if (ShouldSuppressMirroredGamepadSelectionLocked(controller_index)) {
    return;
  }

  const int previous_index =
      active_controller_index_ ? static_cast<int>(*active_controller_index_) : -1;
  if (active_controller_index_) {
    auto& previous = controllers_.at(*active_controller_index_);
    StopControllerFeedback(previous, true);
  }
  active_controller_index_ = controller_index;
  ResumeControllerFeedback(controllers_.at(controller_index), "active-controller");
  for (auto& keystroke_state : keystroke_states_) {
    keystroke_state = {};
  }
  REXLOG_INFO(
      "SDL_ACTIVE_CONTROLLER v=1 guest_slot=0 previous_physical_slot={} "
      "physical_slot={} reason={} activity_sequence={}",
      previous_index, controller_index, reason,
      controllers_.at(controller_index).last_activity_sequence);
}

bool SDLInputDriver::ShouldSuppressMirroredGamepadSelectionLocked(size_t controller_index) const {
  if (!active_controller_index_) {
    return false;
  }
  const auto& active = controllers_.at(*active_controller_index_);
  const auto& candidate = controllers_.at(controller_index);
  if (active.is_wheel || candidate.is_wheel || !active.gamepad || !candidate.gamepad) {
    return false;
  }
  return IsMirroredGamepadActivity(active.state.gamepad, candidate.state.gamepad,
                                   active.last_activity_timestamp_ms,
                                   candidate.last_activity_timestamp_ms);
}

void SDLInputDriver::SelectFallbackControllerLocked(const char* reason) {
  std::array<uint64_t, kControllerSlotCount> activity_sequences = {};
  for (size_t slot = 0; slot < controllers_.size(); ++slot) {
    activity_sequences[slot] = controllers_.at(slot).last_activity_sequence;
  }
  const auto fallback =
      ChooseMostRecentlyActiveController(GetConnectedControllerMask(), activity_sequences);
  if (fallback) {
    SelectActiveControllerLocked(*fallback, reason);
  } else {
    slot_zero_state_source_.reset();
    for (auto& keystroke_state : keystroke_states_) {
      keystroke_state = {};
    }
    REXLOG_INFO("SDL_ACTIVE_CONTROLLER v=1 guest_slot=0 physical_slot=none reason={}", reason);
  }
}

bool SDLInputDriver::TestSDLVersion() const {
  REXLOG_INFO("SDL: Using version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION,
              SDL_MICRO_VERSION);
  return true;
}

void SDLInputDriver::UpdateXCapabilities(ControllerState& state) {
  assert(IsControllerConnected(state));
  uint16_t cap_flags = 0x0;

  // The RAWINPUT driver combines and enhances input from different APIs. For
  // details, see `SDL_rawinputjoystick.c`. This correlation however has latency
  // which might confuse games calling `GetCapabilities()` (The power level is
  // only available after the controller has been "touched"). Generally that
  // should not be a problem, when in doubt disable the RAWINPUT driver via hint
  // (env var).

  SDL_Joystick* joystick = GetControllerJoystick(state);
  if (SDL_GetJoystickConnectionState(joystick) == SDL_JOYSTICK_CONNECTION_WIRELESS) {
    cap_flags |= X_INPUT_CAPS_WIRELESS;
  }

  if (state.is_wheel && state.haptic && REXCVAR_GET(wheel_force_feedback) &&
      (SDL_GetHapticFeatures(state.haptic) & kMclaWheelHapticFeatures) != 0) {
    cap_flags |= X_INPUT_CAPS_FFB_SUPPORTED;
  }

  if (state.is_wheel) {
    const WheelMapping mapping = GetConfiguredWheelMapping();
    const bool has_dpad_hat = SDL_GetNumJoystickHats(joystick) >= 1;
    const bool has_mapped_dpad = mapping.dpad_up_button >= 0 && mapping.dpad_down_button >= 0 &&
                                 mapping.dpad_left_button >= 0 && mapping.dpad_right_button >= 0;
    if ((!has_dpad_hat && !has_mapped_dpad) || mapping.start_button < 0 ||
        mapping.back_button < 0) {
      cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
    }
  } else {
    // Check if all navigational buttons are present.
    static constexpr std::array<SDL_GamepadButton, 6> nav_buttons = {
        SDL_GAMEPAD_BUTTON_START,     SDL_GAMEPAD_BUTTON_BACK,      SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    for (auto it = nav_buttons.begin(); it < nav_buttons.end(); it++) {
      if (!SDL_GamepadHasButton(state.gamepad, *it)) {
        cap_flags |= X_INPUT_CAPS_NO_NAVIGATION;
        break;
      }
    }
  }

  auto& c = state.caps;
  c.type = 0x01;                              // XINPUT_DEVTYPE_GAMEPAD
  c.sub_type = state.is_wheel ? 0x02 : 0x01;  // WHEEL or GAMEPAD
  c.flags = cap_flags;
  c.gamepad.buttons =
      state.is_wheel
          ? 0xF3FF | (GetConfiguredWheelMapping().guide_button >= 0 ? X_INPUT_GAMEPAD_GUIDE : 0x0)
          : 0xF3FF | (REXCVAR_GET(guide_button) ? X_INPUT_GAMEPAD_GUIDE : 0x0);
  c.gamepad.left_trigger = 0xFF;
  c.gamepad.right_trigger = 0xFF;
  c.gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ly = state.is_wheel ? 0 : static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_rx = state.is_wheel ? 0 : static_cast<int16_t>(0xFFFFu);
  c.gamepad.thumb_ry = state.is_wheel ? 0 : static_cast<int16_t>(0xFFFFu);
  const uint16_t vibration_caps = state.is_wheel ? 0 : 0xFFFFu;
  c.vibration.left_motor_speed = vibration_caps;
  c.vibration.right_motor_speed = vibration_caps;
}

void SDLInputDriver::QueueControllerUpdate() {
  // Pump SDL events to ensure controller state is up to date.
  bool is_queued = false;
  sdl_pumpevents_queued_.compare_exchange_strong(is_queued, true);
  if (!is_queued) {
    attached_window_->app_context().CallInUIThread([this]() {
      SDL_PumpEvents();
      sdl_pumpevents_queued_ = false;
    });
  }
}

// Check if the analog inputs exceed their thresholds to become a button press
// and build the bitfield.
inline uint64_t SDLInputDriver::AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > HID_SDL_TRIGG_THRES) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > HID_SDL_TRIGG_THRES) << 17;

  auto thumb_x = static_cast<int16_t>(gamepad.thumb_lx);
  auto thumb_y = static_cast<int16_t>(gamepad.thumb_ly);
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > HID_SDL_THUMB_THRES;
    uint64_t d = thumb_y < ~HID_SDL_THUMB_THRES;
    uint64_t r = thumb_x > HID_SDL_THUMB_THRES;
    uint64_t l = thumb_x < ~HID_SDL_THUMB_THRES;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = static_cast<int16_t>(gamepad.thumb_rx);
    thumb_y = static_cast<int16_t>(gamepad.thumb_ry);
  }
  return f;
}

}  // namespace rex::input::sdl
