#include <array>
#include <cmath>
#include <cstddef>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/audio/channel_rms_probe.h>
#include <rex/audio/conversion.h>
#include <rex/audio/flags.h>
#include <rex/audio/speaker_config.h>
#include <rex/cvar.h>

namespace rex::audio {
namespace {

constexpr size_t kChannelSamples = 8;
constexpr size_t kFl = 0, kFr = 1, kFc = 2, kLfe = 3, kBl = 4, kBr = 5;

using PlanarFrame = std::array<float, 6 * kChannelSamples>;

// Guest frame layout: planar big-endian, FL FR FC LFE BL BR.
PlanarFrame MakeFrame(const std::array<float, 6>& channel_values) {
  PlanarFrame frame = {};
  for (size_t channel = 0; channel < 6; ++channel) {
    for (size_t i = 0; i < kChannelSamples; ++i) {
      // Vary per sample so a sample-order mistake is visible.
      const float value = channel_values[channel] * static_cast<float>(i + 1) / 8.0f;
      frame[channel * kChannelSamples + i] = rex::byte_swap(value);
    }
  }
  return frame;
}

using DownmixFn = void (*)(float*, const float*, size_t, const conversion::StereoDownmixWeights&);

void CheckDownmix(DownmixFn downmix, const conversion::StereoDownmixWeights& weights) {
  const std::array<float, 6> values = {0.11f, 0.23f, 0.37f, 0.97f, 0.41f, 0.53f};
  const PlanarFrame frame = MakeFrame(values);
  std::array<float, 2 * kChannelSamples> out = {};
  downmix(out.data(), frame.data(), kChannelSamples, weights);
  for (size_t i = 0; i < kChannelSamples; ++i) {
    const float scale = static_cast<float>(i + 1) / 8.0f;
    const float fl = values[kFl] * scale, fr = values[kFr] * scale, fc = values[kFc] * scale;
    const float bl = values[kBl] * scale, br = values[kBr] * scale;
    // LFE is dropped; BL feeds left and BR feeds right.
    CHECK(out[i * 2] ==
          Catch::Approx(weights.front * fl + weights.center * fc + weights.surround * bl)
              .margin(1e-6));
    CHECK(out[i * 2 + 1] ==
          Catch::Approx(weights.front * fr + weights.center * fc + weights.surround * br)
              .margin(1e-6));
  }
}

}  // namespace

TEST_CASE("Stereo downmix weights are ITU Lo/Ro, normalised", "[audio][downmix]") {
  const auto& itu = conversion::kItuStereoDownmix;
  CHECK(itu.center / itu.front == Catch::Approx(0.70710678).margin(1e-6));
  CHECK(itu.surround / itu.front == Catch::Approx(0.70710678).margin(1e-6));
  CHECK(itu.front + itu.center + itu.surround == Catch::Approx(1.0).margin(1e-6));
  const auto& legacy = conversion::kLegacyStereoDownmix;
  CHECK(legacy.front == 0.4f);
  CHECK(legacy.center == 0.2f);
  CHECK(legacy.surround == 0.4f);
}

TEST_CASE("Scalar stereo downmix uses weights and channel order", "[audio][downmix]") {
  CheckDownmix(&conversion::sequential_6_BE_to_interleaved_2_LE_scalar,
               conversion::kItuStereoDownmix);
  CheckDownmix(&conversion::sequential_6_BE_to_interleaved_2_LE_scalar,
               conversion::kLegacyStereoDownmix);
}

TEST_CASE("Dispatched stereo downmix uses weights and channel order", "[audio][downmix]") {
  CheckDownmix(&conversion::sequential_6_BE_to_interleaved_2_LE, conversion::kItuStereoDownmix);
  CheckDownmix(&conversion::sequential_6_BE_to_interleaved_2_LE, conversion::kLegacyStereoDownmix);
}

TEST_CASE("Stereo downmix of full-scale input does not clip", "[audio][downmix]") {
  const PlanarFrame frame = MakeFrame({1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});  // last sample = 1.0
  std::array<float, 2 * kChannelSamples> simd = {};
  std::array<float, 2 * kChannelSamples> scalar = {};
  conversion::sequential_6_BE_to_interleaved_2_LE(simd.data(), frame.data(), kChannelSamples,
                                                  conversion::kItuStereoDownmix);
  conversion::sequential_6_BE_to_interleaved_2_LE_scalar(
      scalar.data(), frame.data(), kChannelSamples, conversion::kItuStereoDownmix);
  for (size_t i = 0; i < simd.size(); ++i) {
    CHECK(std::fabs(simd[i]) <= 1.0f + 1e-6f);
    CHECK(simd[i] == Catch::Approx(scalar[i]).margin(1e-6));
  }
  CHECK(simd[2 * kChannelSamples - 1] == Catch::Approx(1.0).margin(1e-6));
}

TEST_CASE("6-channel interleave keeps channel order on both paths", "[audio][downmix]") {
  const PlanarFrame frame = MakeFrame({0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
  std::array<float, 6 * kChannelSamples> simd = {};
  std::array<float, 6 * kChannelSamples> scalar = {};
  conversion::sequential_6_BE_to_interleaved_6_LE(simd.data(), frame.data(), kChannelSamples);
  conversion::sequential_6_BE_to_interleaved_6_LE_scalar(scalar.data(), frame.data(),
                                                         kChannelSamples);
  for (size_t i = 0; i < kChannelSamples; ++i) {
    for (size_t channel = 0; channel < 6; ++channel) {
      const float expected = rex::byte_swap(frame[channel * kChannelSamples + i]);
      CHECK(simd[i * 6 + channel] == expected);
      CHECK(scalar[i * 6 + channel] == expected);
    }
  }
}

TEST_CASE("Speaker config answer per audio_speaker_config value", "[audio][speaker]") {
  SpeakerConfigMode mode = SpeakerConfigMode::kSurround;
  REQUIRE(ParseSpeakerConfigMode("auto", &mode));
  CHECK(mode == SpeakerConfigMode::kAuto);
  REQUIRE(ParseSpeakerConfigMode("stereo", &mode));
  CHECK(mode == SpeakerConfigMode::kStereo);
  REQUIRE(ParseSpeakerConfigMode("surround", &mode));
  CHECK(mode == SpeakerConfigMode::kSurround);
  CHECK_FALSE(ParseSpeakerConfigMode("Stereo", &mode));
  CHECK_FALSE(ParseSpeakerConfigMode("", &mode));

  // The title reads bit 16 only: set = 5.1 digital (mode 3), clear = stereo (mode 1).
  CHECK((kSpeakerConfigStereo & 0x10000u) == 0);
  CHECK((kSpeakerConfigSurround & 0x10000u) != 0);
  CHECK(kSpeakerConfigSurround == 0x00010001u);

  for (int channels : {0, 1, 2, 6, 8}) {
    const auto stereo = ResolveSpeakerConfig(SpeakerConfigMode::kStereo, channels);
    CHECK(stereo.value == kSpeakerConfigStereo);
    CHECK_FALSE(stereo.device_unknown);
    const auto surround = ResolveSpeakerConfig(SpeakerConfigMode::kSurround, channels);
    CHECK(surround.value == kSpeakerConfigSurround);
    CHECK_FALSE(surround.device_unknown);
  }

  const auto unknown = ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 0);
  CHECK(unknown.value == kSpeakerConfigSurround);
  CHECK(unknown.device_unknown);
  CHECK(ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 1).value == kSpeakerConfigStereo);
  CHECK(ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 2).value == kSpeakerConfigStereo);
  CHECK_FALSE(ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 2).device_unknown);
  CHECK(ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 6).value == kSpeakerConfigSurround);
  CHECK(ResolveSpeakerConfig(SpeakerConfigMode::kAuto, 8).value == kSpeakerConfigSurround);
}

TEST_CASE("Audio mix CVars expose the expected defaults", "[audio][speaker]") {
  const rex::cvar::FlagEntry* speaker = rex::cvar::GetFlagInfo("audio_speaker_config");
  const rex::cvar::FlagEntry* itu = rex::cvar::GetFlagInfo("audio_downmix_itu");
  const rex::cvar::FlagEntry* rms = rex::cvar::GetFlagInfo("audio_channel_rms_probe");
  REQUIRE(speaker);
  REQUIRE(itu);
  REQUIRE(rms);
  CHECK(speaker->default_value == "auto");
  CHECK(speaker->lifecycle == rex::cvar::Lifecycle::kRequiresRestart);
  CHECK(itu->default_value == "true");
  CHECK(rms->default_value == "false");

  rex::cvar::ResetToDefault("audio_speaker_config");
  CHECK(rex::cvar::SetFlagByName("audio_speaker_config", "stereo"));
  CHECK(REXCVAR_GET(audio_speaker_config) == "stereo");
  CHECK_FALSE(rex::cvar::SetFlagByName("audio_speaker_config", "quad"));
  CHECK(REXCVAR_GET(audio_speaker_config) == "stereo");
  rex::cvar::ResetToDefault("audio_speaker_config");
  CHECK(REXCVAR_GET(audio_speaker_config) == "auto");
}

TEST_CASE("Channel RMS probe reports per-channel RMS once per window", "[audio][rms]") {
  ChannelRmsAccumulator accumulator(2 * kChannelSamples);
  // Constant per channel: FL 0.5, FR -0.25, FC 1.0, LFE 0, BL 0.125, BR NaN (counted as silence).
  PlanarFrame frame = {};
  const std::array<float, 6> values = {0.5f, -0.25f, 1.0f, 0.0f, 0.125f, std::nanf("")};
  for (size_t channel = 0; channel < 6; ++channel) {
    for (size_t i = 0; i < kChannelSamples; ++i) {
      frame[channel * kChannelSamples + i] = rex::byte_swap(values[channel]);
    }
  }
  ChannelRmsAccumulator::Values rms = {};
  CHECK_FALSE(accumulator.AddPlanarBigEndian(frame.data(), kChannelSamples, &rms));
  REQUIRE(accumulator.AddPlanarBigEndian(frame.data(), kChannelSamples, &rms));
  CHECK(rms[kFl] == Catch::Approx(0.5));
  CHECK(rms[kFr] == Catch::Approx(0.25));
  CHECK(rms[kFc] == Catch::Approx(1.0));
  CHECK(rms[kLfe] == 0.0);
  CHECK(rms[kBl] == Catch::Approx(0.125));
  CHECK(rms[kBr] == 0.0);
  CHECK(FormatChannelRms(rms) == "fl=0.5000 fr=0.2500 fc=1.0000 lfe=0.0000 bl=0.1250 br=0.0000");

  // The window restarts after reporting.
  CHECK_FALSE(accumulator.AddPlanarBigEndian(frame.data(), kChannelSamples, &rms));
}

}  // namespace rex::audio
