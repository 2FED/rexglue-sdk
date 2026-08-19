/**
 * Regression coverage for explicit offline XONLINE/social/XHV hook results.
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/kernel/xam/offline_progression.h>
#include <rex/system/xtypes.h>

using rex::X_RESULT;

extern "C" REX_FUNC(__imp__XamShowFriendsUI);
extern "C" REX_FUNC(__imp__XamShowGamerCardUIForXUID);
extern "C" REX_FUNC(__imp__XamShowPlayerReviewUI);
extern "C" REX_FUNC(__imp__NetDll_XNetServerToInAddr);
extern "C" REX_FUNC(__imp__NetDll_XNetUnregisterInAddr);
extern "C" REX_FUNC(__imp__NetDll_XNetConnect);
extern "C" REX_FUNC(__imp__NetDll_XNetGetConnectStatus);
extern "C" REX_FUNC(__imp__NetDll_XNetQosLookup);
extern "C" REX_FUNC(__imp__XNetLogonGetTitleID);
extern "C" REX_FUNC(__imp__XamVoiceHeadsetPresent);
extern "C" REX_FUNC(__imp__XamVoiceSubmitPacket);
extern "C" REX_FUNC(__imp__XamShowAchievementsUI);
extern "C" REX_FUNC(__imp__XamUserCreateStatsEnumerator);

namespace {

using Hook = void (*)(PPCContext&, uint8_t*);

uint64_t CallOfflineHook(Hook hook) {
  PPCContext ctx{};
  ctx.r3.u64 = 0xDEADBEEF;
  hook(ctx, nullptr);
  return ctx.r3.u64;
}

}  // namespace

TEST_CASE("Offline social UI hooks return not-logged-on instead of stale r3",
          "[kernel][xam][offline]") {
  CHECK(CallOfflineHook(__imp__XamShowFriendsUI) == uint32_t(X_ERROR_NOT_LOGGED_ON));
  CHECK(CallOfflineHook(__imp__XamShowGamerCardUIForXUID) == uint32_t(X_ERROR_NOT_LOGGED_ON));
  CHECK(CallOfflineHook(__imp__XamShowPlayerReviewUI) == uint32_t(X_ERROR_NOT_LOGGED_ON));
}

TEST_CASE("Offline XNet hooks expose deterministic failure and cleanup states",
          "[kernel][xam][offline]") {
  constexpr uint32_t kWsaNetworkUnreachable = 0x2743;
  constexpr uint32_t kXNetConnectStatusLost = 3;

  CHECK(CallOfflineHook(__imp__NetDll_XNetServerToInAddr) == kWsaNetworkUnreachable);
  CHECK(CallOfflineHook(__imp__NetDll_XNetUnregisterInAddr) == 0);
  CHECK(CallOfflineHook(__imp__NetDll_XNetConnect) == kWsaNetworkUnreachable);
  CHECK(CallOfflineHook(__imp__NetDll_XNetGetConnectStatus) == kXNetConnectStatusLost);
  CHECK(CallOfflineHook(__imp__NetDll_XNetQosLookup) == uint32_t(X_ERROR_FUNCTION_FAILED));
  CHECK(CallOfflineHook(__imp__XNetLogonGetTitleID) == 0);
}

TEST_CASE("Offline XHV headset query is deterministically false", "[kernel][xam][offline]") {
  CHECK(CallOfflineHook(__imp__XamVoiceHeadsetPresent) == 0);
}

TEST_CASE("Offline return-bearing hooks never preserve stale r3", "[kernel][xam][offline]") {
  CHECK(CallOfflineHook(__imp__XamShowAchievementsUI) == uint32_t(X_ERROR_NOT_LOGGED_ON));
  CHECK(CallOfflineHook(__imp__XamVoiceSubmitPacket) ==
        rex::kernel::xam::kOfflineVoiceSubmitResult);
  CHECK(CallOfflineHook(__imp__XamUserCreateStatsEnumerator) ==
        uint32_t(X_ERROR_INVALID_PARAMETER));
}

TEST_CASE("Offline progression policies separate local state from retired services",
          "[kernel][xam][offline]") {
  using rex::kernel::xam::GetOfflineProgressionBehavior;
  using rex::kernel::xam::OfflineProgressionBehavior;
  using rex::kernel::xam::OfflineProgressionFeature;

  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kAchievements) ==
        OfflineProgressionBehavior::kLocal);
  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kPresence) ==
        OfflineProgressionBehavior::kUnavailable);
  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kLeaderboards) ==
        OfflineProgressionBehavior::kEmptyEnumeration);
  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kRateMyRide) ==
        OfflineProgressionBehavior::kUnavailable);
  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kDrivingTest) ==
        OfflineProgressionBehavior::kLocalSave);
  CHECK(GetOfflineProgressionBehavior(OfflineProgressionFeature::kVoice) ==
        OfflineProgressionBehavior::kUnavailable);
}

TEST_CASE("Offline leaderboard buffer sizing is bounded and title-compatible",
          "[kernel][xam][offline]") {
  using rex::kernel::xam::ComputeOfflineStatsBufferSize;

  uint32_t size = 0;
  constexpr std::array<uint32_t, 2> columns = {3, 1};
  CHECK(ComputeOfflineStatsBufferSize(5, columns, &size));
  CHECK(size == 0x08 + 2 * 0x10 + 2 * 5 * 0x30 + 4 * 5 * 0x1C);

  constexpr std::array<uint32_t, 1> too_many_columns = {65};
  CHECK_FALSE(ComputeOfflineStatsBufferSize(0, columns, &size));
  CHECK_FALSE(ComputeOfflineStatsBufferSize(101, columns, &size));
  CHECK_FALSE(ComputeOfflineStatsBufferSize(1, too_many_columns, &size));
  CHECK_FALSE(ComputeOfflineStatsBufferSize(1, columns, nullptr));
}
