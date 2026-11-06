/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    MCLA-R, 2026 - KI-069 command-processor freeze watchdog
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/util/cp_freeze_watchdog.h>
#include <rex/perf/perf_split.h>

#include <string>

namespace {

using rex::graphics::CpFreezeEpisodes;
using Action = rex::graphics::CpFreezeEpisodes::Action;
using rex::perf::CpScopeStack;
using rex::perf::PerfRegion;

constexpr uint64_t kMs = 1000000ull;

}  // namespace

TEST_CASE("freeze watchdog ignores the time before the first frame", "[cp-freeze]") {
  CpFreezeEpisodes episodes;
  REQUIRE(episodes.Observe(10000 * kMs, 0, 0, 1000) == Action::kNone);
  REQUIRE_FALSE(episodes.in_episode());
}

TEST_CASE("freeze watchdog logs one begin and one end per episode", "[cp-freeze]") {
  CpFreezeEpisodes episodes;
  // Frame 100 completed at 1000 ms.
  REQUIRE(episodes.Observe(1500 * kMs, 100, 1000 * kMs, 1000) == Action::kNone);
  REQUIRE(episodes.Observe(2000 * kMs, 100, 1000 * kMs, 1000) == Action::kNone);
  REQUIRE(episodes.Observe(2001 * kMs, 100, 1000 * kMs, 1000) == Action::kBegin);
  REQUIRE(episodes.in_episode());
  REQUIRE(episodes.episodes() == 1);
  REQUIRE(episodes.stall_ms() == 1001);
  // Still frozen: no second begin line.
  REQUIRE(episodes.Observe(4000 * kMs, 100, 1000 * kMs, 1000) == Action::kNone);
  REQUIRE(episodes.Observe(6500 * kMs, 100, 1000 * kMs, 1000) == Action::kNone);
  // The next frame completes at 6530 ms.
  REQUIRE(episodes.Observe(6600 * kMs, 101, 6530 * kMs, 1000) == Action::kEnd);
  REQUIRE(episodes.episode_ms() == 5530);
  REQUIRE_FALSE(episodes.in_episode());
  // Normal frames again.
  REQUIRE(episodes.Observe(6700 * kMs, 105, 6690 * kMs, 1000) == Action::kNone);
  // A second episode is counted separately.
  REQUIRE(episodes.Observe(8000 * kMs, 105, 6690 * kMs, 1000) == Action::kBegin);
  REQUIRE(episodes.episodes() == 2);
}

TEST_CASE("freeze watchdog threshold 0 is off but ends an open episode", "[cp-freeze]") {
  CpFreezeEpisodes episodes;
  REQUIRE(episodes.Observe(100000 * kMs, 5, 1 * kMs, 0) == Action::kNone);
  REQUIRE(episodes.Observe(3000 * kMs, 5, 1 * kMs, 1000) == Action::kBegin);
  REQUIRE(episodes.Observe(4000 * kMs, 6, 3500 * kMs, 0) == Action::kEnd);
}

TEST_CASE("freeze begin line format", "[cp-freeze]") {
  rex::graphics::CpFreezeReport r;
  r.episode = 1;
  r.stall_ms = 1001;
  r.frames = 20576;
  r.cp_tid = 1234;
  r.cp_state = 0;
  r.perf_split = true;
  r.scope = "copy>readback_gpu";
  r.scope_depth = 2;
  r.scope_ms = 990;
  r.copy_active = true;
  r.copy_seq = 77;
  r.copy_ms = 995;
  r.copy_address = 0x1F00000;
  r.copy_length = 4096;
  r.log_active = true;
  r.log_async = false;
  r.log_queue = 0;
  r.log_dropped = 0;
  r.log_op_ms = 980;
  r.log_op_thread = 1234;
  r.log_op_flush = true;
  r.log_writer = 99;
  REQUIRE(rex::graphics::FormatCpFreezeBegin(r) ==
          "MCLA_CP_FREEZE v=1 kind=begin episode=1 stall_ms=1001 frames=20576 cp_tid=1234 "
          "cp_state=busy perf_split=1 scope=copy>readback_gpu scope_depth=2 scope_ms=990 "
          "copy_active=1 copy_seq=77 copy_ms=995 copy_addr=01F00000 copy_len=4096 "
          "log_mode=sync log_queue=0 log_dropped=0 log_op=flush log_op_ms=980 "
          "log_op_thread=1234 log_writer=99");
  REQUIRE(rex::graphics::FormatCpFreezeEnd(1, 5530, 20577) ==
          "MCLA_CP_FREEZE v=1 kind=end episode=1 episode_ms=5530 frames=20577");
  REQUIRE(std::string(rex::graphics::CpWaitStateName(1)) == "ring_idle");
  REQUIRE(std::string(rex::graphics::CpWaitStateName(2)) == "wrm");
}

TEST_CASE("cp scope stack publishes the region path", "[cp-freeze]") {
  CpScopeStack stack;
  REQUIRE(rex::perf::FormatCpScopePath(stack.Read()) == "-");
  stack.Push(PerfRegion::kCopy, 100 * kMs);
  stack.Push(PerfRegion::kReadbackWait, 150 * kMs);
  auto snapshot = stack.Read();
  REQUIRE(snapshot.depth == 2);
  REQUIRE(rex::perf::FormatCpScopePath(snapshot) == "copy>readback_wait");
  REQUIRE(rex::perf::CpScopeInnermostMs(snapshot, 1150 * kMs) == 1000);
  stack.Pop();
  snapshot = stack.Read();
  REQUIRE(rex::perf::FormatCpScopePath(snapshot) == "copy");
  REQUIRE(rex::perf::CpScopeInnermostMs(snapshot, 1100 * kMs) == 1000);
  stack.Pop();
  stack.Pop();  // extra pops are ignored
  REQUIRE(stack.Read().depth == 0);
}

TEST_CASE("cp scope stack counts levels deeper than it records", "[cp-freeze]") {
  CpScopeStack stack;
  for (size_t i = 0; i < rex::perf::kCpScopeDepth + 2; ++i) {
    stack.Push(PerfRegion::kUpload, i * kMs);
  }
  const auto snapshot = stack.Read();
  REQUIRE(snapshot.depth == rex::perf::kCpScopeDepth + 2);
  const std::string path = rex::perf::FormatCpScopePath(snapshot);
  REQUIRE(path.substr(path.size() - 3) == ">+2");
  // The innermost recorded level is the last one that fit.
  REQUIRE(rex::perf::CpScopeInnermostMs(snapshot, 100 * kMs) == 100 - 7);
}

TEST_CASE("perf region names cover every region", "[cp-freeze]") {
  for (size_t i = 0; i < size_t(PerfRegion::kCount); ++i) {
    REQUIRE(rex::perf::kPerfRegionNames[i] != nullptr);
  }
  REQUIRE(std::string(rex::perf::kPerfRegionNames[size_t(PerfRegion::kReadbackWait)]) ==
          "readback_wait");
  REQUIRE(std::string(rex::perf::kPerfRegionNames[size_t(PerfRegion::kEmit)]) == "emit");
}
