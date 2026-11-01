#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/livery_diag2.h>

namespace rex::graphics::test {

TEST_CASE("Sporadic read-back resolve predicate", "[graphics][livery-diag2]") {
  CHECK(IsSporadicReadbackResolve(false, 0, 0));
  CHECK(IsSporadicReadbackResolve(false, 0, 500));
  // Same frame and the next frame: per-frame keys are not sporadic.
  CHECK_FALSE(IsSporadicReadbackResolve(true, 100, 100));
  CHECK_FALSE(IsSporadicReadbackResolve(true, 100, 101));
  CHECK(IsSporadicReadbackResolve(true, 100, 102));
  CHECK(IsSporadicReadbackResolve(true, 100, 5000));
  // A frame counter that went backwards (restart) counts as sporadic.
  CHECK(IsSporadicReadbackResolve(true, 100, 50));
}

TEST_CASE("Long pipeline wait threshold is 1 ms", "[graphics][livery-diag2]") {
  CHECK_FALSE(IsLongPipelineWait(0));
  CHECK_FALSE(IsLongPipelineWait(999));
  CHECK(IsLongPipelineWait(1000));
  CHECK(IsLongPipelineWait(28000));
}

TEST_CASE("Guest range overlap is half-open and ignores empty ranges", "[graphics][livery-diag2]") {
  CHECK(GuestRangesOverlap(0x1000, 0x100, 0x10FF, 0x10));
  CHECK_FALSE(GuestRangesOverlap(0x1000, 0x100, 0x1100, 0x10));
  CHECK_FALSE(GuestRangesOverlap(0x1100, 0x10, 0x1000, 0x100));
  CHECK(GuestRangesOverlap(0x1000, 0x1000, 0x1800, 0x10));
  CHECK_FALSE(GuestRangesOverlap(0x1000, 0, 0x1000, 0x100));
  // No 32-bit wrap at the top of the address space.
  CHECK(GuestRangesOverlap(0xFFFFF000, 0x1000, 0xFFFFFF00, 0x100));
  CHECK_FALSE(GuestRangesOverlap(0xFFFFF000, 0x1000, 0x0, 0x100));
}

TEST_CASE("Read-back key decoding and pair id", "[graphics][livery-diag2]") {
  uint64_t a = (uint64_t(0x1A2B3000) << 32) | 0x400000;
  uint64_t b = (uint64_t(0x1A2B3000) << 32) | 0x100000;
  CHECK(ReadbackKeyAddress(a) == 0x1A2B3000);
  CHECK(ReadbackKeyLength(a) == 0x400000);
  CHECK(ReadbackKeyPairId(a, b) == ReadbackKeyPairId(b, a));
  CHECK(ReadbackKeyPairId(a, b) != ReadbackKeyPairId(a, a));
}

TEST_CASE("Livery atlas shapes", "[graphics][livery-diag2]") {
  CHECK(IsLiveryAtlasShape(1024, 1024, kLiveryDiag2FormatDxt45));
  CHECK(IsLiveryAtlasShape(1024, 1024, kLiveryDiag2FormatDxt45As16161616));
  CHECK(IsLiveryAtlasShape(2048, 2048, kLiveryDiag2Format8888));
  CHECK(IsLiveryAtlasShape(2048, 2048, kLiveryDiag2Format8888A));
  CHECK_FALSE(IsLiveryAtlasShape(1024, 1024, kLiveryDiag2Format8888));
  CHECK_FALSE(IsLiveryAtlasShape(2048, 2048, kLiveryDiag2FormatDxt45));
  CHECK_FALSE(IsLiveryAtlasShape(1024, 512, kLiveryDiag2FormatDxt45));
}

TEST_CASE("ReadbackKeyHistory remembers resolve and copy frames", "[graphics][livery-diag2]") {
  ReadbackKeyHistory history;
  CHECK_FALSE(history.Get(7).seen);
  history.Record(7, 10, false);
  ReadbackKeyHistory::Lookup lookup = history.Get(7);
  CHECK(lookup.seen);
  CHECK(lookup.previous_resolve_frame == 10);
  CHECK_FALSE(lookup.copied_before);
  history.Record(7, 20, true);
  history.Record(7, 25, false);
  lookup = history.Get(7);
  CHECK(lookup.previous_resolve_frame == 25);
  CHECK(lookup.copied_before);
  CHECK(lookup.last_copy_frame == 20);
}

TEST_CASE("ReadbackKeyHistory prunes old entries when full", "[graphics][livery-diag2]") {
  ReadbackKeyHistory history(4, 100);
  history.Record(1, 0, false);
  history.Record(2, 0, false);
  history.Record(3, 150, false);
  history.Record(4, 150, false);
  // Full: keys 1 and 2 are older than 100 frames at frame 200 and go.
  history.Record(5, 200, false);
  CHECK_FALSE(history.Get(1).seen);
  CHECK_FALSE(history.Get(2).seen);
  CHECK(history.Get(3).seen);
  CHECK(history.Get(5).seen);
  CHECK(history.size() == 3);
  // Full of recent keys: cleared.
  history.Record(6, 200, false);
  history.Record(7, 201, false);
  CHECK(history.size() == 1);
  CHECK(history.Get(7).seen);
}

TEST_CASE("RecentReadbackRanges matches overlapping destinations within the window",
          "[graphics][livery-diag2]") {
  RecentReadbackRanges ranges(600);
  ranges.Note(0x10000, 0x4000, 100, false);
  ranges.Note(0x12000, 0x1000, 300, true);
  ranges.Note(0x80000, 0x1000, 300, true);
  RecentReadbackRanges::Match match = ranges.Find(0x11000, 0x2000, 400);
  CHECK(match.found);
  CHECK(match.count == 2);
  CHECK(match.address == 0x12000);
  CHECK(match.last_resolve_frame == 300);
  CHECK(match.copied);
  CHECK(match.last_copy_frame == 300);
  // Outside the window only the newer one remains.
  match = ranges.Find(0x11000, 0x2000, 750);
  CHECK(match.count == 1);
  CHECK(match.address == 0x12000);
  CHECK_FALSE(ranges.Find(0x11000, 0x2000, 1000).found);
  CHECK_FALSE(ranges.Find(0x40000, 0x1000, 300).found);
}

TEST_CASE("TextureLoadWatchList keys invalidations back to the load", "[graphics][livery-diag2]") {
  TextureLoadWatchList list(4);
  CHECK_FALSE(list.OnLoad(1, 10));
  CHECK(list.OnLoad(1, 12));
  uint64_t load_frame = 0;
  CHECK(list.TakeOnInvalidate(1, &load_frame));
  CHECK(load_frame == 12);
  CHECK_FALSE(list.TakeOnInvalidate(1, &load_frame));
  // Capacity: the older half is dropped and counted as expired.
  list.OnLoad(1, 1);
  list.OnLoad(2, 2);
  list.OnLoad(3, 3);
  list.OnLoad(4, 4);
  list.OnLoad(5, 5);
  CHECK(list.TakeExpired() == 2);
  CHECK(list.TakeExpired() == 0);
  CHECK_FALSE(list.TakeOnInvalidate(1, nullptr));
  CHECK(list.TakeOnInvalidate(5, nullptr));
}

TEST_CASE("FrameWaitWindow sums the last five frames", "[graphics][livery-diag2]") {
  FrameWaitWindow window;
  window.Add(10, 100);
  window.Add(10, 50);
  window.Add(12, 1000);
  CHECK(window.SumUs(12) == 1150);
  CHECK(window.SumUs(14) == 1150);
  // Frame 10 leaves the window at frame 15.
  CHECK(window.SumUs(15) == 1000);
  // Frame 15 reuses frame 10's slot.
  window.Add(15, 7);
  CHECK(window.SumUs(15) == 1007);
  CHECK(window.SumUs(17) == 7);
  CHECK(window.SumUs(20) == 0);
}

TEST_CASE("PendingLongWaits merges by base and matches the next resolve",
          "[graphics][livery-diag2]") {
  PendingLongWaits waits;
  waits.Add(320, 100, 5, 2000);
  waits.Add(320, 101, 9, 7000);
  waits.Add(0, 101, 12, 1500);
  CHECK(waits.size() == 2);
  PendingLongWaits::Wait wait;
  CHECK_FALSE(waits.TakeForBase(640, 102, &wait));
  CHECK(waits.TakeForBase(320, 102, &wait));
  CHECK(wait.first_frame == 100);
  CHECK(wait.first_draw == 5);
  CHECK(wait.last_frame == 101);
  CHECK(wait.count == 2);
  CHECK(wait.total_us == 9000);
  CHECK(wait.max_us == 7000);
  CHECK_FALSE(waits.TakeForBase(320, 103, &wait));
  // Expired entries are dropped.
  CHECK_FALSE(waits.TakeForBase(0, 101 + kLiveryDiag2WaitMatchFrames + 1, &wait));
  CHECK(waits.size() == 0);
  // Bounded: the oldest base is dropped.
  for (uint32_t i = 0; i < PendingLongWaits::kCapacity + 1; ++i) {
    waits.Add(i, 200, i, 1000);
  }
  CHECK(waits.size() == PendingLongWaits::kCapacity);
  CHECK_FALSE(waits.TakeForBase(0, 200, &wait));
  CHECK(waits.TakeForBase(PendingLongWaits::kCapacity, 200, &wait));
}

TEST_CASE("LiveryDiag2TextureTracker tracks atlas and read-back loads",
          "[graphics][livery-diag2]") {
  LiveryDiag2TextureTracker tracker;
  CHECK_FALSE(tracker.enabled());
  tracker.SetEnabled(true);
  tracker.SetFrame(50);
  tracker.NoteReadbackDestination(0x200000, 0x100000, true);
  tracker.SetFrame(60);
  uint64_t atlas = LiveryDiag2TexturePartId(0x300, 1024, 1024, kLiveryDiag2FormatDxt45, false);
  uint64_t plain = LiveryDiag2TexturePartId(0x900, 256, 256, kLiveryDiag2FormatDxt45, false);
  uint64_t overlapping = LiveryDiag2TexturePartId(0x280, 512, 512, kLiveryDiag2Format8888, false);

  auto load =
      tracker.OnTextureLoad(atlas, 0x300000, 0x100000, 1024, 1024, kLiveryDiag2FormatDxt45, 0);
  CHECK(load.tracked);
  CHECK(load.log);
  CHECK(load.atlas_shape);
  CHECK(load.first_upload);
  CHECK_FALSE(load.readback.found);
  CHECK(load.frame == 60);

  CHECK_FALSE(
      tracker.OnTextureLoad(plain, 0x900000, 0x8000, 256, 256, kLiveryDiag2FormatDxt45, 0).tracked);

  load =
      tracker.OnTextureLoad(overlapping, 0x280000, 0x100000, 512, 512, kLiveryDiag2Format8888, 0);
  CHECK(load.tracked);
  CHECK_FALSE(load.atlas_shape);
  CHECK(load.readback.found);
  CHECK(load.readback.last_resolve_frame == 50);

  tracker.SetFrame(63);
  auto inval = tracker.OnTextureInvalidated(atlas, false, 0);
  CHECK(inval.tracked);
  CHECK(inval.log);
  CHECK(inval.load_frame == 60);
  CHECK(inval.frame == 63);
  CHECK_FALSE(tracker.OnTextureInvalidated(atlas, false, 0).tracked);
  CHECK_FALSE(tracker.OnTextureInvalidated(plain, true, 0).tracked);

  // Reload of the same part: not the first upload any more.
  load = tracker.OnTextureLoad(atlas, 0x300000, 0x100000, 1024, 1024, kLiveryDiag2FormatDxt45, 0);
  CHECK_FALSE(load.first_upload);
  CHECK_FALSE(load.reload_without_invalidation);
  load = tracker.OnTextureLoad(atlas, 0x300000, 0x100000, 1024, 1024, kLiveryDiag2FormatDxt45, 0);
  CHECK(load.reload_without_invalidation);

  LiveryDiag2TextureTracker::Counters counters = tracker.TakeCounters();
  CHECK(counters.loads_tracked == 4);
  CHECK(counters.loads_atlas_shape == 3);
  CHECK(counters.loads_readback_overlap == 1);
  CHECK(counters.reloads_without_invalidation == 1);
  CHECK(counters.invalidations == 1);
  CHECK(counters.invalidations_by_gpu == 0);
  CHECK(tracker.TakeCounters().loads_tracked == 0);

  // Disabling clears the state.
  tracker.SetEnabled(false);
  tracker.SetEnabled(true);
  CHECK(tracker.waiting_loads() == 0);
  load = tracker.OnTextureLoad(atlas, 0x300000, 0x100000, 1024, 1024, kLiveryDiag2FormatDxt45, 0);
  CHECK(load.first_upload);
}

TEST_CASE("LiveryDiag2TextureTracker load lines have a per-minute budget",
          "[graphics][livery-diag2]") {
  LiveryDiag2TextureTracker tracker;
  tracker.SetEnabled(true);
  int logged = 0;
  for (uint32_t i = 0; i < 100; ++i) {
    uint64_t id = LiveryDiag2TexturePartId(i, 1024, 1024, kLiveryDiag2FormatDxt45, false);
    logged +=
        tracker.OnTextureLoad(id, i << 12, 0x1000, 1024, 1024, kLiveryDiag2FormatDxt45, 1000).log
            ? 1
            : 0;
  }
  CHECK(logged == int(kLiveryDiag2LinesPerMinute));
  LiveryDiag2TextureTracker::Counters counters = tracker.TakeCounters();
  CHECK(counters.loads_tracked == 100);
  CHECK(counters.load_lines_suppressed == 100 - kLiveryDiag2LinesPerMinute);
  // The invalidation budget is separate.
  uint64_t id = LiveryDiag2TexturePartId(0, 1024, 1024, kLiveryDiag2FormatDxt45, false);
  CHECK(tracker.OnTextureInvalidated(id, true, 1000).log);
}

}  // namespace rex::graphics::test
