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

namespace rex::audio {

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

void RecordAudioRouteClient(bool success, uint32_t index);
void RecordAudioRouteSubmittedFrame(const float* samples, size_t sample_count);
void RecordAudioRouteDeviceFrame(const float* samples, size_t sample_count, uint32_t channels,
                                 bool submitted);
void RecordAudioRouteQueueDepth(size_t depth);
void RecordAudioRouteStarvation();
void RecordAudioRouteXmaFrame(const uint8_t* samples, size_t byte_count);
void EmitAudioRouteAuditSummary(const char* phase);

}  // namespace rex::audio
