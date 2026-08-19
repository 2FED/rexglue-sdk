/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <rex/perf/frame_audit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(performance_telemetry_audit, false, "Perf",
                    "Emit bounded timestamped CPU/GPU/streaming/audio/shader telemetry")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex::perf {
namespace {

constexpr uint64_t kSampleLimit = 300;
constexpr size_t kPendingFrameLimit = 1024;

struct FrameRecord {
  uint64_t cpu_us = 0;
  uint64_t gpu_us = 0;
  uint64_t streaming_reads = 0;
  uint64_t streaming_bytes = 0;
  uint64_t streaming_stalls = 0;
  uint64_t max_read_us = 0;
  uint64_t audio_underruns = 0;
  uint64_t shader_translations = 0;
  uint64_t shader_failures = 0;
  uint64_t shader_us = 0;
  uint64_t pipeline_creations = 0;
  uint64_t pipeline_failures = 0;
  uint64_t pipeline_us = 0;
  bool cpu_ready = false;
  bool gpu_ready = false;
};

struct Totals {
  uint64_t streaming_reads = 0;
  uint64_t streaming_bytes = 0;
  uint64_t streaming_stalls = 0;
  uint64_t audio_underruns = 0;
  uint64_t shader_translations = 0;
  uint64_t shader_failures = 0;
  uint64_t shader_us = 0;
  uint64_t pipeline_creations = 0;
  uint64_t pipeline_failures = 0;
  uint64_t pipeline_us = 0;
};

std::mutex audit_mutex;
bool audit_initialized = false;
std::atomic<bool> audit_enabled = false;
uint64_t active_guest_frame = 0;
uint64_t emitted_samples = 0;
uint64_t marker_overflow = 0;
std::unordered_map<uint64_t, FrameRecord> pending_frames;
Totals totals;
const auto audit_start = std::chrono::steady_clock::now();

uint64_t HostMicroseconds() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - audit_start)
                      .count());
}

void InitializeLocked() {
  if (audit_initialized) {
    return;
  }
  audit_initialized = true;
  audit_enabled.store(REXCVAR_GET(performance_telemetry_audit), std::memory_order_release);
  if (audit_enabled.load(std::memory_order_relaxed)) {
    REXLOG_INFO(
        "REX_PERF_AUDIT_CONFIG v=1 enabled=1 backend=d3d12 sample_limit={} "
        "streaming_stall_us={}",
        kSampleLimit, kFrameAuditStreamingStallUs);
  }
}

FrameRecord* CurrentFrameLocked() {
  if (!active_guest_frame) {
    return nullptr;
  }
  auto it = pending_frames.find(active_guest_frame);
  return it == pending_frames.end() ? nullptr : &it->second;
}

void EmitIfCompleteLocked(uint64_t guest_frame) {
  auto it = pending_frames.find(guest_frame);
  if (it == pending_frames.end() || !it->second.cpu_ready || !it->second.gpu_ready) {
    return;
  }
  const FrameRecord record = it->second;
  pending_frames.erase(it);
  const uint64_t sample = emitted_samples++;
  REXLOG_INFO(
      "REX_PERF_AUDIT_SAMPLE v=1 sample={} host_us={} guest_frame={} cpu_us={} gpu_us={} "
      "stream_reads={} stream_bytes={} stream_stalls={} stream_max_us={} audio_underruns={} "
      "shader_count={} shader_fail={} shader_us={} pso_count={} pso_fail={} pso_us={}",
      sample, HostMicroseconds(), guest_frame, record.cpu_us, record.gpu_us, record.streaming_reads,
      record.streaming_bytes, record.streaming_stalls, record.max_read_us, record.audio_underruns,
      record.shader_translations, record.shader_failures, record.shader_us,
      record.pipeline_creations, record.pipeline_failures, record.pipeline_us);
  if (emitted_samples != kSampleLimit) {
    return;
  }
  audit_enabled.store(false, std::memory_order_release);
  REXLOG_INFO(
      "REX_PERF_AUDIT_SUMMARY v=1 status=PASS samples={} stream_reads={} stream_bytes={} "
      "stream_stalls={} audio_underruns={} shader_count={} shader_fail={} shader_us={} "
      "pso_count={} pso_fail={} pso_us={} marker_overflow={}",
      emitted_samples, totals.streaming_reads, totals.streaming_bytes, totals.streaming_stalls,
      totals.audio_underruns, totals.shader_translations, totals.shader_failures, totals.shader_us,
      totals.pipeline_creations, totals.pipeline_failures, totals.pipeline_us, marker_overflow);
  pending_frames.clear();
  active_guest_frame = 0;
}

}  // namespace

bool InitializeFrameAudit() {
  std::lock_guard<std::mutex> lock(audit_mutex);
  InitializeLocked();
  return audit_enabled.load(std::memory_order_acquire);
}

bool IsFrameAuditEnabled() {
  return audit_enabled.load(std::memory_order_acquire);
}

void BeginGuestFrame(uint64_t guest_frame) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  if (pending_frames.size() >= kPendingFrameLimit && !pending_frames.contains(guest_frame)) {
    ++marker_overflow;
    return;
  }
  active_guest_frame = guest_frame;
  pending_frames.try_emplace(guest_frame);
}

void RecordCpuFrame(uint64_t guest_frame, uint64_t elapsed_us) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  auto it = pending_frames.find(guest_frame);
  if (it == pending_frames.end()) {
    ++marker_overflow;
    return;
  }
  it->second.cpu_us = elapsed_us;
  it->second.cpu_ready = true;
  EmitIfCompleteLocked(guest_frame);
}

void RecordGpuFrame(uint64_t guest_frame, uint64_t elapsed_us) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  auto it = pending_frames.find(guest_frame);
  if (it == pending_frames.end()) {
    ++marker_overflow;
    return;
  }
  it->second.gpu_us = elapsed_us;
  it->second.gpu_ready = true;
  EmitIfCompleteLocked(guest_frame);
}

void RecordStreamingRead(size_t bytes_read, uint64_t elapsed_us) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  FrameRecord* record = CurrentFrameLocked();
  if (!record) {
    return;
  }
  ++record->streaming_reads;
  record->streaming_bytes += bytes_read;
  record->max_read_us = std::max(record->max_read_us, elapsed_us);
  totals.streaming_reads++;
  totals.streaming_bytes += bytes_read;
  if (IsFrameAuditStreamingStall(elapsed_us)) {
    ++record->streaming_stalls;
    ++totals.streaming_stalls;
  }
}

void RecordAudioUnderrun() {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  FrameRecord* record = CurrentFrameLocked();
  if (record) {
    ++record->audio_underruns;
  }
  ++totals.audio_underruns;
}

void RecordShaderTranslation(bool succeeded, uint64_t elapsed_us) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  FrameRecord* record = CurrentFrameLocked();
  if (record) {
    ++record->shader_translations;
    record->shader_failures += succeeded ? 0 : 1;
    record->shader_us += elapsed_us;
  }
  ++totals.shader_translations;
  totals.shader_failures += succeeded ? 0 : 1;
  totals.shader_us += elapsed_us;
}

void RecordPipelineCreation(bool succeeded, uint64_t elapsed_us) {
  if (!audit_enabled.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(audit_mutex);
  if (!audit_enabled.load(std::memory_order_relaxed)) {
    return;
  }
  FrameRecord* record = CurrentFrameLocked();
  if (record) {
    ++record->pipeline_creations;
    record->pipeline_failures += succeeded ? 0 : 1;
    record->pipeline_us += elapsed_us;
  }
  ++totals.pipeline_creations;
  totals.pipeline_failures += succeeded ? 0 : 1;
  totals.pipeline_us += elapsed_us;
}

}  // namespace rex::perf
