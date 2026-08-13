/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/sdl/controller_matrix.h>
#include <rex/input/sdl/controller_matrix_audit.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(sdl_controller_matrix_audit, false, "Input",
                    "Emit bounded privacy-safe full controller audit markers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sdl_controller_matrix_resume, false, "Input",
                    "Resume controller audit using completed prior digital and rumble evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(
    sdl_controller_matrix_hotplug_resume, false, "Input",
    "Resume controller audit using completed prior input, rumble, and focus evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::input::sdl {
namespace {

class ControllerMatrixAudit {
 public:
  static ControllerMatrixAudit& Get() {
    static ControllerMatrixAudit audit;
    return audit;
  }

  void DeviceAssigned(uint32_t slot) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (summary_)
      return;
    if (slot >= 4 || (active_mask_ & (1u << slot))) {
      ++unexpected_;
      Failure("device_assignment");
      return;
    }
    const bool reconnect = armed_ && removed_ && !reconnected_ && slot == 0;
    if ((assigned_mask_ & (1u << slot)) && !reconnect) {
      ++unexpected_;
      Failure("device_reassignment");
      return;
    }
    if (armed_ && slot != 0) {
      ++unexpected_;
      Failure("extra_device");
      return;
    }
    active_mask_ |= 1u << slot;
    assigned_mask_ |= 1u << slot;
    REXLOG_INFO("SDL_CONTROLLER_MATRIX_AUDIT_DEVICE v=1 event={} slot={} active_mask={:X}",
                reconnect ? "reconnected" : "assigned", slot, active_mask_);
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_MULTIPAD v=1 assigned_mask={:X} active_mask={:X} "
        "rejected={}",
        assigned_mask_, active_mask_, rejected_);
    if (reconnect) {
      reconnected_ = true;
      reconnect_seq_ = ++sequence_;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_HOTPLUG v=1 event=reconnected slot=0 "
          "active_mask=1 seq={}",
          reconnect_seq_);
    }
  }

  void DeviceRemoved(uint32_t slot) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (summary_)
      return;
    if (slot >= 4 || !(active_mask_ & (1u << slot))) {
      ++unexpected_;
      Failure("device_removal");
      return;
    }
    active_mask_ &= ~(1u << slot);
    REXLOG_INFO("SDL_CONTROLLER_MATRIX_AUDIT_DEVICE v=1 event=removed slot={} active_mask={:X}",
                slot, active_mask_);
    if (armed_ && slot == 0 && !removed_) {
      removed_ = true;
      remove_seq_ = ++sequence_;
      REXLOG_INFO("SDL_CONTROLLER_MATRIX_AUDIT_HOTPLUG v=1 event=removed slot=0 seq={}",
                  remove_seq_);
    }
  }

  void DeviceRejected() {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (summary_)
      return;
    ++rejected_;
    ++unexpected_;
    REXLOG_ERROR(
        "SDL_CONTROLLER_MATRIX_AUDIT_MULTIPAD v=1 assigned_mask={:X} active_mask={:X} "
        "rejected={}",
        assigned_mask_, active_mask_, rejected_);
    Failure("device_rejected");
  }

  void StaleEvent(bool axis) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (summary_)
      return;
    ++unexpected_;
    Failure(axis ? "stale_axis" : "stale_button");
  }

  void Arm(const char* phase) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (arm_attempted_)
      return;
    arm_attempted_ = true;
    phase_valid_ = phase && std::strcmp(phase, "title") == 0;
    const bool neutral = current_buttons_ == 0 && current_analog_ == 0;
    const bool resume = REXCVAR_GET(sdl_controller_matrix_resume);
    const bool hotplug_resume = REXCVAR_GET(sdl_controller_matrix_hotplug_resume);
    const bool resume_modes_valid = AreControllerMatrixResumeModesValid(resume, hotplug_resume);
    armed_ = phase_valid_ && active_mask_ == 1 && neutral && resume_modes_valid && failures_ == 0;
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_ARM v=1 phase={} status={} active_mask={:X} neutral={}",
        phase_valid_ ? "title" : "invalid", armed_ ? "READY" : "FAIL", active_mask_,
        neutral ? 1 : 0);
    if (!armed_) {
      Failure(resume_modes_valid ? "arm_contract" : "resume_mode_conflict");
      return;
    }
    if (resume) {
      const auto seed = GetControllerMatrixResumeSeed(true);
      button_sdl_mask_ = seed.button_sdl_mask;
      button_guest_mask_ = seed.button_guest_mask;
      rumble_pattern_mask_ = seed.rumble_pattern_mask;
      rumble_stop_mask_ = seed.rumble_stop_mask;
      source_sequence_ = seed.source_sequence;
      input_ready_ = seed.input_ready;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_RESUME v=1 phase=title status=READY prior_digital=1 "
          "prior_rumble=1 button_sdl_mask=F3FF button_guest_mask=F3FF "
          "rumble_pattern_mask=7 rumble_stop_mask=7 source_seq=14");
    } else if (hotplug_resume) {
      const auto seed = GetControllerMatrixHotplugResumeSeed(true);
      button_sdl_mask_ = seed.completed.button_sdl_mask;
      button_guest_mask_ = seed.completed.button_guest_mask;
      rumble_pattern_mask_ = seed.completed.rumble_pattern_mask;
      rumble_stop_mask_ = seed.completed.rumble_stop_mask;
      source_sequence_ = seed.source_sequence;
      input_ready_ = seed.completed.input_ready;
      analog_sdl_mask_ = seed.analog_sdl_mask;
      analog_guest_mask_ = seed.analog_guest_mask;
      focus_lost_ = seed.focus_lost;
      focus_neutral_ = seed.focus_neutral;
      focus_gained_ = seed.focus_gained;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_HOTPLUG_RESUME v=1 phase=title status=READY "
          "prior_digital=1 prior_rumble=1 prior_analog=1 prior_focus=1 "
          "button_sdl_mask=F3FF button_guest_mask=F3FF analog_sdl_mask=3FF "
          "analog_guest_mask=3FF rumble_pattern_mask=7 rumble_stop_mask=7 focus_lost=1 "
          "focus_neutral=1 focus_gained=1 source_seq=24");
    }
  }

  void SdlButton(uint32_t slot, uint16_t button, bool down) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    if (summary_)
      return;
    if (REXCVAR_GET(sdl_controller_matrix_hotplug_resume))
      return;
    if (down)
      current_buttons_ |= button;
    else
      current_buttons_ &= ~button;
    if (!input_ready_) {
      MaybeInputReady();
      return;
    }
    if (!down) {
      if (button_guest_down_ == button && pending_button_ == 0 && pending_button_release_ == 0 &&
          current_buttons_ == 0) {
        pending_button_release_ = button;
        REXLOG_INFO(
            "SDL_CONTROLLER_MATRIX_AUDIT_BUTTON v=1 layer=sdl event=up slot=0 bit={:04X} "
            "seen_mask={:04X} source_seq={}",
            button, button_sdl_mask_, active_button_seq_);
      }
      return;
    }
    if (!armed_ || slot != 0 || !(button & kControllerMatrixButtonMask))
      return;
    const uint16_t fresh = button & kControllerMatrixButtonMask & ~button_sdl_mask_;
    if (!fresh)
      return;
    if (!button_source_ready_ || pending_button_ || !IsSingleControllerMatrixBit(fresh)) {
      ++unexpected_;
      Failure("button_source_overlap");
      return;
    }
    pending_button_ = fresh;
    button_source_ready_ = false;
    pending_button_seq_ = ++source_sequence_;
    active_button_seq_ = pending_button_seq_;
    button_sdl_mask_ |= fresh;
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_BUTTON v=1 layer=sdl event=down slot=0 bit={:04X} "
        "seen_mask={:04X} source_seq={}",
        fresh, button_sdl_mask_, pending_button_seq_);
    MaybeSummary();
  }

  void SdlAnalog(uint32_t slot, uint16_t mask) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    if (summary_)
      return;
    if (REXCVAR_GET(sdl_controller_matrix_hotplug_resume))
      return;
    current_analog_ = mask;
    if (!input_ready_) {
      MaybeInputReady();
      return;
    }
    if (!armed_ || slot != 0)
      return;
    if (mask == 0) {
      if (analog_guest_extreme_ && pending_analog_ == 0 && pending_analog_neutral_ == 0) {
        pending_analog_neutral_ = analog_guest_extreme_;
        REXLOG_INFO(
            "SDL_CONTROLLER_MATRIX_AUDIT_ANALOG v=1 layer=sdl event=neutral slot=0 bit={:03X} "
            "seen_mask={:03X} source_seq={}",
            analog_guest_extreme_, analog_sdl_mask_, active_analog_seq_);
      }
      return;
    }
    const uint16_t fresh = mask & kControllerMatrixAnalogMask & ~analog_sdl_mask_;
    if (!fresh)
      return;
    if (!analog_source_ready_ || pending_analog_ || !IsSingleControllerMatrixBit(fresh)) {
      ++unexpected_;
      Failure("analog_source_overlap");
      return;
    }
    pending_analog_ = fresh;
    analog_source_ready_ = false;
    pending_analog_seq_ = ++source_sequence_;
    active_analog_seq_ = pending_analog_seq_;
    analog_sdl_mask_ |= fresh;
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_ANALOG v=1 layer=sdl event=extreme slot=0 bit={:03X} "
        "seen_mask={:03X} source_seq={}",
        fresh, analog_sdl_mask_, pending_analog_seq_);
    MaybeSummary();
  }

  void Focus(bool focused) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    if (summary_)
      return;
    if (!armed_)
      return;
    if (ShouldIgnoreControllerMatrixFocusChurn(focus_lost_, focus_neutral_, focus_gained_))
      return;
    const bool pending_input = pending_button_ || pending_analog_ || pending_button_release_ ||
                               pending_analog_neutral_ || button_guest_down_ ||
                               analog_guest_extreme_;
    if (!IsControllerMatrixSurfaceComplete(button_sdl_mask_, button_guest_mask_, analog_sdl_mask_,
                                           analog_guest_mask_, pending_input)) {
      // Console focus used for operator prompts is not the focus-loss challenge.
      return;
    }
    if (!focused && !focus_lost_) {
      if (current_buttons_ == 0 && current_analog_ == 0) {
        ++unexpected_;
        Failure("focus_not_active");
        return;
      }
      focus_lost_ = true;
      focus_seq_ = ++sequence_;
      packet_before_focus_ = last_packet_;
      REXLOG_INFO("SDL_CONTROLLER_MATRIX_AUDIT_FOCUS v=1 event=lost seq={}", focus_seq_);
    } else if (focused && focus_lost_ && focus_neutral_ && !focus_gained_) {
      focus_gained_ = true;
      REXLOG_INFO("SDL_CONTROLLER_MATRIX_AUDIT_FOCUS v=1 event=gained seq={}", ++sequence_);
      MaybeSummary();
    } else {
      ++unexpected_;
      Failure("focus_order");
    }
  }

  void GuestState(uint32_t requested, uint32_t actual, uint32_t result,
                  const X_INPUT_STATE* state) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    if (summary_)
      return;
    if (!armed_ || requested != 0 || actual != 0)
      return;
    if (removed_ && !reconnected_ && result == X_ERROR_DEVICE_NOT_CONNECTED && !disconnected_) {
      disconnected_ = true;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_HOTPLUG v=1 event=disconnected slot=0 "
          "result=0000048F seq={}",
          remove_seq_);
      return;
    }
    if (reconnected_ && result == X_ERROR_SUCCESS && state && !reconnect_success_) {
      reconnect_success_ = true;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_HOTPLUG v=1 event=success slot=0 result=00000000 "
          "seq={}",
          reconnect_seq_);
      MaybeSummary();
    }
    if (result != X_ERROR_SUCCESS || !state)
      return;

    if (!input_ready_) {
      last_packet_ = state->packet_number;
      return;
    }

    const uint16_t buttons =
        static_cast<uint16_t>(state->gamepad.buttons) & kControllerMatrixButtonMask;
    const uint16_t fresh_buttons = buttons & ~button_guest_mask_;
    if (fresh_buttons) {
      if (!IsCausalControllerMatrixPair(pending_button_, fresh_buttons)) {
        ++unexpected_;
        Failure("button_guest_mismatch");
        return;
      }
      button_guest_mask_ |= fresh_buttons;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_BUTTON v=1 layer=guest event=down slot=0 bit={:04X} "
          "seen_mask={:04X} source_seq={}",
          fresh_buttons, button_guest_mask_, pending_button_seq_);
      pending_button_ = 0;
      pending_button_seq_ = 0;
      button_guest_down_ = fresh_buttons;
    }
    if (pending_button_release_ && !(buttons & pending_button_release_)) {
      if (pending_button_release_ != button_guest_down_) {
        ++unexpected_;
        Failure("button_guest_release_mismatch");
        return;
      }
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_BUTTON v=1 layer=guest event=up slot=0 bit={:04X} "
          "seen_mask={:04X} source_seq={}",
          pending_button_release_, button_guest_mask_, active_button_seq_);
      pending_button_release_ = 0;
      button_guest_down_ = 0;
      active_button_seq_ = 0;
      button_source_ready_ = true;
      MaybeSummary();
    }
    const uint16_t analog = ClassifyControllerMatrixAnalog(state->gamepad);
    const uint16_t fresh_analog = analog & ~analog_guest_mask_;
    if (fresh_analog) {
      if (!IsCausalControllerMatrixPair(pending_analog_, fresh_analog)) {
        ++unexpected_;
        Failure("analog_guest_mismatch");
        return;
      }
      analog_guest_mask_ |= fresh_analog;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_ANALOG v=1 layer=guest event=extreme slot=0 bit={:03X} "
          "seen_mask={:03X} source_seq={}",
          fresh_analog, analog_guest_mask_, pending_analog_seq_);
      pending_analog_ = 0;
      pending_analog_seq_ = 0;
      analog_guest_extreme_ = fresh_analog;
    }
    if (pending_analog_neutral_ && analog == 0) {
      if (pending_analog_neutral_ != analog_guest_extreme_) {
        ++unexpected_;
        Failure("analog_guest_neutral_mismatch");
        return;
      }
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_ANALOG v=1 layer=guest event=neutral slot=0 bit={:03X} "
          "seen_mask={:03X} source_seq={}",
          pending_analog_neutral_, analog_guest_mask_, active_analog_seq_);
      pending_analog_neutral_ = 0;
      analog_guest_extreme_ = 0;
      active_analog_seq_ = 0;
      analog_source_ready_ = true;
      MaybeSummary();
    }
    const uint32_t packet = state->packet_number;
    if (focus_lost_ && !focus_neutral_ && buttons == 0 && analog == 0) {
      focus_neutral_ = packet != packet_before_focus_;
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_FOCUS v=1 event=neutral slot=0 result=00000000 "
          "packet_changed={} seq={}",
          focus_neutral_ ? 1 : 0, focus_seq_);
      if (!focus_neutral_)
        Failure("focus_packet");
    }
    last_packet_ = packet;
  }

  void Rumble(uint32_t slot, uint8_t motor_mask, bool supported, uint32_t result) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    if (summary_)
      return;
    if (REXCVAR_GET(sdl_controller_matrix_resume) ||
        REXCVAR_GET(sdl_controller_matrix_hotplug_resume))
      return;
    if (!armed_ || slot != 0)
      return;
    const bool stopping = motor_mask == 0;
    const bool accepted = supported && result == X_ERROR_SUCCESS;
    if (stopping) {
      if (active_rumble_pattern_ == 0) {
        // Titles commonly initialize vibration with a harmless zero command.
        // It is not part of the physical start/stop challenge.
        return;
      }
      if (accepted)
        rumble_stop_mask_ |= active_rumble_pattern_;
    } else {
      const uint8_t pattern_bit = ControllerMatrixRumblePatternBit(motor_mask);
      if (!pattern_bit) {
        ++unexpected_;
        Failure("rumble_pattern");
        return;
      }
      if (active_rumble_pattern_ || pattern_bit != next_rumble_pattern_) {
        ++unexpected_;
        Failure("rumble_order");
        return;
      }
      if (accepted)
        rumble_pattern_mask_ |= pattern_bit;
      active_rumble_pattern_ = pattern_bit;
    }
    const uint8_t marker_pattern = active_rumble_pattern_;
    const char* pattern = marker_pattern == 1 ? "left" : marker_pattern == 2 ? "right" : "both";
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_RUMBLE v=1 event={} slot=0 pattern={} result={:08X} "
        "supported={}",
        stopping ? "stop" : "start", pattern, result, supported ? 1 : 0);
    ++rumble_current_records_;
    if (!accepted)
      Failure("rumble_result");
    if (stopping) {
      next_rumble_pattern_ = active_rumble_pattern_ == 1 ? 2 : active_rumble_pattern_ == 2 ? 4 : 0;
      active_rumble_pattern_ = 0;
    }
    MaybeInputReady();
    MaybeSummary();
  }

  void ResumeRumbleSuppressed(uint32_t slot, uint8_t motor_mask) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    Config();
    if (summary_)
      return;
    ++resume_rumble_suppressed_;
    ++unexpected_;
    REXLOG_ERROR(
        "SDL_CONTROLLER_MATRIX_AUDIT_RUMBLE_SUPPRESSED v=1 slot={} motor_mask={:X} "
        "result=0000065B count={}",
        slot, motor_mask, resume_rumble_suppressed_);
    Failure("resume_rumble_attempt");
  }

 private:
  bool Enabled() const { return REXCVAR_GET(sdl_controller_matrix_audit); }
  void Config() {
    if (config_)
      return;
    config_ = true;
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_CONFIG v=1 enabled=1 expected_slot=0 "
        "button_mask=F3FF analog_mask=3FF");
  }
  void Failure(const char* classification) {
    ++failures_;
    if (!failure_logged_) {
      failure_logged_ = true;
      REXLOG_ERROR("SDL_CONTROLLER_MATRIX_AUDIT_FAILURE v=1 class={} count={}", classification,
                   failures_);
    }
    Summary(false);
  }
  void MaybeInputReady() {
    if (input_ready_ ||
        !IsControllerMatrixInputReady(armed_, rumble_pattern_mask_, rumble_stop_mask_,
                                      active_rumble_pattern_, current_buttons_, current_analog_)) {
      return;
    }
    input_ready_ = true;
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_INPUT v=1 status=READY neutral=1 "
        "rumble_pattern_mask=7 rumble_stop_mask=7");
  }
  void MaybeSummary() {
    const bool resume = REXCVAR_GET(sdl_controller_matrix_resume);
    const bool hotplug_resume = REXCVAR_GET(sdl_controller_matrix_hotplug_resume);
    const bool pass =
        input_ready_ && button_sdl_mask_ == kControllerMatrixButtonMask &&
        button_guest_mask_ == kControllerMatrixButtonMask &&
        analog_sdl_mask_ == kControllerMatrixAnalogMask &&
        analog_guest_mask_ == kControllerMatrixAnalogMask && focus_lost_ && focus_neutral_ &&
        focus_gained_ && removed_ && disconnected_ && reconnected_ && reconnect_success_ &&
        rumble_pattern_mask_ == 7 && rumble_stop_mask_ == 7 && assigned_mask_ == 1 &&
        pending_button_ == 0 && pending_analog_ == 0 && pending_button_release_ == 0 &&
        pending_analog_neutral_ == 0 && button_guest_down_ == 0 && analog_guest_extreme_ == 0 &&
        rumble_current_records_ ==
            ControllerMatrixExpectedCurrentRumbleRecords(resume || hotplug_resume) &&
        unexpected_ == 0 && failures_ == 0 && dropped_records_ == 0;
    if (pass)
      Summary(true);
  }
  void Summary(bool pass) {
    if (summary_)
      return;
    summary_ = true;
    const bool resume = armed_ && REXCVAR_GET(sdl_controller_matrix_resume);
    const bool hotplug_resume = armed_ && REXCVAR_GET(sdl_controller_matrix_hotplug_resume);
    if (hotplug_resume) {
      REXLOG_INFO(
          "SDL_CONTROLLER_MATRIX_AUDIT_SUMMARY v=1 phase={} status={} button_sdl_mask={:04X} "
          "button_guest_mask={:04X} analog_sdl_mask={:03X} analog_guest_mask={:03X} "
          "focus_lost={} focus_neutral={} focus_gained={} hotplug_removed={} "
          "hotplug_disconnected={} hotplug_reconnected={} hotplug_success={} assigned_mask={:X} "
          "rumble_pattern_mask={:X} rumble_stop_mask={:X} digital_source=prior-evidence "
          "digital_current_records=0 analog_source=prior-evidence analog_current_records=0 "
          "focus_source=prior-evidence focus_current_records=0 rumble_source=prior-evidence "
          "rumble_current_records={} unexpected={} failures={} dropped_records={}",
          phase_valid_ ? "title" : "invalid", pass ? "PASS" : "FAIL", button_sdl_mask_,
          button_guest_mask_, analog_sdl_mask_, analog_guest_mask_, focus_lost_ ? 1 : 0,
          focus_neutral_ ? 1 : 0, focus_gained_ ? 1 : 0, removed_ ? 1 : 0, disconnected_ ? 1 : 0,
          reconnected_ ? 1 : 0, reconnect_success_ ? 1 : 0, assigned_mask_, rumble_pattern_mask_,
          rumble_stop_mask_, rumble_current_records_, unexpected_, failures_, dropped_records_);
      return;
    }
    REXLOG_INFO(
        "SDL_CONTROLLER_MATRIX_AUDIT_SUMMARY v=1 phase={} status={} button_sdl_mask={:04X} "
        "button_guest_mask={:04X} analog_sdl_mask={:03X} analog_guest_mask={:03X} "
        "focus_lost={} focus_neutral={} focus_gained={} hotplug_removed={} "
        "hotplug_disconnected={} hotplug_reconnected={} hotplug_success={} assigned_mask={:X} "
        "rumble_pattern_mask={:X} rumble_stop_mask={:X} rumble_source={} "
        "rumble_current_records={} unexpected={} failures={} dropped_records={}",
        phase_valid_ ? "title" : "invalid", pass ? "PASS" : "FAIL", button_sdl_mask_,
        button_guest_mask_, analog_sdl_mask_, analog_guest_mask_, focus_lost_ ? 1 : 0,
        focus_neutral_ ? 1 : 0, focus_gained_ ? 1 : 0, removed_ ? 1 : 0, disconnected_ ? 1 : 0,
        reconnected_ ? 1 : 0, reconnect_success_ ? 1 : 0, assigned_mask_, rumble_pattern_mask_,
        rumble_stop_mask_, resume ? "prior-evidence" : "current", rumble_current_records_,
        unexpected_, failures_, dropped_records_);
  }

  std::mutex mutex_;
  bool config_ = false, arm_attempted_ = false, phase_valid_ = false, armed_ = false;
  bool input_ready_ = false;
  bool summary_ = false, failure_logged_ = false;
  bool focus_lost_ = false, focus_neutral_ = false, focus_gained_ = false;
  bool removed_ = false, disconnected_ = false, reconnected_ = false, reconnect_success_ = false;
  uint8_t rumble_pattern_mask_ = 0, rumble_stop_mask_ = 0;
  uint8_t active_rumble_pattern_ = 0, next_rumble_pattern_ = 1;
  bool button_source_ready_ = true, analog_source_ready_ = true;
  uint16_t current_buttons_ = 0, current_analog_ = 0;
  uint16_t button_sdl_mask_ = 0, button_guest_mask_ = 0;
  uint16_t analog_sdl_mask_ = 0, analog_guest_mask_ = 0;
  uint16_t pending_button_ = 0, pending_analog_ = 0;
  uint16_t pending_button_release_ = 0, pending_analog_neutral_ = 0;
  uint16_t button_guest_down_ = 0, analog_guest_extreme_ = 0;
  uint32_t active_mask_ = 0, assigned_mask_ = 0, rejected_ = 0;
  uint32_t sequence_ = 0, focus_seq_ = 0, remove_seq_ = 0, reconnect_seq_ = 0;
  uint32_t source_sequence_ = 0, pending_button_seq_ = 0, pending_analog_seq_ = 0;
  uint32_t active_button_seq_ = 0, active_analog_seq_ = 0;
  uint32_t last_packet_ = 0, packet_before_focus_ = 0;
  uint32_t rumble_current_records_ = 0;
  uint32_t resume_rumble_suppressed_ = 0;
  uint32_t unexpected_ = 0, failures_ = 0, dropped_records_ = 0;
};

}  // namespace

void ArmControllerMatrixAudit(const char* phase) {
  ControllerMatrixAudit::Get().Arm(phase);
}

namespace internal {
void RecordControllerMatrixDeviceAssigned(uint32_t slot) {
  ControllerMatrixAudit::Get().DeviceAssigned(slot);
}
void RecordControllerMatrixDeviceRemoved(uint32_t slot) {
  ControllerMatrixAudit::Get().DeviceRemoved(slot);
}
void RecordControllerMatrixDeviceRejected() {
  ControllerMatrixAudit::Get().DeviceRejected();
}
void RecordControllerMatrixStaleEvent(bool axis) {
  ControllerMatrixAudit::Get().StaleEvent(axis);
}
void RecordControllerMatrixSdlButton(uint32_t slot, uint16_t button, bool down) {
  ControllerMatrixAudit::Get().SdlButton(slot, button, down);
}
void RecordControllerMatrixSdlAnalog(uint32_t slot, uint16_t analog_mask) {
  ControllerMatrixAudit::Get().SdlAnalog(slot, analog_mask);
}
void RecordControllerMatrixFocus(bool focused) {
  ControllerMatrixAudit::Get().Focus(focused);
}
void RecordControllerMatrixGuestState(uint32_t requested, uint32_t actual, uint32_t result,
                                      const X_INPUT_STATE* state) {
  ControllerMatrixAudit::Get().GuestState(requested, actual, result, state);
}
bool ShouldSuppressControllerMatrixResumeRumble(bool stopping) {
  return ShouldSuppressControllerMatrixRumble(REXCVAR_GET(sdl_controller_matrix_resume) ||
                                                  REXCVAR_GET(sdl_controller_matrix_hotplug_resume),
                                              stopping);
}
void RecordControllerMatrixResumeRumbleSuppressed(uint32_t slot, uint8_t motor_mask) {
  ControllerMatrixAudit::Get().ResumeRumbleSuppressed(slot, motor_mask);
}
void RecordControllerMatrixRumble(uint32_t slot, uint8_t motor_mask, bool supported,
                                  uint32_t result) {
  ControllerMatrixAudit::Get().Rumble(slot, motor_mask, supported, result);
}
}  // namespace internal
}  // namespace rex::input::sdl
