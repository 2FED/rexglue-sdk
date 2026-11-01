#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/replay_wake.h>

#include <string>

namespace rex::graphics::test {

using Decision = ReplayWakeBatcher::Decision;

TEST_CASE("Replay wake: spin limits", "[graphics][replay-wake]") {
  CHECK(ReplaySpinLimit(false) == 4096);
  CHECK(ReplaySpinLimit(true) == 128);
}

TEST_CASE("Replay wake: default policy wakes a sleeping worker at every publish",
          "[graphics][replay-wake]") {
  ReplayWakeBatcher b;
  b.Begin(false);
  CHECK(b.OnPublish(32, false, false) == Decision::kNone);
  CHECK(b.OnPublish(32, true, false) == Decision::kWake);
  CHECK(b.OnPublish(32, true, false) == Decision::kWake);
  b.NoteSeal();
  CHECK(b.OnPublish(32, true, false) == Decision::kWake);
  CHECK(b.pending() == 0);
}

TEST_CASE("Replay wake: batched policy wakes every 512 commands", "[graphics][replay-wake]") {
  ReplayWakeBatcher b;
  b.Begin(true);
  // 15 publishes of 32 commands to a sleeping worker are deferred, the 16th
  // (512 commands) wakes it.
  for (int i = 0; i < 15; ++i) {
    CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  }
  CHECK(b.pending() == 480);
  CHECK(b.OnPublish(32, true, false) == Decision::kWake);
  CHECK(b.pending() == 0);
  // A short publish does not reach the batch.
  CHECK(b.OnPublish(7, true, false) == Decision::kDeferred);
  CHECK(b.pending() == 7);
}

TEST_CASE("Replay wake: an awake worker resets the batch", "[graphics][replay-wake]") {
  ReplayWakeBatcher b;
  b.Begin(true);
  for (int i = 0; i < 10; ++i) {
    CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  }
  // The worker woke by itself (or was woken): it sees everything published.
  CHECK(b.OnPublish(32, false, false) == Decision::kNone);
  CHECK(b.pending() == 0);
  for (int i = 0; i < 15; ++i) {
    CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  }
  CHECK(b.OnPublish(32, true, false) == Decision::kWake);
}

TEST_CASE("Replay wake: block seals and finish force a wake", "[graphics][replay-wake]") {
  ReplayWakeBatcher b;
  b.Begin(true);
  CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  b.NoteSeal();
  CHECK(b.OnPublish(32, true, false) == Decision::kForced);
  CHECK(b.pending() == 0);
  // The seal is consumed by one wake.
  CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  // Stream start and FinishStreaming publish with force.
  CHECK(b.OnPublish(3, true, true) == Decision::kForced);
  // A seal seen while the worker is awake needs no wake later.
  b.NoteSeal();
  CHECK(b.OnPublish(32, false, false) == Decision::kNone);
  CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
  // Forced publishes to an awake worker do nothing.
  CHECK(b.OnPublish(32, false, true) == Decision::kNone);
  // Begin resets everything.
  b.NoteSeal();
  b.Begin(true);
  CHECK(b.OnPublish(32, true, false) == Decision::kDeferred);
}

TEST_CASE("Replay wake: a deferred worker is always woken by the end of a submission",
          "[graphics][replay-wake]") {
  // Model a submission: the worker sleeps from the start, the CP publishes
  // N commands in batches of 32 and finishes with a forced publish. Count
  // wakes and check that the last publish is never left without one.
  for (uint32_t total : {0u, 31u, 32u, 500u, 511u, 512u, 513u, 4000u, 40000u}) {
    ReplayWakeBatcher b;
    b.Begin(true);
    uint32_t written = 0;
    uint32_t wakes = 0;
    uint32_t since_wake = 0;
    uint32_t max_since_wake = 0;
    while (written + 32 <= total) {
      written += 32;
      since_wake += 32;
      const Decision d = b.OnPublish(32, true, false);
      if (d == Decision::kWake || d == Decision::kForced) {
        ++wakes;
        since_wake = 0;
      }
      max_since_wake = since_wake > max_since_wake ? since_wake : max_since_wake;
    }
    const Decision last = b.OnPublish(total - written, true, true);
    CHECK(last == Decision::kForced);
    CHECK(max_since_wake < kReplayWakeBatchCommands);
    CHECK(wakes == (written / 32) / (kReplayWakeBatchCommands / 32));
  }
}

TEST_CASE("Replay wake: line format", "[graphics][replay-wake]") {
  ReplayWakeCounters c;
  c.submissions = 270;
  c.batched_submissions = 270;
  c.idle_episodes = 60000;
  c.spin_pauses = 7680000;
  c.spin_us = 123456;
  c.sleeps = 50000;
  c.wakes = 3500;
  c.forced_wakes = 900;
  c.deferred_wakes = 52000;
  CHECK(FormatReplayWakeLine(true, c) ==
        "MCLA_REPLAY_WAKE v=1 batched=1 spin_limit=128 wake_batch=512 submissions=270 "
        "batched_submissions=270 idle_episodes=60000 spin_pauses=7680000 spin_ms=123.456 "
        "sleeps=50000 wakes=3500 forced_wakes=900 deferred_wakes=52000");
  CHECK(FormatReplayWakeLine(false, ReplayWakeCounters{}) ==
        "MCLA_REPLAY_WAKE v=1 batched=0 spin_limit=4096 wake_batch=0 submissions=0 "
        "batched_submissions=0 idle_episodes=0 spin_pauses=0 spin_ms=0.000 sleeps=0 wakes=0 "
        "forced_wakes=0 deferred_wakes=0");
}

}  // namespace rex::graphics::test
