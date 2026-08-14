/**
 ******************************************************************************
 * Regression coverage for durable XAM saved-game content metadata.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xam/content_manager.h>

namespace {

namespace fs = std::filesystem;
using rex::X_RESULT;
using rex::system::XContentType;
using rex::system::xam::ContentManager;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

class TempContentRoot {
 public:
  TempContentRoot() {
    static std::atomic<uint64_t> sequence{0};
    path_ =
        fs::temp_directory_path() / ("rex-xam-content-persistence-" + std::to_string(++sequence));
    std::error_code error;
    fs::remove_all(path_, error);
    REQUIRE(fs::create_directories(path_));
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

}  // namespace

TEST_CASE("XAM saved-game metadata survives manager restart", "[system][xam][content]") {
  TempContentRoot temp;
  const auto expected = MakeSavedGameData();
  const uint64_t xuid = expected.xuid;
  const uint32_t title_id = expected.title_id;

  {
    ContentManager manager(nullptr, temp.path());
    REQUIRE(manager.WriteContentHeaderFile(xuid, expected) == X_ERROR_SUCCESS);

    const auto package_path =
        temp.path() / "0123456789ABCDEF" / "545407F8" / "00000001" / "mc4.sav";
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
