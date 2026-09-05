#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/util/nanosecond_diagnostic_scope.h>

namespace rex::graphics::test {
namespace {

// Expose the stack-only helper without constructing a graphics system, backend,
// native window or guest runtime. Production command flow remains untouched.
class DiagnosticScopeAccess : public CommandProcessor {
 public:
  using CommandProcessor::ScopedPerformanceDiagnostic;
};

using DiagnosticScope = DiagnosticScopeAccess::ScopedPerformanceDiagnostic;
static_assert(!std::is_copy_constructible_v<ScopedNanosecondDiagnostic>);
static_assert(!std::is_move_constructible_v<ScopedNanosecondDiagnostic>);
static_assert(!std::is_copy_constructible_v<DiagnosticScope>);
static_assert(!std::is_move_constructible_v<DiagnosticScope>);

}  // namespace

TEST_CASE("Graphics diagnostic scopes record completed calls on every exit",
          "[graphics][performance-diagnostic]") {
  std::atomic<uint64_t> count{4};
  std::atomic<uint64_t> elapsed_us{17};
  {
    const DiagnosticScope scope(count, elapsed_us);
    CHECK(count.load(std::memory_order_relaxed) == 4);
    CHECK(elapsed_us.load(std::memory_order_relaxed) == 17);
  }
  CHECK(count.load(std::memory_order_relaxed) == 5);
  CHECK(elapsed_us.load(std::memory_order_relaxed) >= 17);

  const auto early_return = [&]() {
    const DiagnosticScope scope(count, elapsed_us);
    return false;
  };
  const auto before_return = elapsed_us.load(std::memory_order_relaxed);
  CHECK_FALSE(early_return());
  CHECK(count.load(std::memory_order_relaxed) == 6);
  CHECK(elapsed_us.load(std::memory_order_relaxed) >= before_return);

  const auto throw_from_scope = [&]() {
    const DiagnosticScope scope(count, elapsed_us);
    throw std::runtime_error("diagnostic fixture");
  };
  const auto before_throw = elapsed_us.load(std::memory_order_relaxed);
  CHECK_THROWS_AS(throw_from_scope(), std::runtime_error);
  CHECK(count.load(std::memory_order_relaxed) == 7);
  CHECK(elapsed_us.load(std::memory_order_relaxed) >= before_throw);
}

TEST_CASE("Nested graphics diagnostic scopes keep independent inclusive totals",
          "[graphics][performance-diagnostic]") {
  std::atomic<uint64_t> primary_count{0};
  std::atomic<uint64_t> primary_us{0};
  std::atomic<uint64_t> packet_count{0};
  std::atomic<uint64_t> packet_us{0};
  {
    const DiagnosticScope primary(primary_count, primary_us);
    for (unsigned i = 0; i < 2; ++i) {
      const DiagnosticScope packet(packet_count, packet_us);
    }
    CHECK(primary_count.load(std::memory_order_relaxed) == 0);
    CHECK(packet_count.load(std::memory_order_relaxed) == 2);
  }
  CHECK(primary_count.load(std::memory_order_relaxed) == 1);
  CHECK(packet_count.load(std::memory_order_relaxed) == 2);
  CHECK(primary_us.load(std::memory_order_relaxed) >= packet_us.load(std::memory_order_relaxed));
}

TEST_CASE("Graphics command diagnostic snapshot defaults do not imply executed work",
          "[graphics][performance-diagnostic]") {
  const GraphicsPerformanceDiagnosticSnapshot snapshot;
  CHECK(snapshot.backend == GraphicsPerformanceDiagnosticSnapshot::Backend::kUnknown);
  CHECK(snapshot.command_primary_buffer_count == 0);
  CHECK(snapshot.command_primary_buffer_us == 0);
  CHECK(snapshot.command_draw_packet_count == 0);
  CHECK(snapshot.command_draw_packet_us == 0);
  CHECK(snapshot.command_swap_packet_count == 0);
  CHECK(snapshot.command_swap_packet_us == 0);
  CHECK(snapshot.command_interrupt_packet_count == 0);
  CHECK(snapshot.command_interrupt_packet_us == 0);
  CHECK(snapshot.vulkan_fence_call_count == 0);
  CHECK(snapshot.vulkan_fence_call_us == 0);
  CHECK(snapshot.vulkan_resolve_count == 0);
  CHECK(snapshot.vulkan_resolve_us == 0);
  CHECK(snapshot.vulkan_resolve_readback_copy_count == 0);
  CHECK(snapshot.vulkan_resolve_readback_bytes == 0);
  CHECK(snapshot.vulkan_resolve_readback_copy_us == 0);
  CHECK(snapshot.vulkan_resolve_readback_wait_count == 0);
  CHECK(snapshot.vulkan_resolve_readback_wait_us == 0);
  CHECK(snapshot.vulkan_binding_update_count == 0);
  CHECK(snapshot.vulkan_binding_update_us == 0);
  CHECK(snapshot.vulkan_texture_descriptor_reuse_count == 0);
  CHECK(snapshot.vulkan_texture_descriptor_write_count == 0);
  CHECK(snapshot.vulkan_descriptor_update_count == 0);
  CHECK(snapshot.vulkan_descriptor_update_us == 0);
}

TEST_CASE("Short graphics diagnostic scopes retain nanoseconds until snapshot conversion",
          "[graphics][performance-diagnostic]") {
  std::atomic<uint64_t> count{0}, elapsed_ns{999}, outer_count{0}, outer_ns{0};
  {
    const ScopedNanosecondDiagnostic outer(outer_count, outer_ns);
    for (unsigned i = 0; i < 1000; ++i) {
      const ScopedNanosecondDiagnostic inner(count, elapsed_ns);
    }
  }
  CHECK(count.load() == 1000);
  CHECK(outer_count.load() == 1);
  CHECK(elapsed_ns.load() >= 999);
  CHECK(elapsed_ns.load() - 999 <= outer_ns.load());
  CHECK(outer_ns.load() > 0);
  // Conversion is once per cumulative snapshot, preserving sub-us remainder.
  CHECK(elapsed_ns.load() / 1000 <= (outer_ns.load() + 999) / 1000);
  // An independently timed inner workload gives a deterministic unit check:
  // microseconds mislabeled as ns cannot cover this enclosed ns interval.
  std::atomic<uint64_t> work{0}, measured_count{0}, measured_ns{0};
  uint64_t enclosed_ns;
  {
    const ScopedNanosecondDiagnostic scope(measured_count, measured_ns);
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 10000; ++i) work.fetch_add(1, std::memory_order_relaxed);
    enclosed_ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count());
  }
  CHECK(enclosed_ns > 0);
  CHECK(measured_ns.load() >= enclosed_ns);
  CHECK(measured_count.load() == 1);
}

TEST_CASE("Nanosecond diagnostics complete on early return and unwind",
          "[graphics][performance-diagnostic]") {
  std::atomic<uint64_t> count{0}, elapsed_ns{123};
  const auto fail = [&]() {
    const ScopedNanosecondDiagnostic scope(count, elapsed_ns);
    return false;
  };
  CHECK_FALSE(fail());
  CHECK(count.load() == 1);
  const auto raise = [&]() {
    const ScopedNanosecondDiagnostic scope(count, elapsed_ns);
    throw std::runtime_error("fixture");
  };
  CHECK_THROWS_AS(raise(), std::runtime_error);
  CHECK(count.load() == 2);
  CHECK(elapsed_ns.load() >= 123);
}

}  // namespace rex::graphics::test
