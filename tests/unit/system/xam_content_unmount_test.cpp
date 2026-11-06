/**
 ******************************************************************************
 * M11-001 (KI-068): content unmount closes the host handles of guest file
 * objects, and the package removal moves delete-pending files aside.
 */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/content_log.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/platform.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xfile.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REXCVAR_DECLARE(bool, xam_content_close_host_handles_on_unmount);
REXCVAR_DECLARE(bool, xam_content_move_aside_on_delete_failure);

namespace {

namespace fs = std::filesystem;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::system::object_ref;
using rex::system::XContentType;
using rex::system::XFile;
using rex::system::xam::ContentManager;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

constexpr const char* kDevicePath = "\\Device\\Content\\77\\";

class TempDir {
 public:
  TempDir() {
    static std::atomic<uint64_t> sequence{0};
    std::random_device random;
    const auto parent = fs::temp_directory_path();
    for (uint32_t attempt = 0; attempt < 128; ++attempt) {
      const auto candidate = parent / ("rex-xam-content-unmount-" + std::to_string(random()) + "-" +
                                       std::to_string(random()) + "-" + std::to_string(++sequence));
      std::error_code error;
      if (fs::create_directory(candidate, error) && !error) {
        path_ = candidate;
        return;
      }
    }
    FAIL("Could not reserve a unique fixture directory");
  }
  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// Restores a bool cvar at scope exit.
class CvarScope {
 public:
  CvarScope(bool& storage, bool value) : storage_(storage), saved_(storage) { storage_ = value; }
  ~CvarScope() { storage_ = saved_; }

 private:
  bool& storage_;
  bool saved_;
};

void WriteFile(const fs::path& path, std::string_view payload) {
  fs::create_directories(path.parent_path());
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  REQUIRE(file.is_open());
  file << payload;
  REQUIRE(file.good());
}

#if REX_PLATFORM_WIN32
// True when another host handle is open on `path`: an exclusive open (no
// sharing) fails with a sharing violation. NTFS POSIX delete would hide the
// leak from a plain delete, so this is the observable stand-in for what Wine
// does with a delete-pending file.
bool HostHandleOpen(const fs::path& path) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_SHARING_VIOLATION;
  }
  CloseHandle(handle);
  return false;
}
#endif

struct GuestSaveFile {
  rex::filesystem::VirtualFileSystem vfs;
  object_ref<XFile> file;
  fs::path host_file;
};

// Mounts `package` like ContentPackage does (share-delete host device) and
// opens mc4.sav through the VFS as a guest file object.
void OpenGuestSaveFile(GuestSaveFile& out, const fs::path& package) {
  out.host_file = package / "mc4.sav";
  WriteFile(out.host_file, "first save");
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDevicePath, package, false,
                                                                  /*allow_share_delete=*/true);
  REQUIRE(device->Initialize());
  REQUIRE(out.vfs.RegisterDevice(std::move(device)));
  REQUIRE(out.vfs.RegisterSymbolicLink("save:", kDevicePath));
  rex::filesystem::File* vfs_file = nullptr;
  rex::filesystem::FileAction action{};
  REQUIRE(out.vfs.OpenFile(nullptr, "save:\\mc4.sav", rex::filesystem::FileDisposition::kOpen,
                           rex::filesystem::FileAccess::kGenericRead |
                               rex::filesystem::FileAccess::kGenericWrite,
                           false, true, &vfs_file, &action) == X_STATUS_SUCCESS);
  REQUIRE(vfs_file != nullptr);
  // No kernel state: the object is not in an object table, which the helper
  // under test does not need.
  out.file = object_ref<XFile>(new XFile(nullptr, vfs_file, true));
}

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

fs::path PackageRoot(const fs::path& root) {
  return root / "0123456789ABCDEF" / "545407F8" / "00000001";
}

std::vector<std::string> DeadFiles(const fs::path& dir) {
  std::vector<std::string> names;
  for (const auto& entry : fs::directory_iterator(dir)) {
    const auto name = entry.path().filename().string();
    if (name.find(".rexdead-") != std::string::npos) {
      names.push_back(name);
    }
  }
  return names;
}

struct FaultScope {
  explicit FaultScope(std::function<std::error_code(const fs::path&, int)> hook) {
    ContentManager::SetRemoveTreeFaultForTesting(std::move(hook));
  }
  ~FaultScope() {
    ContentManager::SetRemoveTreeFaultForTesting(nullptr);
    ContentManager::SetSessionTagForTesting("");
  }
};

std::error_code Win32Error(int code) {
  return std::error_code(code, std::system_category());
}

}  // namespace

TEST_CASE("XAM unmount force-closes the host handle of a still-referenced guest file",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  const fs::path package = temp.path() / "mc4.sav";
  GuestSaveFile guest;
  OpenGuestSaveFile(guest, package);
  CvarScope close_on(FLAGS_xam_content_close_host_handles_on_unmount_storage_(), true);

  // A second reference (an extra retain, a pending I/O, a completion port...)
  // survives the single handle release XamContentClose does today.
  object_ref<XFile> second(guest.file);
  second.reset();
  REQUIRE(guest.file->pointer_ref_count() == 1);
#if REX_PLATFORM_WIN32
  // The leak: the host handle of save #1 is still open after the "close".
  CHECK(HostHandleOpen(guest.host_file));
#endif

  std::vector<object_ref<XFile>> files;
  files.push_back(guest.file);
  size_t still_referenced = 0;
  CHECK(ContentManager::ForceCloseHostFiles(files, kDevicePath, &still_referenced) == 1);
  CHECK(still_referenced == 1);
  CHECK(guest.file->host_closed());
  CHECK(guest.file->file()->host_closed());
#if REX_PLATFORM_WIN32
  CHECK_FALSE(HostHandleOpen(guest.host_file));
#endif

  // Save #2's overwrite: the package can now really be removed.
  std::error_code error;
  fs::remove(guest.host_file, error);
  CHECK_FALSE(error);
  fs::remove_all(package, error);
  CHECK_FALSE(error);
  CHECK_FALSE(fs::exists(package));

  // Later guest I/O fails cleanly instead of touching a dead host handle.
  uint8_t buffer[4] = {};
  size_t transferred = 0;
  CHECK(guest.file->file()->ReadSync(buffer, 0, &transferred) == X_STATUS_FILE_CLOSED);
  CHECK(guest.file->file()->WriteSync(buffer, 0, &transferred) == X_STATUS_FILE_CLOSED);
  CHECK(guest.file->file()->SetLength(0) == X_STATUS_FILE_CLOSED);
  CHECK(guest.file->Rename("save:\\other.sav") == X_STATUS_FILE_CLOSED);

  // A second unmount pass finds nothing left to close.
  CHECK(ContentManager::ForceCloseHostFiles(files, kDevicePath, &still_referenced) == 0);
}

TEST_CASE("XAM unmount leaves files of other devices open",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  GuestSaveFile guest;
  OpenGuestSaveFile(guest, temp.path() / "mc4.sav");
  CvarScope close_on(FLAGS_xam_content_close_host_handles_on_unmount_storage_(), true);
  std::vector<object_ref<XFile>> files;
  files.push_back(guest.file);
  size_t still_referenced = 0;
  CHECK(ContentManager::ForceCloseHostFiles(files, "\\Device\\Content\\7\\", &still_referenced) ==
        0);
  CHECK(ContentManager::ForceCloseHostFiles(files, "", &still_referenced) == 0);
  CHECK_FALSE(guest.file->host_closed());
#if REX_PLATFORM_WIN32
  CHECK(HostHandleOpen(guest.host_file));
#endif
}

TEST_CASE("XAM unmount with the close cvar off keeps the host handle (old behaviour)",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  GuestSaveFile guest;
  OpenGuestSaveFile(guest, temp.path() / "mc4.sav");
  CvarScope close_off(FLAGS_xam_content_close_host_handles_on_unmount_storage_(), false);
  std::vector<object_ref<XFile>> files;
  files.push_back(guest.file);
  size_t still_referenced = 7;
  CHECK(ContentManager::ForceCloseHostFiles(files, kDevicePath, &still_referenced) == 0);
  CHECK(still_referenced == 0);
  CHECK_FALSE(guest.file->host_closed());
#if REX_PLATFORM_WIN32
  CHECK(HostHandleOpen(guest.host_file));
#endif
  uint8_t buffer[4] = {};
  size_t transferred = 0;
  CHECK(guest.file->file()->ReadSync(buffer, 0, &transferred) == X_STATUS_SUCCESS);
}

TEST_CASE("XAM DeleteContent moves files aside after a simulated Win32 145",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  CvarScope move_on(FLAGS_xam_content_move_aside_on_delete_failure_storage_(), true);
  ContentManager::SetSessionTagForTesting("sessiona");
  int attempts = 0;
  FaultScope fault([&attempts](const fs::path&, int attempt) {
    ++attempts;
    return attempt == 0 ? Win32Error(145) : std::error_code();
  });
  const auto data = MakeSavedGameData();
  WriteFile(PackageRoot(temp.path()) / "mc4.sav" / "mc4.sav", "first save");
  WriteFile(PackageRoot(temp.path()) / "mc4.sav" / "sub" / "extra.bin", "x");

  ContentManager manager(nullptr, temp.path());
  CHECK(manager.DeleteContent(data.xuid, data) == X_ERROR_SUCCESS);
  CHECK(attempts == 2);
  CHECK_FALSE(fs::exists(PackageRoot(temp.path()) / "mc4.sav"));
  const auto dead = DeadFiles(PackageRoot(temp.path()));
  REQUIRE(dead.size() == 2);
  for (const auto& name : dead) {
    CHECK(name.find(".rexdead-sessiona-") != std::string::npos);
  }
  // Enumeration ignores the moved files (directories only).
  CHECK(manager.ListContent(1, data.xuid, XContentType::kSavedGame, data.title_id).empty());
  // Nothing outside the package parent was touched.
  CHECK(fs::exists(PackageRoot(temp.path())));
}

TEST_CASE("XAM sweep deletes earlier-session dead files only",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  FaultScope cleanup(nullptr);
  const auto root = PackageRoot(temp.path());
  WriteFile(root / "mc4.sav.rexdead-oldsession-1", "old");
  WriteFile(root / "mc4.sav.rexdead-current-2", "pending");
  WriteFile(root / "unrelated.bin", "keep");
  fs::create_directories(root / "dir.rexdead-oldsession-3");
  ContentManager::SetSessionTagForTesting("current");

  ContentManager manager(nullptr, temp.path());
  CHECK(manager.SweepDeadFiles(root) == 1);
  CHECK_FALSE(fs::exists(root / "mc4.sav.rexdead-oldsession-1"));
  CHECK(fs::exists(root / "mc4.sav.rexdead-current-2"));
  CHECK(fs::exists(root / "unrelated.bin"));
  CHECK(fs::exists(root / "dir.rexdead-oldsession-3"));

  // Enumeration sweeps too.
  WriteFile(root / "mc4.sav.rexdead-older-4", "old");
  const auto data = MakeSavedGameData();
  manager.ListContent(1, data.xuid, XContentType::kSavedGame, data.title_id);
  CHECK_FALSE(fs::exists(root / "mc4.sav.rexdead-older-4"));
}

TEST_CASE("XAM DeleteContent with the move-aside cvar off fails as before",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  CvarScope move_off(FLAGS_xam_content_move_aside_on_delete_failure_storage_(), false);
  int attempts = 0;
  FaultScope fault([&attempts](const fs::path&, int) {
    ++attempts;
    return Win32Error(145);
  });
  const auto data = MakeSavedGameData();
  WriteFile(PackageRoot(temp.path()) / "mc4.sav" / "mc4.sav", "first save");

  ContentManager manager(nullptr, temp.path());
  CHECK(manager.DeleteContent(data.xuid, data) == X_ERROR_ACCESS_DENIED);
  CHECK(attempts == 1);
  CHECK(fs::exists(PackageRoot(temp.path()) / "mc4.sav" / "mc4.sav"));
  CHECK(DeadFiles(PackageRoot(temp.path())).empty());
}

TEST_CASE("XAM move-aside applies to 145 and 5 only and reports a failed retry",
          "[system][xam][content][content-unmount]") {
  TempDir temp;
  CvarScope move_on(FLAGS_xam_content_move_aside_on_delete_failure_storage_(), true);
  const auto data = MakeSavedGameData();
  WriteFile(PackageRoot(temp.path()) / "mc4.sav" / "mc4.sav", "first save");
  ContentManager manager(nullptr, temp.path());
  {
    // A sharing violation (32) is not what a delete-pending file reports.
    FaultScope fault([](const fs::path&, int) { return Win32Error(32); });
    CHECK(manager.DeleteContent(data.xuid, data) == X_ERROR_ACCESS_DENIED);
    CHECK(DeadFiles(PackageRoot(temp.path())).empty());
  }
  {
    // Access denied on both attempts: the files were moved, the error stays.
    FaultScope fault([](const fs::path&, int) { return Win32Error(5); });
    CHECK(manager.DeleteContent(data.xuid, data) == X_ERROR_ACCESS_DENIED);
    CHECK(DeadFiles(PackageRoot(temp.path())).size() == 1);
  }
}

TEST_CASE("XAM saved-game transaction stages reach Commit", "[system][xam][content]") {
  TempDir temp;
  const auto data = MakeSavedGameData();
  WriteFile(PackageRoot(temp.path()) / "mc4.sav" / "mc4.sav", "first save");
  ContentManager manager(nullptr, temp.path());
  REQUIRE(manager.BeginContentTransaction(data.xuid, data) == X_ERROR_SUCCESS);
  manager.NoteTransactionStage(data.xuid, data, "delete-existing,create-package,write-header");
  CHECK(manager.CommitContentTransaction(data.xuid, data) == X_ERROR_SUCCESS);
  // A Commit without a recorded Begin still succeeds (stages=commit).
  CHECK(manager.CommitContentTransaction(data.xuid, data) == X_ERROR_SUCCESS);
  // Unknown transactions ignore stage notes.
  manager.NoteTransactionStage(data.xuid, data, "ignored");
}

REXCVAR_DECLARE(bool, xam_content_stable_creation_time);

namespace {

struct ContentMount {
  rex::filesystem::VirtualFileSystem vfs;
  explicit ContentMount(const fs::path& package) {
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(
        "\\Device\\Content\\78\\", package, false, /*allow_share_delete=*/true);
    REQUIRE(device->Initialize());
    REQUIRE(vfs.RegisterDevice(std::move(device)));
    REQUIRE(vfs.RegisterSymbolicLink("save:", "\\Device\\Content\\78\\"));
  }
};

#if REX_PLATFORM_WIN32
// Stand-in for Wine on ext4, where the reported creation time moves when the
// file is written: rewrite the host creation time behind the SDK's back.
uint64_t ShiftHostCreationTime(const fs::path& path) {
  HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES | FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(handle != INVALID_HANDLE_VALUE);
  FILETIME created{}, accessed{}, written{};
  REQUIRE(GetFileTime(handle, &created, &accessed, &written));
  ULARGE_INTEGER value;
  value.LowPart = created.dwLowDateTime;
  value.HighPart = created.dwHighDateTime;
  value.QuadPart += UINT64_C(36000000000);  // one hour later
  created.dwLowDateTime = value.LowPart;
  created.dwHighDateTime = value.HighPart;
  REQUIRE(SetFileTime(handle, &created, nullptr, nullptr));
  CloseHandle(handle);
  return value.QuadPart;
}
#endif

}  // namespace

TEST_CASE("XAM content creation time stays constant across writes and remounts",
          "[system][xam][content][content-ctime]") {
  TempDir temp;
  const fs::path package = temp.path() / "mc4.sav";
  fs::create_directories(package);
  CvarScope stable_on(FLAGS_xam_content_stable_creation_time_storage_(), true);

  uint64_t created = 0;
  {
    ContentMount mount(package);
    rex::filesystem::File* file = nullptr;
    rex::filesystem::FileAction action{};
    REQUIRE(mount.vfs.OpenFile(nullptr, "save:\\mc4.sav", rex::filesystem::FileDisposition::kCreate,
                               rex::filesystem::FileAccess::kGenericRead |
                                   rex::filesystem::FileAccess::kGenericWrite,
                               false, true, &file, &action) == X_STATUS_SUCCESS);
    REQUIRE(file != nullptr);
    created = file->entry()->create_timestamp();
    const uint8_t payload[] = {1, 2, 3, 4};
    size_t written = 0;
    CHECK(file->WriteSync(payload, 0, &written) == X_STATUS_SUCCESS);
    file->Destroy();
  }
#if REX_PLATFORM_WIN32
  const uint64_t moved = ShiftHostCreationTime(package / "mc4.sav");
  rex::filesystem::FileInfo info;
  REQUIRE(rex::filesystem::GetInfo(package / "mc4.sav", &info));
  REQUIRE(info.create_timestamp == moved);
  REQUIRE(moved != created);
#endif
  {
    // Save #2's check runs on a fresh mount whose entries come from a listing.
    ContentMount mount(package);
    auto* entry = mount.vfs.ResolvePath("save:\\mc4.sav");
    REQUIRE(entry != nullptr);
    CHECK(entry->create_timestamp() == created);
  }
  {
    // A guest-set creation time is what later mounts report.
    ContentMount mount(package);
    auto* entry = mount.vfs.ResolvePath("save:\\mc4.sav");
    REQUIRE(entry != nullptr);
    REQUIRE(entry->SetCreateTimestamp(created + 7));
  }
  {
    ContentMount mount(package);
    CHECK(mount.vfs.ResolvePath("save:\\mc4.sav")->create_timestamp() == created + 7);
  }
#if REX_PLATFORM_WIN32
  {
    // Cvar off: the host value is reported as before (the Wine symptom).
    CvarScope stable_off(FLAGS_xam_content_stable_creation_time_storage_(), false);
    ContentMount mount(package);
    CHECK(mount.vfs.ResolvePath("save:\\mc4.sav")->create_timestamp() == moved);
  }
#endif
}

TEST_CASE("XAM stable creation time table remembers the first value per key",
          "[system][xam][content][content-ctime]") {
  bool remembered = true;
  CHECK(rex::content_log::StableCreationTime("k-ctime-test", 100, &remembered) == 100);
  CHECK_FALSE(remembered);
  CHECK(rex::content_log::StableCreationTime("k-ctime-test", 200, &remembered) == 100);
  CHECK(remembered);
  rex::content_log::SetStableCreationTime("k-ctime-test", 300);
  CHECK(rex::content_log::StableCreationTime("k-ctime-test", 400, &remembered) == 300);
}
