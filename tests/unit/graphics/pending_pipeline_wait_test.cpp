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

}  // namespace rex::graphics::test
