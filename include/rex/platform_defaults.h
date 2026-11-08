/**
 * @file        rex/platform_defaults.h
 * @brief       l80: per-platform cvar defaults (Windows vs Wine/Proton).
 *
 * The l79 PC and Steam Deck A/B runs (docs/evidence/M11-l79-perf-ab-20261108.md,
 * docs/evidence/M11-l79-deck-ab-20261108.md) chose different defaults per
 * platform: placed texture heaps help on Windows (committed creation about
 * 400 us per texture on NVIDIA) but only add heap-creation hitches under
 * vkd3d-proton (committed creation already about 17 us); the timer queue's
 * blocking wait is on for both, with a shorter spin tail under Wine.
 *
 * SelectPlatformDefaults is a pure function of the platform (unit-tested);
 * the cvar definitions call it with content_log::RunningUnderWine() so the
 * default is in place before any subsystem reads the cvar.
 */

#pragma once

#include <cstdint>

namespace rex {

struct PlatformDefaults {
  // core/timer_queue.cpp
  bool timer_queue_blocking_wait = true;
  int32_t timer_queue_spin_tail_us = 200;
  // audio/xma_context.cpp
  bool xma_canary_fixes = true;
  // graphics/d3d12/texture_cache.cpp
  bool d3d12_texture_placed_heaps = true;
  // system/xthread.cpp (KI-060): "native", "yield" or "hires_timer"
  const char* ke_delay_short_sleep_mode = "native";
  // audio/audio_system.cpp, audio/xma_decoder.cpp: "normal", "above_normal"
  // or "highest"
  const char* apu_worker_priority = "normal";
  // l81 system/xthread.cpp (KI-060): "native" or "backoff"
  const char* ke_delay_zero_mode = "native";
  // l81 system/xthread.cpp (KI-060): "normal", "above_normal" or "highest"
  const char* guest_audio_thread_priority = "normal";
};

constexpr PlatformDefaults SelectPlatformDefaults(bool is_wine) {
  PlatformDefaults d;
  d.timer_queue_blocking_wait = true;
  d.timer_queue_spin_tail_us = is_wine ? 50 : 200;
  d.xma_canary_fixes = true;
  d.d3d12_texture_placed_heaps = !is_wine;
  d.ke_delay_short_sleep_mode = is_wine ? "hires_timer" : "native";
  d.apu_worker_priority = is_wine ? "above_normal" : "normal";
  d.ke_delay_zero_mode = is_wine ? "backoff" : "native";
  d.guest_audio_thread_priority = is_wine ? "above_normal" : "normal";
  return d;
}

// The defaults of the running process (Wine detection: content_log).
const PlatformDefaults& CurrentPlatformDefaults();

}  // namespace rex
