#pragma once

#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

constexpr X_STATUS GetDismountVolumeByFileHandleResult(bool is_file_handle) {
  return is_file_handle ? X_STATUS_SUCCESS : X_STATUS_INVALID_HANDLE;
}

}  // namespace rex::kernel::xboxkrnl
