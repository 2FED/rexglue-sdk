// Unit tests for the Xenia Canary XMA fixes (xma_canary_fixes, l79):
// 6bef1fbed (drain before Work ends), 7015473a7 (skip chain past frameless
// packets), 5dd1cdbbf (loop_start resolved to a frame boundary), ade7e610b
// (no-progress pass ends the Work loop). Synthetic packets only.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <rex/audio/xma/canary_fixes.h>
#include <rex/audio/xma/frame_walk.h>

using rex::audio::xma::AnalyzePacketFrames;
using rex::audio::xma::CanaryMinuteTotals;
using rex::audio::xma::FindNextPacketReadOffset;
using rex::audio::xma::FormatCanaryMinuteFields;
using rex::audio::xma::IsNoProgressPass;
using rex::audio::xma::kWalkBitsPerPacket;
using rex::audio::xma::kWalkBytesPerPacket;
using rex::audio::xma::kWalkPacketHeaderBits;
using rex::audio::xma::PacketFrames;
using rex::audio::xma::ResolveLoopStartOffset;
using rex::audio::xma::ShouldEndWorkLoop;

namespace {

void PutBits(uint8_t* data, uint32_t bit, uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t b = bit + i;
    const uint8_t mask = uint8_t(1u << (7 - (b & 7)));
    if ((value >> (count - 1 - i)) & 1u) {
      data[b >> 3] |= mask;
    } else {
      data[b >> 3] &= uint8_t(~mask);
    }
  }
}

// XMA2 packet header; first_frame = absolute packet bit of the first frame
// start (>= 32), or 0 for a packet in which no frame starts.
void SetHeader(uint8_t* packet, uint32_t count, uint32_t first_frame, uint8_t skip) {
  const uint32_t offset = first_frame ? first_frame - kWalkPacketHeaderBits : 0x7FFF;
  packet[0] = uint8_t((count << 2) | ((offset >> 13) & 3));
  packet[1] = uint8_t(offset >> 5);
  packet[2] = uint8_t(((offset & 0x1F) << 3) | 1);
  packet[3] = skip;
}

// A frame at absolute packet bit `pos`: 15-bit length, trailer = last bit.
void PutFrame(uint8_t* packet, uint32_t pos, uint32_t size, bool trailer) {
  PutBits(packet, pos, size, 15);
  PutBits(packet, pos + size - 1, trailer ? 1 : 0, 1);
}

}  // namespace

TEST_CASE("XMA canary: Work loop ends only after the frame is delivered (6bef1fbed)",
          "[audio][xma][canary]") {
  // Input still valid, no error: never ends here.
  CHECK_FALSE(ShouldEndWorkLoop(true, 0, 0, false));
  CHECK_FALSE(ShouldEndWorkLoop(true, 0, 3, true));
  // Input exhausted: upstream ends at once, the fix waits for the frame.
  CHECK(ShouldEndWorkLoop(false, 0, 3, false));
  CHECK_FALSE(ShouldEndWorkLoop(false, 0, 3, true));
  CHECK(ShouldEndWorkLoop(false, 0, 0, true));
  // error_status 4 behaves the same.
  CHECK(ShouldEndWorkLoop(true, 4, 5, false));
  CHECK_FALSE(ShouldEndWorkLoop(true, 4, 5, true));
  CHECK(ShouldEndWorkLoop(true, 4, 0, true));
}

TEST_CASE("XMA canary: no-progress pass (ade7e610b)", "[audio][xma][canary]") {
  // Nothing pending, offset unchanged, no frame produced: stalled.
  CHECK(IsNoProgressPass(1000, 0, 1000, 0));
  // Offset moved: progress.
  CHECK_FALSE(IsNoProgressPass(1000, 0, 5000, 0));
  // A frame was decoded (subframes pending after the pass): progress.
  CHECK_FALSE(IsNoProgressPass(1000, 0, 1000, 4));
  // Entered with subframes pending (Consume-only pass): never a stall, even
  // when the pass drained the frame completely.
  CHECK_FALSE(IsNoProgressPass(1000, 2, 1000, 0));
  CHECK_FALSE(IsNoProgressPass(1000, 4, 1000, 2));
}

TEST_CASE("XMA canary: loop_start resolved to a frame boundary (5dd1cdbbf)",
          "[audio][xma][canary]") {
  std::vector<uint8_t> packet(kWalkBytesPerPacket, 0);
  SetHeader(packet.data(), 2, 32, 0);
  PutFrame(packet.data(), 32, 4000, true);
  PutFrame(packet.data(), 4032, 4000, false);

  // One bit short of the second frame: no frame size (read as a split header)
  // but the next frame start is reported.
  PacketFrames info = AnalyzePacketFrames(packet.data(), 4031);
  CHECK(info.current_frame_size == 0);
  CHECK(info.next_frame_start == 4032);
  CHECK(ResolveLoopStartOffset(true, 4031, info.next_frame_start) == 4032);
  // Only on a loop restart.
  CHECK(ResolveLoopStartOffset(false, 4031, info.next_frame_start) == 4031);
  // Re-analysis at the resolved offset finds the real frame.
  info = AnalyzePacketFrames(packet.data(), 4032);
  CHECK(info.next_frame_start == 4032);
  CHECK(info.current_frame == 1);
  CHECK(info.current_frame_size == 4000);
  CHECK(ResolveLoopStartOffset(true, 4032, info.next_frame_start) == 4032);
  // Exactly on the first frame.
  info = AnalyzePacketFrames(packet.data(), 32);
  CHECK(info.next_frame_start == 32);
  // A bit short of the first frame (before the first-frame offset): upstream's
  // split-frame continuation, left alone.
  info = AnalyzePacketFrames(packet.data(), 31);
  CHECK(info.next_frame_start == 31);
  // Past the last frame: no later start, offset unchanged.
  info = AnalyzePacketFrames(packet.data(), 9000);
  CHECK(info.next_frame_start == 9000);
}

TEST_CASE("XMA canary: loop_start one bit short of a split-tail frame", "[audio][xma][canary]") {
  std::vector<uint8_t> packet(kWalkBytesPerPacket, 0);
  // One frame ending 10 bits before the packet end, trailer 1: the next
  // frame's header straddles the boundary.
  const uint32_t tail = kWalkBitsPerPacket - 10;
  SetHeader(packet.data(), 1, 32, 0);
  PutFrame(packet.data(), 32, tail - 32, true);
  const PacketFrames info = AnalyzePacketFrames(packet.data(), tail - 1);
  CHECK(info.split_tail_pos == tail);
  CHECK(info.next_frame_start == tail);
}

TEST_CASE("XMA canary: next packet search follows the skip chain (7015473a7)",
          "[audio][xma][canary]") {
  // Two interleaved sub-streams A (packets 0, 2, 4) and B (1, 3); each packet
  // skips the other stream's packet (skip count 1).
  std::vector<uint8_t> buffer(5 * kWalkBytesPerPacket, 0);
  auto packet = [&](uint32_t i) { return buffer.data() + size_t(i) * kWalkBytesPerPacket; };
  SetHeader(packet(0), 1, 100, 1);
  SetHeader(packet(1), 1, 200, 1);
  SetHeader(packet(2), 0, 0, 1);  // A: frameless (continuation of a long frame)
  SetHeader(packet(3), 1, 300, 1);
  SetHeader(packet(4), 1, 400, 1);

  // Upstream: sequential scan from packet 2 lands in stream B's packet 3.
  CHECK(FindNextPacketReadOffset(buffer.data(), 2, 5) == 3 * kWalkBitsPerPacket + 300);
  // Fix: packet 2's own skip count leads to packet 4 (stream A).
  CHECK(FindNextPacketReadOffset(buffer.data(), 2, 5, true) == 4 * kWalkBitsPerPacket + 400);
  // A packet with a frame start is taken at once either way.
  CHECK(FindNextPacketReadOffset(buffer.data(), 1, 5, true) == 1 * kWalkBitsPerPacket + 200);
  // The chain leaving the buffer: none (swap).
  CHECK(FindNextPacketReadOffset(buffer.data(), 2, 4, true) == kWalkPacketHeaderBits);
  // A frameless packet with skip 0xFF ends the chain.
  SetHeader(packet(2), 0, 0, 0xFF);
  CHECK(FindNextPacketReadOffset(buffer.data(), 2, 5, true) == kWalkPacketHeaderBits);
  CHECK(FindNextPacketReadOffset(buffer.data(), 2, 5) == 3 * kWalkBitsPerPacket + 300);
}

TEST_CASE("XMA canary: minute fields", "[audio][xma][canary]") {
  CanaryMinuteTotals c;
  c.drain_passes = 3;
  c.skip_follows = 1;
  c.no_progress = 2;
  CHECK(FormatCanaryMinuteFields(c, true) ==
        "canary_drain=3 canary_skip_follow=1 canary_loop_resolved=0 canary_no_progress=2 "
        "canary_fix=1");
}
