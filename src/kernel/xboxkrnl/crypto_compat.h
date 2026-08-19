#pragma once

#include <rex/system/xtypes.h>

namespace rex::kernel::xboxkrnl {

constexpr X_STATUS GetConsolePrivateKeySignResult(bool has_hash, bool has_signature) {
  if (!has_hash || !has_signature) {
    return X_STATUS_INVALID_PARAMETER;
  }
  return X_STATUS_NOT_SUPPORTED;
}

}  // namespace rex::kernel::xboxkrnl
