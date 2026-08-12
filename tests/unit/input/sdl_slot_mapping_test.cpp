/**
 * Regression coverage for deterministic SDL to Xbox user-slot selection.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/input/sdl/slot_mapping.h>

using rex::input::sdl::ChooseControllerSlot;
using rex::input::sdl::IsInputSlotAuditChallengeButton;

TEST_CASE("First SDL controller deterministically owns Xbox slot zero", "[input][sdl][slot]") {
  CHECK(ChooseControllerSlot(0, 0) == 0);
  const auto slot = ChooseControllerSlot(3, 0);
  REQUIRE(slot.has_value());
  CHECK(*slot == 0);
}

TEST_CASE("SDL controller selection preserves a valid free player index", "[input][sdl][slot]") {
  CHECK(ChooseControllerSlot(2, 0x1) == 2);
  CHECK(ChooseControllerSlot(3, 0x3) == 3);
}

TEST_CASE("SDL controller selection falls back to the lowest free slot", "[input][sdl][slot]") {
  CHECK(ChooseControllerSlot(-1, 0x1) == 1);
  CHECK(ChooseControllerSlot(1, 0x3) == 2);
  CHECK(ChooseControllerSlot(8, 0x5) == 1);
  CHECK(ChooseControllerSlot(3, 0xE) == 0);
}

TEST_CASE("SDL controller selection rejects a full four-slot map", "[input][sdl][slot]") {
  CHECK_FALSE(ChooseControllerSlot(2, 0xF).has_value());
}

TEST_CASE("Input-slot audit accepts only the SDL South face button", "[input][sdl][slot]") {
  CHECK(IsInputSlotAuditChallengeButton(0));
  CHECK_FALSE(IsInputSlotAuditChallengeButton(19));
}
