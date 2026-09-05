#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

REXCVAR_DECLARE(int32_t, primitive_processor_cache_min_indices);

namespace rex::graphics::test {
namespace {

class ScopedCacheThreshold {
 public:
  explicit ScopedCacheThreshold(int32_t value)
      : previous_(REXCVAR_GET(primitive_processor_cache_min_indices)) {
    REXCVAR_SET(primitive_processor_cache_min_indices, value);
  }
  ~ScopedCacheThreshold() { REXCVAR_SET(primitive_processor_cache_min_indices, previous_); }
  ScopedCacheThreshold(const ScopedCacheThreshold&) = delete;
  ScopedCacheThreshold& operator=(const ScopedCacheThreshold&) = delete;

 private:
  int32_t previous_;
};

// The exercised CPU-converted DMA path must never request a GPU upload.
class CpuSharedMemory final : public SharedMemory {
 public:
  explicit CpuSharedMemory(memory::Memory& memory) : SharedMemory(memory) {}
  void Initialize() { InitializeCommon(); }
  void StopWatching() { ShutdownCommon(); }
  uint32_t upload_calls = 0;

 private:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>&) override {
    ++upload_calls;
    return false;
  }
};

class CpuPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  CpuPrimitiveProcessor(const RegisterFile& registers, memory::Memory& memory, TraceWriter& trace,
                        SharedMemory& shared)
      : PrimitiveProcessor(registers, memory, trace, shared) {}

  bool Initialize() {
    // Force only triangle-fan conversion through host capabilities, not another
    // global override. No device, window, command processor or title is created.
    return InitializeCommon(true, false, true, true, true, true);
  }
  void ClearCache() { ClearPerFrameCache(); }
  size_t allocation_count() const { return converted_.size(); }
  std::function<void()> on_next_allocation;

  std::vector<uint16_t> Indices(const ProcessingResult& result) const {
    REQUIRE(result.index_buffer_type == kHostConverted);
    REQUIRE(result.host_index_format == xenos::IndexFormat::kInt16);
    REQUIRE(result.host_index_buffer_handle < converted_.size());
    const auto& buffer = converted_[result.host_index_buffer_handle];
    std::vector<uint16_t> indices(result.host_draw_vertex_count);
    REQUIRE(indices.size() * sizeof(uint16_t) == buffer.length);
    std::memcpy(indices.data(),
                reinterpret_cast<const uint8_t*>(buffer.words.data()) + buffer.offset,
                buffer.length);
    return indices;
  }

 private:
  struct Buffer {
    std::vector<uint32_t> words;
    size_t offset = 0;
    size_t length = 0;
  };

  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override {
    builtin_.resize((size_bytes + sizeof(uint32_t) - 1) / sizeof(uint32_t));
    fill_callback(builtin_.data());
    return true;
  }

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t original_address,
                                                       size_t& handle_out) override {
    // Deliberately reenter invalidation while the production CacheTransaction
    // is open. Moving the hook prevents subsequent clean conversions reusing it.
    auto hook = std::move(on_next_allocation);
    on_next_allocation = {};
    if (hook) {
      hook();
    }
    const size_t length =
        size_t(index_count) *
        (format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t));
    handle_out = converted_.size();
    auto& buffer = converted_.emplace_back();
    buffer.length = length;
    buffer.words.resize((length + XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE + sizeof(uint32_t) - 1) /
                        sizeof(uint32_t));
    if (coalign_for_simd) {
      buffer.offset = size_t(GetSimdCoalignmentOffset(buffer.words.data(), original_address));
    }
    // Every returned allocation remains alive, including across explicit cache
    // clears, so an old result cannot accidentally point into reused storage.
    return reinterpret_cast<uint8_t*>(buffer.words.data()) + buffer.offset;
  }

  std::vector<uint32_t> builtin_;
  std::deque<Buffer> converted_;
};

struct CacheFixture {
  ScopedCacheThreshold threshold;
  memory::Memory memory;
  RegisterFile registers;
  std::unique_ptr<TraceWriter> trace;
  CpuSharedMemory shared;
  std::unique_ptr<CpuPrimitiveProcessor> processor;
  memory::PhysicalHeap* heap = nullptr;
  uint32_t address = 0;
  uint32_t physical = 0;
  std::vector<uint16_t> source;

  explicit CacheFixture(uint32_t count = 4096, int32_t minimum = 4096, uint32_t alias = 0xE0000000,
                        bool cross_bucket = false)
      : threshold(minimum), shared(memory), source(count) {
    rex::InitLogging();
    REQUIRE(memory.Initialize());
    // An unopened trace writer makes WriteMemoryRead a no-op: no trace files.
    trace = std::make_unique<TraceWriter>(memory.physical_membase());
    shared.Initialize();
    processor = std::make_unique<CpuPrimitiveProcessor>(registers, memory, *trace, shared);
    REQUIRE(processor->Initialize());
    heap = static_cast<memory::PhysicalHeap*>(memory.LookupHeap(alias));
    REQUIRE(heap != nullptr);
    REQUIRE(heap->Alloc(cross_bucket ? 0x80000 : 0x10000, cross_bucket ? 0x40000 : 0x10000,
                        memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                        memory::kMemoryProtectRead | memory::kMemoryProtectWrite, false, &address));
    // Keep both neighboring ranges inside the allocation. The boundary fixture
    // deliberately spans two 256 KiB cache buckets; ordinary fixtures use one.
    if (cross_bucket) {
      const uint32_t allocation_physical = memory.GetPhysicalAddress(address);
      REQUIRE(allocation_physical != UINT32_MAX);
      const uint32_t next_bucket = (allocation_physical + 0x40000) & ~uint32_t(0x3FFFF);
      REQUIRE(next_bucket - allocation_physical >= 0x1000);
      address += next_bucket - allocation_physical - 0x1000;
    } else {
      address += 0x4000;
    }
    physical = memory.GetPhysicalAddress(address);
    REQUIRE(physical != UINT32_MAX);
    REQUIRE(count >= 3);
    REQUIRE(count <= 4096);
    REQUIRE(((physical >> 18) != ((physical + bytes() - 1) >> 18)) == cross_bucket);
    for (uint32_t i = 0; i < count; ++i) {
      source[i] = uint16_t(i + 1);
    }
    std::memcpy(memory.TranslateVirtual(address), source.data(), bytes());

    reg::VGT_DRAW_INITIATOR draw{};
    draw.prim_type = xenos::PrimitiveType::kTriangleFan;
    draw.source_select = xenos::SourceSelect::kDMA;
    draw.major_mode = xenos::MajorMode::kImplicit;
    draw.index_size = xenos::IndexFormat::kInt16;
    draw.num_indices = count;
    registers[reg::VGT_DRAW_INITIATOR::register_index] = draw.value;
    reg::VGT_DMA_SIZE dma{};
    dma.num_words = count;
    dma.swap_mode = xenos::Endian::kNone;
    registers[reg::VGT_DMA_SIZE::register_index] = dma.value;
    registers[XE_GPU_REG_VGT_DMA_BASE] = physical;
  }

  uint32_t bytes() const { return uint32_t(source.size() * sizeof(uint16_t)); }

  PrimitiveProcessor::ProcessingResult Process() {
    PrimitiveProcessor::ProcessingResult result{};
    REQUIRE(processor->Process(result));
    CHECK(result.guest_draw_vertex_count == source.size());
    CHECK(result.host_draw_vertex_count == 3 * (source.size() - 2));
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kTriangleList);
    CHECK(shared.upload_calls == 0);
    return result;
  }

  std::vector<uint16_t> ExpectedIndices() const {
    std::vector<uint16_t> result;
    result.reserve(3 * (source.size() - 2));
    for (size_t i = 2; i < source.size(); ++i) {
      result.insert(result.end(), {source[i - 1], source[i], source[0]});
    }
    return result;
  }

  void WriteGuest(size_t index, uint16_t value) {
    REQUIRE(index < source.size());
    *memory.TranslateVirtual<volatile uint16_t*>(address + uint32_t(index * sizeof(uint16_t))) =
        value;
    source[index] = value;
  }

  void WriteAfterExplicitInvalidation(size_t index, uint16_t value) {
    REQUIRE(index < source.size());
    // Explicit invalidation cases isolate the callback predicate. Writing the
    // raw backing afterward avoids a second alias fault masking a missed range.
    *memory.TranslatePhysical<volatile uint16_t*>(physical + uint32_t(index * sizeof(uint16_t))) =
        value;
    source[index] = value;
  }
};

}  // namespace

TEST_CASE("Primitive cache default avoids protecting tiny conversions",
          "[graphics][primitive-cache]") {
  CHECK(REXCVAR_GET(primitive_processor_cache_min_indices) == 4096);
}

TEST_CASE("Primitive cache bypass sees mutations in small index buffers",
          "[graphics][primitive-cache]") {
  CacheFixture fixture(4);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  const auto first = fixture.Process();
  const std::vector<uint16_t> original{2, 3, 1, 3, 4, 1};
  CHECK(fixture.processor->Indices(first) == original);
  fixture.WriteGuest(0, 123);
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 2);
  CHECK(second.host_index_buffer_handle != first.host_index_buffer_handle);
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
  CHECK(fixture.processor->Indices(first) == original);
  const auto after = fixture.memory.GetAccessDiagnosticSnapshot();
  CHECK(after.watch_arm_calls == before.watch_arm_calls);
  CHECK(after.non_mmio_write_fault_count == before.non_mmio_write_fault_count);
}

TEST_CASE("Primitive cache threshold includes the exact guest index count",
          "[graphics][primitive-cache]") {
  uint32_t count = 4096;
  int32_t threshold = 4096;
  bool cached = true;
  SECTION("4095 indices remain uncached at 4096") {
    count = 4095;
    cached = false;
  }
  SECTION("4096 indices are cached at 4096") {}
  SECTION("Zero threshold caches small nonempty conversions") {
    count = 4;
    threshold = 0;
  }
  CacheFixture fixture(count, threshold);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  const auto first = fixture.Process();
  const auto first_diagnostics = fixture.memory.GetAccessDiagnosticSnapshot();
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == (cached ? 1 : 2));
  CHECK((first.host_index_buffer_handle == second.host_index_buffer_handle) == cached);
  CHECK(fixture.processor->Indices(first) == fixture.ExpectedIndices());
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
  CHECK((first_diagnostics.watch_arm_calls > before.watch_arm_calls) == cached);
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().watch_arm_calls ==
        first_diagnostics.watch_arm_calls);
}

TEST_CASE("Primitive cache invalidates exactly overlapping half-open ranges",
          "[graphics][primitive-cache]") {
  CacheFixture fixture;
  const auto first = fixture.Process();
  CHECK(fixture.Process().host_index_buffer_handle == first.host_index_buffer_handle);
  REQUIRE(fixture.processor->allocation_count() == 1);
  uint32_t start = fixture.physical;
  uint32_t length = fixture.bytes();
  size_t mutated_index = 0;
  bool overlaps = true;
  SECTION("Equal range") {}
  SECTION("Containing range") {
    start -= 2;
    length += 4;
  }
  SECTION("Partial overlap at the beginning") {
    start -= 2;
    length = 4;
  }
  SECTION("Partial overlap at the end") {
    start += length - 2;
    length = 4;
    mutated_index = fixture.source.size() - 1;
  }
  SECTION("Contained interior index") {
    start += 4;
    length = 2;
    mutated_index = 2;
  }
  SECTION("Immediately preceding range is disjoint") {
    start -= 2;
    length = 2;
    overlaps = false;
  }
  SECTION("Immediately following range is disjoint") {
    start += length;
    length = 2;
    overlaps = false;
  }
  fixture.processor->MemoryInvalidationCallback(start, length, true);
  if (overlaps) {
    fixture.WriteAfterExplicitInvalidation(mutated_index, 54321);
  }
  const auto after = fixture.Process();
  CHECK(fixture.processor->allocation_count() == (overlaps ? 2 : 1));
  CHECK((after.host_index_buffer_handle != first.host_index_buffer_handle) == overlaps);
  CHECK(fixture.processor->Indices(after) == fixture.ExpectedIndices());
}

TEST_CASE("Primitive cache non-exact invalidation removes a contained bucket entry",
          "[graphics][primitive-cache]") {
  CacheFixture fixture;
  const auto first = fixture.Process();
  // This models the broader bucket invalidation requested by the fault route.
  fixture.processor->MemoryInvalidationCallback(fixture.physical + 2, 2, false);
  fixture.WriteAfterExplicitInvalidation(1, 54321);
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 2);
  CHECK(second.host_index_buffer_handle != first.host_index_buffer_handle);
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
}

TEST_CASE("Primitive cache clear ends reuse without reusing old buffer storage",
          "[graphics][primitive-cache]") {
  CacheFixture fixture;
  const auto first = fixture.Process();
  const auto original = fixture.processor->Indices(first);
  CHECK(fixture.Process().host_index_buffer_handle == first.host_index_buffer_handle);
  fixture.processor->ClearCache();
  fixture.WriteAfterExplicitInvalidation(0, 54321);
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 2);
  CHECK(second.host_index_buffer_handle != first.host_index_buffer_handle);
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
  CHECK(fixture.processor->Indices(first) == original);
  CHECK(fixture.Process().host_index_buffer_handle == second.host_index_buffer_handle);
  CHECK(fixture.processor->allocation_count() == 2);
}

TEST_CASE("Primitive cache no-hit unwatch range excludes live entries in other buckets",
          "[graphics][primitive-cache]") {
  CacheFixture fixture;
  const auto first = fixture.Process();
  constexpr uint32_t bucket_size = 1u << 18;
  const uint32_t unrelated = (fixture.physical & ~(bucket_size - 1)) + bucket_size;
  const auto unwatch = fixture.processor->MemoryInvalidationCallback(unrelated, 2, false);
  const uint64_t unwatch_end = uint64_t(unwatch.first) + unwatch.second;
  CHECK(unwatch.second > 0);
  CHECK(unwatch.first <= unrelated);
  CHECK(unwatch_end >= uint64_t(unrelated) + 2);
  CHECK((unwatch_end <= fixture.physical ||
         uint64_t(unwatch.first) >= uint64_t(fixture.physical) + fixture.bytes()));
  CHECK(fixture.Process().host_index_buffer_handle == first.host_index_buffer_handle);
  CHECK(fixture.processor->allocation_count() == 1);
}

TEST_CASE("Primitive cache unrelated bucket fault preserves a live index write watch",
          "[graphics][primitive-cache][memory]") {
  // C allocates a 16 MiB guest page, so both 256 KiB buckets are committed
  // within this allocation. No synthetic host permission changes are needed.
  CacheFixture fixture(4096, 4096, 0xC0000000);
  fixture.Process();
  REQUIRE(fixture.processor->allocation_count() == 1);
  // The unused SharedMemory callback also returns a bounded range and could
  // mask an overbroad primitive-cache reply. Isolate the real primitive watch
  // owner; CPU-converted indices never call RequestRange/UploadRanges.
  fixture.shared.StopWatching();
  constexpr uint32_t bucket_size = 1u << 18;
  const uint32_t unrelated = (fixture.physical & ~(bucket_size - 1)) + bucket_size;
  const uint32_t unrelated_address = fixture.address + unrelated - fixture.physical;
  fixture.memory.EnablePhysicalMemoryAccessCallbacks(unrelated, 2, true, false);
  const auto before = fixture.memory.GetAccessDiagnosticSnapshot();
  *fixture.memory.TranslateVirtual<volatile uint16_t*>(unrelated_address) = 123;
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().non_mmio_write_fault_count ==
        before.non_mmio_write_fault_count + 1);
  size_t length = sizeof(uint16_t);
  memory::PageAccess access;
  REQUIRE(memory::QueryProtect(fixture.memory.TranslateVirtual(fixture.address), length, access));
  CHECK(access == memory::PageAccess::kReadOnly);
  fixture.WriteGuest(0, 54321);
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().non_mmio_write_fault_count ==
        before.non_mmio_write_fault_count + 2);
  const auto after = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 2);
  CHECK(fixture.processor->Indices(after) == fixture.ExpectedIndices());
  CHECK(fixture.memory.GetAccessDiagnosticSnapshot().watch_protect_failures ==
        before.watch_protect_failures);
}

TEST_CASE("Primitive cache follows real A C and E alias write faults and rearming",
          "[graphics][primitive-cache][memory]") {
  uint32_t alias = 0xE0000000;
  SECTION("A alias with 64 KiB guest pages") {
    alias = 0xA0000000;
  }
  SECTION("C alias with 16 MiB guest pages") {
    alias = 0xC0000000;
  }
  SECTION("E alias with 4 KiB guest pages") {}
  CacheFixture fixture(4096, 4096, alias);
  fixture.Process();
  REQUIRE(fixture.processor->allocation_count() == 1);
  const auto initial = fixture.memory.GetAccessDiagnosticSnapshot();
  for (uint32_t write = 1; write <= 2; ++write) {
    fixture.WriteGuest(0, uint16_t(50000 + write));
    const auto fault = fixture.memory.GetAccessDiagnosticSnapshot();
    CHECK(fault.non_mmio_write_fault_count == initial.non_mmio_write_fault_count + write);
    const auto result = fixture.Process();
    CHECK(fixture.processor->allocation_count() == 1 + write);
    CHECK(fixture.processor->Indices(result) == fixture.ExpectedIndices());
    CHECK(fixture.Process().host_index_buffer_handle == result.host_index_buffer_handle);
    CHECK(fixture.processor->allocation_count() == 1 + write);
    CHECK(fixture.memory.GetAccessDiagnosticSnapshot().watch_protect_failures ==
          initial.watch_protect_failures);
  }
}

TEST_CASE("Primitive cache does not retain a result invalidated during conversion",
          "[graphics][primitive-cache][memory]") {
  CacheFixture fixture;
  bool hook_called = false;
  fixture.processor->on_next_allocation = [&] {
    hook_called = true;
    REQUIRE(fixture.heap->TriggerCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                           fixture.address, fixture.bytes(), true, true));
    fixture.WriteGuest(0, 54321);
  };
  const auto first = fixture.Process();
  REQUIRE(hook_called);
  CHECK(fixture.processor->Indices(first) == fixture.ExpectedIndices());
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 2);
  CHECK(second.host_index_buffer_handle != first.host_index_buffer_handle);
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
  // The next, uninterrupted transaction should still be reusable.
  CHECK(fixture.Process().host_index_buffer_handle == second.host_index_buffer_handle);
  CHECK(fixture.processor->allocation_count() == 2);
}

TEST_CASE("Primitive cache unlinks both buckets of a crossing index range",
          "[graphics][primitive-cache]") {
  CacheFixture fixture(4096, 4096, 0xE0000000, true);
  REQUIRE((fixture.physical >> 18) != ((fixture.physical + fixture.bytes() - 1) >> 18));
  auto previous = fixture.Process();
  for (size_t index : {size_t(0), fixture.source.size() - 1, size_t(0)}) {
    const size_t allocations = fixture.processor->allocation_count();
    fixture.processor->MemoryInvalidationCallback(
        fixture.physical + uint32_t(index * sizeof(uint16_t)), 2, true);
    fixture.WriteAfterExplicitInvalidation(index, uint16_t(50000 + allocations));
    const auto next = fixture.Process();
    CHECK(fixture.processor->allocation_count() == allocations + 1);
    CHECK(next.host_index_buffer_handle != previous.host_index_buffer_handle);
    CHECK(fixture.processor->Indices(next) == fixture.ExpectedIndices());
    CHECK(fixture.Process().host_index_buffer_handle == next.host_index_buffer_handle);
    previous = next;
  }
}

TEST_CASE("Primitive cache retains active conversion after disjoint exact invalidation",
          "[graphics][primitive-cache]") {
  CacheFixture fixture;
  bool hook_called = false;
  fixture.processor->on_next_allocation = [&] {
    hook_called = true;
    fixture.processor->MemoryInvalidationCallback(fixture.physical + fixture.bytes(), 2, true);
  };
  const auto first = fixture.Process();
  REQUIRE(hook_called);
  const auto second = fixture.Process();
  CHECK(fixture.processor->allocation_count() == 1);
  CHECK(second.host_index_buffer_handle == first.host_index_buffer_handle);
  CHECK(fixture.processor->Indices(second) == fixture.ExpectedIndices());
}

}  // namespace rex::graphics::test
