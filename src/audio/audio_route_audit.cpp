/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe audio-route audit                           *
 ******************************************************************************
 */

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <string_view>

#include <rex/audio/audio_route_audit.h>
#include <rex/logging.h>
#include <rex/types.h>

REXCVAR_DEFINE_BOOL(sdl_audio_route_audit, false, "Audio",
                    "Emit bounded privacy-safe SDL/XAudio/XMA route evidence")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::audio {
namespace {

constexpr float kNonzeroThreshold = 1.0e-7f;

AudioSampleClassification ClassifyFloatAudio(const float* samples, size_t sample_count,
                                             bool big_endian) {
  AudioSampleClassification result;
  if (!samples || !sample_count) {
    result.finite = false;
    return result;
  }
  float peak = 0.0f;
  for (size_t i = 0; i < sample_count; ++i) {
    float sample = samples[i];
    if (big_endian) {
      sample = std::bit_cast<float>(rex::byte_swap(std::bit_cast<uint32_t>(sample)));
    }
    if (!std::isfinite(sample)) {
      result.finite = false;
      continue;
    }
    const float magnitude = std::abs(sample);
    peak = std::max(peak, magnitude);
    result.nonzero |= magnitude > kNonzeroThreshold;
  }
  result.peak_ppm = static_cast<uint32_t>(std::min(peak, 16.0f) * (1000000.0f / 16.0f));
  return result;
}

class AudioRouteAudit {
 public:
  void Client(bool success, uint32_t index) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    ++client_calls_;
    if (success)
      ++client_success_;
    if (!client_recorded_) {
      REXAPU_INFO("SDL_AUDIO_AUDIT_CLIENT v=1 event=register result={:08X} index_class={}",
                  success ? 0u : 0xC0000001u, index < 8 ? "bounded" : "invalid");
      client_recorded_ = true;
    }
  }

  void Submitted(const AudioSampleClassification& c) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    ++submit_frames_;
    submit_nonzero_ += c.nonzero;
    submit_invalid_ += !c.finite;
    submit_peak_ppm_ = std::max(submit_peak_ppm_, c.peak_ppm);
    RecordFrameLocked("submit", c, 6, submit_recorded_);
  }

  void Device(const AudioSampleClassification& c, uint32_t channels, bool submitted) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    ++device_frames_;
    device_nonzero_ += c.nonzero;
    device_invalid_ += !c.finite;
    device_submit_fail_ += !submitted;
    device_peak_ppm_ = std::max(device_peak_ppm_, c.peak_ppm);
    RecordFrameLocked("device", c, channels, device_recorded_);
  }

  void QueueDepth(size_t depth) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    max_queue_depth_ = std::max<uint64_t>(max_queue_depth_, depth);
    submitted_once_ = true;
  }

  void Starvation() {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || !submitted_once_)
      return;
    ++starvation_fills_;
    ++consecutive_starvation_fills_;
    max_consecutive_starvation_fills_ =
        std::max(max_consecutive_starvation_fills_, consecutive_starvation_fills_);
  }

  void Consumed() {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    consecutive_starvation_fills_ = 0;
  }

  void Xma(const AudioSampleClassification& c) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked())
      return;
    ++xma_frames_;
    xma_nonzero_ += c.nonzero;
    xma_invalid_ += !c.finite;
    xma_peak_ppm_ = std::max(xma_peak_ppm_, c.peak_ppm);
    RecordFrameLocked("xma", c, 0, xma_recorded_);
  }

  void Summary(const char* phase) {
    std::lock_guard lock(mutex_);
    if (!BeginLocked() || summary_)
      return;
    summary_ = true;
    const bool phase_valid = phase && std::string_view(phase) == "title";
    const bool pass =
        phase_valid && client_success_ > 0 && submit_frames_ > 0 && submit_nonzero_ > 0 &&
        submit_invalid_ == 0 && device_frames_ > 0 && device_nonzero_ > 0 && device_invalid_ == 0 &&
        device_submit_fail_ == 0 && xma_frames_ > 0 && xma_nonzero_ > 0 && xma_invalid_ == 0 &&
        IsAudioRouteBufferHealthBounded(max_queue_depth_, max_consecutive_starvation_fills_);
    enabled_ = false;
    REXAPU_INFO(
        "SDL_AUDIO_AUDIT_SUMMARY v=1 phase={} status={} client_calls={} "
        "client_success={} submit_frames={} submit_nonzero={} submit_invalid={} "
        "submit_peak_ppm={} device_frames={} device_nonzero={} device_invalid={} "
        "device_submit_fail={} device_peak_ppm={} xma_frames={} xma_nonzero={} "
        "xma_invalid={} xma_peak_ppm={} max_queue_depth={} starvation_fills={} "
        "max_consecutive_starvation_fills={} dropped_records=0",
        phase_valid ? "title" : "invalid", pass ? "PASS" : "FAIL", client_calls_, client_success_,
        submit_frames_, submit_nonzero_, submit_invalid_, submit_peak_ppm_, device_frames_,
        device_nonzero_, device_invalid_, device_submit_fail_, device_peak_ppm_, xma_frames_,
        xma_nonzero_, xma_invalid_, xma_peak_ppm_, max_queue_depth_, starvation_fills_,
        max_consecutive_starvation_fills_);
  }

 private:
  bool BeginLocked() {
    if (!enabled_ || summary_)
      return false;
    if (!configured_) {
      REXAPU_INFO(
          "SDL_AUDIO_AUDIT_CONFIG v=1 enabled=1 backend=sdl sample_rate=48000 "
          "source_channels=6");
      configured_ = true;
    }
    return true;
  }

  void RecordFrameLocked(const char* layer, const AudioSampleClassification& c, uint32_t channels,
                         bool& recorded) {
    if (recorded)
      return;
    REXAPU_INFO(
        "SDL_AUDIO_AUDIT_FRAME v=1 layer={} class={} finite={} channels={} "
        "peak_ppm={}",
        layer, c.nonzero ? "nonzero" : "silence", c.finite ? 1 : 0, channels, c.peak_ppm);
    recorded = true;
  }

  std::mutex mutex_;
  bool enabled_ = REXCVAR_GET(sdl_audio_route_audit);
  bool configured_ = false;
  bool summary_ = false;
  bool client_recorded_ = false;
  bool submit_recorded_ = false;
  bool device_recorded_ = false;
  bool xma_recorded_ = false;
  uint64_t client_calls_ = 0, client_success_ = 0;
  uint64_t submit_frames_ = 0, submit_nonzero_ = 0, submit_invalid_ = 0;
  uint64_t device_frames_ = 0, device_nonzero_ = 0, device_invalid_ = 0, device_submit_fail_ = 0;
  uint64_t xma_frames_ = 0, xma_nonzero_ = 0, xma_invalid_ = 0;
  bool submitted_once_ = false;
  uint64_t max_queue_depth_ = 0, starvation_fills_ = 0;
  uint64_t consecutive_starvation_fills_ = 0, max_consecutive_starvation_fills_ = 0;
  uint32_t submit_peak_ppm_ = 0, device_peak_ppm_ = 0, xma_peak_ppm_ = 0;
};

AudioRouteAudit& GetAudioRouteAudit() {
  static AudioRouteAudit audit;
  return audit;
}

}  // namespace

AudioSampleClassification ClassifyBigEndianFloatAudio(const float* samples, size_t sample_count) {
  return ClassifyFloatAudio(samples, sample_count, true);
}
AudioSampleClassification ClassifyNativeFloatAudio(const float* samples, size_t sample_count) {
  return ClassifyFloatAudio(samples, sample_count, false);
}
AudioSampleClassification ClassifyBigEndianS16Audio(const uint8_t* samples, size_t byte_count) {
  AudioSampleClassification result;
  if (!samples || byte_count < 2 || (byte_count & 1)) {
    result.finite = false;
    return result;
  }
  uint32_t peak = 0;
  for (size_t i = 0; i < byte_count; i += 2) {
    const int16_t sample =
        static_cast<int16_t>((uint16_t(samples[i]) << 8) | uint16_t(samples[i + 1]));
    const uint32_t magnitude = sample == INT16_MIN ? 32768u : uint32_t(std::abs(sample));
    peak = std::max(peak, magnitude);
    result.nonzero |= magnitude != 0;
  }
  result.peak_ppm = static_cast<uint32_t>((uint64_t(peak) * 1000000u) / 32768u);
  return result;
}

bool IsAudioRouteBufferHealthBounded(uint64_t max_queue_depth,
                                     uint64_t max_consecutive_starvation_fills) {
  return max_queue_depth <= 64 && max_consecutive_starvation_fills <= 2;
}

void RecordAudioRouteClient(bool success, uint32_t index) {
  GetAudioRouteAudit().Client(success, index);
}
void RecordAudioRouteSubmittedFrame(const float* samples, size_t sample_count) {
  GetAudioRouteAudit().Submitted(ClassifyBigEndianFloatAudio(samples, sample_count));
}
void RecordAudioRouteDeviceFrame(const float* samples, size_t sample_count, uint32_t channels,
                                 bool submitted) {
  GetAudioRouteAudit().Device(ClassifyNativeFloatAudio(samples, sample_count), channels, submitted);
  if (submitted) {
    GetAudioRouteAudit().Consumed();
  }
}
void RecordAudioRouteQueueDepth(size_t depth) {
  GetAudioRouteAudit().QueueDepth(depth);
}
void RecordAudioRouteStarvation() {
  GetAudioRouteAudit().Starvation();
}
void RecordAudioRouteXmaFrame(const uint8_t* samples, size_t byte_count) {
  GetAudioRouteAudit().Xma(ClassifyBigEndianS16Audio(samples, byte_count));
}
void EmitAudioRouteAuditSummary(const char* phase) {
  GetAudioRouteAudit().Summary(phase);
}

}  // namespace rex::audio
