/**
 * @file        filesystem_test.cpp
 * @brief       Host filesystem path conversion tests
 *
 * @copyright   Copyright (c) 2026 Tom Clay
 * @license     BSD 3-Clause License
 */

#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <rex/filesystem.h>

TEST_CASE("filesystem paths round-trip non-ASCII text through UTF-8", "[filesystem]") {
  constexpr std::u16string_view kUtf16 = u"MCLA-\u0414\u0430\u043d\u0456";
  constexpr std::string_view kUtf8 = "MCLA-\xD0\x94\xD0\xB0\xD0\xBD\xD1\x96";

  const auto path = rex::to_path(kUtf16);
  CHECK(rex::path_to_utf8(path) == kUtf8);
  CHECK(rex::to_path(rex::path_to_utf8(path)) == path);
}
