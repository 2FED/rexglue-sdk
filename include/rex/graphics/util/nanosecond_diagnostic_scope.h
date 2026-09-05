#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rex::graphics {

// Accumulate before converting units. Per-call microsecond truncation loses
// most time in millions of short descriptor updates on fast native drivers.
// Export microseconds by dividing the cumulative ns counter in the snapshot.
class ScopedNanosecondDiagnostic {
 public:
  ScopedNanosecondDiagnostic(std::atomic<uint64_t>& completed_count,
                            std::atomic<uint64_t>& elapsed_ns) noexcept
      : count_(completed_count), elapsed_ns_(elapsed_ns),
        start_(std::chrono::steady_clock::now()) {}
  ~ScopedNanosecondDiagnostic() noexcept {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - start_).count();
    elapsed_ns_.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
  }
  ScopedNanosecondDiagnostic(const ScopedNanosecondDiagnostic&) = delete;
  ScopedNanosecondDiagnostic& operator=(const ScopedNanosecondDiagnostic&) = delete;

 private:
  std::atomic<uint64_t>& count_;
  std::atomic<uint64_t>& elapsed_ns_;
  const std::chrono::steady_clock::time_point start_;
};

}  // namespace rex::graphics
