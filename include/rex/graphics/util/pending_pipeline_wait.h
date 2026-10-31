#pragma once

#include <cstdint>

namespace rex::graphics {

// Bounded wait for an asynchronously created pipeline, and the per-episode
// bookkeeping of the d3d12_async_pipeline_wait mode. Pure logic: the caller
// supplies the completion check, a "help" step (create one queued pipeline on
// the calling thread; false when nothing was queued), an idle step and a
// monotonic millisecond clock, so this is unit-tested without a GPU.

enum class PendingWaitResult { kCompleted, kTimedOut };

template <typename IsComplete, typename HelpOne, typename Idle, typename NowMs>
PendingWaitResult AwaitPending(IsComplete&& is_complete, HelpOne&& help_one, Idle&& idle,
                               NowMs&& now_ms, uint64_t timeout_ms, uint64_t* elapsed_ms_out) {
  const uint64_t start = now_ms();
  PendingWaitResult result = PendingWaitResult::kCompleted;
  while (!is_complete()) {
    if (now_ms() - start >= timeout_ms) {
      result = PendingWaitResult::kTimedOut;
      break;
    }
    if (!help_one()) {
      idle();
    }
  }
  if (elapsed_ms_out) {
    *elapsed_ms_out = now_ms() - start;
  }
  return result;
}

// One episode is the span during which the wait mode is observed on. Waits
// are bounded per draw (kPerWaitTimeoutMs) and per episode (kEpisodeBudgetMs);
// after the budget is spent the remaining draws of the episode are skipped as
// in the default asynchronous mode.
class PendingWaitEpisode {
 public:
  static constexpr uint64_t kPerWaitTimeoutMs = 2000;
  static constexpr uint64_t kEpisodeBudgetMs = 8000;
  static constexpr uint64_t kSlowWaitMs = 500;

  struct Stats {
    uint64_t waits = 0;
    uint64_t timeouts = 0;
    uint64_t failed = 0;
    uint64_t budget_skips = 0;
    uint64_t total_ms = 0;
    uint64_t max_ms = 0;
  };

  // Records the current mode. Returns true when an episode with at least one
  // wait or skip has just ended (the caller logs Stats() once, then the next
  // episode starts from zero on the next enabled observation).
  bool Observe(bool enabled) {
    if (enabled == enabled_) {
      return false;
    }
    enabled_ = enabled;
    if (enabled) {
      stats_ = {};
      slow_logged_ = false;
      return false;
    }
    return stats_.waits != 0 || stats_.budget_skips != 0;
  }

  bool enabled() const { return enabled_; }

  // Timeout for the next wait, 0 when the episode budget is spent (the draw
  // is skipped and counted as a budget skip).
  uint64_t NextTimeoutMs() {
    if (stats_.total_ms >= kEpisodeBudgetMs) {
      ++stats_.budget_skips;
      return 0;
    }
    uint64_t left = kEpisodeBudgetMs - stats_.total_ms;
    return left < kPerWaitTimeoutMs ? left : kPerWaitTimeoutMs;
  }

  // Records one finished wait. Returns true when this wait is the first slow
  // one (>= kSlowWaitMs) or the first timeout of the episode, to be logged.
  bool Record(uint64_t elapsed_ms, PendingWaitResult result, bool created) {
    ++stats_.waits;
    stats_.total_ms += elapsed_ms;
    if (elapsed_ms > stats_.max_ms) {
      stats_.max_ms = elapsed_ms;
    }
    bool timed_out = result == PendingWaitResult::kTimedOut;
    if (timed_out) {
      ++stats_.timeouts;
    } else if (!created) {
      ++stats_.failed;
    }
    if (!slow_logged_ && (timed_out || elapsed_ms >= kSlowWaitMs)) {
      slow_logged_ = true;
      return true;
    }
    return false;
  }

  const Stats& stats() const { return stats_; }

 private:
  bool enabled_ = false;
  bool slow_logged_ = false;
  Stats stats_;
};

// Bookkeeping of the d3d12_async_pipeline_wait_always mode: every draw whose
// pipeline is still being created waits, bounded per draw (kPerWaitTimeoutMs)
// and per one-minute window (kMinuteBudgetMs). A window starts at the first
// enabled observation and lasts kWindowMs; once its budget is spent, draws are
// skipped as in the default asynchronous mode until the next window starts.
class PendingWaitMinuteBudget {
 public:
  using Stats = PendingWaitEpisode::Stats;
  static constexpr uint64_t kPerWaitTimeoutMs = 2000;
  static constexpr uint64_t kMinuteBudgetMs = 15000;
  static constexpr uint64_t kWindowMs = 60000;
  static constexpr uint64_t kSlowWaitMs = PendingWaitEpisode::kSlowWaitMs;

  // Records the current mode at now_ms. Returns true when a window with at
  // least one wait or skip has just ended, either because kWindowMs elapsed
  // or because the mode was turned off; the caller then logs FinishedStats()
  // once. A rolled-over window starts at now_ms.
  bool Observe(bool enabled, uint64_t now_ms) {
    if (!enabled) {
      if (!enabled_) {
        return false;
      }
      enabled_ = false;
      return FinishWindow(now_ms);
    }
    if (!enabled_) {
      enabled_ = true;
      StartWindow(now_ms);
      return false;
    }
    if (now_ms - window_start_ms_ < kWindowMs) {
      return false;
    }
    bool report = FinishWindow(now_ms);
    StartWindow(now_ms);
    return report;
  }

  bool enabled() const { return enabled_; }

  // Timeout for the next wait, 0 when the window budget is spent (the draw is
  // skipped and counted as a budget skip).
  uint64_t NextTimeoutMs() {
    if (stats_.total_ms >= kMinuteBudgetMs) {
      ++stats_.budget_skips;
      return 0;
    }
    uint64_t left = kMinuteBudgetMs - stats_.total_ms;
    return left < kPerWaitTimeoutMs ? left : kPerWaitTimeoutMs;
  }

  // True once per window, at the first budget skip, for one warning.
  bool TakeExhaustedNotice() {
    if (stats_.budget_skips == 0 || exhausted_noticed_) {
      return false;
    }
    exhausted_noticed_ = true;
    return true;
  }

  // Records one finished wait. Returns true when this wait is the first slow
  // one (>= kSlowWaitMs) or the first timeout of the window, to be logged.
  bool Record(uint64_t elapsed_ms, PendingWaitResult result, bool created) {
    ++stats_.waits;
    stats_.total_ms += elapsed_ms;
    if (elapsed_ms > stats_.max_ms) {
      stats_.max_ms = elapsed_ms;
    }
    bool timed_out = result == PendingWaitResult::kTimedOut;
    if (timed_out) {
      ++stats_.timeouts;
    } else if (!created) {
      ++stats_.failed;
    }
    if (!slow_logged_ && (timed_out || elapsed_ms >= kSlowWaitMs)) {
      slow_logged_ = true;
      return true;
    }
    return false;
  }

  // The window in progress.
  const Stats& stats() const { return stats_; }
  // The last window that ended (valid after Observe returned true).
  const Stats& finished_stats() const { return finished_; }
  uint64_t finished_duration_ms() const { return finished_duration_ms_; }

 private:
  void StartWindow(uint64_t now_ms) {
    window_start_ms_ = now_ms;
    stats_ = {};
    slow_logged_ = false;
    exhausted_noticed_ = false;
  }
  bool FinishWindow(uint64_t now_ms) {
    finished_ = stats_;
    finished_duration_ms_ = now_ms - window_start_ms_;
    stats_ = {};
    return finished_.waits != 0 || finished_.budget_skips != 0;
  }

  bool enabled_ = false;
  bool slow_logged_ = false;
  bool exhausted_noticed_ = false;
  uint64_t window_start_ms_ = 0;
  Stats stats_;
  Stats finished_;
  uint64_t finished_duration_ms_ = 0;
};

// Which budget governs a draw whose pipeline is pending. The capture window
// (d3d12_async_pipeline_wait) takes precedence; the always-on minute budget
// applies only while the window is off.
enum class PendingWaitMode { kNone, kWindow, kAlways };

inline PendingWaitMode SelectPendingWaitMode(bool window_on, bool always_on) {
  if (window_on) {
    return PendingWaitMode::kWindow;
  }
  return always_on ? PendingWaitMode::kAlways : PendingWaitMode::kNone;
}

}  // namespace rex::graphics
