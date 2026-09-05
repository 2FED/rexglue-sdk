#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <rex/memory/utils.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#if REX_PLATFORM_WIN32
namespace {
struct CommittedMapping {
  std::filesystem::path name =
      "rex_committed_mapping_test_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  size_t size = rex::memory::allocation_granularity();
  rex::memory::FileMappingHandle handle = rex::memory::CreateFileMappingHandle(
      name, size, rex::memory::PageAccess::kReadWrite, true);
  void* view = nullptr;

  ~CommittedMapping() {
    if (view) {
      rex::memory::UnmapFileView(handle, view, size);
    }
    if (handle != rex::memory::kFileMappingHandleInvalid) {
      rex::memory::CloseFileMappingHandle(handle, name);
    }
  }
};
}  // namespace

TEST_CASE("Committed section supports existing physical heap commit and protection",
          "[memory][physical-mapping]") {
  using namespace rex::memory;
  CommittedMapping mapping;
  REQUIRE(mapping.handle != kFileMappingHandleInvalid);
  mapping.view = MapFileView(mapping.handle, nullptr, mapping.size, PageAccess::kReadWrite, 0);
  REQUIRE(mapping.view != nullptr);
  auto* value = static_cast<volatile uint32_t*>(mapping.view);
  *value = 0x12345678;
  for (PageAccess requested : {PageAccess::kReadOnly, PageAccess::kNoAccess,
                               PageAccess::kReadWrite}) {
    REQUIRE(AllocFixed(mapping.view, mapping.size, AllocationType::kCommit, requested) ==
            mapping.view);
    size_t length = mapping.size;
    PageAccess actual;
    REQUIRE(QueryProtect(mapping.view, length, actual));
    CHECK(actual == requested);
  }
  CHECK(*value == 0x12345678);
}

TEST_CASE("Physical backing preserves every alias and virtual reservation",
          "[memory][physical-mapping]") {
  using namespace rex::memory;
  rex::InitLogging();
  Memory memory;
  REQUIRE(memory.Initialize());
  constexpr uint32_t kReadWrite = kMemoryProtectRead | kMemoryProtectWrite;
  // 7F is the first 16 MiB writeback alias. E has a guest 4 KiB offset
  // independent of the host allocation-granularity adjustment.
  for (uint32_t physical : {0x1000u, 0x00FFF000u, 0x1FD00FFCu}) {
    auto* raw = memory.TranslatePhysical<volatile uint32_t*>(physical);
    *raw = physical ^ 0x55AA;
    CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0xA0000000 + physical) == *raw);
    CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0xC0000000 + physical) == *raw);
    auto* e = memory.TranslateVirtual<volatile uint32_t*>(0xE0000000 + physical - 0x1000);
    CHECK(*e == *raw);
    *e = physical ^ 0xAA55;
    CHECK(*raw == (physical ^ 0xAA55));
    if (physical < 0x01000000) {
      CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0x7F000000 + physical) == *raw);
    }
  }
  auto* heap = const_cast<BaseHeap*>(memory.LookupHeap(0x20000000));
  HeapAllocationInfo info{};
  REQUIRE(heap->QueryRegionInfo(0x20000000, &info));
  CHECK(info.state == 0);
  size_t length = 4096;
  PageAccess access;
  REQUIRE(QueryProtect(memory.TranslateVirtual(0x20000000), length, access));
  CHECK(access == PageAccess::kNoAccess);
  REQUIRE(heap->AllocFixed(0x20000000, 4096, 4096, kMemoryAllocationReserve, kReadWrite));
  REQUIRE(heap->QueryRegionInfo(0x20000000, &info));
  CHECK(info.state == kMemoryAllocationReserve);
  REQUIRE(heap->AllocFixed(0x20000000, 4096, 4096, kMemoryAllocationCommit, kReadWrite));
  *memory.TranslateVirtual<volatile uint32_t*>(0x20000000) = 0xBEEF;
  CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0x20000000) == 0xBEEF);
  // The encrypted/XEX virtual alias must not accidentally use physical RAM.
  auto* xex = const_cast<BaseHeap*>(memory.LookupHeap(0x81000000));
  REQUIRE(xex->AllocFixed(0x81000000, 0x10000, 0x10000,
                         kMemoryAllocationReserve | kMemoryAllocationCommit, kReadWrite));
  *memory.TranslateVirtual<volatile uint32_t*>(0x81000000) = 0xC0FFEE;
  CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0x91000000) == 0xC0FFEE);
}

TEST_CASE("Physical alias protections remain independent and write watches rearm",
          "[memory][physical-mapping]") {
  using namespace rex::memory;
  rex::InitLogging();
  Memory memory;
  REQUIRE(memory.Initialize());
  auto* a = const_cast<BaseHeap*>(memory.LookupHeap(0xA0000000));
  constexpr uint32_t address = 0xA2000000;
  REQUIRE(a->AllocFixed(address, 0x10000, 0x10000,
                       kMemoryAllocationReserve | kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite));
  size_t length = 4096;
  PageAccess access;
  auto* c = memory.TranslateVirtual(0xC2000000);
  REQUIRE(Protect(memory.TranslateVirtual(address), 4096, PageAccess::kReadOnly));
  REQUIRE(QueryProtect(c, length, access));
  CHECK(access == PageAccess::kReadWrite);
  REQUIRE(Protect(memory.TranslateVirtual(address), 4096, PageAccess::kReadWrite));
  const auto before = memory.GetAccessDiagnosticSnapshot();
  for (uint32_t value : {1u, 2u}) {
    memory.EnablePhysicalMemoryAccessCallbacks(0x02000000, 4096, true, false);
    REQUIRE(QueryProtect(memory.TranslateVirtual(address), length, access));
    CHECK(access == PageAccess::kReadOnly);
    *memory.TranslateVirtual<volatile uint32_t*>(address) = value;
    CHECK(*memory.TranslatePhysical<volatile uint32_t*>(0x02000000) == value);
  }
  const auto after = memory.GetAccessDiagnosticSnapshot();
  CHECK(after.non_mmio_write_fault_count == before.non_mmio_write_fault_count + 2);
  CHECK(after.watch_protect_failures == before.watch_protect_failures);
}

TEST_CASE("Physical guest rights are independent of committed backing",
          "[memory][physical-mapping]") {
  using namespace rex::memory;
  rex::InitLogging();
  Memory memory;
  REQUIRE(memory.Initialize());
  auto* a = const_cast<BaseHeap*>(memory.LookupHeap(0xA0000000));
  constexpr uint32_t address = 0xA2000000;
  HeapAllocationInfo info{};
  REQUIRE(a->QueryRegionInfo(address, &info));
  CHECK(info.state == 0);
  REQUIRE(a->AllocFixed(address, 0x10000, 0x10000, kMemoryAllocationReserve,
                       kMemoryProtectRead));
  REQUIRE(a->QueryRegionInfo(address, &info));
  CHECK(info.state == kMemoryAllocationReserve);
  REQUIRE(a->AllocFixed(address, 0x10000, 0x10000, kMemoryAllocationCommit,
                       kMemoryProtectRead));
  for (uint32_t rights : {uint32_t(kMemoryProtectRead), uint32_t(kMemoryProtectNoAccess)}) {
    REQUIRE(a->Protect(address, 0x10000, rights));
    auto before = memory.GetAccessDiagnosticSnapshot();
    memory.EnablePhysicalMemoryAccessCallbacks(0x02000000, 4096, true, false);
    CHECK(memory.GetAccessDiagnosticSnapshot().watch_arm_calls == before.watch_arm_calls);
    REQUIRE(a->QueryRegionInfo(address, &info));
    CHECK(info.protect == rights);
    size_t length = 4096;
    PageAccess access;
    REQUIRE(QueryProtect(memory.TranslateVirtual(address), length, access));
    CHECK(access == (rights == kMemoryProtectRead ? PageAccess::kReadOnly : PageAccess::kNoAccess));
  }
}

TEST_CASE("Partial mapping collision cleans views before retry and destruction",
          "[memory][physical-mapping]") {
  using namespace rex::memory;
  rex::InitLogging();
  // Force failure after several successful views at the first candidate base.
  struct Reservation {
    void* base = nullptr;
    ~Reservation() {
      if (base) DeallocFixed(base, 0, DeallocationType::kRelease);
    }
  } collision, reused;
  const auto first_base = uintptr_t(1) << 32;
  collision.base = AllocFixed(reinterpret_cast<void*>(first_base + 0x80000000),
                              allocation_granularity(), AllocationType::kReserve,
                              PageAccess::kNoAccess);
  REQUIRE(collision.base != nullptr);
  for (int cycle = 0; cycle < 2; ++cycle) {
    Memory memory;
    REQUIRE(memory.Initialize());
    CHECK(memory.TranslateVirtual(0) != reinterpret_cast<void*>(first_base));
    *memory.TranslatePhysical<volatile uint32_t*>(0x1000) = 0xABCD;
    CHECK(*memory.TranslateVirtual<volatile uint32_t*>(0xE0000000) == 0xABCD);
  }
  reused.base = AllocFixed(reinterpret_cast<void*>(first_base), allocation_granularity(),
                           AllocationType::kReserve, PageAccess::kNoAccess);
  // A leaked first view would prevent re-reserving this exact address.
  REQUIRE(reused.base != nullptr);
}
#endif
