/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    MCLA-R, 2026 - M10-001 command-processor cost split
 */

#include <rex/perf/perf_split.h>

#include <atomic>
#include <mutex>
#include <string>

#include <rex/thread.h>

#if REX_PERF_SPLIT_HAS_TSC && !defined(_WIN32)
#include <cpuid.h>
#endif

namespace rex::perf {
namespace {

std::atomic<uint32_t> g_active_mode{0};
std::atomic<uint32_t> g_render_tid{0};
std::atomic<uint32_t> g_render_handle{0};
std::atomic<uint64_t> g_render_wait_ticks{0};
std::atomic<uint64_t> g_render_poll_ticks{0};
std::atomic<uint64_t> g_other_wait_ticks{0};

std::atomic<bool> g_mark_pending{false};
std::mutex g_mark_mutex;
std::string g_mark_reason;

std::atomic<bool> g_flight_pending{false};
std::mutex g_flight_mutex;
std::string g_flight_reason;

std::mutex g_snapshot_mutex;
PerfSplitSnapshot g_snapshot;

CpScopeStack g_cp_scope;
thread_local bool t_cp_scope_thread = false;

bool CurrentIsRenderThread() {
  const uint32_t tid = g_render_tid.load(std::memory_order_relaxed);
  return tid && tid == rex::thread::current_thread_system_id();
}

}  // namespace

void SetPerfSplitActiveMode(uint32_t mode) {
  g_active_mode.store(mode, std::memory_order_relaxed);
}

uint32_t PerfSplitActiveMode() {
  return g_active_mode.load(std::memory_order_relaxed);
}

void NotePerfSplitRenderThread(uint32_t guest_handle) {
  const uint32_t tid = rex::thread::current_thread_system_id();
  if (g_render_tid.load(std::memory_order_relaxed) != tid) {
    g_render_tid.store(tid, std::memory_order_relaxed);
  }
  if (g_render_handle.load(std::memory_order_relaxed) != guest_handle) {
    g_render_handle.store(guest_handle, std::memory_order_relaxed);
  }
}

uint32_t PerfSplitRenderThreadId() {
  return g_render_tid.load(std::memory_order_relaxed);
}

uint32_t PerfSplitRenderGuestHandle() {
  return g_render_handle.load(std::memory_order_relaxed);
}

void AddRenderSerialWaitTicks(uint64_t ticks) {
  if (CurrentIsRenderThread()) {
    g_render_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
  } else {
    g_other_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
  }
}

void AddRenderSerialPollTicks(uint64_t ticks) {
  if (CurrentIsRenderThread()) {
    g_render_poll_ticks.fetch_add(ticks, std::memory_order_relaxed);
  } else {
    g_other_wait_ticks.fetch_add(ticks, std::memory_order_relaxed);
  }
}

uint64_t RenderSerialWaitTicksTotal() {
  return g_render_wait_ticks.load(std::memory_order_relaxed);
}

uint64_t RenderSerialPollTicksTotal() {
  return g_render_poll_ticks.load(std::memory_order_relaxed);
}

uint64_t OtherThreadSerialWaitTicksTotal() {
  return g_other_wait_ticks.load(std::memory_order_relaxed);
}

void RequestPerfSplitMark(std::string_view reason) {
  std::lock_guard<std::mutex> lock(g_mark_mutex);
  if (!g_mark_pending.load(std::memory_order_relaxed)) {
    g_mark_reason = SanitizePerfSplitReason(reason);
  }
  g_mark_pending.store(true, std::memory_order_release);
}

bool PerfSplitMarkPending() {
  return g_mark_pending.load(std::memory_order_acquire);
}

bool TakePerfSplitMark(std::string& reason) {
  if (!g_mark_pending.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mark_mutex);
  if (!g_mark_pending.load(std::memory_order_relaxed)) {
    return false;
  }
  reason = g_mark_reason;
  g_mark_pending.store(false, std::memory_order_relaxed);
  return true;
}

void RequestDiagFlightDump(std::string_view reason) {
  std::lock_guard<std::mutex> lock(g_flight_mutex);
  if (!g_flight_pending.load(std::memory_order_relaxed)) {
    g_flight_reason = SanitizePerfSplitReason(reason);
  }
  g_flight_pending.store(true, std::memory_order_release);
}

bool TakeDiagFlightDump(std::string& reason) {
  if (!g_flight_pending.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_flight_mutex);
  if (!g_flight_pending.load(std::memory_order_relaxed)) {
    return false;
  }
  reason = g_flight_reason;
  g_flight_pending.store(false, std::memory_order_relaxed);
  return true;
}

void PublishPerfSplitSnapshot(const PerfSplitWindow& window) {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  g_snapshot.available = true;
  g_snapshot.last_window = window;
  AccumulatePerfSplitWindow(g_snapshot.totals, window);
}

PerfSplitSnapshot GetPerfSplitSnapshot() {
  std::lock_guard<std::mutex> lock(g_snapshot_mutex);
  return g_snapshot;
}

void MarkCpScopeThread() {
  t_cp_scope_thread = true;
}

uint64_t CpScopeNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void NoteCpScopeEnter(PerfRegion region) {
  if (t_cp_scope_thread) {
    g_cp_scope.Push(region, CpScopeNowNs());
  }
}

void NoteCpScopeExit() {
  if (t_cp_scope_thread) {
    g_cp_scope.Pop();
  }
}

CpScopeSnapshot GetCpScopeSnapshot() {
  return g_cp_scope.Read();
}

bool HasInvariantTsc() {
#if REX_PERF_SPLIT_HAS_TSC
#if defined(_WIN32)
  int regs[4] = {};
  __cpuid(regs, int(0x80000000u));
  if (uint32_t(regs[0]) < 0x80000007u) {
    return false;
  }
  __cpuid(regs, int(0x80000007u));
  return (uint32_t(regs[3]) & (1u << 8)) != 0;
#else
  unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
  if (!__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) || eax < 0x80000007u) {
    return false;
  }
  if (!__get_cpuid(0x80000007u, &eax, &ebx, &ecx, &edx)) {
    return false;
  }
  return (edx & (1u << 8)) != 0;
#endif
#else
  return false;
#endif
}

}  // namespace rex::perf

namespace rex::graphics {

void MarkPerfSplit(const char* tag) {
  rex::perf::RequestDiagFlightDump(tag ? std::string_view(tag) : std::string_view("mark"));
  if (!rex::perf::PerfSplitActive()) {
    return;
  }
  rex::perf::RequestPerfSplitMark(tag ? std::string_view(tag) : std::string_view("mark"));
}

}  // namespace rex::graphics
