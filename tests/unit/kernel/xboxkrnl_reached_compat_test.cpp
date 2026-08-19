#include <catch2/catch_test_macros.hpp>

#include "kernel/xboxkrnl/crypto_compat.h"
#include "kernel/xboxkrnl/io_compat.h"

namespace rex::kernel::xboxkrnl {

TEST_CASE("Dismount compatibility accepts only file handles",
          "[kernel][xboxkrnl][reached-compat]") {
  CHECK(GetDismountVolumeByFileHandleResult(true) == X_STATUS_SUCCESS);
  CHECK(GetDismountVolumeByFileHandleResult(false) == X_STATUS_INVALID_HANDLE);
}

TEST_CASE("Console private-key signing fails closed without a console key",
          "[kernel][xboxkrnl][reached-compat]") {
  CHECK(GetConsolePrivateKeySignResult(true, true) == X_STATUS_NOT_SUPPORTED);
  CHECK(GetConsolePrivateKeySignResult(false, true) == X_STATUS_INVALID_PARAMETER);
  CHECK(GetConsolePrivateKeySignResult(true, false) == X_STATUS_INVALID_PARAMETER);
}

}  // namespace rex::kernel::xboxkrnl
