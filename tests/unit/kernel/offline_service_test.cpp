/**
 * Regression coverage for explicit offline XONLINE/social/XHV hook results.
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
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
