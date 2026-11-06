/**
 ******************************************************************************
 * ReXGlue - per-channel RMS probe for the guest 6-channel output frame       *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cstddef>
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, audio_channel_rms_probe);

namespace rex::audio {

// Accumulates per-channel RMS over planar big-endian guest frames (FL FR FC LFE BL BR).
class ChannelRmsAccumulator {
 public:
  static constexpr size_t kChannels = 6;
  using Values = std::array<double, kChannels>;

  explicit ChannelRmsAccumulator(size_t window_channel_samples = 48000)
      : window_(window_channel_samples ? window_channel_samples : 1) {}

  // Returns true when this frame completed a window; out_rms then receives the RMS of the
  // window and the accumulator restarts. Non-finite samples count as silence.
  bool AddPlanarBigEndian(const float* frame, size_t channel_samples, Values* out_rms);

 private:
  Values sum_squares_ = {};
  size_t samples_ = 0;
  size_t window_;
};

// "fl=0.0000 fr=0.0000 fc=0.0000 lfe=0.0000 bl=0.0000 br=0.0000"
std::string FormatChannelRms(const ChannelRmsAccumulator::Values& rms);

// Gated by audio_channel_rms_probe; logs one line per second of guest audio (48 kHz).
void RecordChannelRmsProbeFrame(const float* frame, size_t channel_samples);

}  // namespace rex::audio
