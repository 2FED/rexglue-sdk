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
REXCVAR_DEFINE_BOOL(sdl_audio_event_audit, false, "Audio",
                    "Emit bounded privacy-safe audio event-window evidence")
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

class AudioEventAudit {
 public:
  bool Begin(AudioEventPhase phase) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || summary_) {
      return false;
    }
    ConfigureLocked();
    if (active_ || !IsAudioEventPhaseOrderValid(next_phase_, phase)) {
      FailLocked("phase_order");
      return false;
    }
    active_ = true;
    active_phase_ = phase;
    frames_ = nonzero_ = invalid_ = submit_failures_ = 0;
    peak_ppm_ = 0;
    REXAPU_INFO("SDL_AUDIO_EVENT_AUDIT_PHASE v=1 event=BEGIN phase={} index={}",
                AudioEventPhaseName(phase), next_phase_);
    return true;
  }

  bool End(AudioEventPhase phase) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || summary_) {
      return false;
    }
    if (!active_ || active_phase_ != phase || !IsAudioEventPhaseOrderValid(next_phase_, phase)) {
      FailLocked("phase_order");
      return false;
    }
    const bool healthy = IsAudioEventPhaseHealthy(frames_, nonzero_, invalid_, submit_failures_);
    REXAPU_INFO(
        "SDL_AUDIO_EVENT_AUDIT_PHASE v=1 event=END phase={} index={} status={} "
        "device_frames={} nonzero={} invalid={} submit_failures={} peak_ppm={}",
        AudioEventPhaseName(phase), next_phase_, healthy ? "PASS" : "FAIL", frames_, nonzero_,
        invalid_, submit_failures_, peak_ppm_);
    active_ = false;
    if (!healthy) {
      FailLocked("phase_health");
      return false;
    }
    ++next_phase_;
    return true;
  }

  void Device(const AudioSampleClassification& classification, bool submitted) {
    std::lock_guard lock(mutex_);
    if (!enabled_ || summary_ || !active_) {
      return;
    }
    ++frames_;
    nonzero_ += classification.nonzero;
    invalid_ += !classification.finite;
    submit_failures_ += !submitted;
    peak_ppm_ = std::max(peak_ppm_, classification.peak_ppm);
  }

  void Summary() {
    std::lock_guard lock(mutex_);
    if (!enabled_ || summary_) {
      return;
    }
    const bool pass = configured_ && !active_ && !failure_count_ &&
                      next_phase_ == uint32_t(AudioEventPhase::kCount);
    summary_ = true;
    enabled_ = false;
    REXAPU_INFO(
        "SDL_AUDIO_EVENT_AUDIT_SUMMARY v=1 status={} phases={} failures={} "
        "dropped_records=0 external_listening_required=1",
        pass ? "PASS" : "FAIL", next_phase_, failure_count_);
  }

 private:
  void ConfigureLocked() {
    if (configured_) {
      return;
    }
    configured_ = true;
    REXAPU_INFO(
        "SDL_AUDIO_EVENT_AUDIT_CONFIG v=1 enabled=1 backend=sdl "
        "sample_rate=48000 phases=music,ambient,voice,engine,collision,ui");
  }

  void FailLocked(const char* failure_class) {
    ++failure_count_;
    active_ = false;
    summary_ = true;
    enabled_ = false;
    REXAPU_INFO("SDL_AUDIO_EVENT_AUDIT_FAILURE v=1 class={} count={}", failure_class,
                failure_count_);
    REXAPU_INFO(
        "SDL_AUDIO_EVENT_AUDIT_SUMMARY v=1 status=FAIL phases={} failures={} "
        "dropped_records=0 external_listening_required=1",
        next_phase_, failure_count_);
  }

  std::mutex mutex_;
  bool enabled_ = REXCVAR_GET(sdl_audio_event_audit);
  bool configured_ = false;
  bool active_ = false;
  bool summary_ = false;
  AudioEventPhase active_phase_ = AudioEventPhase::kMusic;
  uint32_t next_phase_ = 0;
  uint64_t frames_ = 0;
  uint64_t nonzero_ = 0;
  uint64_t invalid_ = 0;
  uint64_t submit_failures_ = 0;
  uint64_t failure_count_ = 0;
  uint32_t peak_ppm_ = 0;
};

AudioRouteAudit& GetAudioRouteAudit() {
  static AudioRouteAudit audit;
  return audit;
}

AudioEventAudit& GetAudioEventAudit() {
  static AudioEventAudit audit;
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

const char* AudioEventPhaseName(AudioEventPhase phase) {
  switch (phase) {
    case AudioEventPhase::kMusic:
      return "music";
    case AudioEventPhase::kAmbient:
      return "ambient";
    case AudioEventPhase::kVoice:
      return "voice";
    case AudioEventPhase::kEngine:
      return "engine";
    case AudioEventPhase::kCollision:
      return "collision";
    case AudioEventPhase::kUi:
      return "ui";
    default:
      return "invalid";
  }
}

bool IsAudioEventPhaseOrderValid(uint32_t expected_index, AudioEventPhase phase) {
  return expected_index < uint32_t(AudioEventPhase::kCount) && expected_index == uint32_t(phase);
}

bool IsAudioEventPhaseHealthy(uint64_t frames, uint64_t nonzero, uint64_t invalid,
                              uint64_t submit_failures) {
  return frames > 0 && nonzero > 0 && nonzero <= frames && invalid == 0 && submit_failures == 0;
}

void RecordAudioRouteClient(bool success, uint32_t index) {
  GetAudioRouteAudit().Client(success, index);
}
void RecordAudioRouteSubmittedFrame(const float* samples, size_t sample_count) {
  GetAudioRouteAudit().Submitted(ClassifyBigEndianFloatAudio(samples, sample_count));
}
void RecordAudioRouteDeviceFrame(const float* samples, size_t sample_count, uint32_t channels,
                                 bool submitted) {
  const auto classification = ClassifyNativeFloatAudio(samples, sample_count);
  GetAudioRouteAudit().Device(classification, channels, submitted);
  GetAudioEventAudit().Device(classification, submitted);
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

bool BeginAudioEventPhase(AudioEventPhase phase) {
  return GetAudioEventAudit().Begin(phase);
}

bool EndAudioEventPhase(AudioEventPhase phase) {
  return GetAudioEventAudit().End(phase);
}

void EmitAudioEventAuditSummary() {
  GetAudioEventAudit().Summary();
}

}  // namespace rex::audio
