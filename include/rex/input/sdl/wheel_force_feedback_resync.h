#pragma once

// KI-022: bounded unconditional resync of infinite wheel force-feedback
// effects. Some DirectInput wheel drivers (observed on a Thrustmaster T300RS)
// drop a downloaded infinite effect at a burst of event effects or a game
// time-scale transition and keep reporting it as playing, so the status-based
// re-arm in wheel_force_feedback_rearm.h never fires. The focus-resume path
// (stop + run) restores such an effect every time, so the resync repeats that
// stop + run for every infinite effect the host believes is running, on a few
// bounded triggers:
//   - a host request (RequestWheelEffectResync), e.g. at a pause/slow-motion
//     transition the title performs;
//   - the end of a burst of finite event-effect starts;
//   - a slow periodic safety pass.
// The policy below is free of SDL calls so it can be unit tested with a fake
// haptic layer; the driver passes SDL lambdas.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <rex/input/input.h>
#include <rex/input/sdl/wheel_force_feedback.h>
#include <rex/input/sdl/wheel_force_feedback_rearm.h>
#include <rex/logging/format.h>

// Feature macro so a host can detect this API at compile time.
#define REX_INPUT_SDL_WHEEL_EFFECT_RESYNC 1

namespace rex::input::sdl {

// Thread-safe request from the host for one resync pass of the believed
// running infinite effects on the active wheel. Requests are coalesced (one
// pending pass, first reason kept) and executed on the input poll path
// (GetState) of the active focused wheel. A request that has not been
// executed within kWheelResyncRequestExpiryMs is dropped. `reason` is a short
// ASCII tag; a leading "host:" is accepted and not repeated. Safe to call
// with no wheel connected or with force feedback disabled (no-op then).
void RequestWheelEffectResync(const char* reason);

inline constexpr uint32_t kWheelResyncBudgetPerWindow = 6;
inline constexpr uint64_t kWheelResyncBudgetWindowMs = 60000;
inline constexpr uint64_t kWheelResyncPeriodicIntervalMs = 10000;
inline constexpr uint32_t kWheelResyncBurstMinStarts = 4;
inline constexpr uint64_t kWheelResyncBurstWindowMs = 2000;
// The race-start burst on a T300 was 28 finite starts on one slot within
// 0.8 s; the resync fires once the burst has settled, never inside it.
inline constexpr uint64_t kWheelResyncBurstQuietMs = 300;
inline constexpr uint64_t kWheelResyncRequestExpiryMs = 2000;
// A periodic pass leaves this many budget attempts per window for the
// event-driven triggers (host request, burst end).
inline constexpr uint32_t kWheelResyncPeriodicReserve = 1;
inline constexpr size_t kWheelResyncReasonMaxLength = 32;

enum class WheelResyncTrigger {
  kHost,
  kBurst,
  kPeriodic,
};

// Per-slot bound on resyncs; same window/one-line semantics as the re-arm
// budget, with its own limit.
struct WheelResyncBudget {
  uint64_t window_start_ms = 0;
  uint32_t attempts = 0;
  bool window_started = false;
  bool limit_logged = false;
};

constexpr void RefreshWheelResyncBudgetWindow(WheelResyncBudget& budget, uint64_t now_ms) {
  if (!budget.window_started || now_ms - budget.window_start_ms >= kWheelResyncBudgetWindowMs) {
    budget.window_started = true;
    budget.window_start_ms = now_ms;
    budget.attempts = 0;
    budget.limit_logged = false;
  }
}

constexpr WheelRearmBudgetResult ConsumeWheelResyncBudget(WheelResyncBudget& budget,
                                                          uint64_t now_ms) {
  RefreshWheelResyncBudgetWindow(budget, now_ms);
  if (budget.attempts < kWheelResyncBudgetPerWindow) {
    budget.attempts++;
    return WheelRearmBudgetResult::kAllowed;
  }
  if (!budget.limit_logged) {
    budget.limit_logged = true;
    return WheelRearmBudgetResult::kLimitReached;
  }
  return WheelRearmBudgetResult::kLimitedSilently;
}

// True when a periodic pass may spend budget on this slot without using the
// reserve kept for event-driven triggers.
constexpr bool WheelResyncPeriodicHasBudget(WheelResyncBudget& budget, uint64_t now_ms) {
  RefreshWheelResyncBudgetWindow(budget, now_ms);
  return budget.attempts + kWheelResyncPeriodicReserve < kWheelResyncBudgetPerWindow;
}

// Same candidate rule as the re-arm (running, created, infinite), but no
// status query: the device status is not trusted.
constexpr bool IsWheelEffectResyncCandidate(bool running, bool effect_created,
                                            uint32_t duration_ms) {
  return IsWheelEffectRearmCandidate(running, effect_created, duration_ms);
}

// Periodic cadence; `last_resync_ms` is the time of the last resync pass of
// any trigger (or the time the periodic timer was armed).
constexpr bool ShouldRunWheelPeriodicResync(uint64_t now_ms, uint64_t last_resync_ms) {
  return now_ms - last_resync_ms >= kWheelResyncPeriodicIntervalMs;
}

// Detects a burst of finite event-effect starts (>= kWheelResyncBurstMinStarts
// within kWheelResyncBurstWindowMs) and fires once kWheelResyncBurstQuietMs
// after the last start of the burst.
struct WheelFiniteBurstTracker {
  std::array<uint64_t, kWheelResyncBurstMinStarts> recent_ms = {};
  uint32_t recorded = 0;
  size_t next = 0;
  uint64_t last_start_ms = 0;
  bool armed = false;

  constexpr void OnFiniteStart(uint64_t now_ms) {
    recent_ms[next] = now_ms;
    next = (next + 1) % recent_ms.size();
    if (recorded < recent_ms.size()) {
      recorded++;
    }
    last_start_ms = now_ms;
    // `next` now indexes the oldest of the last kWheelResyncBurstMinStarts.
    if (recorded == recent_ms.size() && now_ms - recent_ms[next] <= kWheelResyncBurstWindowMs) {
      armed = true;
    }
  }

  // Returns true once per burst, when the quiet period after its last start
  // has elapsed.
  constexpr bool ConsumeBurstEnd(uint64_t now_ms) {
    if (!armed || now_ms - last_start_ms < kWheelResyncBurstQuietMs) {
      return false;
    }
    armed = false;
    recorded = 0;
    next = 0;
    return true;
  }
};

// Normalizes a host reason to a short log-safe tag without the "host:"
// prefix: [A-Za-z0-9._-] kept, other characters become '-', truncated to
// kWheelResyncReasonMaxLength; empty becomes "request".
inline std::string NormalizeWheelResyncHostReason(const char* reason) {
  std::string_view text = reason ? std::string_view(reason) : std::string_view();
  constexpr std::string_view kPrefix = "host:";
  if (text.substr(0, kPrefix.size()) == kPrefix) {
    text.remove_prefix(kPrefix.size());
  }
  std::string out;
  for (char c : text) {
    if (out.size() >= kWheelResyncReasonMaxLength) {
      break;
    }
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
    out.push_back(keep ? c : '-');
  }
  if (out.empty()) {
    out = "request";
  }
  return out;
}

// One coalesced pending host request (not thread-safe by itself; the driver
// guards it with a mutex). Offer() keeps the first reason of a pending,
// unexpired request; Take() consumes it and drops it when expired.
struct WheelResyncRequestSlot {
  bool pending = false;
  std::string reason;
  uint64_t requested_ms = 0;

  // Returns false when the request was coalesced into a pending one.
  bool Offer(std::string tag, uint64_t now_ms) {
    if (pending && now_ms - requested_ms <= kWheelResyncRequestExpiryMs) {
      return false;
    }
    pending = true;
    reason = std::move(tag);
    requested_ms = now_ms;
    return true;
  }

  bool Take(uint64_t now_ms, std::string& out_reason) {
    if (!pending) {
      return false;
    }
    pending = false;
    if (now_ms - requested_ms > kWheelResyncRequestExpiryMs) {
      return false;
    }
    out_reason = reason;
    return true;
  }
};

// KI-022 visibility: l46 logged only the first update per slot, so a title
// that neutralised an effect later (zero gain/strength, or an infinite effect
// turned finite) would be invisible. The driver logs an update whose
// neutral/finite state differs from the slot's previous one, rate limited.
inline constexpr uint32_t kWheelUpdateChangeLogPerWindow = 6;
inline constexpr uint64_t kWheelUpdateChangeLogWindowMs = 60000;

constexpr bool IsWheelConditionNeutral(const ForceFeedbackCondition& condition) {
  return (condition.right_coefficient == 0 && condition.left_coefficient == 0) ||
         (condition.right_saturation == 0 && condition.left_saturation == 0);
}

// True when the effect, after the host gain, produces no force.
constexpr bool IsWheelEffectNeutral(const ForceFeedbackEffect& effect, uint32_t gain_percent) {
  if (gain_percent == 0) {
    return true;
  }
  switch (effect.type) {
    case ForceFeedbackEffectType::kConstant:
      return effect.constant_level == 0;
    case ForceFeedbackEffectType::kRamp:
      return effect.ramp_start == 0 && effect.ramp_end == 0;
    case ForceFeedbackEffectType::kSquare:
    case ForceFeedbackEffectType::kSine:
    case ForceFeedbackEffectType::kTriangle:
    case ForceFeedbackEffectType::kSawtoothUp:
    case ForceFeedbackEffectType::kSawtoothDown:
      return effect.periodic_magnitude == 0 && effect.periodic_offset == 0;
    case ForceFeedbackEffectType::kSpring:
    case ForceFeedbackEffectType::kDamper:
      for (const auto& condition : effect.conditions) {
        if (!IsWheelConditionNeutral(condition)) {
          return false;
        }
      }
      return true;
  }
  return false;
}

struct WheelUpdateChangeState {
  bool known = false;
  bool neutral = false;
  bool finite = false;
  WheelResyncBudget log_budget;
};

enum class WheelUpdateChangeLog {
  kNone,
  kLog,
  kLimitReached,  // first refused change in the window; log the limit once
};

// Records the slot's state at create (or the first update) without logging.
constexpr void ResetWheelUpdateChange(WheelUpdateChangeState& state, bool neutral, bool finite) {
  state.known = true;
  state.neutral = neutral;
  state.finite = finite;
}

// Observes a later update; returns kLog when its neutral/finite state
// changed and the per-slot log budget allows it.
constexpr WheelUpdateChangeLog ObserveWheelUpdateChange(WheelUpdateChangeState& state,
                                                        bool neutral, bool finite,
                                                        uint64_t now_ms) {
  if (!state.known) {
    ResetWheelUpdateChange(state, neutral, finite);
    return WheelUpdateChangeLog::kNone;
  }
  if (state.neutral == neutral && state.finite == finite) {
    return WheelUpdateChangeLog::kNone;
  }
  state.neutral = neutral;
  state.finite = finite;
  auto& budget = state.log_budget;
  if (!budget.window_started || now_ms - budget.window_start_ms >= kWheelUpdateChangeLogWindowMs) {
    budget.window_started = true;
    budget.window_start_ms = now_ms;
    budget.attempts = 0;
    budget.limit_logged = false;
  }
  if (budget.attempts < kWheelUpdateChangeLogPerWindow) {
    budget.attempts++;
    return WheelUpdateChangeLog::kLog;
  }
  if (!budget.limit_logged) {
    budget.limit_logged = true;
    return WheelUpdateChangeLog::kLimitReached;
  }
  return WheelUpdateChangeLog::kNone;
}

inline std::string FormatWheelUpdateChangeLine(size_t slot_number, ForceFeedbackEffectType type,
                                               bool neutral, uint32_t duration_ms,
                                               uint32_t gain_percent) {
  return fmt::format(
      "SDL_WHEEL_FFB_EFFECT v=2 action=update-change slot={} type={} neutral={} finite={} "
      "duration={} gain_percent={}",
      slot_number, GetWheelEffectTypeName(type), neutral ? 1 : 0,
      duration_ms != UINT32_MAX ? 1 : 0, duration_ms, gain_percent);
}

inline std::string FormatWheelUpdateChangeLimitLine(size_t slot_number,
                                                    ForceFeedbackEffectType type) {
  return fmt::format(
      "SDL_WHEEL_FFB_EFFECT v=2 action=update-change-limit slot={} type={} limit={} window_ms={}",
      slot_number, GetWheelEffectTypeName(type), kWheelUpdateChangeLogPerWindow,
      kWheelUpdateChangeLogWindowMs);
}

enum class WheelResyncEvent {
  kResynced,
  kRunFailed,
  kLimitReached,
};

// Resyncs one slot: if the host believes it is a running infinite effect,
// stop it and run it again (the focus-resume sequence), regardless of the
// device status (never queried; the start path's status-playing skip does
// not apply) and without re-sending parameters: the device keeps the ones it
// holds. `stop(effect_id)` and `run(effect_id, iterations)` wrap the
// haptic layer; `on_event(event, slot_number, type)` receives every outcome
// worth logging. A failed run keeps the running belief (the status re-arm or
// a later resync can retry, still bounded). Returns true on success.
template <typename Slot, typename StopFn, typename RunFn, typename EventFn>
bool ResyncWheelEffect(Slot& slot, size_t slot_number, uint64_t now_ms, WheelResyncTrigger trigger,
                       StopFn&& stop, RunFn&& run, EventFn&& on_event) {
  if (!IsWheelEffectResyncCandidate(slot.running, slot.effect_id >= 0, slot.duration_ms)) {
    return false;
  }
  if (trigger == WheelResyncTrigger::kPeriodic &&
      !WheelResyncPeriodicHasBudget(slot.resync_budget, now_ms)) {
    return false;
  }
  switch (ConsumeWheelResyncBudget(slot.resync_budget, now_ms)) {
    case WheelRearmBudgetResult::kAllowed:
      break;
    case WheelRearmBudgetResult::kLimitReached:
      on_event(WheelResyncEvent::kLimitReached, slot_number, slot.type);
      return false;
    case WheelRearmBudgetResult::kLimitedSilently:
      return false;
  }
  stop(slot.effect_id);
  if (!run(slot.effect_id, slot.iterations)) {
    on_event(WheelResyncEvent::kRunFailed, slot_number, slot.type);
    return false;
  }
  on_event(WheelResyncEvent::kResynced, slot_number, slot.type);
  return true;
}

template <typename Slots, typename StopFn, typename RunFn, typename EventFn>
uint32_t ResyncWheelEffects(Slots& slots, uint64_t now_ms, WheelResyncTrigger trigger,
                            StopFn&& stop, RunFn&& run, EventFn&& on_event) {
  uint32_t resynced = 0;
  size_t slot_number = 1;
  for (auto& slot : slots) {
    if (ResyncWheelEffect(slot, slot_number, now_ms, trigger, stop, run, on_event)) {
      resynced++;
    }
    slot_number++;
  }
  return resynced;
}

template <typename Slots>
bool AnyWheelEffectResyncCandidate(const Slots& slots) {
  for (const auto& slot : slots) {
    if (IsWheelEffectResyncCandidate(slot.running, slot.effect_id >= 0, slot.duration_ms)) {
      return true;
    }
  }
  return false;
}

// `reason` is the full log reason: "host:<tag>", "burst" or "periodic".
inline std::string FormatWheelResyncLine(size_t slot_number, ForceFeedbackEffectType type,
                                         std::string_view reason) {
  return fmt::format("SDL_WHEEL_FFB_EFFECT v=2 action=resync slot={} type={} reason={}",
                     slot_number, GetWheelEffectTypeName(type), reason);
}

inline std::string FormatWheelResyncLimitLine(size_t slot_number, ForceFeedbackEffectType type,
                                              std::string_view reason) {
  return fmt::format(
      "SDL_WHEEL_FFB_EFFECT v=2 action=resync-limit slot={} type={} reason={} limit={} "
      "window_ms={}",
      slot_number, GetWheelEffectTypeName(type), reason, kWheelResyncBudgetPerWindow,
      kWheelResyncBudgetWindowMs);
}

}  // namespace rex::input::sdl
