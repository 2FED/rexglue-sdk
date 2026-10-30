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
#include <rex/input/sdl/wheel_force_feedback_resync.h>

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
using rex::input::sdl::AnyWheelEffectResyncCandidate;
using rex::input::sdl::ConsumeWheelResyncBudget;
using rex::input::sdl::FormatWheelResyncLimitLine;
using rex::input::sdl::FormatWheelResyncLine;
using rex::input::sdl::kWheelResyncBudgetPerWindow;
using rex::input::sdl::kWheelResyncBudgetWindowMs;
using rex::input::sdl::kWheelResyncBurstMinStarts;
using rex::input::sdl::kWheelResyncBurstQuietMs;
using rex::input::sdl::kWheelResyncBurstWindowMs;
using rex::input::sdl::kWheelResyncPeriodicIntervalMs;
using rex::input::sdl::kWheelResyncRequestExpiryMs;
using rex::input::sdl::NormalizeWheelResyncHostReason;
using rex::input::sdl::ResyncWheelEffect;
using rex::input::sdl::ResyncWheelEffects;
using rex::input::sdl::ShouldRunWheelPeriodicResync;
using rex::input::sdl::WheelFiniteBurstTracker;
using rex::input::sdl::WheelResyncBudget;
using rex::input::sdl::WheelResyncEvent;
using rex::input::sdl::WheelResyncRequestSlot;
using rex::input::sdl::WheelResyncTrigger;

namespace {

constexpr uint32_t kInfinite = UINT32_MAX;

struct FakeSlot {
  int effect_id = -1;
  ForceFeedbackEffectType type = ForceFeedbackEffectType::kConstant;
  uint32_t iterations = 1;
  uint32_t duration_ms = 0;
  bool running = false;
  WheelRearmBudget rearm_budget;
  WheelResyncBudget resync_budget;
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

namespace {

// Device that drops nothing visibly: status always reads "playing", which is
// exactly the T300 case the status re-arm cannot see.
struct FakeResyncHaptic {
  std::vector<int> stops;
  std::vector<RunRecord> runs;
  std::vector<std::string> lines;
  std::vector<std::string> ops;
  uint32_t stop_alls = 0;
  bool run_succeeds = true;
  bool stop_all_succeeds = true;

  auto stop_all() {
    return [this]() {
      stop_alls++;
      ops.push_back("stop_all");
      return stop_all_succeeds;
    };
  }
  auto stop() {
    return [this](int effect_id) {
      stops.push_back(effect_id);
      ops.push_back("stop " + std::to_string(effect_id));
      return true;
    };
  }
  auto run() {
    return [this](int effect_id, uint32_t iterations) {
      runs.emplace_back(effect_id, iterations);
      ops.push_back("run " + std::to_string(effect_id) + " x" + std::to_string(iterations));
      return run_succeeds;
    };
  }
  auto events(std::string reason) {
    return [this, reason](WheelResyncEvent event, size_t slot_number,
                          ForceFeedbackEffectType type) {
      switch (event) {
        case WheelResyncEvent::kResynced:
          lines.push_back(FormatWheelResyncLine(slot_number, type, reason));
          break;
        case WheelResyncEvent::kRunFailed:
          lines.push_back("run-failed");
          break;
        case WheelResyncEvent::kLimitReached:
          lines.push_back(FormatWheelResyncLimitLine(slot_number, type, reason));
          break;
        case WheelResyncEvent::kStopAllFailed:
          lines.push_back("stop-all-failed");
          break;
      }
    };
  }
};

std::array<FakeSlot, 4> RaceSlots() {
  std::array<FakeSlot, 4> slots = {};
  slots[0] = InfiniteSpring(11);  // centering
  slots[1].effect_id = 12;        // finite collision square, still marked running
  slots[1].type = ForceFeedbackEffectType::kSquare;
  slots[1].duration_ms = 150;
  slots[1].running = true;
  slots[2].effect_id = 13;  // infinite damper the title stopped
  slots[2].type = ForceFeedbackEffectType::kDamper;
  slots[2].duration_ms = kInfinite;
  slots[2].running = false;
  slots[3].effect_id = 14;  // infinite engine periodic
  slots[3].type = ForceFeedbackEffectType::kSquare;
  slots[3].duration_ms = kInfinite;
  slots[3].running = true;
  return slots;
}

}  // namespace

TEST_CASE("A host resync request restarts believed-running infinite slots once",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeResyncHaptic haptic;
  auto slots = RaceSlots();
  CHECK(AnyWheelEffectResyncCandidate(slots));

  // The device would report every effect as playing; the resync ignores it.
  CHECK(ResyncWheelEffects(slots, 5000, WheelResyncTrigger::kHost, haptic.stop_all(),
                           haptic.stop(), haptic.run(), haptic.events("host:resume")) == 2);
  CHECK(haptic.ops == std::vector<std::string>{"stop_all", "run 11 x1", "run 14 x1"});
  CHECK(haptic.stops.empty());
  REQUIRE(haptic.runs.size() == 2);
  CHECK(haptic.runs[0] == RunRecord{11, 1});
  CHECK(haptic.runs[1] == RunRecord{14, 1});
  REQUIRE(haptic.lines.size() == 2);
  CHECK(haptic.lines[0] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=1 type=spring reason=host:resume");
  CHECK(haptic.lines[1] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=4 type=square reason=host:resume");
  // Finite and title-stopped effects keep their state and are untouched.
  CHECK(slots[1].running);
  CHECK_FALSE(slots[2].running);
  CHECK(slots[1].resync_budget.attempts == 0);
  CHECK(slots[2].resync_budget.attempts == 0);
}

TEST_CASE("Host resync requests are coalesced, normalised and expire",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(NormalizeWheelResyncHostReason("host:resume") == "resume");
  CHECK(NormalizeWheelResyncHostReason("racer-active") == "racer-active");
  CHECK(NormalizeWheelResyncHostReason("photo close!") == "photo-close-");
  CHECK(NormalizeWheelResyncHostReason("") == "request");
  CHECK(NormalizeWheelResyncHostReason(nullptr) == "request");
  CHECK(NormalizeWheelResyncHostReason(std::string(80, 'a').c_str()).size() == 32);

  WheelResyncRequestSlot request;
  std::string reason;
  CHECK_FALSE(request.Take(100, reason));
  CHECK(request.Offer("resume", 1000));
  CHECK_FALSE(request.Offer("racer-active", 1100));  // coalesced, first reason kept
  CHECK(request.Take(1200, reason));
  CHECK(reason == "resume");
  CHECK_FALSE(request.Take(1216, reason));  // one pass per request

  CHECK(request.Offer("photo-close", 2000));
  CHECK_FALSE(request.Take(2000 + kWheelResyncRequestExpiryMs + 1, reason));
  // An expired pending request is replaced by a new one.
  CHECK(request.Offer("resume", 5000));
  CHECK(request.Offer("racer-active", 5000 + kWheelResyncRequestExpiryMs + 1));
  CHECK(request.Take(5000 + kWheelResyncRequestExpiryMs + 2, reason));
  CHECK(reason == "racer-active");
}

TEST_CASE("A burst of finite starts arms one resync after it ends",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(kWheelResyncBurstMinStarts == 4);
  CHECK(kWheelResyncBurstWindowMs == 2000);
  CHECK(kWheelResyncBurstQuietMs == 300);

  WheelFiniteBurstTracker tracker;
  // Three starts are not a burst.
  tracker.OnFiniteStart(1000);
  tracker.OnFiniteStart(1100);
  tracker.OnFiniteStart(1200);
  CHECK_FALSE(tracker.armed);
  CHECK_FALSE(tracker.ConsumeBurstEnd(5000));

  // Four starts spread over more than two seconds are not a burst either.
  WheelFiniteBurstTracker slow;
  for (uint64_t t = 0; t < 4; ++t) {
    slow.OnFiniteStart(10000 + t * 700);
  }
  CHECK_FALSE(slow.armed);

  // The race-start pattern: 17 square starts at about frame rate.
  WheelFiniteBurstTracker race;
  uint64_t now_ms = 20000;
  for (int i = 0; i < 17; ++i) {
    race.OnFiniteStart(now_ms);
    CHECK_FALSE(race.ConsumeBurstEnd(now_ms + 16));  // still inside the burst
    now_ms += 45;
  }
  const uint64_t last_start = now_ms - 45;
  CHECK(race.armed);
  CHECK_FALSE(race.ConsumeBurstEnd(last_start + kWheelResyncBurstQuietMs - 1));
  CHECK(race.ConsumeBurstEnd(last_start + kWheelResyncBurstQuietMs));
  CHECK_FALSE(race.ConsumeBurstEnd(last_start + kWheelResyncBurstQuietMs + 16));  // once
  // A single later start does not re-arm.
  race.OnFiniteStart(last_start + 3000);
  CHECK_FALSE(race.armed);
}

TEST_CASE("The periodic resync runs every ten seconds and leaves a reserve",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(kWheelResyncPeriodicIntervalMs == 10000);
  CHECK_FALSE(ShouldRunWheelPeriodicResync(19999, 10000));
  CHECK(ShouldRunWheelPeriodicResync(20000, 10000));

  // Guest polling at 16 ms for 45 s with the spring believed running.
  FakeResyncHaptic haptic;
  auto slots = RaceSlots();
  uint64_t last_resync_ms = 100000;
  uint32_t passes = 0;
  for (uint64_t now_ms = 100000; now_ms <= 145000; now_ms += 16) {
    if (ShouldRunWheelPeriodicResync(now_ms, last_resync_ms)) {
      ResyncWheelEffects(slots, now_ms, WheelResyncTrigger::kPeriodic, haptic.stop_all(),
                         haptic.stop(), haptic.run(), haptic.events("periodic"));
      last_resync_ms = now_ms;
      passes++;
    }
  }
  CHECK(passes == 4);
  CHECK(haptic.stop_alls == 4);
  CHECK(haptic.runs.size() == 8);  // spring and engine periodic, four passes each
  for (const auto& record : haptic.runs) {
    CHECK((record.first == 11 || record.first == 14));
  }

  // A fifth periodic pass in the window is allowed, a sixth would use the
  // reserve kept for host/burst triggers and is skipped silently.
  FakeResyncHaptic reserve;
  FakeSlot spring = InfiniteSpring(1);
  for (uint32_t i = 0; i < kWheelResyncBudgetPerWindow - 1; ++i) {
    CHECK(ResyncWheelEffect(spring, 1, 1000 + i * 10000, WheelResyncTrigger::kPeriodic,
                            reserve.stop(), reserve.run(), reserve.events("periodic")));
  }
  CHECK_FALSE(ResyncWheelEffect(spring, 1, 51000, WheelResyncTrigger::kPeriodic, reserve.stop(),
                                reserve.run(), reserve.events("periodic")));
  CHECK(reserve.lines.size() == kWheelResyncBudgetPerWindow - 1);
  CHECK(ResyncWheelEffect(spring, 1, 52000, WheelResyncTrigger::kHost, reserve.stop(),
                          reserve.run(), reserve.events("host:resume")));
  CHECK(reserve.runs.size() == kWheelResyncBudgetPerWindow);
}

TEST_CASE("The per-slot resync budget allows six per minute with one limit line",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(kWheelResyncBudgetPerWindow == 6);
  CHECK(kWheelResyncBudgetWindowMs == 60000);
  FakeResyncHaptic haptic;
  FakeSlot spring = InfiniteSpring(7);
  for (uint32_t i = 0; i < 10; ++i) {
    ResyncWheelEffect(spring, 3, 1000 + i * 100, WheelResyncTrigger::kHost, haptic.stop(),
                      haptic.run(), haptic.events("host:resume"));
  }
  CHECK(haptic.runs.size() == 6);
  CHECK(haptic.stops.size() == 6);
  REQUIRE(haptic.lines.size() == 7);
  CHECK(haptic.lines[6] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync-limit slot=3 type=spring reason=host:resume "
        "limit=6 window_ms=60000");
  // The next window allows resyncs again.
  CHECK(ResyncWheelEffect(spring, 3, 1000 + kWheelResyncBudgetWindowMs, WheelResyncTrigger::kBurst,
                          haptic.stop(), haptic.run(), haptic.events("burst")));
  CHECK(haptic.lines.back() ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=3 type=spring reason=burst");

  // The resync budget is independent of the status re-arm budget.
  WheelResyncBudget budget;
  for (uint32_t i = 0; i < kWheelResyncBudgetPerWindow; ++i) {
    CHECK(ConsumeWheelResyncBudget(budget, 10) == WheelRearmBudgetResult::kAllowed);
  }
  CHECK(ConsumeWheelResyncBudget(budget, 20) == WheelRearmBudgetResult::kLimitReached);
  CHECK(ConsumeWheelResyncBudget(budget, 30) == WheelRearmBudgetResult::kLimitedSilently);
  CHECK(spring.rearm_budget.attempts == 0);
}

TEST_CASE("A failed resync run keeps the running belief and finite slots stay untouched",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeResyncHaptic haptic;
  haptic.run_succeeds = false;
  FakeSlot spring = InfiniteSpring(9);
  CHECK_FALSE(ResyncWheelEffect(spring, 1, 1000, WheelResyncTrigger::kHost, haptic.stop(),
                                haptic.run(), haptic.events("host:photo-close")));
  CHECK(spring.running);
  CHECK(spring.resync_budget.attempts == 1);
  REQUIRE(haptic.lines.size() == 1);
  CHECK(haptic.lines[0] == "run-failed");

  FakeResyncHaptic finite_haptic;
  std::array<FakeSlot, 2> finite = {};
  finite[0].effect_id = 1;
  finite[0].type = ForceFeedbackEffectType::kConstant;
  finite[0].duration_ms = 150;
  finite[0].running = true;
  finite[1].effect_id = 2;
  finite[1].type = ForceFeedbackEffectType::kSquare;
  finite[1].duration_ms = 150;
  finite[1].running = true;
  CHECK_FALSE(AnyWheelEffectResyncCandidate(finite));
  for (auto trigger :
       {WheelResyncTrigger::kHost, WheelResyncTrigger::kBurst, WheelResyncTrigger::kPeriodic}) {
    CHECK(ResyncWheelEffects(finite, 1000, trigger, finite_haptic.stop_all(), finite_haptic.stop(),
                             finite_haptic.run(), finite_haptic.events("burst")) == 0);
  }
  CHECK(finite_haptic.stop_alls == 0);  // no candidate: the device is not stopped
  CHECK(finite_haptic.stops.empty());
  CHECK(finite_haptic.runs.empty());
  CHECK(finite_haptic.lines.empty());
}

TEST_CASE("A resync stops the device once, then runs each believed-running infinite slot",
          "[input][sdl][wheel][ffb][ki022]") {
  // The T300 baseline at the race start: spring, constant (gain 0 by policy,
  // still believed running), damper, plus finite event slots.
  std::array<FakeSlot, 6> slots = {};
  slots[0] = InfiniteSpring(21);
  slots[1].effect_id = 22;
  slots[1].type = ForceFeedbackEffectType::kConstant;
  slots[1].duration_ms = kInfinite;
  slots[1].running = true;
  slots[2].effect_id = 23;
  slots[2].type = ForceFeedbackEffectType::kDamper;
  slots[2].duration_ms = kInfinite;
  slots[2].running = true;
  slots[3].effect_id = 24;
  slots[3].type = ForceFeedbackEffectType::kSquare;
  slots[3].duration_ms = 150;
  slots[3].running = true;
  slots[4].effect_id = 25;
  slots[4].type = ForceFeedbackEffectType::kConstant;
  slots[4].duration_ms = 150;
  slots[4].running = true;
  slots[5].effect_id = 26;
  slots[5].type = ForceFeedbackEffectType::kSquare;
  slots[5].duration_ms = kInfinite;
  slots[5].running = true;
  slots[5].iterations = 3;

  std::vector<std::string> ops;
  uint32_t status_queries = 0;
  auto stop_all = [&]() {
    ops.push_back("stop_all");
    return true;
  };
  auto stop = [&](int effect_id) {
    ops.push_back("stop " + std::to_string(effect_id));
    return true;
  };
  auto run = [&](int effect_id, uint32_t iterations) {
    ops.push_back("run " + std::to_string(effect_id) + " x" + std::to_string(iterations));
    return true;
  };
  std::vector<std::string> lines;
  auto events = [&](WheelResyncEvent event, size_t slot_number, ForceFeedbackEffectType type) {
    if (event == WheelResyncEvent::kResynced) {
      lines.push_back(FormatWheelResyncLine(slot_number, type, "burst"));
    }
  };
  // A device that keeps reporting every effect as playing: the status re-arm
  // does nothing, the resync ignores the status entirely.
  auto always_playing = [&](int) {
    status_queries++;
    return true;
  };
  FakeHaptic rearm_haptic;
  CHECK(RearmStoppedWheelEffects(slots, 1000, always_playing, rearm_haptic.run(),
                                 rearm_haptic.events("watchdog")) == 0);
  CHECK(rearm_haptic.runs.empty());
  const uint32_t queries_before = status_queries;

  CHECK(ResyncWheelEffects(slots, 1000, WheelResyncTrigger::kBurst, stop_all, stop, run,
                           events) == 4);
  CHECK(status_queries == queries_before);  // no status query at all
  // Exactly the focus-resume sequence: one global stop, then the runs in slot
  // order with the stored iterations; finite slots are not run again.
  CHECK(ops == std::vector<std::string>{"stop_all", "run 21 x1", "run 22 x1", "run 23 x1",
                                        "run 26 x3"});
  REQUIRE(lines.size() == 4);
  CHECK(lines[1] == "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=2 type=constant reason=burst");
  CHECK(lines[3] == "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=6 type=square reason=burst");
}

TEST_CASE("The race-start burst resyncs once after it settles, not inside it",
          "[input][sdl][wheel][ffb][ki022]") {
  CHECK(kWheelResyncBurstQuietMs == 300);
  // 28 finite square starts on one slot within 0.76 s (often two in the same
  // millisecond) while the guest polls GetState every 16 ms.
  std::vector<uint64_t> starts;
  for (uint64_t frame = 0; frame < 13; ++frame) {
    starts.push_back(50000 + frame * 55);
    starts.push_back(50000 + frame * 55);
  }
  starts.push_back(50000 + 13 * 55);
  starts.push_back(50000 + 13 * 55 + 40);
  REQUIRE(starts.size() == 28);
  const uint64_t last_start = starts.back();
  REQUIRE(last_start - starts.front() <= 800);

  WheelFiniteBurstTracker tracker;
  size_t next_start = 0;
  std::vector<uint64_t> fired;
  for (uint64_t now_ms = 49990; now_ms <= 53000; now_ms += 16) {
    while (next_start < starts.size() && starts[next_start] <= now_ms) {
      tracker.OnFiniteStart(starts[next_start++]);
    }
    if (tracker.ConsumeBurstEnd(now_ms)) {
      fired.push_back(now_ms);
    }
  }
  REQUIRE(fired.size() == 1);
  CHECK(fired[0] >= last_start + kWheelResyncBurstQuietMs);
  CHECK(fired[0] < last_start + kWheelResyncBurstQuietMs + 16);
}

TEST_CASE("Later updates that neutralise or shorten an effect are logged, rate limited",
          "[input][sdl][wheel][ffb][ki022]") {
  using rex::input::ForceFeedbackEffect;
  using rex::input::sdl::FormatWheelUpdateChangeLimitLine;
  using rex::input::sdl::FormatWheelUpdateChangeLine;
  using rex::input::sdl::IsWheelEffectNeutral;
  using rex::input::sdl::kWheelUpdateChangeLogPerWindow;
  using rex::input::sdl::ObserveWheelUpdateChange;
  using rex::input::sdl::ResetWheelUpdateChange;
  using rex::input::sdl::WheelUpdateChangeLog;
  using rex::input::sdl::WheelUpdateChangeState;

  ForceFeedbackEffect spring;
  spring.type = ForceFeedbackEffectType::kSpring;
  spring.duration_ms = kInfinite;
  spring.conditions[0].right_coefficient = 89;
  spring.conditions[0].left_coefficient = 89;
  spring.conditions[0].right_saturation = 178;
  spring.conditions[0].left_saturation = 178;
  CHECK_FALSE(IsWheelEffectNeutral(spring, 100));
  CHECK(IsWheelEffectNeutral(spring, 0));
  ForceFeedbackEffect zero_spring = spring;
  zero_spring.conditions[0].right_coefficient = 0;
  zero_spring.conditions[0].left_coefficient = 0;
  CHECK(IsWheelEffectNeutral(zero_spring, 100));

  ForceFeedbackEffect periodic;
  periodic.type = ForceFeedbackEffectType::kSquare;
  periodic.periodic_magnitude = 38;
  CHECK_FALSE(IsWheelEffectNeutral(periodic, 40));
  periodic.periodic_magnitude = 0;
  CHECK(IsWheelEffectNeutral(periodic, 40));
  ForceFeedbackEffect constant;
  constant.type = ForceFeedbackEffectType::kConstant;
  constant.constant_level = -25;
  CHECK_FALSE(IsWheelEffectNeutral(constant, 100));
  CHECK(IsWheelEffectNeutral(constant, 0));  // continuous constant policy gain 0

  WheelUpdateChangeState state;
  ResetWheelUpdateChange(state, false, false);
  // Ordinary speed-dependent updates keep the state: nothing logged.
  for (int i = 0; i < 100; ++i) {
    CHECK(ObserveWheelUpdateChange(state, false, false, 1000 + i) == WheelUpdateChangeLog::kNone);
  }
  CHECK(ObserveWheelUpdateChange(state, true, false, 2000) == WheelUpdateChangeLog::kLog);
  CHECK(ObserveWheelUpdateChange(state, true, false, 2016) == WheelUpdateChangeLog::kNone);
  CHECK(ObserveWheelUpdateChange(state, false, true, 2032) == WheelUpdateChangeLog::kLog);
  // Flapping is bounded: six lines per minute, then one limit line.
  uint32_t logged = 2;
  uint32_t limits = 0;
  for (int i = 0; i < 20; ++i) {
    const auto result = ObserveWheelUpdateChange(state, i % 2 == 0, false, 3000 + i);
    logged += result == WheelUpdateChangeLog::kLog ? 1 : 0;
    limits += result == WheelUpdateChangeLog::kLimitReached ? 1 : 0;
  }
  CHECK(logged == kWheelUpdateChangeLogPerWindow);
  CHECK(limits == 1);

  CHECK(FormatWheelUpdateChangeLine(1, ForceFeedbackEffectType::kSpring, true, kInfinite, 100) ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=update-change slot=1 type=spring neutral=1 finite=0 "
        "duration=4294967295 gain_percent=100");
  CHECK(FormatWheelUpdateChangeLimitLine(3, ForceFeedbackEffectType::kDamper) ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=update-change-limit slot=3 type=damper limit=6 "
        "window_ms=60000");
}

TEST_CASE("A failed global stop falls back to per-slot stop and run",
          "[input][sdl][wheel][ffb][ki022]") {
  FakeResyncHaptic haptic;
  haptic.stop_all_succeeds = false;
  auto slots = RaceSlots();
  CHECK(ResyncWheelEffects(slots, 5000, WheelResyncTrigger::kHost, haptic.stop_all(),
                           haptic.stop(), haptic.run(), haptic.events("host:resume")) == 2);
  CHECK(haptic.ops == std::vector<std::string>{"stop_all", "stop 11", "run 11 x1", "stop 14",
                                               "run 14 x1"});
  REQUIRE(haptic.lines.size() == 3);
  CHECK(haptic.lines[0] == "stop-all-failed");
  CHECK(slots[0].resync_budget.attempts == 1);
  CHECK(slots[3].resync_budget.attempts == 1);
}

TEST_CASE("A slot over budget is never stopped by a global stop",
          "[input][sdl][wheel][ffb][ki022]") {
  auto slots = RaceSlots();
  // The spring used its whole budget; the engine periodic was created later.
  for (uint32_t i = 0; i < kWheelResyncBudgetPerWindow; ++i) {
    ConsumeWheelResyncBudget(slots[0].resync_budget, 1000);
  }
  FakeResyncHaptic periodic;
  CHECK(ResyncWheelEffects(slots, 2000, WheelResyncTrigger::kPeriodic, periodic.stop_all(),
                           periodic.stop(), periodic.run(), periodic.events("periodic")) == 0);
  CHECK(periodic.ops.empty());  // the periodic pass is skipped as a whole

  FakeResyncHaptic host;
  CHECK(ResyncWheelEffects(slots, 2000, WheelResyncTrigger::kHost, host.stop_all(), host.stop(),
                           host.run(), host.events("host:resume")) == 1);
  // No global stop: the spring keeps playing, the periodic slot is resynced
  // on its own, and the spring logs its limit once.
  CHECK(host.ops == std::vector<std::string>{"stop 14", "run 14 x1"});
  REQUIRE(host.lines.size() == 2);
  CHECK(host.lines[0] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync-limit slot=1 type=spring reason=host:resume "
        "limit=6 window_ms=60000");
  CHECK(host.lines[1] ==
        "SDL_WHEEL_FFB_EFFECT v=2 action=resync slot=4 type=square reason=host:resume");
}
