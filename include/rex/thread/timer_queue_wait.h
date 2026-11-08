/**
 * @file        rex/thread/timer_queue_wait.h
 * @brief       l79: scheduling decision of the timer queue's blocking wait.
 *
 * The timer queue dispatch thread (src/core/timer_queue.cpp) normally spins
 * (disruptorplus spin_wait_strategy: pause bursts, then yield, a 1 ms sleep
 * every 20th yield) until the next timer is due or a new timer is queued. With
 * timer_queue_blocking_wait it instead blocks on a high-resolution waitable
 * timer (Windows: CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) plus a wake event set
 * by QueueTimer, set to wake `spin_tail` before the deadline, and spins only
 * that short tail, so the deadline accuracy stays that of the spin path.
 *
 * Design from the idea in docs/evidence/M11-external-repos-survey-20261108.md
 * (no third-party code). Pure function, unit-tested.
 */

#pragma once

#include <chrono>

namespace rex::thread {

enum class TimerWaitKind {
  kRunDue,         // the earliest timer is due now: dispatch it
  kSpin,           // due within the spin tail: spin (pause) and re-check
  kBlock,          // block until block_until (or a queue wake), then re-check
  kBlockInfinite,  // no timer queued: block until a queue wake
};

template <typename Clock>
struct TimerWaitPlan {
  TimerWaitKind kind = TimerWaitKind::kRunDue;
  typename Clock::time_point block_until{};
};

// now: current time; has_due/due: the earliest queued timer; spin_tail: how
// long before the deadline the blocking wait ends (the rest is spun);
// min_block: a block shorter than this is spun instead (a wait that short
// costs more in wake-up latency than it saves).
template <typename Clock>
TimerWaitPlan<Clock> PlanTimerWait(typename Clock::time_point now, bool has_due,
                                   typename Clock::time_point due,
                                   typename Clock::duration spin_tail,
                                   typename Clock::duration min_block) {
  TimerWaitPlan<Clock> plan;
  if (!has_due) {
    plan.kind = TimerWaitKind::kBlockInfinite;
    return plan;
  }
  if (due <= now) {
    plan.kind = TimerWaitKind::kRunDue;
    return plan;
  }
  if (spin_tail < typename Clock::duration::zero()) {
    spin_tail = typename Clock::duration::zero();
  }
  const auto remaining = due - now;
  if (remaining <= spin_tail || remaining - spin_tail < min_block) {
    plan.kind = TimerWaitKind::kSpin;
    return plan;
  }
  plan.kind = TimerWaitKind::kBlock;
  plan.block_until = due - spin_tail;
  return plan;
}

// Relative due time for SetWaitableTimerEx: negative, in 100 ns units, at
// least 1 unit (a zero/positive value would mean an absolute time).
template <typename Duration>
long long WaitableTimerRelativeDue(Duration wait) {
  const long long ticks =
      std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10000000>>>(wait)
          .count();
  return ticks > 0 ? -ticks : -1;
}

}  // namespace rex::thread
