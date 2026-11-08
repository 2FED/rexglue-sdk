/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    MCLA-R l79, 2026 - timer_queue_blocking_wait (high-resolution
 *              waitable timer + wake event with a short spin tail instead of
 *              a pure spin) and a per-minute dispatch-lateness line.
 */

#include <algorithm>
#include <forward_list>

#include <disruptorplus/multi_threaded_claim_strategy.hpp>
#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/sequence_barrier.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>
#include <rex/thread/timer_queue.h>
#include <rex/thread/timer_queue_wait.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"
#else
#include <condition_variable>
#include <mutex>
#endif

#if defined(_M_IX86) || defined(_M_AMD64) || defined(__i386__) || defined(__amd64__)
#include <immintrin.h>
#define REX_TIMER_QUEUE_X86 1
#endif

REXCVAR_DEFINE_BOOL(timer_queue_blocking_wait, false, "Kernel",
                    "l79: the host timer queue thread (KeTimeStampBundle 1 ms tick on Windows; "
                    "all guest timers on Linux) blocks on a high-resolution waitable timer and a "
                    "wake event until timer_queue_spin_tail_us before the next deadline and "
                    "spins only that tail, instead of spinning/yielding the whole time. Needs "
                    "CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+); otherwise the "
                    "spin path stays. Dispatch lateness: MCLA_TIMERQ kind=minute");
REXCVAR_DEFINE_INT32(timer_queue_spin_tail_us, 500, "Kernel",
                     "l79: with timer_queue_blocking_wait, wake this many microseconds before "
                     "the next timer deadline and spin the rest (0 = no spin tail; clamped to "
                     "0..5000)");

namespace dp = disruptorplus;

namespace rex::thread {

namespace {

inline void CpuRelax() {
#if defined(REX_TIMER_QUEUE_X86)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

// Blocks the dispatch thread until a deadline or a Wake() from QueueTimer.
// Spurious early returns are allowed (the caller re-plans).
class TimerQueueSleeper {
 public:
  using clock = TimerQueueWaitItem::clock;

#if REX_PLATFORM_WIN32
  TimerQueueSleeper() {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
  }
  ~TimerQueueSleeper() {
    if (timer_) {
      CloseHandle(timer_);
    }
    if (event_) {
      CloseHandle(event_);
    }
  }
  TimerQueueSleeper(const TimerQueueSleeper&) = delete;
  TimerQueueSleeper& operator=(const TimerQueueSleeper&) = delete;

  // Without a high-resolution timer the wait could overshoot by a scheduler
  // tick (up to 15.6 ms): the spin path stays then.
  bool ok() const { return event_ != nullptr && timer_ != nullptr; }
  void Wake() { SetEvent(event_); }
  // infinite: until Wake(). Returns true when woken by Wake().
  bool WaitUntil(bool infinite, clock::time_point until) {
    if (infinite) {
      return WaitForSingleObject(event_, INFINITE) == WAIT_OBJECT_0;
    }
    const auto wait = until - clock::now();
    if (wait <= clock::duration::zero()) {
      return false;
    }
    LARGE_INTEGER due;
    due.QuadPart = WaitableTimerRelativeDue(wait);
    if (!SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
      return false;  // the caller spins this round
    }
    const HANDLE handles[2] = {event_, timer_};
    const DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0) {
      CancelWaitableTimer(timer_);
      return true;
    }
    return false;
  }

 private:
  HANDLE event_ = nullptr;
  HANDLE timer_ = nullptr;
#else
  bool ok() const { return true; }
  void Wake() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      signalled_ = true;
    }
    cv_.notify_one();
  }
  bool WaitUntil(bool infinite, clock::time_point until) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (infinite) {
      cv_.wait(lock, [this] { return signalled_; });
    } else {
      cv_.wait_until(lock, until, [this] { return signalled_; });
    }
    const bool woken = signalled_;
    signalled_ = false;
    return woken;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool signalled_ = false;
#endif
};

// Per-minute dispatch statistics (MCLA_TIMERQ kind=minute), dispatch thread only.
struct TimerQueueMinuteStats {
  uint64_t dispatches = 0;
  uint64_t late_250us = 0;   // dispatched 250 us or more after the deadline
  uint64_t late_1ms = 0;     // ... 1 ms or more
  int64_t max_late_us = 0;
  uint64_t blocks = 0;       // blocking waits entered
  uint64_t queue_wakes = 0;  // blocking waits ended by QueueTimer
};

}  // namespace

using WaitItem = TimerQueueWaitItem;

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ =
        std::jthread([this](std::stop_token stop_token) { TimerThreadMain(stop_token); });
  }

  ~TimerQueue() {
    dispatch_thread_.request_stop();

    // Kick dispatch thread to check stop token
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this, clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    // std::jthread auto-joins on destruction
  }

  void TimerThreadMain(std::stop_token stop_token) {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    set_current_thread_name("rex::thread::TimerQueue");

    bool announced = false;
    auto minute_start = clock::now();
    while (!stop_token.stop_requested()) {
      // l79: timer_queue_blocking_wait, read every pass (the queue starts at
      // static initialisation, before the configuration is applied).
      const bool blocking_requested = REXCVAR_GET(timer_queue_blocking_wait);
      const bool blocking = blocking_requested && sleeper_.ok();
      if (blocking_requested && !announced) {
        announced = true;
        REXLOG_INFO(
            "timer_queue: blocking wait {} (high-resolution waitable timer {}), spin tail {} us",
            blocking ? "on" : "unavailable, spin path kept", sleeper_.ok() ? "ok" : "missing",
            std::chrono::duration_cast<std::chrono::microseconds>(SpinTail()).count());
      }
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available =
            blocking ? claim_strategy_.last_published_after(next_sequence - 1)
                     : claim_strategy_.wait_until_published(
                           next_sequence, next_sequence - 1,
                           wait_queue_.empty() ? clock::time_point::max()
                                               : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() && wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(state, WaitItem::State::kInCallback,
                                                        std::memory_order_acq_rel)) {
            NoteDispatch(wait_item->due_);
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle, std::memory_order_release);
              wait_item->state_.notify_all();
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed, std::memory_order_release);
              wait_item->state_.notify_all();
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }

      const auto now = clock::now();
      // No logging once stopping (static destruction: the logger may be gone).
      if (now - minute_start >= std::chrono::minutes(1) && !stop_token.stop_requested()) {
        EmitMinute(blocking);
        minute_start = now;
      }
      if (blocking) {
        WaitBlocking(next_sequence);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ = std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);
    // l79 blocking wait: wake the dispatch thread so it re-plans with the new
    // deadline (handshake: see WaitBlocking).
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (sleeping_.load(std::memory_order_relaxed)) {
      sleeper_.Wake();
    }

    return wait_item_weak;
  }

  std::jthread::id dispatch_thread_id() const { return dispatch_thread_.get_id(); }

 private:
  // One blocking-mode wait step; returns to the loop, which re-checks the
  // ring and the due timers.
  void WaitBlocking(dp::sequence_t next_sequence) {
    const bool has_due = !wait_queue_.empty();
    const auto plan = PlanTimerWait<clock>(clock::now(), has_due,
                                           has_due ? wait_queue_.front()->due_ : clock::time_point{},
                                           SpinTail(), kMinBlock);
    switch (plan.kind) {
      case TimerWaitKind::kRunDue:
        return;
      case TimerWaitKind::kSpin:
        for (int i = 0; i < 16; ++i) {
          CpuRelax();
        }
        return;
      case TimerWaitKind::kBlock:
      case TimerWaitKind::kBlockInfinite:
        break;
    }
    // Dekker-style handshake with QueueTimer: set the flag, then re-check the
    // ring; QueueTimer publishes, then reads the flag. Either this re-check
    // sees the new item or QueueTimer sees the flag and signals the (sticky,
    // auto-reset) wake event.
    sleeping_.store(true, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (claim_strategy_.last_published_after(next_sequence - 1) == next_sequence - 1) {
      ++minute_.blocks;
      if (sleeper_.WaitUntil(plan.kind == TimerWaitKind::kBlockInfinite, plan.block_until)) {
        ++minute_.queue_wakes;
      }
    }
    sleeping_.store(false, std::memory_order_relaxed);
  }

  static clock::duration SpinTail() {
    const int32_t us = std::clamp<int32_t>(REXCVAR_GET(timer_queue_spin_tail_us), 0, 5000);
    return std::chrono::duration_cast<clock::duration>(std::chrono::microseconds(us));
  }

  void NoteDispatch(clock::time_point due) {
    ++minute_.dispatches;
    const int64_t late_us =
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - due).count();
    if (late_us >= 250) {
      ++minute_.late_250us;
    }
    if (late_us >= 1000) {
      ++minute_.late_1ms;
    }
    minute_.max_late_us = std::max(minute_.max_late_us, late_us);
  }

  void EmitMinute(bool blocking) {
    REXLOG_INFO(
        "MCLA_TIMERQ v=1 kind=minute dispatches={} late_250us={} late_1ms={} max_late_us={} "
        "blocks={} queue_wakes={} blocking={} spin_tail_us={}",
        minute_.dispatches, minute_.late_250us, minute_.late_1ms, minute_.max_late_us,
        minute_.blocks, minute_.queue_wakes, int(blocking),
        std::chrono::duration_cast<std::chrono::microseconds>(SpinTail()).count());
    minute_ = TimerQueueMinuteStats{};
  }

  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  // l79 blocking wait: a block shorter than this is spun instead.
  static constexpr clock::duration kMinBlock =
      std::chrono::duration_cast<clock::duration>(std::chrono::microseconds(50));
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;
  dp::spin_wait_strategy wait_strategy_;
  dp::multi_threaded_claim_strategy<dp::spin_wait_strategy> claim_strategy_;
  dp::sequence_barrier<dp::spin_wait_strategy> consumed_;

  // l79 blocking wait (constructed before dispatch_thread_ starts).
  TimerQueueSleeper sleeper_;
  std::atomic<bool> sleeping_{false};
  TimerQueueMinuteStats minute_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::jthread dispatch_thread_;
};

rex::thread::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed, std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      break;
    }
    if (state == State::kInCallback || state == State::kInCallbackSelfDisarmed) {
      // Wait for callback to complete - dispatch thread will notify
      state_.wait(state, std::memory_order_acquire);
    }
    state = State::kIdle;
  }
}

std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback, void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, WaitItem::clock::duration::zero()));
}

std::weak_ptr<WaitItem> QueueTimerRecurring(std::function<void(void*)> callback, void* userdata,
                                            WaitItem::clock::time_point due,
                                            WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace rex::thread
