#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/pending_pipeline_wait.h>

namespace rex::graphics::test {

TEST_CASE("AwaitPending returns at once for a completed pipeline", "[graphics][pipeline-wait]") {
  uint64_t clock = 100, elapsed = 99;
  int helps = 0, idles = 0;
  auto result = AwaitPending([] { return true; }, [&] { return ++helps, false; },
                             [&] { ++idles; }, [&] { return clock; }, 2000, &elapsed);
  CHECK(result == PendingWaitResult::kCompleted);
  CHECK(elapsed == 0);
  CHECK(helps == 0);
  CHECK(idles == 0);
}

TEST_CASE("AwaitPending helps with queued work, then idles until completion",
          "[graphics][pipeline-wait]") {
  uint64_t clock = 0, elapsed = 0;
  int queued = 3, idles = 0;
  bool complete = false;
  auto result = AwaitPending(
      [&] { return complete; },
      [&] {
        if (!queued) return false;
        --queued;
        clock += 10;
        return true;
      },
      [&] {
        // A worker thread finishes the target after two idle steps.
        if (++idles == 2) complete = true;
        clock += 1;
      },
      [&] { return clock; }, 2000, &elapsed);
  CHECK(result == PendingWaitResult::kCompleted);
  CHECK(queued == 0);
  CHECK(idles == 2);
  CHECK(elapsed == 32);
}

TEST_CASE("AwaitPending times out", "[graphics][pipeline-wait]") {
  uint64_t clock = 0, elapsed = 0;
  auto result = AwaitPending([] { return false; }, [] { return false; }, [&] { clock += 7; },
                             [&] { return clock; }, 100, &elapsed);
  CHECK(result == PendingWaitResult::kTimedOut);
  CHECK(elapsed >= 100);
  CHECK(elapsed < 107);
}

TEST_CASE("Wait episodes start on enable and report only when something happened",
          "[graphics][pipeline-wait]") {
  PendingWaitEpisode episode;
  CHECK_FALSE(episode.enabled());
  CHECK_FALSE(episode.Observe(false));
  CHECK_FALSE(episode.Observe(true));
  CHECK(episode.enabled());
  CHECK_FALSE(episode.Observe(true));
  // Nothing waited: no report at the end.
  CHECK_FALSE(episode.Observe(false));

  CHECK_FALSE(episode.Observe(true));
  CHECK(episode.NextTimeoutMs() == PendingWaitEpisode::kPerWaitTimeoutMs);
  CHECK_FALSE(episode.Record(40, PendingWaitResult::kCompleted, true));
  CHECK(episode.Record(600, PendingWaitResult::kCompleted, true));  // first slow wait
  CHECK_FALSE(episode.Record(700, PendingWaitResult::kCompleted, true));
  CHECK_FALSE(episode.Record(2000, PendingWaitResult::kTimedOut, false));
  CHECK_FALSE(episode.Record(5, PendingWaitResult::kCompleted, false));
  CHECK(episode.Observe(false));
  const auto& stats = episode.stats();
  CHECK(stats.waits == 5);
  CHECK(stats.total_ms == 3345);
  CHECK(stats.max_ms == 2000);
  CHECK(stats.timeouts == 1);
  CHECK(stats.failed == 1);
  CHECK(stats.budget_skips == 0);

  // A new episode starts from zero and may log a slow wait again.
  CHECK_FALSE(episode.Observe(true));
  CHECK(episode.stats().waits == 0);
  CHECK(episode.Record(2000, PendingWaitResult::kTimedOut, false));
}

TEST_CASE("Wait episodes are bounded by a total budget", "[graphics][pipeline-wait]") {
  PendingWaitEpisode episode;
  episode.Observe(true);
  for (int i = 0; i < 3; ++i) {
    CHECK(episode.NextTimeoutMs() == PendingWaitEpisode::kPerWaitTimeoutMs);
    episode.Record(PendingWaitEpisode::kPerWaitTimeoutMs, PendingWaitResult::kTimedOut, false);
  }
  CHECK(episode.NextTimeoutMs() ==
        PendingWaitEpisode::kEpisodeBudgetMs - 3 * PendingWaitEpisode::kPerWaitTimeoutMs);
  episode.Record(PendingWaitEpisode::kEpisodeBudgetMs - 3 * PendingWaitEpisode::kPerWaitTimeoutMs,
                 PendingWaitResult::kTimedOut, false);
  CHECK(episode.NextTimeoutMs() == 0);
  CHECK(episode.NextTimeoutMs() == 0);
  CHECK(episode.stats().budget_skips == 2);
  CHECK(episode.Observe(false));
}

TEST_CASE("Minute budget windows start on enable and report only with activity",
          "[graphics][pipeline-wait]") {
  using B = PendingWaitMinuteBudget;
  B budget;
  CHECK_FALSE(budget.enabled());
  CHECK_FALSE(budget.Observe(false, 0));
  CHECK_FALSE(budget.Observe(true, 1000));
  CHECK(budget.enabled());
  // An idle minute rolls over without a report.
  CHECK_FALSE(budget.Observe(true, 1000 + B::kWindowMs - 1));
  CHECK_FALSE(budget.Observe(true, 1000 + B::kWindowMs));
  CHECK(budget.enabled());

  CHECK(budget.NextTimeoutMs() == B::kPerWaitTimeoutMs);
  CHECK_FALSE(budget.Record(30, PendingWaitResult::kCompleted, true));
  CHECK(budget.Record(900, PendingWaitResult::kCompleted, true));  // first slow wait
  CHECK_FALSE(budget.Record(2000, PendingWaitResult::kTimedOut, false));
  CHECK_FALSE(budget.Observe(true, 1000 + 2 * B::kWindowMs - 1));
  CHECK(budget.Observe(true, 1000 + 2 * B::kWindowMs + 5));
  const auto& done = budget.finished_stats();
  CHECK(done.waits == 3);
  CHECK(done.total_ms == 2930);
  CHECK(done.max_ms == 2000);
  CHECK(done.timeouts == 1);
  CHECK(done.failed == 0);
  CHECK(budget.finished_duration_ms() == B::kWindowMs + 5);
  // The new window starts from zero and may log a slow wait again.
  CHECK(budget.stats().waits == 0);
  CHECK(budget.Record(600, PendingWaitResult::kCompleted, true));
}

TEST_CASE("Minute budget exhausts, warns once and resets in the next minute",
          "[graphics][pipeline-wait]") {
  using B = PendingWaitMinuteBudget;
  B budget;
  budget.Observe(true, 0);
  const uint64_t full_waits = B::kMinuteBudgetMs / B::kPerWaitTimeoutMs;  // 7
  for (uint64_t i = 0; i < full_waits; ++i) {
    CHECK(budget.NextTimeoutMs() == B::kPerWaitTimeoutMs);
    budget.Record(B::kPerWaitTimeoutMs, PendingWaitResult::kTimedOut, false);
  }
  CHECK(budget.NextTimeoutMs() == B::kMinuteBudgetMs - full_waits * B::kPerWaitTimeoutMs);
  CHECK_FALSE(budget.TakeExhaustedNotice());
  budget.Record(B::kMinuteBudgetMs - full_waits * B::kPerWaitTimeoutMs,
                PendingWaitResult::kTimedOut, false);
  CHECK(budget.stats().total_ms == B::kMinuteBudgetMs);
  CHECK(budget.NextTimeoutMs() == 0);
  CHECK(budget.TakeExhaustedNotice());
  CHECK(budget.NextTimeoutMs() == 0);
  CHECK(budget.NextTimeoutMs() == 0);
  CHECK_FALSE(budget.TakeExhaustedNotice());
  CHECK(budget.stats().budget_skips == 3);

  // Still exhausted until the window ends.
  CHECK_FALSE(budget.Observe(true, B::kWindowMs - 1));
  CHECK(budget.NextTimeoutMs() == 0);
  CHECK(budget.Observe(true, B::kWindowMs));
  CHECK(budget.finished_stats().budget_skips == 4);
  CHECK(budget.finished_stats().total_ms == B::kMinuteBudgetMs);
  // Next minute: full budget again and a new warning when it is spent.
  CHECK(budget.NextTimeoutMs() == B::kPerWaitTimeoutMs);
  CHECK_FALSE(budget.TakeExhaustedNotice());
  budget.Record(B::kMinuteBudgetMs, PendingWaitResult::kCompleted, true);
  CHECK(budget.NextTimeoutMs() == 0);
  CHECK(budget.TakeExhaustedNotice());
}

TEST_CASE("Minute budget reports a partial window when turned off",
          "[graphics][pipeline-wait]") {
  using B = PendingWaitMinuteBudget;
  B budget;
  budget.Observe(true, 500);
  budget.Record(120, PendingWaitResult::kCompleted, true);
  CHECK(budget.Observe(false, 20500));
  CHECK_FALSE(budget.enabled());
  CHECK(budget.finished_stats().waits == 1);
  CHECK(budget.finished_duration_ms() == 20000);
  CHECK_FALSE(budget.Observe(false, 30000));
  // Re-enabling starts a fresh window with a fresh budget.
  CHECK_FALSE(budget.Observe(true, 40000));
  CHECK(budget.stats().waits == 0);
  CHECK(budget.NextTimeoutMs() == B::kPerWaitTimeoutMs);
  // Turned off with nothing waited: no report.
  PendingWaitMinuteBudget idle;
  idle.Observe(true, 0);
  CHECK_FALSE(idle.Observe(false, 100));
}

TEST_CASE("Capture window and always-on wait compose with separate budgets",
          "[graphics][pipeline-wait]") {
  CHECK(SelectPendingWaitMode(false, false) == PendingWaitMode::kNone);
  CHECK(SelectPendingWaitMode(true, false) == PendingWaitMode::kWindow);
  CHECK(SelectPendingWaitMode(false, true) == PendingWaitMode::kAlways);
  CHECK(SelectPendingWaitMode(true, true) == PendingWaitMode::kWindow);

  // Spending the always-on minute budget does not shorten a capture window,
  // and waits inside the window do not consume the minute budget.
  PendingWaitEpisode episode;
  PendingWaitMinuteBudget minute;
  minute.Observe(true, 0);
  minute.Record(PendingWaitMinuteBudget::kMinuteBudgetMs, PendingWaitResult::kCompleted, true);
  CHECK(minute.NextTimeoutMs() == 0);
  episode.Observe(true);
  CHECK(episode.NextTimeoutMs() == PendingWaitEpisode::kPerWaitTimeoutMs);
  episode.Record(PendingWaitEpisode::kEpisodeBudgetMs, PendingWaitResult::kCompleted, true);
  CHECK(episode.NextTimeoutMs() == 0);
  CHECK(minute.stats().total_ms == PendingWaitMinuteBudget::kMinuteBudgetMs);
  CHECK(episode.Observe(false));
  // A new minute restores the always-on budget regardless of the window.
  CHECK(minute.Observe(true, PendingWaitMinuteBudget::kWindowMs));
  CHECK(minute.NextTimeoutMs() == PendingWaitMinuteBudget::kPerWaitTimeoutMs);
}

}  // namespace rex::graphics::test
