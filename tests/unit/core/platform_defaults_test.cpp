// l80: per-platform cvar defaults (SelectPlatformDefaults) and the registered
// defaults of the four cvars that use them.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include <rex/content_log.h>
#include <rex/cvar.h>
#include <rex/platform_defaults.h>

using rex::PlatformDefaults;
using rex::SelectPlatformDefaults;

namespace {

std::string RegisteredDefault(std::string_view name) {
  for (const rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
    if (entry.name == name) {
      return entry.default_value;
    }
  }
  return "<missing>";
}

}  // namespace

TEST_CASE("Platform defaults: Windows", "[core][platform_defaults]") {
  constexpr PlatformDefaults d = SelectPlatformDefaults(false);
  STATIC_REQUIRE(d.timer_queue_blocking_wait);
  STATIC_REQUIRE(d.timer_queue_spin_tail_us == 200);
  STATIC_REQUIRE(d.xma_canary_fixes);
  STATIC_REQUIRE(d.d3d12_texture_placed_heaps);
  CHECK(std::string_view(d.ke_delay_short_sleep_mode) == "native");
  CHECK(std::string_view(d.apu_worker_priority) == "normal");
  CHECK(std::string_view(d.ke_delay_zero_mode) == "native");
  CHECK(std::string_view(d.guest_audio_thread_priority) == "normal");
}

TEST_CASE("Platform defaults: Wine/Proton", "[core][platform_defaults]") {
  constexpr PlatformDefaults d = SelectPlatformDefaults(true);
  STATIC_REQUIRE(d.timer_queue_blocking_wait);
  STATIC_REQUIRE(d.timer_queue_spin_tail_us == 50);
  STATIC_REQUIRE(d.xma_canary_fixes);
  STATIC_REQUIRE_FALSE(d.d3d12_texture_placed_heaps);
  CHECK(std::string_view(d.ke_delay_short_sleep_mode) == "hires_timer");
  CHECK(std::string_view(d.apu_worker_priority) == "above_normal");
  CHECK(std::string_view(d.ke_delay_zero_mode) == "backoff");
  CHECK(std::string_view(d.guest_audio_thread_priority) == "above_normal");
}

TEST_CASE("Platform defaults: current process", "[core][platform_defaults]") {
  const PlatformDefaults& d = rex::CurrentPlatformDefaults();
  const PlatformDefaults expected = SelectPlatformDefaults(rex::content_log::RunningUnderWine());
  CHECK(d.timer_queue_blocking_wait == expected.timer_queue_blocking_wait);
  CHECK(d.timer_queue_spin_tail_us == expected.timer_queue_spin_tail_us);
  CHECK(d.xma_canary_fixes == expected.xma_canary_fixes);
  CHECK(d.d3d12_texture_placed_heaps == expected.d3d12_texture_placed_heaps);
  // The cvars defined in the runtime DLL register the platform default.
  CHECK(RegisteredDefault("timer_queue_blocking_wait") ==
        (expected.timer_queue_blocking_wait ? "true" : "false"));
  CHECK(RegisteredDefault("timer_queue_spin_tail_us") ==
        std::to_string(expected.timer_queue_spin_tail_us));
  CHECK(RegisteredDefault("xma_canary_fixes") == (expected.xma_canary_fixes ? "true" : "false"));
  CHECK(RegisteredDefault("ke_delay_short_sleep_mode") == expected.ke_delay_short_sleep_mode);
  CHECK(RegisteredDefault("apu_worker_priority") == expected.apu_worker_priority);
  CHECK(RegisteredDefault("ke_delay_zero_mode") == expected.ke_delay_zero_mode);
  CHECK(RegisteredDefault("guest_audio_thread_priority") ==
        expected.guest_audio_thread_priority);
  CHECK(RegisteredDefault("guest_thread_priority_boost_start") == "823EC8C8,823EC990");
}
