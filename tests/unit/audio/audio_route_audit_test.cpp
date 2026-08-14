#include <array>
#include <bit>
#include <limits>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include <rex/audio/audio_route_audit.h>
#include <rex/types.h>

namespace {

float BigEndianFloat(float value) {
  return std::bit_cast<float>(rex::byte_swap(std::bit_cast<uint32_t>(value)));
}

}  // namespace

TEST_CASE("audio route classifies big-endian guest PCM", "[audio][route-audit]") {
  const std::array<float, 4> silence = {};
  auto result = rex::audio::ClassifyBigEndianFloatAudio(silence.data(), silence.size());
  CHECK(result.finite);
  CHECK_FALSE(result.nonzero);
  CHECK(result.peak_ppm == 0);

  const std::array<float, 4> signal = {BigEndianFloat(-0.5f), BigEndianFloat(0.25f), 0, 0};
  result = rex::audio::ClassifyBigEndianFloatAudio(signal.data(), signal.size());
  CHECK(result.finite);
  CHECK(result.nonzero);
  CHECK(result.peak_ppm == 31250);
}

TEST_CASE("audio route rejects invalid float data", "[audio][route-audit]") {
  const std::array<float, 2> signal = {0.0f, std::numeric_limits<float>::infinity()};
  const auto result = rex::audio::ClassifyNativeFloatAudio(signal.data(), signal.size());
  CHECK_FALSE(result.finite);
  CHECK_FALSE(result.nonzero);
}

TEST_CASE("audio route classifies big-endian XMA PCM", "[audio][route-audit]") {
  const std::array<uint8_t, 6> signal = {0x00, 0x00, 0x40, 0x00, 0x80, 0x00};
  const auto result = rex::audio::ClassifyBigEndianS16Audio(signal.data(), signal.size());
  CHECK(result.finite);
  CHECK(result.nonzero);
  CHECK(result.peak_ppm == 1000000);
}

TEST_CASE("audio route rejects malformed buffers", "[audio][route-audit]") {
  CHECK_FALSE(rex::audio::ClassifyNativeFloatAudio(nullptr, 1).finite);
  const uint8_t odd[] = {0};
  CHECK_FALSE(rex::audio::ClassifyBigEndianS16Audio(odd, 1).finite);
}

TEST_CASE("audio route bounds queue growth and consecutive starvation", "[audio][route-audit]") {
  CHECK(rex::audio::IsAudioRouteBufferHealthBounded(64, 2));
  CHECK_FALSE(rex::audio::IsAudioRouteBufferHealthBounded(65, 2));
  CHECK_FALSE(rex::audio::IsAudioRouteBufferHealthBounded(64, 3));
}

TEST_CASE("audio event phases use a fixed privacy-safe vocabulary", "[audio][event-audit]") {
  CHECK(std::string_view(rex::audio::AudioEventPhaseName(rex::audio::AudioEventPhase::kMusic)) ==
        "music");
  CHECK(std::string_view(rex::audio::AudioEventPhaseName(
            rex::audio::AudioEventPhase::kCollision)) == "collision");
  CHECK(std::string_view(rex::audio::AudioEventPhaseName(rex::audio::AudioEventPhase::kUi)) ==
        "ui");
  CHECK(std::string_view(rex::audio::AudioEventPhaseName(rex::audio::AudioEventPhase::kCount)) ==
        "invalid");
}

TEST_CASE("audio event phases reject reordered windows", "[audio][event-audit]") {
  CHECK(rex::audio::IsAudioEventPhaseOrderValid(0, rex::audio::AudioEventPhase::kMusic));
  CHECK(rex::audio::IsAudioEventPhaseOrderValid(5, rex::audio::AudioEventPhase::kUi));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseOrderValid(0, rex::audio::AudioEventPhase::kVoice));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseOrderValid(6, rex::audio::AudioEventPhase::kCount));
}

TEST_CASE("audio event phases require finite nonzero submitted output", "[audio][event-audit]") {
  CHECK(rex::audio::IsAudioEventPhaseHealthy(100, 90, 0, 0));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseHealthy(0, 0, 0, 0));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseHealthy(100, 0, 0, 0));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseHealthy(100, 101, 0, 0));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseHealthy(100, 90, 1, 0));
  CHECK_FALSE(rex::audio::IsAudioEventPhaseHealthy(100, 90, 0, 1));
}
