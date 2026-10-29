/**
 * Regression coverage for deterministic SDL to Xbox user-slot selection.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/input/sdl/slot_mapping.h>

using rex::input::sdl::ChooseControllerSlot;
using rex::input::sdl::ChooseMostRecentlyActiveController;
using rex::input::sdl::IsInputSlotAuditChallengeButton;
using rex::input::sdl::ResolveLogicalControllerSlot;

TEST_CASE("First SDL controller deterministically owns physical slot zero", "[input][sdl][slot]") {
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

TEST_CASE("Xbox slot zero follows the active physical controller", "[input][sdl][slot]") {
  CHECK(ResolveLogicalControllerSlot(0, 2, 0x7) == 2);
  CHECK_FALSE(ResolveLogicalControllerSlot(2, 2, 0x7).has_value());
  CHECK(ResolveLogicalControllerSlot(1, 2, 0x7) == 1);
  CHECK_FALSE(ResolveLogicalControllerSlot(3, 2, 0x7).has_value());
  CHECK_FALSE(ResolveLogicalControllerSlot(0, 2, 0x3).has_value());
  CHECK_FALSE(ResolveLogicalControllerSlot(4, 0, 0x1).has_value());
}

TEST_CASE("Fallback selection uses the most recently active connected controller",
          "[input][sdl][slot]") {
  std::array<uint64_t, rex::input::sdl::kControllerSlotCount> activity = {7, 0, 11, 3};
  CHECK(ChooseMostRecentlyActiveController(0xF, activity) == 2);
  CHECK(ChooseMostRecentlyActiveController(0xB, activity) == 0);
  CHECK(ChooseMostRecentlyActiveController(0xA, activity) == 3);
  CHECK_FALSE(ChooseMostRecentlyActiveController(0, activity).has_value());

  activity = {};
  CHECK(ChooseMostRecentlyActiveController(0xC, activity) == 2);
}

TEST_CASE("Latest meaningful input alternates guest slot zero between wheel and gamepad",
          "[input][sdl][slot]") {
  // Physical slot 0 is a gamepad and slot 2 is a wheel. Device kind is
  // intentionally absent from the selector: only connected state and the
  // monotonic meaningful-input sequence decide which device owns guest slot 0.
  std::array<uint64_t, rex::input::sdl::kControllerSlotCount> activity = {10, 0, 20, 0};
  CHECK(ChooseMostRecentlyActiveController(0x5, activity) == 2);
  CHECK(ResolveLogicalControllerSlot(0, 2, 0x5) == 2);

  activity[0] = 30;
  CHECK(ChooseMostRecentlyActiveController(0x5, activity) == 0);
  CHECK(ResolveLogicalControllerSlot(0, 0, 0x5) == 0);

  activity[2] = 40;
  CHECK(ChooseMostRecentlyActiveController(0x5, activity) == 2);
  CHECK(ResolveLogicalControllerSlot(0, 2, 0x5) == 2);

  CHECK(ChooseMostRecentlyActiveController(0x4, activity) == 2);
}

TEST_CASE("Hot remove and re-add with a new SDL instance re-binds guest slot zero",
          "[input][sdl][slot][hotplug]") {
  // KI-040: one pad, removed and re-added by SDL under a new instance id.
  // Connect: the first device owns physical slot 0 and guest slot 0.
  uint32_t connected = 0;
  std::array<uint64_t, rex::input::sdl::kControllerSlotCount> activity = {};
  auto slot = ChooseControllerSlot(0, connected);
  REQUIRE(slot == 0);
  connected |= uint32_t(1) << *slot;
  std::optional<uint32_t> active = slot;
  CHECK(ResolveLogicalControllerSlot(0, active, connected) == 0);

  // Remove: the slot is freed and the fallback finds nothing, so guest slot 0
  // reports not-connected instead of a stale device.
  connected &= ~(uint32_t(1) << *slot);
  active = ChooseMostRecentlyActiveController(connected, activity);
  CHECK_FALSE(active.has_value());
  CHECK_FALSE(ResolveLogicalControllerSlot(0, active, connected).has_value());
  for (uint32_t logical = 1; logical < rex::input::sdl::kControllerSlotCount; ++logical) {
    CHECK_FALSE(ResolveLogicalControllerSlot(logical, active, connected).has_value());
  }

  // Re-add: SDL may report any player index (XInput user index can change);
  // the empty map still gives physical slot 0 and guest slot 0 follows it.
  for (const int32_t reported : {-1, 0, 1, 3, 7}) {
    CHECK(ChooseControllerSlot(reported, connected) == 0);
  }
  slot = ChooseControllerSlot(1, connected);
  REQUIRE(slot == 0);
  connected |= uint32_t(1) << *slot;
  active = slot;
  CHECK(ResolveLogicalControllerSlot(0, active, connected) == 0);
  CHECK_FALSE(ResolveLogicalControllerSlot(1, active, connected).has_value());

  // A second remove/re-add cycle behaves identically.
  connected = 0;
  active = ChooseMostRecentlyActiveController(connected, activity);
  CHECK_FALSE(ResolveLogicalControllerSlot(0, active, connected).has_value());
  CHECK(ChooseControllerSlot(2, connected) == 0);
}

TEST_CASE("Removing one of two pads hands guest slot zero to the remaining pad",
          "[input][sdl][slot][hotplug]") {
  std::array<uint64_t, rex::input::sdl::kControllerSlotCount> activity = {5, 9, 0, 0};
  uint32_t connected = 0x3;
  CHECK(ResolveLogicalControllerSlot(0, 1, connected) == 1);

  connected &= ~uint32_t(0x2);  // the active pad in physical slot 1 drops
  const auto active = ChooseMostRecentlyActiveController(connected, activity);
  CHECK(active == 0);
  CHECK(ResolveLogicalControllerSlot(0, active, connected) == 0);

  // It comes back with a new instance id; slot 0 is still taken, so the
  // reported player index (or the lowest free slot) is used and slot zero
  // stays on the pad that kept working until new input selects the other.
  CHECK(ChooseControllerSlot(1, connected) == 1);
  CHECK(ChooseControllerSlot(-1, connected) == 1);
  CHECK(ResolveLogicalControllerSlot(0, active, connected | 0x2) == 0);
}

TEST_CASE("SDL device log fields name the backend and withhold OS device paths",
          "[input][sdl][hotplug]") {
  using rex::input::sdl::PrivacySafeSdlDevicePath;
  using rex::input::sdl::SdlJoystickBackendName;

  CHECK(SdlJoystickBackendName('x') == "xinput");
  CHECK(SdlJoystickBackendName('r') == "rawinput");
  CHECK(SdlJoystickBackendName('h') == "hidapi");
  CHECK(SdlJoystickBackendName('v') == "virtual");
  CHECK(SdlJoystickBackendName(0) == "dinput-or-other");
  CHECK(SdlJoystickBackendName('?') == "unknown");

  CHECK(PrivacySafeSdlDevicePath("XInput#0") == "XInput#0");
  CHECK(PrivacySafeSdlDevicePath("XInput#3") == "XInput#3");
  CHECK(PrivacySafeSdlDevicePath("") == "none");
  CHECK(PrivacySafeSdlDevicePath("XInput#") == "withheld");
  CHECK(PrivacySafeSdlDevicePath("XInput#0&serial") == "withheld");
  CHECK(PrivacySafeSdlDevicePath(R"(\?\HID#VID_2DC8&PID_310B#7&1a2b3c4d&0&0000#{guid})") ==
        "withheld");
}
