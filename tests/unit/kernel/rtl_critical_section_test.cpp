#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xboxkrnl/rtl.h>

namespace rex::kernel::xboxkrnl::test {

TEST_CASE("Critical-section leave ownership policy is fail-closed by default",
          "[kernel][rtl][critical-section]") {
  constexpr uint32_t kOwner = 0x1000;
  constexpr uint32_t kOtherThread = 0x2000;

  CHECK(IsCriticalSectionLeaveAllowed(kOwner, kOwner, false));
  CHECK_FALSE(IsCriticalSectionLeaveAllowed(kOwner, kOtherThread, false));
  CHECK(IsCriticalSectionLeaveAllowed(kOwner, kOtherThread, true));
}

}  // namespace rex::kernel::xboxkrnl::test
