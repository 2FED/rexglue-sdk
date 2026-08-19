/**
 ******************************************************************************
 * ReXGlue : Xbox 360 Recompilation Runtime                                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace rex::audio::sdl {

struct PlaybackDeviceConfigEntry {
  uint32_t id;
  std::string_view name;
};

inline uint32_t FindPlaybackDeviceByName(std::string_view requested_name,
                                         std::span<const PlaybackDeviceConfigEntry> devices) {
  for (const PlaybackDeviceConfigEntry& device : devices) {
    if (device.name == requested_name) {
      return device.id;
    }
  }
  return 0;
}

inline void ApplyOutputVolume(float* samples, size_t sample_count, float volume) {
  if (!samples || volume == 1.0f) {
    return;
  }
  for (size_t i = 0; i < sample_count; ++i) {
    samples[i] *= volume;
  }
}

}  // namespace rex::audio::sdl
