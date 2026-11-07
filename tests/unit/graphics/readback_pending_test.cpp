#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/readback_range.h>
#include <rex/graphics/util/readback_pending.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rex::graphics::test {

namespace {
uint64_t Key(uint32_t base, uint32_t length) {
  return (uint64_t(base) << 32) | length;
}

ReadbackPendingTable::KeyInfo Info(uint32_t base, uint32_t length, bool exempt) {
  ReadbackPendingTable::KeyInfo info;
  info.base = base;
  info.length = length;
  info.slot_size = length;
  info.width = 256;
  info.height = 256;
  info.format = 6;
  info.exempt = exempt;
  return info;
}
}  // namespace

// M11-007c: rex::gpu::ReadbackSyncQueryRange / ReadbackSyncEnsureRange.
TEST_CASE("Readback ranges overlap", "[graphics][readback-pending]") {
  CHECK(ReadbackRangesOverlap(0x1000, 0x100, 0x1000, 0x100));
  CHECK(ReadbackRangesOverlap(0x1000, 0x100, 0x10FF, 0x10));
  CHECK_FALSE(ReadbackRangesOverlap(0x1000, 0x100, 0x1100, 0x10));
  CHECK_FALSE(ReadbackRangesOverlap(0x1100, 0x10, 0x1000, 0x100));
  CHECK(ReadbackRangesOverlap(0x0F00, 0x200, 0x1000, 0x10));
  CHECK_FALSE(ReadbackRangesOverlap(0x1000, 0, 0x1000, 0x100));
  // No 32-bit wrap.
  CHECK(ReadbackRangesOverlap(0xFFFFFF00, 0x100, 0xFFFFFFF0, 0x10));
  CHECK_FALSE(ReadbackRangesOverlap(0xFFFFFF00, 0x100, 0x0, 0x10));
}

TEST_CASE("Readback ensure step", "[graphics][readback-pending]") {
  // Fence reached: copy now.
  CHECK(PlanReadbackEnsure(10, 10, 12) == ReadbackEnsureStep::kDeliverNow);
  CHECK(PlanReadbackEnsure(10, 11, 11) == ReadbackEnsureStep::kDeliverNow);
  // Executed, still running on the GPU: wait.
  CHECK(PlanReadbackEnsure(10, 9, 11) == ReadbackEnsureStep::kWait);
  // Still in the open submission: request the submit, then wait.
  CHECK(PlanReadbackEnsure(11, 9, 11) == ReadbackEnsureStep::kRequestSubmitThenWait);
}

TEST_CASE("Readback pending table lifecycle", "[graphics][readback-pending]") {
  ReadbackPendingTable table;
  const uint32_t base = 0x039CA000;
  const uint32_t length = 64;
  const uint64_t key = Key(base, length);
  std::vector<uint8_t> slots[2] = {std::vector<uint8_t>(length, 0xA0),
                                   std::vector<uint8_t>(length, 0xB1)};
  std::vector<uint8_t> guest(length, 0);
  auto copy = [&](uint32_t b, const void* src, uint32_t len) {
    CHECK(b == base);
    std::memcpy(guest.data(), src, len);
  };

  rex::gpu::ReadbackRangeInfo info;
  CHECK_FALSE(table.Query(base, 4, &info));
  ReadbackPendingRef ref;
  CHECK_FALSE(table.FindPending(base, 4, &ref));

  // Frame 100, resolve into slot 0, submission 7 (open: next is 7).
  table.SetFrame(100);
  table.SetNextSubmission(7);
  table.BeginResolve(key, 0);
  table.EndResolve(key, Info(base, length, true), true, 0, 7, slots[0].data(), false);
  REQUIRE(table.Query(base + 16, 4, &info));
  CHECK(info.key_base == base);
  CHECK(info.key_length == length);
  CHECK(info.exempt);
  CHECK(info.pending);
  CHECK(info.unsubmitted);
  CHECK(info.copy_age_frames == UINT32_MAX);
  CHECK(info.resolve_age_frames == 0);
  REQUIRE(table.FindPending(base, length, &ref));
  CHECK(ref.key == key);
  CHECK(ref.submission == 7);
  CHECK(ref.unsubmitted);
  CHECK(ref.exempt);
  // Not overlapping.
  CHECK_FALSE(table.FindPending(base + length, 16, &ref));
  REQUIRE(table.FindPending(base, length, &ref));

  // Executed later; the caller delivers it.
  table.SetNextSubmission(8);
  table.SetFrame(101);
  REQUIRE(table.Query(base, 4, &info));
  CHECK_FALSE(info.unsubmitted);
  CHECK(table.DeliverPending(ref, copy) == ReadbackDeliverResult::kDelivered);
  CHECK(guest[0] == 0xA0);
  // A second delivery of the same generation: already delivered.
  CHECK(table.DeliverPending(ref, copy) == ReadbackDeliverResult::kAlreadyDelivered);
  REQUIRE(table.Query(base, 4, &info));
  CHECK_FALSE(info.pending);
  CHECK(info.copy_age_frames == 1);
  CHECK(info.resolve_age_frames == 1);
  CHECK_FALSE(table.FindPending(base, length, &ref));

  // Frame 101, resolve into slot 1 (pending), then frame 102 into slot 0:
  // BeginResolve on slot 0 leaves slot 1 pending; the command processor's
  // fast delivery from slot 1 marks it delivered.
  table.BeginResolve(key, 1);
  table.EndResolve(key, Info(base, length, true), true, 1, 8, slots[1].data(), false);
  REQUIRE(table.FindPending(base, length, &ref));
  ReadbackPendingRef old = ref;
  table.SetFrame(102);
  table.BeginResolve(key, 0);
  REQUIRE(table.FindPending(base, length, &ref));
  CHECK(ref.generation == old.generation);
  bool cp_copied = false;
  table.DeliverByCommandProcessor(key, 1, [&]() { cp_copied = true; });
  CHECK(cp_copied);
  CHECK_FALSE(table.FindPending(base, length, &ref));
  CHECK(table.DeliverPending(old, copy) == ReadbackDeliverResult::kAlreadyDelivered);
  table.EndResolve(key, Info(base, length, true), true, 0, 9, slots[0].data(), false);
  REQUIRE(table.Query(base, 4, &info));
  CHECK(info.pending);
  CHECK(info.copy_age_frames == 1);  // the frame 101 resolve

  // Superseded: a newer copy recorded before anybody delivered the old one.
  REQUIRE(table.FindPending(base, length, &old));
  table.SetFrame(103);
  table.BeginResolve(key, 1);
  table.EndResolve(key, Info(base, length, true), true, 1, 10, slots[1].data(), false);
  CHECK(table.DeliverPending(old, copy) == ReadbackDeliverResult::kSuperseded);

  // BeginResolve on the pending slot drops it (the slot is rewritten).
  table.SetFrame(105);
  table.BeginResolve(key, 1);
  CHECK_FALSE(table.FindPending(base, length, &ref));
  CHECK(table.DeliverPending(old, copy) == ReadbackDeliverResult::kSuperseded);
  // Own-slot delivery (forced or full): recorded and delivered at once.
  table.EndResolve(key, Info(base, length, false), true, 1, 11, slots[1].data(), true);
  REQUIRE(table.Query(base, 4, &info));
  CHECK_FALSE(info.pending);
  CHECK_FALSE(info.exempt);
  CHECK(info.copy_age_frames == 0);
  // Elided copy: nothing becomes pending.
  table.BeginResolve(key, 0);
  table.EndResolve(key, Info(base, length, false), false, 0, 0, slots[0].data(), false);
  CHECK_FALSE(table.FindPending(base, length, &ref));

  // Eviction.
  table.EndResolve(key, Info(base, length, false), true, 0, 12, slots[0].data(), false);
  REQUIRE(table.FindPending(base, length, &ref));
  table.Erase(key);
  CHECK(table.DeliverPending(ref, copy) == ReadbackDeliverResult::kGone);
  CHECK_FALSE(table.Query(base, 4, &info));
  CHECK(table.size() == 0);
}

TEST_CASE("Readback pending table picks the newest overlapping key",
          "[graphics][readback-pending]") {
  ReadbackPendingTable table;
  int dummy = 0;
  table.SetFrame(10);
  table.EndResolve(Key(0x1000, 0x400), Info(0x1000, 0x400, false), true, 0, 3, &dummy, false);
  table.SetFrame(11);
  table.EndResolve(Key(0x1200, 0x400), Info(0x1200, 0x400, true), true, 0, 4, &dummy, false);
  ReadbackPendingRef ref;
  REQUIRE(table.FindPending(0x1300, 4, &ref));
  CHECK(ref.base == 0x1200);
  REQUIRE(table.FindPending(0x1000, 4, &ref));
  CHECK(ref.base == 0x1000);
  rex::gpu::ReadbackRangeInfo info;
  REQUIRE(table.Query(0x1000, 0x1000, &info));
  CHECK(info.key_base == 0x1200);
  CHECK(table.Query(0x1000, 4, nullptr));
  // The ensure line budget.
  for (uint32_t i = 0; i < kReadbackEnsureLinesPerKey; ++i) {
    CHECK(table.TakeEnsureLine(Key(0x1000, 0x400), kReadbackEnsureLinesPerKey));
  }
  CHECK_FALSE(table.TakeEnsureLine(Key(0x1000, 0x400), kReadbackEnsureLinesPerKey));
  CHECK(table.TakeEnsureLine(Key(0x1200, 0x400), kReadbackEnsureLinesPerKey));
  CHECK_FALSE(table.TakeEnsureLine(Key(0x9000, 0x400), kReadbackEnsureLinesPerKey));
  table.Clear();
  CHECK_FALSE(table.FindPending(0x1000, 0x1000, &ref));
}

TEST_CASE("Readback ensure counters and report", "[graphics][readback-pending]") {
  ReadbackEnsureCounters c;
  CHECK(ReadbackEnsureReport(c.Peek()).empty());
  c.calls += 3;
  c.pending += 2;
  c.delivered += 1;
  c.timeouts += 1;
  c.submit_requests += 1;
  c.CountWait(1500);
  c.CountWait(20000);
  CHECK(ReadbackEnsureReport(c.Peek()) ==
        " ensure_calls=3 ensure_pending=2 ensure_delivered=1 ensure_delivered_by_cp=0 "
        "ensure_timeouts=1 ensure_wait_ms_total=21.500 ensure_wait_ms_max=20.000 "
        "ensure_submit_requests=1 ensure_submits=0");
  ReadbackEnsureCounters::Snapshot s = c.Take();
  CHECK(s.calls == 3);
  CHECK(s.max_wait_us == 20000);
  CHECK(ReadbackEnsureReport(c.Peek()).empty());
}

namespace {
class FakeProvider : public rex::gpu::IReadbackRangeProvider {
 public:
  bool QueryRange(uint32_t guest_phys, uint32_t len, rex::gpu::ReadbackRangeInfo* out) override {
    ++queries;
    if (out) {
      out->key_base = guest_phys;
      out->key_length = len;
    }
    return true;
  }
  rex::gpu::ReadbackSyncEnsureResult EnsureRange(uint32_t, uint32_t, uint32_t timeout_ms) override {
    rex::gpu::ReadbackSyncEnsureResult r;
    r.pending = true;
    r.delivered = true;
    r.waited_ms = timeout_ms / 2;
    return r;
  }
  int queries = 0;
};
}  // namespace

TEST_CASE("Readback range bridge routes to the provider", "[graphics][readback-pending]") {
  rex::gpu::SetReadbackRangeProvider(nullptr);
  rex::gpu::ReadbackRangeInfo info;
  CHECK_FALSE(rex::gpu::ReadbackSyncQueryRange(0x1000, 4, &info));
  rex::gpu::ReadbackSyncEnsureResult none = rex::gpu::ReadbackSyncEnsureRange(0x1000, 4, 10);
  CHECK_FALSE(none.pending);
  CHECK_FALSE(none.delivered);
  CHECK_FALSE(none.timed_out);
  FakeProvider fake;
  rex::gpu::SetReadbackRangeProvider(&fake);
  CHECK(rex::gpu::ReadbackSyncQueryRange(0x2000, 8, &info));
  CHECK(info.key_base == 0x2000);
  CHECK(info.key_length == 8);
  rex::gpu::ReadbackSyncEnsureResult r = rex::gpu::ReadbackSyncEnsureRange(0x2000, 8, 10);
  CHECK(r.delivered);
  CHECK(r.waited_ms == 5);
  rex::gpu::SetReadbackRangeProvider(nullptr);
  CHECK_FALSE(rex::gpu::ReadbackSyncQueryRange(0x2000, 8, &info));
  CHECK(fake.queries == 1);
}

}  // namespace rex::graphics::test
