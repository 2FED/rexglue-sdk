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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/perf/perf_split.h>

#include <string>

namespace {

using Catch::Approx;
using rex::perf::PerfDrawChain;
using rex::perf::PerfDrawExit;
using rex::perf::PerfPhase;
using rex::perf::PerfRegion;
using rex::perf::PerfSplitAccumulator;
using rex::perf::PerfSplitInputs;
using rex::perf::PerfSplitWindow;

size_t Index(PerfRegion region) {
  return size_t(region);
}
size_t Index(PerfPhase phase) {
  return size_t(phase);
}

}  // namespace

TEST_CASE("Perf split calibrates the TSC against steady time", "[perf][perf-split]") {
  // 3,000,000,000 ticks over one second = 3000 MHz.
  CHECK(rex::perf::TscMhz(3000000000ull, 1000000000ull) == Approx(3000.0));
  CHECK(rex::perf::TscMhz(123, 0) == 0.0);
  CHECK(rex::perf::TicksToMs(3000000, 3000.0) == Approx(1.0));
  CHECK(rex::perf::TicksToMs(3000000, 0.0) == 0.0);
}

TEST_CASE("Perf split normalizes the cvar mode", "[perf][perf-split]") {
  CHECK(rex::perf::NormalizePerfSplitMode(-3) == 0);
  CHECK(rex::perf::NormalizePerfSplitMode(0) == 0);
  CHECK(rex::perf::NormalizePerfSplitMode(1) == 1);
  CHECK(rex::perf::NormalizePerfSplitMode(2) == 2);
  CHECK(rex::perf::NormalizePerfSplitMode(9) == 2);
}

TEST_CASE("Perf split mode 1 samples about one draw in 16", "[perf][perf-split]") {
  PerfSplitAccumulator a;
  a.mode = 1;
  uint32_t sampled = 0;
  constexpr uint32_t kDraws = 160000;
  for (uint32_t i = 0; i < kDraws; ++i) {
    sampled += a.SampleNext() ? 1 : 0;
  }
  CHECK(sampled > kDraws / 16 * 9 / 10);
  CHECK(sampled < kDraws / 16 * 11 / 10);
  // No fixed period: a draw pattern of length 16 is not aliased.
  uint32_t by_slot[16] = {};
  PerfSplitAccumulator b;
  b.mode = 1;
  for (uint32_t i = 0; i < kDraws; ++i) {
    by_slot[i % 16] += b.SampleNext() ? 1 : 0;
  }
  for (uint32_t count : by_slot) {
    CHECK(count > 400);
  }
  PerfSplitAccumulator every;
  every.mode = 2;
  for (int i = 0; i < 100; ++i) {
    CHECK(every.SampleNext());
  }
}

TEST_CASE("Perf split scales sampled phases to all draws", "[perf][perf-split]") {
  CHECK(rex::perf::ScaleSampled(2.0, 1600, 100) == Approx(32.0));
  CHECK(rex::perf::ScaleSampled(2.0, 1600, 0) == 0.0);
}

TEST_CASE("Perf split regions are exclusive of nested regions", "[perf][perf-split]") {
  PerfSplitAccumulator a;
  // copy [100, 400) contains resolve [150, 250) which contains upload [160, 200).
  auto copy = a.Enter(100);
  auto resolve = a.Enter(150);
  auto upload = a.Enter(160);
  a.Exit(PerfRegion::kUpload, upload, 200);
  a.Exit(PerfRegion::kResolve, resolve, 250);
  a.Exit(PerfRegion::kCopy, copy, 400);
  const auto& c = a.counters;
  CHECK(c.region_excl_ticks[Index(PerfRegion::kUpload)] == 40);
  CHECK(c.region_excl_ticks[Index(PerfRegion::kResolve)] == 60);
  CHECK(c.region_excl_ticks[Index(PerfRegion::kCopy)] == 200);
  CHECK(c.region_incl_ticks[Index(PerfRegion::kCopy)] == 300);
  CHECK(c.region_calls[Index(PerfRegion::kUpload)] == 1);
  // Exclusive times add up to the outer duration.
  CHECK(a.charged == 300);
  CHECK(a.depth == 0);
}

TEST_CASE("Perf split phases subtract region time charged inside them", "[perf][perf-split]") {
  PerfSplitAccumulator a;
  a.mode = 2;
  {
    PerfDrawChain chain(&a, 1000);
    REQUIRE(chain.sampled());
    // A 30-tick fence wait inside the setup phase.
    auto fence = a.Enter(1010);
    a.Exit(PerfRegion::kFenceWait, fence, 1040);
    chain.MarkAt(PerfPhase::kSetup, 1100);
    chain.MarkAt(PerfPhase::kPrimitive, 1150);
    chain.SetExit(PerfDrawExit::kIssued);
    chain.FinishAt(1200);
  }
  const auto& c = a.counters;
  CHECK(c.phase_ticks[Index(PerfPhase::kSetup)] == 70);
  CHECK(c.phase_ticks[Index(PerfPhase::kPrimitive)] == 50);
  // The open phase (render targets) is closed at the early return.
  CHECK(c.phase_ticks[Index(PerfPhase::kRenderTargets)] == 50);
  CHECK(c.draws == 1);
  CHECK(c.sampled == 1);
  CHECK(c.draws_issued == 1);
  CHECK(c.in_draw_region_ticks == 30);
  CHECK_FALSE(a.in_draw);
}

TEST_CASE("Perf split counts draw exits and copy outcomes", "[perf][perf-split]") {
  PerfSplitAccumulator a;
  a.mode = 1;
  { PerfDrawChain chain(&a, 0); }
  {
    PerfDrawChain chain(&a, 0);
    chain.SetExit(PerfDrawExit::kPipelineSkip);
  }
  {
    PerfDrawChain chain(&a, 0);
    chain.SetExit(PerfDrawExit::kIssued);
  }
  CHECK(a.counters.draws == 3);
  CHECK(a.counters.draws_noop == 1);
  CHECK(a.counters.draws_pso_skip == 1);
  CHECK(a.counters.draws_issued == 1);
  a.CountCopy(rex::perf::ClassifyPerfCopy(true, true));
  a.CountCopy(rex::perf::ClassifyPerfCopy(false, false));
  a.CountCopy(rex::perf::ClassifyPerfCopy(true, false));
  CHECK(a.counters.copies_now == 1);
  CHECK(a.counters.copies_deferred == 1);
  CHECK(a.counters.copies_skipped == 1);
  // A null accumulator is a no-op.
  {
    PerfDrawChain off(nullptr, 0);
    off.Mark(PerfPhase::kSetup);
  }
  { rex::perf::ScopedPerfRegion off(nullptr, PerfRegion::kUpload); }
  CHECK(a.counters.draws == 3);
}

TEST_CASE("Perf split window converts, scales and closes the residual", "[perf][perf-split]") {
  PerfSplitInputs previous;
  previous.steady_ns = 1000000000ull;
  previous.tsc = 5000000000ull;
  previous.ring_idle_us = 100;
  PerfSplitInputs current = previous;
  // 60 s window at 2000 MHz.
  current.steady_ns += 60000000000ull;
  current.tsc += 120000000000ull;
  auto& c = current.counters;
  c.frames = 3600;
  c.draws = 1600;
  c.sampled = 100;
  c.draws_issued = 1500;
  // 2 ms of sampled setup time -> 32 ms scaled.
  c.phase_ticks[Index(PerfPhase::kSetup)] = 4000000;
  // 10 ms of fence wait, 5 ms of uploads (40 calls, 8 KiB).
  c.region_excl_ticks[Index(PerfRegion::kFenceWait)] = 20000000;
  c.region_excl_ticks[Index(PerfRegion::kUpload)] = 10000000;
  c.region_calls[Index(PerfRegion::kUpload)] = 40;
  c.upload_bytes = 8192;
  c.region_calls[Index(PerfRegion::kExecute)] = 120;
  c.region_incl_ticks[Index(PerfRegion::kReadbackWait)] = 2000000;
  current.ring_idle_us = 100 + 20000000;  // 20 s idle
  current.wrm_total_us = 5000000;         // 5 s WAIT_REG_MEM
  current.wrm_memory_us = 1000000;        // 1 s of it on memory
  current.cp_user_100ns = 300000000;      // 30 s
  current.cp_kernel_100ns = 10000000;     // 1 s
  current.gpu_busy_us = 45000000;
  current.render_tid = 42;
  previous.render_tid = 42;
  current.render_times_valid = true;
  previous.render_times_valid = true;
  current.render_user_100ns = 590000000;       // 59 s
  current.render_wait_ticks = 60000000000ull;  // 30 s

  const PerfSplitWindow w = rex::perf::ComputePerfSplitWindow(previous, current, 1);
  CHECK(w.wall_ms == Approx(60000.0));
  CHECK(w.tsc_mhz == Approx(2000.0));
  CHECK(w.frames == 3600);
  CHECK(w.p_setup_ms == Approx(32.0));
  CHECK(w.draw_ms == Approx(32.0));
  CHECK(w.fence_wait_ms == Approx(10.0));
  CHECK(w.upload_ms == Approx(5.0));
  CHECK(w.upload_calls == 40);
  CHECK(w.upload_kb == 8);
  CHECK(w.submissions == 120);
  CHECK(w.readback_wait_ms == Approx(1.0));
  CHECK(w.ring_idle_ms == Approx(20000.0));
  CHECK(w.wrm_mem_ms == Approx(1000.0));
  CHECK(w.wrm_reg_ms == Approx(4000.0));
  CHECK(w.cp_busy_ms == Approx(35000.0));
  CHECK(w.cp_cpu_ms == Approx(31000.0));
  CHECK(w.packets_other_ms == Approx(35000.0 - 32.0 - 15.0));
  CHECK(w.gpu_busy_ms == Approx(45000.0));
  CHECK(w.render_cpu_valid);
  CHECK(w.render_cpu_ms == Approx(59000.0));
  CHECK(w.render_wait_ms == Approx(30000.0));

  // A changed render thread makes its CPU time incomparable.
  PerfSplitInputs moved = current;
  moved.render_tid = 43;
  const PerfSplitWindow m = rex::perf::ComputePerfSplitWindow(previous, moved, 1);
  CHECK_FALSE(m.render_cpu_valid);
  CHECK(m.render_cpu_ms == -1.0);
}

TEST_CASE("Perf split totals accumulate windows", "[perf][perf-split]") {
  PerfSplitWindow total;
  PerfSplitWindow a;
  a.seq = 1;
  a.reason = "minute";
  a.wall_ms = 60000.0;
  a.draws = 10;
  a.fence_wait_ms = 1.5;
  a.render_cpu_valid = true;
  a.render_cpu_ms = 100.0;
  PerfSplitWindow b = a;
  b.seq = 2;
  b.reason = "f10";
  b.render_cpu_valid = false;
  b.render_cpu_ms = -1.0;
  rex::perf::AccumulatePerfSplitWindow(total, a);
  rex::perf::AccumulatePerfSplitWindow(total, b);
  CHECK(total.seq == 2);
  CHECK(total.reason == "f10");
  CHECK(total.wall_ms == Approx(120000.0));
  CHECK(total.draws == 20);
  CHECK(total.fence_wait_ms == Approx(3.0));
  CHECK(total.render_cpu_ms == Approx(100.0));
  CHECK(total.render_cpu_valid);
}

TEST_CASE("Perf split line has the v=1 header and every field once", "[perf][perf-split]") {
  PerfSplitWindow w;
  w.seq = 7;
  w.reason = "f10";
  w.mode = 1;
  w.wall_ms = 61234.56;
  w.tsc_mhz = 2995.04;
  w.draws = 400000;
  w.fence_wait_ms = 12.345;
  w.upload_kb = 77;
  w.render_tid = 1234;
  w.render_handle = 0xF80004E0u;
  w.render_cpu_valid = false;
  const std::string line = rex::perf::FormatPerfSplitLine(w);
  CHECK(line.rfind("MCLA_PERF_SPLIT v=1 reason=f10 seq=7 wall_ms=61234.6 mode=1 sample_div=16 "
                   "tsc_mhz=2995.0 frames=0 draws=400000 ",
                   0) == 0);
  CHECK(line.find(" fence_wait_ms=12.35 ") != std::string::npos);
  CHECK(line.find(" upload_kb=77 ") != std::string::npos);
  CHECK(line.find(" render_cpu_ms=-1 ") != std::string::npos);
  CHECK(line.find(" packets_other_ms=0.00 ") != std::string::npos);
  const std::string tail = " render_tid=1234 render_handle=F80004E0";
  REQUIRE(line.size() > tail.size());
  CHECK(line.substr(line.size() - tail.size()) == tail);
  CHECK(line.find('\n') == std::string::npos);
  bool printable = true;
  for (const char ch : line) {
    printable = printable && ch >= 32 && ch < 127;
  }
  CHECK(printable);
  const char* const keys[] = {
      " draws=",       " p_setup_ms=",    " p_record_ms=", " copy_ms=",
      " gpu_busy_ms=", " replay_cpu_ms=", " emit_ms=",     " render_wait_ms="};
  for (const char* key : keys) {
    const size_t first = line.find(key);
    REQUIRE(first != std::string::npos);
    CHECK(line.find(key, first + 1) == std::string::npos);
  }
  CHECK(rex::perf::SanitizePerfSplitReason("F10 now!") == "F10_now_");
  CHECK(rex::perf::SanitizePerfSplitReason("") == "none");
}

TEST_CASE("Perf split marks merge until taken", "[perf][perf-split]") {
  std::string reason;
  while (rex::perf::TakePerfSplitMark(reason)) {}
  rex::perf::SetPerfSplitActiveMode(0);
  rex::graphics::MarkPerfSplit("f10");
  CHECK_FALSE(rex::perf::PerfSplitMarkPending());
  rex::perf::SetPerfSplitActiveMode(1);
  rex::graphics::MarkPerfSplit("f10");
  rex::graphics::MarkPerfSplit("pad");
  CHECK(rex::perf::PerfSplitMarkPending());
  REQUIRE(rex::perf::TakePerfSplitMark(reason));
  CHECK(reason == "f10");
  CHECK_FALSE(rex::perf::TakePerfSplitMark(reason));
  rex::perf::SetPerfSplitActiveMode(0);
}

TEST_CASE("Perf split render-thread sink counts the swap thread", "[perf][perf-split]") {
  const uint64_t render_before = rex::perf::RenderSerialWaitTicksTotal();
  const uint64_t poll_before = rex::perf::RenderSerialPollTicksTotal();
  rex::perf::NotePerfSplitRenderThread(0xF80004E0u);
  CHECK(rex::perf::PerfSplitRenderGuestHandle() == 0xF80004E0u);
  CHECK(rex::perf::PerfSplitRenderThreadId() != 0);
  rex::perf::AddRenderSerialWaitTicks(100);
  rex::perf::AddRenderSerialPollTicks(7);
  CHECK(rex::perf::RenderSerialWaitTicksTotal() == render_before + 100);
  CHECK(rex::perf::RenderSerialPollTicksTotal() == poll_before + 7);
}

TEST_CASE("Perf split publishes totals for the host snapshot", "[perf][perf-split]") {
  PerfSplitWindow w;
  w.seq = 1;
  w.reason = "minute";
  w.wall_ms = 1000.0;
  w.draws = 5;
  const auto before = rex::perf::GetPerfSplitSnapshot();
  rex::perf::PublishPerfSplitSnapshot(w);
  const auto after = rex::perf::GetPerfSplitSnapshot();
  CHECK(after.available);
  CHECK(after.last_window.draws == 5);
  CHECK(after.totals.draws == before.totals.draws + 5);
  CHECK(after.totals.wall_ms == Approx(before.totals.wall_ms + 1000.0));
}
