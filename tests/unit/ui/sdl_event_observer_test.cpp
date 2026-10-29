#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include <rex/ui/sdl_event_observer.h>

namespace {

SDL_Event ButtonEvent(uint32_t which, uint8_t button, bool down) {
  SDL_Event event{};
  event.type = down ? SDL_EVENT_JOYSTICK_BUTTON_DOWN : SDL_EVENT_JOYSTICK_BUTTON_UP;
  event.jbutton.which = which;
  event.jbutton.button = button;
  event.jbutton.down = down;
  return event;
}

}  // namespace

TEST_CASE("SDL event observers see dispatched events until removed", "[ui][sdl_event_observer]") {
  using namespace rex::ui;
  const size_t before = internal::SdlEventObserverCountForTesting();
  REQUIRE(AddSdlEventObserver({}) == 0);
  REQUIRE(internal::SdlEventObserverCountForTesting() == before);

  std::vector<uint8_t> first_seen;
  int second_calls = 0;
  const auto first = AddSdlEventObserver([&](const SDL_Event& event) {
    if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
      first_seen.push_back(event.jbutton.button);
    }
  });
  const auto second = AddSdlEventObserver([&](const SDL_Event&) { ++second_calls; });
  REQUIRE(first != 0);
  REQUIRE(second != 0);
  REQUIRE(first != second);
  REQUIRE(internal::SdlEventObserverCountForTesting() == before + 2);

  internal::DispatchSdlEventObserversForTesting(ButtonEvent(7, 8, true));
  internal::DispatchSdlEventObserversForTesting(ButtonEvent(7, 8, false));
  REQUIRE(first_seen == std::vector<uint8_t>{8});
  REQUIRE(second_calls == 2);

  REQUIRE(RemoveSdlEventObserver(first));
  REQUIRE_FALSE(RemoveSdlEventObserver(first));
  REQUIRE_FALSE(RemoveSdlEventObserver(0));
  internal::DispatchSdlEventObserversForTesting(ButtonEvent(7, 9, true));
  REQUIRE(first_seen == std::vector<uint8_t>{8});
  REQUIRE(second_calls == 3);
  REQUIRE(RemoveSdlEventObserver(second));
  REQUIRE(internal::SdlEventObserverCountForTesting() == before);
}

TEST_CASE("SDL event observer removal waits for in-flight dispatch", "[ui][sdl_event_observer]") {
  using namespace rex::ui;
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  std::atomic<bool> finished{false};
  const auto token = AddSdlEventObserver([&](const SDL_Event&) {
    entered.store(true);
    while (!release.load()) {
      std::this_thread::yield();
    }
    finished.store(true);
  });
  REQUIRE(token != 0);
  std::thread dispatcher(
      [] { internal::DispatchSdlEventObserversForTesting(ButtonEvent(1, 0, true)); });
  while (!entered.load()) {
    std::this_thread::yield();
  }
  std::atomic<bool> removed{false};
  std::atomic<bool> remove_result{false};
  std::thread remover([&] {
    remove_result.store(RemoveSdlEventObserver(token));
    removed.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  REQUIRE_FALSE(removed.load());
  release.store(true);
  remover.join();
  dispatcher.join();
  REQUIRE(finished.load());
  REQUIRE(removed.load());
  REQUIRE(remove_result.load());
}
