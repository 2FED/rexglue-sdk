/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/content_log.h>
#include <rex/filesystem/device.h>

#include <chrono>
#include <mutex>

#include <rex/perf/frame_audit.h>

namespace rex::filesystem {

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

bool HostPathFile::CloseHost() {
  std::unique_lock<std::shared_mutex> lock(host_mutex_);
  if (host_closed_.load(std::memory_order_acquire)) {
    return false;
  }
  // Capture what the stale-I/O WARN needs while the entry is still alive.
  if (entry_ && entry_->device()) {
    closed_path_ = entry_->path();
    closed_on_content_device_ =
        rex::content_log::IsContentDevicePath(entry_->device()->mount_path());
  }
  const bool had_handle = file_handle_ != nullptr;
  file_handle_.reset();
  host_closed_.store(true, std::memory_order_release);
  return had_handle;
}

X_STATUS HostPathFile::ClosedStatus(const char* op) const {
  if (closed_on_content_device_) {
    rex::content_log::Warn(rex::content_log::Kind::kStaleIo,
                           "{} on a file closed by content unmount: path='{}' status={:#x}", op,
                           closed_path_, uint32_t(X_STATUS_FILE_CLOSED));
  }
  return X_STATUS_FILE_CLOSED;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  std::shared_lock<std::shared_mutex> lock(host_mutex_);
  if (!file_handle_) {
    return ClosedStatus("read");
  }
  bool read_succeeded;
  if (rex::perf::IsFrameAuditEnabled()) {
    const auto read_start = std::chrono::steady_clock::now();
    read_succeeded = file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read);
    const uint64_t read_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - read_start)
                                          .count());
    rex::perf::RecordStreamingRead(read_succeeded && out_bytes_read ? *out_bytes_read : 0, read_us);
  } else {
    read_succeeded = file_handle_->Read(byte_offset, buffer.data(), buffer.size(), out_bytes_read);
  }
  if (read_succeeded) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  std::shared_lock<std::shared_mutex> lock(host_mutex_);
  if (!file_handle_) {
    return ClosedStatus("write");
  }
  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    const uint32_t system_error = rex::content_log::LastSystemError();
    if (rex::content_log::IsContentDevicePath(entry_->device()->mount_path())) {
      rex::content_log::Warn(rex::content_log::Kind::kFileWrite,
                             "content file write failed: path='{}' offset={} length={} win32={} "
                             "({})",
                             entry_->path(), byte_offset, buffer.size(), system_error,
                             rex::content_log::SystemErrorText(system_error));
    }
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  std::shared_lock<std::shared_mutex> lock(host_mutex_);
  if (!file_handle_) {
    return ClosedStatus("set-length");
  }
  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    const uint32_t system_error = rex::content_log::LastSystemError();
    if (rex::content_log::IsContentDevicePath(entry_->device()->mount_path())) {
      rex::content_log::Warn(rex::content_log::Kind::kFileSetInfo,
                             "content file set-length failed: path='{}' length={} win32={} ({})",
                             entry_->path(), length, system_error,
                             rex::content_log::SystemErrorText(system_error));
    }
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
