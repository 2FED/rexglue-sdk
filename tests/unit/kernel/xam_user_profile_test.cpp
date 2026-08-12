/**
 * Regression coverage for the deterministic local XAM user profile.
 */

#include <cstdint>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/user_profile.h>

namespace {

using rex::system::xam::UserProfile;

const UserProfile::Int32Setting& GetInt32Setting(UserProfile& profile, uint32_t setting_id) {
  auto* setting = profile.GetSetting(setting_id);
  REQUIRE(setting != nullptr);
  REQUIRE(setting->type == UserProfile::Setting::Type::INT32);
  return *static_cast<UserProfile::Int32Setting*>(setting);
}

}  // namespace

TEST_CASE("Default XAM profile exposes one stable local signed-in user", "[kernel][xam][profile]") {
  UserProfile profile;

  CHECK(profile.name() == "User");
  CHECK(profile.xuid() == UINT64_C(0xB13EBABEBABEBABE));
  CHECK(profile.xuid() != 0);
  CHECK((profile.xuid() & UINT64_C(0x00C0000000000000)) == 0);
  CHECK(profile.signin_state() == 1);
  CHECK(profile.type() == (1 | 2));
}

TEST_CASE("Default XAM profile supplies MCLA voice settings", "[kernel][xam][profile]") {
  UserProfile profile;

  CHECK(GetInt32Setting(profile, 0x1004000C).value == 0);
  CHECK(GetInt32Setting(profile, 0x1004000D).value == 0);
  CHECK(GetInt32Setting(profile, 0x1004000E).value == 100);
}
