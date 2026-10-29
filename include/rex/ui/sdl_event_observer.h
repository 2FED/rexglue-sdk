/**
 * @file        ui/sdl_event_observer.h
 * @brief       Read-only host observation of the SDK's own SDL event stream
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

// The SDK links SDL statically into rexruntime. A host that links its own
// SDL copy gets a second, uninitialised SDL instance that never sees the
// window, joystick or gamepad events, so hosts must observe events through
// this API instead of calling SDL_AddEventWatch themselves.
//
// SDL_Event is used only as a plain data type here; including this header
// does not make the host reference any SDL function.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>

namespace rex::ui {

// Called for every event SDL pushes (all types), on whichever thread pushes
// it (the UI thread or an SDL joystick thread). Observers are read-only:
// they cannot consume, filter or modify events, they must return quickly,
// must not block on the UI thread, and must not call Add/Remove themselves.
using SdlEventObserver = std::function<void(const SDL_Event&)>;

// Zero is never a valid token.
using SdlEventObserverToken = uint64_t;

// Registers an observer. Returns 0 if the observer is empty or the SDL event
// watch cannot be installed. Thread safe.
SdlEventObserverToken AddSdlEventObserver(SdlEventObserver observer);

// Unregisters an observer. When this returns, no call to that observer is in
// flight and none will start. Returns false for an unknown token. Thread
// safe; must not be called from inside an observer.
bool RemoveSdlEventObserver(SdlEventObserverToken token);

// One SDL joystick as the SDK's SDL instance sees it. Values are filled only
// for joysticks the SDK input driver has opened (opened == true).
struct SdlJoystickSnapshot {
  uint32_t instance_id = 0;
  std::string name;
  std::string guid;  // SDL_GUIDToString form (32 hex digits).
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  int joystick_type = 0;  // SDL_JoystickType value.
  bool is_gamepad = false;
  bool is_wheel = false;  // Same admission rule as the SDK wheel driver.
  bool opened = false;
  bool haptic = false;
  std::vector<int16_t> axes;
  std::vector<uint8_t> buttons;
  std::vector<uint8_t> hats;
};

inline constexpr size_t kSdlJoystickSnapshotMaxDevices = 16;
inline constexpr size_t kSdlJoystickSnapshotMaxAxes = 32;
inline constexpr size_t kSdlJoystickSnapshotMaxButtons = 128;
inline constexpr size_t kSdlJoystickSnapshotMaxHats = 8;

// Reads the connected joysticks. Never opens or closes a device and never
// touches haptic state. Thread safe (uses only SDL's own joystick lock).
std::vector<SdlJoystickSnapshot> CaptureSdlJoysticks(bool include_values = true);

// Same as CaptureSdlJoysticks for a single instance ID.
std::optional<SdlJoystickSnapshot> QuerySdlJoystick(uint32_t instance_id, bool include_values);

namespace internal {
// Test seams: dispatch without SDL and inspect the registry.
void DispatchSdlEventObserversForTesting(const SDL_Event& event);
size_t SdlEventObserverCountForTesting();
}  // namespace internal

}  // namespace rex::ui
