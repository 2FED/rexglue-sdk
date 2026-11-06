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

#pragma once

#include <atomic>
#include <shared_mutex>
#include <span>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/file.h>

namespace rex::filesystem {

class HostPathEntry;

class HostPathFile : public File {
 public:
  HostPathFile(uint32_t file_access, HostPathEntry* entry,
               std::unique_ptr<rex::filesystem::FileHandle> file_handle);
  ~HostPathFile() override;

  void Destroy() override;

  X_STATUS ReadSync(std::span<uint8_t> buffer, size_t byte_offset, size_t* out_bytes_read) override;
  X_STATUS WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                     size_t* out_bytes_written) override;
  X_STATUS SetLength(size_t length) override;

  bool CloseHost() override;
  bool host_closed() const override { return host_closed_.load(std::memory_order_acquire); }

 private:
  // Logs one paced WARN for guest I/O on a file whose host handle was closed
  // by a content unmount, and returns X_STATUS_FILE_CLOSED. Never touches
  // entry_: the entry may belong to an unregistered (destroyed) device.
  X_STATUS ClosedStatus(const char* op) const;

  std::unique_ptr<rex::filesystem::FileHandle> file_handle_;
  // I/O holds it shared around file_handle_ use; CloseHost holds it
  // exclusively. Never held while taking the global critical region.
  mutable std::shared_mutex host_mutex_;
  std::atomic<bool> host_closed_{false};
  // Device-relative path and content flag captured at CloseHost time.
  std::string closed_path_;
  bool closed_on_content_device_ = false;
};

}  // namespace rex::filesystem
