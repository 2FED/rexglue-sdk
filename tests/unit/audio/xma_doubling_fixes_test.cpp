#include <catch2/catch_test_macros.hpp>

#include <rex/audio/xma/doubling_fixes.h>

using rex::audio::xma::ComputeSwapCarry;
using rex::audio::xma::IsStaleRingReplay;
using rex::audio::xma::MergeOutputReadOffset;
using rex::audio::xma::ShouldClearOutputValid;
using rex::audio::xma::ShouldRunWorkAfterLock;
using rex::audio::xma::SwapCarry;

TEST_CASE("XMA swap carry: stream i of a shared buffer continues at packet i",
          "[audio][xma][ki071]") {
  // 3 streams interleaved in a 30-packet buffer: stream 0 ends at packet 27
  // (skip 2 -> next 30), stream 1 at 28 (-> 31), stream 2 at 29 (-> 32).
  for (uint32_t stream = 0; stream < 3; ++stream) {
    const SwapCarry carry = ComputeSwapCarry(30 + stream, 30, 30);
    CHECK(carry.carry == stream);
    CHECK(carry.start_packet == stream);
    CHECK_FALSE(carry.fallback);
  }
}

TEST_CASE("XMA swap carry: no carry when the next index is inside the old buffer",
          "[audio][xma][ki071]") {
  // A swap forced because no later packet starts a frame.
  const SwapCarry inside = ComputeSwapCarry(12, 30, 30);
  CHECK(inside.carry == 0);
  CHECK(inside.start_packet == 0);
  CHECK_FALSE(inside.fallback);
  const SwapCarry exact = ComputeSwapCarry(30, 30, 8);
  CHECK(exact.carry == 0);
  CHECK(exact.start_packet == 0);
  CHECK_FALSE(exact.fallback);
}

TEST_CASE("XMA swap carry: out-of-range carry falls back to packet 0", "[audio][xma][ki071]") {
  const SwapCarry last_fit = ComputeSwapCarry(37, 30, 8);
  CHECK(last_fit.carry == 7);
  CHECK(last_fit.start_packet == 7);
  CHECK_FALSE(last_fit.fallback);

  const SwapCarry too_far = ComputeSwapCarry(38, 30, 8);
  CHECK(too_far.carry == 8);
  CHECK(too_far.start_packet == 0);
  CHECK(too_far.fallback);

  const SwapCarry empty_new = ComputeSwapCarry(31, 30, 0);
  CHECK(empty_new.carry == 1);
  CHECK(empty_new.start_packet == 0);
  CHECK(empty_new.fallback);

  // Large skip counts (up to 254 + 1) clamp the same way.
  const SwapCarry huge = ComputeSwapCarry(29 + 255, 30, 64);
  CHECK(huge.carry == 254);
  CHECK(huge.start_packet == 0);
  CHECK(huge.fallback);
}

TEST_CASE("XMA output valid: cleared only on a ring this Work closed", "[audio][xma][ki071]") {
  // Upstream rule (fix off): any read == write clears valid.
  CHECK(ShouldClearOutputValid(true, 0, false));
  CHECK(ShouldClearOutputValid(true, 5, false));
  CHECK_FALSE(ShouldClearOutputValid(false, 5, false));
  // Fix on: a drained ring with nothing written keeps valid (empty, not full).
  CHECK_FALSE(ShouldClearOutputValid(true, 0, true));
  CHECK(ShouldClearOutputValid(true, 1, true));
  CHECK_FALSE(ShouldClearOutputValid(false, 0, true));
  CHECK_FALSE(ShouldClearOutputValid(false, 3, true));
}

TEST_CASE("XMA output valid: stale ring replay event", "[audio][xma][ki071]") {
  CHECK(IsStaleRingReplay(true, true, 0));
  CHECK_FALSE(IsStaleRingReplay(true, true, 2));
  CHECK_FALSE(IsStaleRingReplay(true, false, 0));
  CHECK_FALSE(IsStaleRingReplay(false, true, 0));
}

TEST_CASE("XMA Work lock re-check decision", "[audio][xma][ki071]") {
  CHECK(ShouldRunWorkAfterLock(true, true));
  CHECK(ShouldRunWorkAfterLock(true, false));
  // Disabled between the unlocked check and the lock.
  CHECK_FALSE(ShouldRunWorkAfterLock(false, true));
  CHECK(ShouldRunWorkAfterLock(false, false));  // upstream: runs anyway
}

TEST_CASE("XMA merge keeps the guest-owned output read offset", "[audio][xma][ki071]") {
  CHECK(MergeOutputReadOffset(9, 4, true) == 9);
  CHECK(MergeOutputReadOffset(9, 4, false) == 4);  // upstream: host snapshot
  CHECK(MergeOutputReadOffset(4, 4, true) == 4);
}
