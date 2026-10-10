/**
 * Regression coverage for the deterministic local XAM user profile.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/user_profile.h>

namespace {

using rex::system::xam::UserProfile;
using rex::X_RESULT;
using rex::X_STATUS;

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
    static std::atomic<uint64_t> sequence{0};
    std::random_device random;
    for (uint32_t attempt = 0; attempt < 128; ++attempt) {
      const auto candidate = std::filesystem::temp_directory_path() /
          ("rexglue-profile-test-" + std::to_string(random()) + "-" +
           std::to_string(random()) + "-" + std::to_string(++sequence));
      std::error_code ec;
      const bool created = std::filesystem::create_directory(candidate, ec);
      REQUIRE_FALSE(ec);
      if (created) {
        path = candidate;
        return;
      }
    }
    FAIL("Could not reserve an owned profile fixture directory");
  }
  ~TemporaryProfileRoot() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

}  // namespace

TEST_CASE("Local display name keeps the legacy profile and save identity",
          "[kernel][xam][profile][display-name]") {
  UserProfile original;
  UserProfile renamed(1, "Mcla Test_B-2");
  CHECK(renamed.name() == "Mcla Test_B-2");
  CHECK(renamed.xuid() == original.xuid());
  CHECK(renamed.signin_state() == original.signin_state());
  CHECK(renamed.type() == original.type());
  CHECK(renamed.storage_key() == "User");
  CHECK(renamed.storage_key() == original.storage_key());

  TemporaryProfileRoot root;
  CHECK(renamed.GlobalProfilePath(root.path) == original.GlobalProfilePath(root.path));
  CHECK(renamed.GlobalProfilePath(root.path) == root.path / "profile" / "User");
  CHECK(renamed.TitleProfilePath(root.path, 0x545407F8) ==
        original.TitleProfilePath(root.path, 0x545407F8));
  CHECK(renamed.TitleProfilePath(root.path, 0x545407F8) ==
        root.path / "545407F8" / "profile" / "User");
  rex::system::xam::ContentManager manager(nullptr, root.path);
  rex::system::xam::XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = rex::system::XContentType::kSavedGame;
  data.xuid = original.xuid();
  data.title_id = 0x545407F8;
  data.set_display_name(u"Synthetic existing save");
  data.set_file_name("mc4.sav");
  REQUIRE(manager.WriteContentHeaderFile(original.xuid(), data) == X_ERROR_SUCCESS);
  CHECK(std::filesystem::is_regular_file(root.path / "B13EBABEBABEBABE" /
      "545407F8" / "Headers" / "00000001" / "mc4.sav.header"));
  const auto old_save = root.path / "B13EBABEBABEBABE" / "545407F8" /
                       "00000001" / "mc4.sav";
  REQUIRE(std::filesystem::create_directories(old_save));
  {
    std::ofstream payload(old_save / "mc4.sav", std::ios::binary);
    REQUIRE(payload.is_open());
    payload << "synthetic existing payload";
    REQUIRE(payload.good());
  }
  CHECK(manager.ContentExists(original.xuid(), data));
  rex::system::xam::ContentManager restarted(nullptr, root.path);
  rex::system::xam::XCONTENT_AGGREGATE_DATA restored{};
  REQUIRE(restarted.ReadContentHeaderFile("mc4.sav", renamed.xuid(), 0x545407F8,
      rex::system::XContentType::kSavedGame, restored) == X_ERROR_SUCCESS);
  CHECK(restored == data);
  CHECK(restarted.ContentExists(renamed.xuid(), restored));
  const auto listed = restarted.ListContent(1, renamed.xuid(),
      rex::system::XContentType::kSavedGame, 0x545407F8);
  REQUIRE(listed.size() == 1);
  CHECK(listed.front() == data);
}

TEST_CASE("Local display names reject ambiguous or truncated identities",
          "[kernel][xam][profile][display-name]") {
  for (const auto& name : {std::string(""), std::string(" leading"),
       std::string("trailing "), std::string("a/b"), std::string("a\\b"),
       std::string(".."), std::string("a\nb"), std::string("a\tb"),
       std::string("a\0b", 3), std::string("1234567890123456"),
       std::string("\xC3\xA9")}) {
    CHECK_FALSE(UserProfile::IsValidDisplayName(name));
    CHECK_THROWS_AS(UserProfile(1, name), std::invalid_argument);
  }
  CHECK(UserProfile::IsValidDisplayName("A"));
  CHECK(UserProfile::IsValidDisplayName("123456789012345"));
  CHECK(UserProfile(1, "123456789012345").name() == "123456789012345");
}

TEST_CASE("Renamed local profile reloads the same legacy settings",
          "[kernel][xam][profile][display-name][persistence]") {
  TemporaryProfileRoot root;
  const auto legacy_root = root.path / "profile" / "User";
  {
    UserProfile original;
    original.SetPersistenceRoot(original.GlobalProfilePath(root.path));
    original.AddSetting(std::make_unique<UserProfile::Int32Setting>(0x10040002, 1));
    original.AddSetting(std::make_unique<UserProfile::UnicodeSetting>(0x402C0011, u"Saved"));
  }
  for (const auto& name : {"MclaTestA", "MclaTestB", "User"}) {
    UserProfile renamed(1, name);
    const auto resolved = renamed.GlobalProfilePath(root.path);
    CHECK(resolved == legacy_root);
    renamed.SetPersistenceRoot(resolved);
    CHECK(GetInt32Setting(renamed, 0x10040002).value == 1);
    CHECK(GetUnicodeSetting(renamed, 0x402C0011).value == u"Saved");
    CHECK_FALSE(std::filesystem::exists(root.path / "profile" / "MclaTestA"));
    CHECK_FALSE(std::filesystem::exists(root.path / "profile" / "MclaTestB"));
  }
}

TEST_CASE("Actual local display name CVar persists and rejects invalid startup configuration",
          "[kernel][xam][profile][display-name][cvar]") {
  struct ResetCvars {
    ResetCvars() { rex::cvar::testing::ResetAllForTesting(); }
    ~ResetCvars() { rex::cvar::testing::ResetAllForTesting(); }
  } reset;
  TemporaryProfileRoot root;
  const auto config = root.path / "display-name.toml";
  REQUIRE(rex::cvar::SetFlagByName("local_user_display_name", "MclaTestA"));
  rex::cvar::SaveConfig(config);
  rex::cvar::testing::ResetAllForTesting();
  CHECK(rex::cvar::GetFlagByName("local_user_display_name") == "User");
  rex::cvar::LoadConfig(config);
  UserProfile restarted(1, rex::cvar::GetFlagByName("local_user_display_name"));
  CHECK(restarted.name() == "MclaTestA");
  CHECK(restarted.storage_key() == "User");
  REQUIRE(rex::cvar::SetFlagByName("local_user_display_name", "MclaTestB"));
  rex::cvar::SaveConfig(config);
  rex::cvar::testing::ResetAllForTesting();
  rex::cvar::LoadConfig(config);
  UserProfile renamed_restart(1, rex::cvar::GetFlagByName("local_user_display_name"));
  CHECK(renamed_restart.name() == "MclaTestB");
  CHECK(renamed_restart.xuid() == restarted.xuid());
  CHECK(renamed_restart.GlobalProfilePath(root.path) == restarted.GlobalProfilePath(root.path));
  {
    std::ofstream invalid(config, std::ios::trunc);
    REQUIRE(invalid.is_open());
    invalid << "local_user_display_name = \" bad\"\n";
  }
  rex::cvar::LoadConfig(config);
  CHECK(rex::cvar::GetFlagByName("local_user_display_name") == " bad");
  CHECK_FALSE(UserProfile::IsValidDisplayName(rex::cvar::GetFlagByName("local_user_display_name")));
  CHECK_THROWS_AS(UserProfile(1, rex::cvar::GetFlagByName("local_user_display_name")),
                  std::invalid_argument);
  REQUIRE(rex::Runtime::instance() == nullptr);
  {
    rex::Runtime runtime(root.path / "unused-game", root.path / "unused-user");
    CHECK(runtime.Setup(rex::RuntimeConfig{}) == X_STATUS_INVALID_PARAMETER);
    CHECK(rex::Runtime::instance() == nullptr);
    CHECK(runtime.kernel_state() == nullptr);
    CHECK_FALSE(std::filesystem::exists(root.path / "unused-user"));
  }
  REQUIRE(rex::cvar::SetFlagByName("local_user_display_name", "MclaTestA"));
  rex::cvar::FinalizeInit();
  CHECK_FALSE(rex::cvar::SetFlagByName("local_user_display_name", "MclaTestB"));
  CHECK(rex::cvar::GetFlagByName("local_user_display_name") == "MclaTestA");
}

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
