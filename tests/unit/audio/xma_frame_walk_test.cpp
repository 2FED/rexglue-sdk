#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/audio/xma/frame_walk.h>
#include <rex/audio/xma/helpers.h>
#include <rex/cvar.h>
#include <rex/stream.h>

using rex::audio::xma::AnalyzePacketFrames;
using rex::audio::xma::CheckSplitTail;
using rex::audio::xma::ClaimSplitTail;
using rex::audio::xma::ClassifyFrameStart;
using rex::audio::xma::FindNextPacketReadOffset;
using rex::audio::xma::FormatWalkMinuteFields;
using rex::audio::xma::kWalkBitsPerPacket;
using rex::audio::xma::kWalkBytesPerPacket;
using rex::audio::xma::kWalkPacketHeaderBits;
using rex::audio::xma::NextReadOffsetInPacket;
using rex::audio::xma::PacketFrames;
using rex::audio::xma::ReadBitsMsb;
using rex::audio::xma::ReadSplitFrameSize;
using rex::audio::xma::SimulateBufferWalk;
using rex::audio::xma::SplitTailCheck;
using rex::audio::xma::WalkFrameEnd;
using rex::audio::xma::WalkMinuteTotals;
using rex::audio::xma::WalkPath;
using rex::audio::xma::WalkStep;
using rex::audio::xma::WalkVerdict;

namespace {

constexpr uint32_t kPayloadBits = kWalkBitsPerPacket - kWalkPacketHeaderBits;  // 16352

// Synthetic XMA2 buffer: frames packed back to back over the packet payloads
// (as the encoder does), packet headers derived from the layout.
struct SynthStream {
  std::vector<uint8_t> bytes;
  uint32_t packets = 0;
  std::vector<uint32_t> starts;  // absolute read offset of each frame
  uint32_t split_tails = 0;      // frames whose 15-bit header straddles a packet end
  uint32_t split_tails_not_first = 0;
};

// Payload bit g -> absolute buffer bit.
uint32_t PayloadToBuffer(uint32_t g) {
  return (g / kPayloadBits) * kWalkBitsPerPacket + kWalkPacketHeaderBits + g % kPayloadBits;
}

void PutBit(std::vector<uint8_t>& bytes, uint32_t bit, uint32_t value) {
  const uint8_t mask = uint8_t(1u << (7 - (bit & 7)));
  if (value) {
    bytes[bit >> 3] |= mask;
  } else {
    bytes[bit >> 3] &= uint8_t(~mask);
  }
}

void PutPayload(std::vector<uint8_t>& bytes, uint32_t g, uint32_t value, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    PutBit(bytes, PayloadToBuffer(g + i), (value >> (count - 1 - i)) & 1u);
  }
}

// count_split_in_header: the packet's XMA2 frame count includes a frame whose
// header starts in the last 1-14 bits (false reproduces the KI-071 trigger).
SynthStream BuildStream(const std::vector<uint32_t>& sizes, uint32_t packets,
                        bool count_split_in_header) {
  SynthStream s;
  s.packets = packets;
  s.bytes.assign(size_t(packets) * kWalkBytesPerPacket, 0);
  // Payload filler: alternating bits (never a frame boundary by itself).
  for (uint32_t g = 0; g < packets * kPayloadBits; ++g) {
    PutBit(s.bytes, PayloadToBuffer(g), g & 1u);
  }
  std::vector<uint32_t> first_start(packets, UINT32_MAX);
  std::vector<uint32_t> count(packets, 0);
  uint32_t g = 0;
  for (size_t i = 0; i < sizes.size(); ++i) {
    const uint32_t size = sizes[i];
    REQUIRE(g + size <= packets * kPayloadBits);
    const uint32_t packet = g / kPayloadBits;
    const uint32_t left = kPayloadBits - g % kPayloadBits;
    s.starts.push_back(PayloadToBuffer(g));
    if (first_start[packet] == UINT32_MAX) {
      first_start[packet] = g % kPayloadBits;
    }
    const bool split = left < 15;
    if (split) {
      ++s.split_tails;
      if (first_start[packet] != g % kPayloadBits) {
        ++s.split_tails_not_first;
      }
    }
    if (!split || count_split_in_header) {
      ++count[packet];
    }
    PutPayload(s.bytes, g, size, 15);
    // Trailer: 1 when the next frame starts in the packet holding this
    // frame's last bit.
    const uint32_t end = g + size;
    const bool more = i + 1 < sizes.size() && end / kPayloadBits == (end - 1) / kPayloadBits;
    PutBit(s.bytes, PayloadToBuffer(end - 1), more ? 1 : 0);
    g = end;
  }
  // Stream end: ones up to the packet end (reads as length 0x7FFF).
  while (g % kPayloadBits) {
    PutBit(s.bytes, PayloadToBuffer(g), 1);
    ++g;
  }
  for (uint32_t p = 0; p < packets; ++p) {
    uint8_t* h = s.bytes.data() + size_t(p) * kWalkBytesPerPacket;
    const uint32_t offset = first_start[p] == UINT32_MAX ? 0x7FFF : first_start[p];
    h[0] = uint8_t((count[p] << 2) | ((offset >> 13) & 3));
    h[1] = uint8_t(offset >> 5);
    h[2] = uint8_t(((offset & 0x1F) << 3) | 1);  // metadata 1 (XMA2)
    h[3] = 0;                                    // skip count
  }
  return s;
}

std::vector<uint32_t> Starts(const std::vector<WalkStep>& steps) {
  std::vector<uint32_t> out;
  for (const WalkStep& step : steps) {
    out.push_back(step.read_offset);
  }
  return out;
}

uint32_t CountVerdict(const std::vector<WalkStep>& steps, WalkVerdict verdict) {
  uint32_t n = 0;
  for (const WalkStep& step : steps) {
    n += step.verdict == verdict;
  }
  return n;
}

// Packet 0: frames ending 10 bits before the packet end, then a 3000-bit
// frame whose 15-bit header straddles into packet 1, then more frames.
std::vector<uint32_t> SplitTailSizes() {
  std::vector<uint32_t> sizes = {4000, 4000, 4000, 4342};  // 16342: 10 bits left
  for (int i = 0; i < 8; ++i) {
    sizes.push_back(3000);
  }
  return sizes;
}

// Verbatim upstream XmaContext::GetPacketInfo (bd9314f), for the differential
// test: AnalyzePacketFrames must give the same kPacketInfo fields.
struct UpstreamPacketInfo {
  uint8_t frame_count_ = 0;
  uint8_t current_frame_ = 0;
  uint32_t current_frame_size_ = 0;
};

UpstreamPacketInfo UpstreamGetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  constexpr uint32_t kBitsPerPacket = 2048 * 8;
  constexpr uint32_t kBitsPerFrameHeader = 15;
  UpstreamPacketInfo packet_info = {};

  const uint32_t first_frame_offset = rex::audio::xma::GetPacketFrameOffset(packet);
  rex::stream::BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == rex::audio::xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (rex::audio::xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = rex::audio::xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

uint64_t Lcg(uint64_t& state) {
  state = state * 6364136223846793005ull + 1442695040888963407ull;
  return state >> 33;
}

}  // namespace

TEST_CASE("XMA walk: split header not in the packet frame count skips a frame upstream",
          "[audio][xma][walk]") {
  const SynthStream s = BuildStream(SplitTailSizes(), 3, /*count_split_in_header=*/false);
  REQUIRE(s.split_tails == 1);

  const std::vector<WalkStep> old_walk = SimulateBufferWalk(s.bytes.data(), s.packets, false);
  // The KI-071 slip: one frame fewer, the straddling frame (index 4) is never
  // decoded and the walk resumes at packet 1's first-frame offset.
  REQUIRE(old_walk.size() == s.starts.size() - 1);
  std::vector<uint32_t> expected_old = s.starts;
  expected_old.erase(expected_old.begin() + 4);
  CHECK(Starts(old_walk) == expected_old);
  CHECK(old_walk[3].path == WalkPath::kLastInPacket);
  CHECK(old_walk[4].verdict == WalkVerdict::kGap);
  CHECK(CountVerdict(old_walk, WalkVerdict::kGap) == 1);

  const std::vector<WalkStep> fixed = SimulateBufferWalk(s.bytes.data(), s.packets, true);
  CHECK(Starts(fixed) == s.starts);
  REQUIRE(fixed.size() == s.starts.size());
  CHECK(fixed[3].path == WalkPath::kInPacket);
  CHECK(fixed[4].path == WalkPath::kSplitHeader);
  CHECK(fixed[4].frame_size == 3000);
  CHECK(fixed[4].claimed);
  CHECK(CountVerdict(fixed, WalkVerdict::kGap) == 0);
  CHECK(CountVerdict(fixed, WalkVerdict::kDup) == 0);
}

TEST_CASE("XMA walk: split header counted by the packet header decodes once either way",
          "[audio][xma][walk]") {
  const SynthStream s = BuildStream(SplitTailSizes(), 3, /*count_split_in_header=*/true);
  for (bool fix : {false, true}) {
    const std::vector<WalkStep> steps = SimulateBufferWalk(s.bytes.data(), s.packets, fix);
    CHECK(Starts(steps) == s.starts);
    CHECK(CountVerdict(steps, WalkVerdict::kGap) == 0);
    CHECK(CountVerdict(steps, WalkVerdict::kDup) == 0);
    REQUIRE(steps.size() == s.starts.size());
    CHECK(steps[4].path == WalkPath::kSplitHeader);
    CHECK_FALSE(steps[4].claimed);
  }
}

TEST_CASE("XMA walk: frame ending exactly on the packet end and a 15-bit header that fits",
          "[audio][xma][walk]") {
  // 16352 = exact end of packet 0; next frame starts at packet 1's payload.
  const SynthStream exact = BuildStream({5000, 5000, 6352, 2000, 2000, 2000}, 2, false);
  // 16337: exactly 15 bits left, the header fits and the frame spans.
  const SynthStream fits = BuildStream({5000, 5000, 6337, 2000, 2000, 2000}, 2, false);
  for (const SynthStream* s : {&exact, &fits}) {
    REQUIRE(s->split_tails == 0);
    for (bool fix : {false, true}) {
      const std::vector<WalkStep> steps = SimulateBufferWalk(s->bytes.data(), s->packets, fix);
      CHECK(Starts(steps) == s->starts);
      CHECK(CountVerdict(steps, WalkVerdict::kGap) == 0);
      CHECK(CountVerdict(steps, WalkVerdict::kDup) == 0);
    }
  }
  const std::vector<WalkStep> steps = SimulateBufferWalk(fits.bytes.data(), fits.packets, false);
  REQUIRE(steps.size() == fits.starts.size());
  CHECK(steps[3].path == WalkPath::kSpanning);
}

TEST_CASE("XMA walk: random layouts, one decode per frame with the fix", "[audio][xma][walk]") {
  uint64_t state = 0x4B49303731ull;  // "KI071"
  uint32_t triggers = 0;
  for (int seed = 0; seed < 400; ++seed) {
    std::vector<uint32_t> sizes;
    uint32_t total = 0;
    while (true) {
      const uint32_t size = 300 + uint32_t(Lcg(state) % 900);
      if (total + size > 4 * kPayloadBits - 64) {
        break;
      }
      sizes.push_back(size);
      total += size;
    }
    for (bool counted : {true, false}) {
      const SynthStream s = BuildStream(sizes, 4, counted);
      const std::vector<WalkStep> fixed = SimulateBufferWalk(s.bytes.data(), s.packets, true);
      CHECK(Starts(fixed) == s.starts);
      CHECK(CountVerdict(fixed, WalkVerdict::kGap) == 0);
      CHECK(CountVerdict(fixed, WalkVerdict::kDup) == 0);
      const std::vector<WalkStep> old_walk = SimulateBufferWalk(s.bytes.data(), s.packets, false);
      CHECK(CountVerdict(old_walk, WalkVerdict::kDup) == 0);
      if (counted) {
        CHECK(Starts(old_walk) == s.starts);
      } else {
        // Upstream loses exactly the straddling frames the walk cannot count
        // (a straddling first frame of a packet is still found through the
        // packet's first-frame offset).
        CHECK(old_walk.size() + s.split_tails_not_first == s.starts.size());
        CHECK(CountVerdict(old_walk, WalkVerdict::kGap) == s.split_tails_not_first);
        triggers += s.split_tails_not_first;
      }
    }
  }
  CHECK(triggers > 0);
}

TEST_CASE("XMA walk: a trailer-1 frame followed by padding is not claimed", "[audio][xma][walk]") {
  SynthStream s = BuildStream({4000, 4000, 4000, 4342, 2000, 2000}, 2, false);
  // Turn the straddling frame into padding: ones over the last 10 bits of
  // packet 0 and packet 1 starting with a new frame (no continuation).
  for (uint32_t bit = kWalkBitsPerPacket - 10; bit < kWalkBitsPerPacket; ++bit) {
    PutBit(s.bytes, bit, 1);
  }
  const SynthStream tail = BuildStream({2000, 2000, 2000}, 1, false);
  std::copy(tail.bytes.begin(), tail.bytes.end(), s.bytes.begin() + kWalkBytesPerPacket);
  const uint8_t* p0 = s.bytes.data();
  const uint8_t* p1 = p0 + kWalkBytesPerPacket;
  const PacketFrames info = AnalyzePacketFrames(p0, s.starts[3] % kWalkBitsPerPacket);
  CHECK(info.split_tail_bits == 10);
  CHECK_FALSE(info.split_tail_counted);
  CHECK(CheckSplitTail(p0, p1, info.split_tail_pos) == SplitTailCheck::kNotReal);

  const std::vector<WalkStep> fixed = SimulateBufferWalk(s.bytes.data(), 2, true);
  REQUIRE(fixed.size() == 7);
  CHECK(fixed[3].path == WalkPath::kLastInPacket);
  CHECK_FALSE(fixed[3].claimed);
  CHECK(fixed[4].read_offset == kWalkBitsPerPacket + kWalkPacketHeaderBits);
}

TEST_CASE("XMA walk: packet analysis matches the upstream walk", "[audio][xma][walk]") {
  const SynthStream s = BuildStream(SplitTailSizes(), 3, false);
  const uint8_t* p0 = s.bytes.data();
  // Frame 2 of packet 0.
  PacketFrames info = AnalyzePacketFrames(p0, s.starts[2]);
  CHECK(info.walked == 4);
  CHECK(info.header_count == 4);
  CHECK(info.frame_count == 4);
  CHECK(info.current_frame == 2);
  CHECK(info.current_frame_size == 4000);
  CHECK(info.split_tail_pos == kWalkBitsPerPacket - 10);
  CHECK(info.split_tail_bits == 10);
  CHECK_FALSE(info.IsLast());
  // Frame 3 is the last one by the walk; claiming the tail changes that.
  info = AnalyzePacketFrames(p0, s.starts[3]);
  CHECK(info.IsLast());
  ClaimSplitTail(&info, s.starts[3]);
  CHECK_FALSE(info.IsLast());
  CHECK(info.frame_count == 5);
  // The straddling frame itself: upstream sees no length (size 0) and, with
  // the header not counting it, frame index 0 of 4 (not last).
  info = AnalyzePacketFrames(p0, s.starts[4]);
  CHECK(info.current_frame_size == 0);
  CHECK(info.current_frame == 0);
  CHECK_FALSE(info.IsLast());
  ClaimSplitTail(&info, s.starts[4]);
  CHECK(info.current_frame == 4);
  CHECK(info.IsLast());
  CHECK(ReadSplitFrameSize(p0, p0 + kWalkBytesPerPacket, s.starts[4]) == 3000);
  CHECK(CheckSplitTail(p0, p0 + kWalkBytesPerPacket, s.starts[4]) == SplitTailCheck::kExact);
  // Offset before the first frame: the continuation-size quirk of upstream.
  const uint8_t* p1 = p0 + kWalkBytesPerPacket;
  info = AnalyzePacketFrames(p1, kWalkPacketHeaderBits);
  CHECK(info.first_frame_offset == kWalkPacketHeaderBits + (3000 - 10));
  CHECK(info.current_frame == 0);
  CHECK(info.current_frame_size == 3000 - 10);
}

TEST_CASE("XMA walk: offset helpers", "[audio][xma][walk]") {
  // In-packet next offset wraps to bit 0 of the same packet at the packet end.
  CHECK(NextReadOffsetInPacket(kWalkBitsPerPacket + 100, 50) == kWalkBitsPerPacket + 150);
  CHECK(NextReadOffsetInPacket(kWalkBitsPerPacket + 100, kWalkBitsPerPacket - 100) ==
        kWalkBitsPerPacket);
  // Packets without a frame start are passed over; none left = header bits.
  std::vector<uint8_t> buffer(3 * kWalkBytesPerPacket, 0);
  for (int p = 0; p < 2; ++p) {
    buffer[size_t(p) * kWalkBytesPerPacket + 0] = 0x03;  // offset 0x7FFF
    buffer[size_t(p) * kWalkBytesPerPacket + 1] = 0xFF;
    buffer[size_t(p) * kWalkBytesPerPacket + 2] = 0xF9;
  }
  uint8_t* p2 = buffer.data() + 2 * kWalkBytesPerPacket;
  p2[1] = 0x00;  // offset 8
  p2[2] = 0x41;
  CHECK(FindNextPacketReadOffset(buffer.data(), 0, 3) ==
        2 * kWalkBitsPerPacket + kWalkPacketHeaderBits + 8);
  CHECK(FindNextPacketReadOffset(buffer.data(), 0, 2) == kWalkPacketHeaderBits);
  CHECK(ReadBitsMsb(p2, kWalkBitsPerPacket, 16, 8) == 0x41);
}

TEST_CASE("XMA walk: continuity classification", "[audio][xma][walk]") {
  WalkFrameEnd prev;
  CHECK(ClassifyFrameStart(prev, 0x1000, 100, 32) == WalkVerdict::kFirst);
  prev.valid = true;
  prev.packet_addr = 0x1000;
  prev.end_bit = 5000;
  prev.trailer = true;
  CHECK(ClassifyFrameStart(prev, 0x1000, 5000, 32) == WalkVerdict::kContinuous);
  CHECK(ClassifyFrameStart(prev, 0x1000, 4000, 32) == WalkVerdict::kDup);
  CHECK(ClassifyFrameStart(prev, 0x1000, 6000, 32) == WalkVerdict::kGap);
  // Trailer 1 with bits left, next start in another packet: a frame skipped.
  CHECK(ClassifyFrameStart(prev, 0x1800, 64, 64) == WalkVerdict::kGap);
  // Trailer 1 followed by a 0x7FFF length marker: padding, not a skipped frame.
  prev.next_is_marker = true;
  CHECK(ClassifyFrameStart(prev, 0x1800, 64, 64) == WalkVerdict::kContinuous);
  prev.next_is_marker = false;
  // Trailer 0: the next frame is the next packet's first frame.
  prev.trailer = false;
  CHECK(ClassifyFrameStart(prev, 0x1800, 64, 64) == WalkVerdict::kContinuous);
  CHECK(ClassifyFrameStart(prev, 0x1800, 900, 64) == WalkVerdict::kGap);
  // Exact packet end.
  prev.trailer = true;
  prev.end_bit = kWalkBitsPerPacket;
  CHECK(ClassifyFrameStart(prev, 0x1800, 32, 32) == WalkVerdict::kContinuous);
  // Spanning frame ending in the next packet, next frame right after it.
  prev.packet_addr = 0x1800;
  prev.end_bit = 32 + 700;
  CHECK(ClassifyFrameStart(prev, 0x1800, 732, 732) == WalkVerdict::kContinuous);
}

TEST_CASE("XMA walk: minute fields", "[audio][xma][walk]") {
  WalkMinuteTotals w;
  w.dup = 1;
  w.gap = 2;
  w.ctxs = 3;
  w.split_tail = 4;
  w.split_lost = 2;
  w.input_files = 5;
  CHECK(FormatWalkMinuteFields(w, true) ==
        "walk_dup=1 walk_gap=2 walk_ctxs=3 walk_boundary=0 walk_split_tail=4 "
        "walk_split_counted=0 walk_split_lost=2 walk_split_real=0 walk_split_claimed=0 "
        "walk_split_padding=0 walk_dup_prevented=0 walk_resyncs=0 walk_fix=1 input_dump_files=5");
}

TEST_CASE("XMA walk: packet analysis equals the upstream GetPacketInfo", "[audio][xma][walk]") {
  uint64_t state = 0x55AA1234ull;
  uint32_t compared = 0;
  for (int seed = 0; seed < 60; ++seed) {
    std::vector<uint32_t> sizes;
    uint32_t total = 0;
    while (true) {
      const uint32_t size = 70 + uint32_t(Lcg(state) % 1400);  // < 256 frames per packet
      if (total + size > 3 * kPayloadBits - 64) {
        break;
      }
      sizes.push_back(size);
      total += size;
    }
    const SynthStream s = BuildStream(sizes, 3, (seed & 1) != 0);
    for (uint32_t p = 0; p < s.packets; ++p) {
      // BitStream::Peek reads up to 8 bytes past the offset: pad the copy.
      std::vector<uint8_t> packet(kWalkBytesPerPacket + 8, 0);
      std::copy_n(s.bytes.begin() + size_t(p) * kWalkBytesPerPacket, kWalkBytesPerPacket,
                  packet.begin());
      std::vector<uint32_t> offsets = {kWalkPacketHeaderBits, kWalkPacketHeaderBits + 1,
                                       kWalkBitsPerPacket - 1};
      for (uint32_t start : s.starts) {
        if (start / kWalkBitsPerPacket == p) {
          offsets.push_back(start % kWalkBitsPerPacket);
          offsets.push_back(start % kWalkBitsPerPacket + 3);
        }
      }
      for (uint32_t offset : offsets) {
        const UpstreamPacketInfo up = UpstreamGetPacketInfo(packet.data(), offset);
        const PacketFrames ours = AnalyzePacketFrames(packet.data(), offset);
        CHECK(uint8_t(ours.frame_count) == up.frame_count_);
        CHECK(uint8_t(ours.current_frame) == up.current_frame_);
        CHECK(ours.current_frame_size == up.current_frame_size_);
        ++compared;
      }
    }
  }
  CHECK(compared > 1000);
}

TEST_CASE("XMA walk: the frame-walk fix is on by default", "[audio][xma][walk]") {
  // KI-071 verified fixed (l76 owner run and offline trace replay): l77 makes
  // the fix the default; false restores the upstream walk.
  const rex::cvar::FlagEntry* fix = rex::cvar::GetFlagInfo("xma_frame_walk_fix");
  const rex::cvar::FlagEntry* trace = rex::cvar::GetFlagInfo("xma_frame_walk_trace");
  REQUIRE(fix);
  REQUIRE(trace);
  CHECK(fix->default_value == "true");
  CHECK(trace->default_value == "false");
  rex::cvar::ResetToDefault("xma_frame_walk_fix");
  CHECK(REXCVAR_GET(xma_frame_walk_fix));
}
