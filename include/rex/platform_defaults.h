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
};

constexpr PlatformDefaults SelectPlatformDefaults(bool is_wine) {
  PlatformDefaults d;
  d.timer_queue_blocking_wait = true;
  d.timer_queue_spin_tail_us = is_wine ? 50 : 200;
  d.xma_canary_fixes = true;
  d.d3d12_texture_placed_heaps = !is_wine;
  return d;
}

// The defaults of the running process (Wine detection: content_log).
const PlatformDefaults& CurrentPlatformDefaults();

}  // namespace rex
