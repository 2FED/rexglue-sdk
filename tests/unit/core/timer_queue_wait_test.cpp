// l79: timer_queue_blocking_wait scheduling decision (PlanTimerWait) and a
// coarse live check of the timer queue in blocking mode (wake on a new,
// earlier timer; deadlines still met).

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/cvar.h>
#include <rex/thread/timer_queue.h>
#include <rex/thread/timer_queue_wait.h>

REXCVAR_DECLARE(bool, timer_queue_blocking_wait);

using rex::thread::PlanTimerWait;
using rex::thread::TimerWaitKind;
using rex::thread::WaitableTimerRelativeDue;

namespace {

using clock_t_ = std::chrono::steady_clock;
using namespace std::chrono_literals;

const clock_t_::duration kTail = std::chrono::duration_cast<clock_t_::duration>(500us);
const clock_t_::duration kMin = std::chrono::duration_cast<clock_t_::duration>(50us);

}  // namespace

TEST_CASE("Timer queue wait: no timer blocks until a queue wake", "[core][timer_queue]") {
  const auto now = clock_t_::now();
  const auto plan = PlanTimerWait<clock_t_>(now, false, {}, kTail, kMin);
  CHECK(plan.kind == TimerWaitKind::kBlockInfinite);
}

TEST_CASE("Timer queue wait: due or overdue timer runs at once", "[core][timer_queue]") {
  const auto now = clock_t_::now();
  CHECK(PlanTimerWait<clock_t_>(now, true, now, kTail, kMin).kind == TimerWaitKind::kRunDue);
  CHECK(PlanTimerWait<clock_t_>(now, true, now - 3ms, kTail, kMin).kind == TimerWaitKind::kRunDue);
}

TEST_CASE("Timer queue wait: inside the spin tail spins", "[core][timer_queue]") {
  const auto now = clock_t_::now();
  CHECK(PlanTimerWait<clock_t_>(now, true, now + 100us, kTail, kMin).kind == TimerWaitKind::kSpin);
  CHECK(PlanTimerWait<clock_t_>(now, true, now + 500us, kTail, kMin).kind == TimerWaitKind::kSpin);
  // Remaining block shorter than min_block (500 + 30 us): spin as well.
  CHECK(PlanTimerWait<clock_t_>(now, true, now + 530us, kTail, kMin).kind == TimerWaitKind::kSpin);
}

TEST_CASE("Timer queue wait: blocks until the spin tail before the deadline",
          "[core][timer_queue]") {
  const auto now = clock_t_::now();
  const auto due = now + 1ms;
  const auto plan = PlanTimerWait<clock_t_>(now, true, due, kTail, kMin);
  CHECK(plan.kind == TimerWaitKind::kBlock);
  CHECK(plan.block_until == due - kTail);
  CHECK(plan.block_until > now);
  // Long wait: same rule.
  const auto far = PlanTimerWait<clock_t_>(now, true, now + 250ms, kTail, kMin);
  CHECK(far.kind == TimerWaitKind::kBlock);
  CHECK(far.block_until == now + 250ms - kTail);
}

TEST_CASE("Timer queue wait: zero or negative spin tail blocks to the deadline",
          "[core][timer_queue]") {
  const auto now = clock_t_::now();
  const auto due = now + 2ms;
  auto plan = PlanTimerWait<clock_t_>(now, true, due, clock_t_::duration::zero(), kMin);
  CHECK(plan.kind == TimerWaitKind::kBlock);
  CHECK(plan.block_until == due);
  plan = PlanTimerWait<clock_t_>(now, true, due, -kTail, kMin);
  CHECK(plan.kind == TimerWaitKind::kBlock);
  CHECK(plan.block_until == due);
  // Closer than min_block with no tail: spin.
  CHECK(PlanTimerWait<clock_t_>(now, true, now + 20us, clock_t_::duration::zero(), kMin).kind ==
        TimerWaitKind::kSpin);
}

TEST_CASE("Timer queue wait: waitable timer relative due time", "[core][timer_queue]") {
  CHECK(WaitableTimerRelativeDue(1ms) == -10000);
  CHECK(WaitableTimerRelativeDue(500us) == -5000);
  CHECK(WaitableTimerRelativeDue(std::chrono::nanoseconds(150)) == -1);
  // Never zero/positive (that would be an absolute time).
  CHECK(WaitableTimerRelativeDue(std::chrono::nanoseconds(0)) == -1);
  CHECK(WaitableTimerRelativeDue(std::chrono::nanoseconds(-5)) == -1);
}

TEST_CASE("Timer queue: blocking mode wakes for an earlier timer", "[core][timer_queue]") {
  const bool saved = REXCVAR_GET(timer_queue_blocking_wait);
  REXCVAR_SET(timer_queue_blocking_wait, true);

  // A far timer puts the dispatch thread into a long blocking wait.
  std::atomic<int> far_fired{0};
  auto far = rex::thread::QueueTimerOnce([&](void*) { far_fired.fetch_add(1); }, nullptr,
                                         clock_t_::now() + 10s);
  std::this_thread::sleep_for(20ms);

  // A new, earlier timer must wake it (QueueTimer -> wake event).
  std::atomic<bool> near_fired{false};
  std::atomic<int64_t> late_us{-1};
  const auto due = clock_t_::now() + 5ms;
  auto near = rex::thread::QueueTimerOnce(
      [&](void*) {
        late_us.store(
            std::chrono::duration_cast<std::chrono::microseconds>(clock_t_::now() - due).count());
        near_fired.store(true);
      },
      nullptr, due);
  const auto give_up = clock_t_::now() + 2s;
  while (!near_fired.load() && clock_t_::now() < give_up) {
    std::this_thread::sleep_for(1ms);
  }
  CHECK(near_fired.load());
  // Never early; generous bound for loaded test machines.
  CHECK(late_us.load() >= 0);
  CHECK(late_us.load() < 200000);

  if (auto item = far.lock()) {
    item->Disarm();
  }
  CHECK(far_fired.load() == 0);
  REXCVAR_SET(timer_queue_blocking_wait, saved);
}
