#pragma once

// KI-022: lifecycle helpers for wheel force-feedback effects whose host-side
// "running" belief can drift from the device. DirectInput can stop downloaded
// effects behind the host's back (silent re-acquire, driver eviction under a
// burst of event effects) and SDL_UpdateHapticEffect never restarts a stopped
// effect. These helpers keep the policy free of SDL calls so the decision
// logic can be unit tested with a fake status/run layer.

#include <cstddef>
#include <cstdint>
#include <string>

#include <rex/input/input.h>
#include <rex/input/sdl/wheel_force_feedback.h>
#include <rex/logging/format.h>

namespace rex::input::sdl {

inline constexpr uint64_t kWheelRearmWatchdogIntervalMs = 1000;
inline constexpr uint64_t kWheelRearmBudgetWindowMs = 60000;
inline constexpr uint32_t kWheelRearmBudgetPerWindow = 10;

// Per-slot bound on re-arm attempts so a device that always reports an
// effect as stopped cannot make the host spin on SDL_RunHapticEffect.
struct WheelRearmBudget {
  uint64_t window_start_ms = 0;
  uint32_t attempts = 0;
  bool window_started = false;
  bool limit_logged = false;
};

enum class WheelRearmBudgetResult {
  kAllowed,
  kLimitReached,     // first refusal in the current window; log it once
  kLimitedSilently,  // later refusals in the same window
};

constexpr WheelRearmBudgetResult ConsumeWheelRearmBudget(WheelRearmBudget& budget,
                                                         uint64_t now_ms) {
  if (!budget.window_started || now_ms - budget.window_start_ms >= kWheelRearmBudgetWindowMs) {
    budget.window_started = true;
    budget.window_start_ms = now_ms;
    budget.attempts = 0;
    budget.limit_logged = false;
  }
  if (budget.attempts < kWheelRearmBudgetPerWindow) {
    budget.attempts++;
    return WheelRearmBudgetResult::kAllowed;
  }
  if (!budget.limit_logged) {
    budget.limit_logged = true;
    return WheelRearmBudgetResult::kLimitReached;
  }
  return WheelRearmBudgetResult::kLimitedSilently;
}

constexpr bool ShouldRunWheelRearmWatchdog(uint64_t now_ms, uint64_t last_check_ms) {
  return now_ms - last_check_ms >= kWheelRearmWatchdogIntervalMs;
}

// Only infinite effects (centering spring/damper, continuous engine or road
// periodic) are owned across frames. Finite event effects must expire.
constexpr bool IsWheelEffectRearmCandidate(bool running, bool effect_created,
                                           uint32_t duration_ms) {
  return running && effect_created && duration_ms == UINT32_MAX;
}

// True when a guest UpdateEffect changes the slot's effect type, which forces
// a destroy/create of the host effect.
constexpr bool IsWheelEffectRetype(bool update, bool effect_created,
                                   ForceFeedbackEffectType previous_type,
                                   ForceFeedbackEffectType new_type) {
  return update && effect_created && previous_type != new_type;
}

// Logs the first suppressed infinite start per run of a slot; the flag is
// cleared by every successful start/stop.
constexpr bool ShouldLogWheelStartSuppressed(bool& already_logged) {
  if (already_logged) {
    return false;
  }
  already_logged = true;
  return true;
}

enum class WheelRearmEvent {
  kRearmed,
  kRunFailed,
  kLimitReached,
};

// Re-arms one slot if the host believes it is a running infinite effect and
// the device reports it stopped. `Slot` needs effect_id, type, iterations,
// duration_ms, running and rearm_budget members. `status_playing(effect_id)`
// and `run(effect_id, iterations)` wrap the haptic layer;
// `on_event(event, slot_number, type)` receives every outcome worth logging.
// Returns true when the effect was re-run successfully.
template <typename Slot, typename StatusPlayingFn, typename RunFn, typename EventFn>
bool RearmStoppedWheelEffect(Slot& slot, size_t slot_number, uint64_t now_ms,
                             StatusPlayingFn&& status_playing, RunFn&& run, EventFn&& on_event) {
  if (!IsWheelEffectRearmCandidate(slot.running, slot.effect_id >= 0, slot.duration_ms)) {
    return false;
  }
  if (status_playing(slot.effect_id)) {
    return false;
  }
  switch (ConsumeWheelRearmBudget(slot.rearm_budget, now_ms)) {
    case WheelRearmBudgetResult::kAllowed:
      break;
    case WheelRearmBudgetResult::kLimitReached:
      on_event(WheelRearmEvent::kLimitReached, slot_number, slot.type);
      return false;
    case WheelRearmBudgetResult::kLimitedSilently:
      return false;
  }
  if (!run(slot.effect_id, slot.iterations)) {
    // The host keeps its running belief so a later watchdog pass can retry,
    // still bounded by the per-slot budget.
    on_event(WheelRearmEvent::kRunFailed, slot_number, slot.type);
    return false;
  }
  on_event(WheelRearmEvent::kRearmed, slot_number, slot.type);
  return true;
}

template <typename Slots, typename StatusPlayingFn, typename RunFn, typename EventFn>
uint32_t RearmStoppedWheelEffects(Slots& slots, uint64_t now_ms, StatusPlayingFn&& status_playing,
                                  RunFn&& run, EventFn&& on_event) {
  uint32_t rearmed = 0;
  size_t slot_number = 1;
  for (auto& slot : slots) {
    if (RearmStoppedWheelEffect(slot, slot_number, now_ms, status_playing, run, on_event)) {
      rearmed++;
    }
    slot_number++;
  }
  return rearmed;
}

inline std::string FormatWheelRearmLine(size_t slot_number, ForceFeedbackEffectType type,
                                        const char* reason) {
  return fmt::format("SDL_WHEEL_FFB_EFFECT v=2 action=rearm slot={} type={} reason={}",
                     slot_number, GetWheelEffectTypeName(type), reason);
}

inline std::string FormatWheelRearmLimitLine(size_t slot_number, ForceFeedbackEffectType type,
                                             const char* reason) {
  return fmt::format(
      "SDL_WHEEL_FFB_EFFECT v=2 action=rearm-limit slot={} type={} reason={} limit={} "
      "window_ms={}",
      slot_number, GetWheelEffectTypeName(type), reason, kWheelRearmBudgetPerWindow,
      kWheelRearmBudgetWindowMs);
}

inline std::string FormatWheelRetypeLine(size_t slot_number, ForceFeedbackEffectType from,
                                         ForceFeedbackEffectType to, bool restart) {
  return fmt::format("SDL_WHEEL_FFB_EFFECT v=2 action=retype slot={} from={} to={} restart={}",
                     slot_number, GetWheelEffectTypeName(from), GetWheelEffectTypeName(to),
                     restart ? 1 : 0);
}

inline std::string FormatWheelStartSuppressedLine(size_t slot_number,
                                                  ForceFeedbackEffectType type) {
  return fmt::format(
      "SDL_WHEEL_FFB_EFFECT v=2 action=start-suppressed slot={} type={} reason=status-playing",
      slot_number, GetWheelEffectTypeName(type));
}

}  // namespace rex::input::sdl
