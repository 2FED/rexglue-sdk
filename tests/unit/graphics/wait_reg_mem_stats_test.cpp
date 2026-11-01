#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/wait_reg_mem_stats.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace rex::graphics::test {

namespace {

// Deterministic auto-reset event with a scripted writer. Every Wait() either
// consumes a pending signal at once, or advances the fake clock: if a writer
// action is due within the timeout, it runs (it may write the value and set
// the event) and the wait returns signalled when it did set the event;
// otherwise the wait times out.
struct FakeAutoResetEvent {
  bool signaled = false;
  uint64_t now_us = 0;
  struct Action {
    uint64_t at_us;
    std::function<void(FakeAutoResetEvent&)> run;
  };
  std::vector<Action> actions;
  size_t next_action = 0;
  uint32_t waits = 0;

  void Set() { signaled = true; }

  // Returns true when signalled.
  bool Wait(uint64_t timeout_us) {
    ++waits;
    if (signaled) {
      signaled = false;
      return true;
    }
    const uint64_t deadline = now_us + timeout_us;
    while (next_action < actions.size() && actions[next_action].at_us <= deadline) {
      Action& action = actions[next_action++];
      if (action.at_us > now_us) {
        now_us = action.at_us;
      }
      action.run(*this);
      if (signaled) {
        signaled = false;
        return true;
      }
    }
    now_us = deadline;
    return false;
  }
};

struct FakeWaiter {
  FakeAutoResetEvent event;
  uint32_t value = 1;  // the guest release flag; the predicate is value == 0
  uint32_t polls = 0;
  bool running = true;
  WrmCounters counters;

  WrmLoopResult Run(uint64_t timeout_us = 1000) {
    return RunWaitRegMemLoop(
        [&]() {
          ++polls;
          return value == 0;
        },
        [&](uint32_t) {
          WrmBlockResult r;
          r.kind = WrmBlockKind::kEvent;
          const uint64_t before = event.now_us;
          r.signaled = event.Wait(timeout_us);
          r.blocked_us = event.now_us - before;
          return r;
        },
        [&](const WrmBlockResult& step) { counters.CountBlock(step); }, [&]() { return running; });
  }
};

}  // namespace

TEST_CASE("WRM histogram buckets", "[graphics][wrm]") {
  CHECK(WrmHistogramBucket(0) == 0);
  CHECK(WrmHistogramBucket(499) == 0);
  CHECK(WrmHistogramBucket(500) == 1);
  CHECK(WrmHistogramBucket(999) == 1);
  CHECK(WrmHistogramBucket(1000) == 2);
  CHECK(WrmHistogramBucket(1999) == 2);
  CHECK(WrmHistogramBucket(2000) == 3);
  CHECK(WrmHistogramBucket(3999) == 3);
  CHECK(WrmHistogramBucket(4000) == 4);
  CHECK(WrmHistogramBucket(UINT64_MAX) == 4);
}

TEST_CASE("WRM counters: packets, histogram and top predicate", "[graphics][wrm]") {
  WrmCounters c;
  WrmPredicate release{true, 0x1FCA3006, 3, 0, 0xFFFFFFFF};
  WrmPredicate scratch{true, 0x1FCA3006, 3, 1, 0xFFFFFFFF};
  WrmPredicate reg{false, 0x0A2F, 3, 0x80000000, 0x80000000};

  c.CountPacket(scratch, true, 0, 0, 0);  // satisfied at once
  c.CountPacket(release, true, 1, 0, 1900);
  c.CountPacket(release, true, 2, 1, 2100);
  c.CountPacket(reg, true, 3, 0, 300);
  c.CountPacket(release, false, 5, 0, 5000);  // aborted: not a waited episode

  CHECK(c.packets == 5);
  CHECK(c.immediate == 1);
  CHECK(c.waited == 3);
  CHECK(c.aborted == 1);
  CHECK(c.failed_polls == 11);
  CHECK(c.stale_wakes == 1);
  CHECK(c.wait_us == 4300);
  CHECK(c.max_wait_us == 2100);
  CHECK(c.histogram[0] == 1);
  CHECK(c.histogram[2] == 1);
  CHECK(c.histogram[3] == 1);
  CHECK(c.histogram[1] == 0);
  CHECK(c.histogram[4] == 0);
  CHECK(c.slot_count == 2);
  const WrmPredicateSlot* top = c.TopPredicate();
  REQUIRE(top != nullptr);
  CHECK(top->predicate == release);
  CHECK(top->episodes == 2);
  CHECK(top->wait_us == 4000);

  const std::string line = FormatWrmLine("minute", "minute", 1234, true, c);
  CHECK(line.rfind("MCLA_WRM v=1 kind=minute reason=minute frame=1234 event_wake=1 packets=5 "
                   "immediate=1 waited=3 aborted=1 failed_polls=11 ",
                   0) == 0);
  CHECK(line.find(" wait_ms=4.300 ") != std::string::npos);
  CHECK(line.find(" max_wait_ms=2.100") != std::string::npos);
  CHECK(line.find(" h_lt0_5ms=1 h_0_5to1ms=0 h_1to2ms=1 h_2to4ms=1 h_ge4ms=0 ") !=
        std::string::npos);
  CHECK(line.find(" top=mem:1FCA3006 top_func=3 top_ref=00000000 top_mask=FFFFFFFF "
                  "top_episodes=2 top_wait_ms=4.000") != std::string::npos);
  for (const char ch : line) {
    CHECK((ch >= 0x20 && ch < 0x7F));
  }

  WrmCounters empty;
  CHECK(empty.TopPredicate() == nullptr);
  CHECK(FormatWrmLine("mark", "F10 key!", 1, false, empty).find(" reason=F10_key_ ") !=
        std::string::npos);
  CHECK(FormatWrmLine("mark", "", 1, false, empty).find(" top=none") != std::string::npos);
}

TEST_CASE("WRM counters: predicate slots overflow into other", "[graphics][wrm]") {
  WrmCounters c;
  for (uint32_t i = 0; i < kWrmPredicateSlots + 3; ++i) {
    c.CountPacket(WrmPredicate{true, 0x1000 + i * 4, 3, 0, 0xFFFFFFFF}, true, 1, 0, 10 + i);
  }
  CHECK(c.slot_count == kWrmPredicateSlots);
  CHECK(c.other_predicate_episodes == 3);
  CHECK(c.waited == kWrmPredicateSlots + 3);
}

TEST_CASE("WRM block counting", "[graphics][wrm]") {
  WrmCounters c;
  c.CountBlock({WrmBlockKind::kYield, false, 0});
  c.CountBlock({WrmBlockKind::kYieldLong, false, 0});
  c.CountBlock({WrmBlockKind::kSleep, false, 1100});
  c.CountBlock({WrmBlockKind::kEvent, true, 40});
  c.CountBlock({WrmBlockKind::kEvent, false, 1000});
  CHECK(c.yields == 2);
  CHECK(c.sleeps == 1);
  CHECK(c.event_waits == 2);
  CHECK(c.event_wakes == 1);
  CHECK(c.event_timeouts == 1);
  CHECK(c.blocked_us == 2140);
}

TEST_CASE("WRM loop: satisfied at once never blocks", "[graphics][wrm]") {
  FakeWaiter w;
  w.value = 0;
  const WrmLoopResult r = w.Run();
  CHECK(r.matched);
  CHECK(r.failed_polls == 0);
  CHECK(w.event.waits == 0);
  CHECK(w.polls == 1);
}

TEST_CASE("WRM loop: a signal during the wait wakes it and the predicate is re-checked",
          "[graphics][wrm]") {
  FakeWaiter w;
  // The interrupt callback clears the flag 200 us in, then the event is set.
  w.event.actions.push_back({200, [&](FakeAutoResetEvent& e) {
                               w.value = 0;
                               e.Set();
                             }});
  const WrmLoopResult r = w.Run();
  CHECK(r.matched);
  CHECK(r.failed_polls == 1);
  CHECK(r.stale_wakes == 0);
  CHECK(w.polls == 2);  // the re-check after the wake
  CHECK(w.event.now_us == 200);
  CHECK(w.counters.event_wakes == 1);
  CHECK(w.counters.event_timeouts == 0);
  CHECK(w.counters.blocked_us == 200);
}

TEST_CASE("WRM loop: a stale signal costs one extra poll, not a missed release",
          "[graphics][wrm]") {
  FakeWaiter w;
  // A signal left from an earlier interrupt (nothing written yet).
  w.event.Set();
  // An unrelated interrupt (callback did not clear the flag) at 300 us, the
  // releasing one at 700 us.
  w.event.actions.push_back({300, [](FakeAutoResetEvent& e) { e.Set(); }});
  w.event.actions.push_back({700, [&](FakeAutoResetEvent& e) {
                               w.value = 0;
                               e.Set();
                             }});
  const WrmLoopResult r = w.Run();
  CHECK(r.matched);
  CHECK(r.failed_polls == 3);
  CHECK(r.stale_wakes == 2);
  CHECK(w.event.waits == 3);
  CHECK(w.event.now_us == 700);
  CHECK(w.counters.event_wakes == 3);
}

TEST_CASE("WRM loop: coalesced signals of an auto-reset event are not lost", "[graphics][wrm]") {
  FakeWaiter w;
  // Two interrupts before the waiter blocks: the event holds one signal, and
  // the value written before it is seen by the re-check.
  w.event.Set();
  w.event.Set();
  w.value = 0;  // written by the second interrupt, after the first poll below
  bool first = true;
  const WrmLoopResult r = RunWaitRegMemLoop(
      [&]() {
        if (first) {
          first = false;
          return false;  // polled before the write became visible
        }
        return w.value == 0;
      },
      [&](uint32_t) {
        WrmBlockResult b;
        b.kind = WrmBlockKind::kEvent;
        b.signaled = w.event.Wait(1000);
        return b;
      },
      [](const WrmBlockResult&) {}, []() { return true; });
  CHECK(r.matched);
  CHECK(r.failed_polls == 1);
  CHECK(w.event.waits == 1);
  CHECK(w.event.now_us == 0);  // returned at once
}

TEST_CASE("WRM loop: without a signal each wait is bounded by the old sleep", "[graphics][wrm]") {
  FakeWaiter w;
  // A writer that never signals (not an interrupt callback) releases at
  // 2500 us: the waiter falls back to the timeout cadence.
  w.event.actions.push_back({2500, [&](FakeAutoResetEvent&) { w.value = 0; }});
  const WrmLoopResult r = w.Run(1000);
  CHECK(r.matched);
  CHECK(r.failed_polls == 3);
  CHECK(w.counters.event_timeouts == 3);
  CHECK(w.counters.event_wakes == 0);
  CHECK(w.event.now_us == 3000);
}

TEST_CASE("WRM loop: a stopped command processor leaves after a long wait", "[graphics][wrm]") {
  FakeWaiter w;
  w.running = false;
  const WrmLoopResult r = w.Run();
  CHECK_FALSE(r.matched);
  CHECK(r.failed_polls == 1);

  // Short yields (wait < 0x100) do not check it, as before.
  uint32_t polls = 0;
  const WrmLoopResult y =
      RunWaitRegMemLoop([&]() { return ++polls == 4; }, [](uint32_t) { return WrmBlockResult{}; },
                        [](const WrmBlockResult&) {}, []() { return false; });
  CHECK(y.matched);
  CHECK(y.failed_polls == 3);
}

TEST_CASE("WRM loop: real auto-reset event never misses a release", "[graphics][wrm]") {
  // A writer thread releases the waiter many times (value then Set, as the
  // interrupt dispatch does); every wait is bounded by 5 s, so a lost signal
  // would show up as a timeout.
  auto event = rex::thread::Event::CreateAutoResetEvent(false);
  REQUIRE(event);
  std::atomic<uint32_t> value{0};
  std::atomic<uint32_t> acknowledged{0};
  constexpr uint32_t kRounds = 300;
  std::thread writer([&]() {
    for (uint32_t i = 1; i <= kRounds; ++i) {
      while (acknowledged.load(std::memory_order_acquire) != i - 1) {
        std::this_thread::yield();
      }
      if (i % 3 == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
      }
      value.store(i, std::memory_order_release);
      event->Set();
      if (i % 5 == 0) {
        event->Set();  // a spurious extra signal (stale for the next round)
      }
    }
  });
  WrmCounters counters;
  uint32_t matched = 0;
  for (uint32_t i = 1; i <= kRounds; ++i) {
    const WrmLoopResult r = RunWaitRegMemLoop(
        [&]() { return value.load(std::memory_order_acquire) == i; },
        [&](uint32_t) {
          WrmBlockResult b;
          b.kind = WrmBlockKind::kEvent;
          b.signaled = rex::thread::Wait(event.get(), false, std::chrono::milliseconds(5000)) ==
                       rex::thread::WaitResult::kSuccess;
          return b;
        },
        [&](const WrmBlockResult& step) { counters.CountBlock(step); }, []() { return true; });
    matched += r.matched ? 1 : 0;
    acknowledged.store(i, std::memory_order_release);
  }
  writer.join();
  CHECK(matched == kRounds);
  CHECK(counters.event_timeouts == 0);
}

}  // namespace rex::graphics::test
