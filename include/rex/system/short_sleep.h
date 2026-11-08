/**
 * @file        rex/system/short_sleep.h
 * @brief       l80 (KI-060): short guest delays and the APU worker priority.
 *
 * The l79 Deck A/B (docs/evidence/M11-l79-deck-ab-20261108.md section 3)
 * tied the audio underruns to the high phase of a guest polling thread whose
 * sleep/IPC loop (KeDelayExecutionThread + XMsgInProcessCall) costs about 0.8
 * of a core, nearly half of it in the Wine kernel. XThread::Delay truncates a
 * relative delay to whole milliseconds, so a sub-millisecond delay becomes a
 * yield (non-alertable) or SleepEx(0, TRUE) (alertable), and 1-2 ms becomes
 * Sleep(1).
 *
 * ke_delay_short_sleep_mode picks how relative delays shorter than 2 ms are
 * carried out:
 *   native      - as before;
 *   yield       - SwitchToThread (alertable: a pending guest APC still takes
 *                 the native alertable path, which delivers it);
 *   hires_timer - the exact delay on a per-thread high-resolution waitable
 *                 timer (alertable waits stay alertable); a zero delay stays
 *                 native.
 * apu_worker_priority sets the host priority of the Audio Worker and XMA
 * Decoder threads.
 *
 * Everything here is pure (unit-tested in tests/unit/system/short_sleep_test.cpp);
 * the waits are in src/system/xthread.cpp and src/core/threading_*.cpp.
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace rex::system {

enum class ShortSleepMode : uint8_t { kNative, kYield, kHiresTimer };

inline std::optional<ShortSleepMode> ParseShortSleepMode(std::string_view text) {
  if (text == "native") {
    return ShortSleepMode::kNative;
  }
  if (text == "yield") {
    return ShortSleepMode::kYield;
  }
  if (text == "hires_timer") {
    return ShortSleepMode::kHiresTimer;
  }
  return std::nullopt;
}

inline const char* ShortSleepModeName(ShortSleepMode mode) {
  switch (mode) {
    case ShortSleepMode::kYield:
      return "yield";
    case ShortSleepMode::kHiresTimer:
      return "hires_timer";
    case ShortSleepMode::kNative:
    default:
      return "native";
  }
}

// 2 ms in 100 ns units (the guest's KeDelayExecutionThread interval unit).
constexpr int64_t kShortSleepLimitTicks = 20000;

// guest_interval: the raw KeDelayExecutionThread interval (negative =
// relative, positive = absolute, 0 = yield). Returns the relative delay in
// 100 ns units, or -1 for an absolute time.
constexpr int64_t RelativeDelayTicks(int64_t guest_interval) {
  return guest_interval > 0 ? -1 : -guest_interval;
}

// scaled_ticks: the relative delay after guest time scaling (-1 = absolute).
constexpr bool IsShortSleep(int64_t scaled_ticks) {
  return scaled_ticks >= 0 && scaled_ticks < kShortSleepLimitTicks;
}

enum class ShortSleepAction : uint8_t { kNative, kYield, kHiresTimer };

constexpr ShortSleepAction PlanShortSleep(ShortSleepMode mode, int64_t scaled_ticks) {
  if (!IsShortSleep(scaled_ticks) || mode == ShortSleepMode::kNative) {
    return ShortSleepAction::kNative;
  }
  if (mode == ShortSleepMode::kYield) {
    return ShortSleepAction::kYield;
  }
  // hires_timer: a zero delay has nothing to wait for; keep the native
  // yield / SleepEx(0, TRUE), which also drains host wake callbacks.
  return scaled_ticks == 0 ? ShortSleepAction::kNative : ShortSleepAction::kHiresTimer;
}

enum class ApuWorkerPriority : uint8_t { kNormal, kAboveNormal, kHighest };

inline std::optional<ApuWorkerPriority> ParseApuWorkerPriority(std::string_view text) {
  if (text == "normal") {
    return ApuWorkerPriority::kNormal;
  }
  if (text == "above_normal") {
    return ApuWorkerPriority::kAboveNormal;
  }
  if (text == "highest") {
    return ApuWorkerPriority::kHighest;
  }
  return std::nullopt;
}

inline const char* ApuWorkerPriorityName(ApuWorkerPriority priority) {
  switch (priority) {
    case ApuWorkerPriority::kAboveNormal:
      return "above_normal";
    case ApuWorkerPriority::kHighest:
      return "highest";
    case ApuWorkerPriority::kNormal:
    default:
      return "normal";
  }
}

// Per-minute counters of guest KeDelayExecutionThread calls (MCLA_SLEEP).
struct ShortSleepMinuteStats {
  uint64_t delays = 0;        // all KeDelayExecutionThread delays
  uint64_t short_sleeps = 0;  // ... relative and shorter than 2 ms (incl. zero)
  uint64_t zero = 0;          // ... of them with a zero interval
  uint64_t alertable = 0;     // ... of them alertable
  uint64_t alerted = 0;       // ... of them ended by an APC
  uint64_t yields = 0;        // ... carried out as a yield (mode yield)
  uint64_t timers = 0;        // ... carried out on the high-resolution timer
  uint64_t fallbacks = 0;     // ... timer unavailable or failed: native instead
  uint64_t requested_us_total = 0;
  uint64_t slept_us_total = 0;  // measured wall time of the short sleeps
  uint64_t slept_us_max = 0;
  uint32_t top_thread_id = 0;  // handle of the guest thread with the most short sleeps
  uint64_t top_thread_short = 0;
};

inline std::string FormatShortSleepMinute(const ShortSleepMinuteStats& s, ShortSleepMode mode) {
  const uint64_t avg_us = s.short_sleeps ? s.slept_us_total / s.short_sleeps : 0;
  const uint64_t req_avg_us = s.short_sleeps ? s.requested_us_total / s.short_sleeps : 0;
  return fmt::format(
      "MCLA_SLEEP v=1 kind=minute short_sleeps={} mode={} avg_us={} max_us={} req_avg_us={} "
      "zero={} alertable={} alerted={} yields={} timers={} fallbacks={} delays={} "
      "top_thread={:08X} top_short={}",
      s.short_sleeps, ShortSleepModeName(mode), avg_us, s.slept_us_max, req_avg_us, s.zero,
      s.alertable, s.alerted, s.yields, s.timers, s.fallbacks, s.delays, s.top_thread_id,
      s.top_thread_short);
}

}  // namespace rex::system
