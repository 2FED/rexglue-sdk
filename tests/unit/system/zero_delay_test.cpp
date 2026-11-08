// l81 (KI-060): repeated zero guest delays (ke_delay_zero_mode=backoff): the
// streak and backoff decision and the guest call-site walk.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <string>

#include <rex/system/zero_delay.h>

using rex::system::GuestCallerSite;
using rex::system::kZeroDelayBackoffStartUs;
using rex::system::MakeZeroDelayConfig;
using rex::system::NoteZeroDelayEnd;
using rex::system::ParseZeroDelayMode;
using rex::system::PlanZeroDelay;
using rex::system::ZeroDelayConfig;
using rex::system::ZeroDelayMode;
using rex::system::ZeroDelayModeName;
using rex::system::ZeroDelayStreak;

namespace {

// One zero delay at `now`, ending `slept` microseconds later (or 1 us for a
// native yield). Returns the planned backoff.
uint32_t ZeroAt(ZeroDelayStreak& s, const ZeroDelayConfig& c, int64_t& now) {
  const uint32_t backoff = PlanZeroDelay(s, c, true, now);
  now += backoff ? backoff : 1;
  NoteZeroDelayEnd(s, now);
  now += 5;  // guest work until the next delay
  return backoff;
}

}  // namespace

TEST_CASE("Zero delay: mode names", "[system][zero_delay]") {
  CHECK(ParseZeroDelayMode("native") == ZeroDelayMode::kNative);
  CHECK(ParseZeroDelayMode("backoff") == ZeroDelayMode::kBackoff);
  CHECK_FALSE(ParseZeroDelayMode("yield").has_value());
  CHECK(std::string(ZeroDelayModeName(ZeroDelayMode::kBackoff)) == "backoff");
  CHECK(std::string(ZeroDelayModeName(ZeroDelayMode::kNative)) == "native");
}

TEST_CASE("Zero delay: config clamps", "[system][zero_delay]") {
  constexpr ZeroDelayConfig d = MakeZeroDelayConfig(32, 500, 200);
  STATIC_REQUIRE(d.after == 32);
  STATIC_REQUIRE(d.max_us == 500);
  STATIC_REQUIRE(d.gap_us == 200);
  constexpr ZeroDelayConfig low = MakeZeroDelayConfig(0, 1, 0);
  STATIC_REQUIRE(low.after == 1);
  STATIC_REQUIRE(low.max_us == kZeroDelayBackoffStartUs);
  STATIC_REQUIRE(low.gap_us == 1);
  constexpr ZeroDelayConfig high = MakeZeroDelayConfig(1 << 30, 100000, 1 << 30);
  STATIC_REQUIRE(high.after == (1u << 20));
  STATIC_REQUIRE(high.max_us == 2000);
  STATIC_REQUIRE(high.gap_us == 100000);
}

TEST_CASE("Zero delay: first `after` zero delays stay native, then 50 us doubling to max",
          "[system][zero_delay]") {
  const ZeroDelayConfig c = MakeZeroDelayConfig(32, 500, 200);
  ZeroDelayStreak s;
  int64_t now = 1000;
  for (int i = 0; i < 32; ++i) {
    CHECK(ZeroAt(s, c, now) == 0);
  }
  CHECK(s.streak == 32);
  CHECK(ZeroAt(s, c, now) == 50);
  CHECK(ZeroAt(s, c, now) == 100);
  CHECK(ZeroAt(s, c, now) == 200);
  CHECK(ZeroAt(s, c, now) == 400);
  CHECK(ZeroAt(s, c, now) == 500);
  CHECK(ZeroAt(s, c, now) == 500);
  CHECK(s.streak == 38);
}

TEST_CASE("Zero delay: the backoff sleep itself does not break the streak",
          "[system][zero_delay]") {
  // max 2000 us is far above the 200 us gap: the gap is measured from the
  // end of the previous delay, not its start.
  const ZeroDelayConfig c = MakeZeroDelayConfig(2, 2000, 200);
  ZeroDelayStreak s;
  int64_t now = 0;
  CHECK(ZeroAt(s, c, now) == 0);
  CHECK(ZeroAt(s, c, now) == 0);
  uint32_t last = 0;
  for (int i = 0; i < 8; ++i) {
    last = ZeroAt(s, c, now);
  }
  CHECK(last == 2000);
}

TEST_CASE("Zero delay: a gap of guest work resets the streak", "[system][zero_delay]") {
  const ZeroDelayConfig c = MakeZeroDelayConfig(4, 500, 200);
  ZeroDelayStreak s;
  int64_t now = 0;
  for (int i = 0; i < 6; ++i) {
    ZeroAt(s, c, now);
  }
  CHECK(s.backoff_us == 100);
  now += 200;  // exactly the gap: not "less than gap"
  CHECK(PlanZeroDelay(s, c, true, now) == 0);
  CHECK(s.streak == 1);
  CHECK(s.backoff_us == 0);
  NoteZeroDelayEnd(s, now + 1);
  now += 2;
  for (int i = 0; i < 3; ++i) {
    CHECK(ZeroAt(s, c, now) == 0);
  }
  CHECK(ZeroAt(s, c, now) == 50);  // restarts at 50 us
}

TEST_CASE("Zero delay: a non-eligible delay resets the streak", "[system][zero_delay]") {
  // Non-zero, absolute, alertable or pending-APC delays are all passed as
  // eligible_zero = false by XThread::Delay.
  const ZeroDelayConfig c = MakeZeroDelayConfig(3, 500, 200);
  ZeroDelayStreak s;
  int64_t now = 0;
  for (int i = 0; i < 5; ++i) {
    ZeroAt(s, c, now);
  }
  CHECK(s.backoff_us == 100);
  CHECK(PlanZeroDelay(s, c, false, now) == 0);
  CHECK(s.streak == 0);
  CHECK(s.backoff_us == 0);
  CHECK_FALSE(s.has_last_end);
  now += 1;
  // The next zero delay starts a new streak even though it follows closely.
  CHECK(PlanZeroDelay(s, c, true, now) == 0);
  CHECK(s.streak == 1);
}

TEST_CASE("Zero delay: clock going backwards starts a new streak", "[system][zero_delay]") {
  const ZeroDelayConfig c = MakeZeroDelayConfig(1, 500, 200);
  ZeroDelayStreak s;
  CHECK(PlanZeroDelay(s, c, true, 1000) == 0);
  NoteZeroDelayEnd(s, 1001);
  CHECK(PlanZeroDelay(s, c, true, 500) == 0);
  CHECK(s.streak == 1);
}

TEST_CASE("Zero delay: guest call site from the caller's frame", "[system][zero_delay]") {
  // Frame of the title's SleepEx (sub_82460270): stwu r1,-112(r1) after
  // stw r12,-8(r1); the saved LR is at r1 + 104 and the back chain at 0(r1).
  std::map<uint32_t, uint32_t> mem;
  const uint32_t limit = 0x70000000;
  const uint32_t base = 0x70010000;
  const uint32_t r1 = 0x7000FE00;
  mem[r1] = r1 + 112;
  mem[r1 + 104] = 0x821449D0;
  auto load = [&mem](uint32_t a) { return mem.count(a) ? mem[a] : 0u; };
  CHECK(GuestCallerSite(r1, limit, base, load) == 0x821449D0);
  // Out of the stack range or a broken back chain: unknown.
  CHECK(GuestCallerSite(0x60000000, limit, base, load) == 0);
  CHECK(GuestCallerSite(base, limit, base, load) == 0);
  CHECK(GuestCallerSite(r1, 0, 0, load) == 0);
  mem[r1] = r1;  // back chain not above r1
  CHECK(GuestCallerSite(r1, limit, base, load) == 0);
  mem[r1] = base + 16;  // back chain above the stack base
  CHECK(GuestCallerSite(r1, limit, base, load) == 0);
}
