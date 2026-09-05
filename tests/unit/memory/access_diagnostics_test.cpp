#include <catch2/catch_test_macros.hpp>

#include <atomic>
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

  explicit WatchedMemory(uint32_t pages) {
    rex::InitLogging();
    REQUIRE(memory.Initialize());
    heap = static_cast<rex::memory::PhysicalHeap*>(memory.LookupHeap(0xE0000000));
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
