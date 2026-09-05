/**
 ******************************************************************************
 * Regression coverage for durable XAM saved-game content metadata.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/content_manager.h>
#include <rex/system/xam/content_device.h>

namespace {

namespace fs = std::filesystem;
using rex::X_RESULT;
using rex::system::XContentType;
using rex::system::xam::ContentManager;
using rex::system::xam::HasDummyHddCapacity;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

class TempContentRoot {
 public:
  TempContentRoot() {
    static std::atomic<uint64_t> sequence{0};
    // CTest runs cases in separate processes. A process-local counter alone
    // let one fixture delete another fixture's live save. Reserve a fresh
    // directory atomically; never clean up a path this instance did not create.
    std::random_device random;
    const auto parent = fs::temp_directory_path();
    for (uint32_t attempt = 0; attempt < 128; ++attempt) {
      const auto candidate = parent / ("rex-xam-content-persistence-" +
          std::to_string(random()) + "-" + std::to_string(random()) + "-" +
          std::to_string(++sequence));
      std::error_code error;
      const bool created = fs::create_directory(candidate, error);
      REQUIRE_FALSE(error);
      if (created) {
        path_ = candidate;
        return;
      }
    }
    FAIL("Could not reserve a unique content fixture directory");
  }

  ~TempContentRoot() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

XCONTENT_AGGREGATE_DATA MakeSavedGameData() {
  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;
  data.content_type = XContentType::kSavedGame;
  data.xuid = UINT64_C(0x0123456789ABCDEF);
  data.title_id = 0x545407F8;
  data.set_display_name(u"MCLA regression save");
  data.set_file_name("mc4.sav");
  return data;
}

fs::path PackagePath(const fs::path& root) {
  return root / "0123456789ABCDEF" / "545407F8" / "00000001" / "mc4.sav";
}

fs::path HeaderPath(const fs::path& root) {
  return root / "0123456789ABCDEF" / "545407F8" / "Headers" / "00000001" / "mc4.sav.header";
}

void WritePayload(const fs::path& root, std::string_view payload) {
  const auto package_path = PackagePath(root);
  REQUIRE((fs::create_directories(package_path) || fs::exists(package_path)));
  std::ofstream file(package_path / "mc4.sav", std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << payload;
  REQUIRE(file.good());
}

std::string ReadPayload(const fs::path& root) {
  std::ifstream file(PackagePath(root) / "mc4.sav", std::ios::binary);
  REQUIRE(file.is_open());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

TEST_CASE("XAM saved-game metadata survives manager restart", "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  const uint64_t xuid = expected.xuid;
  const uint32_t title_id = expected.title_id;

  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.WriteContentHeaderFile(xuid, expected) == X_ERROR_SUCCESS);

    const auto package_path = PackagePath(temp.path());
    REQUIRE(fs::create_directories(package_path));
    CHECK(manager.ContentExists(xuid, expected));
  }

  ContentManager restarted(nullptr, temp.path());
  XCONTENT_AGGREGATE_DATA restored{};
  REQUIRE(restarted.ReadContentHeaderFile("mc4.sav", xuid, title_id, XContentType::kSavedGame,
                                          restored) == X_ERROR_SUCCESS);
  CHECK(restored == expected);
  CHECK(restored.xuid == expected.xuid);
  CHECK(restored.display_name() == expected.display_name());

  const auto listed = restarted.ListContent(1, xuid, XContentType::kSavedGame, title_id);
  REQUIRE(listed.size() == 1);
  CHECK(listed.front() == expected);
  CHECK(listed.front().display_name() == expected.display_name());
}

TEST_CASE("XAM saved-game metadata rejects truncated headers", "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  const auto header_path =
      temp.path() / "0123456789ABCDEF" / "545407F8" / "Headers" / "00000001" / "mc4.sav.header";
  REQUIRE(fs::create_directories(header_path.parent_path()));
  {
    std::ofstream file(header_path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    const uint32_t truncated = 0;
    file.write(reinterpret_cast<const char*>(&truncated), sizeof(truncated));
    REQUIRE(file.good());
  }

  ContentManager manager(nullptr, temp.path());
  XCONTENT_AGGREGATE_DATA restored{};
  CHECK(manager.ReadContentHeaderFile("mc4.sav", expected.xuid, expected.title_id,
                                      XContentType::kSavedGame,
                                      restored) == X_ERROR_FILE_NOT_FOUND);
}

TEST_CASE("XAM saved-game metadata rejects mismatched full-size headers",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  auto corrupt = expected;
  corrupt.set_file_name("other.sav");
  const auto header_path = HeaderPath(temp.path());
  REQUIRE(fs::create_directories(header_path.parent_path()));
  {
    std::ofstream file(header_path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(reinterpret_cast<const char*>(&corrupt), sizeof(corrupt));
    REQUIRE(file.good());
  }
  WritePayload(temp.path(), "payload");

  ContentManager manager(nullptr, temp.path());
  XCONTENT_AGGREGATE_DATA restored{};
  CHECK(manager.ReadContentHeaderFile("mc4.sav", expected.xuid, expected.title_id,
                                      XContentType::kSavedGame,
                                      restored) == X_ERROR_FILE_NOT_FOUND);
  CHECK(manager.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).empty());
}

TEST_CASE("XAM interrupted saved-game overwrite restores the prior complete save",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
    WritePayload(temp.path(), "complete-before-interruption");
    REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
    REQUIRE(fs::remove_all(PackagePath(temp.path())) > 0);
    REQUIRE(fs::remove(HeaderPath(temp.path())));
    WritePayload(temp.path(), "partial-after-interruption");
    REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
    // Deliberately do not commit or roll back: this models a terminated title.
  }

  ContentManager restarted(nullptr, temp.path());
  const auto listed =
      restarted.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id);
  REQUIRE(listed.size() == 1);
  CHECK(ReadPayload(temp.path()) == "complete-before-interruption");
  CHECK_FALSE(fs::exists(HeaderPath(temp.path()).string() + ".rexpending"));
  CHECK_FALSE(fs::exists(PackagePath(temp.path()).string() + ".rexbackup"));
}

TEST_CASE("XAM interrupted brand-new save is not exposed as loadable content",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
    WritePayload(temp.path(), "partial-new-save");
    REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
  }

  ContentManager restarted(nullptr, temp.path());
  CHECK(
      restarted.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).empty());
  CHECK_FALSE(fs::exists(PackagePath(temp.path())));
  CHECK_FALSE(fs::exists(HeaderPath(temp.path())));
}

TEST_CASE("XAM saved-game recovery remains restart-safe while restoring a backup",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
    WritePayload(temp.path(), "complete-before-recovery");
    REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
    REQUIRE(fs::remove_all(PackagePath(temp.path())) > 0);
    WritePayload(temp.path(), "partial-replacement");

    // Model interruption after the package half of recovery was copied but
    // before the authoritative marker could be removed.
    REQUIRE(fs::remove_all(PackagePath(temp.path())) > 0);
    std::error_code error;
    fs::copy(PackagePath(temp.path()).string() + ".rexbackup", PackagePath(temp.path()),
             fs::copy_options::recursive, error);
    REQUIRE_FALSE(error);
  }

  ContentManager restarted(nullptr, temp.path());
  REQUIRE(
      restarted.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).size() ==
      1);
  CHECK(ReadPayload(temp.path()) == "complete-before-recovery");
  CHECK_FALSE(fs::exists(HeaderPath(temp.path()).string() + ".rexpending"));
}

TEST_CASE("XAM committed marker keeps replacement after stale-backup interruption",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
    WritePayload(temp.path(), "old");
    REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
    WritePayload(temp.path(), "new-after-commit-point");
    REQUIRE(fs::remove(HeaderPath(temp.path()).string() + ".rexpending"));
    // Backups deliberately remain, modelling interruption immediately after
    // the marker (the transaction commit point) was removed.
  }

  ContentManager restarted(nullptr, temp.path());
  REQUIRE(
      restarted.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).size() ==
      1);
  CHECK(ReadPayload(temp.path()) == "new-after-commit-point");
  CHECK_FALSE(fs::exists(PackagePath(temp.path()).string() + ".rexbackup"));
  CHECK_FALSE(fs::exists(HeaderPath(temp.path()).string() + ".rexbackup"));
}

TEST_CASE("XAM enumeration does not roll back an active saved-game transaction",
          "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  ContentManager manager(nullptr, temp.path());
  REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
  WritePayload(temp.path(), "before-active-write");
  REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
  CHECK(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_BUSY);
  WritePayload(temp.path(), "during-active-write");

  REQUIRE(
      manager.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).size() ==
      1);
  CHECK(ReadPayload(temp.path()) == "during-active-write");
  CHECK(fs::exists(HeaderPath(temp.path()).string() + ".rexpending"));
  REQUIRE(manager.CommitContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
  CHECK(ReadPayload(temp.path()) == "during-active-write");
}

TEST_CASE("XAM committed saved-game overwrite keeps the replacement", "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  ContentManager manager(nullptr, temp.path());
  REQUIRE(manager.WriteContentHeaderFile(expected.xuid, expected) == X_ERROR_SUCCESS);
  WritePayload(temp.path(), "old");
  REQUIRE(manager.BeginContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);
  WritePayload(temp.path(), "new");
  REQUIRE(manager.CommitContentTransaction(expected.xuid, expected) == X_ERROR_SUCCESS);

  ContentManager restarted(nullptr, temp.path());
  REQUIRE(
      restarted.ListContent(1, expected.xuid, XContentType::kSavedGame, expected.title_id).size() ==
      1);
  CHECK(ReadPayload(temp.path()) == "new");
}

TEST_CASE("XAM dummy HDD capacity reports full storage fail closed", "[system][xam][content]") {
  CHECK_FALSE(HasDummyHddCapacity(0, 0));
  CHECK_FALSE(HasDummyHddCapacity(1, 0));
  CHECK(HasDummyHddCapacity(0, 1));
  CHECK(HasDummyHddCapacity(1024, 1024));
  CHECK_FALSE(HasDummyHddCapacity(1025, 1024));
}
