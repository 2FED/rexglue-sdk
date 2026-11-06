/**
 ******************************************************************************
 * ReXGlue - XAudioGetSpeakerConfig answer selection                          *
 ******************************************************************************
 */

#include <rex/audio/speaker_config.h>

REXCVAR_DEFINE_STRING(audio_speaker_config, "auto", "Audio",
                      "Speaker configuration reported to the title: auto (follow the host playback "
                      "device: stereo for 1-2 channels, 5.1 otherwise), stereo, or surround (5.1)")
    .allowed({"auto", "stereo", "surround"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::audio {

bool ParseSpeakerConfigMode(std::string_view text, SpeakerConfigMode* out_mode) {
  if (text == "auto") {
    *out_mode = SpeakerConfigMode::kAuto;
  } else if (text == "stereo") {
    *out_mode = SpeakerConfigMode::kStereo;
  } else if (text == "surround") {
    *out_mode = SpeakerConfigMode::kSurround;
  } else {
    return false;
  }
  return true;
}

const char* SpeakerConfigModeName(SpeakerConfigMode mode) {
  switch (mode) {
    case SpeakerConfigMode::kAuto:
      return "auto";
    case SpeakerConfigMode::kStereo:
      return "stereo";
    case SpeakerConfigMode::kSurround:
      return "surround";
  }
  return "unknown";
}

SpeakerConfigAnswer ResolveSpeakerConfig(SpeakerConfigMode mode, int device_channels) {
  SpeakerConfigAnswer answer;
  switch (mode) {
    case SpeakerConfigMode::kStereo:
      answer.value = kSpeakerConfigStereo;
      break;
    case SpeakerConfigMode::kSurround:
      answer.value = kSpeakerConfigSurround;
      break;
    case SpeakerConfigMode::kAuto:
    default:
      if (device_channels <= 0) {
        answer.value = kSpeakerConfigSurround;
        answer.device_unknown = true;
      } else if (device_channels <= 2) {
        answer.value = kSpeakerConfigStereo;
      } else {
        answer.value = kSpeakerConfigSurround;
      }
      break;
  }
  return answer;
}

}  // namespace rex::audio
