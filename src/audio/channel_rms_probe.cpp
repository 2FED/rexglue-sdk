/**
 ******************************************************************************
 * ReXGlue - per-channel RMS probe for the guest 6-channel output frame       *
 ******************************************************************************
 */

#include <cmath>
#include <mutex>

#include <fmt/format.h>

#include <rex/audio/channel_rms_probe.h>
#include <rex/logging.h>
#include <rex/types.h>

REXCVAR_DEFINE_BOOL(audio_channel_rms_probe, false, "Audio",
                    "Log per-channel RMS of the guest 6-channel output once per second");

namespace rex::audio {

bool ChannelRmsAccumulator::AddPlanarBigEndian(const float* frame, size_t channel_samples,
                                               Values* out_rms) {
  if (!frame || !channel_samples) {
    return false;
  }
  for (size_t channel = 0; channel < kChannels; ++channel) {
    const float* samples = frame + channel * channel_samples;
    double sum = 0.0;
    for (size_t i = 0; i < channel_samples; ++i) {
      const float value = rex::byte_swap(samples[i]);
      if (std::isfinite(value)) {
        sum += static_cast<double>(value) * static_cast<double>(value);
      }
    }
    sum_squares_[channel] += sum;
  }
  samples_ += channel_samples;
  if (samples_ < window_) {
    return false;
  }
  if (out_rms) {
    for (size_t channel = 0; channel < kChannels; ++channel) {
      (*out_rms)[channel] = std::sqrt(sum_squares_[channel] / static_cast<double>(samples_));
    }
  }
  sum_squares_ = {};
  samples_ = 0;
  return true;
}

std::string FormatChannelRms(const ChannelRmsAccumulator::Values& rms) {
  return fmt::format("fl={:.4f} fr={:.4f} fc={:.4f} lfe={:.4f} bl={:.4f} br={:.4f}", rms[0], rms[1],
                     rms[2], rms[3], rms[4], rms[5]);
}

void RecordChannelRmsProbeFrame(const float* frame, size_t channel_samples) {
  if (!REXCVAR_GET(audio_channel_rms_probe)) {
    return;
  }
  static std::mutex mutex;
  static ChannelRmsAccumulator accumulator;
  ChannelRmsAccumulator::Values rms = {};
  bool complete;
  {
    std::lock_guard<std::mutex> guard(mutex);
    complete = accumulator.AddPlanarBigEndian(frame, channel_samples, &rms);
  }
  if (complete) {
    REXAPU_INFO("[audio] rms: {}", FormatChannelRms(rms));
  }
}

}  // namespace rex::audio
