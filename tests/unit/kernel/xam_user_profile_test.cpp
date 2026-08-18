/**
 * Regression coverage for the deterministic local XAM user profile.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

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

const UserProfile::UnicodeSetting& GetUnicodeSetting(UserProfile& profile, uint32_t setting_id) {
  auto* setting = profile.GetSetting(setting_id);
  REQUIRE(setting != nullptr);
  REQUIRE(setting->type == UserProfile::Setting::Type::WSTRING);
  return *static_cast<UserProfile::UnicodeSetting*>(setting);
}

struct TemporaryProfileRoot {
  TemporaryProfileRoot() {
    static uint32_t sequence = 0;
    path = std::filesystem::temp_directory_path() /
           ("rexglue-profile-test-" + std::to_string(++sequence));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  ~TemporaryProfileRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

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

TEST_CASE("XAM profile sign-in compatibility state is explicit and fail-closed",
          "[kernel][xam][profile]") {
  UserProfile online_compatible(2);
  UserProfile invalid(99);

  CHECK(online_compatible.signin_state() == 2);
  CHECK(invalid.signin_state() == 1);
}

TEST_CASE("Default XAM profile supplies MCLA voice settings", "[kernel][xam][profile]") {
  UserProfile profile;

  CHECK(GetInt32Setting(profile, 0x1004000C).value == 0);
  CHECK(GetInt32Setting(profile, 0x1004000D).value == 0);
  CHECK(GetInt32Setting(profile, 0x1004000E).value == 100);
}

TEST_CASE("XAM profile persists allowlisted scalar and Unicode settings across restart",
          "[kernel][xam][profile][persistence]") {
  TemporaryProfileRoot root;
  {
    UserProfile profile;
    profile.SetPersistenceRoot(root.path);
    profile.AddSetting(std::make_unique<UserProfile::Int32Setting>(0x10040002, 1));
    profile.AddSetting(std::make_unique<UserProfile::UnicodeSetting>(0x402C0011, u"Racer"));
  }

  CHECK(std::filesystem::exists(root.path / "10040002.bin"));
  CHECK(std::filesystem::exists(root.path / "402C0011.bin"));
  CHECK(std::distance(std::filesystem::directory_iterator(root.path),
                      std::filesystem::directory_iterator{}) == 2);

  UserProfile restarted;
  restarted.SetPersistenceRoot(root.path);
  CHECK(GetInt32Setting(restarted, 0x10040002).value == 1);
  CHECK(GetUnicodeSetting(restarted, 0x402C0011).value == u"Racer");
  CHECK(GetInt32Setting(restarted, 0x1004000E).value == 100);
}

TEST_CASE("XAM profile rejects mismatched persistent setting metadata",
          "[kernel][xam][profile][persistence]") {
  TemporaryProfileRoot root;
  {
    UserProfile profile;
    profile.SetPersistenceRoot(root.path);
    profile.AddSetting(std::make_unique<UserProfile::Int32Setting>(0x10040002, 1));
  }

  auto path = root.path / "10040002.bin";
  std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
  REQUIRE(file);
  file.seekp(12);
  const uint8_t wrong_type[4] = {5, 0, 0, 0};
  file.write(reinterpret_cast<const char*>(wrong_type), sizeof(wrong_type));
  file.close();

  UserProfile restarted;
  restarted.SetPersistenceRoot(root.path);
  CHECK(GetInt32Setting(restarted, 0x10040002).value == 0);
}

TEST_CASE("XAM profile restart recovers the previous committed setting",
          "[kernel][xam][profile][persistence]") {
  TemporaryProfileRoot root;
  {
    UserProfile profile;
    profile.SetPersistenceRoot(root.path);
    profile.AddSetting(std::make_unique<UserProfile::Int32Setting>(0x10040002, 1));
  }

  auto final_path = root.path / "10040002.bin";
  auto backup_path = root.path / "10040002.bin.bak";
  auto temporary_path = root.path / "10040002.bin.tmp";
  std::filesystem::rename(final_path, backup_path);
  {
    std::ofstream temporary(temporary_path, std::ios::binary);
    temporary << "incomplete";
  }

  UserProfile restarted;
  restarted.SetPersistenceRoot(root.path);
  CHECK(GetInt32Setting(restarted, 0x10040002).value == 1);
  CHECK(std::filesystem::exists(final_path));
  CHECK_FALSE(std::filesystem::exists(backup_path));
  CHECK_FALSE(std::filesystem::exists(temporary_path));
}

TEST_CASE("XAM title-specific settings never leak into the global profile root",
          "[kernel][xam][profile][persistence]") {
  TemporaryProfileRoot root;
  UserProfile profile;
  profile.SetPersistenceRoot(root.path);
  profile.AddSetting(
      std::make_unique<UserProfile::BinarySetting>(0x63E83FFF, std::vector<uint8_t>{1, 2, 3}));

  CHECK_FALSE(std::filesystem::exists(root.path / "63E83FFF.bin"));
}
