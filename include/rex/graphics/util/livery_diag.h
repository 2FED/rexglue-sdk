#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace rex::graphics {

// Pure helpers of the d3d12_livery_diag diagnostics (MCLA KI-047: foreign
// content in a car livery that is composited once, resolved and read back by
// the CPU). Bounded, privacy-safe: callers log only frame and draw numbers,
// guest addresses, sizes, formats and hashes, never guest memory contents or
// host paths. No GPU is needed, so this is unit-tested directly.

// Allows at most `limit` events per window of `window_ms` milliseconds. A
// window starts at the first event after the previous window has ended (or
// when the clock goes backwards); events over the limit are counted as
// suppressed until TakeSuppressed() is called.
class WindowRateLimiter {
 public:
  explicit WindowRateLimiter(uint32_t limit, uint64_t window_ms = 60000)
      : limit_(limit), window_ms_(window_ms) {}

  bool Allow(uint64_t now_ms) {
    if (!started_ || now_ms < window_start_ms_ || now_ms - window_start_ms_ >= window_ms_) {
      started_ = true;
      window_start_ms_ = now_ms;
      used_ = 0;
    }
    if (used_ < limit_) {
      ++used_;
      return true;
    }
    ++suppressed_;
    return false;
  }

  uint64_t TakeSuppressed() {
    uint64_t suppressed = suppressed_;
    suppressed_ = 0;
    return suppressed;
  }

  uint32_t limit() const { return limit_; }

 private:
  uint32_t limit_;
  uint64_t window_ms_;
  bool started_ = false;
  uint64_t window_start_ms_ = 0;
  uint32_t used_ = 0;
  uint64_t suppressed_ = 0;
};

// Remembers the last frame each key was seen in. A key is "fresh" when it was
// never seen or not seen during the last `quiet_frames` frames, so a key that
// recurs every frame (a per-frame resolve, a pipeline skipped on every frame
// while it compiles) is reported once and then only after it went quiet. The
// table is bounded: when it is full it is cleared, which at worst reports
// each key once more.
class RecentKeyFilter {
 public:
  explicit RecentKeyFilter(uint64_t quiet_frames, size_t capacity = 4096)
      : quiet_frames_(quiet_frames), capacity_(capacity) {}

  bool IsFresh(uint64_t key, uint64_t frame) {
    auto it = last_frame_.find(key);
    if (it != last_frame_.end()) {
      bool fresh = frame < it->second || frame - it->second > quiet_frames_;
      it->second = frame;
      return fresh;
    }
    if (last_frame_.size() >= capacity_) {
      last_frame_.clear();
    }
    last_frame_.emplace(key, frame);
    return true;
  }

  size_t size() const { return last_frame_.size(); }

 private:
  uint64_t quiet_frames_;
  size_t capacity_;
  std::unordered_map<uint64_t, uint64_t> last_frame_;
};

// KI-047 H3: with resolution scaling, the readback downscales the scaled
// range that starts at copy_dest_base but writes the CPU copy to
// written_address (copy_dest_extent_start). When the two differ, the copy is
// shifted by their difference. Without scaling the copy reads from
// written_address itself, so the condition does not apply.
inline bool IsScaledReadbackShifted(bool scaled, uint32_t copy_dest_base,
                                    uint32_t written_address) {
  return scaled && copy_dest_base != written_address;
}

// What happened to the CPU copy of one read-back resolve.
enum class ReadbackCopyDecision : uint8_t {
  // Copied to guest memory during this resolve.
  kNow,
  // Not copied because of the "some" every-Nth rule (a later resolve of the
  // same range will copy).
  kDeferred,
  // Not copied for another reason (no buffer, unsupported format, early exit).
  kSkipped,
};

inline ReadbackCopyDecision ClassifyReadbackCopy(bool should_copy, bool copied) {
  if (copied) {
    return ReadbackCopyDecision::kNow;
  }
  return should_copy ? ReadbackCopyDecision::kSkipped : ReadbackCopyDecision::kDeferred;
}

inline const char* ReadbackCopyDecisionName(ReadbackCopyDecision decision) {
  switch (decision) {
    case ReadbackCopyDecision::kNow:
      return "now";
    case ReadbackCopyDecision::kDeferred:
      return "deferred";
    case ReadbackCopyDecision::kSkipped:
      return "skipped";
  }
  return "unknown";
}

// Per-minute counters, reported in one MCLA_LIVERY_DIAG minute line.
struct LiveryDiagCounters {
  uint64_t pso_pending_skips = 0;
  uint64_t pso_failed_skips = 0;
  uint64_t resolves = 0;
  uint64_t scaled_resolves = 0;
  uint64_t h3_resolves = 0;
  uint64_t copies_now = 0;
  uint64_t copies_deferred = 0;
  uint64_t copies_skipped = 0;
  uint64_t reads_previous_slot = 0;
  uint64_t reads_previous_slot_unfenced = 0;
  uint64_t resolves_after_skip = 0;

  void Count(ReadbackCopyDecision decision) {
    switch (decision) {
      case ReadbackCopyDecision::kNow:
        ++copies_now;
        break;
      case ReadbackCopyDecision::kDeferred:
        ++copies_deferred;
        break;
      case ReadbackCopyDecision::kSkipped:
        ++copies_skipped;
        break;
    }
  }
};

}  // namespace rex::graphics
