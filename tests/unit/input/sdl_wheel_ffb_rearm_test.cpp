/**
 * KI-022 regression coverage: batched XInputdFF start/stop entries and the
 * re-arm of infinite wheel effects that the device stopped behind the host.
 * The haptic layer is faked through the status/run callables the driver
 * passes to the shared helpers.
 */

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/input/input.h>
#include <rex/input/sdl/wheel_force_feedback_rearm.h>

using rex::input::ApplyForceFeedbackOperationBatch;
using rex::input::ClampForceFeedbackOperationCount;
using rex::input::ForceFeedbackEffectType;
using rex::input::ForceFeedbackOperationEntry;
using rex::input::sdl::ConsumeWheelRearmBudget;
using rex::input::sdl::FormatWheelRearmLimitLine;
using rex::input::sdl::FormatWheelRearmLine;
using rex::input::sdl::FormatWheelRetypeLine;
using rex::input::sdl::FormatWheelStartSuppressedLine;
using rex::input::sdl::IsWheelEffectRearmCandidate;
using rex::input::sdl::IsWheelEffectRetype;
using rex::input::sdl::kWheelRearmBudgetPerWindow;
using rex::input::sdl::kWheelRearmBudgetWindowMs;
using rex::input::sdl::kWheelRearmWatchdogIntervalMs;
using rex::input::sdl::RearmStoppedWheelEffect;
using rex::input::sdl::RearmStoppedWheelEffects;
using rex::input::sdl::ShouldLogWheelStartSuppressed;
using rex::input::sdl::ShouldRunWheelRearmWatchdog;
using rex::input::sdl::WheelRearmBudget;
using rex::input::sdl::WheelRearmBudgetResult;
using rex::input::sdl::WheelRearmEvent;

namespace {

constexpr uint32_t kInfinite = UINT32_MAX;

struct FakeSlot {
  int effect_id = -1;
  ForceFeedbackEffectType type = ForceFeedbackEffectType::kConstant;
  uint32_t iterations = 1;
  uint32_t duration_ms = 0;
  bool running = false;
  WheelRearmBudget rearm_budget;
};

using RunRecord = std::pair<int, uint32_t>;

struct FakeHaptic {
  std::map<int, bool> playing;
  std::vector<RunRecord> runs;
  std::vector<std::string> lines;
  bool run_succeeds = true;
  // Mirrors a device that never reports the effect as playing again.
  bool stays_stopped_after_run = false;

  auto status() {
    return [this](int effect_id) {
      const auto it = playing.find(effect_id);
      return it != playing.end() && it->second;
    };
  }
  auto run() {
    return [this](int effect_id, uint32_t iterations) {
      runs.emplace_back(effect_id, iterations);
      if (run_succeeds && !stays_stopped_after_run) {
        playing[effect_id] = true;
      }
      return run_succeeds;
    };
  }
  auto events(const char* reason) {
    return [this, reason](WheelRearmEvent event, size_t slot_number, ForceFeedbackEffectType type) {
      switch (event) {
        case WheelRearmEvent::kRearmed:
          lines.push_back(FormatWheelRearmLine(slot_number, type, reason));
          break;
        case WheelRearmEvent::kRunFailed:
          lines.push_back("run-failed");
          break;
        case WheelRearmEvent::kLimitReached:
          lines.push_back(FormatWheelRearmLimitLine(slot_number, type, reason));
          break;
      }
    };
  }
};

FakeSlot InfiniteSpring(int effect_id) {
  FakeSlot slot;
  slot.effect_id = effect_id;
  slot.type = ForceFeedbackEffectType::kSpring;
  slot.duration_ms = kInfinite;
  slot.running = true;
  return slot;
}

}  // namespace

TEST_CASE("XInputdFF operation count is bounded to the guest slot range",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(ClampForceFeedbackOperationCount(0) == 1);
  CHECK(ClampForceFeedbackOperationCount(1) == 1);
  CHECK(ClampForceFeedbackOperationCount(3) == 3);
  CHECK(ClampForceFeedbackOperationCount(64) == 64);
  CHECK(ClampForceFeedbackOperationCount(65) == 64);
  CHECK(ClampForceFeedbackOperationCount(0xFFFFFFFFu) == 64);
}

TEST_CASE("A batched XInputdFF start applies every entry", "[input][sdl][wheel][ffb][ki022]") {
  // {effect_index, iterations} pairs, in the layout the title passes.
  const std::array<uint32_t, 6> words = {7, 1, 3, 2, 12, 0};
  std::vector<ForceFeedbackOperationEntry> applied;
  const uint32_t result = ApplyForceFeedbackOperationBatch(
      3, true, [&](size_t word) { return words.at(word); },
      [&](const ForceFeedbackOperationEntry& entry) {
        applied.push_back(entry);
        return 0u;
      });
  CHECK(result == 0);
  REQUIRE(applied.size() == 3);
  CHECK(applied[0].effect_index == 7);
  CHECK(applied[0].iterations == 1);
  CHECK(applied[1].effect_index == 3);
  CHECK(applied[1].iterations == 2);
  CHECK(applied[2].effect_index == 12);
  CHECK(applied[2].iterations == 1);  // zero iterations normalise to one
}

TEST_CASE("A batched XInputdFF stop applies every entry with one iteration",
          "[input][sdl][wheel][ffb][ki022]") {
  const std::array<uint32_t, 4> words = {5, 99, 6, 99};
  std::vector<size_t> loaded;
  std::vector<ForceFeedbackOperationEntry> applied;
  ApplyForceFeedbackOperationBatch(
      2, false,
      [&](size_t word) {
        loaded.push_back(word);
        return words.at(word);
      },
      [&](const ForceFeedbackOperationEntry& entry) {
        applied.push_back(entry);
        return 0u;
      });
  REQUIRE(applied.size() == 2);
  CHECK(applied[0].effect_index == 5);
  CHECK(applied[1].effect_index == 6);
  CHECK(applied[0].iterations == 1);
  CHECK(applied[1].iterations == 1);
  // A stop never reads the iterations word.
  CHECK(loaded == std::vector<size_t>{0, 2});
}

TEST_CASE("A single-entry XInputdFF operation is unchanged", "[input][sdl][wheel][ffb][ki022]") {
  const std::array<uint32_t, 4> words = {9, 4, 10, 4};
  for (const uint32_t raw_count : {0u, 1u}) {
    std::vector<ForceFeedbackOperationEntry> applied;
    const uint32_t result = ApplyForceFeedbackOperationBatch(
        raw_count, true, [&](size_t word) { return words.at(word); },
        [&](const ForceFeedbackOperationEntry& entry) {
          applied.push_back(entry);
          return 0x48Fu;
        });
    CHECK(result == 0x48F);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].effect_index == 9);
    CHECK(applied[0].iterations == 4);
  }
}

TEST_CASE("A failing XInputdFF batch entry does not stop later entries",
          "[input][sdl][wheel][ffb][ki022]") {
  const std::array<uint32_t, 6> words = {1, 1, 2, 1, 3, 1};
  std::vector<uint32_t> applied;
  const uint32_t result = ApplyForceFeedbackOperationBatch(
      3, true, [&](size_t word) { return words.at(word); },
      [&](const ForceFeedbackOperationEntry& entry) {
        applied.push_back(entry.effect_index);
        return entry.effect_index == 2 ? 0x65Bu : (entry.effect_index == 3 ? 0x48Fu : 0u);
      });
  CHECK(applied == std::vector<uint32_t>{1, 2, 3});
  CHECK(result == 0x65B);  // first failure wins
}

TEST_CASE("Only running infinite effects are re-arm candidates",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(IsWheelEffectRearmCandidate(true, true, kInfinite));
  CHECK_FALSE(IsWheelEffectRearmCandidate(false, true, kInfinite));
  CHECK_FALSE(IsWheelEffectRearmCandidate(true, false, kInfinite));
  CHECK_FALSE(IsWheelEffectRearmCandidate(true, true, 250));
  CHECK_FALSE(IsWheelEffectRearmCandidate(true, true, 0));
}

TEST_CASE("A device-stopped infinite effect is re-armed once per update",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeHaptic haptic;
  FakeSlot slot = InfiniteSpring(4);
  haptic.playing[4] = false;  // the device stopped it behind the host

  CHECK(RearmStoppedWheelEffect(slot, 2, 1000, haptic.status(), haptic.run(),
                                haptic.events("update")));
  REQUIRE(haptic.runs.size() == 1);
  CHECK(haptic.runs[0] == RunRecord{4, 1});
  REQUIRE(haptic.lines.size() == 1);
  CHECK(haptic.lines[0] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=rearm slot=2 type=spring reason=update");
  CHECK(slot.running);

  // The next update finds it playing again: nothing is re-articulated.
  CHECK_FALSE(RearmStoppedWheelEffect(slot, 2, 1016, haptic.status(), haptic.run(),
                                      haptic.events("update")));
  CHECK(haptic.runs.size() == 1);

  // Stopped again by the device: the next update re-arms it again, once.
  haptic.playing[4] = false;
  CHECK(RearmStoppedWheelEffect(slot, 2, 1032, haptic.status(), haptic.run(),
                                haptic.events("update")));
  CHECK(haptic.runs.size() == 2);
}

TEST_CASE("Finite, title-stopped and playing effects are never re-armed",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeHaptic haptic;
  std::array<FakeSlot, 5> slots = {};
  // Slot 1: finite collision effect that expired but is still marked running.
  slots[0].effect_id = 1;
  slots[0].type = ForceFeedbackEffectType::kSquare;
  slots[0].duration_ms = 300;
  slots[0].running = true;
  // Slot 2: infinite spring the title stopped.
  slots[1] = InfiniteSpring(2);
  slots[1].running = false;
  // Slot 3: infinite periodic that is genuinely playing.
  slots[2].effect_id = 3;
  slots[2].type = ForceFeedbackEffectType::kSine;
  slots[2].duration_ms = kInfinite;
  slots[2].running = true;
  haptic.playing[3] = true;
  // Slot 4: never created.
  slots[3].duration_ms = kInfinite;
  slots[3].running = true;
  // Slot 5: infinite damper the device stopped.
  slots[4].effect_id = 5;
  slots[4].type = ForceFeedbackEffectType::kDamper;
  slots[4].duration_ms = kInfinite;
  slots[4].running = true;

  CHECK(RearmStoppedWheelEffects(slots, 5000, haptic.status(), haptic.run(),
                                 haptic.events("watchdog")) == 1);
  REQUIRE(haptic.runs.size() == 1);
  CHECK(haptic.runs[0].first == 5);
  REQUIRE(haptic.lines.size() == 1);
  CHECK(haptic.lines[0] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=rearm slot=5 type=damper reason=watchdog");
}

TEST_CASE("The re-arm watchdog runs at most once per second",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(kWheelRearmWatchdogIntervalMs == 1000);
  CHECK_FALSE(ShouldRunWheelRearmWatchdog(1999, 1000));
  CHECK(ShouldRunWheelRearmWatchdog(2000, 1000));

  // Simulate the guest polling GetState every 16 ms for three seconds while
  // the device keeps dropping the spring between watchdog passes.
  FakeHaptic haptic;
  std::array<FakeSlot, 2> slots = {InfiniteSpring(1), FakeSlot{}};
  slots[1].effect_id = 2;
  slots[1].type = ForceFeedbackEffectType::kSquare;
  slots[1].duration_ms = 200;
  slots[1].running = true;
  uint64_t last_check_ms = 10000;
  uint32_t passes = 0;
  for (uint64_t now_ms = 10000; now_ms <= 13000; now_ms += 16) {
    haptic.playing[1] = false;
    haptic.playing[2] = false;
    if (ShouldRunWheelRearmWatchdog(now_ms, last_check_ms)) {
      last_check_ms = now_ms;
      passes++;
      RearmStoppedWheelEffects(slots, now_ms, haptic.status(), haptic.run(),
                               haptic.events("watchdog"));
    }
  }
  CHECK(passes == 2);
  CHECK(haptic.runs.size() == 2);
  for (const auto& record : haptic.runs) {
    CHECK(record.first == 1);  // the finite square is never touched
  }
}

TEST_CASE("The per-slot re-arm budget stops a device that always reports stopped",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeHaptic haptic;
  haptic.stays_stopped_after_run = true;
  FakeSlot slot = InfiniteSpring(8);

  uint64_t now_ms = 50000;
  for (uint32_t i = 0; i < kWheelRearmBudgetPerWindow; ++i, now_ms += 1000) {
    CHECK(RearmStoppedWheelEffect(slot, 3, now_ms, haptic.status(), haptic.run(),
                                  haptic.events("watchdog")));
  }
  CHECK(haptic.runs.size() == kWheelRearmBudgetPerWindow);

  // Limit reached: one rearm-limit line, then silence, no further runs.
  CHECK_FALSE(RearmStoppedWheelEffect(slot, 3, now_ms, haptic.status(), haptic.run(),
                                      haptic.events("watchdog")));
  CHECK_FALSE(RearmStoppedWheelEffect(slot, 3, now_ms + 1000, haptic.status(), haptic.run(),
                                      haptic.events("watchdog")));
  CHECK(haptic.runs.size() == kWheelRearmBudgetPerWindow);
  REQUIRE(haptic.lines.size() == kWheelRearmBudgetPerWindow + 1);
  CHECK(haptic.lines.back() ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=rearm-limit slot=3 type=spring reason=watchdog "
        "limit=10 window_ms=60000");

  // A new window allows re-arming again.
  CHECK(RearmStoppedWheelEffect(slot, 3, 50000 + kWheelRearmBudgetWindowMs, haptic.status(),
                                haptic.run(), haptic.events("watchdog")));
  CHECK(haptic.runs.size() == kWheelRearmBudgetPerWindow + 1);
}

TEST_CASE("A failed re-arm keeps the running belief and consumes the budget",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeHaptic haptic;
  haptic.run_succeeds = false;
  FakeSlot slot = InfiniteSpring(6);
  CHECK_FALSE(RearmStoppedWheelEffect(slot, 1, 100, haptic.status(), haptic.run(),
                                      haptic.events("watchdog")));
  CHECK(slot.running);
  CHECK(slot.rearm_budget.attempts == 1);
  REQUIRE(haptic.lines.size() == 1);
  CHECK(haptic.lines[0] == "run-failed");
}

TEST_CASE("The re-arm budget window resets after a minute", "[input][sdl][wheel][ffb][ki022]") {
  WheelRearmBudget budget;
  for (uint32_t i = 0; i < kWheelRearmBudgetPerWindow; ++i) {
    CHECK(ConsumeWheelRearmBudget(budget, 100 + i) == WheelRearmBudgetResult::kAllowed);
  }
  CHECK(ConsumeWheelRearmBudget(budget, 200) == WheelRearmBudgetResult::kLimitReached);
  CHECK(ConsumeWheelRearmBudget(budget, 300) == WheelRearmBudgetResult::kLimitedSilently);
  CHECK(ConsumeWheelRearmBudget(budget, 100 + kWheelRearmBudgetWindowMs) ==
        WheelRearmBudgetResult::kAllowed);
  CHECK(budget.attempts == 1);
  CHECK_FALSE(budget.limit_logged);
}

TEST_CASE("Suppressed infinite starts and retypes produce diagnostic lines",
          "[input][sdl][wheel][ffb][ki022]") {
  bool logged = false;
  CHECK(ShouldLogWheelStartSuppressed(logged));
  CHECK_FALSE(ShouldLogWheelStartSuppressed(logged));
  CHECK(FormatWheelStartSuppressedLine(4, ForceFeedbackEffectType::kSpring) ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=start-suppressed slot=4 type=spring "
        "reason=status-playing");

  CHECK(IsWheelEffectRetype(true, true, ForceFeedbackEffectType::kSpring,
                            ForceFeedbackEffectType::kSquare));
  CHECK_FALSE(IsWheelEffectRetype(false, true, ForceFeedbackEffectType::kSpring,
                                  ForceFeedbackEffectType::kSquare));
  CHECK_FALSE(IsWheelEffectRetype(true, false, ForceFeedbackEffectType::kSpring,
                                  ForceFeedbackEffectType::kSquare));
  CHECK_FALSE(IsWheelEffectRetype(true, true, ForceFeedbackEffectType::kSpring,
                                  ForceFeedbackEffectType::kSpring));
  CHECK(FormatWheelRetypeLine(1, ForceFeedbackEffectType::kSpring,
                              ForceFeedbackEffectType::kSquare, true) ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=retype slot=1 from=spring to=square restart=1");
}
