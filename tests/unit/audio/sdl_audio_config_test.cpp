#include <array>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/flags.h>
#include <rex/audio/sdl/audio_config.h>
#include <rex/cvar.h>

namespace rex::audio::sdl {

TEST_CASE("SDL audio config resolves an exact playback device name", "[audio][config]") {
  constexpr std::array devices = {
      PlaybackDeviceConfigEntry{7, "Speakers"},
      PlaybackDeviceConfigEntry{11, "Headphones"},
  };

  CHECK(FindPlaybackDeviceByName("Headphones", devices) == 11);
  CHECK(FindPlaybackDeviceByName("headphones", devices) == 0);
  CHECK(FindPlaybackDeviceByName("Missing", devices) == 0);
}

TEST_CASE("SDL audio config applies host output volume", "[audio][config]") {
  std::array<float, 4> samples = {-1.0f, -0.5f, 0.5f, 1.0f};
  ApplyOutputVolume(samples.data(), samples.size(), 0.5f);

  CHECK(samples[0] == -0.5f);
  CHECK(samples[1] == -0.25f);
  CHECK(samples[2] == 0.25f);
  CHECK(samples[3] == 0.5f);

  ApplyOutputVolume(samples.data(), samples.size(), 0.0f);
  for (float sample : samples) {
    CHECK(sample == 0.0f);
  }
}

TEST_CASE("SDL audio host CVars expose safe defaults and reject bad volume", "[audio][config]") {
  const rex::cvar::FlagEntry* device = rex::cvar::GetFlagInfo("audio_device");
  const rex::cvar::FlagEntry* volume = rex::cvar::GetFlagInfo("audio_volume");
  REQUIRE(device);
  REQUIRE(volume);
  CHECK(device->default_value == "default");
  CHECK(device->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(volume->default_value == "1.000000");
  CHECK(volume->constraints.min == 0.0);
  CHECK(volume->constraints.max == 1.0);
  CHECK(volume->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);

  rex::cvar::ResetToDefault("audio_volume");
  CHECK(rex::cvar::SetFlagByName("audio_volume", "0.25"));
  CHECK(REXCVAR_GET(audio_volume) == 0.25);
  CHECK_FALSE(rex::cvar::SetFlagByName("audio_volume", "-0.01"));
  CHECK(REXCVAR_GET(audio_volume) == 0.25);
  CHECK_FALSE(rex::cvar::SetFlagByName("audio_volume", "1.01"));
  CHECK(REXCVAR_GET(audio_volume) == 0.25);
  CHECK_FALSE(rex::cvar::SetFlagByName("audio_volume", "loud"));
  CHECK(REXCVAR_GET(audio_volume) == 0.25);
  rex::cvar::ResetToDefault("audio_volume");
}

}  // namespace rex::audio::sdl
