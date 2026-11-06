/**
 * Content/save log pacing and the transaction indicator (MCLA KI-065).
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/content_log.h>

namespace cl = rex::content_log;

TEST_CASE("Content log budget allows a burst of 16 then 1 line per second", "[content-log]") {
  cl::LineBudget budget;
  CHECK(budget.burst() == 16);
  CHECK(budget.per_second() == 1);
  for (int i = 0; i < 16; ++i) {
    CHECK(budget.Allow(1000));
  }
  CHECK_FALSE(budget.Allow(1000));
  CHECK_FALSE(budget.Allow(1500));
  CHECK(budget.TakeSuppressed() == 2);
  CHECK(budget.TakeSuppressed() == 0);
  // One second after the last refill reference: exactly one more line.
  CHECK(budget.Allow(2500));
  CHECK_FALSE(budget.Allow(2600));
  CHECK(budget.Allow(3500));
  CHECK_FALSE(budget.Allow(3500));
  // A long quiet period refills to the burst, not beyond it.
  int allowed = 0;
  for (int i = 0; i < 40; ++i) {
    allowed += budget.Allow(1000000) ? 1 : 0;
  }
  CHECK(allowed == 16);
}

TEST_CASE("Content log budget tolerates a clock that goes backwards", "[content-log]") {
  cl::LineBudget budget(2, 1);
  CHECK(budget.Allow(5000));
  CHECK(budget.Allow(5000));
  CHECK_FALSE(budget.Allow(4000));
  // The reference restarted at 4000: no refill for the backwards step.
  CHECK_FALSE(budget.Allow(4500));
  CHECK(budget.Allow(5000));
}

TEST_CASE("Content log kinds are paced independently and report suppression", "[content-log]") {
  cl::ResetForTesting();
  uint64_t suppressed = 99;
  for (int i = 0; i < 16; ++i) {
    REQUIRE(cl::AllowAt(cl::Kind::kCreate, 10000, &suppressed));
    CHECK(suppressed == 0);
  }
  CHECK_FALSE(cl::AllowAt(cl::Kind::kCreate, 10000, &suppressed));
  CHECK_FALSE(cl::AllowAt(cl::Kind::kCreate, 10100, &suppressed));
  CHECK_FALSE(cl::AllowAt(cl::Kind::kCreate, 10200, &suppressed));
  // Another kind still has its whole burst.
  CHECK(cl::AllowAt(cl::Kind::kTransaction, 10200, &suppressed));
  CHECK(suppressed == 0);
  // The next allowed kCreate line carries the three dropped lines.
  CHECK(cl::AllowAt(cl::Kind::kCreate, 11000, &suppressed));
  CHECK(suppressed == 3);
  CHECK_FALSE(cl::AllowAt(cl::Kind::kCount, 11000, &suppressed));
  cl::ResetForTesting();
}

TEST_CASE("Content log recognizes content device paths", "[content-log]") {
  CHECK(cl::IsContentDevicePath("\\Device\\Content\\1\\"));
  CHECK(cl::IsContentDevicePath("\\Device\\Content\\12\\mc4.sav"));
  CHECK(cl::IsContentDevicePath("\\DEVICE\\content\\3\\x"));
  CHECK_FALSE(cl::IsContentDevicePath("\\Device\\Harddisk0\\Partition1\\x"));
  CHECK_FALSE(cl::IsContentDevicePath("\\Device\\Cont"));
  CHECK_FALSE(cl::IsContentDevicePath("game:\\data"));
  CHECK_FALSE(cl::IsContentDevicePath(""));
}

TEST_CASE("Content log system error text is ASCII and never empty", "[content-log]") {
  CHECK(cl::SystemErrorText(0) == "none");
  const std::string text = cl::SystemErrorText(32);
  CHECK_FALSE(text.empty());
  CHECK(text.back() != '\n');
  CHECK(std::string(cl::KindName(cl::Kind::kFileWrite)) == "file-write");
  CHECK(std::string(cl::KindName(cl::Kind::kCount)) == "unknown");
}

TEST_CASE("Content transaction indicator counts begun transactions and busy scopes",
          "[content-log]") {
  const uint32_t base = cl::TransactionsActive();
  CHECK(rex_xam_content_transactions_active() == base);
  cl::NoteTransactionBegun();
  CHECK(cl::TransactionsActive() == base + 1);
  {
    cl::BusyScope busy;
    CHECK(cl::TransactionsActive() == base + 2);
    CHECK(rex_xam_content_transactions_active() == base + 2);
  }
  CHECK(cl::TransactionsActive() == base + 1);
  cl::NoteTransactionsEnded();
  CHECK(cl::TransactionsActive() == base);
  if (base == 0) {
    // Ending more than were begun saturates at zero.
    cl::NoteTransactionBegun();
    cl::NoteTransactionsEnded(5);
    CHECK(cl::TransactionsActive() == 0);
  }
}

TEST_CASE("Content trace de-duplicates polled calls by change and quiet gap", "[content-log]") {
  cl::ResetForTesting();
  uint64_t repeats = 99;
  CHECK(cl::AllowChangedAt("GetDeviceState:1", 0, 1000, 5000, &repeats));
  CHECK(repeats == 0);
  // Polled every frame with the same answer: swallowed.
  CHECK_FALSE(cl::AllowChangedAt("GetDeviceState:1", 0, 1016, 5000, &repeats));
  CHECK_FALSE(cl::AllowChangedAt("GetDeviceState:1", 0, 1033, 5000, &repeats));
  // A changed answer is logged with the swallowed count.
  CHECK(cl::AllowChangedAt("GetDeviceState:1", 1, 1050, 5000, &repeats));
  CHECK(repeats == 2);
  // Continuous polling never reaches the gap...
  for (uint64_t t = 1066; t < 20000; t += 16) {
    CHECK_FALSE(cl::AllowChangedAt("GetDeviceState:1", 1, t, 5000, &repeats));
  }
  // ...a call after a quiet gap is logged again.
  CHECK(cl::AllowChangedAt("GetDeviceState:1", 1, 30000, 5000, &repeats));
  CHECK(repeats > 1000);
  // Keys are independent; a backwards clock counts as a gap.
  CHECK(cl::AllowChangedAt("GetDeviceData:1", 7, 30000, 5000, &repeats));
  CHECK(cl::AllowChangedAt("GetDeviceData:1", 7, 100, 5000, &repeats));
  cl::ResetForTesting();
  CHECK(cl::AllowChangedAt("GetDeviceState:1", 1, 30001, 5000, &repeats));
}

TEST_CASE("Content trace kinds have names and the trace switch works", "[content-log]") {
  CHECK(std::string(cl::KindName(cl::Kind::kUnmount)) == "unmount");
  CHECK(std::string(cl::KindName(cl::Kind::kStaleIo)) == "stale-io");
  CHECK(std::string(cl::KindName(cl::Kind::kMoveAside)) == "move-aside");
  CHECK(std::string(cl::KindName(cl::Kind::kSaveOk)) == "save-ok");
  CHECK(std::string(cl::KindName(cl::Kind::kTraceDevice)) == "trace-device");
  CHECK(std::string(cl::KindName(cl::Kind::kTraceFile)) == "trace-file");
  cl::SetTraceEnabledForTesting(false);
  CHECK_FALSE(cl::TraceEnabled());
  cl::SetTraceEnabledForTesting(true);
  CHECK(cl::TraceEnabled());
  // Wine detection is stable within a process.
  CHECK(cl::RunningUnderWine() == cl::RunningUnderWine());
}
