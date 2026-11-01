// M10 watch census, step 1 disarm bridging and step 2 upload-ahead
// (rex/system/watch_census.h).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/watch_census.h>
#include <rex/system/xmemory.h>

namespace wc = rex::memory::watch_census;

namespace {

using wc::DisarmPage;
using wc::PageRun;

std::vector<PageRun> BuildRuns(bool bridge, const std::vector<DisarmPage>& pages,
                               uint64_t* gaps = nullptr, uint64_t* gap_pages = nullptr) {
  wc::DisarmRunBuilder builder(bridge);
  std::vector<PageRun> runs;
  PageRun run;
  for (uint32_t i = 0; i < pages.size(); ++i) {
    if (builder.Feed(i, pages[i], &run)) {
      runs.push_back(run);
    }
  }
  if (builder.Finish(&run)) {
    runs.push_back(run);
  }
  if (gaps) {
    *gaps = builder.bridged_gaps();
  }
  if (gap_pages) {
    *gap_pages = builder.bridged_pages();
  }
  return runs;
}

constexpr DisarmPage D = DisarmPage::kDisarm;
constexpr DisarmPage G = DisarmPage::kSafeGap;
constexpr DisarmPage B = DisarmPage::kBarrier;

constexpr uint32_t kReadWrite = rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

struct WatchedMemory {
  rex::memory::Memory memory;
  rex::memory::PhysicalHeap* heap = nullptr;
  uint32_t address = 0;
  uint32_t physical = 0;
  uint32_t page_size = uint32_t(rex::memory::page_size());

  explicit WatchedMemory(uint32_t pages, uint32_t alias = 0xE0000000) {
    rex::InitLogging();
    REQUIRE(memory.Initialize());
    heap = static_cast<rex::memory::PhysicalHeap*>(memory.LookupHeap(alias));
    REQUIRE(heap != nullptr);
    REQUIRE(
        heap->Alloc(page_size * pages, page_size,
                    rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    kReadWrite, false, &address));
    physical = memory.GetPhysicalAddress(address);
    REQUIRE(physical != UINT32_MAX);
  }

  void Arm(uint32_t first, uint32_t count = 1) {
    heap->EnableAccessCallbacks(physical + first * page_size, count * page_size, true, false);
  }

  rex::memory::PageAccess HostAccess(uint32_t page) {
    size_t length = page_size;
    rex::memory::PageAccess access;
    REQUIRE(rex::memory::QueryProtect(memory.TranslateVirtual(address + page * page_size), length,
                                      access));
    return access;
  }

  bool TriggerAll(uint32_t pages) {
    return heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(), address,
                                  page_size * pages, true, true);
  }
};

class BridgeCvar {
 public:
  explicit BridgeCvar(bool value) : previous_(REXCVAR_GET(memory_watch_disarm_bridge_safe_gaps)) {
    REXCVAR_SET(memory_watch_disarm_bridge_safe_gaps, value);
  }
  ~BridgeCvar() { REXCVAR_SET(memory_watch_disarm_bridge_safe_gaps, previous_); }

 private:
  bool previous_;
};

struct HeapMutexAccess : rex::memory::BaseHeap {
  static auto Member() { return &HeapMutexAccess::heap_mutex_; }
};

}  // namespace

TEST_CASE("Watch census pages-per-call buckets", "[watch-census]") {
  CHECK(wc::PagesBucket(0) == 0);
  CHECK(wc::PagesBucket(1) == 0);
  CHECK(wc::PagesBucket(2) == 1);
  CHECK(wc::PagesBucket(4) == 1);
  CHECK(wc::PagesBucket(5) == 2);
  CHECK(wc::PagesBucket(16) == 2);
  CHECK(wc::PagesBucket(17) == 3);
  CHECK(wc::PagesBucket(64) == 3);
  CHECK(wc::PagesBucket(65) == 4);
  CHECK(wc::PagesBucket(100000) == 4);
}

TEST_CASE("Watch census thread classes", "[watch-census]") {
  CHECK(wc::ClassifyThread(10, 10, 20) == wc::ThreadClass::kCp);
  CHECK(wc::ClassifyThread(20, 10, 20) == wc::ThreadClass::kRender);
  CHECK(wc::ClassifyThread(30, 10, 20) == wc::ThreadClass::kOther);
  CHECK(wc::ClassifyThread(30, 0, 0) == wc::ThreadClass::kOther);
  CHECK(wc::ClassifyThread(0, 0, 0) == wc::ThreadClass::kOther);
}

TEST_CASE("Watch census block hotness history", "[watch-census]") {
  CHECK(wc::HotnessClass(0xFF) == 0);
  CHECK(wc::HotnessClass(0x0F) == 1);
  CHECK(wc::HotnessClass(0x7F) == 1);
  CHECK(wc::HotnessClass(0x07) == 2);
  CHECK(wc::HotnessClass(0x01) == 2);
  CHECK(wc::HotnessClass(0x00) == 3);
  uint8_t history = 0;
  for (int i = 0; i < 8; ++i) {
    history = wc::AdvanceHistory(history, true);
  }
  CHECK(history == 0xFF);
  history = wc::AdvanceHistory(history, false);
  CHECK(history == 0xFE);
  CHECK(wc::HotnessClass(history) == 1);
}

TEST_CASE("Disarm run building without bridging is the legacy split", "[watch-census][bridge]") {
  CHECK(BuildRuns(false, {D, G, D}) == std::vector<PageRun>{{0, 0}, {2, 2}});
  CHECK(BuildRuns(false, {D, D, G, G, D, D}) == std::vector<PageRun>{{0, 1}, {4, 5}});
  CHECK(BuildRuns(false, {G, D, B, D}) == std::vector<PageRun>{{1, 1}, {3, 3}});
  CHECK(BuildRuns(false, {G, B, G}).empty());
}

TEST_CASE("Disarm run building bridges only safe gaps between disarm pages",
          "[watch-census][bridge]") {
  uint64_t gaps = 0, pages = 0;
  CHECK(BuildRuns(true, {D, G, D}, &gaps, &pages) == std::vector<PageRun>{{0, 2}});
  CHECK(gaps == 1);
  CHECK(pages == 1);
  CHECK(BuildRuns(true, {D, G, D, G, G, D}, &gaps, &pages) == std::vector<PageRun>{{0, 5}});
  CHECK(gaps == 2);
  CHECK(pages == 3);
  // Leading and trailing gap pages are never protected.
  CHECK(BuildRuns(true, {G, G, D, D, G}, &gaps, &pages) == std::vector<PageRun>{{2, 3}});
  CHECK(gaps == 0);
  // A barrier (unknown, read-only, uncommitted, ...) ends the run; safe pages
  // next to it are not included.
  CHECK(BuildRuns(true, {D, G, B, G, D}, &gaps) == std::vector<PageRun>{{0, 0}, {4, 4}});
  CHECK(gaps == 0);
  CHECK(BuildRuns(true, {D, G, G, B}) == std::vector<PageRun>{{0, 0}});
  CHECK(BuildRuns(true, {G, G, G}).empty());
  CHECK(BuildRuns(true, {}).empty());
}

TEST_CASE("Upload-ahead extends forward then backward up to the cap", "[watch-census][ahead]") {
  // Block 0, nothing blocked.
  CHECK(wc::ExtendUploadRun(0, 0, 0, 0, 0, UINT32_MAX, 16) == PageRun{0, 15});
  CHECK(wc::ExtendUploadRun(10, 10, 0, 0, 0, UINT32_MAX, 64) == PageRun{0, 63});
  // Block end stops the forward growth; the rest of the cap goes backward.
  CHECK(wc::ExtendUploadRun(60, 60, 0, 0, 0, UINT32_MAX, 16) == PageRun{48, 63});
  // Absolute page numbers in block 3 (pages 192..255).
  CHECK(wc::ExtendUploadRun(197, 197, 0, 0, 0, UINT32_MAX, 64) == PageRun{192, 255});
  CHECK(wc::ExtendUploadRun(197, 198, 0, 0, 0, UINT32_MAX, 16) == PageRun{197, 212});
}

TEST_CASE("Upload-ahead never crosses blocked pages", "[watch-census][ahead]") {
  // Page 5 is GPU-written (or valid, or not writable): growth stops before it.
  const uint64_t gpu_written_5 = uint64_t(1) << 5;
  CHECK(wc::ExtendUploadRun(2, 2, gpu_written_5, gpu_written_5, 0, UINT32_MAX, 16) ==
        PageRun{0, 4});
  // Already-valid pages on both sides: nothing is re-uploaded.
  const uint64_t valid_around = (uint64_t(1) << 9) | (uint64_t(1) << 11);
  CHECK(wc::ExtendUploadRun(10, 10, valid_around, valid_around, 0, UINT32_MAX, 64) ==
        PageRun{10, 10});
  // A fully blocked block leaves the run unchanged.
  CHECK(wc::ExtendUploadRun(20, 21, UINT64_MAX, UINT64_MAX, 0, UINT32_MAX, 64) == PageRun{20, 21});
  // Non-contiguous: page 13 free but 12 blocked: 13 is not reached.
  const uint64_t blocked_12 = uint64_t(1) << 12;
  CHECK(wc::ExtendUploadRun(10, 11, blocked_12, blocked_12, 10, UINT32_MAX, 64) == PageRun{10, 11});
}

TEST_CASE("Upload-ahead respects neighbouring runs, cap and run length", "[watch-census][ahead]") {
  CHECK(wc::ExtendUploadRun(10, 10, 0, 0, 8, 12, 64) == PageRun{8, 12});
  // Already at or over the cap: unchanged, never shrunk.
  CHECK(wc::ExtendUploadRun(0, 15, 0, 0, 0, UINT32_MAX, 16) == PageRun{0, 15});
  CHECK(wc::ExtendUploadRun(0, 40, 0, 0, 0, UINT32_MAX, 16) == PageRun{0, 40});
  CHECK(wc::ExtendUploadRun(5, 5, 0, 0, 0, UINT32_MAX, 0) == PageRun{5, 5});
  // A run across two blocks grows forward in the last block, backward in the
  // first block.
  CHECK(wc::ExtendUploadRun(60, 70, 0, 0, 0, UINT32_MAX, 16) == PageRun{60, 75});
  CHECK(wc::ExtendUploadRun(60, 70, 0, UINT64_MAX, 0, UINT32_MAX, 16) == PageRun{55, 70});
  CHECK(wc::SanitizeUploadAheadPages(0) == 0);
  CHECK(wc::SanitizeUploadAheadPages(16) == 16);
  CHECK(wc::SanitizeUploadAheadPages(64) == 64);
  CHECK(wc::SanitizeUploadAheadPages(32) == 0);
  CHECK(wc::SanitizeUploadAheadPages(-16) == 0);
}

TEST_CASE("Upload-ahead bit scans match a page-by-page reference", "[watch-census][ahead]") {
  // Reference: grow one page at a time (forward in the last block, then
  // backward in the first block).
  const auto reference = [](uint32_t first, uint32_t last, uint64_t blocked_first,
                            uint64_t blocked_last, uint32_t min_page, uint32_t max_page,
                            uint32_t cap) {
    PageRun run{first, last};
    if (cap == 0) {
      return run;
    }
    const uint32_t forward_limit = std::min<uint32_t>(last | 63, max_page);
    while (run.count() < cap && run.last < forward_limit &&
           !((blocked_last >> ((run.last + 1) & 63)) & 1)) {
      ++run.last;
    }
    const uint32_t backward_limit = std::max<uint32_t>(first & ~uint32_t(63), min_page);
    while (run.count() < cap && run.first > backward_limit &&
           !((blocked_first >> ((run.first - 1) & 63)) & 1)) {
      --run.first;
    }
    return run;
  };
  uint64_t state = 0x9E3779B97F4A7C15ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (int i = 0; i < 20000; ++i) {
    const uint32_t block = uint32_t(next() % 4);
    const uint32_t first = block * 64 + uint32_t(next() % 64);
    const uint32_t length = 1 + uint32_t(next() % 80);
    const uint32_t last = first + length - 1;
    // Sparse and dense blocked masks.
    const uint64_t blocked_first = (next() & 1) ? next() & next() : next() | next();
    const uint64_t blocked_last = (next() & 1) ? next() & next() & next() : next();
    const uint32_t min_page = (next() & 3) ? 0 : first - std::min<uint32_t>(first, next() % 8);
    const uint32_t max_page = (next() & 3) ? UINT32_MAX : last + uint32_t(next() % 8);
    const uint32_t cap = (next() & 1) ? 16 : 64;
    REQUIRE(
        wc::ExtendUploadRun(first, last, blocked_first, blocked_last, min_page, max_page, cap) ==
        reference(first, last, blocked_first, blocked_last, min_page, max_page, cap));
  }
}

TEST_CASE("Watch census page bitmaps", "[watch-census][ahead]") {
  std::vector<uint64_t> bits(4, 0);
  CHECK_FALSE(wc::AllPagesSet(bits.data(), 60, 70));
  CHECK(wc::SetPages(bits.data(), 60, 70) == 11);
  CHECK(wc::AllPagesSet(bits.data(), 60, 70));
  CHECK(wc::AllPagesSet(bits.data(), 63, 64));
  CHECK_FALSE(wc::AllPagesSet(bits.data(), 59, 60));
  CHECK(wc::SetPages(bits.data(), 58, 61) == 2);
  wc::ClearPages(bits.data(), 64, 64);
  CHECK_FALSE(wc::AllPagesSet(bits.data(), 60, 70));
  CHECK(wc::AllPagesSet(bits.data(), 58, 63));
  CHECK(wc::SetPages(bits.data(), 0, 255) == 256 - 12);
  CHECK(wc::AllPagesSet(bits.data(), 0, 255));
}

TEST_CASE("Watch census SetPages counts new bits like a page-by-page reference",
          "[watch-census][ahead]") {
  uint64_t state = 0xD1B54A32D192ED03ull;
  const auto next = [&state] {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  };
  for (int i = 0; i < 5000; ++i) {
    std::vector<uint64_t> bits(4);
    for (uint64_t& word : bits) {
      word = (next() & 1) ? next() & next() : next();
    }
    std::vector<uint64_t> expected_bits = bits;
    const uint32_t first = uint32_t(next() % 256);
    const uint32_t last = first + uint32_t(next() % (256 - first));
    uint64_t expected_added = 0;
    for (uint32_t page = first; page <= last; ++page) {
      const uint64_t bit = uint64_t(1) << (page & 63);
      if (!(expected_bits[page >> 6] & bit)) {
        ++expected_added;
        expected_bits[page >> 6] |= bit;
      }
    }
    REQUIRE(wc::SetPages(bits.data(), first, last) == expected_added);
    REQUIRE(bits == expected_bits);
  }
}

TEST_CASE("Watch census block histories advance by the closed frame's fault bits",
          "[watch-census]") {
  // 130 blocks: three fault words, the last one partly used.
  constexpr size_t kBlocks = 130;
  std::vector<uint8_t> history(kBlocks);
  for (size_t b = 0; b < kBlocks; ++b) {
    history[b] = uint8_t(b * 37);
  }
  std::vector<uint64_t> faulted(3, 0);
  for (size_t b : {size_t(0), size_t(63), size_t(64), size_t(100), size_t(129)}) {
    faulted[b >> 6] |= uint64_t(1) << (b & 63);
  }
  std::vector<uint8_t> next_history(kBlocks, 0xAA);
  wc::AdvanceHistories(next_history.data(), history.data(), faulted.data(), kBlocks);
  for (size_t b = 0; b < kBlocks; ++b) {
    const bool hit = (faulted[b >> 6] >> (b & 63)) & 1;
    REQUIRE(next_history[b] == wc::AdvanceHistory(history[b], hit));
  }
  // In place (out == in) gives the same result.
  std::vector<uint8_t> in_place = history;
  wc::AdvanceHistories(in_place.data(), in_place.data(), faulted.data(), kBlocks);
  CHECK(in_place == next_history);
  // Zero blocks touch nothing.
  std::vector<uint8_t> untouched(4, 0x55);
  wc::AdvanceHistories(untouched.data(), history.data(), faulted.data(), 0);
  CHECK(untouched == std::vector<uint8_t>(4, 0x55));
}

TEST_CASE("Watch census snapshot difference and memory fields", "[watch-census]") {
  wc::Snapshot a, b;
  a.protect_calls[0][1] = 5;
  b.protect_calls[0][1] = 2;
  a.protect_ticks[0][1] = 3000;
  a.faults[1] = 7;
  b.faults[1] = 4;
  a.bridge_shadow_saved_calls = 9;
  const wc::Snapshot d = wc::Difference(a, b);
  CHECK(d.protect_calls[0][1] == 3);
  CHECK(d.faults[1] == 3);
  CHECK(d.bridge_shadow_saved_calls == 9);
  const std::string fields = wc::FormatMemoryFields(d, 1.0);  // 1 tick per us
  CHECK(fields.find("arm_calls=3 arm_ms=3.0") != std::string::npos);
  CHECK(fields.find("arm_calls_by_pages=1:0,2_4:3,5_16:0,17_64:0,65p:0") != std::string::npos);
  CHECK(fields.find("faults_render=3") != std::string::npos);
  CHECK(fields.find("bridge_shadow_saved_calls=9") != std::string::npos);
  CHECK(fields.find("disarm_calls=0") != std::string::npos);
}

TEST_CASE("Watch census counts protection calls by pages per call", "[watch-census][memory]") {
  REQUIRE(REXCVAR_GET(memory_watch_census));
  WatchedMemory fixture(8);
  const wc::Snapshot before = wc::GetSnapshot();
  fixture.Arm(0, 3);
  fixture.Arm(4);
  const wc::Snapshot after = wc::Difference(wc::GetSnapshot(), before);
  CHECK(after.protect_calls[0][0] == 1);
  CHECK(after.protect_calls[0][1] == 1);
  CHECK(after.protect_pages[0] == 4);
  CHECK(after.arm_walks == 2);
  rex::memory::WatchArmResult result;
  fixture.heap->EnableAccessCallbacks(fixture.physical + 6 * fixture.page_size,
                                      2 * fixture.page_size, true, false, &result);
  CHECK(result.calls == 1);
  rex::memory::WatchArmResult unchanged;
  fixture.heap->EnableAccessCallbacks(fixture.physical + 6 * fixture.page_size,
                                      2 * fixture.page_size, true, false, &unchanged);
  CHECK(unchanged.calls == 0);
}

TEST_CASE("Disarm bridging makes one call across safe gaps", "[watch-census][bridge][memory]") {
  BridgeCvar bridge(true);
  WatchedMemory fixture(7);
  for (uint32_t page : {0u, 2u, 5u}) {
    fixture.Arm(page);
  }
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census_before = wc::GetSnapshot();
  REQUIRE(fixture.TriggerAll(7));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census = wc::Difference(wc::GetSnapshot(), census_before);
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 1);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK(census.bridged_gaps == 2);
  CHECK(census.bridged_pages == 3);
  CHECK(census.protect_calls[1][2] == 1);  // one call of 6 pages
  for (uint32_t page = 0; page < 7; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }
  // Nothing stays watched.
  CHECK_FALSE(fixture.TriggerAll(7));
}

TEST_CASE("Disarm bridging never touches pages of unknown or unsafe state",
          "[watch-census][bridge][memory]") {
  BridgeCvar bridge(true);
  WatchedMemory fixture(3);
  SECTION("Read-only gap") {
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectRead));
  }
  SECTION("No-access gap") {
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectNoAccess));
  }
  SECTION("Decommitted gap") {
    REQUIRE(fixture.heap->Decommit(fixture.address + fixture.page_size, fixture.page_size));
  }
  const auto gap_access = fixture.HostAccess(1);
  fixture.Arm(0);
  fixture.Arm(2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.TriggerAll(3));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK(fixture.HostAccess(0) == rex::memory::PageAccess::kReadWrite);
  CHECK(fixture.HostAccess(1) == gap_access);
  CHECK(fixture.HostAccess(2) == rex::memory::PageAccess::kReadWrite);
}

TEST_CASE("Disarm bridging falls back to the legacy split on a heap try-lock miss",
          "[watch-census][bridge][memory]") {
  BridgeCvar bridge(true);
  WatchedMemory fixture(3);
  fixture.Arm(0);
  fixture.Arm(2);
  auto& heap_mutex = fixture.heap->*HeapMutexAccess::Member();
  std::promise<void> acquired;
  std::promise<void> release;
  auto release_future = release.get_future();
  std::thread holder([&heap_mutex, &acquired, &release_future] {
    std::unique_lock<std::recursive_mutex> lock(heap_mutex);
    acquired.set_value();
    release_future.wait();
  });
  acquired.get_future().wait();
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census_before = wc::GetSnapshot();
  auto trigger = std::async(std::launch::async, [&fixture] { return fixture.TriggerAll(3); });
  const bool completed = trigger.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  release.set_value();
  holder.join();
  CHECK(completed);
  REQUIRE(trigger.get());
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census = wc::Difference(wc::GetSnapshot(), census_before);
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_disarm_gap_lock_misses == before.watch_disarm_gap_lock_misses + 1);
  CHECK(census.bridged_gaps == 0);
  CHECK(census.gap_lock_misses == 1);
  for (uint32_t page = 0; page < 3; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }
}

TEST_CASE("Disarm bridge shadow counts the calls bridging would save",
          "[watch-census][bridge][memory]") {
  BridgeCvar bridge(false);
  WatchedMemory fixture(7);
  for (uint32_t page : {0u, 2u, 5u}) {
    fixture.Arm(page);
  }
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census_before = wc::GetSnapshot();
  REQUIRE(fixture.TriggerAll(7));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  const wc::Snapshot census = wc::Difference(wc::GetSnapshot(), census_before);
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 3);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + 2);
  CHECK(census.bridge_shadow_saved_calls == 2);
  CHECK(census.bridge_shadow_pages == 3);
  CHECK(census.bridged_gaps == 0);
  CHECK(census.disarm_walks == 1);
}

TEST_CASE("Physical writable page query for upload-ahead", "[watch-census][ahead][memory]") {
  WatchedMemory fixture(4);
  const auto writable = [&fixture](uint32_t page) {
    const uint32_t physical = fixture.physical + page * fixture.page_size;
    const uint32_t block_base = physical & ~uint32_t(0x3FFFF);
    uint64_t mask = 0;
    REQUIRE(fixture.memory.TryQueryPhysicalWritablePages(block_base, 64, &mask));
    return ((mask >> ((physical - block_base) >> 12)) & 1) != 0;
  };
  for (uint32_t page = 0; page < 4; ++page) {
    CHECK(writable(page));
  }
  REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                rex::memory::kMemoryProtectRead));
  CHECK(writable(0));
  CHECK_FALSE(writable(1));
  CHECK(writable(2));
  uint64_t unused = 0;
  CHECK_FALSE(fixture.memory.TryQueryPhysicalWritablePages(0, 65, &unused));
  CHECK_FALSE(fixture.memory.TryQueryPhysicalWritablePages(0x20000000, 1, &unused));
}
