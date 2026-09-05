#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

namespace {

constexpr uint32_t kReadWrite =
    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite;

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
    REQUIRE(heap->Alloc(page_size * pages, page_size,
                        rex::memory::kMemoryAllocationReserve |
                            rex::memory::kMemoryAllocationCommit,
                        kReadWrite, false, &address));
    physical = memory.GetPhysicalAddress(address);
    REQUIRE(physical != UINT32_MAX);
  }

  void Arm(uint32_t first, uint32_t count = 1) {
    // Use one real alias to make call counts deterministic. Guest-to-host
    // translation still exercises the E-heap's platform-specific offset.
    heap->EnableAccessCallbacks(physical + first * page_size, count * page_size, true, false);
  }

  rex::memory::PageAccess HostAccess(uint32_t page) {
    size_t length = page_size;
    rex::memory::PageAccess access;
    REQUIRE(rex::memory::QueryProtect(memory.TranslateVirtual(address + page * page_size), length,
                                      access));
    return access;
  }
};

std::pair<uint32_t, uint32_t> CountInvalidation(void* context, uint32_t first, uint32_t length,
                                               bool) {
  static_cast<std::atomic<uint64_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
  return {first, length};
}

struct BoundedInvalidation {
  rex::memory::Memory& memory;
  uint32_t first;
  uint32_t length;
  uint32_t calls = 0;
  uint32_t requested_first = 0;
  uint32_t requested_length = 0;
  bool requested_exact = false;
  void* handle;

  BoundedInvalidation(rex::memory::Memory& memory, uint32_t first, uint32_t length)
      : memory(memory), first(first), length(length) {
    handle = memory.RegisterPhysicalMemoryInvalidationCallback(
        [](void* context, uint32_t requested_first, uint32_t requested_length, bool exact) {
          auto& self = *static_cast<BoundedInvalidation*>(context);
          ++self.calls;
          self.requested_first = requested_first;
          self.requested_length = requested_length;
          self.requested_exact = exact;
          return std::pair<uint32_t, uint32_t>{self.first, self.length};
        },
        this);
  }

  ~BoundedInvalidation() { memory.UnregisterPhysicalMemoryInvalidationCallback(handle); }
};

// Form a protected base-member pointer legally through a derived access shim,
// then apply it to the actual BaseHeap subobject. No downcast or fake object.
struct HeapMutexAccess : rex::memory::BaseHeap {
  static auto Member() { return &HeapMutexAccess::heap_mutex_; }
};

class HeldHeapMutex {
 public:
  explicit HeldHeapMutex(std::recursive_mutex& mutex) {
    std::promise<void> acquired;
    auto ready = acquired.get_future();
    auto release = release_.get_future();
    holder_ = std::thread([&mutex, acquired = std::move(acquired),
                           release = std::move(release)]() mutable {
      std::unique_lock<std::recursive_mutex> lock(mutex);
      acquired.set_value();
      release.wait();
    });
    ready.get();
  }

  HeldHeapMutex(const HeldHeapMutex&) = delete;
  HeldHeapMutex& operator=(const HeldHeapMutex&) = delete;
  ~HeldHeapMutex() { Release(); }

  void Release() {
    if (holder_.joinable()) {
      release_.set_value();
      holder_.join();
    }
  }

 private:
  std::promise<void> release_;
  std::thread holder_;
};

}  // namespace

TEST_CASE("Uninitialized memory exposes zero detailed access diagnostics", "[memory][diagnostics]") {
  rex::memory::Memory memory;
  const auto snapshot = memory.GetAccessDiagnosticSnapshot();
  CHECK(snapshot.non_mmio_write_fault_count == 0);
  CHECK(snapshot.non_mmio_write_fault_us == 0);
  CHECK(snapshot.write_fault_lock_wait_us == 0);
  CHECK(snapshot.write_fault_query_us == 0);
  CHECK(snapshot.write_fault_callback_us == 0);
  CHECK(snapshot.write_fault_already_cleared_count == 0);
  CHECK(snapshot.watch_arm_calls == 0);
  CHECK(snapshot.watch_arm_us == 0);
  CHECK(snapshot.watch_arm_lock_wait_us == 0);
  CHECK(snapshot.watch_disarm_calls == 0);
  CHECK(snapshot.watch_disarm_us == 0);
  CHECK(snapshot.watch_protect_failures == 0);
  CHECK(snapshot.watch_arm_gap_splits == 0);
  CHECK(snapshot.watch_disarm_safe_gap_splits == 0);
  CHECK(snapshot.watch_disarm_gap_lock_misses == 0);
}

TEST_CASE("Watch diagnostics count actual calls without coalescing protection", "[memory][diagnostics]") {
  WatchedMemory fixture(4);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  fixture.Arm(1);
  CHECK(fixture.HostAccess(1) == rex::memory::PageAccess::kReadOnly);
  fixture.Arm(0, 3);
  const auto armed = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(armed.watch_arm_calls == before.watch_arm_calls + 3);
  CHECK(armed.watch_arm_gap_splits == before.watch_arm_gap_splits + 1);
  CHECK(armed.watch_arm_us >= before.watch_arm_us);
  CHECK(armed.watch_arm_lock_wait_us >= before.watch_arm_lock_wait_us);
  CHECK(armed.watch_protect_failures == before.watch_protect_failures);
  for (uint32_t page = 0; page < 3; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadOnly);
  }
  CHECK(fixture.HostAccess(3) == rex::memory::PageAccess::kReadWrite);

  fixture.Arm(0, 3);
  const auto unchanged = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(unchanged.watch_arm_calls == armed.watch_arm_calls);
  CHECK(unchanged.watch_arm_us == armed.watch_arm_us);
  CHECK(unchanged.watch_arm_gap_splits == armed.watch_arm_gap_splits);

  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          fixture.address, fixture.page_size * 3, true, true));
  const auto disarmed = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(disarmed.watch_disarm_calls == before.watch_disarm_calls + 1);
  CHECK(disarmed.watch_disarm_us >= before.watch_disarm_us);
  // Explicit callback triggering is not a CPU access violation.
  CHECK(disarmed.non_mmio_write_fault_count == before.non_mmio_write_fault_count);
  for (uint32_t page = 0; page < 4; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }
}

TEST_CASE("Watch gap diagnostics exclude trailing and unsafe gaps", "[memory][diagnostics]") {
  WatchedMemory fixture(4);
  uint64_t expected_new_calls = 0;
  uint64_t expected_gap_splits = 0;

  SECTION("Leading and trailing watched pages are not splits") {
    fixture.Arm(0);
    fixture.Arm(2);
    expected_new_calls = 1;
  }
  SECTION("A two-page watched gap counts once") {
    fixture.Arm(1, 2);
    expected_new_calls = 2;
    expected_gap_splits = 1;
  }
  SECTION("Guest read-only page stops a safe gap") {
    fixture.Arm(1);
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectRead));
    expected_new_calls = 2;
  }
  SECTION("Decommitted page stops a safe gap") {
    REQUIRE(fixture.heap->Decommit(fixture.address + fixture.page_size, fixture.page_size));
    // Decommit currently clears guest metadata only. Existing arming still
    // permits a watch here; diagnostics must not call it a safe bridge.
    fixture.Arm(1);
    expected_new_calls = 2;
  }
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  fixture.Arm(0, expected_gap_splits ? 4 : 3);
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_arm_calls == before.watch_arm_calls + expected_new_calls);
  CHECK(after.watch_arm_gap_splits == before.watch_arm_gap_splits + expected_gap_splits);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
}

TEST_CASE("Disarm safe-gap diagnostics do not coalesce native protection calls",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(7);
  std::vector<uint32_t> watched{0, 2};
  uint64_t expected_calls = 2;
  uint64_t expected_gaps = 1;
  SECTION("One-page gap") {}
  SECTION("Two-page gap counts once") { watched = {0, 3}; }
  SECTION("Three watched islands count two gaps") {
    watched = {0, 2, 4};
    expected_calls = 3;
    expected_gaps = 2;
  }
  SECTION("Multi-page runs count one gap") { watched = {0, 1, 3, 4}; }
  SECTION("Leading and trailing gaps are not splits") {
    watched = {2};
    expected_calls = 1;
    expected_gaps = 0;
  }
  for (uint32_t page : watched) {
    fixture.Arm(page);
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadOnly);
  }
  BoundedInvalidation callback(fixture.memory, fixture.physical, fixture.page_size * 7);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 7, true, true));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + expected_calls);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + expected_gaps);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK(after.watch_arm_calls == before.watch_arm_calls);
  CHECK(callback.calls == 1);
  CHECK(callback.requested_first == fixture.physical);
  CHECK(callback.requested_length == fixture.page_size * 7);
  CHECK(callback.requested_exact);
  for (uint32_t page = 0; page < 7; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }
  CHECK_FALSE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                            fixture.address, fixture.page_size * 7, true, true));
  const auto repeated = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(repeated.watch_disarm_safe_gap_splits == after.watch_disarm_safe_gap_splits);
  CHECK(repeated.watch_disarm_calls == after.watch_disarm_calls);
  CHECK(callback.calls == 1);
}

TEST_CASE("Disarm safe-gap diagnostics reject unsafe gaps without changing guest rights",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(3);
  SECTION("Read-only gap") {
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectRead));
  }
  SECTION("No-access gap") {
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectNoAccess));
  }
  SECTION("Decommitted gap with stale RW permission") {
    REQUIRE(fixture.heap->Decommit(fixture.address + fixture.page_size, fixture.page_size));
  }
  SECTION("Watched read-only gap is not a safe bridge") {
    fixture.Arm(1);
    REQUIRE(fixture.heap->Protect(fixture.address + fixture.page_size, fixture.page_size,
                                  rex::memory::kMemoryProtectRead));
  }
  const auto gap_access = fixture.HostAccess(1);
  rex::memory::HeapAllocationInfo gap_before{};
  REQUIRE(fixture.heap->QueryRegionInfo(fixture.address + fixture.page_size, &gap_before));
  fixture.Arm(0);
  fixture.Arm(2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 3, true, true));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK(fixture.HostAccess(0) == rex::memory::PageAccess::kReadWrite);
  CHECK(fixture.HostAccess(1) == gap_access);
  CHECK(fixture.HostAccess(2) == rex::memory::PageAccess::kReadWrite);
  rex::memory::HeapAllocationInfo gap_after{};
  REQUIRE(fixture.heap->QueryRegionInfo(fixture.address + fixture.page_size, &gap_after));
  CHECK(gap_after.state == gap_before.state);
  CHECK(gap_after.protect == gap_before.protect);
}

TEST_CASE("Disarm safe-gap diagnostics require complete committed neighboring runs",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(5);
  uint32_t decommitted_page = 0;
  SECTION("Beginning of left run is uncommitted") {}
  SECTION("End of left run is uncommitted") { decommitted_page = 1; }
  SECTION("Beginning of right run is uncommitted") { decommitted_page = 3; }
  SECTION("End of right run is uncommitted") { decommitted_page = 4; }
  REQUIRE(fixture.heap->Decommit(fixture.address + decommitted_page * fixture.page_size,
                                 fixture.page_size));
  // Existing arming and disarming use stale RW protection even after Decommit.
  // Do not change that behavior or classify the whole native run as a bridge.
  fixture.Arm(0, 2);
  fixture.Arm(3, 2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 5, true, true));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  rex::memory::HeapAllocationInfo info{};
  REQUIRE(fixture.heap->QueryRegionInfo(fixture.address + decommitted_page * fixture.page_size,
                                        &info));
  CHECK((info.state & rex::memory::kMemoryAllocationCommit) == 0);
}

TEST_CASE("Disarm safe-gap diagnostics do not bridge separate calls or unprotect-false paths",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(3);
  fixture.Arm(0);
  fixture.Arm(2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  uint64_t expected_calls = 0;
  SECTION("Separate approved ranges") {
    REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          fixture.address, fixture.page_size, true, true));
    REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          fixture.address + fixture.page_size * 2,
                                          fixture.page_size, true, true));
    expected_calls = 2;
  }
  SECTION("Unwatch without changing protection") {
    BoundedInvalidation callback(fixture.memory, fixture.physical, fixture.page_size * 3);
    REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          fixture.address, fixture.page_size * 3, true, false, false));
    CHECK(callback.calls == 1);
    CHECK(callback.requested_exact);
    CHECK(fixture.HostAccess(0) == rex::memory::PageAccess::kReadOnly);
    CHECK(fixture.HostAccess(2) == rex::memory::PageAccess::kReadOnly);
  }
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + expected_calls);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits);
  CHECK(after.watch_disarm_gap_lock_misses == before.watch_disarm_gap_lock_misses);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK_FALSE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                            fixture.address, fixture.page_size * 3, true, true));
}

TEST_CASE("Disarm safe-gap diagnostics resume after a complete eligible run",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(7);
  REQUIRE(fixture.heap->Decommit(fixture.address + fixture.page_size * 2, fixture.page_size));
  for (uint32_t page : {0u, 2u, 4u, 6u}) {
    fixture.Arm(page);
  }
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 7, true, true));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 4);
  // Neither side of the uncommitted run qualifies, but the later 4 -> 6 pair does.
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + 1);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
}

TEST_CASE("Disarm safe-gap diagnostics respect intersected callback ranges",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(5);
  fixture.Arm(1);
  fixture.Arm(3);
  bool allow_both = false;
  SECTION("Second callback restricts expansion") {}
  SECTION("Both callbacks allow both islands") { allow_both = true; }
  BoundedInvalidation broad(fixture.memory, fixture.physical, fixture.page_size * 5);
  BoundedInvalidation bounded(fixture.memory, fixture.physical + fixture.page_size,
                              fixture.page_size * (allow_both ? 3 : 1));
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address + fixture.page_size, fixture.page_size,
                                        true, false));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + (allow_both ? 2 : 1));
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits +
                                                 (allow_both ? 1 : 0));
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  for (const auto* callback : {&broad, &bounded}) {
    CHECK(callback->calls == 1);
    CHECK(callback->requested_first == fixture.physical + fixture.page_size);
    CHECK(callback->requested_length == fixture.page_size);
    CHECK_FALSE(callback->requested_exact);
  }
  CHECK(fixture.HostAccess(1) == rex::memory::PageAccess::kReadWrite);
  CHECK(fixture.HostAccess(2) == rex::memory::PageAccess::kReadWrite);
  CHECK(fixture.HostAccess(3) == (allow_both ? rex::memory::PageAccess::kReadWrite
                                          : rex::memory::PageAccess::kReadOnly));
  // A callback-excluded island remains watched, not merely read-only by chance.
  CHECK(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                      fixture.address + fixture.page_size * 3,
                                      fixture.page_size, true, true) == !allow_both);
}

TEST_CASE("Disarm safe-gap diagnostics do not cross the excess-unwatch limit",
          "[memory][diagnostics][disarm-gap]") {
  const uint32_t distant_page = (4 * 1024 * 1024) / uint32_t(rex::memory::page_size()) + 1;
  WatchedMemory fixture(distant_page + 1);
  fixture.Arm(0);
  fixture.Arm(distant_page);
  BoundedInvalidation broad(fixture.memory, fixture.physical,
                             fixture.page_size * (distant_page + 1));
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size, true, false));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 1);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  CHECK(fixture.HostAccess(distant_page) == rex::memory::PageAccess::kReadOnly);
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address + distant_page * fixture.page_size,
                                        fixture.page_size, true, true));
}

TEST_CASE("Disarm safe-gap diagnostics use each physical alias host-page mapping",
          "[memory][diagnostics][disarm-gap]") {
  uint32_t alias = 0xE0000000;
  SECTION("E physical alias") {}
  SECTION("A physical alias") { alias = 0xA0000000; }
  SECTION("C physical alias") { alias = 0xC0000000; }
  WatchedMemory fixture(3, alias);
  fixture.Arm(0);
  fixture.Arm(2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 3, true, true));
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + 1);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  for (uint32_t page = 0; page < 3; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }
}

TEST_CASE("Disarm safe-gap aggregation loses no concurrent updates",
          "[memory][diagnostics][disarm-gap]") {
  constexpr uint32_t kThreads = 4;
  constexpr uint32_t kIterations = 8;
  WatchedMemory fixture(kThreads * 3);
  std::atomic<bool> callbacks_succeeded{true};
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  std::vector<std::thread> workers;
  for (uint32_t thread = 0; thread < kThreads; ++thread) {
    workers.emplace_back([&fixture, &callbacks_succeeded, thread] {
      const uint32_t first = thread * 3;
      for (uint32_t i = 0; i < kIterations; ++i) {
        fixture.Arm(first);
        fixture.Arm(first + 2);
        if (!fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                             fixture.address + first * fixture.page_size,
                                             fixture.page_size * 3, true, true)) {
          callbacks_succeeded.store(false, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  constexpr uint64_t kEpisodes = kThreads * kIterations;
  CHECK(callbacks_succeeded.load(std::memory_order_relaxed));
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + kEpisodes * 2);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + kEpisodes);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
}

TEST_CASE("Disarm gap classification skips contended heap metadata and resumes after release",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(3);
  fixture.Arm(0);
  fixture.Arm(2);
  BoundedInvalidation callback(fixture.memory, fixture.physical, fixture.page_size * 3);
  auto& heap_mutex = fixture.heap->*HeapMutexAccess::Member();
  HeldHeapMutex holder(heap_mutex);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  auto trigger = std::async(std::launch::async, [&fixture] {
    return fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           fixture.address, fixture.page_size * 3, true, true);
  });
  // This timeout is only a deadlock guard, not a performance threshold. Release
  // the holder before any assertion/get, so a blocking regression can finish.
  const bool completed_before_release =
      trigger.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
  holder.Release();
  const bool triggered = trigger.get();
  CHECK(completed_before_release);
  REQUIRE(triggered);
  const auto skipped = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(skipped.watch_disarm_gap_lock_misses == before.watch_disarm_gap_lock_misses + 1);
  CHECK(skipped.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits);
  CHECK(skipped.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(skipped.watch_protect_failures == before.watch_protect_failures);
  CHECK(callback.calls == 1);
  CHECK(callback.requested_exact);
  for (uint32_t page = 0; page < 3; ++page) {
    CHECK(fixture.HostAccess(page) == rex::memory::PageAccess::kReadWrite);
  }

  fixture.Arm(0);
  fixture.Arm(2);
  REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        fixture.address, fixture.page_size * 3, true, true));
  const auto resumed = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(resumed.watch_disarm_gap_lock_misses == skipped.watch_disarm_gap_lock_misses);
  CHECK(resumed.watch_disarm_safe_gap_splits == skipped.watch_disarm_safe_gap_splits + 1);
  CHECK(resumed.watch_disarm_calls == skipped.watch_disarm_calls + 2);
  CHECK(resumed.watch_protect_failures == skipped.watch_protect_failures);
  CHECK(callback.calls == 2);
}

TEST_CASE("Disarm gap classification permits existing same-thread recursive heap ownership",
          "[memory][diagnostics][disarm-gap]") {
  WatchedMemory fixture(3);
  fixture.Arm(0);
  fixture.Arm(2);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  {
    std::unique_lock<std::recursive_mutex> lock(fixture.heap->*HeapMutexAccess::Member());
    REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                          fixture.address, fixture.page_size * 3, true, true));
  }
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_disarm_gap_lock_misses == before.watch_disarm_gap_lock_misses);
  CHECK(after.watch_disarm_safe_gap_splits == before.watch_disarm_safe_gap_splits + 1);
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
}

TEST_CASE("Memory merges successful real write faults with watch diagnostics", "[memory][diagnostics]") {
  WatchedMemory fixture(1);
  std::atomic<uint64_t> callbacks{0};
  auto* handle = fixture.memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation,
                                                                            &callbacks);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  fixture.Arm(0);
  auto* byte = fixture.memory.TranslateVirtual<volatile uint8_t*>(fixture.address);
  const uint8_t previous = *byte;
  (void)previous;
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().non_mmio_write_fault_count ==
        before.non_mmio_write_fault_count);
  *byte = 0x5A;
  const auto first = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(*byte == 0x5A);
  CHECK(callbacks.load(std::memory_order_relaxed) == 1);
  CHECK(first.non_mmio_write_fault_count == before.non_mmio_write_fault_count + 1);
  CHECK(first.write_fault_already_cleared_count == before.write_fault_already_cleared_count);
  CHECK(first.watch_arm_calls == before.watch_arm_calls + 1);
  CHECK(first.watch_disarm_calls == before.watch_disarm_calls + 1);
  const uint64_t phase_us = first.write_fault_lock_wait_us - before.write_fault_lock_wait_us +
                            first.write_fault_query_us - before.write_fault_query_us +
                            first.write_fault_callback_us - before.write_fault_callback_us;
  CHECK(phase_us <= first.non_mmio_write_fault_us - before.non_mmio_write_fault_us);
  *byte = 0x6B;
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().non_mmio_write_fault_count ==
        first.non_mmio_write_fault_count);

  fixture.Arm(0);
  *byte = 0x7C;
  const auto second = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(callbacks.load(std::memory_order_relaxed) == 2);
  CHECK(second.non_mmio_write_fault_count == before.non_mmio_write_fault_count + 2);
  CHECK(second.watch_arm_calls == before.watch_arm_calls + 2);
  CHECK(second.watch_disarm_calls == before.watch_disarm_calls + 2);
  CHECK(second.write_fault_lock_wait_us >= first.write_fault_lock_wait_us);
  CHECK(second.write_fault_query_us >= first.write_fault_query_us);
  CHECK(second.write_fault_callback_us >= first.write_fault_callback_us);
  CHECK(second.watch_protect_failures == before.watch_protect_failures);
  fixture.memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
}

TEST_CASE("Watch and fault diagnostic aggregation loses no concurrent updates", "[memory][diagnostics]") {
  constexpr uint32_t kThreads = 4;
  constexpr uint32_t kIterations = 8;
  WatchedMemory fixture(kThreads);
  std::atomic<uint64_t> callbacks{0};
  auto* handle = fixture.memory.RegisterPhysicalMemoryInvalidationCallback(CountInvalidation,
                                                                            &callbacks);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  std::vector<std::thread> workers;
  for (uint32_t page = 0; page < kThreads; ++page) {
    workers.emplace_back([&fixture, page] {
      auto* byte = fixture.memory.TranslateVirtual<volatile uint8_t*>(fixture.address +
                                                                     page * fixture.page_size);
      for (uint32_t i = 0; i < kIterations; ++i) {
        fixture.Arm(page);
        *byte = uint8_t(i + 1);
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  constexpr uint64_t kWrites = kThreads * kIterations;
  CHECK(callbacks.load(std::memory_order_relaxed) == kWrites);
  CHECK(after.non_mmio_write_fault_count == before.non_mmio_write_fault_count + kWrites);
  CHECK(after.watch_arm_calls == before.watch_arm_calls + kWrites);
  CHECK(after.watch_disarm_calls == before.watch_disarm_calls + kWrites);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
  fixture.memory.UnregisterPhysicalMemoryInvalidationCallback(handle);
}
