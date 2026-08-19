/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe audio-route audit                           *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, sdl_audio_route_audit);
REXCVAR_DECLARE(bool, sdl_audio_event_audit);
REXCVAR_DECLARE(bool, sdl_audio_stability_audit);

namespace rex::audio {

enum class AudioEventPhase : uint32_t {
  kMusic = 0,
  kAmbient,
  kVoice,
  kEngine,
  kCollision,
  kUi,
  kCount,
};

enum class AudioDeviceEvent : uint32_t {
  kAdded = 0,
  kRemoved,
  kFormatChanged,
  kMigrated,
  kCount,
};

struct AudioSampleClassification {
  bool finite = true;
  bool nonzero = false;
  uint32_t peak_ppm = 0;
};

AudioSampleClassification ClassifyBigEndianFloatAudio(const float* samples, size_t sample_count);
AudioSampleClassification ClassifyNativeFloatAudio(const float* samples, size_t sample_count);
AudioSampleClassification ClassifyBigEndianS16Audio(const uint8_t* samples, size_t byte_count);
bool IsAudioRouteBufferHealthBounded(uint64_t max_queue_depth,
                                     uint64_t max_consecutive_starvation_fills);
const char* AudioEventPhaseName(AudioEventPhase phase);
bool IsAudioEventPhaseOrderValid(uint32_t expected_index, AudioEventPhase phase);
bool IsAudioEventPhaseHealthy(uint64_t frames, uint64_t nonzero, uint64_t invalid,
                              uint64_t submit_failures);
const char* AudioDeviceEventName(AudioDeviceEvent event);
bool IsAudioStabilityHealthy(uint64_t driver_success, uint64_t pause_success,
                             uint64_t resume_success, uint64_t resume_recoveries,
                             uint64_t device_events, uint64_t device_recoveries, uint64_t failures);

void RecordAudioRouteClient(bool success, uint32_t index);
void RecordAudioRouteSubmittedFrame(const float* samples, size_t sample_count);
void RecordAudioRouteDeviceFrame(const float* samples, size_t sample_count, uint32_t channels,
                                 bool submitted);
void RecordAudioRouteQueueDepth(size_t depth);
void RecordAudioRouteStarvation();
void RecordAudioRouteXmaFrame(const uint8_t* samples, size_t byte_count);
void EmitAudioRouteAuditSummary(const char* phase);
void RecordAudioStabilityDriver(bool success, uint32_t channels);
void RecordAudioStabilityPause(bool paused, bool success);
void PrepareAudioStabilityResume();
bool ArmAudioStabilityDeviceSwitch();
void RecordAudioStabilityDeviceEvent(AudioDeviceEvent event);
void EmitAudioStabilityAuditSummary(const char* phase);
bool BeginAudioEventPhase(AudioEventPhase phase);
bool EndAudioEventPhase(AudioEventPhase phase);
void EmitAudioEventAuditSummary();

}  // namespace rex::audio
