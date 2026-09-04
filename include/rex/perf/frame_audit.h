/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace rex::perf {

constexpr uint64_t kFrameAuditStreamingStallUs = 5000;

constexpr bool IsFrameAuditStreamingStall(uint64_t elapsed_us) {
  return elapsed_us >= kFrameAuditStreamingStallUs;
}

constexpr uint64_t ConvertGpuTicksToMicroseconds(uint64_t ticks, uint64_t frequency) {
  return frequency ? ticks * UINT64_C(1000000) / frequency : 0;
}

bool InitializeFrameAudit();
bool IsFrameAuditEnabled();
// Called by a graphics backend after the timestamp resources required for a
// complete CPU/GPU audit have been created or destroyed.
void SetFrameAuditGpuTimingAvailable(bool available);
// Arms one bounded 300-frame audit after startup, for example from an
// application diagnostic hotkey. Returns false while an audit is already
// active or when GPU timing is unavailable, so an accidental repeated key
// press can't discard an in-flight capture or start one that cannot finish.
bool ArmFrameAuditForDiagnostics();
void BeginGuestFrame(uint64_t guest_frame);
void RecordCpuFrame(uint64_t guest_frame, uint64_t elapsed_us);
void RecordGpuFrame(uint64_t guest_frame, uint64_t elapsed_us);
void RecordStreamingRead(size_t bytes_read, uint64_t elapsed_us);
void RecordAudioUnderrun();
void RecordShaderTranslation(bool succeeded, uint64_t elapsed_us);
void RecordPipelineCreation(bool succeeded, uint64_t elapsed_us);

}  // namespace rex::perf
