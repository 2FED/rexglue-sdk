// l80 (KI-060): short guest delay planning (ke_delay_short_sleep_mode), the
// APU worker priority names, the MCLA_SLEEP minute line and a live check of
// rex::thread::HighResolutionSleep.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <rex/system/short_sleep.h>
#include <rex/thread.h>

using rex::system::ApuWorkerPriority;
using rex::system::FormatShortSleepMinute;
using rex::system::IsShortSleep;
using rex::system::ParseApuWorkerPriority;
using rex::system::ParseShortSleepMode;
using rex::system::PlanShortSleep;
using rex::system::RelativeDelayTicks;
using rex::system::ShortSleepAction;
using rex::system::ShortSleepMinuteStats;
using rex::system::ShortSleepMode;

TEST_CASE("Short sleep: mode names", "[system][short_sleep]") {
  CHECK(ParseShortSleepMode("native") == ShortSleepMode::kNative);
  CHECK(ParseShortSleepMode("yield") == ShortSleepMode::kYield);
  CHECK(ParseShortSleepMode("hires_timer") == ShortSleepMode::kHiresTimer);
  CHECK_FALSE(ParseShortSleepMode("hires").has_value());
  CHECK_FALSE(ParseShortSleepMode("").has_value());
  CHECK(std::string(rex::system::ShortSleepModeName(ShortSleepMode::kHiresTimer)) == "hires_timer");
}

TEST_CASE("Short sleep: relative ticks and the 2 ms limit", "[system][short_sleep]") {
  STATIC_REQUIRE(RelativeDelayTicks(-10000) == 10000);
  STATIC_REQUIRE(RelativeDelayTicks(0) == 0);
  STATIC_REQUIRE(RelativeDelayTicks(123456789) == -1);  // absolute time
  STATIC_REQUIRE(IsShortSleep(0));
  STATIC_REQUIRE(IsShortSleep(19999));
  STATIC_REQUIRE_FALSE(IsShortSleep(20000));
  STATIC_REQUIRE_FALSE(IsShortSleep(-1));
}

TEST_CASE("Short sleep: plan per mode", "[system][short_sleep]") {
  // native: always the old path.
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kNative, 0) == ShortSleepAction::kNative);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kNative, 5000) == ShortSleepAction::kNative);
  // Long or absolute delays: native in every mode.
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kYield, 20000) == ShortSleepAction::kNative);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, 20000) == ShortSleepAction::kNative);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, -1) == ShortSleepAction::kNative);
  // yield: every short delay, including zero.
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kYield, 0) == ShortSleepAction::kYield);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kYield, 15000) == ShortSleepAction::kYield);
  // hires_timer: zero stays native, 1 tick .. 2 ms on the timer.
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, 0) == ShortSleepAction::kNative);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, 1) == ShortSleepAction::kHiresTimer);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, 10000) ==
                 ShortSleepAction::kHiresTimer);
  STATIC_REQUIRE(PlanShortSleep(ShortSleepMode::kHiresTimer, 19999) ==
                 ShortSleepAction::kHiresTimer);
}

TEST_CASE("Short sleep: APU worker priority names", "[system][short_sleep]") {
  CHECK(ParseApuWorkerPriority("normal") == ApuWorkerPriority::kNormal);
  CHECK(ParseApuWorkerPriority("above_normal") == ApuWorkerPriority::kAboveNormal);
  CHECK(ParseApuWorkerPriority("highest") == ApuWorkerPriority::kHighest);
  CHECK_FALSE(ParseApuWorkerPriority("realtime").has_value());
  CHECK(std::string(rex::system::ApuWorkerPriorityName(ApuWorkerPriority::kAboveNormal)) ==
        "above_normal");
}

TEST_CASE("Short sleep: MCLA_SLEEP minute line", "[system][short_sleep]") {
  ShortSleepMinuteStats s;
  CHECK(FormatShortSleepMinute(s, ShortSleepMode::kNative) ==
        "MCLA_SLEEP v=1 kind=minute short_sleeps=0 mode=native avg_us=0 max_us=0 req_avg_us=0 "
        "zero=0 alertable=0 alerted=0 yields=0 timers=0 fallbacks=0 delays=0 "
        "top_thread=00000000 top_short=0");
  s.delays = 12;
  s.short_sleeps = 4;
  s.zero = 1;
  s.alertable = 3;
  s.alerted = 1;
  s.timers = 3;
  s.requested_us_total = 3000;
  s.slept_us_total = 4400;
  s.slept_us_max = 1500;
  s.top_thread_id = 0xF800068C;
  s.top_thread_short = 4;
  CHECK(FormatShortSleepMinute(s, ShortSleepMode::kHiresTimer) ==
        "MCLA_SLEEP v=1 kind=minute short_sleeps=4 mode=hires_timer avg_us=1100 max_us=1500 "
        "req_avg_us=750 zero=1 alertable=3 alerted=1 yields=0 timers=3 fallbacks=0 delays=12 "
        "top_thread=F800068C top_short=4");
}

TEST_CASE("Short sleep: high-resolution sleep is never early", "[system][short_sleep]") {
  using clock = std::chrono::steady_clock;
  for (int i = 0; i < 20; ++i) {
    rex::thread::SleepResult result = rex::thread::SleepResult::kAlerted;
    const auto start = clock::now();
    const bool ok =
        rex::thread::HighResolutionSleep(std::chrono::microseconds(500), false, &result);
    const auto elapsed = clock::now() - start;
    if (!ok) {
      SKIP("no high-resolution waitable timer on this system");
    }
    CHECK(result == rex::thread::SleepResult::kSuccess);
    // Allow 50 us of clock granularity; never more than 200 ms late.
    CHECK(elapsed >= std::chrono::microseconds(450));
    CHECK(elapsed < std::chrono::milliseconds(200));
  }
}
