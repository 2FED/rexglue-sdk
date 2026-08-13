/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe locale selection audit                      *
 ******************************************************************************
 */

#include <mutex>
#include <string_view>

#include <rex/kernel/xam/locale_audit.h>
#include <rex/logging.h>
#include <rex/system/flags.h>

REXCVAR_DEFINE_BOOL(xam_locale_audit, false, "XAM",
                    "Emit bounded privacy-safe locale selection evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::kernel::xam {
namespace {

constexpr uint32_t kEnglish = 1;
constexpr uint32_t kMaxLanguage = 12;
constexpr uint32_t kMaxCountry = 111;
constexpr uint32_t kSuccess = 0;

class LocaleAudit {
 public:
  void RecordLanguage(uint32_t result) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || summary_) {
      return;
    }
    ++xget_calls_;
    const bool matches = result == language_;
    xget_matches_ += matches;
    mismatches_ += !matches;
    if (!xget_recorded_) {
      REXKRNL_INFO("LOCALE_AUDIT_XGET v=1 language={} result={} match={}", language_, result,
                   matches ? 1 : 0);
      xget_recorded_ = true;
      ++records_;
    }
  }

  void RecordXConfig(uint16_t category, uint16_t setting, uint32_t result, bool value_present,
                     uint32_t value) {
    if (category != 3 || (setting != 9 && setting != 14)) {
      return;
    }
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || summary_) {
      return;
    }
    const bool is_language = setting == 9;
    uint64_t& calls = is_language ? xconfig_language_calls_ : xconfig_country_calls_;
    uint64_t& value_calls =
        is_language ? xconfig_language_value_calls_ : xconfig_country_value_calls_;
    bool& recorded = is_language ? xconfig_language_recorded_ : xconfig_country_recorded_;
    ++calls;
    const bool success = result == kSuccess;
    xconfig_failures_ += !success;
    if (success && value_present) {
      ++value_calls;
      const uint32_t expected = is_language ? language_ : country_;
      mismatches_ += value != expected;
    }
    if (!recorded && success && value_present) {
      REXKRNL_INFO("LOCALE_AUDIT_XCONFIG v=1 setting={} value={} result={:08X} match={}",
                   is_language ? "language" : "country", value, result,
                   value == (is_language ? language_ : country_) ? 1 : 0);
      recorded = true;
      ++records_;
    }
  }

  void Summary(const char* phase) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || summary_) {
      return;
    }
    summary_ = true;
    const bool valid_phase = phase && std::string_view(phase) == "title";
    const bool pass = valid_phase && language_valid_ && country_valid_ &&
                      xconfig_language_value_calls_ != 0 && xget_matches_ == xget_calls_ &&
                      xconfig_failures_ == 0 && mismatches_ == 0;
    REXKRNL_INFO(
        "LOCALE_AUDIT_SUMMARY v=1 phase={} status={} language={} country={} "
        "xget_reach={} xget_calls={} xget_matches={} xconfig_language_calls={} "
        "xconfig_language_value_calls={} xconfig_country_reach={} xconfig_country_calls={} "
        "xconfig_country_value_calls={} xconfig_failures={} mismatches={} records={} "
        "overflow=0 dropped_records=0",
        valid_phase ? "title" : "invalid", pass ? "PASS" : "FAIL", language_, country_,
        xget_calls_ ? "reached" : "unreached", xget_calls_, xget_matches_, xconfig_language_calls_,
        xconfig_language_value_calls_, xconfig_country_calls_ ? "reached" : "unreached",
        xconfig_country_calls_, xconfig_country_value_calls_, xconfig_failures_, mismatches_,
        records_);
  }

 private:
  bool BeginLocked() {
    if (!enabled_) {
      return false;
    }
    if (!configured_) {
      REXKRNL_INFO(
          "LOCALE_AUDIT_CONFIG v=1 enabled=1 language={} country={} "
          "language_valid={} country_valid={} record_limit=3",
          language_, country_, language_valid_ ? 1 : 0, country_valid_ ? 1 : 0);
      configured_ = true;
    }
    return true;
  }

  std::mutex mutex_;
  const bool enabled_ = REXCVAR_GET(xam_locale_audit);
  const uint32_t requested_language_ = REXCVAR_GET(user_language);
  const uint32_t language_ = NormalizeUserLanguage(requested_language_);
  const uint32_t country_ = REXCVAR_GET(user_country);
  const bool language_valid_ =
      requested_language_ >= kEnglish && requested_language_ <= kMaxLanguage;
  const bool country_valid_ = IsUserCountryValid(country_);
  bool configured_ = false;
  bool summary_ = false;
  bool xget_recorded_ = false;
  bool xconfig_language_recorded_ = false;
  bool xconfig_country_recorded_ = false;
  uint64_t xget_calls_ = 0;
  uint64_t xget_matches_ = 0;
  uint64_t xconfig_language_calls_ = 0;
  uint64_t xconfig_language_value_calls_ = 0;
  uint64_t xconfig_country_calls_ = 0;
  uint64_t xconfig_country_value_calls_ = 0;
  uint64_t xconfig_failures_ = 0;
  uint64_t mismatches_ = 0;
  uint32_t records_ = 0;
};

LocaleAudit& GetLocaleAudit() {
  static LocaleAudit audit;
  return audit;
}

}  // namespace

uint32_t NormalizeUserLanguage(uint32_t language) {
  return language >= kEnglish && language <= kMaxLanguage ? language : kEnglish;
}

bool IsUserCountryValid(uint32_t country) {
  return country <= kMaxCountry;
}

void RecordXGetLanguage(uint32_t result) {
  GetLocaleAudit().RecordLanguage(result);
}

void RecordLocaleXConfig(uint16_t category, uint16_t setting, uint32_t result, bool value_present,
                         uint32_t value) {
  GetLocaleAudit().RecordXConfig(category, setting, result, value_present, value);
}

void EmitLocaleAuditSummary(const char* phase) {
  GetLocaleAudit().Summary(phase);
}

}  // namespace rex::kernel::xam
