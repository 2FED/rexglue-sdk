#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/readback_sync.h>
#include <rex/perf/perf_split.h>

#include <string>
#include <unordered_map>

namespace rex::graphics::test {

namespace {

struct LiveEntry {
  uint64_t last_used_frame = 0;
};
using LiveKeys = std::unordered_map<uint64_t, LiveEntry>;

uint64_t Key(uint32_t address, uint32_t length) {
  return (uint64_t(address) << 32) | length;
}

// A colour k_8_8_8_8 resolve from EDRAM base 0 (the livery composite family).
ReadbackSyncResolve Composite(uint32_t size, uint32_t address, uint32_t length) {
  ReadbackSyncResolve r;
  r.source_is_color = true;
  r.source_edram_base = 0;
  r.width = size;
  r.height = size;
  r.dest_format = kReadbackSyncFormat8888;
  r.bits_per_pixel = 32;
  r.address = address;
  r.length = length;
  r.frame = 1000;
  return r;
}

}  // namespace

TEST_CASE("Read-back sync rule b: sporadic live keys", "[graphics][readback-sync]") {
  ReadbackSyncResolve r = Composite(512, 0x050A0000, 0x100000);
  // A key without a live entry is a cache miss, which already waits.
  r.key_live = false;
  CHECK_FALSE(IsReadbackSyncSporadic(r, ReadbackSyncScope::kComposite));
  r.key_live = true;
  r.previous_frame = 993;
  CHECK_FALSE(IsReadbackSyncSporadic(r, ReadbackSyncScope::kComposite));
  r.previous_frame = 992;
  CHECK(IsReadbackSyncSporadic(r, ReadbackSyncScope::kComposite));
  r.previous_frame = 999;
  CHECK_FALSE(IsReadbackSyncSporadic(r, ReadbackSyncScope::kComposite));
  // A frame counter that went backwards counts as sporadic.
  r.previous_frame = 2000;
  CHECK(IsReadbackSyncSporadic(r, ReadbackSyncScope::kComposite));

  // The k_8 tiles from EDRAM base 224 every 49 frames (l54 log): only in the
  // "all" scope.
  ReadbackSyncResolve tile = r;
  tile.source_edram_base = 224;
  tile.width = 128;
  tile.height = 128;
  tile.dest_format = 2;
  tile.bits_per_pixel = 8;
  tile.previous_frame = 951;
  CHECK_FALSE(IsReadbackSyncSporadic(tile, ReadbackSyncScope::kComposite));
  CHECK(IsReadbackSyncSporadic(tile, ReadbackSyncScope::kAll));
  // k_8_8_8_8 from another EDRAM base is in the composite scope for rule b.
  tile.dest_format = kReadbackSyncFormat8888;
  tile.bits_per_pixel = 32;
  CHECK(IsReadbackSyncSporadic(tile, ReadbackSyncScope::kComposite));
  // Depth resolves are never in the composite scope.
  tile.source_is_color = false;
  CHECK_FALSE(IsReadbackSyncSporadic(tile, ReadbackSyncScope::kComposite));
  CHECK(IsReadbackSyncSporadic(tile, ReadbackSyncScope::kAll));
}

TEST_CASE("Read-back sync rule c: composite shape", "[graphics][readback-sync]") {
  for (uint32_t size : {64u, 128u, 256u, 512u, 1024u}) {
    CHECK(IsReadbackSyncCompositeShape(Composite(size, 0x050A0000, size * size * 4),
                                       ReadbackSyncScope::kComposite));
  }
  for (uint32_t size : {8u, 16u, 32u, 2048u, 96u, 768u}) {
    CHECK_FALSE(IsReadbackSyncCompositeShape(Composite(size, 0x050A0000, size * size * 4),
                                             ReadbackSyncScope::kComposite));
  }
  ReadbackSyncResolve r = Composite(1024, 0x050A0000, 0x400000);
  r.height = 512;
  CHECK_FALSE(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kComposite));
  r = Composite(1024, 0x050A0000, 0x400000);
  r.source_edram_base = 224;
  CHECK_FALSE(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kComposite));
  r = Composite(1024, 0x050A0000, 0x400000);
  r.source_is_color = false;
  CHECK_FALSE(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kAll));

  // The per-frame k_32_FLOAT 128x128 chain from EDRAM base 0 (l54 log): only
  // in the "all" scope, which asks for 32 bits per pixel instead of the
  // k_8_8_8_8 format.
  r = Composite(128, 0x073ED000, 0x10000);
  r.dest_format = 36;
  CHECK_FALSE(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kComposite));
  CHECK(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kAll));
  r.bits_per_pixel = 64;
  CHECK_FALSE(IsReadbackSyncCompositeShape(r, ReadbackSyncScope::kAll));
}

TEST_CASE("Read-back sync rule a: shared range of another length", "[graphics][readback-sync]") {
  CHECK(IsReadbackSyncSharedRange(0x050A0000, 0x100000, 0x050A0000, 0x400000));
  CHECK(IsReadbackSyncSharedRange(0x051F6000, 0x1000, 0x050A0000, 0x400000));
  // The same length is the same key shape, not a shared range.
  CHECK_FALSE(IsReadbackSyncSharedRange(0x050A0000, 0x100000, 0x050B0000, 0x100000));
  CHECK_FALSE(IsReadbackSyncSharedRange(0x054A0000, 0x100000, 0x050A0000, 0x400000));
  CHECK_FALSE(IsReadbackSyncSharedRange(0x050A0000, 0, 0x050A0000, 0x400000));

  CHECK(ReadbackSyncSharedRangeApplies(Composite(8, 0x051F6000, 0x1000),
                                       ReadbackSyncScope::kComposite));
  ReadbackSyncResolve other_base = Composite(256, 0x06EB5000, 0x40000);
  other_base.source_edram_base = 224;
  CHECK_FALSE(ReadbackSyncSharedRangeApplies(other_base, ReadbackSyncScope::kComposite));
  CHECK(ReadbackSyncSharedRangeApplies(other_base, ReadbackSyncScope::kAll));
}

TEST_CASE("Read-back sync rules on the l54 composite sequence", "[graphics][readback-sync]") {
  // Frame 893 of the l54 cold session: level 0 (1024x1024, 4 MB) is live at
  // 050A0000, then the 512..8 mips are resolved into the same scratch range.
  LiveKeys live;
  live[Key(0x050A0000, 0x400000)].last_used_frame = 877;
  struct Mip {
    uint32_t size;
    uint32_t address;
    uint32_t length;
  };
  const Mip mips[] = {{512, 0x050A0000, 0x100000}, {256, 0x051A0000, 0x40000},
                      {128, 0x051E0000, 0x10000},  {64, 0x051F0000, 0x4000},
                      {32, 0x051F4000, 0x1000},    {16, 0x051F5000, 0x1000},
                      {8, 0x051F6000, 0x1000},     {8, 0x051F7000, 0x1000}};
  for (const Mip& mip : mips) {
    ReadbackSyncResolve r = Composite(mip.size, mip.address, mip.length);
    r.frame = 893;
    ReadbackSyncRules rules = EvaluateReadbackSyncRules(r, ReadbackSyncScope::kComposite, live,
                                                        Key(mip.address, mip.length));
    CHECK(rules.shared_range);
    CHECK(rules.other_key == Key(0x050A0000, 0x400000));
    CHECK(rules.other_last_used_frame == 877);
    CHECK(rules.other_keys == 1);
    CHECK(rules.composite_shape == (mip.size >= 64));
    CHECK(rules.composite());
    CHECK(ShouldForceReadbackSync(true, true, false, rules));
    live[Key(mip.address, mip.length)].last_used_frame = 893;
  }

  // Frame 5946: level 1 (512x512) at 050A0000 was deferred and read the
  // previous slot; with the rules it is forced (shape, and the live level 0).
  ReadbackSyncResolve level1 = Composite(512, 0x050A0000, 0x100000);
  level1.frame = 5946;
  level1.key_live = true;
  level1.previous_frame = 5940;
  live[Key(0x050A0000, 0x400000)].last_used_frame = 5946;
  ReadbackSyncRules rules = EvaluateReadbackSyncRules(level1, ReadbackSyncScope::kComposite, live,
                                                      Key(0x050A0000, 0x100000));
  CHECK(rules.composite_shape);
  CHECK(rules.shared_range);
  CHECK_FALSE(rules.sporadic);
  CHECK(std::string(ReadbackSyncRuleNames(rules)) == "ac");
}

TEST_CASE("Read-back sync rules leave routine per-frame overlaps alone",
          "[graphics][readback-sync]") {
  // l54: k_8_8_8_8 256x256 tiles from EDRAM base 224 inside the 640x640
  // depth key, and k_32_FLOAT chains from base 0 inside another depth key,
  // about 21 overlapping resolves per frame.
  LiveKeys live;
  live[Key(0x06E79000, 3194880)].last_used_frame = 1000;
  live[Key(0x07199000, 3194880)].last_used_frame = 1000;

  ReadbackSyncResolve tile = Composite(256, 0x06EB5000, 0x40000);
  tile.source_edram_base = 224;
  tile.key_live = true;
  tile.previous_frame = 999;
  ReadbackSyncRules rules = EvaluateReadbackSyncRules(tile, ReadbackSyncScope::kComposite, live,
                                                      Key(tile.address, tile.length));
  CHECK_FALSE(rules.any());
  rules = EvaluateReadbackSyncRules(tile, ReadbackSyncScope::kAll, live,
                                    Key(tile.address, tile.length));
  CHECK(rules.shared_range);
  CHECK_FALSE(rules.sporadic);

  ReadbackSyncResolve chain = Composite(128, 0x073ED000, 0x10000);
  chain.dest_format = 36;
  chain.key_live = true;
  chain.previous_frame = 999;
  rules = EvaluateReadbackSyncRules(chain, ReadbackSyncScope::kComposite, live,
                                    Key(chain.address, chain.length));
  CHECK_FALSE(rules.any());
  CHECK(std::string(ReadbackSyncRuleNames(rules)) == "none");
  rules = EvaluateReadbackSyncRules(chain, ReadbackSyncScope::kAll, live,
                                    Key(chain.address, chain.length));
  CHECK(rules.shared_range);
  CHECK(rules.composite_shape);
}

TEST_CASE("Read-back sync evaluation skips its own key and picks the latest other",
          "[graphics][readback-sync]") {
  LiveKeys live;
  live[Key(0x050A0000, 0x100000)].last_used_frame = 990;  // self
  live[Key(0x050A0000, 0x400000)].last_used_frame = 900;
  live[Key(0x05000000, 0x200000)].last_used_frame = 950;
  live[Key(0x06000000, 0x200000)].last_used_frame = 999;  // no overlap
  ReadbackSyncResolve r = Composite(512, 0x050A0000, 0x100000);
  ReadbackSyncRules rules =
      EvaluateReadbackSyncRules(r, ReadbackSyncScope::kComposite, live, Key(0x050A0000, 0x100000));
  CHECK(rules.shared_range);
  CHECK(rules.other_keys == 2);
  CHECK(rules.other_key == Key(0x05000000, 0x200000));
  CHECK(rules.other_last_used_frame == 950);

  LiveKeys only_self;
  only_self[Key(0x050A0000, 0x100000)].last_used_frame = 990;
  rules = EvaluateReadbackSyncRules(r, ReadbackSyncScope::kComposite, only_self,
                                    Key(0x050A0000, 0x100000));
  CHECK_FALSE(rules.shared_range);
  CHECK(rules.other_keys == 0);
}

TEST_CASE("Read-back sync is forced only when on, delayed and not a miss",
          "[graphics][readback-sync]") {
  ReadbackSyncRules none;
  ReadbackSyncRules shape;
  shape.composite_shape = true;
  CHECK(ShouldForceReadbackSync(true, true, false, shape));
  CHECK_FALSE(ShouldForceReadbackSync(false, true, false, shape));
  CHECK_FALSE(ShouldForceReadbackSync(true, false, false, shape));
  CHECK_FALSE(ShouldForceReadbackSync(true, true, true, shape));
  CHECK_FALSE(ShouldForceReadbackSync(true, true, false, none));
  ReadbackSyncRules sporadic;
  sporadic.sporadic = true;
  CHECK(ShouldForceReadbackSync(true, true, false, sporadic));
  CHECK_FALSE(sporadic.composite());
  CHECK(std::string(ReadbackSyncRuleNames(sporadic)) == "b");
  ReadbackSyncRules all;
  all.shared_range = all.sporadic = all.composite_shape = true;
  CHECK(std::string(ReadbackSyncRuleNames(all)) == "abc");

  CHECK(NormalizeReadbackSyncScope(0) == ReadbackSyncScope::kComposite);
  CHECK(NormalizeReadbackSyncScope(1) == ReadbackSyncScope::kAll);
  CHECK(NormalizeReadbackSyncScope(7) == ReadbackSyncScope::kComposite);
  CHECK(std::string(ReadbackSyncScopeName(ReadbackSyncScope::kAll)) == "all");
}

TEST_CASE("Read-back sync counters count once per resolve and once per rule",
          "[graphics][readback-sync]") {
  ReadbackSyncCounters c;
  ReadbackSyncRules ac;
  ac.shared_range = true;
  ac.composite_shape = true;
  ReadbackSyncRules b;
  b.sporadic = true;
  c.CountMatched(ac);
  c.CountMatched(b);
  c.CountForced(ac, 1500);
  c.CountForced(b, 4000);
  CHECK(c.matched == 2);
  CHECK(c.matched_rule_a == 1);
  CHECK(c.matched_rule_b == 1);
  CHECK(c.matched_rule_c == 1);
  CHECK(c.forced == 2);
  CHECK(c.forced_rule_a == 1);
  CHECK(c.forced_rule_b == 1);
  CHECK(c.forced_rule_c == 1);
  CHECK(c.wait_us == 5500);
  CHECK(c.max_wait_us == 4000);
}

TEST_CASE("Read-back sync targeted wait covers the copy's own submission",
          "[graphics][readback-sync][readback-targeted]") {
  // Only with the switch on and an open submission (the copy is in it).
  CHECK(ShouldUseReadbackTargetedWait(true, true));
  CHECK_FALSE(ShouldUseReadbackTargetedWait(true, false));
  CHECK_FALSE(ShouldUseReadbackTargetedWait(false, true));
  CHECK_FALSE(ShouldUseReadbackTargetedWait(false, false));

  // The copy was recorded into submission 41 (submission_current_ = 41).
  // Ending it advanced the index to 42; the fence reached 41.
  CHECK(ClassifyReadbackTargetedWait(41, 42, 41) == ReadbackTargetedResult::kCompleted);
  // A later completion also covers it (in-order direct queue).
  CHECK(ClassifyReadbackTargetedWait(41, 42, 43) == ReadbackTargetedResult::kCompleted);
  // The fence is still behind the copy: not satisfied, not copied.
  CHECK(ClassifyReadbackTargetedWait(41, 42, 40) == ReadbackTargetedResult::kFenceFailed);
  // Ending the submission failed (index unchanged): the full wait decides.
  CHECK(ClassifyReadbackTargetedWait(41, 41, 40) == ReadbackTargetedResult::kFallbackToFull);
  CHECK(ClassifyReadbackTargetedWait(41, 41, 41) == ReadbackTargetedResult::kFallbackToFull);

  // Same condition as AwaitAllQueueOperationsCompletion once the copy's
  // submission is the last one: completed + 1 >= current.
  for (uint64_t completed = 38; completed <= 43; ++completed) {
    CHECK(ReadbackTargetedWaitSatisfied(completed, 41) == (completed + 1 >= 42));
  }
  CHECK(ReadbackTargetedSubmissionEnded(41, 42));
  CHECK_FALSE(ReadbackTargetedSubmissionEnded(41, 41));
}

TEST_CASE("Read-back sync kick only when on, past the draw count and the GPU is idle",
          "[graphics][readback-sync][readback-targeted]") {
  ReadbackKickInput in;
  in.targeted = true;
  in.sync_enabled = true;
  in.kick_draws = 512;
  in.submission_open = true;
  in.draws_in_submission = 512;
  in.can_end_immediately = true;
  in.occlusion_query_active = false;
  in.submission_completed = 9;
  in.submission_current = 10;
  CHECK(ShouldKickReadbackSyncSubmission(in));

  ReadbackKickInput t = in;
  t.targeted = false;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  t = in;
  t.sync_enabled = false;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  t = in;
  t.kick_draws = 0;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  t = in;
  t.submission_open = false;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  t = in;
  t.draws_in_submission = 511;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  t.draws_in_submission = 5000;
  CHECK(ShouldKickReadbackSyncSubmission(t));
  // Pipelines being created: the primary-buffer-end submission waits too.
  t = in;
  t.can_end_immediately = false;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  // Ending a submission ends an active host occlusion query.
  t = in;
  t.occlusion_query_active = true;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
  // The GPU still runs submission 9: it has work, no kick.
  t = in;
  t.submission_completed = 8;
  CHECK_FALSE(ShouldKickReadbackSyncSubmission(t));
}

TEST_CASE("Read-back sync targeted counters", "[graphics][readback-sync][readback-targeted]") {
  ReadbackSyncCounters c;
  ReadbackTargetedWait a;
  a.submit_us = 300;
  a.fence_us = 2700;
  a.gpu_busy_at_submit = true;
  a.backlog_draws = 400;
  ReadbackTargetedWait b;
  b.submit_us = 100;
  b.fence_us = 900;
  b.backlog_draws = 120;
  c.CountTargeted(a);
  c.CountTargeted(b);
  CHECK(c.targeted_waits == 2);
  CHECK(c.targeted_wait_us == 4000);
  CHECK(c.targeted_max_wait_us == 3000);
  CHECK(c.targeted_submit_us == 400);
  CHECK(c.targeted_fence_us == 3600);
  CHECK(c.targeted_gpu_busy == 1);
  CHECK(c.targeted_backlog_draws == 520);
  CHECK(c.fallbacks_to_full == 0);
  CHECK(c.kicks == 0);
  // The existing fields are untouched by the targeted counts.
  CHECK(c.forced == 0);
  CHECK(c.wait_us == 0);
}

TEST_CASE("Paced line budget: burst, then lines per second", "[graphics][livery-diag2]") {
  PacedLineBudget budget(2, 4);
  int allowed = 0;
  for (int i = 0; i < 10; ++i) {
    allowed += budget.Allow(1000) ? 1 : 0;
  }
  CHECK(allowed == 4);
  CHECK(budget.TakeSuppressed() == 6);
  CHECK(budget.TakeSuppressed() == 0);
  // 2 per second: one token after 500 ms, none after 499 more.
  CHECK(budget.Allow(1500));
  CHECK_FALSE(budget.Allow(1999));
  CHECK(budget.Allow(2000));
  CHECK_FALSE(budget.Allow(2000));
  // A long quiet period refills at most the burst.
  allowed = 0;
  for (int i = 0; i < 10; ++i) {
    allowed += budget.Allow(100000) ? 1 : 0;
  }
  CHECK(allowed == 4);
  // A clock that goes backwards does not refill.
  CHECK_FALSE(budget.Allow(50000));
  CHECK_FALSE(budget.Allow(50000));
  CHECK(budget.Allow(50500));
  // The real budgets.
  PacedLineBudget real(kLiveryDiag2LinesPerSecond, kLiveryDiag2LineBurst);
  allowed = 0;
  for (uint64_t ms = 0; ms < 60000; ms += 10) {
    allowed += real.Allow(ms) ? 1 : 0;
  }
  // The burst plus about 2 per second over the minute.
  CHECK(allowed >= int(kLiveryDiag2LineBurst + 2 * 59));
  CHECK(allowed <= int(kLiveryDiag2LineBurst + 2 * 60));
}

TEST_CASE("Flight recorder ring keeps the last 64 lines in order", "[graphics][livery-diag2]") {
  DiagLineRing ring;
  uint64_t pushed = 99;
  CHECK(ring.Snapshot(&pushed).empty());
  CHECK(pushed == 0);
  for (int i = 0; i < 10; ++i) {
    ring.Push("line" + std::to_string(i));
  }
  std::vector<std::string> lines = ring.Snapshot(&pushed);
  REQUIRE(lines.size() == 10);
  CHECK(lines.front() == "line0");
  CHECK(lines.back() == "line9");
  CHECK(pushed == 10);
  for (int i = 10; i < 100; ++i) {
    ring.Push("line" + std::to_string(i));
  }
  lines = ring.Snapshot(&pushed);
  REQUIRE(lines.size() == DiagLineRing::kCapacity);
  CHECK(lines.front() == "line36");
  CHECK(lines.back() == "line99");
  CHECK(pushed == 100);
  // A snapshot does not remove the lines (a second F10 shows them again).
  CHECK(ring.Snapshot().size() == DiagLineRing::kCapacity);
  ring.Clear();
  CHECK(ring.Snapshot().empty());
}

TEST_CASE("Texture tracker keeps atlas lines for the flight recorder", "[graphics][livery-diag2]") {
  LiveryDiag2TextureTracker tracker;
  tracker.SetEnabled(true);
  tracker.atlas_lines().Push("kind=tex_load frame=1");
  CHECK(tracker.atlas_lines().Snapshot().size() == 1);
  // Disabling and enabling clears the ring with the rest of the state.
  tracker.SetEnabled(false);
  tracker.SetEnabled(true);
  CHECK(tracker.atlas_lines().Snapshot().empty());
}

TEST_CASE("Key history remembers whether the last resolve was copied", "[graphics][livery-diag2]") {
  ReadbackKeyHistory history;
  history.Record(7, 10, true);
  CHECK(history.Get(7).last_copied);
  history.Record(7, 11, false);
  ReadbackKeyHistory::Lookup lookup = history.Get(7);
  CHECK_FALSE(lookup.last_copied);
  CHECK(lookup.copied_before);
  CHECK(lookup.last_copy_frame == 10);
}

TEST_CASE("F10 marks request a flight-recorder dump even with the split off",
          "[graphics][livery-diag2][perf-split]") {
  std::string reason;
  while (rex::perf::TakeDiagFlightDump(reason)) {}
  while (rex::perf::TakePerfSplitMark(reason)) {}
  rex::perf::SetPerfSplitActiveMode(0);
  rex::graphics::MarkPerfSplit("f10");
  CHECK_FALSE(rex::perf::PerfSplitMarkPending());
  rex::graphics::MarkPerfSplit("f10 again");
  REQUIRE(rex::perf::TakeDiagFlightDump(reason));
  CHECK(reason == "f10");
  CHECK_FALSE(rex::perf::TakeDiagFlightDump(reason));
  rex::perf::RequestDiagFlightDump("x y");
  REQUIRE(rex::perf::TakeDiagFlightDump(reason));
  CHECK(reason == "x_y");
}

}  // namespace rex::graphics::test
