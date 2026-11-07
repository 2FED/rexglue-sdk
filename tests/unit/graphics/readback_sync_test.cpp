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

TEST_CASE("Read-back sync steady run counts consecutive frames",
          "[graphics][readback-sync][readback-steady]") {
  // A key without a live entry, or with no run yet, starts at 1.
  CHECK(NextReadbackConsecutiveFrames(false, 0, 0, 5) == 1);
  CHECK(NextReadbackConsecutiveFrames(false, 4, 7, 5) == 1);
  CHECK(NextReadbackConsecutiveFrames(true, 4, 0, 5) == 1);
  // The next frame extends the run, the same frame keeps it.
  CHECK(NextReadbackConsecutiveFrames(true, 4, 7, 5) == 8);
  CHECK(NextReadbackConsecutiveFrames(true, 5, 8, 5) == 8);
  // A gap of one frame (or a counter that went backwards) resets it.
  CHECK(NextReadbackConsecutiveFrames(true, 3, 7, 5) == 1);
  CHECK(NextReadbackConsecutiveFrames(true, 9, 7, 5) == 1);
  // Saturates.
  CHECK(NextReadbackConsecutiveFrames(true, 4, UINT32_MAX, 5) == UINT32_MAX);
}

TEST_CASE("Read-back sync steady exemption: rule c alone after 30 frames",
          "[graphics][readback-sync][readback-steady]") {
  CHECK(kReadbackSyncSteadyMinFrames == 30);
  ReadbackSyncRules c_only;
  c_only.composite_shape = true;
  ReadbackSyncRules a_and_c = c_only;
  a_and_c.shared_range = true;
  ReadbackSyncRules b_and_c = c_only;
  b_and_c.sporadic = true;
  ReadbackSyncRules a_only;
  a_only.shared_range = true;
  ReadbackSyncRules none;
  for (uint32_t run = 0; run <= 31; ++run) {
    INFO("run " << run);
    CHECK(IsReadbackSyncSteadyExempt(c_only, run) == (run >= 30));
    CHECK_FALSE(IsReadbackSyncSteadyExempt(a_and_c, run));
    CHECK_FALSE(IsReadbackSyncSteadyExempt(b_and_c, run));
    CHECK_FALSE(IsReadbackSyncSteadyExempt(a_only, run));
    CHECK_FALSE(IsReadbackSyncSteadyExempt(none, run));
  }
}

TEST_CASE("Read-back sync steady exemption removes only the rule c force",
          "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncRules rules;
  rules.composite_shape = true;
  CHECK(rules.forces());
  CHECK(ShouldForceReadbackSync(true, true, false, rules));
  rules.steady_exempt = true;
  // Still matched (any() and the rule names keep rule c for the counters).
  CHECK(rules.any());
  CHECK(std::string(ReadbackSyncRuleNames(rules)) == "c");
  CHECK_FALSE(rules.forces());
  CHECK_FALSE(ShouldForceReadbackSync(true, true, false, rules));
  // Rules a and b still force even with the flag set.
  ReadbackSyncRules with_a = rules;
  with_a.shared_range = true;
  CHECK(ShouldForceReadbackSync(true, true, false, with_a));
  ReadbackSyncRules with_b = rules;
  with_b.sporadic = true;
  CHECK(ShouldForceReadbackSync(true, true, false, with_b));
}

TEST_CASE("Read-back sync steady exemption on frame sequences",
          "[graphics][readback-sync][readback-steady]") {
  // The steady 256x256 key at 03702000: resolved every frame, rule c only.
  ReadbackSyncResolve steady = Composite(256, 0x03702000, 0x40000);
  LiveKeys live;
  const uint64_t steady_key = Key(0x03702000, 0x40000);
  bool was_live = false;
  uint64_t previous_frame = 0;
  uint32_t run = 0;
  auto resolve = [&](ReadbackSyncResolve r, uint64_t key, uint64_t frame) {
    r.frame = frame;
    r.key_live = was_live;
    r.previous_frame = previous_frame;
    run = NextReadbackConsecutiveFrames(was_live, previous_frame, run, frame);
    r.consecutive_frames = run;
    ReadbackSyncRules rules =
        EvaluateReadbackSyncRules(r, ReadbackSyncScope::kComposite, live, key);
    rules.steady_exempt = IsReadbackSyncSteadyExempt(rules, r.consecutive_frames);
    was_live = true;
    previous_frame = frame;
    live[key].last_used_frame = frame;
    return rules;
  };
  for (uint64_t frame = 100; frame < 129; ++frame) {
    ReadbackSyncRules rules = resolve(steady, steady_key, frame);
    INFO("frame " << frame);
    CHECK(rules.composite_shape);
    CHECK_FALSE(rules.steady_exempt);
  }
  // Frame 129 is the 30th consecutive frame.
  ReadbackSyncRules rules = resolve(steady, steady_key, 129);
  CHECK(rules.steady_exempt);
  CHECK_FALSE(ShouldForceReadbackSync(true, true, false, rules));
  // A second resolve in the same frame stays exempt.
  CHECK(resolve(steady, steady_key, 129).steady_exempt);
  CHECK(resolve(steady, steady_key, 130).steady_exempt);
  // A gap of one frame: the next resolve is forced again (a one-shot
  // composite re-using the key after the run).
  rules = resolve(steady, steady_key, 132);
  CHECK(run == 1);
  CHECK_FALSE(rules.steady_exempt);
  CHECK(ShouldForceReadbackSync(true, true, false, rules));

  // A livery burst of 3 consecutive frames (the longest seen) is never exempt.
  was_live = false;
  previous_frame = 0;
  run = 0;
  live.clear();
  ReadbackSyncResolve livery = Composite(512, 0x050A0000, 0x100000);
  for (uint64_t frame = 500; frame < 503; ++frame) {
    rules = resolve(livery, Key(0x050A0000, 0x100000), frame);
    CHECK_FALSE(rules.steady_exempt);
    CHECK(rules.forces());
  }
  CHECK(run == 3);

  // A steady key whose range is shared by another live key of a different
  // length (rule a) is forced whatever its run.
  was_live = true;
  previous_frame = 999;
  run = 100;
  live.clear();
  live[Key(0x03702000, 0x10000)].last_used_frame = 990;
  rules = resolve(steady, steady_key, 1000);
  CHECK(run == 101);
  CHECK(rules.shared_range);
  CHECK_FALSE(rules.steady_exempt);
  CHECK(ShouldForceReadbackSync(true, true, false, rules));
}

TEST_CASE("Read-back sync steady end reasons", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncRules c_only;
  c_only.composite_shape = true;
  CHECK(std::string(ReadbackSyncSteadyEndReason(false, c_only, 50)) == "disabled");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 1)) == "gap");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 30)) == "none");
  ReadbackSyncRules a = c_only;
  a.shared_range = true;
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, a, 50)) == "rule_a");
  ReadbackSyncRules b = c_only;
  b.sporadic = true;
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, b, 50)) == "rule_b");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, ReadbackSyncRules(), 50)) == "not_c");
}

TEST_CASE("Read-back sync steady counters and report",
          "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncCounters c;
  // Off and nothing exempt: the minute line is unchanged.
  c.CountSteadyRun(12);
  CHECK(ReadbackSyncSteadyReport(c, false).empty());
  CHECK(ReadbackSyncSteadyReport(c, true) ==
        " steady=1 steady_min_frames=30 steady_exempt=0 steady_exempt_keys=0 steady_max_run=12");
  c.CountSteadyRun(3);
  CHECK(c.steady_max_run == 12);
  c.CountSteadyExempt(true);
  c.CountSteadyExempt(false);
  c.CountSteadyExempt(false);
  c.CountSteadyRun(1500);
  CHECK(ReadbackSyncSteadyReport(c, true) ==
        " steady=1 steady_min_frames=30 steady_exempt=3 steady_exempt_keys=1 "
        "steady_max_run=1500");
  // Switched off within the minute after counting: still reported.
  CHECK(ReadbackSyncSteadyReport(c, false) ==
        " steady=0 steady_min_frames=30 steady_exempt=3 steady_exempt_keys=1 "
        "steady_max_run=1500");
  // The existing counters are untouched.
  CHECK(c.matched == 0);
  CHECK(c.forced == 0);
}

TEST_CASE("Read-back sync steady key line", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncSteadyKeyEvent e;
  e.exempt = true;
  e.reason = "run";
  e.frame = 4321;
  e.draw = 26;
  e.address = 0x03702000;
  e.length = 0x40000;
  e.width = 256;
  e.height = 256;
  e.dest_format = 6;
  e.source_edram_base = 0;
  e.run = 30;
  CHECK(FormatReadbackSyncSteadyKeyEvent(e) ==
        "kind=steady_key event=exempt reason=run frame=4321 draw=26 base=03702000 len=262144 "
        "size=256x256 fmt=6 src_base=0 run=30 min_frames=30 suppressed=0");
  e.exempt = false;
  e.reason = "gap";
  e.run = 812;
  e.suppressed = 2;
  CHECK(FormatReadbackSyncSteadyKeyEvent(e) ==
        "kind=steady_key event=end reason=gap frame=4321 draw=26 base=03702000 len=262144 "
        "size=256x256 fmt=6 src_base=0 run=812 min_frames=30 suppressed=2");
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

// M11-007b: sticky, shape and cap filters.

namespace {

struct ShapeEntry {
  bool shape_known = true;
  ReadbackSyncKeyShape shape;
};
using ShapeKeys = std::unordered_map<uint64_t, ShapeEntry>;

ReadbackSyncKeyShape Shape(uint32_t address, uint32_t size, uint32_t format = 6,
                           uint32_t src_base = 0) {
  ReadbackSyncKeyShape s;
  s.address = address;
  s.width = size;
  s.height = size;
  s.length = size * size * 4;
  s.dest_format = format;
  s.source_edram_base = src_base;
  return s;
}

void AddShape(ShapeKeys& keys, const ReadbackSyncKeyShape& s) {
  keys[Key(s.address, s.length)].shape = s;
}

}  // namespace

TEST_CASE("Read-back sync steady chain neighbour", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncKeyShape k256 = Shape(0x039CA000, 256);
  REQUIRE(k256.length == 0x40000);
  // The key itself.
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, k256));
  // The next smaller level after the key.
  CHECK(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x40000, 128)));
  // The next larger level before the key (512x512 = 1 MB ending at it).
  CHECK(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 - 0x100000, 512)));
  // A half-size key ending at the key is not a level of its chain.
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039BA000, 128)));
  // A double-size key starting right after it is not either.
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x40000, 512)));
  // Adjacent same-size keys are not levels.
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x40000, 256)));
  // Another format or EDRAM source base does not count for adjacency.
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x40000, 128, 2)));
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x40000, 128, 6, 224)));
  // Overlap of the same format counts whatever the source base and size; of
  // another format it does not.
  CHECK(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000, 1024, 6, 224)));
  CHECK(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000 + 0x3F000, 64)));
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, Shape(0x039CA000, 1024, 2)));
  // Not square (width only halved): not a level.
  ReadbackSyncKeyShape wide = Shape(0x039CA000 + 0x40000, 128);
  wide.height = 256;
  CHECK_FALSE(IsReadbackSyncChainNeighbour(k256, wide));
  // Address arithmetic does not wrap at the top of the address space.
  ReadbackSyncKeyShape top = Shape(0xFFFC0000, 256);
  CHECK_FALSE(IsReadbackSyncChainNeighbour(top, Shape(0x00000000, 128)));
}

TEST_CASE("Read-back sync steady shape filter on the l74 key sets",
          "[graphics][readback-sync][readback-steady]") {
  // Race-time keys: the steady 256x256 start-of-frame resolve at 039CA000,
  // the 128x128 key ending exactly at it, and the round-robin k_8 tiles
  // from EDRAM base 224.
  ShapeKeys race;
  ReadbackSyncKeyShape steady = Shape(0x039CA000, 256);
  AddShape(race, steady);
  AddShape(race, Shape(0x039BA000, 128));
  ReadbackSyncKeyShape tile128 = Shape(0x03A0A000, 128, 2, 224);
  tile128.length = 128 * 128;
  AddShape(race, tile128);
  ReadbackSyncKeyShape tile256 = Shape(0x03A0E000, 256, 2, 224);
  tile256.length = 256 * 256;
  AddShape(race, tile256);
  CHECK_FALSE(HasReadbackSyncChainNeighbour(steady, race));

  // The pre-race showcase adds the player-car livery chain (512 at +0, 256
  // at +1 MB, 128 at +1.25 MB, 64 at +1.31 MB): none of its levels may be
  // exempt, and 039CA000 stays exempt.
  ShapeKeys showcase = race;
  const ReadbackSyncKeyShape chain[] = {Shape(0x05368000, 512), Shape(0x05468000, 256),
                                        Shape(0x054A8000, 128), Shape(0x054B8000, 64)};
  REQUIRE(chain[0].address + chain[0].length == chain[1].address);
  REQUIRE(chain[1].address + chain[1].length == chain[2].address);
  REQUIRE(chain[2].address + chain[2].length == chain[3].address);
  for (const ReadbackSyncKeyShape& level : chain) {
    AddShape(showcase, level);
  }
  CHECK_FALSE(HasReadbackSyncChainNeighbour(steady, showcase));
  for (const ReadbackSyncKeyShape& level : chain) {
    INFO("level " << level.width);
    CHECK(HasReadbackSyncChainNeighbour(level, showcase));
  }
  // The 1024 level at the same base alone (other levels evicted) also keeps
  // the 512 level out (overlap).
  ShapeKeys overlap;
  AddShape(overlap, Shape(0x05368000, 1024));
  AddShape(overlap, chain[0]);
  CHECK(HasReadbackSyncChainNeighbour(chain[0], overlap));
  // An entry whose shape is not known is ignored.
  ShapeKeys unknown;
  AddShape(unknown, chain[1]);
  unknown.begin()->second.shape_known = false;
  CHECK_FALSE(HasReadbackSyncChainNeighbour(chain[0], unknown));
  // A lone 256x256 fmt 6 key is not refused by the shape filter: the
  // allowlist of the size alone would not separate it from the chain's 256
  // level, the neighbours do.
  ShapeKeys lone;
  AddShape(lone, chain[1]);
  CHECK_FALSE(HasReadbackSyncChainNeighbour(chain[1], lone));
}

TEST_CASE("Read-back sync steady sticky set", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncStickySet sticky;
  ReadbackSyncStickyKey k512 = MakeReadbackSyncStickyKey(Shape(0x05368000, 512));
  ReadbackSyncStickyKey k256 = MakeReadbackSyncStickyKey(Shape(0x039CA000, 256));
  CHECK_FALSE(sticky.Excludes(k512));
  sticky.Note(k512);
  sticky.Note(k512);
  CHECK(sticky.size() == 1);
  CHECK(sticky.Excludes(k512));
  CHECK_FALSE(sticky.Excludes(k256));
  // The identity includes format and size.
  ReadbackSyncStickyKey other_format = k512;
  other_format.dest_format = 2;
  CHECK_FALSE(sticky.Excludes(other_format));
  ReadbackSyncStickyKey other_size = k512;
  other_size.width = 256;
  CHECK_FALSE(sticky.Excludes(other_size));
  sticky.Clear();
  CHECK_FALSE(sticky.Excludes(k512));
  // Saturation excludes every key.
  for (uint32_t i = 0; i < kReadbackSyncStickyMaxKeys; ++i) {
    sticky.Note(ReadbackSyncStickyKey{i << 12, 0x1000, 32, 32, 6});
  }
  CHECK_FALSE(sticky.saturated());
  CHECK_FALSE(sticky.Excludes(k256));
  sticky.Note(k512);
  CHECK(sticky.saturated());
  CHECK(sticky.Excludes(k256));
  sticky.Clear();
  CHECK_FALSE(sticky.saturated());
  CHECK(sticky.size() == 0);
}

TEST_CASE("Read-back sync steady sticky rule on a frame sequence",
          "[graphics][readback-sync][readback-steady]") {
  // A livery-like key: one sporadic (rule b) resolve, then a 40-frame run of
  // rule c alone. Without sticky it would be exempt from its 30th frame;
  // with sticky it never is. The steady key next to it is unaffected.
  ReadbackSyncStickySet sticky;
  ReadbackSyncSteadyCap cap;
  ShapeKeys none;
  ReadbackSyncKeyShape livery = Shape(0x05468000, 256);
  ReadbackSyncKeyShape steady = Shape(0x039CA000, 256);
  const uint64_t livery_key = Key(livery.address, livery.length);
  const uint64_t steady_key = Key(steady.address, steady.length);
  LiveKeys live;
  live[livery_key].last_used_frame = 100;
  ReadbackSyncResolve r = Composite(256, livery.address, livery.length);
  r.key_live = true;
  r.previous_frame = 100;
  r.frame = 120;
  ReadbackSyncRules rules = EvaluateReadbackSyncRules(r, ReadbackSyncScope::kComposite, live, 0);
  REQUIRE(rules.sporadic);
  if (rules.shared_range || rules.sporadic) {
    sticky.Note(MakeReadbackSyncStickyKey(livery));
  }
  ReadbackSyncRules c_only;
  c_only.composite_shape = true;
  auto filter = [&](const ReadbackSyncKeyShape& s, uint64_t key, uint32_t run, uint64_t frame) {
    bool candidate = IsReadbackSyncSteadyExempt(c_only, run);
    return FilterReadbackSyncSteady(
        candidate, candidate && sticky.Excludes(MakeReadbackSyncStickyKey(s)),
        [&]() { return HasReadbackSyncChainNeighbour(s, none); }, cap, frame, key, nullptr);
  };
  for (uint32_t run = 1; run <= 40; ++run) {
    uint64_t frame = 120 + run;
    INFO("run " << run);
    ReadbackSyncSteadyFilter f = filter(livery, livery_key, run, frame);
    CHECK(f ==
          (run < 30 ? ReadbackSyncSteadyFilter::kNotCandidate : ReadbackSyncSteadyFilter::kSticky));
  }
  // A different key in a later frame: exempt (the sticky livery resolves
  // never reached the cap).
  CHECK(filter(steady, steady_key, 30, 161) == ReadbackSyncSteadyFilter::kExempt);
  ReadbackSyncCounters c;
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kSticky);
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kShape);
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kShape);
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kCap);
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kExempt);
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kNotCandidate);
  CHECK(c.steady_sticky_excluded == 1);
  CHECK(c.steady_shape_excluded == 2);
  CHECK(c.steady_cap_skipped == 1);
}

TEST_CASE("Read-back sync steady filter order", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncSteadyCap cap;
  int chain_checks = 0;
  auto chain = [&](bool result) {
    return [&chain_checks, result]() {
      ++chain_checks;
      return result;
    };
  };
  CHECK(FilterReadbackSyncSteady(false, true, chain(true), cap, 1, 1, nullptr) ==
        ReadbackSyncSteadyFilter::kNotCandidate);
  CHECK(FilterReadbackSyncSteady(true, true, chain(true), cap, 1, 1, nullptr) ==
        ReadbackSyncSteadyFilter::kSticky);
  CHECK(chain_checks == 0);
  CHECK(FilterReadbackSyncSteady(true, false, chain(true), cap, 1, 1, nullptr) ==
        ReadbackSyncSteadyFilter::kShape);
  CHECK(chain_checks == 1);
  // Refused candidates never reach the cap.
  CHECK_FALSE(cap.has_frame);
  ReadbackSyncSteadyCap::Result result;
  CHECK(FilterReadbackSyncSteady(true, false, chain(false), cap, 1, 1, &result) ==
        ReadbackSyncSteadyFilter::kExempt);
  CHECK(result.allowed);
  CHECK(result.keys == 1);
}

TEST_CASE("Read-back sync steady single-key cap", "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncSteadyCap cap;
  const uint64_t a = Key(0x039CA000, 0x40000);
  const uint64_t b = Key(0x05468000, 0x40000);
  // One key per frame: always allowed (also twice in one frame).
  for (uint64_t frame = 10; frame < 20; ++frame) {
    CHECK(cap.Admit(frame, a).allowed);
    CHECK(cap.Admit(frame, a).allowed);
  }
  // A second key in frame 20: A's first resolve was already allowed, B and
  // every later resolve of either key in this frame are refused; the episode
  // starts once.
  CHECK(cap.Admit(20, a).allowed);
  ReadbackSyncSteadyCap::Result r = cap.Admit(20, b);
  CHECK_FALSE(r.allowed);
  CHECK(r.episode_started);
  CHECK(r.keys == 2);
  r = cap.Admit(20, a);
  CHECK_FALSE(r.allowed);
  CHECK_FALSE(r.episode_started);
  // Frame 21 follows a frame with two keys: refused from its first resolve.
  r = cap.Admit(21, a);
  CHECK_FALSE(r.allowed);
  CHECK_FALSE(r.episode_started);
  CHECK_FALSE(cap.Admit(21, b).allowed);
  // B stops after frame 21: frame 22 still follows a two-key frame.
  CHECK_FALSE(cap.Admit(22, a).allowed);
  // Frame 23 follows a one-key frame: allowed again, the episode ended.
  CHECK(cap.Admit(23, a).allowed);
  CHECK_FALSE(cap.episode);
  // A new episode is reported again.
  r = cap.Admit(24, b);
  CHECK(r.allowed);
  r = cap.Admit(24, a);
  CHECK_FALSE(r.allowed);
  CHECK(r.episode_started);
  // A gap of frames resets the previous-frame count and the episode.
  r = cap.Admit(40, a);
  CHECK(r.allowed);
  CHECK_FALSE(cap.episode);
  r = cap.Admit(40, b);
  CHECK_FALSE(r.allowed);
  CHECK(r.episode_started);
  cap.Reset();
  CHECK_FALSE(cap.has_frame);
  CHECK(cap.Admit(41, b).allowed);
}

TEST_CASE("Read-back sync steady filter end reasons and fields",
          "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncRules c_only;
  c_only.composite_shape = true;
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, ReadbackSyncRules(), 50)) == "not_c");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 50,
                                                ReadbackSyncSteadyFilter::kSticky)) == "sticky");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 50,
                                                ReadbackSyncSteadyFilter::kShape)) == "shape");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 50,
                                                ReadbackSyncSteadyFilter::kCap)) == "cap");
  CHECK(std::string(ReadbackSyncSteadyEndReason(
            true, c_only, 50, ReadbackSyncSteadyFilter::kNotCandidate)) == "none");
  // The rule reasons come first.
  ReadbackSyncRules b = c_only;
  b.sporadic = true;
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, b, 50, ReadbackSyncSteadyFilter::kCap)) ==
        "rule_b");
  CHECK(std::string(ReadbackSyncSteadyEndReason(true, c_only, 3,
                                                ReadbackSyncSteadyFilter::kNotCandidate)) == "gap");
  CHECK(std::string(ReadbackSyncSteadyEndReason(
            false, c_only, 50, ReadbackSyncSteadyFilter::kNotCandidate)) == "disabled");

  ReadbackSyncCounters c;
  // Off and nothing counted: the minute line is unchanged.
  CHECK(ReadbackSyncSteadyFilterReport(c, false, true).empty());
  CHECK(ReadbackSyncSteadyFilterReport(c, true, true) ==
        " steady_fast=1 steady_sticky_excluded=0 steady_shape_excluded=0 steady_cap_skipped=0 "
        "steady_fast_copies=0 steady_fast_kb=0 steady_fast_skipped=0");
  c.CountSteadyFilter(ReadbackSyncSteadyFilter::kShape);
  c.steady_fast_copies = 3;
  c.steady_fast_bytes = 3 * 0x40000;
  c.steady_fast_skipped = 1;
  CHECK(ReadbackSyncSteadyFilterReport(c, false, false) ==
        " steady_fast=0 steady_sticky_excluded=0 steady_shape_excluded=1 steady_cap_skipped=0 "
        "steady_fast_copies=3 steady_fast_kb=768 steady_fast_skipped=1");
  CHECK(FormatReadbackSyncSteadyCapLine(1433, 7, 0x05468000, 0x40000, 2, 0) ==
        "kind=steady_cap event=start frame=1433 draw=7 base=05468000 len=262144 keys=2 "
        "suppressed=0");
}

TEST_CASE("Resolve flight ring keeps about 120 frames", "[graphics][livery-diag2]") {
  // l75: 64 lines = 37-42 frames; 200 lines = ~120 frames.
  STATIC_REQUIRE(DiagResolveLineRing::kCapacity == 200);
  STATIC_REQUIRE(DiagLineRing::kCapacity == 64);
  DiagResolveLineRing ring;
  for (int i = 0; i < 450; ++i) {
    ring.Push("r" + std::to_string(i));
  }
  uint64_t pushed = 0;
  std::vector<std::string> lines = ring.Snapshot(&pushed);
  REQUIRE(lines.size() == 200);
  CHECK(lines.front() == "r250");
  CHECK(lines.back() == "r449");
  CHECK(pushed == 450);
}

TEST_CASE("Readback sync steady wait counters and report",
          "[graphics][readback-sync][readback-steady]") {
  ReadbackSyncCounters c;
  CHECK_FALSE(c.SteadyWaitCounted());
  CHECK(ReadbackSyncSteadyWaitReport(c, false, 20).empty());
  CHECK(ReadbackSyncSteadyWaitReport(c, true, 20) ==
        " steady_fast_wait_ms=20 steady_fast_waits=0 steady_fast_wait_ms_total=0.000 "
        "steady_fast_wait_ms_max=0.000 steady_fast_wait_timeouts=0 steady_fast_forced=0 "
        "steady_unfenced_avoided=0 steady_skip_episodes=0 steady_skip_longest=0");
  uint32_t run = 0;
  // fast, wait ok (1.5 ms), wait ok (4 ms), timeout (20 ms, forced), fast,
  // forced (unwaitable), fast.
  c.CountSteadyDelivery(false, false, false, false, false, 0, run);
  CHECK(run == 0);
  c.CountSteadyDelivery(true, false, false, true, false, 1500, run);
  c.CountSteadyDelivery(true, false, false, true, false, 4000, run);
  c.CountSteadyDelivery(true, true, true, true, true, 20000, run);
  CHECK(run == 3);
  c.CountSteadyDelivery(false, false, false, false, false, 0, run);
  CHECK(run == 0);
  c.CountSteadyDelivery(false, false, true, true, false, 0, run);
  c.CountSteadyDelivery(false, false, false, false, false, 0, run);
  CHECK(c.steady_fast_waits == 3);
  CHECK(c.steady_fast_wait_us == 25500);
  CHECK(c.steady_fast_wait_max_us == 20000);
  CHECK(c.steady_fast_wait_timeouts == 1);
  CHECK(c.steady_fast_forced == 2);
  CHECK(c.steady_unfenced_avoided == 1);
  CHECK(c.steady_skip_episodes == 2);
  CHECK(c.steady_skip_longest == 3);
  CHECK(c.SteadyWaitCounted());
  CHECK(ReadbackSyncSteadyWaitReport(c, false, 0) ==
        " steady_fast_wait_ms=0 steady_fast_waits=3 steady_fast_wait_ms_total=25.500 "
        "steady_fast_wait_ms_max=20.000 steady_fast_wait_timeouts=1 steady_fast_forced=2 "
        "steady_unfenced_avoided=1 steady_skip_episodes=2 steady_skip_longest=3");
  // A run that continues after a minute reset counts in the new minute's
  // longest but not again as an episode.
  ReadbackSyncCounters next;
  run = 5;
  next.CountSteadyDelivery(false, false, false, true, false, 0, run);
  CHECK(next.steady_skip_episodes == 0);
  CHECK(next.steady_skip_longest == 6);
}

}  // namespace rex::graphics::test
