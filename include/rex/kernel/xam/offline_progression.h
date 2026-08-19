/**
 ******************************************************************************
 * ReXGlue - explicit offline progression and retired-service policy          *
 ******************************************************************************
 */

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <span>

#include <rex/system/xtypes.h>

namespace rex::kernel::xam {

enum class OfflineProgressionBehavior : uint8_t {
  kLocal,
  kEmptyEnumeration,
  kUnavailable,
  kLocalSave,
};

enum class OfflineProgressionFeature : uint8_t {
  kAchievements,
  kPresence,
  kLeaderboards,
  kRateMyRide,
  kDrivingTest,
  kVoice,
};

struct OfflineProgressionPolicy {
  OfflineProgressionFeature feature;
  OfflineProgressionBehavior behavior;
};

inline constexpr std::array<OfflineProgressionPolicy, 6> kOfflineProgressionPolicies = {{
    {OfflineProgressionFeature::kAchievements, OfflineProgressionBehavior::kLocal},
    {OfflineProgressionFeature::kPresence, OfflineProgressionBehavior::kUnavailable},
    {OfflineProgressionFeature::kLeaderboards, OfflineProgressionBehavior::kEmptyEnumeration},
    {OfflineProgressionFeature::kRateMyRide, OfflineProgressionBehavior::kUnavailable},
    {OfflineProgressionFeature::kDrivingTest, OfflineProgressionBehavior::kLocalSave},
    {OfflineProgressionFeature::kVoice, OfflineProgressionBehavior::kUnavailable},
}};

constexpr OfflineProgressionBehavior GetOfflineProgressionBehavior(
    OfflineProgressionFeature feature) {
  for (const auto& policy : kOfflineProgressionPolicies) {
    if (policy.feature == feature) {
      return policy.behavior;
    }
  }
  return OfflineProgressionBehavior::kUnavailable;
}

inline constexpr uint32_t kOfflineAchievementsUiResult = X_ERROR_NOT_LOGGED_ON;
inline constexpr uint32_t kOfflineVoiceSubmitResult = X_HRESULT_FROM_WIN32(X_ERROR_BUSY);

inline constexpr uint32_t kOfflineStatsMaxRows = 100;
inline constexpr uint32_t kOfflineStatsMaxSpecs = 64;
inline constexpr uint32_t kOfflineStatsMaxColumnsPerSpec = 64;
inline constexpr uint32_t kOfflineStatsResultsSize = 0x08;
inline constexpr uint32_t kOfflineStatsViewSize = 0x10;
inline constexpr uint32_t kOfflineStatsRowSize = 0x30;
// X_USER_STATS_COLUMN is 0x18 bytes, with four bytes of title-expected
// per-column storage in the enumerator result layout.
inline constexpr uint32_t kOfflineStatsColumnStride = 0x1C;

constexpr bool ComputeOfflineStatsBufferSize(uint32_t num_rows,
                                             std::span<const uint32_t> column_counts,
                                             uint32_t* buffer_size_out) {
  if (!buffer_size_out || !num_rows || num_rows > kOfflineStatsMaxRows || column_counts.empty() ||
      column_counts.size() > kOfflineStatsMaxSpecs) {
    return false;
  }

  uint64_t size = kOfflineStatsResultsSize;
  for (uint32_t columns : column_counts) {
    if (columns > kOfflineStatsMaxColumnsPerSpec) {
      return false;
    }
    size += kOfflineStatsViewSize;
    size += uint64_t(kOfflineStatsRowSize) * num_rows;
    size += uint64_t(kOfflineStatsColumnStride) * num_rows * columns;
    if (size > std::numeric_limits<uint32_t>::max()) {
      return false;
    }
  }
  *buffer_size_out = static_cast<uint32_t>(size);
  return true;
}

}  // namespace rex::kernel::xam
