/**
 * @file        core/platform_defaults.cpp
 * @brief       l80: per-platform cvar defaults of the running process.
 */

#include <rex/content_log.h>
#include <rex/platform_defaults.h>

namespace rex {

const PlatformDefaults& CurrentPlatformDefaults() {
  static const PlatformDefaults defaults = SelectPlatformDefaults(content_log::RunningUnderWine());
  return defaults;
}

}  // namespace rex
