#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xam/locale_audit.h>

namespace rex::kernel::xam {

TEST_CASE("XGetLanguage selection accepts the Xbox language range", "[kernel][locale]") {
  for (uint32_t language = 1; language <= 12; ++language) {
    CHECK(NormalizeUserLanguage(language) == language);
  }
}

TEST_CASE("XGetLanguage selection falls back to English", "[kernel][locale]") {
  CHECK(NormalizeUserLanguage(0) == 1);
  CHECK(NormalizeUserLanguage(13) == 1);
  CHECK(NormalizeUserLanguage(UINT32_MAX) == 1);
}

TEST_CASE("Locale country range is bounded", "[kernel][locale]") {
  CHECK(IsUserCountryValid(0));
  CHECK(IsUserCountryValid(34));
  CHECK(IsUserCountryValid(88));
  CHECK(IsUserCountryValid(103));
  CHECK(IsUserCountryValid(111));
  CHECK_FALSE(IsUserCountryValid(112));
  CHECK_FALSE(IsUserCountryValid(UINT32_MAX));
}

}  // namespace rex::kernel::xam
