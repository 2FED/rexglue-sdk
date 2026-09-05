#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/command_processor.h>

namespace rex::graphics::test {
namespace {

// Expose the stack-only helper without constructing a graphics system, backend,
// native window or guest runtime. Production command flow remains untouched.
class DiagnosticScopeAccess : public CommandProcessor {
 public:
  using CommandProcessor::ScopedPerformanceDiagnostic;
};

using DiagnosticScope = DiagnosticScopeAccess::ScopedPerformanceDiagnostic;
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
}

}  // namespace rex::graphics::test
