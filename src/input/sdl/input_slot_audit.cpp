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
#include <rex/input/sdl/input_slot_audit.h>
#include <rex/logging.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_BOOL(sdl_input_slot_audit, false, "Input",
                    "Emit bounded privacy-safe SDL to Xbox user-slot audit markers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::input::sdl {
namespace {

class InputSlotAudit {
 public:
  static InputSlotAudit& Get() {
    static InputSlotAudit audit;
    return audit;
  }

  void RecordDeviceAssigned(uint32_t slot) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    if (slot >= 4) {
      ++rejected_;
      ++failures_;
      EmitRejectedDeviceOnce();
      return;
    }
    const uint32_t slot_bit = uint32_t(1) << slot;
    if (active_slot_mask_ & slot_bit) {
      ++rejected_;
      ++failures_;
      EmitRejectedDeviceOnce();
      return;
    }
    active_slot_mask_ |= slot_bit;
    ++active_devices_;
    if (slot != 0 || active_devices_ != 1)
      ++unexpected_;
    if (!(assigned_record_mask_ & slot_bit)) {
      assigned_record_mask_ |= slot_bit;
      REXLOG_INFO("SDL_INPUT_SLOT_AUDIT_DEVICE v=1 event=assigned slot={} active_devices={}", slot,
                  active_devices_);
    }
  }

  void RecordDeviceRemoved(uint32_t slot) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++removals_;
    if (slot < 4) {
      const uint32_t slot_bit = uint32_t(1) << slot;
      if (active_slot_mask_ & slot_bit) {
        active_slot_mask_ &= ~slot_bit;
        if (active_devices_)
          --active_devices_;
      }
    }
    if (!removal_recorded_) {
      removal_recorded_ = true;
      REXLOG_ERROR("SDL_INPUT_SLOT_AUDIT_DEVICE v=1 event=removed slot={} active_devices={}", slot,
                   active_devices_);
    }
  }

  void RecordDeviceRejected() {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++rejected_;
    EmitRejectedDeviceOnce();
  }

  void Arm(const char* phase) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    if (arm_attempted_)
      return;
    arm_attempted_ = true;
    phase_valid_ = phase && std::strcmp(phase, "title") == 0;
    const bool neutral = !sdl_a_down_;
    armed_ = phase_valid_ && active_devices_ == 1 && active_slot_mask_ == 1 && neutral &&
             removals_ == 0 && rejected_ == 0 && unexpected_ == 0 && failures_ == 0;
    if (armed_) {
      REXLOG_INFO(
          "SDL_INPUT_SLOT_AUDIT_ARM v=1 phase=title status=READY active_devices=1 "
          "slot_mask=1 neutral=1");
    } else {
      ++failures_;
      REXLOG_ERROR(
          "SDL_INPUT_SLOT_AUDIT_ARM v=1 phase={} status=FAIL active_devices={} slot_mask={:X} "
          "neutral={}",
          phase_valid_ ? "title" : "invalid", active_devices_, active_slot_mask_, neutral ? 1 : 0);
      EmitSummaryLocked(false);
    }
  }

  void RecordSdlAEdge(uint32_t slot, bool down) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    sdl_a_down_ = down;
    if (!armed_ || summary_emitted_)
      return;
    if (slot != 0) {
      ++unexpected_;
      ++failures_;
      EmitEdgeFailureOnce(slot, down);
      return;
    }
    if (down) {
      if (sdl_down_seq_) {
        ++unexpected_;
        ++failures_;
        EmitEdgeFailureOnce(slot, down);
        return;
      }
      sdl_down_seq_ = ++source_seq_;
      REXLOG_INFO("SDL_INPUT_SLOT_AUDIT_SDL_EDGE v=1 control=A edge=down slot=0 source_seq={}",
                  sdl_down_seq_);
      return;
    }
    if (!sdl_down_seq_ || sdl_up_seq_) {
      ++unexpected_;
      ++failures_;
      EmitEdgeFailureOnce(slot, down);
      return;
    }
    sdl_up_seq_ = ++source_seq_;
    REXLOG_INFO("SDL_INPUT_SLOT_AUDIT_SDL_EDGE v=1 control=A edge=up slot=0 source_seq={}",
                sdl_up_seq_);
  }

  void RecordGuestState(uint32_t requested_user_index, uint32_t actual_user_index, uint32_t result,
                        const X_INPUT_STATE* state) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    if (requested_user_index < 4) {
      const uint32_t bit = uint32_t(1) << requested_user_index;
      query_mask_ |= bit;
      if (result == X_ERROR_SUCCESS) {
        success_mask_ |= bit;
      } else if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
        disconnected_mask_ |= bit;
      } else {
        ++failures_;
        if (!guest_failure_recorded_) {
          guest_failure_recorded_ = true;
          REXLOG_ERROR(
              "SDL_INPUT_SLOT_AUDIT_GUEST_STATE v=1 control=A edge=none slot={} "
              "result={:08X} source_seq=0",
              requested_user_index, result);
        }
      }
    }
    if (!armed_ || summary_emitted_ || requested_user_index != 0 || actual_user_index != 0 ||
        result != X_ERROR_SUCCESS || !state) {
      return;
    }
    const bool a_down = (static_cast<uint16_t>(state->gamepad.buttons) & X_INPUT_GAMEPAD_A) != 0;
    if (a_down && sdl_down_seq_ && !guest_down_seq_) {
      guest_down_seq_ = sdl_down_seq_;
      REXLOG_INFO(
          "SDL_INPUT_SLOT_AUDIT_GUEST_STATE v=1 control=A edge=down slot=0 "
          "result=00000000 source_seq={}",
          guest_down_seq_);
      return;
    }
    if (!a_down && guest_down_seq_ && sdl_up_seq_ && !guest_up_seq_) {
      guest_up_seq_ = sdl_up_seq_;
      REXLOG_INFO(
          "SDL_INPUT_SLOT_AUDIT_GUEST_STATE v=1 control=A edge=up slot=0 result=00000000 "
          "source_seq={}",
          guest_up_seq_);
      const bool pass = active_devices_ == 1 && active_slot_mask_ == 1 && query_mask_ == 0xF &&
                        success_mask_ == 1 && disconnected_mask_ == 0xE && sdl_down_seq_ == 1 &&
                        guest_down_seq_ == 1 && sdl_up_seq_ == 2 && guest_up_seq_ == 2 &&
                        removals_ == 0 && rejected_ == 0 && unexpected_ == 0 && failures_ == 0 &&
                        dropped_records_ == 0;
      EmitSummaryLocked(pass);
    }
  }

 private:
  bool Enabled() const { return REXCVAR_GET(sdl_input_slot_audit); }

  void EnsureConfig() {
    if (!config_emitted_) {
      config_emitted_ = true;
      REXLOG_INFO("SDL_INPUT_SLOT_AUDIT_CONFIG v=1 enabled=1 expected_slot=0");
    }
  }

  void EmitRejectedDeviceOnce() {
    if (!rejected_recorded_) {
      rejected_recorded_ = true;
      REXLOG_ERROR("SDL_INPUT_SLOT_AUDIT_DEVICE v=1 event=rejected slot=none active_devices={}",
                   active_devices_);
    }
  }

  void EmitEdgeFailureOnce(uint32_t slot, bool down) {
    if (!edge_failure_recorded_) {
      edge_failure_recorded_ = true;
      REXLOG_ERROR("SDL_INPUT_SLOT_AUDIT_SDL_EDGE v=1 control=A edge={} slot={} source_seq=0",
                   down ? "down" : "up", slot);
    }
  }

  void EmitSummaryLocked(bool pass) {
    if (summary_emitted_)
      return;
    summary_emitted_ = true;
    REXLOG_INFO(
        "SDL_INPUT_SLOT_AUDIT_SUMMARY v=1 phase={} status={} active_devices={} slot_mask={:X} "
        "query_mask={:X} success_mask={:X} disconnected_mask={:X} sdl_down_seq={} "
        "guest_down_seq={} sdl_up_seq={} guest_up_seq={} removals={} rejected={} unexpected={} "
        "failures={} dropped_records={}",
        phase_valid_ ? "title" : "invalid", pass ? "PASS" : "FAIL", active_devices_,
        active_slot_mask_, query_mask_, success_mask_, disconnected_mask_, sdl_down_seq_,
        guest_down_seq_, sdl_up_seq_, guest_up_seq_, removals_, rejected_, unexpected_, failures_,
        dropped_records_);
  }

  std::mutex mutex_;
  bool config_emitted_ = false;
  bool arm_attempted_ = false;
  bool armed_ = false;
  bool phase_valid_ = false;
  bool summary_emitted_ = false;
  bool rejected_recorded_ = false;
  bool removal_recorded_ = false;
  bool edge_failure_recorded_ = false;
  bool guest_failure_recorded_ = false;
  bool sdl_a_down_ = false;
  uint32_t active_devices_ = 0;
  uint32_t active_slot_mask_ = 0;
  uint32_t assigned_record_mask_ = 0;
  uint32_t query_mask_ = 0;
  uint32_t success_mask_ = 0;
  uint32_t disconnected_mask_ = 0;
  uint32_t source_seq_ = 0;
  uint32_t sdl_down_seq_ = 0;
  uint32_t guest_down_seq_ = 0;
  uint32_t sdl_up_seq_ = 0;
  uint32_t guest_up_seq_ = 0;
  uint32_t removals_ = 0;
  uint32_t rejected_ = 0;
  uint32_t unexpected_ = 0;
  uint32_t failures_ = 0;
  uint32_t dropped_records_ = 0;
};

}  // namespace

void ArmInputSlotAudit(const char* phase) {
  InputSlotAudit::Get().Arm(phase);
}

namespace internal {

void RecordInputSlotDeviceAssigned(uint32_t slot) {
  InputSlotAudit::Get().RecordDeviceAssigned(slot);
}

void RecordInputSlotDeviceRemoved(uint32_t slot) {
  InputSlotAudit::Get().RecordDeviceRemoved(slot);
}

void RecordInputSlotDeviceRejected() {
  InputSlotAudit::Get().RecordDeviceRejected();
}

void RecordInputSlotSdlAEdge(uint32_t slot, bool down) {
  InputSlotAudit::Get().RecordSdlAEdge(slot, down);
}

void RecordInputSlotGuestState(uint32_t requested_user_index, uint32_t actual_user_index,
                               uint32_t result, const X_INPUT_STATE* state) {
  InputSlotAudit::Get().RecordGuestState(requested_user_index, actual_user_index, result, state);
}

}  // namespace internal
}  // namespace rex::input::sdl
