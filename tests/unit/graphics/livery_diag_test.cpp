#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/livery_diag.h>

#include <string_view>

namespace rex::graphics::test {

TEST_CASE("WindowRateLimiter allows the limit per window and counts the rest",
          "[graphics][livery-diag]") {
  WindowRateLimiter limiter(3, 60000);
  CHECK(limiter.Allow(1000));
  CHECK(limiter.Allow(1001));
  CHECK(limiter.Allow(30000));
  CHECK_FALSE(limiter.Allow(30001));
  CHECK_FALSE(limiter.Allow(60999));
  CHECK(limiter.TakeSuppressed() == 2);
  CHECK(limiter.TakeSuppressed() == 0);
  // The window started at 1000, so 61000 opens a new one.
  CHECK(limiter.Allow(61000));
  CHECK(limiter.Allow(61001));
  CHECK(limiter.Allow(61002));
  CHECK_FALSE(limiter.Allow(61003));
  CHECK(limiter.TakeSuppressed() == 1);
}

TEST_CASE("WindowRateLimiter restarts the window when the clock goes backwards",
          "[graphics][livery-diag]") {
  WindowRateLimiter limiter(1, 60000);
  CHECK(limiter.Allow(5000));
  CHECK_FALSE(limiter.Allow(5001));
  CHECK(limiter.Allow(100));
  CHECK_FALSE(limiter.Allow(200));
  CHECK(limiter.TakeSuppressed() == 2);
}

TEST_CASE("WindowRateLimiter with limit 64 matches the diagnostic budget",
          "[graphics][livery-diag]") {
  WindowRateLimiter limiter(64);
  int allowed = 0;
  for (uint64_t t = 0; t < 1000; ++t) {
    allowed += limiter.Allow(t) ? 1 : 0;
  }
  CHECK(allowed == 64);
  CHECK(limiter.TakeSuppressed() == 936);
  CHECK(limiter.limit() == 64);
}

TEST_CASE("RecentKeyFilter reports new and quiet keys only", "[graphics][livery-diag]") {
  RecentKeyFilter filter(120);
  CHECK(filter.IsFresh(0xABC, 10));
  CHECK_FALSE(filter.IsFresh(0xABC, 11));
  CHECK_FALSE(filter.IsFresh(0xABC, 131));  // 120 frames after the last sighting
  CHECK(filter.IsFresh(0xABC, 252));        // 121 frames of quiet
  CHECK(filter.IsFresh(0xDEF, 252));
  CHECK_FALSE(filter.IsFresh(0xDEF, 253));
  CHECK(filter.IsFresh(0xDEF, 5));  // frame counter went backwards
}

TEST_CASE("RecentKeyFilter stays bounded", "[graphics][livery-diag]") {
  RecentKeyFilter filter(120, 4);
  for (uint64_t key = 0; key < 4; ++key) {
    CHECK(filter.IsFresh(key, 1));
  }
  CHECK(filter.size() == 4);
  CHECK(filter.IsFresh(99, 1));
  CHECK(filter.size() == 1);
  CHECK(filter.IsFresh(0, 2));  // forgotten when the table was cleared
}

TEST_CASE("IsScaledReadbackShifted is the H3 condition", "[graphics][livery-diag]") {
  CHECK_FALSE(IsScaledReadbackShifted(true, 0x1A000000, 0x1A000000));
  CHECK(IsScaledReadbackShifted(true, 0x1A000000, 0x1A010000));
  CHECK(IsScaledReadbackShifted(true, 0x1A010000, 0x1A000000));
  // Without scaling the copy reads from written_address itself.
  CHECK_FALSE(IsScaledReadbackShifted(false, 0x1A000000, 0x1A010000));
  CHECK_FALSE(IsScaledReadbackShifted(false, 0, 0));
}

TEST_CASE("ClassifyReadbackCopy separates now, deferred and skipped", "[graphics][livery-diag]") {
  CHECK(ClassifyReadbackCopy(true, true) == ReadbackCopyDecision::kNow);
  CHECK(ClassifyReadbackCopy(false, false) == ReadbackCopyDecision::kDeferred);
  CHECK(ClassifyReadbackCopy(true, false) == ReadbackCopyDecision::kSkipped);
  CHECK(std::string_view(ReadbackCopyDecisionName(ReadbackCopyDecision::kNow)) == "now");
  CHECK(std::string_view(ReadbackCopyDecisionName(ReadbackCopyDecision::kDeferred)) ==
        "deferred");
  CHECK(std::string_view(ReadbackCopyDecisionName(ReadbackCopyDecision::kSkipped)) == "skipped");
}

TEST_CASE("LiveryDiagCounters counts copy decisions", "[graphics][livery-diag]") {
  LiveryDiagCounters counters;
  counters.Count(ReadbackCopyDecision::kNow);
  counters.Count(ReadbackCopyDecision::kDeferred);
  counters.Count(ReadbackCopyDecision::kDeferred);
  counters.Count(ReadbackCopyDecision::kSkipped);
  CHECK(counters.copies_now == 1);
  CHECK(counters.copies_deferred == 2);
  CHECK(counters.copies_skipped == 1);
  counters = {};
  CHECK(counters.copies_deferred == 0);
}

}  // namespace rex::graphics::test
