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

#pragma once

// M10-001 measurement harness (d3d12_perf_split). Splits the D3D12 command
// processor (CP) thread's time per window into draw phases, slow-path regions
// and waits, and joins it with thread CPU times and GPU busy time in one
// MCLA_PERF_SPLIT v=1 line per window (one minute, or an F10 mark).
//
// Everything above the "runtime state" section is pure (no globals, no OS
// calls) and unit-tested in tests/unit/core/perf_split_test.cpp.
//
// Accounting model (CP thread only, single writer, plain integers):
// - A timed region (fence wait, upload, copy, submission, present ...) reads
//   the TSC at entry and exit. Its exclusive time is its duration minus the
//   time of regions nested in it, so the exclusive times of all regions never
//   overlap. `charged` is the running sum of exclusive region time.
// - A sampled draw reads the TSC at entry and at each phase end; a phase gets
//   its wall time minus the region time charged during it (waits, uploads,
//   copies happen inside phases), so phases never overlap regions either.
// - Window: cp_busy = wall - ring idle - WAIT_REG_MEM; packets_other =
//   cp_busy - scaled phases - exclusive regions (PM4 decode, register writes,
//   untimed packets and the unsampled share of the scaling error).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>

#if defined(_M_X64) || defined(__x86_64__)
#if defined(_WIN32)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#define REX_PERF_SPLIT_HAS_TSC 1
#else
#define REX_PERF_SPLIT_HAS_TSC 0
#endif

namespace rex::perf {

// Line and snapshot schema version.
inline constexpr uint32_t kPerfSplitVersion = 1;
// Mode 1 samples one draw in this many (xorshift-selected).
inline constexpr uint32_t kPerfSplitSampleDivisor = 16;

inline uint64_t ReadTsc() {
#if REX_PERF_SPLIT_HAS_TSC
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}

// TSC ticks per microsecond (MHz) over a calibration interval. 0 when the
// interval is empty.
constexpr double TscMhz(uint64_t tsc_delta, uint64_t steady_delta_ns) {
  return steady_delta_ns ? double(tsc_delta) * 1000.0 / double(steady_delta_ns) : 0.0;
}

constexpr double TicksToMs(uint64_t ticks, double tsc_mhz) {
  return tsc_mhz > 0.0 ? double(ticks) / (tsc_mhz * 1000.0) : 0.0;
}

// Scales a phase total measured on `sampled` draws to all `draws`.
constexpr double ScaleSampled(double sampled_value, uint64_t draws, uint64_t sampled) {
  return sampled ? sampled_value * double(draws) / double(sampled) : 0.0;
}

// Mode values of the d3d12_perf_split cvar.
constexpr uint32_t NormalizePerfSplitMode(int32_t value) {
  return value <= 0 ? 0u : (value >= 2 ? 2u : 1u);
}

enum class PerfPhase : uint8_t {
  kSetup,          // entry .. BeginSubmission (shader analysis, early outs)
  kPrimitive,      // primitive processor
  kRenderTargets,  // shader modifications, render-target cache Update
  kPipeline,       // translations, ConfigurePipeline, pending-pipeline block
  kTextures,       // texture cache RequestTextures
  kBindings,       // pipeline bind, viewport/scissor, constants, UpdateBindings
  kVertexRanges,   // vertex fetch and memexport shared-memory ranges
  kRecord,         // topology, index buffer, barriers, draw recording, audit
  kCount,
};

inline constexpr const char* kPerfPhaseNames[size_t(PerfPhase::kCount)] = {
    "setup", "prim", "rt", "pso", "tex", "bind", "vb", "record"};

enum class PerfRegion : uint8_t {
  kFenceWait,         // CheckSubmissionFence WaitForSingleObject
  kPipelineWait,      // WaitForPendingPipeline when it actually waits
  kUpload,            // SharedMemory::UploadRanges
  kCopy,              // IssueCopy (EDRAM resolve packet)
  kResolve,           // render-target cache Resolve
  kReadbackGpu,       // read-back buffer setup and GPU copy recording
  kReadbackWait,      // synchronous read-back AwaitAllQueueOperationsCompletion
  kReadbackMemcpy,    // read-back CPU copy into guest memory
  kSubmit,            // EndSubmission
  kStreamFinishWait,  // FinishStreaming (wait for the replay worker)
  kExecute,           // ExecuteCommandLists
  kPresent,           // IssueSwap
  kEmit,              // building and logging this line
  kCount,
};

inline constexpr const char* kPerfRegionNames[size_t(PerfRegion::kCount)] = {
    "fence_wait", "pipeline_wait",      "upload",        "copy",
    "resolve",    "readback_gpu",       "readback_wait", "readback_memcpy",
    "submit",     "stream_finish_wait", "execute",       "present",
    "emit"};

// KI-069 CP freeze watchdog: the stack of timed regions the command processor
// thread is in, published for another thread to read. Single writer (the CP
// thread); a reader may see a stack that changes while it reads, which only
// costs accuracy of one diagnostic line. Deeper levels than kCpScopeDepth are
// counted but not recorded.
inline constexpr size_t kCpScopeDepth = 8;

struct CpScopeSnapshot {
  uint32_t depth = 0;
  PerfRegion regions[kCpScopeDepth] = {};
  uint64_t enter_ns[kCpScopeDepth] = {};
};

class CpScopeStack {
 public:
  void Push(PerfRegion region, uint64_t now_ns) {
    const uint32_t depth = depth_.load(std::memory_order_relaxed);
    if (depth < kCpScopeDepth) {
      regions_[depth].store(uint8_t(region), std::memory_order_relaxed);
      enter_ns_[depth].store(now_ns, std::memory_order_relaxed);
    }
    depth_.store(depth + 1, std::memory_order_release);
  }
  void Pop() {
    const uint32_t depth = depth_.load(std::memory_order_relaxed);
    if (depth) {
      depth_.store(depth - 1, std::memory_order_release);
    }
  }
  CpScopeSnapshot Read() const {
    CpScopeSnapshot snapshot;
    snapshot.depth = depth_.load(std::memory_order_acquire);
    const uint32_t recorded = std::min<uint32_t>(snapshot.depth, uint32_t(kCpScopeDepth));
    for (uint32_t i = 0; i < recorded; ++i) {
      const uint8_t region = regions_[i].load(std::memory_order_relaxed);
      snapshot.regions[i] =
          region < uint8_t(PerfRegion::kCount) ? PerfRegion(region) : PerfRegion::kCount;
      snapshot.enter_ns[i] = enter_ns_[i].load(std::memory_order_relaxed);
    }
    return snapshot;
  }

 private:
  std::atomic<uint32_t> depth_{0};
  std::atomic<uint8_t> regions_[kCpScopeDepth] = {};
  std::atomic<uint64_t> enter_ns_[kCpScopeDepth] = {};
};

// "copy>readback_wait" (outermost first; "-" when empty) and the time spent in
// the innermost recorded region.
inline std::string FormatCpScopePath(const CpScopeSnapshot& snapshot) {
  if (!snapshot.depth) {
    return "-";
  }
  std::string path;
  const uint32_t recorded = std::min<uint32_t>(snapshot.depth, uint32_t(kCpScopeDepth));
  for (uint32_t i = 0; i < recorded; ++i) {
    if (i) {
      path += '>';
    }
    const PerfRegion region = snapshot.regions[i];
    path += region < PerfRegion::kCount ? kPerfRegionNames[size_t(region)] : "?";
  }
  if (snapshot.depth > recorded) {
    path += fmt::format(">+{}", snapshot.depth - recorded);
  }
  return path;
}

inline uint64_t CpScopeInnermostMs(const CpScopeSnapshot& snapshot, uint64_t now_ns) {
  if (!snapshot.depth) {
    return 0;
  }
  const uint32_t top = std::min<uint32_t>(snapshot.depth, uint32_t(kCpScopeDepth)) - 1;
  const uint64_t enter = snapshot.enter_ns[top];
  return now_ns > enter ? (now_ns - enter) / 1000000ull : 0;
}

// Runtime (perf_split.cpp): only the thread that called MarkCpScopeThread
// publishes; calls from other threads are ignored.
void MarkCpScopeThread();
void NoteCpScopeEnter(PerfRegion region);
void NoteCpScopeExit();
CpScopeSnapshot GetCpScopeSnapshot();
uint64_t CpScopeNowNs();

// Outcome of one read-back resolve's CPU copy (same rule as the livery
// diagnostics' ClassifyReadbackCopy).
enum class PerfCopyDecision : uint8_t { kNow, kDeferred, kSkipped };

constexpr PerfCopyDecision ClassifyPerfCopy(bool should_copy, bool copied) {
  return copied ? PerfCopyDecision::kNow
                : (should_copy ? PerfCopyDecision::kSkipped : PerfCopyDecision::kDeferred);
}

enum class PerfDrawExit : uint8_t {
  kNoop,          // early return before recording (no-op or failure)
  kIssued,        // draw recorded
  kPipelineSkip,  // skipped because its pipeline is still being created
};

// Cumulative CP-thread counters (since the process started; only advanced
// while the split is on).
struct PerfSplitCounters {
  uint64_t draws = 0;
  uint64_t draws_issued = 0;
  uint64_t draws_noop = 0;
  uint64_t draws_pso_skip = 0;
  uint64_t sampled = 0;
  uint64_t frames = 0;
  uint64_t phase_ticks[size_t(PerfPhase::kCount)] = {};
  uint64_t region_excl_ticks[size_t(PerfRegion::kCount)] = {};
  uint64_t region_incl_ticks[size_t(PerfRegion::kCount)] = {};
  uint64_t region_calls[size_t(PerfRegion::kCount)] = {};
  // Region time charged while inside IssueDraw (all draws, unsampled).
  uint64_t in_draw_region_ticks = 0;
  uint64_t request_calls = 0;
  uint64_t upload_bytes = 0;
  uint64_t readback_bytes = 0;
  uint64_t copies_now = 0;
  uint64_t copies_deferred = 0;
  uint64_t copies_skipped = 0;
  // M11-016 fetch-constant identity cache (cumulative values copied from the
  // command processor and the texture cache at collection; counted whether
  // or not the split is on).
  uint64_t fetch_dw_written = 0;
  uint64_t fetch_dw_unchanged = 0;
  uint64_t tex_slots = 0;
  uint64_t tex_key_changed = 0;
  uint64_t tex_memo_hit = 0;
  uint64_t tex_memo_miss = 0;
  uint64_t tex_memo_mismatch = 0;
  uint64_t tex_gen_bumps = 0;
  uint64_t smp_memo_hit = 0;
  uint64_t smp_memo_miss = 0;
};

class PerfSplitAccumulator {
 public:
  struct RegionFrame {
    uint64_t start = 0;
    uint64_t charged_base = 0;
  };

  PerfSplitCounters counters;
  // Running sum of exclusive region ticks (see the accounting model).
  uint64_t charged = 0;
  uint32_t depth = 0;
  bool in_draw = false;
  uint32_t mode = 0;
  uint32_t rng = 0x9E3779B9u;

  // Mode 2 samples every draw; mode 1 one in kPerfSplitSampleDivisor.
  bool SampleNext() {
    if (mode >= 2) {
      return true;
    }
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (rng & (kPerfSplitSampleDivisor - 1)) == 0;
  }

  RegionFrame Enter(uint64_t now) {
    ++depth;
    return RegionFrame{now, charged};
  }

  void Exit(PerfRegion region, const RegionFrame& frame, uint64_t now) {
    const uint64_t duration = now >= frame.start ? now - frame.start : 0;
    const uint64_t nested = charged - frame.charged_base;
    const size_t index = size_t(region);
    counters.region_excl_ticks[index] += duration > nested ? duration - nested : 0;
    counters.region_incl_ticks[index] += duration;
    ++counters.region_calls[index];
    // The region's whole duration replaces its children's share.
    charged = frame.charged_base + std::max(duration, nested);
    if (depth) {
      --depth;
    }
    if (!depth && in_draw) {
      counters.in_draw_region_ticks += std::max(duration, nested);
    }
  }

  void CountCopy(PerfCopyDecision decision) {
    switch (decision) {
      case PerfCopyDecision::kNow:
        ++counters.copies_now;
        break;
      case PerfCopyDecision::kDeferred:
        ++counters.copies_deferred;
        break;
      case PerfCopyDecision::kSkipped:
        ++counters.copies_skipped;
        break;
    }
  }
};

// RAII region; a null accumulator makes it a no-op (split off).
class ScopedPerfRegion {
 public:
  ScopedPerfRegion(PerfSplitAccumulator* accumulator, PerfRegion region)
      : accumulator_(accumulator), region_(region) {
    if (accumulator_) {
      NoteCpScopeEnter(region_);
      frame_ = accumulator_->Enter(ReadTsc());
    }
  }
  ~ScopedPerfRegion() { End(); }
  void End() {
    if (accumulator_) {
      accumulator_->Exit(region_, frame_, ReadTsc());
      accumulator_ = nullptr;
      NoteCpScopeExit();
    }
  }
  ScopedPerfRegion(const ScopedPerfRegion&) = delete;
  ScopedPerfRegion& operator=(const ScopedPerfRegion&) = delete;

 private:
  PerfSplitAccumulator* accumulator_;
  PerfRegion region_;
  PerfSplitAccumulator::RegionFrame frame_;
};

// Phase chain of one IssueDraw call. With a null accumulator every call is a
// branch on a local; unsampled draws only count their exit kind.
class PerfDrawChain {
 public:
  explicit PerfDrawChain(PerfSplitAccumulator* accumulator) : accumulator_(accumulator) {
    if (accumulator_) {
      Begin(ReadTsc());
    }
  }
  // Test seam: explicit clock.
  PerfDrawChain(PerfSplitAccumulator* accumulator, uint64_t now) : accumulator_(accumulator) {
    if (accumulator_) {
      Begin(now);
    }
  }
  ~PerfDrawChain() {
    if (accumulator_) {
      Finish(sampled_ ? ReadTsc() : 0);
    }
  }
  void Mark(PerfPhase phase) {
    if (sampled_) {
      MarkAt(phase, ReadTsc());
    }
  }
  void MarkAt(PerfPhase phase, uint64_t now) {
    if (!sampled_) {
      return;
    }
    const uint64_t charged = accumulator_->charged;
    const uint64_t elapsed = now >= last_tsc_ ? now - last_tsc_ : 0;
    const uint64_t waited = charged - last_charged_;
    accumulator_->counters.phase_ticks[size_t(phase)] += elapsed > waited ? elapsed - waited : 0;
    last_tsc_ = now;
    last_charged_ = charged;
    next_phase_ = uint8_t(size_t(phase) + 1);
  }
  void SetExit(PerfDrawExit exit) { exit_ = exit; }
  // Test seam: closes the chain at an explicit clock (the destructor then
  // does nothing).
  void FinishAt(uint64_t now) {
    if (accumulator_) {
      Finish(now);
    }
  }
  bool sampled() const { return sampled_; }

 private:
  void Begin(uint64_t now) {
    PerfSplitAccumulator& a = *accumulator_;
    ++a.counters.draws;
    a.in_draw = true;
    sampled_ = a.SampleNext();
    if (sampled_) {
      ++a.counters.sampled;
      last_tsc_ = now;
      last_charged_ = a.charged;
    }
  }
  void Finish(uint64_t now) {
    PerfSplitAccumulator& a = *accumulator_;
    // An early return closes the phase that was in progress.
    if (sampled_ && next_phase_ < uint8_t(PerfPhase::kCount)) {
      MarkAt(PerfPhase(next_phase_), now);
    }
    switch (exit_) {
      case PerfDrawExit::kIssued:
        ++a.counters.draws_issued;
        break;
      case PerfDrawExit::kPipelineSkip:
        ++a.counters.draws_pso_skip;
        break;
      case PerfDrawExit::kNoop:
        ++a.counters.draws_noop;
        break;
    }
    a.in_draw = false;
    accumulator_ = nullptr;
  }

  PerfSplitAccumulator* accumulator_;
  bool sampled_ = false;
  uint8_t next_phase_ = 0;
  PerfDrawExit exit_ = PerfDrawExit::kNoop;
  uint64_t last_tsc_ = 0;
  uint64_t last_charged_ = 0;
};

// Cumulative inputs of one emission (CP thread). Times not measured with the
// TSC are in microseconds or 100 ns units as named.
struct PerfSplitInputs {
  uint64_t steady_ns = 0;
  uint64_t tsc = 0;
  PerfSplitCounters counters;
  uint64_t cp_user_100ns = 0;
  uint64_t cp_kernel_100ns = 0;
  uint64_t replay_user_100ns = 0;
  uint64_t replay_kernel_100ns = 0;
  uint64_t replay_us = 0;
  uint32_t render_tid = 0;
  uint32_t render_handle = 0;
  bool render_times_valid = false;
  uint64_t render_user_100ns = 0;
  uint64_t render_kernel_100ns = 0;
  uint64_t render_wait_ticks = 0;
  uint64_t render_poll_ticks = 0;
  uint64_t ring_idle_us = 0;
  uint64_t wrm_total_us = 0;
  uint64_t wrm_memory_us = 0;
  uint64_t draw_packet_us = 0;
  uint64_t watch_arm_us = 0;
  uint64_t watch_disarm_us = 0;
  uint64_t write_fault_us = 0;
  uint64_t gpu_busy_us = 0;
  uint64_t gpu_submissions = 0;
};

// X-macro field list of one window: (name, kind). kind Count = integer
// count, Ms = milliseconds (printed name_ms), Kb = kibibytes (name_kb).
// The order is the MCLA_PERF_SPLIT v=1 field order after the header fields.
// Fields are only ever appended (M11-016 added fetch_dw_written ..
// smp_memo_miss after gpu_submissions), so v=1 parsers keyed by name keep
// working.
#define REX_PERF_SPLIT_FIELDS(X) \
  X(frames, Count)               \
  X(draws, Count)                \
  X(draws_issued, Count)         \
  X(draws_noop, Count)           \
  X(draws_pso_skip, Count)       \
  X(sampled, Count)              \
  X(copies, Count)               \
  X(submissions, Count)          \
  X(cp_cpu, Ms)                  \
  X(cp_user, Ms)                 \
  X(cp_kernel, Ms)               \
  X(cp_busy, Ms)                 \
  X(ring_idle, Ms)               \
  X(wrm_reg, Ms)                 \
  X(wrm_mem, Ms)                 \
  X(fence_wait, Ms)              \
  X(pso_wait, Ms)                \
  X(readback_wait, Ms)           \
  X(stream_finish_wait, Ms)      \
  X(draw, Ms)                    \
  X(p_setup, Ms)                 \
  X(p_prim, Ms)                  \
  X(p_rt, Ms)                    \
  X(p_pso, Ms)                   \
  X(p_tex, Ms)                   \
  X(p_bind, Ms)                  \
  X(p_vb, Ms)                    \
  X(p_record, Ms)                \
  X(draw_packet, Ms)             \
  X(draw_regions, Ms)            \
  X(request_calls, Count)        \
  X(upload_calls, Count)         \
  X(upload, Ms)                  \
  X(upload, Kb)                  \
  X(watch_arm, Ms)               \
  X(watch_disarm, Ms)            \
  X(write_fault, Ms)             \
  X(copy, Ms)                    \
  X(resolve, Ms)                 \
  X(readback_gpu, Ms)            \
  X(readback_memcpy, Ms)         \
  X(readback, Kb)                \
  X(copies_now, Count)           \
  X(copies_deferred, Count)      \
  X(copies_skipped, Count)       \
  X(submit, Ms)                  \
  X(execute, Ms)                 \
  X(present, Ms)                 \
  X(emit, Ms)                    \
  X(packets_other, Ms)           \
  X(replay_cpu, Ms)              \
  X(replay, Ms)                  \
  X(render_cpu, Ms)              \
  X(render_wait, Ms)             \
  X(render_poll, Ms)             \
  X(gpu_busy, Ms)                \
  X(gpu_submissions, Count)      \
  X(fetch_dw_written, Count)     \
  X(fetch_dw_unchanged, Count)   \
  X(tex_slots, Count)            \
  X(tex_key_changed, Count)      \
  X(tex_memo_hit, Count)         \
  X(tex_memo_miss, Count)        \
  X(tex_memo_mismatch, Count)    \
  X(tex_gen_bumps, Count)        \
  X(smp_memo_hit, Count)         \
  X(smp_memo_miss, Count)

struct PerfSplitWindow {
  uint64_t seq = 0;
  std::string reason;
  uint32_t mode = 0;
  uint32_t sample_div = kPerfSplitSampleDivisor;
  double wall_ms = 0.0;
  double tsc_mhz = 0.0;
  uint32_t render_tid = 0;
  uint32_t render_handle = 0;
  // False when the render thread changed or is unknown in this window:
  // render_cpu_ms is then printed as -1.
  bool render_cpu_valid = false;
#define REX_PERF_SPLIT_DECLARE_Count(name) uint64_t name = 0;
#define REX_PERF_SPLIT_DECLARE_Ms(name) double name##_ms = 0.0;
#define REX_PERF_SPLIT_DECLARE_Kb(name) uint64_t name##_kb = 0;
#define REX_PERF_SPLIT_DECLARE(name, kind) REX_PERF_SPLIT_DECLARE_##kind(name)
  REX_PERF_SPLIT_FIELDS(REX_PERF_SPLIT_DECLARE)
#undef REX_PERF_SPLIT_DECLARE
#undef REX_PERF_SPLIT_DECLARE_Kb
#undef REX_PERF_SPLIT_DECLARE_Ms
#undef REX_PERF_SPLIT_DECLARE_Count
};

namespace perf_split_detail {
constexpr uint64_t Delta(uint64_t current, uint64_t previous) {
  return current >= previous ? current - previous : 0;
}
constexpr double UsToMs(uint64_t us) {
  return double(us) / 1000.0;
}
constexpr double HundredNsToMs(uint64_t value) {
  return double(value) / 10000.0;
}
}  // namespace perf_split_detail

// Window between two emissions. tsc_mhz is calibrated over the window itself
// (TSC delta against steady_clock delta).
inline PerfSplitWindow ComputePerfSplitWindow(const PerfSplitInputs& previous,
                                              const PerfSplitInputs& current, uint32_t mode) {
  using perf_split_detail::Delta;
  using perf_split_detail::HundredNsToMs;
  using perf_split_detail::UsToMs;
  PerfSplitWindow w;
  w.mode = mode;
  const uint64_t steady_ns = Delta(current.steady_ns, previous.steady_ns);
  w.wall_ms = double(steady_ns) / 1e6;
  w.tsc_mhz = TscMhz(Delta(current.tsc, previous.tsc), steady_ns);
  const double mhz = w.tsc_mhz;
  const PerfSplitCounters& c = current.counters;
  const PerfSplitCounters& p = previous.counters;
  auto region_excl = [&](PerfRegion r) {
    return TicksToMs(Delta(c.region_excl_ticks[size_t(r)], p.region_excl_ticks[size_t(r)]), mhz);
  };
  auto region_incl = [&](PerfRegion r) {
    return TicksToMs(Delta(c.region_incl_ticks[size_t(r)], p.region_incl_ticks[size_t(r)]), mhz);
  };
  auto region_calls = [&](PerfRegion r) {
    return Delta(c.region_calls[size_t(r)], p.region_calls[size_t(r)]);
  };

  w.frames = Delta(c.frames, p.frames);
  w.draws = Delta(c.draws, p.draws);
  w.draws_issued = Delta(c.draws_issued, p.draws_issued);
  w.draws_noop = Delta(c.draws_noop, p.draws_noop);
  w.draws_pso_skip = Delta(c.draws_pso_skip, p.draws_pso_skip);
  w.sampled = Delta(c.sampled, p.sampled);
  w.copies = region_calls(PerfRegion::kCopy);
  w.submissions = region_calls(PerfRegion::kExecute);

  const uint64_t cp_user = Delta(current.cp_user_100ns, previous.cp_user_100ns);
  const uint64_t cp_kernel = Delta(current.cp_kernel_100ns, previous.cp_kernel_100ns);
  w.cp_user_ms = HundredNsToMs(cp_user);
  w.cp_kernel_ms = HundredNsToMs(cp_kernel);
  w.cp_cpu_ms = w.cp_user_ms + w.cp_kernel_ms;
  w.ring_idle_ms = UsToMs(Delta(current.ring_idle_us, previous.ring_idle_us));
  const double wrm_total = UsToMs(Delta(current.wrm_total_us, previous.wrm_total_us));
  w.wrm_mem_ms = UsToMs(Delta(current.wrm_memory_us, previous.wrm_memory_us));
  w.wrm_reg_ms = std::max(0.0, wrm_total - w.wrm_mem_ms);
  w.cp_busy_ms = std::max(0.0, w.wall_ms - w.ring_idle_ms - wrm_total);

  w.fence_wait_ms = region_excl(PerfRegion::kFenceWait);
  w.pso_wait_ms = region_excl(PerfRegion::kPipelineWait);
  // Inclusive: contains the fence wait and any submission it forces.
  w.readback_wait_ms = region_incl(PerfRegion::kReadbackWait);
  w.stream_finish_wait_ms = region_excl(PerfRegion::kStreamFinishWait);

  double* phases[size_t(PerfPhase::kCount)] = {&w.p_setup_ms, &w.p_prim_ms,  &w.p_rt_ms,
                                               &w.p_pso_ms,   &w.p_tex_ms,   &w.p_bind_ms,
                                               &w.p_vb_ms,    &w.p_record_ms};
  w.draw_ms = 0.0;
  for (size_t i = 0; i < size_t(PerfPhase::kCount); ++i) {
    const double sampled_ms = TicksToMs(Delta(c.phase_ticks[i], p.phase_ticks[i]), mhz);
    *phases[i] = ScaleSampled(sampled_ms, w.draws, w.sampled);
    w.draw_ms += *phases[i];
  }
  w.draw_packet_ms = UsToMs(Delta(current.draw_packet_us, previous.draw_packet_us));
  w.draw_regions_ms = TicksToMs(Delta(c.in_draw_region_ticks, p.in_draw_region_ticks), mhz);

  w.request_calls = Delta(c.request_calls, p.request_calls);
  w.upload_calls = region_calls(PerfRegion::kUpload);
  w.upload_ms = region_excl(PerfRegion::kUpload);
  w.upload_kb = Delta(c.upload_bytes, p.upload_bytes) / 1024;
  w.watch_arm_ms = UsToMs(Delta(current.watch_arm_us, previous.watch_arm_us));
  w.watch_disarm_ms = UsToMs(Delta(current.watch_disarm_us, previous.watch_disarm_us));
  w.write_fault_ms = UsToMs(Delta(current.write_fault_us, previous.write_fault_us));

  // copy_ms is inclusive (resolve, read-back parts, nested uploads and waits).
  w.copy_ms = region_incl(PerfRegion::kCopy);
  w.resolve_ms = region_excl(PerfRegion::kResolve);
  w.readback_gpu_ms = region_excl(PerfRegion::kReadbackGpu);
  w.readback_memcpy_ms = region_excl(PerfRegion::kReadbackMemcpy);
  w.readback_kb = Delta(c.readback_bytes, p.readback_bytes) / 1024;
  w.copies_now = Delta(c.copies_now, p.copies_now);
  w.copies_deferred = Delta(c.copies_deferred, p.copies_deferred);
  w.copies_skipped = Delta(c.copies_skipped, p.copies_skipped);

  w.submit_ms = region_excl(PerfRegion::kSubmit);
  w.execute_ms = region_excl(PerfRegion::kExecute);
  w.present_ms = region_excl(PerfRegion::kPresent);
  w.emit_ms = region_excl(PerfRegion::kEmit);

  double regions_ms = 0.0;
  for (size_t i = 0; i < size_t(PerfRegion::kCount); ++i) {
    regions_ms += region_excl(PerfRegion(i));
  }
  w.packets_other_ms = w.cp_busy_ms - w.draw_ms - regions_ms;

  w.replay_cpu_ms = HundredNsToMs(Delta(current.replay_user_100ns, previous.replay_user_100ns) +
                                  Delta(current.replay_kernel_100ns, previous.replay_kernel_100ns));
  w.replay_ms = UsToMs(Delta(current.replay_us, previous.replay_us));

  w.render_tid = current.render_tid;
  w.render_handle = current.render_handle;
  w.render_cpu_valid = current.render_times_valid && previous.render_times_valid &&
                       current.render_tid != 0 && current.render_tid == previous.render_tid;
  w.render_cpu_ms =
      w.render_cpu_valid
          ? HundredNsToMs(Delta(current.render_user_100ns, previous.render_user_100ns) +
                          Delta(current.render_kernel_100ns, previous.render_kernel_100ns))
          : -1.0;
  w.render_wait_ms = TicksToMs(Delta(current.render_wait_ticks, previous.render_wait_ticks), mhz);
  w.render_poll_ms = TicksToMs(Delta(current.render_poll_ticks, previous.render_poll_ticks), mhz);

  w.gpu_busy_ms = UsToMs(Delta(current.gpu_busy_us, previous.gpu_busy_us));
  w.gpu_submissions = Delta(current.gpu_submissions, previous.gpu_submissions);

  w.fetch_dw_written = Delta(c.fetch_dw_written, p.fetch_dw_written);
  w.fetch_dw_unchanged = Delta(c.fetch_dw_unchanged, p.fetch_dw_unchanged);
  w.tex_slots = Delta(c.tex_slots, p.tex_slots);
  w.tex_key_changed = Delta(c.tex_key_changed, p.tex_key_changed);
  w.tex_memo_hit = Delta(c.tex_memo_hit, p.tex_memo_hit);
  w.tex_memo_miss = Delta(c.tex_memo_miss, p.tex_memo_miss);
  w.tex_memo_mismatch = Delta(c.tex_memo_mismatch, p.tex_memo_mismatch);
  w.tex_gen_bumps = Delta(c.tex_gen_bumps, p.tex_gen_bumps);
  w.smp_memo_hit = Delta(c.smp_memo_hit, p.smp_memo_hit);
  w.smp_memo_miss = Delta(c.smp_memo_miss, p.smp_memo_miss);
  return w;
}

// Adds a window into a running total (state.json). Header fields take the
// latest window's values.
inline void AccumulatePerfSplitWindow(PerfSplitWindow& total, const PerfSplitWindow& window) {
  total.seq = window.seq;
  total.reason = window.reason;
  total.mode = window.mode;
  total.sample_div = window.sample_div;
  total.tsc_mhz = window.tsc_mhz;
  total.render_tid = window.render_tid;
  total.render_handle = window.render_handle;
  total.wall_ms += window.wall_ms;
  const double render_cpu_before = total.render_cpu_ms;
#define REX_PERF_SPLIT_SUM_Count(name) total.name += window.name;
#define REX_PERF_SPLIT_SUM_Ms(name) total.name##_ms += window.name##_ms;
#define REX_PERF_SPLIT_SUM_Kb(name) total.name##_kb += window.name##_kb;
#define REX_PERF_SPLIT_SUM(name, kind) REX_PERF_SPLIT_SUM_##kind(name)
  REX_PERF_SPLIT_FIELDS(REX_PERF_SPLIT_SUM)
#undef REX_PERF_SPLIT_SUM
#undef REX_PERF_SPLIT_SUM_Kb
#undef REX_PERF_SPLIT_SUM_Ms
#undef REX_PERF_SPLIT_SUM_Count
  // Windows without a comparable render thread (-1) do not add to the total.
  if (!window.render_cpu_valid) {
    total.render_cpu_ms = render_cpu_before;
  }
  total.render_cpu_valid = total.render_cpu_valid || window.render_cpu_valid;
}

// Reason tokens are short ASCII words; anything else becomes '_'.
inline std::string SanitizePerfSplitReason(std::string_view reason) {
  std::string text;
  for (const char c : reason.substr(0, std::min<size_t>(reason.size(), 24))) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    text.push_back(ok ? c : '_');
  }
  return text.empty() ? std::string("none") : text;
}

// One log line, ASCII, single line. Milliseconds with two decimals.
inline std::string FormatPerfSplitLine(const PerfSplitWindow& w) {
  std::string line = fmt::format(
      "MCLA_PERF_SPLIT v={} reason={} seq={} wall_ms={:.1f} mode={} sample_div={} tsc_mhz={:.1f}",
      kPerfSplitVersion, SanitizePerfSplitReason(w.reason), w.seq, w.wall_ms, w.mode, w.sample_div,
      w.tsc_mhz);
  auto out = std::back_inserter(line);
#define REX_PERF_SPLIT_FORMAT_Count(name) fmt::format_to(out, " " #name "={}", w.name);
#define REX_PERF_SPLIT_FORMAT_Kb(name) fmt::format_to(out, " " #name "_kb={}", w.name##_kb);
#define REX_PERF_SPLIT_FORMAT_Ms(name)                                  \
  if (std::string_view(#name) == "render_cpu" && !w.render_cpu_valid) { \
    fmt::format_to(out, " render_cpu_ms=-1");                           \
  } else {                                                              \
    fmt::format_to(out, " " #name "_ms={:.2f}", w.name##_ms);           \
  }
#define REX_PERF_SPLIT_FORMAT(name, kind) REX_PERF_SPLIT_FORMAT_##kind(name)
  REX_PERF_SPLIT_FIELDS(REX_PERF_SPLIT_FORMAT)
#undef REX_PERF_SPLIT_FORMAT
#undef REX_PERF_SPLIT_FORMAT_Ms
#undef REX_PERF_SPLIT_FORMAT_Kb
#undef REX_PERF_SPLIT_FORMAT_Count
  fmt::format_to(out, " render_tid={} render_handle={:08X}", w.render_tid, w.render_handle);
  return line;
}

// ---------------------------------------------------------------------------
// Runtime state (rexruntime; shared by the GPU plugin, the kernel and the
// host). All functions are thread-safe.
// ---------------------------------------------------------------------------

// Published by the CP thread at each emission.
struct PerfSplitSnapshot {
  bool available = false;
  PerfSplitWindow last_window;
  PerfSplitWindow totals;
};

// Set by the CP thread when the d3d12_perf_split mode changes.
void SetPerfSplitActiveMode(uint32_t mode);
uint32_t PerfSplitActiveMode();
inline bool PerfSplitActive() {
  return PerfSplitActiveMode() != 0;
}

// VdSwap: records the calling (guest render) thread. One relaxed load per
// frame when unchanged.
void NotePerfSplitRenderThread(uint32_t guest_handle);
uint32_t PerfSplitRenderThreadId();
uint32_t PerfSplitRenderGuestHandle();

// Host sink for the guest render thread's GPU-serial wait (TSC ticks). Calls
// from other threads are counted separately.
void AddRenderSerialWaitTicks(uint64_t ticks);
void AddRenderSerialPollTicks(uint64_t ticks);
uint64_t RenderSerialWaitTicksTotal();
uint64_t RenderSerialPollTicksTotal();
uint64_t OtherThreadSerialWaitTicksTotal();

// F10 and other marks: the CP emits a reason=<tag> line at the next frame
// close. A second request before that is merged into the first.
void RequestPerfSplitMark(std::string_view reason);
bool PerfSplitMarkPending();
bool TakePerfSplitMark(std::string& reason);

// KI-047 diag v3 flight recorder: MarkPerfSplit also requests a dump of the
// livery diagnostics' kept lines, whether or not the split is active. The CP
// takes it at the next frame close. A second request before that is merged.
void RequestDiagFlightDump(std::string_view reason);
bool TakeDiagFlightDump(std::string& reason);

void PublishPerfSplitSnapshot(const PerfSplitWindow& window);
PerfSplitSnapshot GetPerfSplitSnapshot();

// CPUID 0x80000007 EDX bit 8.
bool HasInvariantTsc();

}  // namespace rex::perf

namespace rex::graphics {
// Host entry point for an F10 (or other) mark: same as
// rex::perf::RequestPerfSplitMark (no effect while the split is off), and
// always rex::perf::RequestDiagFlightDump.
void MarkPerfSplit(const char* tag);
}  // namespace rex::graphics
