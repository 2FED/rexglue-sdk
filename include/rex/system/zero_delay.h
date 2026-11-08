/**
 * @file        rex/system/zero_delay.h
 * @brief       l81 (KI-060): backoff for repeated zero guest delays.
 *
 * The l80 Deck analysis (docs/evidence/M11-l80-deck-analysis-20261108.md
 * sections 2-3) found that about 99.5 % of the short guest delays are
 * KeDelayExecutionThread(0) calls (Sleep(0)) from the title's sound-manager
 * tick, about 180,000 per second. Under Wine each one is a sched_yield
 * syscall; together they cost 0.45-0.85 of a logical CPU on the Deck.
 *
 * ke_delay_zero_mode=backoff keeps an isolated zero delay native, but once a
 * thread has made more than `after` zero delays in a row, each separated from
 * the previous delay's end by less than `gap_us` of guest work, the zero
 * delay becomes a real sleep on the thread's high-resolution timer: 50 us,
 * doubling up to `max_us`. Any non-zero, absolute or alertable delay, a delay
 * with a pending APC, or a gap of `gap_us` or more resets the streak.
 *
 * Everything here is pure (unit-tested in tests/unit/system/zero_delay_test.cpp);
 * the waits are in src/system/xthread.cpp.
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rex::system {

enum class ZeroDelayMode : uint8_t { kNative, kBackoff };

inline std::optional<ZeroDelayMode> ParseZeroDelayMode(std::string_view text) {
  if (text == "native") {
    return ZeroDelayMode::kNative;
  }
  if (text == "backoff") {
    return ZeroDelayMode::kBackoff;
  }
  return std::nullopt;
}

inline const char* ZeroDelayModeName(ZeroDelayMode mode) {
  return mode == ZeroDelayMode::kBackoff ? "backoff" : "native";
}

// First backoff sleep; it doubles on every further zero delay of the streak.
constexpr uint32_t kZeroDelayBackoffStartUs = 50;

struct ZeroDelayConfig {
  uint32_t after = 32;    // zero delays in a streak that stay native
  uint32_t max_us = 500;  // longest backoff sleep
  uint32_t gap_us = 200;  // guest work since the previous delay that ends a streak
};

// Clamps cvar values to their documented ranges.
constexpr ZeroDelayConfig MakeZeroDelayConfig(int32_t after, int32_t max_us, int32_t gap_us) {
  ZeroDelayConfig c;
  c.after = uint32_t(std::clamp(after, 1, 1 << 20));
  c.max_us = uint32_t(std::clamp(max_us, int32_t(kZeroDelayBackoffStartUs), 2000));
  c.gap_us = uint32_t(std::clamp(gap_us, 1, 100000));
  return c;
}

// Per-thread state (owned and touched only by the thread itself).
struct ZeroDelayStreak {
  uint32_t streak = 0;      // zero delays in the current streak (first = 1)
  uint32_t backoff_us = 0;  // last backoff sleep of the streak, 0 = none yet
  bool has_last_end = false;
  int64_t last_end_us = 0;  // end of the previous delay of this thread
};

// Called at the start of every guest delay of the thread. `eligible_zero`:
// a zero relative delay that is not alertable and has no pending APC.
// Returns the backoff sleep in microseconds, or 0 to keep the delay native.
constexpr uint32_t PlanZeroDelay(ZeroDelayStreak& s, const ZeroDelayConfig& c, bool eligible_zero,
                                 int64_t now_us) {
  if (!eligible_zero) {
    s.streak = 0;
    s.backoff_us = 0;
    s.has_last_end = false;
    return 0;
  }
  const bool continues =
      s.has_last_end && now_us >= s.last_end_us && now_us - s.last_end_us < int64_t(c.gap_us);
  if (continues) {
    if (s.streak < UINT32_MAX) {
      ++s.streak;
    }
  } else {
    s.streak = 1;
    s.backoff_us = 0;
  }
  if (s.streak <= c.after) {
    return 0;
  }
  s.backoff_us =
      s.backoff_us == 0 ? kZeroDelayBackoffStartUs : std::min(s.backoff_us * 2, c.max_us);
  s.backoff_us = std::min(s.backoff_us, c.max_us);
  return s.backoff_us;
}

// Called when an eligible zero delay returns, so the backoff sleep itself
// does not count as guest work.
constexpr void NoteZeroDelayEnd(ZeroDelayStreak& s, int64_t end_us) {
  s.has_last_end = true;
  s.last_end_us = end_us;
}

// Guest return address saved in the frame of KeDelayExecutionThread's guest
// caller (for the title's SleepEx: the address SleepEx returns to, i.e. the
// Sleep(0) call site, since its Sleep wrappers are tail calls). Xbox 360
// frames keep the back chain at 0(r1) and the saved LR at back_chain - 8.
// Returns 0 when an address would leave the thread's guest stack
// [stack_limit, stack_base).
template <typename Load32>
constexpr uint32_t GuestCallerSite(uint32_t r1, uint32_t stack_limit, uint32_t stack_base,
                                   Load32&& load32) {
  if (stack_limit >= stack_base || r1 < stack_limit || r1 > stack_base - 4) {
    return 0;
  }
  const uint32_t back = load32(r1);
  if (back <= r1 || back > stack_base || back - 8 < stack_limit) {
    return 0;
  }
  return load32(back - 8);
}

}  // namespace rex::system
