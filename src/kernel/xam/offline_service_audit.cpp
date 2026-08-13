/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe offline-service audit                       *
 ******************************************************************************
 */

#include <array>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include <rex/kernel/xam/offline_service_audit.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(xam_offline_service_audit, false, "XAM",
                    "Emit bounded privacy-safe offline-service evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(xam_offline_network_block, false, "XAM",
                    "Block guest socket creation for offline-route validation")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::xam {
namespace {

constexpr uint32_t kSuccess = 0;
constexpr uint32_t kFail = 0x80004005;
constexpr uint32_t kConnectionInvalid = 0x80151802;
constexpr std::array<uint32_t, 10> kKnownMessages = {
    0x00058004, 0x00058006, 0x00058007, 0x00058009, 0x0005800E,
    0x00058020, 0x00058023, 0x00058035, 0x00058037, 0x00058046,
};

int KnownIndex(uint32_t message) {
  for (size_t i = 0; i < kKnownMessages.size(); ++i) {
    if (kKnownMessages[i] == message) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

class OfflineServiceAudit {
 public:
  bool BeginMessage() {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || !accepting_) {
      return false;
    }
    ++active_calls_;
    return true;
  }

  void EndMessage(bool audited, uint32_t message, uint32_t result) {
    if (!audited) {
      return;
    }
    std::lock_guard lock(mutex_);
    ++message_calls_;
    const bool known = IsOfflineServiceKnownMessage(message);
    const bool expected = IsOfflineServiceExpectedResult(message, result);
    known_calls_ += known;
    local_calls_ += known && IsOfflineServiceLocalMessage(message);
    unavailable_calls_ += known && !IsOfflineServiceLocalMessage(message);
    unexpected_calls_ += !known;
    unexpected_results_ += !expected;

    const int index = KnownIndex(message);
    const bool first_known = index >= 0 && !seen_[index];
    if (index >= 0) {
      seen_[index] = true;
    }
    const bool should_record = first_known || index < 0;
    if (should_record && records_ < kRecordLimit) {
      REXKRNL_INFO(
          "OFFLINE_SERVICE_AUDIT_MESSAGE v=1 id={} msg={:08X} class={} "
          "result={:08X} expected={}",
          records_, message,
          known ? (IsOfflineServiceLocalMessage(message) ? "local" : "unavailable") : "unexpected",
          result, expected ? 1 : 0);
      ++records_;
    } else if (should_record) {
      overflow_ = true;
    }
    --active_calls_;
    condition_.notify_all();
  }

  bool ShouldBlockSocket() const { return network_block_; }

  void RecordSocket(bool blocked) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || !accepting_) {
      return;
    }
    ++socket_attempts_;
    blocked_socket_attempts_ += blocked;
    host_socket_attempts_ += !blocked;
    if (!network_recorded_) {
      REXKRNL_INFO("OFFLINE_SERVICE_AUDIT_NETWORK v=1 class=guest-socket blocked={}",
                   blocked ? 1 : 0);
      network_recorded_ = true;
      ++records_;
    }
  }

  void Summary(const char* phase) {
    std::unique_lock lock(mutex_);
    if (!BeginLocked() || summary_) {
      return;
    }
    accepting_ = false;
    condition_.wait(lock, [this] { return active_calls_ == 0; });
    summary_ = true;
    enabled_ = false;
    const bool valid_phase = phase && std::string_view(phase) == "title";
    const bool pass = valid_phase && known_calls_ == message_calls_ && unexpected_results_ == 0 &&
                      network_block_ && host_socket_attempts_ == 0 && !overflow_;
    REXKRNL_INFO(
        "OFFLINE_SERVICE_AUDIT_SUMMARY v=1 phase={} status={} message_calls={} "
        "known_calls={} local_calls={} unavailable_calls={} unexpected_calls={} "
        "unexpected_results={} socket_attempts={} blocked_socket_attempts={} "
        "host_socket_attempts={} records={} overflow={} dropped_records=0",
        valid_phase ? "title" : "invalid", pass ? "PASS" : "FAIL", message_calls_, known_calls_,
        local_calls_, unavailable_calls_, unexpected_calls_, unexpected_results_, socket_attempts_,
        blocked_socket_attempts_, host_socket_attempts_, records_, overflow_ ? 1 : 0);
  }

 private:
  bool BeginLocked() {
    if (!enabled_) {
      return false;
    }
    if (!configured_) {
      REXKRNL_INFO(
          "OFFLINE_SERVICE_AUDIT_CONFIG v=1 enabled=1 policy=explicit-offline "
          "network_block={} record_limit={}",
          network_block_ ? 1 : 0, kRecordLimit);
      configured_ = true;
    }
    return true;
  }

  static constexpr uint32_t kRecordLimit = 64;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool enabled_ = REXCVAR_GET(xam_offline_service_audit);
  const bool network_block_ = REXCVAR_GET(xam_offline_network_block);
  bool configured_ = false;
  bool accepting_ = true;
  bool summary_ = false;
  bool overflow_ = false;
  bool network_recorded_ = false;
  std::array<bool, kKnownMessages.size()> seen_{};
  uint64_t active_calls_ = 0;
  uint64_t message_calls_ = 0;
  uint64_t known_calls_ = 0;
  uint64_t local_calls_ = 0;
  uint64_t unavailable_calls_ = 0;
  uint64_t unexpected_calls_ = 0;
  uint64_t unexpected_results_ = 0;
  uint64_t socket_attempts_ = 0;
  uint64_t blocked_socket_attempts_ = 0;
  uint64_t host_socket_attempts_ = 0;
  uint32_t records_ = 0;
};

OfflineServiceAudit& GetOfflineServiceAudit() {
  static OfflineServiceAudit audit;
  return audit;
}

}  // namespace

bool IsOfflineServiceKnownMessage(uint32_t message) {
  return KnownIndex(message) >= 0;
}

bool IsOfflineServiceLocalMessage(uint32_t message) {
  return message == 0x00058004 || message == 0x00058006 || message == 0x00058037 ||
         message == 0x00058046;
}

bool IsOfflineServiceExpectedResult(uint32_t message, uint32_t result) {
  if (!IsOfflineServiceKnownMessage(message)) {
    return false;
  }
  if (IsOfflineServiceLocalMessage(message)) {
    return result == kSuccess;
  }
  if (message == 0x00058007) {
    return result == kConnectionInvalid;
  }
  return result == kFail;
}

bool BeginOfflineServiceMessage() {
  return GetOfflineServiceAudit().BeginMessage();
}

void EndOfflineServiceMessage(bool audited, uint32_t message, uint32_t result) {
  GetOfflineServiceAudit().EndMessage(audited, message, result);
}

bool ShouldBlockOfflineGuestSocket() {
  return GetOfflineServiceAudit().ShouldBlockSocket();
}

void RecordOfflineGuestSocketAttempt(bool blocked) {
  GetOfflineServiceAudit().RecordSocket(blocked);
}

void EmitOfflineServiceAuditSummary(const char* phase) {
  GetOfflineServiceAudit().Summary(phase);
}

}  // namespace rex::kernel::xam
