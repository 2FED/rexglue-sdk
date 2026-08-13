#include <array>

#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/offline_service_audit.h>

namespace rex::kernel::xam {

TEST_CASE("Offline service message inventory is explicit", "[kernel][offline-service]") {
  constexpr std::array<uint32_t, 10> messages = {
      0x00058004, 0x00058006, 0x00058007, 0x00058009, 0x0005800E,
      0x00058020, 0x00058023, 0x00058035, 0x00058037, 0x00058046,
  };
  for (uint32_t message : messages) {
    CHECK(IsOfflineServiceKnownMessage(message));
  }
  CHECK_FALSE(IsOfflineServiceKnownMessage(0x00058008));
  CHECK_FALSE(IsOfflineServiceKnownMessage(0x000580FF));
}

TEST_CASE("Offline service local and unavailable classes are distinct",
          "[kernel][offline-service]") {
  CHECK(IsOfflineServiceLocalMessage(0x00058004));
  CHECK(IsOfflineServiceLocalMessage(0x00058006));
  CHECK(IsOfflineServiceLocalMessage(0x00058037));
  CHECK(IsOfflineServiceLocalMessage(0x00058046));
  CHECK_FALSE(IsOfflineServiceLocalMessage(0x00058007));
  CHECK_FALSE(IsOfflineServiceLocalMessage(0x00058020));
}

TEST_CASE("Offline service results fail closed", "[kernel][offline-service]") {
  CHECK(IsOfflineServiceExpectedResult(0x00058004, 0x00000000));
  CHECK(IsOfflineServiceExpectedResult(0x00058006, 0x00000000));
  CHECK(IsOfflineServiceExpectedResult(0x00058037, 0x00000000));
  CHECK(IsOfflineServiceExpectedResult(0x00058046, 0x00000000));
  CHECK(IsOfflineServiceExpectedResult(0x00058007, 0x80151802));
  CHECK(IsOfflineServiceExpectedResult(0x00058009, 0x80004005));
  CHECK(IsOfflineServiceExpectedResult(0x0005800E, 0x80004005));
  CHECK(IsOfflineServiceExpectedResult(0x00058020, 0x80004005));
  CHECK(IsOfflineServiceExpectedResult(0x00058023, 0x80004005));
  CHECK(IsOfflineServiceExpectedResult(0x00058035, 0x80004005));
  CHECK_FALSE(IsOfflineServiceExpectedResult(0x00058004, 0x80004005));
  CHECK_FALSE(IsOfflineServiceExpectedResult(0x00058007, 0x00000000));
  CHECK_FALSE(IsOfflineServiceExpectedResult(0x00058009, 0x00000000));
  CHECK_FALSE(IsOfflineServiceExpectedResult(0x00058008, 0x80004005));
}

}  // namespace rex::kernel::xam
