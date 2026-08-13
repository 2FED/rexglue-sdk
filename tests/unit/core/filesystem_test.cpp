/**
 * @file        filesystem_test.cpp
 * @brief       Host filesystem path conversion tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/memory/mapped_memory.h>

namespace {

using rex::X_STATUS;

class ScopedTestDirectory {
 public:
  ScopedTestDirectory() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ =
        std::filesystem::temp_directory_path() / ("rexglue-read-only-vfs-" + std::to_string(nonce));
    std::filesystem::create_directories(path_);
  }

  ~ScopedTestDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

TEST_CASE("filesystem paths round-trip non-ASCII text through UTF-8", "[filesystem]") {
  constexpr std::u16string_view kUtf16 = u"MCLA-\u0414\u0430\u043d\u0456";
  constexpr std::string_view kUtf8 = "MCLA-\xD0\x94\xD0\xB0\xD0\xBD\xD1\x96";

  const auto path = rex::to_path(kUtf16);
  CHECK(rex::path_to_utf8(path) == kUtf8);
  CHECK(rex::to_path(rex::path_to_utf8(path)) == path);
}

TEST_CASE("filesystem opens a non-ASCII UTF-8 path without mojibake", "[filesystem]") {
  ScopedTestDirectory fixture;
  constexpr std::string_view kUtf8Name =
      "\xD0\xB6\xD1\x83\xD1\x80\xD0\xBD\xD0\xB0\xD0\xBB-\xC3\xA9.log";
  const auto expected_path = fixture.path() / rex::to_path(kUtf8Name);

  FILE* file = rex::filesystem::OpenFile(expected_path, "wb");
  REQUIRE(file != nullptr);
  CHECK(std::fputs("locale-path", file) >= 0);
  CHECK(std::fclose(file) == 0);

  CHECK(std::filesystem::is_regular_file(expected_path));
  CHECK(rex::path_to_utf8(expected_path.filename()) == kUtf8Name);
  CHECK(std::filesystem::directory_iterator(fixture.path())->path().filename() ==
        expected_path.filename());
}

TEST_CASE("read-only host VFS rejects traversal and every mutation path", "[filesystem][vfs]") {
  ScopedTestDirectory fixture;
  const auto original_path = fixture.path() / "default.xex";
  {
    std::ofstream original(original_path, std::ios::binary);
    REQUIRE(original.good());
    original << "XEX2";
  }

  rex::filesystem::VirtualFileSystem vfs;
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Harddisk0\\Partition1",
                                                                  fixture.path(), true);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("game:", "\\Device\\Harddisk0\\Partition1"));
  REQUIRE(vfs.RegisterSymbolicLink("d:", "\\Device\\Harddisk0\\Partition1"));

  auto* game_entry = vfs.ResolvePath("game:\\default.xex");
  REQUIRE(game_entry != nullptr);
  CHECK(vfs.ResolvePath("d:\\default.xex") == game_entry);
  CHECK(vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\default.xex") == game_entry);
  CHECK(vfs.ResolvePath("game:\\..\\default.xex") == nullptr);
  CHECK(vfs.ResolvePath("\\Device\\Harddisk0\\Partition1\\..\\Partition1\\default.xex") == nullptr);

  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action{};
  CHECK(vfs.OpenFile(nullptr, "game:\\default.xex", rex::filesystem::FileDisposition::kOpen,
                     rex::filesystem::FileAccess::kGenericRead, false, true, &file,
                     &action) == X_STATUS_SUCCESS);
  REQUIRE(file != nullptr);
  file->Destroy();
  file = nullptr;

  CHECK(vfs.OpenFile(nullptr, "game:\\default.xex", rex::filesystem::FileDisposition::kOpen,
                     rex::filesystem::FileAccess::kGenericWrite, false, true, &file,
                     &action) == X_STATUS_ACCESS_DENIED);
  CHECK(file == nullptr);
  CHECK(vfs.OpenFile(nullptr, "game:\\created.bin", rex::filesystem::FileDisposition::kCreate,
                     rex::filesystem::FileAccess::kGenericWrite, false, true, &file,
                     &action) == X_STATUS_ACCESS_DENIED);
  CHECK(file == nullptr);
  CHECK_FALSE(std::filesystem::exists(fixture.path() / "created.bin"));

  CHECK_FALSE(vfs.DeletePath("game:\\default.xex"));
  CHECK(game_entry->Rename(rex::to_path("game:\\renamed.xex")) == X_STATUS_ACCESS_DENIED);
  CHECK(std::filesystem::exists(original_path));
  CHECK_FALSE(std::filesystem::exists(fixture.path() / "renamed.xex"));

  auto read_mapping = game_entry->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
  REQUIRE(read_mapping != nullptr);
  CHECK(read_mapping->size() == 4);
  read_mapping->Close();
  CHECK(game_entry->OpenMapped(rex::memory::MappedMemory::Mode::kReadWrite) == nullptr);
}

TEST_CASE("VFS opens content symlink roots with trailing separators", "[filesystem][vfs]") {
  ScopedTestDirectory fixture;
  const auto save_path = fixture.path() / "mc4.sav";
  {
    std::ofstream save(save_path, std::ios::binary);
    REQUIRE(save.good());
    save << "SAVE";
  }

  rex::filesystem::VirtualFileSystem vfs;
  auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\Device\\Content\\1\\",
                                                                  fixture.path(), false);
  REQUIRE(device->Initialize());
  REQUIRE(vfs.RegisterDevice(std::move(device)));
  REQUIRE(vfs.RegisterSymbolicLink("save0:", "\\Device\\Content\\1\\"));

  auto* root = vfs.ResolvePath("save0:\\");
  REQUIRE(root != nullptr);
  CHECK(root->name().empty());
  CHECK(vfs.ResolvePath("save0:\\mc4.sav") != nullptr);

  rex::filesystem::File* file = nullptr;
  rex::filesystem::FileAction action{};
  CHECK(vfs.OpenFile(nullptr, "save0:\\mc4.sav", rex::filesystem::FileDisposition::kOpen,
                     rex::filesystem::FileAccess::kGenericRead, false, true, &file,
                     &action) == X_STATUS_SUCCESS);
  REQUIRE(file != nullptr);
  file->Destroy();
}
