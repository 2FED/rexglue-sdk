#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/graphics/util/deferred_fence_writes.h>

using rex::graphics::DeferredFenceAction;
using rex::graphics::DeferredFenceCounters;
using rex::graphics::DeferredFenceLateFrameBucket;
using rex::graphics::DeferredFenceLateMsBucket;
using rex::graphics::DeferredFenceTargetSubmission;
using rex::graphics::DeferredFenceWrite;
using rex::graphics::DeferredFenceWriteQueue;
using rex::graphics::FormatDeferredFenceFields;
using rex::graphics::kFenceDeferLines;
using rex::graphics::PlanDeferredFenceProgress;
using rex::graphics::ShouldCoalesceFenceEarlySubmit;
using rex::graphics::ShouldDeferFenceWrite;

namespace {

DeferredFenceWrite MakeWrite(uint32_t address, uint32_t value, uint64_t submission,
                             uint64_t frame = 0, uint64_t parse_us = 0) {
  DeferredFenceWrite w;
  w.address = address;
  w.value = value;
  w.submission = submission;
  w.frame = frame;
  w.parse_us = parse_us;
  return w;
}

struct Stored {
  uint32_t address;
  uint32_t value;
  uint64_t frames;
  uint64_t us;
};

size_t FlushInto(DeferredFenceWriteQueue& q, uint64_t completed, std::vector<Stored>& out,
                 uint64_t frame = 0, uint64_t now_us = 0, bool waiting = false) {
  return q.Flush(completed, frame, now_us, waiting,
                 [&](const DeferredFenceWrite& w, uint64_t frames, uint64_t us) {
                   out.push_back({w.address, w.value, frames, us});
                 });
}

}  // namespace

TEST_CASE("Deferred fence: target submission", "[graphics][fence_defer]") {
  // Parsed while a submission is open: the work before it is in that one.
  CHECK(DeferredFenceTargetSubmission(true, 7) == 7);
  // None open: everything before it was in the last ended submission.
  CHECK(DeferredFenceTargetSubmission(false, 7) == 6);
  // Nothing ever submitted: fence value 0, complete from the start.
  CHECK(DeferredFenceTargetSubmission(false, 1) == 0);
  CHECK(DeferredFenceTargetSubmission(false, 0) == 0);
}

TEST_CASE("Deferred fence: cvar off keeps the parse-time store", "[graphics][fence_defer]") {
  CHECK_FALSE(ShouldDeferFenceWrite(false, true));
  CHECK(ShouldDeferFenceWrite(true, true));
  CHECK(ShouldDeferFenceWrite(true, false));
  // Turned off with writes still queued: later fences queue behind them.
  CHECK(ShouldDeferFenceWrite(false, false));
  // Nothing counted and the mode off: the minute line gains no fields.
  CHECK(FormatDeferredFenceFields(DeferredFenceCounters{}, false, 0, 0).empty());
}

TEST_CASE("Deferred fence: immediate when the GPU is already past", "[graphics][fence_defer]") {
  DeferredFenceWriteQueue q;
  CHECK_FALSE(q.Push(MakeWrite(0x1000, 1, 5), 5));
  CHECK(q.empty());
  CHECK(q.Push(MakeWrite(0x1000, 2, 6), 5));
  CHECK(q.size() == 1);
  // Already complete, but an earlier write is queued: it queues too (order).
  CHECK(q.Push(MakeWrite(0x1000, 3, 5), 5));
  CHECK(q.size() == 2);
  const DeferredFenceCounters& c = q.counters();
  CHECK(c.events == 3);
  CHECK(c.immediate == 1);
  CHECK(c.deferred == 2);
  CHECK(c.max_depth == 2);
}

TEST_CASE("Deferred fence: completion gating and order", "[graphics][fence_defer]") {
  DeferredFenceWriteQueue q;
  REQUIRE(q.Push(MakeWrite(0x100, 10, 3), 2));
  REQUIRE(q.Push(MakeWrite(0x200, 20, 3), 2));
  REQUIRE(q.Push(MakeWrite(0x100, 30, 4), 2));
  REQUIRE(q.Push(MakeWrite(0x300, 40, 6), 2));
  CHECK(q.head_submission() == 3);

  std::vector<Stored> out;
  CHECK(FlushInto(q, 2, out) == 0);
  CHECK(out.empty());
  CHECK(FlushInto(q, 3, out) == 2);
  REQUIRE(out.size() == 2);
  CHECK(out[0].address == 0x100);
  CHECK(out[0].value == 10);
  CHECK(out[1].address == 0x200);
  CHECK(out[1].value == 20);
  CHECK(q.head_submission() == 4);
  CHECK(FlushInto(q, 5, out) == 1);
  REQUIRE(out.size() == 3);
  CHECK(out[2].value == 30);
  CHECK(q.size() == 1);
  CHECK(FlushInto(q, 100, out) == 1);
  REQUIRE(out.size() == 4);
  CHECK(out[3].value == 40);
  CHECK(q.empty());
  CHECK(q.counters().written == 4);
}

TEST_CASE("Deferred fence: a later write never lands first", "[graphics][fence_defer]") {
  // Submissions complete in order; even if a later entry's submission is
  // reported complete, nothing passes an undue head.
  DeferredFenceWriteQueue q;
  REQUIRE(q.Push(MakeWrite(0x100, 1, 9), 0));
  REQUIRE(q.Push(MakeWrite(0x100, 2, 9), 0));
  std::vector<Stored> out;
  CHECK(FlushInto(q, 8, out) == 0);
  CHECK(FlushInto(q, 9, out) == 2);
  REQUIRE(out.size() == 2);
  CHECK(out[0].value == 1);
  CHECK(out[1].value == 2);
}

TEST_CASE("Deferred fence: latency counters and idle flushes", "[graphics][fence_defer]") {
  DeferredFenceWriteQueue q;
  REQUIRE(q.Push(MakeWrite(0x100, 1, 2, 10, 1000), 0));
  REQUIRE(q.Push(MakeWrite(0x100, 2, 3, 11, 5000), 0));
  std::vector<Stored> out;
  CHECK(FlushInto(q, 2, out, 12, 4000, false) == 1);
  CHECK(FlushInto(q, 3, out, 12, 9500, true) == 1);
  REQUIRE(out.size() == 2);
  CHECK(out[0].frames == 2);
  CHECK(out[0].us == 3000);
  CHECK(out[1].frames == 1);
  CHECK(out[1].us == 4500);
  q.NoteEarlySubmit();
  DeferredFenceCounters c = q.TakeCounters();
  CHECK(c.max_frames == 2);
  CHECK(c.max_us == 4500);
  CHECK(c.flush_on_idle == 1);
  CHECK(c.early_submits == 1);
  CHECK(c.written == 2);
  CHECK(q.counters().written == 0);
  CHECK(FormatDeferredFenceFields(c, true, 0, 0) ==
        " fence_defer=1 fence_events=2 fence_deferred=2 fence_immediate=0 fence_written=2 "
        "fence_deferred_max_frames=2 fence_deferred_ms_max=4.500 fence_flush_on_idle=1 "
        "fence_early_submits=1 fence_queue_max=2 fence_queue_now=0 fence_late_f0=0 "
        "fence_late_f1=1 fence_late_f2p=1 fence_late_lt5ms=2 fence_late_5_20ms=0 "
        "fence_late_20_50ms=0 fence_late_gt50ms=0 fence_min_gap_us=0 fence_early_coalesced=0");
  // Off, but writes still queued: the fields stay.
  CHECK_FALSE(FormatDeferredFenceFields(DeferredFenceCounters{}, false, 1, 0).empty());
}

TEST_CASE("Deferred fence: progress plan (deadlock rules)", "[graphics][fence_defer]") {
  // Head complete: just flush.
  CHECK(PlanDeferredFenceProgress(4, true, 6, 4, true) == DeferredFenceAction::kNone);
  // Head in the open submission, command processor waiting (idle or
  // WAIT_REG_MEM): end it, the guest may spin on this word.
  CHECK(PlanDeferredFenceProgress(6, true, 6, 3, true) == DeferredFenceAction::kEndSubmission);
  // Head in the open submission, busy, GPU still has work: keep parsing.
  CHECK(PlanDeferredFenceProgress(6, true, 6, 3, false) == DeferredFenceAction::kNone);
  // Head in the open submission, busy, GPU idle (all ended ones complete).
  CHECK(PlanDeferredFenceProgress(6, true, 6, 5, false) == DeferredFenceAction::kEndSubmission);
  // Head submitted, not complete: wait for the fence only when waiting.
  CHECK(PlanDeferredFenceProgress(5, true, 6, 3, true) == DeferredFenceAction::kWaitFence);
  CHECK(PlanDeferredFenceProgress(5, false, 6, 3, true) == DeferredFenceAction::kWaitFence);
  CHECK(PlanDeferredFenceProgress(5, true, 6, 3, false) == DeferredFenceAction::kNone);
  // No open submission can hold a head at submission_current.
  CHECK(PlanDeferredFenceProgress(6, false, 6, 3, true) == DeferredFenceAction::kNone);
}

TEST_CASE("Deferred fence: idle loop never strands a queued write", "[graphics][fence_defer]") {
  // Model of the command processor going idle: the guest stopped feeding the
  // ring (it spins on the fence). Each idle step services the queue exactly
  // as the D3D12 command processor does; the GPU completes one submission
  // per step once submitted. The write must land in a bounded number of
  // steps without new ring data.
  DeferredFenceWriteQueue q;
  bool open = true;
  uint64_t current = 4;    // the open submission
  uint64_t completed = 2;  // 3 still executing
  REQUIRE(q.Push(MakeWrite(0x500, 0xAB, DeferredFenceTargetSubmission(open, current)), completed));
  std::vector<Stored> out;
  int steps = 0;
  for (; steps < 16 && !q.empty(); ++steps) {
    FlushInto(q, completed, out, 0, 0, true);
    if (q.empty()) {
      break;
    }
    if (PlanDeferredFenceProgress(q.head_submission(), open, current, completed, true) ==
        DeferredFenceAction::kEndSubmission) {
      q.NoteEarlySubmit();
      open = false;
      ++current;
    }
    // GPU progress.
    if (completed + 1 < current) {
      ++completed;
    }
  }
  CHECK(q.empty());
  REQUIRE(out.size() == 1);
  CHECK(out[0].value == 0xAB);
  CHECK(q.counters().early_submits == 1);
  CHECK(q.counters().flush_on_idle == 1);
  CHECK(steps < 16);
}

TEST_CASE("Deferred fence: line budget restarts on enable", "[graphics][fence_defer]") {
  DeferredFenceWriteQueue q;
  for (uint32_t i = 0; i < kFenceDeferLines + 2; ++i) {
    REQUIRE(q.Push(MakeWrite(0x100, i, 10), 0));
    CHECK(q.back().seq == i);
  }
  std::vector<Stored> out;
  FlushInto(q, 10, out);
  q.RestartLines();
  REQUIRE(q.Push(MakeWrite(0x100, 99, 11), 10));
  CHECK(q.back().seq == 0);
  q.Clear();
  CHECK(q.empty());
  CHECK(q.head_submission() == 0);
}

TEST_CASE("Deferred fence: lateness histogram buckets", "[graphics][fence_defer]") {
  CHECK(DeferredFenceLateFrameBucket(0) == 0);
  CHECK(DeferredFenceLateFrameBucket(1) == 1);
  CHECK(DeferredFenceLateFrameBucket(2) == 2);
  CHECK(DeferredFenceLateFrameBucket(9) == 2);
  CHECK(DeferredFenceLateMsBucket(0) == 0);
  CHECK(DeferredFenceLateMsBucket(4999) == 0);
  CHECK(DeferredFenceLateMsBucket(5000) == 1);
  CHECK(DeferredFenceLateMsBucket(19999) == 1);
  CHECK(DeferredFenceLateMsBucket(20000) == 2);
  CHECK(DeferredFenceLateMsBucket(50000) == 2);
  CHECK(DeferredFenceLateMsBucket(50001) == 3);

  DeferredFenceWriteQueue q;
  REQUIRE(q.Push(MakeWrite(0x100, 1, 2, 10, 0), 0));
  REQUIRE(q.Push(MakeWrite(0x100, 2, 3, 10, 0), 0));
  REQUIRE(q.Push(MakeWrite(0x100, 3, 4, 10, 0), 0));
  REQUIRE(q.Push(MakeWrite(0x100, 4, 5, 10, 0), 0));
  std::vector<Stored> out;
  CHECK(FlushInto(q, 2, out, 10, 1000) == 1);   // same frame, 1 ms
  CHECK(FlushInto(q, 3, out, 11, 12000) == 1);  // 1 frame, 12 ms
  CHECK(FlushInto(q, 4, out, 12, 30000) == 1);  // 2 frames, 30 ms
  CHECK(FlushInto(q, 5, out, 14, 60000) == 1);  // 4 frames, 60 ms
  const DeferredFenceCounters& c = q.counters();
  CHECK(c.late_frames[0] == 1);
  CHECK(c.late_frames[1] == 1);
  CHECK(c.late_frames[2] == 2);
  CHECK(c.late_ms[0] == 1);
  CHECK(c.late_ms[1] == 1);
  CHECK(c.late_ms[2] == 1);
  CHECK(c.late_ms[3] == 1);
}

TEST_CASE("Deferred fence: min-gap coalescing of early submits", "[graphics][fence_defer]") {
  // 0 = the l77 behaviour: never skipped.
  CHECK_FALSE(ShouldCoalesceFenceEarlySubmit(0, 1000, 1001));
  // Unknown submission start: never skipped.
  CHECK_FALSE(ShouldCoalesceFenceEarlySubmit(2000, 0, 1));
  // Young submission: skipped; old enough: submitted.
  CHECK(ShouldCoalesceFenceEarlySubmit(2000, 10000, 10000));
  CHECK(ShouldCoalesceFenceEarlySubmit(2000, 10000, 11999));
  CHECK_FALSE(ShouldCoalesceFenceEarlySubmit(2000, 10000, 12000));
  CHECK_FALSE(ShouldCoalesceFenceEarlySubmit(2000, 10000, 50000));
  // A clock behind the start counts as elapsed 0.
  CHECK(ShouldCoalesceFenceEarlySubmit(2000, 10000, 9000));
  // Bounded: polling a held head over time eventually submits.
  uint64_t now = 10000;
  int polls = 0;
  while (ShouldCoalesceFenceEarlySubmit(2000, 10000, now)) {
    now += 1000;  // the idle wait polls every 1 ms
    ++polls;
  }
  CHECK(polls == 2);

  DeferredFenceWriteQueue q;
  q.NoteEarlyCoalesced();
  q.NoteEarlyCoalesced();
  q.NoteEarlySubmit();
  const DeferredFenceCounters c = q.TakeCounters();
  CHECK(c.early_coalesced == 2);
  CHECK(c.early_submits == 1);
  const std::string fields = FormatDeferredFenceFields(c, true, 0, 2000);
  CHECK(fields.find(" fence_min_gap_us=2000 fence_early_coalesced=2") != std::string::npos);
  // Off with only coalesced skips counted: the fields stay.
  DeferredFenceCounters only;
  only.early_coalesced = 1;
  CHECK_FALSE(FormatDeferredFenceFields(only, false, 0, 0).empty());
}
