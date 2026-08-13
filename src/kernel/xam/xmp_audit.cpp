/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe XMP route audit                             *
 ******************************************************************************
 */

#include <array>
#include <condition_variable>
#include <mutex>
#include <string_view>

#include <rex/kernel/xam/xmp_audit.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(xmp_route_audit, false, "XAM",
                    "Emit bounded privacy-safe XMP state-transition evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::xam {
namespace {

constexpr std::array<uint32_t, 20> kKnownMessages = {
    0x00070002, 0x00070003, 0x00070004, 0x00070005, 0x00070006, 0x00070007, 0x00070008,
    0x00070009, 0x0007000B, 0x0007000C, 0x0007000D, 0x0007000E, 0x00070013, 0x0007001A,
    0x0007001B, 0x00070025, 0x00070029, 0x0007002E, 0x0007003B, 0x0007003D,
};

int KnownIndex(uint32_t value) {
  for (size_t i = 0; i < kKnownMessages.size(); ++i) {
    if (kKnownMessages[i] == value)
      return static_cast<int>(i);
  }
  return -1;
}

class XmpRouteAudit {
 public:
  bool BeginMessage() {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || !accepting_)
      return false;
    ++active_calls_;
    return true;
  }

  void EndMessage(bool audited, uint32_t message, uint32_t result, const XmpFallbackState& before,
                  const XmpFallbackState& after) {
    if (!audited)
      return;
    std::lock_guard lock(mutex_);
    ++calls_;
    const bool known = IsXmpKnownMessage(message);
    const bool consistent = IsXmpStateConsistent(after);
    known_calls_ += known;
    query_calls_ += IsXmpQueryMessage(message);
    playback_calls_ += IsXmpPlaybackMessage(message);
    unsupported_calls_ += IsXmpPlaybackMessage(message) && result != 0;
    unexpected_calls_ += !known;
    inconsistent_calls_ += !consistent;
    state_changes_ += before.state != after.state;
    const int known_index = KnownIndex(message);
    const bool first_known = known_index >= 0 && !seen_known_[known_index];
    if (known_index >= 0)
      seen_known_[known_index] = true;
    const bool should_record = first_known || known_index < 0;
    if (should_record && record_count_ < kRecordLimit) {
      REXKRNL_INFO(
          "XMP_AUDIT_MESSAGE v=1 id={} msg={:08X} class={} result={:08X} "
          "before={} after={} client={} playlists={} active_songs={} "
          "decoder={} consistent={}",
          record_count_, message,
          IsXmpPlaybackMessage(message) ? "playback"
          : IsXmpQueryMessage(message)  ? "query"
          : known                       ? "metadata"
                                        : "unknown",
          result, before.state, after.state, after.playback_client, after.playlist_count,
          after.active_song_count, after.decoder_available ? 1 : 0, consistent ? 1 : 0);
      ++record_count_;
    } else if (should_record) {
      overflow_ = true;
    }
    --active_calls_;
    condition_.notify_all();
  }

  void Summary(const char* phase) {
    std::unique_lock lock(mutex_);
    if (!BeginLocked() || summary_)
      return;
    accepting_ = false;
    condition_.wait(lock, [this] { return active_calls_ == 0; });
    summary_ = true;
    enabled_ = false;
    const bool valid_phase = phase && std::string_view(phase) == "title";
    const bool pass = valid_phase && calls_ > 0 && query_calls_ > 0 && unexpected_calls_ == 0 &&
                      inconsistent_calls_ == 0 && !overflow_;
    REXKRNL_INFO(
        "XMP_AUDIT_SUMMARY v=1 phase={} status={} calls={} known_calls={} "
        "query_calls={} playback_calls={} unsupported_calls={} "
        "state_changes={} unexpected_calls={} inconsistent_calls={} "
        "records={} overflow={} dropped_records=0",
        valid_phase ? "title" : "invalid", pass ? "PASS" : "FAIL", calls_, known_calls_,
        query_calls_, playback_calls_, unsupported_calls_, state_changes_, unexpected_calls_,
        inconsistent_calls_, record_count_, overflow_ ? 1 : 0);
  }

 private:
  bool BeginLocked() {
    if (!enabled_)
      return false;
    if (!configured_) {
      REXKRNL_INFO(
          "XMP_AUDIT_CONFIG v=1 enabled=1 policy=metadata-only-fallback "
          "decoder=0 record_limit={}",
          kRecordLimit);
      configured_ = true;
    }
    return true;
  }

  static constexpr uint32_t kRecordLimit = 128;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool enabled_ = REXCVAR_GET(xmp_route_audit);
  bool configured_ = false;
  bool summary_ = false;
  bool accepting_ = true;
  bool overflow_ = false;
  std::array<bool, kKnownMessages.size()> seen_known_{};
  uint64_t calls_ = 0, known_calls_ = 0, query_calls_ = 0;
  uint64_t playback_calls_ = 0, unsupported_calls_ = 0, state_changes_ = 0;
  uint64_t unexpected_calls_ = 0, inconsistent_calls_ = 0;
  uint32_t record_count_ = 0;
  uint64_t active_calls_ = 0;
};

XmpRouteAudit& GetXmpRouteAudit() {
  static XmpRouteAudit audit;
  return audit;
}

}  // namespace

bool IsXmpKnownMessage(uint32_t message) {
  return KnownIndex(message) >= 0;
}

bool IsXmpPlaybackMessage(uint32_t message) {
  return (message >= 0x00070002 && message <= 0x00070007) || message == 0x0007003D;
}

bool IsXmpQueryMessage(uint32_t message) {
  return message == 0x00070009 || message == 0x0007000B || message == 0x0007000E ||
         message == 0x0007001B || message == 0x00070025 || message == 0x00070029 ||
         message == 0x0007002E || message == 0x0007003B;
}

bool IsXmpStateConsistent(const XmpFallbackState& state) {
  if (state.state > 2 || state.playback_client > 1)
    return false;
  if (!state.decoder_available && state.state != 0)
    return false;
  if (!state.active_song_count && state.state != 0)
    return false;
  return true;
}

bool BeginXmpRouteMessage() {
  return GetXmpRouteAudit().BeginMessage();
}

void EndXmpRouteMessage(bool audited, uint32_t message, uint32_t result,
                        const XmpFallbackState& before, const XmpFallbackState& after) {
  GetXmpRouteAudit().EndMessage(audited, message, result, before, after);
}

void EmitXmpRouteAuditSummary(const char* phase) {
  GetXmpRouteAudit().Summary(phase);
}

}  // namespace rex::kernel::xam
