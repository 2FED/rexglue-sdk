/**
 * @file        ui/sdl_event_observer.cpp
 * @brief       Read-only host observation of the SDK's own SDL event stream
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/sdl_event_observer.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include <SDL3/SDL.h>

#include <rex/input/sdl/wheel_mapping.h>

namespace rex::ui {
namespace {

struct Entry {
  SdlEventObserverToken token = 0;
  SdlEventObserver observer;
};

// Lock order: SDL may hold its own watch-list lock while it calls Watch, and
// Watch takes the registry lock (shared). SDL_AddEventWatch is therefore never
// called with the registry lock held, and the watch is never removed (it is a
// cheap no-op while the registry is empty).
struct Registry {
  std::shared_mutex mutex;
  std::vector<Entry> entries;
  SdlEventObserverToken next_token = 1;
  std::once_flag watch_once;
  std::atomic<bool> watch_installed{false};
};

Registry& GetRegistry() {
  static Registry registry;
  return registry;
}

void Dispatch(const SDL_Event& event) {
  auto& registry = GetRegistry();
  std::shared_lock lock(registry.mutex);
  for (const auto& entry : registry.entries) {
    entry.observer(event);
  }
}

bool SDLCALL Watch(void*, SDL_Event* event) {
  if (event) {
    Dispatch(*event);
  }
  // SDL ignores the return value of an event watch.
  return true;
}

void FillSnapshot(SDL_JoystickID id, bool include_values, SdlJoystickSnapshot& out) {
  out.instance_id = static_cast<uint32_t>(id);
  if (const char* name = SDL_GetJoystickNameForID(id)) {
    out.name = name;
  }
  char guid[33] = {};
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid, static_cast<int>(sizeof(guid)));
  out.guid = guid;
  out.vendor_id = SDL_GetJoystickVendorForID(id);
  out.product_id = SDL_GetJoystickProductForID(id);
  const SDL_JoystickType type = SDL_GetJoystickTypeForID(id);
  out.joystick_type = static_cast<int>(type);
  out.is_gamepad = SDL_IsGamepad(id);
  out.is_wheel = rex::input::sdl::IsSupportedWheel(out.vendor_id, out.product_id, type);
  // Returns the handle the SDK opened, without adding a reference.
  SDL_Joystick* joystick = SDL_GetJoystickFromID(id);
  out.opened = joystick != nullptr;
  if (!joystick) {
    return;
  }
  out.haptic = SDL_IsJoystickHaptic(joystick);
  if (!include_values) {
    return;
  }
  const auto bounded = [](int count, size_t maximum) {
    return static_cast<size_t>(std::clamp(count, 0, static_cast<int>(maximum)));
  };
  const size_t axes = bounded(SDL_GetNumJoystickAxes(joystick), kSdlJoystickSnapshotMaxAxes);
  const size_t buttons =
      bounded(SDL_GetNumJoystickButtons(joystick), kSdlJoystickSnapshotMaxButtons);
  const size_t hats = bounded(SDL_GetNumJoystickHats(joystick), kSdlJoystickSnapshotMaxHats);
  out.axes.reserve(axes);
  for (size_t i = 0; i < axes; ++i) {
    out.axes.push_back(SDL_GetJoystickAxis(joystick, static_cast<int>(i)));
  }
  out.buttons.reserve(buttons);
  for (size_t i = 0; i < buttons; ++i) {
    out.buttons.push_back(SDL_GetJoystickButton(joystick, static_cast<int>(i)) ? 1 : 0);
  }
  out.hats.reserve(hats);
  for (size_t i = 0; i < hats; ++i) {
    out.hats.push_back(SDL_GetJoystickHat(joystick, static_cast<int>(i)));
  }
}

}  // namespace

SdlEventObserverToken AddSdlEventObserver(SdlEventObserver observer) {
  if (!observer) {
    return 0;
  }
  auto& registry = GetRegistry();
  std::call_once(registry.watch_once, [&registry]() {
    registry.watch_installed.store(SDL_AddEventWatch(&Watch, nullptr),
                                   std::memory_order_release);
  });
  if (!registry.watch_installed.load(std::memory_order_acquire)) {
    return 0;
  }
  std::unique_lock lock(registry.mutex);
  const SdlEventObserverToken token = registry.next_token++;
  registry.entries.push_back({token, std::move(observer)});
  return token;
}

bool RemoveSdlEventObserver(SdlEventObserverToken token) {
  if (token == 0) {
    return false;
  }
  auto& registry = GetRegistry();
  SdlEventObserver removed;
  {
    // Exclusive: waits for every in-flight dispatch to finish.
    std::unique_lock lock(registry.mutex);
    const auto it = std::find_if(registry.entries.begin(), registry.entries.end(),
                                 [token](const Entry& entry) { return entry.token == token; });
    if (it == registry.entries.end()) {
      return false;
    }
    removed = std::move(it->observer);
    registry.entries.erase(it);
  }
  // The observer's captures are destroyed outside the registry lock.
  return true;
}

std::vector<SdlJoystickSnapshot> CaptureSdlJoysticks(bool include_values) {
  std::vector<SdlJoystickSnapshot> result;
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (!ids) {
    return result;
  }
  const size_t devices =
      std::min(static_cast<size_t>(std::max(count, 0)), kSdlJoystickSnapshotMaxDevices);
  result.resize(devices);
  for (size_t i = 0; i < devices; ++i) {
    FillSnapshot(ids[i], include_values, result[i]);
  }
  SDL_free(ids);
  return result;
}

std::optional<SdlJoystickSnapshot> QuerySdlJoystick(uint32_t instance_id, bool include_values) {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetJoysticks(&count);
  if (!ids) {
    return std::nullopt;
  }
  bool connected = false;
  for (int i = 0; i < count; ++i) {
    connected = connected || ids[i] == static_cast<SDL_JoystickID>(instance_id);
  }
  SDL_free(ids);
  if (!connected) {
    return std::nullopt;
  }
  SdlJoystickSnapshot snapshot;
  FillSnapshot(static_cast<SDL_JoystickID>(instance_id), include_values, snapshot);
  return snapshot;
}

namespace internal {

void DispatchSdlEventObserversForTesting(const SDL_Event& event) { Dispatch(event); }

size_t SdlEventObserverCountForTesting() {
  auto& registry = GetRegistry();
  std::shared_lock lock(registry.mutex);
  return registry.entries.size();
}

}  // namespace internal

}  // namespace rex::ui
