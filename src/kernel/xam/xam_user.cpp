/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <array>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/kernel/xam/profile_audit.h>
#include <rex/kernel/xam/private.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/hook.h>
#include <rex/thread.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

REXCVAR_DEFINE_UINT32(user_language, 1, "Kernel", "User's language ID");
REXCVAR_DEFINE_UINT32(xam_user_signin_state, 1, "XAM",
                      "Guest sign-in state: 1 for local, 2 for online-compatible title behavior")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(xam_profile_audit, false, "XAM",
                    "Emit bounded privacy-safe XAM user/profile audit markers")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

namespace {

constexpr std::array<uint32_t, 3> kMclaProfileSettingIds = {0x1004000C, 0x1004000D, 0x1004000E};
constexpr uint32_t kMclaProfileBufferSize =
    sizeof(uint32_t) * 2 + sizeof(X_USER_PROFILE_SETTING) * kMclaProfileSettingIds.size();

class XamProfileAudit {
 public:
  static XamProfileAudit& Get() {
    static XamProfileAudit audit;
    return audit;
  }

  void RecordSigninState(uint32_t user_index, uint32_t state) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++signin_state_calls_;
    const bool slot0_success = user_index == 0 && state == 1;
    const bool other_success = user_index > 0 && user_index < 4 && state == 0;
    const bool success = slot0_success || other_success;
    if (slot0_success) {
      ++signin_state_slot0_local_;
      saw_signin_state_ = true;
    } else if (other_success) {
      ++signin_state_other_;
      signin_state_other_mask_ |= uint32_t(1) << user_index;
    }
    bool& recorded =
        slot0_success ? signin_state_success_recorded_ : signin_state_failure_recorded_;
    if (other_success) {
      const uint32_t slot_bit = uint32_t(1) << user_index;
      if (!(signin_state_other_record_mask_ & slot_bit)) {
        signin_state_other_record_mask_ |= slot_bit;
        REXKRNL_INFO("XAM_PROFILE_AUDIT_SIGNIN_STATE v=1 class=absent user={} state=0", user_index);
      }
      return;
    }
    if (!recorded) {
      recorded = true;
      if (success) {
        REXKRNL_INFO("XAM_PROFILE_AUDIT_SIGNIN_STATE v=1 class=success user={} state={}",
                     user_index, state);
      } else {
        REXKRNL_ERROR("XAM_PROFILE_AUDIT_SIGNIN_STATE v=1 class=failure user={} state={}",
                      user_index, state);
      }
    }
  }

  void RecordXuid(uint32_t user_index, uint32_t mask, uint32_t result, bool nonzero) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++xuid_calls_;
    const bool success = user_index == 0 && mask == 7 && result == X_E_SUCCESS && nonzero;
    if (success) {
      ++xuid_success_;
      ++xuid_nonzero_;
      saw_xuid_ = true;
    }
    bool& recorded = success ? xuid_success_recorded_ : xuid_failure_recorded_;
    if (!recorded) {
      recorded = true;
      if (success) {
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_XUID v=1 class=success user={} mask={} result={:08X} "
            "nonzero={}",
            user_index, mask, result, nonzero ? 1 : 0);
      } else {
        REXKRNL_ERROR(
            "XAM_PROFILE_AUDIT_XUID v=1 class=failure user={} mask={} result={:08X} "
            "nonzero={}",
            user_index, mask, result, nonzero ? 1 : 0);
      }
    }
  }

  void RecordProfileRead(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                         uint32_t setting_count, const be<uint32_t>* setting_ids,
                         uint32_t buffer_size_in, uint32_t buffer_size_out, bool has_buffer,
                         uint32_t result, bool overlapped, uint32_t completed_result) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++profile_read_calls_;
    const bool exact_ids = HasExactMclaSettingIds(setting_count, setting_ids);
    const bool exact_title = title_id == 0xFFFE07D1;
    const bool is_size = exact_title && exact_ids && user_index == 0xFF && xuid_count == 0 &&
                         !has_buffer && buffer_size_in == 0 &&
                         result == X_ERROR_INSUFFICIENT_BUFFER &&
                         buffer_size_out == kMclaProfileBufferSize;
    const bool semantic_success =
        result == X_ERROR_SUCCESS ||
        (overlapped && result == X_ERROR_IO_PENDING && completed_result == X_ERROR_SUCCESS);
    const bool is_fill = exact_title && exact_ids && user_index == 0 && xuid_count == 0 &&
                         has_buffer && buffer_size_in == kMclaProfileBufferSize &&
                         buffer_size_out == kMclaProfileBufferSize && semantic_success;
    const char* class_name = nullptr;
    bool* recorded = nullptr;
    if (is_size) {
      class_name = "size";
      recorded = &profile_read_size_recorded_;
      ++profile_read_size_;
      saw_profile_size_ = true;
    } else if (is_fill) {
      class_name = "fill";
      recorded = &profile_read_fill_recorded_;
      ++profile_read_fill_;
      saw_profile_fill_ = true;
    } else {
      ++profile_read_failure_;
      class_name = "failure";
      recorded = &profile_read_failure_recorded_;
    }
    if (recorded && !*recorded) {
      *recorded = true;
      const std::string ids = FormatSettingIds(setting_count, setting_ids);
      if (is_size || is_fill) {
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_PROFILE_READ v=1 class={} title=profile user={} xuid_count={} "
            "setting_count={} setting_ids={} buffer_size_in={} buffer_size_out={} "
            "result={:08X} overlapped={} completed={:08X}",
            class_name, user_index, xuid_count, setting_count, ids, buffer_size_in, buffer_size_out,
            result, overlapped ? 1 : 0, completed_result);
      } else {
        REXKRNL_ERROR(
            "XAM_PROFILE_AUDIT_PROFILE_READ v=1 class=failure title={} user={} xuid_count={} "
            "setting_count={} setting_ids={} buffer_size_in={} buffer_size_out={} "
            "result={:08X} overlapped={} completed={:08X}",
            exact_title ? "profile" : "other", user_index, xuid_count, setting_count, ids,
            buffer_size_in, buffer_size_out, result, overlapped ? 1 : 0, completed_result);
      }
    }
  }

  void RecordPrivilege(uint32_t user_index, uint32_t mask, uint32_t result, bool allowed) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++privilege_calls_;
    const bool expected_mask = mask == 251 || mask == 252;
    const bool success = user_index == 0 && expected_mask && result == X_ERROR_SUCCESS && !allowed;
    bool* recorded = &privilege_failure_recorded_;
    if (success) {
      ++privilege_success_;
      if (mask == 251) {
        ++privilege_251_false_;
        saw_privilege_251_ = true;
        recorded = &privilege_251_recorded_;
      } else if (mask == 252) {
        ++privilege_252_false_;
        saw_privilege_252_ = true;
        recorded = &privilege_252_recorded_;
      }
    }
    if (allowed)
      ++privilege_allowed_;
    if (!*recorded) {
      *recorded = true;
      if (success) {
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_PRIVILEGE v=1 class=success user={} mask={} result={:08X} "
            "allowed={}",
            user_index, mask, result, allowed ? 1 : 0);
      } else {
        REXKRNL_ERROR(
            "XAM_PROFILE_AUDIT_PRIVILEGE v=1 class=failure user={} mask={} result={:08X} "
            "allowed={}",
            user_index, mask, result, allowed ? 1 : 0);
      }
    }
  }

  void RecordName(uint32_t user_index, uint32_t result, bool profile_match) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++name_calls_;
    const bool success = user_index == 0 && result == X_ERROR_SUCCESS && profile_match;
    if (success)
      ++name_match_;
    bool& recorded = success ? name_success_recorded_ : name_failure_recorded_;
    if (!recorded) {
      recorded = true;
      if (success) {
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_NAME v=1 class=success user={} result={:08X} profile_match=1",
            user_index, result);
      } else {
        REXKRNL_ERROR(
            "XAM_PROFILE_AUDIT_NAME v=1 class=failure user={} result={:08X} "
            "profile_match={}",
            user_index, result, profile_match ? 1 : 0);
      }
    }
  }

  void RecordSigninInfo(uint32_t user_index, uint32_t result, bool state_match, bool xuid_match,
                        bool name_match) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++signin_info_calls_;
    const bool success =
        user_index == 0 && result == X_E_SUCCESS && state_match && xuid_match && name_match;
    const bool absent = user_index > 0 && user_index < 4 && result == X_E_NO_SUCH_USER;
    if (success)
      ++signin_info_consistent_;
    if (absent)
      ++signin_info_absent_;
    if (absent)
      signin_info_absent_mask_ |= uint32_t(1) << user_index;
    const uint32_t absent_bit = absent ? uint32_t(1) << user_index : 0;
    bool should_record = false;
    if (success) {
      should_record = !signin_info_success_recorded_;
    } else if (absent) {
      should_record = !(signin_info_absent_record_mask_ & absent_bit);
    } else {
      should_record = !signin_info_failure_recorded_;
    }
    if (should_record) {
      if (success) {
        signin_info_success_recorded_ = true;
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_SIGNIN_INFO v=1 class=success user={} result={:08X} "
            "state_match=1 xuid_match=1 name_match=1",
            user_index, result);
      } else if (absent) {
        signin_info_absent_record_mask_ |= absent_bit;
        REXKRNL_INFO(
            "XAM_PROFILE_AUDIT_SIGNIN_INFO v=1 class=absent user={} result={:08X} "
            "state_match=0 xuid_match=0 name_match=0",
            user_index, result);
      } else {
        signin_info_failure_recorded_ = true;
        REXKRNL_ERROR(
            "XAM_PROFILE_AUDIT_SIGNIN_INFO v=1 class=failure user={} result={:08X} "
            "state_match={} xuid_match={} name_match={}",
            user_index, result, state_match ? 1 : 0, xuid_match ? 1 : 0, name_match ? 1 : 0);
      }
    }
  }

  void RecordSigninUi() {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    ++signin_ui_calls_;
    ++signin_ui_ordered_;
    if (!signin_ui_recorded_) {
      signin_ui_recorded_ = true;
      REXKRNL_INFO(
          "XAM_PROFILE_AUDIT_SIGNIN_UI v=1 class=success signin_changed_seq=1 "
          "ui_off_seq=2 ordered=1 result=00000000");
    }
  }

  void EmitSummary(const char* phase) {
    if (!Enabled())
      return;
    std::lock_guard lock(mutex_);
    EnsureConfig();
    if (summary_emitted_)
      return;
    summary_emitted_ = true;
    const bool checkpoint = phase && std::strcmp(phase, "checkpoint") == 0;
    const bool profile_contract =
        profile_read_calls_ == 0 || (saw_profile_size_ && saw_profile_fill_);
    const bool privilege_contract =
        privilege_calls_ == 0 || (saw_privilege_251_ && saw_privilege_252_);
    const bool pass =
        checkpoint && saw_signin_state_ && signin_state_other_mask_ == 0xE && saw_xuid_ &&
        xuid_success_ > 0 && xuid_nonzero_ > 0 && name_match_ > 0 && signin_info_consistent_ > 0 &&
        signin_info_absent_mask_ == 0xE && profile_contract && privilege_contract &&
        profile_read_failure_ == 0 && !signin_state_failure_recorded_ && !xuid_failure_recorded_ &&
        !privilege_failure_recorded_ && !name_failure_recorded_ && !signin_info_failure_recorded_ &&
        privilege_allowed_ == 0 && (!signin_ui_calls_ || signin_ui_calls_ == signin_ui_ordered_);
    REXKRNL_INFO(
        "XAM_PROFILE_AUDIT_SUMMARY v=1 phase={} status={} signin_state_calls={} "
        "signin_state_slot0_local={} signin_state_other={} signin_state_other_mask={:X} "
        "xuid_calls={} xuid_success={} "
        "xuid_nonzero={} profile_read_calls={} profile_read_size={} profile_read_fill={} "
        "profile_read_failure={} privilege_calls={} privilege_success={} privilege_allowed={} "
        "privilege_251_false={} privilege_252_false={} name_calls={} name_match={} "
        "signin_info_calls={} signin_info_consistent={} signin_info_absent={} "
        "signin_info_absent_mask={:X} signin_ui_calls={} "
        "signin_ui_ordered={} dropped_records={}",
        phase ? phase : "invalid", pass ? "PASS" : "FAIL", signin_state_calls_,
        signin_state_slot0_local_, signin_state_other_, signin_state_other_mask_, xuid_calls_,
        xuid_success_, xuid_nonzero_, profile_read_calls_, profile_read_size_, profile_read_fill_,
        profile_read_failure_, privilege_calls_, privilege_success_, privilege_allowed_,
        privilege_251_false_, privilege_252_false_, name_calls_, name_match_, signin_info_calls_,
        signin_info_consistent_, signin_info_absent_, signin_info_absent_mask_, signin_ui_calls_,
        signin_ui_ordered_, dropped_records_);
  }

 private:
  bool Enabled() const { return REXCVAR_GET(xam_profile_audit); }

  void EnsureConfig() {
    if (!config_emitted_) {
      config_emitted_ = true;
      REXKRNL_INFO("XAM_PROFILE_AUDIT_CONFIG v=1 enabled=1 slot=0");
    }
  }

  static bool HasExactMclaSettingIds(uint32_t count, const be<uint32_t>* setting_ids) {
    if (count != kMclaProfileSettingIds.size() || !setting_ids)
      return false;
    for (size_t i = 0; i < kMclaProfileSettingIds.size(); ++i) {
      if (static_cast<uint32_t>(setting_ids[i]) != kMclaProfileSettingIds[i])
        return false;
    }
    return true;
  }

  static std::string FormatSettingIds(uint32_t count, const be<uint32_t>* setting_ids) {
    if (!setting_ids || !count)
      return "none";
    std::string result;
    const uint32_t bounded_count = std::min(count, uint32_t(32));
    for (uint32_t i = 0; i < bounded_count; ++i) {
      if (i)
        result.push_back(',');
      result += fmt::format("{:08X}", static_cast<uint32_t>(setting_ids[i]));
    }
    return result;
  }

  std::mutex mutex_;
  bool config_emitted_ = false;
  bool summary_emitted_ = false;
  bool signin_state_success_recorded_ = false;
  bool signin_state_failure_recorded_ = false;
  uint32_t signin_state_other_record_mask_ = 0;
  bool xuid_success_recorded_ = false;
  bool xuid_failure_recorded_ = false;
  bool profile_read_size_recorded_ = false;
  bool profile_read_fill_recorded_ = false;
  bool profile_read_failure_recorded_ = false;
  bool privilege_251_recorded_ = false;
  bool privilege_252_recorded_ = false;
  bool privilege_failure_recorded_ = false;
  bool name_success_recorded_ = false;
  bool name_failure_recorded_ = false;
  bool signin_info_success_recorded_ = false;
  bool signin_info_failure_recorded_ = false;
  uint32_t signin_info_absent_record_mask_ = 0;
  bool signin_ui_recorded_ = false;
  bool saw_signin_state_ = false;
  bool saw_xuid_ = false;
  bool saw_profile_size_ = false;
  bool saw_profile_fill_ = false;
  bool saw_privilege_251_ = false;
  bool saw_privilege_252_ = false;
  uint64_t signin_state_calls_ = 0;
  uint64_t signin_state_slot0_local_ = 0;
  uint64_t signin_state_other_ = 0;
  uint32_t signin_state_other_mask_ = 0;
  uint64_t xuid_calls_ = 0;
  uint64_t xuid_success_ = 0;
  uint64_t xuid_nonzero_ = 0;
  uint64_t profile_read_calls_ = 0;
  uint64_t profile_read_size_ = 0;
  uint64_t profile_read_fill_ = 0;
  uint64_t profile_read_failure_ = 0;
  uint64_t privilege_calls_ = 0;
  uint64_t privilege_success_ = 0;
  uint64_t privilege_allowed_ = 0;
  uint64_t privilege_251_false_ = 0;
  uint64_t privilege_252_false_ = 0;
  uint64_t name_calls_ = 0;
  uint64_t name_match_ = 0;
  uint64_t signin_info_calls_ = 0;
  uint64_t signin_info_consistent_ = 0;
  uint64_t signin_info_absent_ = 0;
  uint32_t signin_info_absent_mask_ = 0;
  uint64_t signin_ui_calls_ = 0;
  uint64_t signin_ui_ordered_ = 0;
  uint64_t dropped_records_ = 0;
};

}  // namespace

void EmitXamProfileAuditSummary(const char* phase) {
  XamProfileAudit::Get().EmitSummary(phase);
}

i32 XamUserGetXUID_entry(u32 user_index, u32 type_mask, mapped_u64 xuid_ptr) {
  if (type_mask != 1 && type_mask != 2 && type_mask != 3 && type_mask != 4 && type_mask != 7) {
    XamProfileAudit::Get().RecordXuid(user_index, type_mask, X_E_INVALIDARG, false);
    return X_E_INVALIDARG;
  }
  if (!xuid_ptr) {
    XamProfileAudit::Get().RecordXuid(user_index, type_mask, X_E_INVALIDARG, false);
    return X_E_INVALIDARG;
  }
  uint32_t result = X_E_NO_SUCH_USER;
  uint64_t xuid = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      auto type = user_profile->type() & type_mask;
      if (type & (2 | 4)) {
        // maybe online profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      } else if (type & 1) {
        // maybe offline profile?
        xuid = user_profile->xuid();
        result = X_E_SUCCESS;
      }
    }
  } else {
    result = X_E_INVALIDARG;
  }
  *xuid_ptr = xuid;
  XamProfileAudit::Get().RecordXuid(user_index, type_mask, result, xuid != 0);
  return result;
}

u32 XamUserGetSigninState_entry(u32 user_index) {
  // This import is polled at high frequency by some titles. Match Xenia's
  // cooperative scheduling behavior instead of monopolizing a host worker.
  rex::thread::MaybeYield();
  uint32_t signin_state = 0;
  if (user_index < 4) {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      signin_state = user_profile->signin_state();
    }
  }
  XamProfileAudit::Get().RecordSigninState(user_index, signin_state);
  return signin_state;
}

typedef struct {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;  // maybe zero?
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;  // ?
  rex::be<uint32_t> unk14;  // ?
  char name[16];
} X_USER_SIGNIN_INFO;
static_assert_size(X_USER_SIGNIN_INFO, 40);

i32 XamUserGetSigninInfo_entry(u32 user_index, u32 flags, ppc_ptr_t<X_USER_SIGNIN_INFO> info) {
  if (!info) {
    XamProfileAudit::Get().RecordSigninInfo(user_index, X_E_INVALIDARG, false, false, false);
    return X_E_INVALIDARG;
  }

  std::memset(info, 0, sizeof(X_USER_SIGNIN_INFO));
  if (user_index) {
    XamProfileAudit::Get().RecordSigninInfo(user_index, X_E_NO_SUCH_USER, false, false, false);
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  info->xuid = user_profile->xuid();
  info->signin_state = user_profile->signin_state();
  rex::string::copy_truncating(info->name, user_profile->name(), rex::countof(info->name));
  XamProfileAudit::Get().RecordSigninInfo(
      user_index, X_E_SUCCESS,
      static_cast<uint32_t>(info->signin_state) == user_profile->signin_state(),
      static_cast<uint64_t>(info->xuid) == user_profile->xuid(),
      std::string_view(info->name) == user_profile->name());
  return X_E_SUCCESS;
}

u32 XamUserGetName_entry(u32 user_index, mapped_string buffer, u32 buffer_len) {
  if (!buffer || !buffer_len) {
    XamProfileAudit::Get().RecordName(user_index, X_E_INVALIDARG, false);
    return X_E_INVALIDARG;
  }
  if (user_index >= 4) {
    XamProfileAudit::Get().RecordName(user_index, X_E_INVALIDARG, false);
    return X_E_INVALIDARG;
  }

  if (user_index) {
    XamProfileAudit::Get().RecordName(user_index, X_E_NO_SUCH_USER, false);
    return X_E_NO_SUCH_USER;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  const auto& user_name = user_profile->name();
  rex::string::copy_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  XamProfileAudit::Get().RecordName(user_index, X_E_SUCCESS, buffer.value() == user_name);
  return X_E_SUCCESS;
}

u32 XamUserGetGamerTag_entry(u32 user_index, mapped_wstring buffer, u32 buffer_len) {
  if (user_index >= 4) {
    return X_E_INVALIDARG;
  }

  if (user_index) {
    return X_E_NO_SUCH_USER;
  }

  if (!buffer || buffer_len < 16) {
    return X_E_INVALIDARG;
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();
  auto user_name = rex::string::to_utf16(user_profile->name());
  rex::string::copy_and_swap_truncating(buffer, user_name, std::min(buffer_len, uint32_t(16)));
  return X_E_SUCCESS;
}

typedef struct {
  rex::be<uint32_t> setting_count;
  rex::be<uint32_t> settings_ptr;
} X_USER_READ_PROFILE_SETTINGS;
static_assert_size(X_USER_READ_PROFILE_SETTINGS, 8);

// https://github.com/oukiar/freestyledash/blob/master/Freestyle/Tools/Generic/xboxtools.cpp
uint32_t XamUserReadProfileSettingsEx(uint32_t title_id, uint32_t user_index, uint32_t xuid_count,
                                      be<uint64_t>* xuids, uint32_t setting_count,
                                      be<uint32_t>* setting_ids, uint32_t unk,
                                      be<uint32_t>* buffer_size_ptr, uint8_t* buffer,
                                      XAM_OVERLAPPED* overlapped) {
  const uint32_t buffer_size_in = buffer_size_ptr ? static_cast<uint32_t>(*buffer_size_ptr) : 0;
  auto finish = [&](uint32_t result) {
    const uint32_t buffer_size_out = buffer_size_ptr ? static_cast<uint32_t>(*buffer_size_ptr) : 0;
    const uint32_t completed_result = overlapped && result == X_ERROR_IO_PENDING
                                          ? static_cast<uint32_t>(overlapped->result)
                                          : result;
    XamProfileAudit::Get().RecordProfileRead(title_id, user_index, xuid_count, setting_count,
                                             setting_count <= 32 ? setting_ids : nullptr,
                                             buffer_size_in, buffer_size_out, buffer != nullptr,
                                             result, overlapped != nullptr, completed_result);
    return result;
  };
  if ((!xuid_count && xuids) || (xuid_count && (!xuids || xuid_count != 1)) || unk != 0 ||
      (setting_count && !setting_ids)) {
    return finish(X_ERROR_INVALID_PARAMETER);
  }
  if (xuid_count) {
    // TODO(gibbed): allow proper lookup of arbitrary XUIDs
    const auto& user_profile = REX_KERNEL_STATE()->user_profile();
    if (static_cast<uint64_t>(xuids[0]) != user_profile->xuid()) {
      return finish(X_ERROR_INVALID_PARAMETER);
    }
    // TODO(gibbed): we assert here, but in case a title passes xuid_count > 1
    // until it's implemented for release builds...
    xuid_count = 1;
  }
  // must have at least 1 to 32 settings
  if (setting_count < 1 || setting_count > 32) {
    return finish(X_ERROR_INVALID_PARAMETER);
  }

  // buffer size pointer must be valid
  if (!buffer_size_ptr) {
    return finish(X_ERROR_INVALID_PARAMETER);
  }

  // if buffer size is non-zero, buffer pointer must be valid
  auto buffer_size = static_cast<uint32_t>(*buffer_size_ptr);
  if (buffer_size && !buffer) {
    return finish(X_ERROR_INVALID_PARAMETER);
  }

  uint32_t needed_header_size = 0;
  uint32_t needed_data_size = 0;
  for (uint32_t i = 0; i < setting_count; ++i) {
    needed_header_size += sizeof(X_USER_PROFILE_SETTING);
    UserProfile::Setting::Key setting_key;
    setting_key.value = static_cast<uint32_t>(setting_ids[i]);
    switch (static_cast<UserProfile::Setting::Type>(setting_key.type)) {
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::BINARY:
        needed_data_size += setting_key.size;
        break;
      default:
        break;
    }
  }
  if (xuids) {
    needed_header_size *= xuid_count;
    needed_data_size *= xuid_count;
  }
  needed_header_size += sizeof(X_USER_READ_PROFILE_SETTINGS);

  uint32_t needed_size = needed_header_size + needed_data_size;
  if (!buffer || buffer_size < needed_size) {
    if (!buffer_size) {
      *buffer_size_ptr = needed_size;
    }
    return finish(X_ERROR_INSUFFICIENT_BUFFER);
  }

  // Title ID = 0 means us.
  // 0xfffe07d1 = profile?

  if (!xuids && user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_NO_SUCH_USER);
      return finish(X_ERROR_IO_PENDING);
    }
    return finish(X_ERROR_NO_SUCH_USER);
  }

  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  // First call asks for size (fill buffer_size_ptr).
  // Second call asks for buffer contents with that size.

  // TODO(gibbed): setting validity checking without needing a user profile
  // object.
  bool any_missing = false;
  for (uint32_t i = 0; i < setting_count; ++i) {
    auto setting_id = static_cast<uint32_t>(setting_ids[i]);
    auto setting = user_profile->GetSetting(setting_id);
    if (!setting) {
      any_missing = true;
      REXKRNL_ERROR(
          "xeXamUserReadProfileSettingsEx requested unimplemented setting "
          "{:08X}",
          setting_id);
    }
  }
  if (any_missing) {
    // TODO(benvanik): don't fail? most games don't even check!
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(
          REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_INVALID_PARAMETER);
      return finish(X_ERROR_IO_PENDING);
    }
    return finish(X_ERROR_INVALID_PARAMETER);
  }

  auto out_header = reinterpret_cast<X_USER_READ_PROFILE_SETTINGS*>(buffer);
  auto out_setting = reinterpret_cast<X_USER_PROFILE_SETTING*>(&out_header[1]);
  out_header->setting_count = static_cast<uint32_t>(setting_count);
  out_header->settings_ptr = REX_KERNEL_MEMORY()->HostToGuestVirtual(out_setting);

  UserProfile::SettingByteStream out_stream(REX_KERNEL_MEMORY()->HostToGuestVirtual(buffer), buffer,
                                            buffer_size, needed_header_size);
  for (uint32_t n = 0; n < setting_count; ++n) {
    uint32_t setting_id = setting_ids[n];
    auto setting = user_profile->GetSetting(setting_id);

    std::memset(out_setting, 0, sizeof(X_USER_PROFILE_SETTING));
    out_setting->from = !setting || !setting->is_set ? 0 : setting->is_title_specific() ? 2 : 1;
    if (xuids) {
      out_setting->xuid = user_profile->xuid();
    } else {
      out_setting->user_index = static_cast<uint32_t>(user_index);
    }
    out_setting->setting_id = setting_id;

    if (setting && setting->is_set) {
      setting->Append(&out_setting->data, &out_stream);
    }
    ++out_setting;
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(
        REX_KERNEL_MEMORY()->HostToGuestVirtual(overlapped), X_ERROR_SUCCESS);
    return finish(X_ERROR_IO_PENDING);
  }
  return finish(X_ERROR_SUCCESS);
}

u32 XamUserReadProfileSettings_entry(u32 title_id, u32 user_index, u32 xuid_count, mapped_u64 xuids,
                                     u32 setting_count, mapped_u32 setting_ids,
                                     mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                     ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, 0, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserReadProfileSettingsEx_entry(u32 title_id, u32 user_index, u32 xuid_count,
                                       mapped_u64 xuids, u32 setting_count, mapped_u32 setting_ids,
                                       mapped_u32 buffer_size_ptr, u32 unk_2,
                                       mapped_void buffer_ptr,
                                       ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  return XamUserReadProfileSettingsEx(title_id, user_index, xuid_count, xuids, setting_count,
                                      setting_ids, unk_2, buffer_size_ptr, buffer_ptr, overlapped);
}

u32 XamUserWriteProfileSettings_entry(u32 title_id, u32 user_index, u32 setting_count,
                                      ppc_ptr_t<X_USER_PROFILE_SETTING> settings,
                                      ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!setting_count || !settings) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index) {
    // Only support user 0.
    if (overlapped) {
      REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(),
                                                      X_ERROR_NO_SUCH_USER);
      return X_ERROR_IO_PENDING;
    }
    return X_ERROR_NO_SUCH_USER;
  }

  // Update and save settings.
  const auto& user_profile = REX_KERNEL_STATE()->user_profile();

  for (uint32_t n = 0; n < setting_count; ++n) {
    const X_USER_PROFILE_SETTING& setting = settings[n];

    auto setting_type = static_cast<UserProfile::Setting::Type>(setting.data.type);
    if (setting_type == UserProfile::Setting::Type::UNSET) {
      continue;
    }

    REXKRNL_DEBUG(
        "XamUserWriteProfileSettings: setting index [{}]:"
        " from={} setting_id={:08X} data.type={}",
        n, (uint32_t)setting.from, (uint32_t)setting.setting_id, setting.data.type);

    switch (setting_type) {
      case UserProfile::Setting::Type::CONTENT:
      case UserProfile::Setting::Type::BINARY: {
        uint8_t* binary_ptr = REX_KERNEL_MEMORY()->TranslateVirtual(setting.data.binary.ptr);
        size_t binary_size = setting.data.binary.size;
        std::vector<uint8_t> bytes;
        if (setting.data.binary.ptr) {
          // Copy provided data
          bytes.resize(binary_size);
          std::memcpy(bytes.data(), binary_ptr, binary_size);
        } else {
          // Data pointer was NULL, so just fill with zeroes
          bytes.resize(binary_size, 0);
        }
        user_profile->AddSetting(
            std::make_unique<xam::UserProfile::BinarySetting>(setting.setting_id, bytes));
      } break;
      case UserProfile::Setting::Type::WSTRING:
      case UserProfile::Setting::Type::DOUBLE:
      case UserProfile::Setting::Type::FLOAT:
      case UserProfile::Setting::Type::INT32:
      case UserProfile::Setting::Type::INT64:
      case UserProfile::Setting::Type::DATETIME:
      default: {
        REXKRNL_ERROR("XamUserWriteProfileSettings: Unimplemented data type {}", setting_type);
      } break;
    };
  }

  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamUserCheckPrivilege_entry(u32 user_index, u32 mask, mapped_u32 out_value) {
  if (!out_value) {
    XamProfileAudit::Get().RecordPrivilege(user_index, mask, X_ERROR_INVALID_PARAMETER, false);
    return X_ERROR_INVALID_PARAMETER;
  }
  // checking all users?
  if (user_index != 0xFF) {
    if (user_index >= 4) {
      XamProfileAudit::Get().RecordPrivilege(user_index, mask, X_ERROR_INVALID_PARAMETER, false);
      return X_ERROR_INVALID_PARAMETER;
    }

    if (user_index) {
      XamProfileAudit::Get().RecordPrivilege(user_index, mask, X_ERROR_NO_SUCH_USER, false);
      return X_ERROR_NO_SUCH_USER;
    }
  }

  // If we deny everything, games should hopefully not try to do stuff.
  *out_value = 0;
  XamProfileAudit::Get().RecordPrivilege(user_index, mask, X_ERROR_SUCCESS, false);
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetFlags_entry(u32 user_index, mapped_u32 out_flags) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // No restrictions?
  *out_flags = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionGetRating_entry(u32 user_index, u32 unk1, mapped_u32 out_unk2,
                                             mapped_u32 out_unk3) {
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }

  // Some games have special case paths for 3F that differ from the failure
  // path, so my guess is that's 'don't care'.
  *out_unk2 = 0x3F;
  *out_unk3 = 0;
  return X_ERROR_SUCCESS;
}

u32 XamUserContentRestrictionCheckAccess_entry(u32 user_index, u32 unk1, u32 unk2, u32 unk3,
                                               u32 unk4, mapped_u32 out_unk5, u32 overlapped_ptr) {
  *out_unk5 = 1;

  if (overlapped_ptr) {
    // TODO(benvanik): does this need the access arg on it?
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
  }

  return X_ERROR_SUCCESS;
}

u32 XamUserIsOnlineEnabled_entry(u32 user_index) {
  return 1;
}

u32 XamUserGetMembershipTier_entry(u32 user_index) {
  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (user_index) {
    return X_ERROR_NO_SUCH_USER;
  }
  return 6 /* 6 appears to be Gold */;
}

u32 XamUserAreUsersFriends_entry(u32 user_index, u32 unk1, u32 unk2, mapped_u32 out_value,
                                 u32 overlapped_ptr) {
  uint32_t are_friends = 0;
  X_RESULT result;

  if (user_index >= 4) {
    result = X_ERROR_INVALID_PARAMETER;
  } else {
    if (user_index == 0) {
      const auto& user_profile = REX_KERNEL_STATE()->user_profile();
      if (user_profile->signin_state() == 0) {
        result = X_ERROR_NOT_LOGGED_ON;
      } else {
        // No friends!
        are_friends = 0;
        result = X_ERROR_SUCCESS;
      }
    } else {
      // Only support user 0.
      result = X_ERROR_NO_SUCH_USER;  // if user is local -> X_ERROR_NOT_LOGGED_ON
    }
  }

  if (out_value) {
    assert_true(!overlapped_ptr);
    *out_value = result == X_ERROR_SUCCESS ? are_friends : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!out_value);
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(
        overlapped_ptr, result == X_ERROR_SUCCESS ? X_ERROR_SUCCESS : X_ERROR_FUNCTION_FAILED,
        X_HRESULT_FROM_WIN32(result), result == X_ERROR_SUCCESS ? are_friends : 0);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamShowSigninUI_entry(u32 unk, u32 unk_mask) {
  // Mask values vary. Probably matching user types? Local/remote?

  // To fix game modes that display a 4 profile signin UI (even if playing
  // alone):
  // XN_SYS_SIGNINCHANGED
  REX_KERNEL_STATE()->BroadcastNotification(0x0000000A, 1);
  // Games seem to sit and loop until we trigger this notification:
  // XN_SYS_UI (off)
  REX_KERNEL_STATE()->BroadcastNotification(0x00000009, 0);
  XamProfileAudit::Get().RecordSigninUi();
  return X_ERROR_SUCCESS;
}

// TODO(gibbed): probably a FILETIME/LARGE_INTEGER, unknown currently
struct X_ACHIEVEMENT_UNLOCK_TIME {
  rex::be<uint32_t> unk_0;
  rex::be<uint32_t> unk_4;
};

struct X_ACHIEVEMENT_DETAILS {
  rex::be<uint32_t> id;
  rex::be<uint32_t> label_ptr;
  rex::be<uint32_t> description_ptr;
  rex::be<uint32_t> unachieved_ptr;
  rex::be<uint32_t> image_id;
  rex::be<uint32_t> gamerscore;
  X_ACHIEVEMENT_UNLOCK_TIME unlock_time;
  rex::be<uint32_t> flags;

  static const size_t kStringBufferSize = 464;
};
static_assert_size(X_ACHIEVEMENT_DETAILS, 36);

class XStaticAchievementEnumerator : public XEnumerator {
 public:
  struct AchievementDetails {
    uint32_t id;
    std::u16string label;
    std::u16string description;
    std::u16string unachieved;
    uint32_t image_id;
    uint32_t gamerscore;
    struct {
      uint32_t unk_0;
      uint32_t unk_4;
    } unlock_time;
    uint32_t flags;
  };

  XStaticAchievementEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                               uint32_t flags)
      : XEnumerator(kernel_state, items_per_enumerate,
                    sizeof(X_ACHIEVEMENT_DETAILS) +
                        (!!(flags & 7) ? X_ACHIEVEMENT_DETAILS::kStringBufferSize : 0)),
        flags_(flags) {}

  void AppendItem(AchievementDetails item) { items_.push_back(std::move(item)); }

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data, uint32_t* written_count) override {
    size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
    if (!count) {
      return X_ERROR_NO_MORE_FILES;
    }

    size_t size = count * item_size();

    auto details = reinterpret_cast<X_ACHIEVEMENT_DETAILS*>(buffer_data);
    size_t string_offset = items_per_enumerate() * sizeof(X_ACHIEVEMENT_DETAILS);
    auto string_buffer =
        StringBuffer{buffer_ptr + static_cast<uint32_t>(string_offset), &buffer_data[string_offset],
                     count * X_ACHIEVEMENT_DETAILS::kStringBufferSize};
    for (size_t i = 0, o = current_item_; i < count; ++i, ++current_item_) {
      const auto& item = items_[current_item_];
      details[i].id = item.id;
      details[i].label_ptr = !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
      details[i].description_ptr =
          !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
      details[i].unachieved_ptr = !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
      details[i].image_id = item.image_id;
      details[i].gamerscore = item.gamerscore;
      details[i].unlock_time.unk_0 = item.unlock_time.unk_0;
      details[i].unlock_time.unk_4 = item.unlock_time.unk_4;
      details[i].flags = item.flags;
    }

    if (written_count) {
      *written_count = static_cast<uint32_t>(count);
    }

    return X_ERROR_SUCCESS;
  }

 private:
  struct StringBuffer {
    uint32_t ptr;
    uint8_t* data;
    size_t remaining_bytes;
  };

  uint32_t AppendString(StringBuffer& sb, const std::u16string_view string) {
    size_t count = string.length() + 1;
    size_t size = count * sizeof(char16_t);
    if (size > sb.remaining_bytes) {
      assert_always();
      return 0;
    }
    auto ptr = sb.ptr;
    rex::string::copy_and_swap_truncating(reinterpret_cast<char16_t*>(sb.data), string, count);
    sb.ptr += static_cast<uint32_t>(size);
    sb.data += size;
    sb.remaining_bytes -= size;
    return ptr;
  }

 private:
  uint32_t flags_;
  std::vector<AchievementDetails> items_;
  size_t current_item_ = 0;
};

u32 XamUserCreateAchievementEnumerator_entry(u32 title_id, u32 user_index, u32 xuid, u32 flags,
                                             u32 offset, u32 count, mapped_u32 buffer_size_ptr,
                                             mapped_u32 handle_ptr) {
  if (!count || !buffer_size_ptr || !handle_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  if (user_index >= 4) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t entry_size = sizeof(X_ACHIEVEMENT_DETAILS);
  if (flags & 7) {
    entry_size += X_ACHIEVEMENT_DETAILS::kStringBufferSize;
  }

  if (buffer_size_ptr) {
    *buffer_size_ptr = static_cast<uint32_t>(entry_size) * count;
  }

  auto e = object_ref<XStaticAchievementEnumerator>(
      new XStaticAchievementEnumerator(REX_KERNEL_STATE(), count, flags));
  auto result = e->Initialize(user_index, 0xFB, 0xB000A, 0xB000B, 0);
  if (XFAILED(result)) {
    return result;
  }

  // ACHIEVED | ACHIEVED_ONLINE flags the game checks to consider an achievement earned.
  constexpr uint32_t kAchievedFlags = 0x00030000;

  auto fill_unlock = [](XStaticAchievementEnumerator::AchievementDetails& item, uint32_t id,
                        const rex::system::KernelState* ks) {
    uint64_t ft = ks->GetAchievementUnlockTime(id);
    if (ft) {
      item.flags |= kAchievedFlags;
      item.unlock_time.unk_0 = static_cast<uint32_t>(ft & 0xFFFF'FFFF);
      item.unlock_time.unk_4 = static_cast<uint32_t>(ft >> 32);
    }
  };

  const auto* ks = REX_KERNEL_STATE();

  // Prefer the runtime store (populated from TOML or XDBF at boot) so that
  // dev-edited labels/descriptions are visible to the game's own queries.
  const auto store = ks->loaded_achievements();
  if (!store.empty()) {
    for (const auto& info : store) {
      auto item = XStaticAchievementEnumerator::AchievementDetails{
          info.id,
          rex::string::to_utf16(info.label),
          rex::string::to_utf16(info.description),
          rex::string::to_utf16(info.unachieved_description),
          info.image_id,
          info.gamerscore,
          {0, 0},
          info.flags};
      fill_unlock(item, info.id, ks);
      e->AppendItem(item);
    }
  } else {
    const util::XdbfGameData db = ks->title_xdbf();
    if (db.is_valid()) {
      const XLanguage language =
          db.GetExistingLanguage(static_cast<XLanguage>(REXCVAR_GET(user_language)));
      for (const util::XdbfAchievementTableEntry& entry : db.GetAchievements()) {
        auto item = XStaticAchievementEnumerator::AchievementDetails{
            entry.id,
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.label_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.description_id)),
            rex::string::to_utf16(db.GetStringTableEntry(language, entry.unachieved_id)),
            entry.image_id,
            entry.gamerscore,
            {0, 0},
            entry.flags};
        fill_unlock(item, entry.id, ks);
        e->AppendItem(item);
      }
    }
  }

  *handle_ptr = e->handle();
  return X_ERROR_SUCCESS;
}

u32 XamParseGamerTileKey_entry(mapped_u32 key_ptr, mapped_u32 out1_ptr, mapped_u32 out2_ptr,
                               mapped_u32 out3_ptr) {
  *out1_ptr = 0xC0DE0001;
  *out2_ptr = 0xC0DE0002;
  *out3_ptr = 0xC0DE0003;
  return X_ERROR_SUCCESS;
}

u32 XamReadTileToTexture_entry(u32 unknown, u32 title_id, u64 tile_id, u32 user_index,
                               mapped_void buffer_ptr, u32 stride, u32 height, u32 overlapped_ptr) {
  // TODO(gibbed): unknown=0,2,3,9
  if (!tile_id) {
    return X_ERROR_INVALID_PARAMETER;
  }

  size_t size = size_t(stride) * size_t(height);
  std::memset(buffer_ptr, 0xFF, size);

  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamWriteGamerTile_entry(u32 arg1, u32 arg2, u32 arg3, u32 arg4, u32 arg5, u32 overlapped_ptr) {
  if (overlapped_ptr) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped_ptr, X_ERROR_SUCCESS);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_SUCCESS;
}

u32 XamSessionCreateHandle_entry(mapped_u32 handle_ptr) {
  *handle_ptr = 0xCAFEDEAD;
  return X_ERROR_SUCCESS;
}

u32 XamSessionRefObjByHandle_entry(u32 handle, mapped_u32 obj_ptr) {
  assert_true(handle == 0xCAFEDEAD);
  // TODO(PermaNull): Implement this properly,
  // For the time being returning 0xDEADF00D will prevent crashing.
  *obj_ptr = 0xDEADF00D;
  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamUserGetXUID, rex::kernel::xam::XamUserGetXUID_entry)
REX_EXPORT(__imp__XamUserGetSigninState, rex::kernel::xam::XamUserGetSigninState_entry)
REX_EXPORT(__imp__XamUserGetSigninInfo, rex::kernel::xam::XamUserGetSigninInfo_entry)
REX_EXPORT(__imp__XamUserGetName, rex::kernel::xam::XamUserGetName_entry)
REX_EXPORT(__imp__XamUserGetGamerTag, rex::kernel::xam::XamUserGetGamerTag_entry)
REX_EXPORT(__imp__XamUserReadProfileSettings, rex::kernel::xam::XamUserReadProfileSettings_entry)
REX_EXPORT(__imp__XamUserReadProfileSettingsEx,
           rex::kernel::xam::XamUserReadProfileSettingsEx_entry)
REX_EXPORT(__imp__XamUserWriteProfileSettings, rex::kernel::xam::XamUserWriteProfileSettings_entry)
REX_EXPORT(__imp__XamUserCheckPrivilege, rex::kernel::xam::XamUserCheckPrivilege_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetFlags,
           rex::kernel::xam::XamUserContentRestrictionGetFlags_entry)
REX_EXPORT(__imp__XamUserContentRestrictionGetRating,
           rex::kernel::xam::XamUserContentRestrictionGetRating_entry)
REX_EXPORT(__imp__XamUserContentRestrictionCheckAccess,
           rex::kernel::xam::XamUserContentRestrictionCheckAccess_entry)
REX_EXPORT(__imp__XamUserIsOnlineEnabled, rex::kernel::xam::XamUserIsOnlineEnabled_entry)
REX_EXPORT(__imp__XamUserGetMembershipTier, rex::kernel::xam::XamUserGetMembershipTier_entry)
REX_EXPORT(__imp__XamUserAreUsersFriends, rex::kernel::xam::XamUserAreUsersFriends_entry)
REX_EXPORT(__imp__XamShowSigninUI, rex::kernel::xam::XamShowSigninUI_entry)
REX_EXPORT(__imp__XamUserCreateAchievementEnumerator,
           rex::kernel::xam::XamUserCreateAchievementEnumerator_entry)
REX_EXPORT(__imp__XamParseGamerTileKey, rex::kernel::xam::XamParseGamerTileKey_entry)
REX_EXPORT(__imp__XamReadTileToTexture, rex::kernel::xam::XamReadTileToTexture_entry)
REX_EXPORT(__imp__XamWriteGamerTile, rex::kernel::xam::XamWriteGamerTile_entry)
REX_EXPORT(__imp__XamSessionCreateHandle, rex::kernel::xam::XamSessionCreateHandle_entry)
REX_EXPORT(__imp__XamSessionRefObjByHandle, rex::kernel::xam::XamSessionRefObjByHandle_entry)

REX_EXPORT_STUB(__imp__XamUserAddRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserAllowedToPostToSocialNetwork);
REX_EXPORT_STUB(__imp__XamUserCreateAvatarAssetEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreatePlayerEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateStatsEnumerator);
REX_EXPORT_STUB(__imp__XamUserCreateTitlesPlayedEnumerator);
REX_EXPORT_STUB(__imp__XamUserFlushLogonQueue);
REX_EXPORT_STUB(__imp__XamUserGetAge);
REX_EXPORT_STUB(__imp__XamUserGetAgeGroup);
REX_EXPORT_STUB(__imp__XamUserGetCachedUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetDeviceId);
REX_EXPORT_STUB(__imp__XamUserGetIndexFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetMembershipTierFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineCountryFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineLanguageFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetOnlineXUIDFromOfflineXUID);
REX_EXPORT_STUB(__imp__XamUserGetReportingInfo);
REX_EXPORT_STUB(__imp__XamUserGetRequestedUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserGetUserFlags);
REX_EXPORT_STUB(__imp__XamUserGetUserFlagsFromXUID);
REX_EXPORT_STUB(__imp__XamUserGetUserIndexMask);
REX_EXPORT_STUB(__imp__XamUserGetUserTenure);
REX_EXPORT_STUB(__imp__XamUserGetUsersMissingAvatars);
REX_EXPORT_STUB(__imp__XamUserGetXUIDForTFA);
REX_EXPORT_STUB(__imp__XamUserInvalidateProfileSetting);
REX_EXPORT_STUB(__imp__XamUserIsGuest);
REX_EXPORT_STUB(__imp__XamUserIsLogonPreviewModeEnabled);
REX_EXPORT_STUB(__imp__XamUserIsParentalControlled);
REX_EXPORT_STUB(__imp__XamUserIsPartial);
REX_EXPORT_STUB(__imp__XamUserIsPartialProfile);
REX_EXPORT_STUB(__imp__XamUserIsUnsafeProgrammingAllowed);
REX_EXPORT_STUB(__imp__XamUserLockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserLogon);
REX_EXPORT_STUB(__imp__XamUserLogonEx);
REX_EXPORT_STUB(__imp__XamUserLookupDevice);
REX_EXPORT_STUB(__imp__XamUserNuiBind);
REX_EXPORT_STUB(__imp__XamUserNuiEnableBiometric);
REX_EXPORT_STUB(__imp__XamUserNuiGetEnrollmentIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndex);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForBind);
REX_EXPORT_STUB(__imp__XamUserNuiGetUserIndexForSignin);
REX_EXPORT_STUB(__imp__XamUserNuiIsBiometricEnabled);
REX_EXPORT_STUB(__imp__XamUserNuiUnbind);
REX_EXPORT_STUB(__imp__XamUserOverrideBindingCallbacks);
REX_EXPORT_STUB(__imp__XamUserOverrideDeviceBindings);
REX_EXPORT_STUB(__imp__XamUserOverrideGlobalState);
REX_EXPORT_STUB(__imp__XamUserOverrideUserInfo);
REX_EXPORT_STUB(__imp__XamUserPrefetchProfileSettings);
REX_EXPORT_STUB(__imp__XamUserProfileSync);
REX_EXPORT_STUB(__imp__XamUserReadUserPreference);
REX_EXPORT_STUB(__imp__XamUserResetSubscriptionType);
REX_EXPORT_STUB(__imp__XamUserUnlockLogonPreviewMode);
REX_EXPORT_STUB(__imp__XamUserUpdateRecentPlayer);
REX_EXPORT_STUB(__imp__XamUserValidateAvatarManifest);
REX_EXPORT_STUB(__imp__XamUserWriteUserPreference);
REX_EXPORT_STUB(__imp__XamVerifyPasscode);
