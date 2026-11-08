// l81 (KI-060): guest_thread_priority_boost_start parsing and matching.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/system/guest_thread_priority.h>

using rex::system::GuestThreadStartMatches;
using rex::system::ParseGuestThreadStartList;

TEST_CASE("Guest thread boost: start list parsing", "[system][guest_thread_priority]") {
  const auto def = ParseGuestThreadStartList("823EC8C8,823EC990");
  REQUIRE(def.has_value());
  CHECK(*def == std::vector<uint32_t>{0x823EC8C8, 0x823EC990});

  const auto spaced = ParseGuestThreadStartList(" 0x823ec8c8 ; 0X82144AB8,, ");
  REQUIRE(spaced.has_value());
  CHECK(*spaced == std::vector<uint32_t>{0x823EC8C8, 0x82144AB8});

  const auto empty = ParseGuestThreadStartList("");
  REQUIRE(empty.has_value());
  CHECK(empty->empty());

  CHECK_FALSE(ParseGuestThreadStartList("823EC8C8,zz").has_value());
  CHECK_FALSE(ParseGuestThreadStartList("1823EC8C8").has_value());  // 9 digits
  CHECK_FALSE(ParseGuestThreadStartList("0").has_value());
  CHECK_FALSE(ParseGuestThreadStartList("0x").has_value());
}

TEST_CASE("Guest thread boost: start address matching", "[system][guest_thread_priority]") {
  const std::vector<uint32_t> list{0x823EC8C8, 0x823EC990};
  CHECK(GuestThreadStartMatches(list, 0x823EC8C8));
  CHECK(GuestThreadStartMatches(list, 0x823EC990));
  CHECK_FALSE(GuestThreadStartMatches(list, 0x82144AB8));
  CHECK_FALSE(GuestThreadStartMatches(list, 0));
  CHECK_FALSE(GuestThreadStartMatches({}, 0x823EC8C8));
}
