/**
 ******************************************************************************
 * ReXGlue - XAudioGetSpeakerConfig answer selection                          *
 ******************************************************************************
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(std::string, audio_speaker_config);

namespace rex::audio {

// Value written by XAudioGetSpeakerConfig. Bit 16 is the digital (Dolby Digital 5.1) flag; the
// low word is the analog output setting (1 = stereo). Midnight Club LA reads only bit 16
// (sub_8213AB78: mode = bit16 ? 3 : 1; sub_82169C70: tests 0x10000).
inline constexpr uint32_t kSpeakerConfigStereo = 0x00000001;
inline constexpr uint32_t kSpeakerConfigSurround = 0x00010001;

enum class SpeakerConfigMode : uint32_t {
  kAuto = 0,
  kStereo,
  kSurround,
};

struct SpeakerConfigAnswer {
  uint32_t value = kSpeakerConfigSurround;
  // True when mode is auto and the host channel count was unknown (answer fell back to surround).
  bool device_unknown = false;
};

bool ParseSpeakerConfigMode(std::string_view text, SpeakerConfigMode* out_mode);
const char* SpeakerConfigModeName(SpeakerConfigMode mode);

// device_channels: host playback device channel count, 0 when unknown.
// auto: 1-2 channels -> stereo, more than 2 -> surround, unknown -> surround (legacy value).
SpeakerConfigAnswer ResolveSpeakerConfig(SpeakerConfigMode mode, int device_channels);

}  // namespace rex::audio
