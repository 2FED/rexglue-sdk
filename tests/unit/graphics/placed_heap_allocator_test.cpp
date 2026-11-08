// l79: placed texture heap sub-allocator (d3d12_texture_placed_heaps).

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include <rex/graphics/util/placed_heap_allocator.h>

using rex::graphics::FormatTextureHeapMinuteFields;
using rex::graphics::PlacedHeapAllocation;
using rex::graphics::PlacedHeapAllocator;
using rex::graphics::TextureHeapMinuteStats;

namespace {
constexpr uint64_t kKB = 1024;
constexpr uint64_t kMB = 1024 * 1024;
}  // namespace

TEST_CASE("Placed heaps: no block, no allocation", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(256 * kMB);
  CHECK_FALSE(a.Allocate(64 * kKB, 64 * kKB).valid());
  CHECK(a.live_blocks() == 0);
  CHECK(a.reserved_bytes() == 0);
}

TEST_CASE("Placed heaps: first fit with alignment", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(16 * kMB);
  CHECK(a.AddBlock() == 0);
  const PlacedHeapAllocation small = a.Allocate(4 * kKB, 4 * kKB);
  REQUIRE(small.valid());
  CHECK(small.block == 0);
  CHECK(small.offset == 0);
  // 64 KB alignment skips to 64 KB; the 60 KB gap stays free.
  const PlacedHeapAllocation big = a.Allocate(128 * kKB, 64 * kKB);
  REQUIRE(big.valid());
  CHECK(big.offset == 64 * kKB);
  // A 4 KB resource reuses the alignment gap.
  const PlacedHeapAllocation gap = a.Allocate(8 * kKB, 4 * kKB);
  REQUIRE(gap.valid());
  CHECK(gap.offset == 4 * kKB);
  CHECK(a.used_bytes() == 140 * kKB);
  CHECK(a.block_used(0) == 140 * kKB);
}

TEST_CASE("Placed heaps: free coalesces neighbours", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(16 * kMB);
  a.AddBlock();
  std::vector<PlacedHeapAllocation> v;
  for (int i = 0; i < 4; ++i) {
    v.push_back(a.Allocate(64 * kKB, 64 * kKB));
    REQUIRE(v.back().valid());
    CHECK(v.back().offset == uint64_t(i) * 64 * kKB);
  }
  CHECK(a.block_free_ranges(0) == 1);  // the tail
  CHECK(a.Free(v[1]));
  CHECK(a.block_free_ranges(0) == 2);
  CHECK(a.Free(v[2]));  // merges with v[1]'s range
  CHECK(a.block_free_ranges(0) == 2);
  CHECK(a.Free(v[0]));  // merges into [0, 192 KB)
  CHECK(a.block_free_ranges(0) == 2);
  CHECK(a.Free(v[3]));  // everything merges into one range
  CHECK(a.block_free_ranges(0) == 1);
  CHECK(a.used_bytes() == 0);
  // The whole block is available again.
  const PlacedHeapAllocation whole = a.Allocate(8 * kMB, 64 * kKB);
  REQUIRE(whole.valid());
  CHECK(whole.offset == 0);
}

TEST_CASE("Placed heaps: double free and foreign allocations are rejected",
          "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(16 * kMB);
  a.AddBlock();
  const PlacedHeapAllocation x = a.Allocate(64 * kKB, 64 * kKB);
  REQUIRE(x.valid());
  CHECK(a.Free(x));
  CHECK_FALSE(a.Free(x));
  CHECK_FALSE(a.Free(PlacedHeapAllocation{}));
  PlacedHeapAllocation foreign;
  foreign.block = 7;
  foreign.size = 4 * kKB;
  CHECK_FALSE(a.Free(foreign));
  CHECK(a.used_bytes() == 0);
}

TEST_CASE("Placed heaps: full block spills to a new block", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(16 * kMB);
  a.AddBlock();
  const PlacedHeapAllocation p = a.Allocate(8 * kMB, 64 * kKB);
  const PlacedHeapAllocation q = a.Allocate(8 * kMB, 64 * kKB);
  REQUIRE(p.valid());
  REQUIRE(q.valid());
  // Block 0 is full: no room until the caller adds a block.
  CHECK_FALSE(a.Allocate(64 * kKB, 64 * kKB).valid());
  CHECK(a.AddBlock() == 1);
  const PlacedHeapAllocation r = a.Allocate(64 * kKB, 64 * kKB);
  REQUIRE(r.valid());
  CHECK(r.block == 1);
  CHECK(a.live_blocks() == 2);
  CHECK(a.reserved_bytes() == 32 * kMB);
}

TEST_CASE("Placed heaps: oversized resources are not placed", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(256 * kMB);
  a.AddBlock();
  CHECK(a.Fits(128 * kMB));
  CHECK_FALSE(a.Fits(128 * kMB + 1));
  CHECK_FALSE(a.Fits(0));
  CHECK_FALSE(a.Allocate(200 * kMB, 64 * kKB).valid());
  // Non-power-of-two alignment is refused.
  CHECK_FALSE(a.Allocate(64 * kKB, 48 * kKB).valid());
}

TEST_CASE("Placed heaps: empty blocks beyond the spare are released", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(16 * kMB);
  a.AddBlock();
  a.AddBlock();
  a.AddBlock();
  const PlacedHeapAllocation p0 = a.Allocate(8 * kMB, 64 * kKB);
  const PlacedHeapAllocation p1 = a.Allocate(8 * kMB, 64 * kKB);
  const PlacedHeapAllocation p2 = a.Allocate(8 * kMB, 64 * kKB);  // block 1
  REQUIRE(p2.block == 1);
  // Block 2 is empty; it is the one spare.
  CHECK(a.TakeReleasableBlocks(1).empty());
  CHECK(a.Free(p2));
  // Now blocks 1 and 2 are empty: keep block 1 (lowest), release block 2.
  const std::vector<uint32_t> released = a.TakeReleasableBlocks(1);
  REQUIRE(released.size() == 1);
  CHECK(released[0] == 2);
  CHECK(a.live_blocks() == 2);
  CHECK_FALSE(a.block_live(2));
  // A new block reuses the released slot.
  CHECK(a.AddBlock() == 2);
  CHECK(a.Free(p0));
  CHECK(a.Free(p1));
  // keep_empty 0: every empty block goes.
  CHECK(a.TakeReleasableBlocks(0).size() == 3);
  CHECK(a.live_blocks() == 0);
  CHECK(a.reserved_bytes() == 0);
  // Allocation on a released block fails to free.
  CHECK_FALSE(a.Free(p0));
}

TEST_CASE("Placed heaps: random churn keeps the books", "[graphics][placed_heaps]") {
  PlacedHeapAllocator a(4 * kMB);
  std::mt19937 rng(1234);
  std::vector<PlacedHeapAllocation> live;
  uint64_t expected_used = 0;
  for (int step = 0; step < 6000; ++step) {
    const bool alloc = live.empty() || (rng() % 3) != 0;
    if (alloc) {
      const uint64_t size = (1 + rng() % 64) * 4 * kKB;
      const uint64_t alignment = (rng() & 1) ? 4 * kKB : 64 * kKB;
      PlacedHeapAllocation p = a.Allocate(size, alignment);
      if (!p.valid()) {
        a.AddBlock();
        p = a.Allocate(size, alignment);
      }
      REQUIRE(p.valid());
      CHECK(p.offset % alignment == 0);
      CHECK(p.offset + p.size <= a.block_size());
      live.push_back(p);
      expected_used += size;
    } else {
      const size_t i = rng() % live.size();
      REQUIRE(a.Free(live[i]));
      expected_used -= live[i].size;
      live[i] = live.back();
      live.pop_back();
      a.TakeReleasableBlocks(1);
    }
    REQUIRE(a.used_bytes() == expected_used);
  }
  // No two live allocations overlap.
  for (size_t i = 0; i < live.size(); ++i) {
    for (size_t j = i + 1; j < live.size(); ++j) {
      if (live[i].block != live[j].block) {
        continue;
      }
      const bool disjoint = live[i].offset + live[i].size <= live[j].offset ||
                            live[j].offset + live[j].size <= live[i].offset;
      REQUIRE(disjoint);
    }
  }
  for (const PlacedHeapAllocation& p : live) {
    REQUIRE(a.Free(p));
  }
  CHECK(a.used_bytes() == 0);
  a.TakeReleasableBlocks(0);
  CHECK(a.live_blocks() == 0);
}

TEST_CASE("Placed heaps: minute fields", "[graphics][placed_heaps]") {
  TextureHeapMinuteStats s;
  s.NoteCreate(10);
  s.NoteCreate(30);
  s.placements = 2;
  s.heaps_created = 1;
  CHECK(s.create_us_avg() == 20);
  CHECK(s.create_us_max == 30);
  CHECK(FormatTextureHeapMinuteFields(s, true, 1, 3 * kMB + 1, 256 * kMB) ==
        "placed=1 heaps=1 used_mb=4 reserved_mb=256 creates=2 placements=2 fallbacks=0 "
        "oversized=0 heaps_created=1 heaps_released=0 heap_failures=0 create_us_avg=20 "
        "create_us_max=30 heap_create_us_max=0");
}
