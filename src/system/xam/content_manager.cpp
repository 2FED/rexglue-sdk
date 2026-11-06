/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <queue>
#include <random>
#include <string>

#include <fmt/format.h>

#include <rex/content_log.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xfile.h>
#include <rex/system/xobject.h>

REXCVAR_DEFINE_BOOL(xam_content_close_host_handles_on_unmount, true, "XAM",
                    "On content unmount (XamContentClose, the overwrite path), close the host "
                    "handle of every guest file object on that package even if guest references "
                    "remain; later I/O on such an object returns STATUS_FILE_CLOSED (M11-001)");
REXCVAR_DEFINE_BOOL(xam_content_move_aside_on_delete_failure, rex::content_log::RunningUnderWine(),
                    "XAM",
                    "When removing a content package fails with Win32 145 or 5, rename the "
                    "remaining files to *.rexdead-* beside the package and retry; default true "
                    "under Wine/Proton only (M11-001)");

namespace rex {
namespace system {
namespace xam {

static const char* kThumbnailFileName = "__thumbnail.png";


static const char* kGameContentHeaderDirName = "Headers";
static constexpr std::string_view kTransactionBackupSuffix = ".rexbackup";
static constexpr std::string_view kTransactionMarkerSuffix = ".rexpending";

static int content_device_id_ = 0;

static constexpr std::string_view kDeadFileMarker = ".rexdead-";

namespace cl = rex::content_log;
using cl::Kind;

// KI-065: one paced WARN line per failed step of the content layer. Names the
// package by its file name and content type, never by its host path.
static void LogContentFailure(Kind kind, std::string_view op, std::string_view stage,
                              const XCONTENT_AGGREGATE_DATA& data, X_RESULT result,
                              uint32_t system_error) {
  cl::Warn(kind, "{} failed: stage={} content='{}' type={:08X} result={:#x} win32={} ({})", op,
           stage, data.file_name(), uint32_t(XContentType(data.content_type)), result, system_error,
           cl::SystemErrorText(system_error));
}

static void LogContentFailure(Kind kind, std::string_view op, std::string_view stage,
                              const XCONTENT_AGGREGATE_DATA& data, X_RESULT result,
                              const std::error_code& error) {
  LogContentFailure(kind, op, stage, data, result, uint32_t(error.value()));
}

namespace {

std::mutex g_test_hook_mutex;
std::function<std::error_code(const std::filesystem::path&, int)> g_remove_tree_fault;
std::string g_session_tag_override;

std::error_code RemoveFaultFor(const std::filesystem::path& path, int attempt) {
  std::lock_guard<std::mutex> lock(g_test_hook_mutex);
  if (g_remove_tree_fault) {
    return g_remove_tree_fault(path, attempt);
  }
  return {};
}

// Win32 145 (ERROR_DIR_NOT_EMPTY) and 5 (ERROR_ACCESS_DENIED): what Wine
// reports while a file of the tree is delete-pending behind an open handle.
// The raw Win32 value is compared on purpose: the portable conditions are too
// wide (MSVC maps sharing violation 32 to errc::permission_denied as well).
bool IsMoveAsideError(const std::error_code& error) {
  if (error.category() != std::system_category()) {
    return false;
  }
  const int value = error.value();
  return value == 145 || value == 5;
}

}  // namespace

void ContentManager::SetRemoveTreeFaultForTesting(
    std::function<std::error_code(const std::filesystem::path&, int attempt)> hook) {
  std::lock_guard<std::mutex> lock(g_test_hook_mutex);
  g_remove_tree_fault = std::move(hook);
}

void ContentManager::SetSessionTagForTesting(std::string tag) {
  std::lock_guard<std::mutex> lock(g_test_hook_mutex);
  g_session_tag_override = std::move(tag);
}

std::string ContentManager::SessionTag() {
  {
    std::lock_guard<std::mutex> lock(g_test_hook_mutex);
    if (!g_session_tag_override.empty()) {
      return g_session_tag_override;
    }
  }
  static const std::string tag = [] {
    std::random_device random;
    const uint64_t value = (uint64_t(random()) << 32) ^ uint64_t(random()) ^
                           uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    return fmt::format("{:016x}", value);
  }();
  return tag;
}

std::error_code ContentManager::RemovePackageTree(const std::filesystem::path& package_path,
                                                  const XCONTENT_AGGREGATE_DATA& data,
                                                  std::string_view op, uintmax_t* removed) {
  namespace fs = std::filesystem;
  uintmax_t count = 0;
  std::error_code error = RemoveFaultFor(package_path, 0);
  if (!error) {
    count = fs::remove_all(package_path, error);
  }
  if (!error) {
    if (removed) {
      *removed = count;
    }
    return {};
  }
  if (!IsMoveAsideError(error) || !REXCVAR_GET(xam_content_move_aside_on_delete_failure)) {
    return error;
  }

  // Wine keeps a delete-pending file in its directory until its last handle
  // closes. Move every remaining file out of the package tree (into the
  // package's parent, where enumeration looks at directories only) and retry.
  cl::Info(Kind::kMoveAside,
           "move-aside: {} remove failed with win32={} ({}); moving remaining files aside: "
           "content='{}' type={:08X}",
           op, error.value(), cl::SystemErrorText(uint32_t(error.value())), data.file_name(),
           uint32_t(XContentType(data.content_type)));
  const fs::path parent = package_path.parent_path();
  const std::string session = SessionTag();
  std::vector<fs::path> files;
  std::error_code walk_error;
  for (fs::recursive_directory_iterator it(package_path, fs::directory_options::none, walk_error),
       end;
       !walk_error && it != end; it.increment(walk_error)) {
    std::error_code type_error;
    const auto status = it->symlink_status(type_error);
    if (!type_error && !fs::is_directory(status)) {
      files.push_back(it->path());
    }
  }
  if (walk_error) {
    cl::Warn(Kind::kMoveAside, "move-aside: listing the package failed: content='{}' win32={} ({})",
             data.file_name(), walk_error.value(),
             cl::SystemErrorText(uint32_t(walk_error.value())));
  }
  static std::atomic<uint32_t> sequence{0};
  size_t moved = 0;
  for (const auto& file : files) {
    const fs::path target =
        parent / rex::to_path(fmt::format("{}{}{}-{}", rex::path_to_utf8(file.filename()),
                                          kDeadFileMarker, session, ++sequence));
    std::error_code rename_error;
    fs::rename(file, target, rename_error);
    if (rename_error) {
      cl::Warn(Kind::kMoveAside, "move-aside: rename failed: content='{}' file='{}' win32={} ({})",
               data.file_name(), rex::path_to_utf8(file.filename()), rename_error.value(),
               cl::SystemErrorText(uint32_t(rename_error.value())));
    } else {
      ++moved;
    }
  }
  error = RemoveFaultFor(package_path, 1);
  if (!error) {
    count = fs::remove_all(package_path, error);
  }
  if (error) {
    cl::Warn(Kind::kMoveAside,
             "move-aside: {} retry failed: content='{}' moved={} of {} win32={} ({})", op,
             data.file_name(), moved, files.size(), error.value(),
             cl::SystemErrorText(uint32_t(error.value())));
    return error;
  }
  cl::Info(Kind::kMoveAside, "move-aside: {} retry succeeded: content='{}' moved={} of {}", op,
           data.file_name(), moved, files.size());
  if (removed) {
    // The moved files count as removed from the package.
    *removed = count + moved;
  }
  return {};
}

size_t ContentManager::SweepDeadFiles(const std::filesystem::path& package_root) {
  namespace fs = std::filesystem;
  const std::string current = std::string(kDeadFileMarker) + SessionTag() + "-";
  size_t deleted = 0;
  size_t kept = 0;
  std::error_code error;
  for (fs::directory_iterator it(package_root, error), end; !error && it != end;
       it.increment(error)) {
    const std::string name = rex::path_to_utf8(it->path().filename());
    if (name.find(kDeadFileMarker) == std::string::npos ||
        name.find(current) != std::string::npos) {
      continue;
    }
    std::error_code type_error;
    const auto status = it->symlink_status(type_error);
    if (type_error || fs::is_directory(status)) {
      continue;
    }
    std::error_code remove_error;
    if (fs::remove(it->path(), remove_error) && !remove_error) {
      ++deleted;
    } else {
      ++kept;
    }
  }
  if (deleted || kept) {
    cl::Info(Kind::kMoveAside, "move-aside: swept {} earlier-session file(s), {} still in use",
             deleted, kept);
  }
  return deleted;
}

void ContentManager::NoteTransactionStage(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                          std::string_view stage) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto marker_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  marker_path += kTransactionMarkerSuffix;
  auto it = transaction_traces_.find(marker_path);
  if (it == transaction_traces_.end()) {
    return;
  }
  it->second.stages += ',';
  it->second.stages += stage;
}

ContentPackage::ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : kernel_state_(kernel_state), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
  content_data_ = data;

  auto fs = kernel_state_->file_system();
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(device_path_, package_path, false,
                                                                  /*allow_share_delete=*/true);
  device->Initialize();
  fs->RegisterDevice(std::move(device));
  fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  auto file = rex::filesystem::OpenFile(header_path, "rb");
  if (!file) {
    return;
  }

  auto file_size = std::filesystem::file_size(header_path);
  if (file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  fread(&license_, 1, sizeof(license_), file);
  fclose(file);
}

ContentManager::ContentManager(KernelState* kernel_state, const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {}

ContentManager::~ContentManager() {
  // Transactions still open at teardown (a terminated title) no longer count
  // as active; their markers stay on disk for the next start's recovery.
  if (!active_content_transactions_.empty()) {
    cl::NoteTransactionsEnded(uint32_t(active_content_transactions_.size()));
  }
}

std::filesystem::path ContentManager::ResolvePackageRoot(uint64_t xuid, XContentType content_type,
                                                         uint32_t title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));

  // Package root path:
  // content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(uint64_t xuid,
                                                         const XCONTENT_AGGREGATE_DATA& data) {
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  // DLCs are stored in common directory
  if (data.content_type == XContentType::kMarketplaceContent) {
    used_xuid = 0;
  }

  // Content path:
  // content_root/xuid/title_id/content_type/data_file_name/
  auto package_root = ResolvePackageRoot(used_xuid, data.content_type, data.title_id);
  return package_root / rex::to_path(data.file_name());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               uint64_t xuid, uint32_t title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Header root path:
  // content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

X_RESULT ContentManager::RecoverContentTransaction(uint64_t xuid,
                                                   const XCONTENT_AGGREGATE_DATA& data) {
  cl::BusyScope busy;
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto package_backup_path = package_path;
  package_backup_path += kTransactionBackupSuffix;
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto header_backup_path = header_path;
  header_backup_path += kTransactionBackupSuffix;
  auto marker_path = header_path;
  marker_path += kTransactionMarkerSuffix;

  std::error_code error;
  if (active_content_transactions_.contains(marker_path)) {
    return X_ERROR_SUCCESS;
  }
  if (!std::filesystem::exists(marker_path)) {
    // Backups without a marker were never made authoritative. They may be
    // remnants of an interruption while the backup itself was being copied.
    std::filesystem::remove_all(package_backup_path, error);
    error.clear();
    std::filesystem::remove(header_backup_path, error);
    return X_ERROR_SUCCESS;
  }

  SweepDeadFiles(package_path.parent_path());
  error = RemovePackageTree(package_path, data, "Recover", nullptr);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Recover", "remove-package", data, X_ERROR_ACCESS_DENIED,
                      error);
    return X_ERROR_ACCESS_DENIED;
  }
  error.clear();
  std::filesystem::remove(header_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Recover", "remove-header", data, X_ERROR_ACCESS_DENIED,
                      error);
    return X_ERROR_ACCESS_DENIED;
  }

  // Keep the backups until the marker is removed so recovery is idempotent if
  // this process is itself interrupted while restoring the prior save.
  if (std::filesystem::exists(package_backup_path)) {
    error.clear();
    std::filesystem::copy(package_backup_path, package_path,
                          std::filesystem::copy_options::recursive, error);
    if (error) {
      LogContentFailure(Kind::kTransaction, "Recover", "restore-package", data,
                        X_ERROR_ACCESS_DENIED, error);
      return X_ERROR_ACCESS_DENIED;
    }
  }
  if (std::filesystem::exists(header_backup_path)) {
    error.clear();
    std::filesystem::copy_file(header_backup_path, header_path,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      LogContentFailure(Kind::kTransaction, "Recover", "restore-header", data,
                        X_ERROR_ACCESS_DENIED, error);
      return X_ERROR_ACCESS_DENIED;
    }
  }
  error.clear();
  std::filesystem::remove(marker_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Recover", "remove-marker", data, X_ERROR_ACCESS_DENIED,
                      error);
    return X_ERROR_ACCESS_DENIED;
  }
  error.clear();
  std::filesystem::remove_all(package_backup_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Recover", "remove-package-backup", data,
                      X_ERROR_ACCESS_DENIED, error);
    return X_ERROR_ACCESS_DENIED;
  }
  error.clear();
  std::filesystem::remove(header_backup_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Recover", "remove-header-backup", data,
                      X_ERROR_ACCESS_DENIED, error);
    return X_ERROR_ACCESS_DENIED;
  }
  cl::Warn(Kind::kTransaction,
           "Recover restored the previous save after an interrupted transaction: content='{}' "
           "type={:08X}",
           data.file_name(), uint32_t(XContentType(data.content_type)));
  return X_ERROR_SUCCESS;
}

void ContentManager::RecoverContentTransactions(uint64_t xuid, XContentType content_type,
                                                uint32_t title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  auto header_root = root_path_ / fmt::format("{:016X}", xuid) / fmt::format("{:08X}", title_id) /
                     kGameContentHeaderDirName / fmt::format("{:08X}", uint32_t(content_type));
  for (const auto& file_info : rex::filesystem::ListFiles(header_root)) {
    auto name = rex::path_to_utf8(file_info.name);
    const std::string suffix = std::string(".header") + std::string(kTransactionMarkerSuffix);
    if (!name.ends_with(suffix)) {
      continue;
    }
    XCONTENT_AGGREGATE_DATA data{};
    data.device_id = 1;
    data.content_type = content_type;
    data.xuid = xuid;
    data.title_id = title_id;
    data.set_file_name(name.substr(0, name.size() - suffix.size()));
    RecoverContentTransaction(xuid, data);
  }
}

X_RESULT ContentManager::BeginContentTransaction(uint64_t xuid,
                                                 const XCONTENT_AGGREGATE_DATA& data) {
  cl::BusyScope busy;
  auto recovery_result = RecoverContentTransaction(xuid, data);
  if (recovery_result != X_ERROR_SUCCESS) {
    LogContentFailure(Kind::kTransaction, "Begin", "recover", data, recovery_result, 0u);
    return recovery_result;
  }

  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto package_backup_path = package_path;
  package_backup_path += kTransactionBackupSuffix;
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto header_backup_path = header_path;
  header_backup_path += kTransactionBackupSuffix;
  auto marker_path = header_path;
  marker_path += kTransactionMarkerSuffix;

  if (active_content_transactions_.contains(marker_path)) {
    LogContentFailure(Kind::kTransaction, "Begin", "already-active", data, X_ERROR_BUSY, 0u);
    return X_ERROR_BUSY;
  }

  std::error_code error;
  std::filesystem::remove_all(package_backup_path, error);
  error.clear();
  std::filesystem::remove(header_backup_path, error);
  const bool had_package = std::filesystem::exists(package_path);
  if (had_package) {
    error.clear();
    std::filesystem::copy(package_path, package_backup_path,
                          std::filesystem::copy_options::recursive, error);
    if (error) {
      LogContentFailure(Kind::kTransaction, "Begin", "backup-package", data, X_ERROR_ACCESS_DENIED,
                        error);
      std::filesystem::remove_all(package_backup_path, error);
      return X_ERROR_ACCESS_DENIED;
    }
  }
  if (std::filesystem::exists(header_path)) {
    error.clear();
    std::filesystem::copy_file(header_path, header_backup_path,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      LogContentFailure(Kind::kTransaction, "Begin", "backup-header", data, X_ERROR_ACCESS_DENIED,
                        error);
      std::filesystem::remove_all(package_backup_path, error);
      std::filesystem::remove(header_backup_path, error);
      return X_ERROR_ACCESS_DENIED;
    }
  }
  error.clear();
  std::filesystem::create_directories(marker_path.parent_path(), error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Begin", "create-marker-directory", data,
                      X_ERROR_ACCESS_DENIED, error);
  } else if (!rex::filesystem::CreateEmptyFile(marker_path)) {
    LogContentFailure(Kind::kTransaction, "Begin", "create-marker", data, X_ERROR_ACCESS_DENIED,
                      cl::LastSystemError());
    error = std::make_error_code(std::errc::io_error);
  }
  if (error) {
    std::filesystem::remove_all(package_backup_path, error);
    std::filesystem::remove(header_backup_path, error);
    return X_ERROR_ACCESS_DENIED;
  }
  if (active_content_transactions_.insert(marker_path).second) {
    cl::NoteTransactionBegun();
  }
  transaction_traces_[marker_path] =
      TransactionTrace{std::chrono::steady_clock::now(), had_package ? "begin,backup" : "begin"};
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CommitContentTransaction(uint64_t xuid,
                                                  const XCONTENT_AGGREGATE_DATA& data) {
  cl::BusyScope busy;
  auto global_lock = global_critical_region_.Acquire();
  auto package_backup_path = ResolvePackagePath(xuid, data);
  package_backup_path += kTransactionBackupSuffix;
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto header_backup_path = header_path;
  header_backup_path += kTransactionBackupSuffix;
  auto marker_path = header_path;
  marker_path += kTransactionMarkerSuffix;
  // Removing the marker commits the replacement. Do this before deleting the
  // backups so an interruption can never leave a marker without recovery data.
  const auto commit_start = std::chrono::steady_clock::now();
  std::string stages = "commit";
  int64_t transaction_ms = -1;
  if (auto trace = transaction_traces_.find(marker_path); trace != transaction_traces_.end()) {
    stages = trace->second.stages + ",commit";
    transaction_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(commit_start - trace->second.begin)
            .count();
    transaction_traces_.erase(trace);
  }
  std::error_code error;
  std::filesystem::remove(marker_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Commit", "remove-marker", data, X_ERROR_ACCESS_DENIED,
                      error);
    return X_ERROR_ACCESS_DENIED;
  }
  if (active_content_transactions_.erase(marker_path)) {
    cl::NoteTransactionsEnded();
  }
  error.clear();
  std::filesystem::remove_all(package_backup_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Commit", "remove-package-backup", data,
                      X_ERROR_ACCESS_DENIED, error);
    return X_ERROR_ACCESS_DENIED;
  }
  error.clear();
  std::filesystem::remove(header_backup_path, error);
  if (error) {
    LogContentFailure(Kind::kTransaction, "Commit", "remove-header-backup", data,
                      X_ERROR_ACCESS_DENIED, error);
    return X_ERROR_ACCESS_DENIED;
  }
  const int64_t commit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - commit_start)
                                .count();
  cl::Info(Kind::kSaveOk, "save ok: content='{}' type={:08X} stages={} ms={} commit_ms={}",
           data.file_name(), uint32_t(XContentType(data.content_type)), stages, transaction_ms,
           commit_ms);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::RollbackContentTransaction(uint64_t xuid,
                                                    const XCONTENT_AGGREGATE_DATA& data) {
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto marker_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  marker_path += kTransactionMarkerSuffix;
  cl::BusyScope busy;
  {
    auto global_lock = global_critical_region_.Acquire();
    if (active_content_transactions_.erase(marker_path)) {
      cl::NoteTransactionsEnded();
    }
    transaction_traces_.erase(marker_path);
  }
  const X_RESULT result = RecoverContentTransaction(xuid, data);
  if (result != X_ERROR_SUCCESS) {
    LogContentFailure(Kind::kTransaction, "Rollback", "recover", data, result, 0u);
  }
  return result;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(uint32_t device_id, uint64_t xuid,
                                                                 XContentType content_type,
                                                                 uint32_t title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  RecoverContentTransactions(xuid, content_type, title_id);

  // Search path:
  // content_root/xuid/title_id/type_name/*
  auto package_root = ResolvePackageRoot(xuid, content_type, title_id);
  SweepDeadFiles(package_root);
  auto file_infos = rex::filesystem::ListFiles(package_root);
  for (const auto& file_info : file_infos) {
    if (file_info.type != rex::filesystem::FileInfo::Type::kDirectory) {
      // Directories only.
      continue;
    }
    auto file_name = rex::path_to_utf8(file_info.name);
    if (file_name.ends_with(kTransactionBackupSuffix)) {
      continue;
    }

    XCONTENT_AGGREGATE_DATA content_data;
    RecoverContentTransaction(xuid, [&] {
      XCONTENT_AGGREGATE_DATA value{};
      value.device_id = device_id;
      value.content_type = content_type;
      value.xuid = xuid;
      value.title_id = title_id;
      value.set_file_name(file_name);
      return value;
    }());
    if (ReadContentHeaderFile(file_name, xuid, title_id, content_type, content_data) ==
        X_ERROR_SUCCESS) {
      result.emplace_back(std::move(content_data));
    } else if (content_type == XContentType::kSavedGame) {
      // Saved games without valid metadata are incomplete or corrupt and must
      // not be surfaced to the guest as loadable slots.
      continue;
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(rex::path_to_utf16(file_info.name));
      content_data.set_file_name(rex::path_to_utf8(file_info.name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;
      result.emplace_back(std::move(content_data));
    }
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, package_path);
  return package;
}

bool ContentManager::ContentExists(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  if (RecoverContentTransaction(xuid, data) != X_ERROR_SUCCESS) {
    return false;
  }
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

X_RESULT ContentManager::WriteContentHeaderFile(uint64_t xuid, XCONTENT_AGGREGATE_DATA data,
                                                uint32_t license_mask) {
  if (data.title_id == uint32_t(-1)) {
    data.title_id = kernel_state_->title_id();
  }
  if (data.xuid == uint64_t(-1)) {
    data.xuid = xuid;
  }
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    std::error_code directory_error;
    if (!std::filesystem::create_directories(parent_path, directory_error)) {
      LogContentFailure(Kind::kHeader, "WriteHeader", "create-directory", data,
                        X_ERROR_ACCESS_DENIED, directory_error);
      return X_ERROR_ACCESS_DENIED;
    }
  }

  rex::filesystem::CreateEmptyFile(header_path);

  auto file = rex::filesystem::OpenFile(header_path, "wb");
  if (!file) {
    LogContentFailure(Kind::kHeader, "WriteHeader", "open", data, X_ERROR_FILE_NOT_FOUND,
                      cl::LastSystemError());
    return X_ERROR_FILE_NOT_FOUND;
  }
  bool write_succeeded =
      fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file) == sizeof(XCONTENT_AGGREGATE_DATA);
  if (license_mask != 0) {
    write_succeeded = write_succeeded &&
                      fwrite(&license_mask, 1, sizeof(license_mask), file) == sizeof(license_mask);
  }
  write_succeeded = fclose(file) == 0 && write_succeeded;
  if (!write_succeeded) {
    LogContentFailure(Kind::kHeader, "WriteHeader", "write", data, X_ERROR_ACCESS_DENIED,
                      cl::LastSystemError());
    std::error_code error;
    std::filesystem::remove(header_path, error);
    return X_ERROR_ACCESS_DENIED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ReadContentHeaderFile(const std::string_view file_name, uint64_t xuid,
                                               uint32_t title_id, XContentType content_type,
                                               XCONTENT_AGGREGATE_DATA& data) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file = rex::filesystem::OpenFile(header_file_path, "rb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file_size = std::filesystem::file_size(header_file_path);
  if (file_size < header_size) {
    fclose(file);
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::array<uint8_t, header_size> buffer;
  size_t result = fread(buffer.data(), 1, header_size, file);
  fclose(file);

  if (result != header_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::memcpy(&data, buffer.data(), buffer.size());
  const uint64_t stored_xuid = data.xuid;
  if (data.content_type != content_type || data.title_id != title_id ||
      (stored_xuid != 0 && stored_xuid != xuid) || data.file_name() != file_name) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name, uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    LogContentFailure(Kind::kCreate, "CreateContent", "package-exists", data,
                      X_ERROR_ALREADY_EXISTS, 0u);
    return X_ERROR_ALREADY_EXISTS;
  }
  std::error_code directory_error;
  if (!std::filesystem::create_directories(package_path, directory_error)) {
    LogContentFailure(Kind::kCreate, "CreateContent", "create-directory", data,
                      X_ERROR_ACCESS_DENIED, directory_error);
    return X_ERROR_ACCESS_DENIED;
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(const std::string_view root_name, uint64_t xuid,
                                     const XCONTENT_AGGREGATE_DATA& data,
                                     uint32_t& content_license) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);
  package->LoadPackageLicenseMask(ResolvePackageHeaderPath(
      data.file_name(), xuid, kernel_state_->title_id(), data.content_type));
  content_license = package->GetPackageLicense();

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  return CloseContent(root_name, nullptr);
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name, bool* committed) {
  if (committed) {
    *committed = false;
  }
  ContentPackage* package = nullptr;
  XCONTENT_AGGREGATE_DATA content_data{};
  {
    auto global_lock = global_critical_region_.Acquire();
    // Some games use different casing between Create and Close (e.g. "save" vs "SAVE")
    auto it = open_packages_.find(string::string_key_case(root_name));
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
    content_data = package->GetPackageContentData();
  }
  delete package;
  if (content_data.content_type == XContentType::kSavedGame) {
    uint64_t xuid = content_data.xuid;
    if (xuid == 0 || xuid == uint64_t(-1)) {
      xuid = kernel_state_->user_profile()->xuid();
    }
    const X_RESULT result = CommitContentTransaction(xuid, content_data);
    if (committed) {
      *committed = result == X_ERROR_SUCCESS;
    }
    return result;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / kThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = rex::filesystem::OpenFile(thumb_path, "rb");
    if (!file) {
      LogContentFailure(Kind::kHeader, "GetThumbnail", "open", data, X_ERROR_ACCESS_DENIED,
                        cl::LastSystemError());
      return X_ERROR_ACCESS_DENIED;
    }
    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::SetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    auto file = rex::filesystem::OpenFile(thumb_path, "wb");
    if (!file) {
      LogContentFailure(Kind::kHeader, "SetThumbnail", "open", data, X_ERROR_ACCESS_DENIED,
                        cl::LastSystemError());
      return X_ERROR_ACCESS_DENIED;
    }
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::DeleteContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
    LogContentFailure(Kind::kDelete, "DeleteContent", "content-open", data, X_ERROR_ACCESS_DENIED,
                      0u);
    return X_ERROR_ACCESS_DENIED;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  uintmax_t dir_removed = 0;
  std::error_code ec = RemovePackageTree(package_path, data, "DeleteContent", &dir_removed);
  if (ec) {
    LogContentFailure(Kind::kDelete, "DeleteContent", "remove-package", data, X_ERROR_ACCESS_DENIED,
                      ec);
    return X_ERROR_ACCESS_DENIED;
  }

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

X_RESULT ContentManager::UnmountContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::UnmountAndDeleteContent(uint64_t xuid,
                                                 const XCONTENT_AGGREGATE_DATA& data) {
  // Unmount phase: tolerant of not-mounted state
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it != open_packages_.end()) {
      package = DetachPackage(it);
    }
  }
  delete package;

  // Delete phase: remove package directory and .header file
  auto package_path = ResolvePackagePath(xuid, data);

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);

  uintmax_t dir_removed = 0;
  std::error_code ec =
      RemovePackageTree(package_path, data, "UnmountAndDeleteContent", &dir_removed);
  if (ec) {
    LogContentFailure(Kind::kDelete, "UnmountAndDeleteContent", "remove-package", data,
                      X_ERROR_ACCESS_DENIED, ec);
    return X_ERROR_ACCESS_DENIED;
  }

  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  return kernel_state_->user_profile()->TitleProfilePath(root_path_, kernel_state_->title_id());
}

std::filesystem::path ContentManager::ResolveGlobalUserProfilePath() {
  return kernel_state_->user_profile()->GlobalProfilePath(root_path_);
}

std::unordered_map<string::string_key_case, ContentPackage*,
                   string::string_key_case::Hash>::iterator
ContentManager::FindOpenPackageByData(const XCONTENT_AGGREGATE_DATA& data) {
  // Resolve kCurrentlyRunningTitleId so both sides compare actual title IDs.
  uint32_t query_title = data.title_id;
  if (query_title == kCurrentlyRunningTitleId) {
    query_title = kernel_state_->title_id();
  }

  for (auto it = open_packages_.begin(); it != open_packages_.end(); ++it) {
    const auto& pkg = it->second->GetPackageContentData();

    uint32_t pkg_title = pkg.title_id;
    if (pkg_title == kCurrentlyRunningTitleId) {
      pkg_title = kernel_state_->title_id();
    }

    // Match on content_type + file_name + resolved title_id.
    // device_id is a virtual storage selector, not a content identifier.
    if (data.content_type == pkg.content_type && data.file_name() == pkg.file_name() &&
        query_title == pkg_title) {
      return it;
    }
  }
  return open_packages_.end();
}

ContentPackage* ContentManager::DetachPackage(
    std::unordered_map<string::string_key_case, ContentPackage*,
                       string::string_key_case::Hash>::iterator it) {
  CloseOpenedFilesFromContent(it->first.view());
  ContentPackage* package = it->second;
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

std::filesystem::path ContentManager::GetOpenPackagePath(const std::string_view root_name) const {
  auto it = open_packages_.find(string::string_key_case(root_name));
  if (it == open_packages_.end()) {
    return {};
  }
  return it->second->package_path();
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(XObject::Type::File);

  std::string resolved_path = "";
  kernel_state_->file_system()->FindSymbolicLink(std::string(root_name) + ':', resolved_path);
  if (resolved_path.empty()) {
    // An empty prefix would match every open file of the title.
    cl::Warn(Kind::kClose, "close: root '{}' has no device link; no file objects released",
             root_name);
    return;
  }

  for (const object_ref<XFile>& file : all_files_handles) {
    if (file->host_closed()) {
      // Already force-closed by an earlier unmount; its entry may be gone.
      continue;
    }
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content = rex::string::utf8_starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }

  // KI-068: a file object still referenced after its handle was released
  // keeps its host handle open (under Wine that blocks the package removal of
  // the next overwrite). The device is about to be unregistered, so close the
  // host handles now.
  size_t still_referenced = 0;
  const size_t closed = ForceCloseHostFiles(all_files_handles, resolved_path, &still_referenced);
  if (closed) {
    cl::Info(Kind::kUnmount,
             "close: {} host handle(s) force-closed on unmount: root='{}' still_referenced={}",
             closed, root_name, still_referenced);
  }
}

size_t ContentManager::ForceCloseHostFiles(const std::vector<object_ref<XFile>>& files,
                                           std::string_view device_prefix,
                                           size_t* still_referenced) {
  size_t closed = 0;
  size_t referenced = 0;
  if (!device_prefix.empty() && REXCVAR_GET(xam_content_close_host_handles_on_unmount)) {
    for (const object_ref<XFile>& file : files) {
      if (!file || file->host_closed() || !file->entry()) {
        continue;
      }
      if (!rex::string::utf8_starts_with(file->entry()->absolute_path(), device_prefix)) {
        continue;
      }
      // One reference is the caller's list.
      if (file->pointer_ref_count() > 1) {
        ++referenced;
      }
      if (file->ForceCloseHost()) {
        ++closed;
      }
    }
  }
  if (still_referenced) {
    *still_referenced = referenced;
  }
  return closed;
}

static X_RESULT ExtractEntry(rex::filesystem::Entry* entry,
                             const std::filesystem::path& base_path) {
  auto dest_path = base_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));

  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    std::error_code ec;
    std::filesystem::create_directories(dest_path, ec);
    if (ec) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }

  // Ensure parent directory exists
  std::error_code ec;
  std::filesystem::create_directories(dest_path.parent_path(), ec);

  rex::filesystem::File* in_file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file);
  if (status != X_STATUS_SUCCESS) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto out_file = rex::filesystem::OpenFile(dest_path, "wb");
  if (!out_file) {
    in_file->Destroy();
    return X_ERROR_ACCESS_DENIED;
  }

  constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4 MiB
  auto buffer = std::make_unique<uint8_t[]>(kBufferSize);
  size_t remaining = entry->size();
  size_t offset = 0;

  while (remaining > 0) {
    size_t bytes_read = 0;
    size_t to_read = std::min(remaining, kBufferSize);
    in_file->ReadSync(std::span<uint8_t>(buffer.get(), to_read), offset, &bytes_read);
    if (bytes_read == 0) {
      break;
    }
    fwrite(buffer.get(), 1, bytes_read, out_file);
    offset += bytes_read;
    remaining -= bytes_read;
  }

  fclose(out_file);
  in_file->Destroy();
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::InstallContent(const std::filesystem::path& package_path) {
  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Mount the STFS package as a virtual filesystem device
  auto device = std::make_unique<rex::filesystem::StfsContainerDevice>("", package_path);
  if (!device->Initialize()) {
    return X_ERROR_ACCESS_DENIED;
  }

  // Derive install destination:
  // root_path_/0000000000000000/{title_id}/00000002/{filename}/
  auto file_name = rex::path_to_utf8(package_path.filename());

  XCONTENT_AGGREGATE_DATA content_data;
  content_data.device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
  content_data.content_type = XContentType::kMarketplaceContent;
  content_data.title_id = kernel_state_->title_id();
  content_data.xuid = 0;
  content_data.set_file_name(file_name);

  // Read display name from STFS metadata
  auto display_name = device->header().metadata.display_name(rex::system::XLanguage::kEnglish);
  if (!display_name.empty()) {
    content_data.set_display_name(display_name);
  } else {
    content_data.set_display_name(rex::path_to_utf16(package_path.filename()));
  }

  auto install_path = ResolvePackagePath(0, content_data);

  // Create destination directory
  std::error_code ec;
  std::filesystem::create_directories(install_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  // Extract all files breadth-first
  auto* root = device->ResolvePath("");
  if (!root) {
    return X_ERROR_ACCESS_DENIED;
  }

  std::queue<rex::filesystem::Entry*> queue;
  queue.push(root);

  while (!queue.empty()) {
    auto* entry = queue.front();
    queue.pop();

    for (auto& child : entry->children()) {
      queue.push(child.get());
    }

    auto result = ExtractEntry(entry, install_path);
    if (result != X_ERROR_SUCCESS) {
      return result;
    }
  }

  // Compute license mask from STFS header licenses
  uint32_t license_mask = 0;
  for (size_t i = 0; i < 0x10; i++) {
    if (device->header().header.licenses[i].license_flags) {
      license_mask |= device->header().header.licenses[i].license_bits;
    }
  }

  // Write .header file
  return WriteContentHeaderFile(0, content_data, license_mask);
}

}  // namespace xam
}  // namespace system
}  // namespace rex
