#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <rex/audio/xma/frame_trim.h>
#include <rex/audio/xma/sibling_trace.h>

using rex::audio::xma::ApplyFrameTrim;
using rex::audio::xma::BufferSlice;
using rex::audio::xma::DumpBurstTracker;
using rex::audio::xma::DumpFileName;
using rex::audio::xma::FrameTrimApplied;
using rex::audio::xma::FrameTrimCarry;
using rex::audio::xma::FrameTrimInfo;
using rex::audio::xma::GroupMember;
using rex::audio::xma::MakeWavHeader;
using rex::audio::xma::ParseFrameTrim;
using rex::audio::xma::SiblingGroup;
using rex::audio::xma::StartBurstWindow;
using rex::audio::xma::StickySiblingGrouper;
using rex::audio::xma::SwapPcm16;

namespace {

// MSB-first bit writer (FFmpeg get_bits order).
struct BitWriter {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(64, 0);
  uint32_t pos = 0;
  void Put(uint32_t value, uint32_t bits) {
    for (uint32_t i = 0; i < bits; ++i) {
      const uint32_t bit = (value >> (bits - 1 - i)) & 1u;
      bytes[pos >> 3] |= uint8_t(bit << (7 - (pos & 7)));
      ++pos;
    }
  }
};

// Mono frame: length, fixed-layout bit, one 512 subframe ('0'), drc, trims.
// frame_bits is set to cover the header plus some payload.
BitWriter MonoHeader(uint32_t lead_bits, bool flag, bool has_start, uint32_t start, bool has_end,
                     uint32_t end) {
  BitWriter w;
  w.Put(0, lead_bits);
  w.Put(300, 15);  // frame length (only bounds the parse)
  w.Put(1, 1);     // fixed channel layout
  w.Put(0, 1);     // subframe of maximum length (512)
  w.Put(0xA5, 8);  // drc gain
  w.Put(flag, 1);
  if (flag) {
    w.Put(has_start, 1);
    if (has_start) {
      w.Put(start, 10);
    }
    w.Put(has_end, 1);
    if (has_end) {
      w.Put(end, 10);
    }
  }
  return w;
}

std::vector<uint8_t> RampFrame(uint32_t channels, uint16_t base = 0) {
  // Big-endian int16 sample index (+ channel * 0x4000), interleaved.
  std::vector<uint8_t> f(512 * 2 * channels);
  for (uint32_t i = 0; i < 512; ++i) {
    for (uint32_t c = 0; c < channels; ++c) {
      const uint16_t v = uint16_t(base + i + c * 0x4000);
      f[(i * channels + c) * 2] = uint8_t(v >> 8);
      f[(i * channels + c) * 2 + 1] = uint8_t(v);
    }
  }
  return f;
}

uint16_t SampleAt(const std::vector<uint8_t>& f, size_t byte_offset) {
  return uint16_t((f[byte_offset] << 8) | f[byte_offset + 1]);
}

}  // namespace

TEST_CASE("XMA frame trim parser: mono headers", "[audio][xma][ki071]") {
  {
    const BitWriter w = MonoHeader(0, false, false, 0, false, 0);
    const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 0, 300, 1);
    CHECK(t.ok);
    CHECK_FALSE(t.flag);
    CHECK(t.start == 0);
    CHECK(t.end == 0);
    CHECK(t.header_bits == 15 + 1 + 1 + 8 + 1);
  }
  {
    // Flag set, start present with 0, no end.
    const BitWriter w = MonoHeader(0, true, true, 0, false, 0);
    const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 0, 300, 1);
    CHECK(t.ok);
    CHECK(t.flag);
    CHECK(t.has_start);
    CHECK_FALSE(t.has_end);
    CHECK(t.start == 0);
  }
  {
    // 10-bit maximum start skip, at a non-byte-aligned frame start.
    const BitWriter w = MonoHeader(5, true, true, 1023, false, 0);
    const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 5, 300, 1);
    CHECK(t.ok);
    CHECK(t.start == 1023);
    CHECK(t.end == 0);
  }
  {
    // Both flags.
    const BitWriter w = MonoHeader(3, true, true, 512, true, 300);
    const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 3, 300, 1);
    CHECK(t.ok);
    CHECK(t.has_start);
    CHECK(t.has_end);
    CHECK(t.start == 512);
    CHECK(t.end == 300);
    CHECK(t.header_bits == 15 + 1 + 1 + 8 + 1 + 11 + 11);
  }
  {
    // End only, 10-bit max.
    const BitWriter w = MonoHeader(0, true, false, 0, true, 1023);
    const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 0, 300, 1);
    CHECK(t.ok);
    CHECK_FALSE(t.has_start);
    CHECK(t.end == 1023);
  }
}

TEST_CASE("XMA frame trim parser: stereo tile header and postproc", "[audio][xma][ki071]") {
  // Variable layout traced through wmaprodec.c decode_tilehdr:
  // ch0 gets 256 + 256, ch1 gets 512.
  BitWriter w;
  w.Put(400, 15);
  w.Put(0, 1);  // not fixed
  w.Put(1, 1);  // ch0 contains subframe 1
  w.Put(0, 1);  // ch1 does not
  w.Put(1, 1);  // not max length
  w.Put(0, 1);  // shift 1 -> 256
  // ch1 alone (channels_for_cur 1): no contains bit; length at offset 0:
  w.Put(0, 1);  // 512
  // ch0 alone at offset 256: length bit 1, shift bit 0 -> 256
  w.Put(1, 1);
  w.Put(0, 1);
  // Postproc transform present with matrix: 1, 1, 4 * 2 * 2 bits.
  w.Put(1, 1);
  w.Put(1, 1);
  w.Put(0xBEEF, 16);
  w.Put(0x11, 8);  // drc
  w.Put(1, 1);     // trim present
  w.Put(1, 1);
  w.Put(128, 10);
  w.Put(1, 1);
  w.Put(640, 10);
  const FrameTrimInfo t = ParseFrameTrim(w.bytes.data(), w.bytes.size(), 0, 400, 2);
  CHECK(t.ok);
  CHECK(t.start == 128);
  CHECK(t.end == 640);
  CHECK(t.header_bits == w.pos);

  // Fixed stereo layout without postproc.
  BitWriter f;
  f.Put(400, 15);
  f.Put(1, 1);  // fixed
  f.Put(0, 1);  // 512
  f.Put(0, 1);  // no postproc
  f.Put(0, 8);
  f.Put(1, 1);
  f.Put(0, 1);
  f.Put(1, 1);
  f.Put(7, 10);
  const FrameTrimInfo u = ParseFrameTrim(f.bytes.data(), f.bytes.size(), 0, 400, 2);
  CHECK(u.ok);
  CHECK(u.start == 0);
  CHECK(u.end == 7);
}

TEST_CASE("XMA frame trim parser: broken and truncated headers", "[audio][xma][ki071]") {
  // Mono 128 then a 512 subframe: channel length > 512 (decode_tilehdr error).
  BitWriter w;
  w.Put(300, 15);
  w.Put(1, 1);
  w.Put(1, 1);
  w.Put(1, 1);  // shift 2 -> 128
  w.Put(0, 1);  // 512 at offset 128
  CHECK_FALSE(ParseFrameTrim(w.bytes.data(), w.bytes.size(), 0, 300, 1).ok);

  // Truncated: frame ends inside the trim fields.
  const BitWriter m = MonoHeader(0, true, true, 1023, true, 1023);
  CHECK(ParseFrameTrim(m.bytes.data(), m.bytes.size(), 0, m.pos, 1).ok);
  CHECK_FALSE(ParseFrameTrim(m.bytes.data(), m.bytes.size(), 0, m.pos - 1, 1).ok);
  CHECK_FALSE(ParseFrameTrim(m.bytes.data(), m.bytes.size(), 0, 300, 3).ok);
}

TEST_CASE("XMA frame trim: sample-exact application carries partial blocks",
          "[audio][xma][ki071]") {
  FrameTrimCarry carry;
  {
    // No trim, no carry: identity.
    std::vector<uint8_t> f = RampFrame(1);
    const std::vector<uint8_t> orig = f;
    const FrameTrimApplied r = ApplyFrameTrim(f, 1, 0, 0, false, carry);
    CHECK(r.blocks == 4);
    CHECK(f == orig);
    CHECK(carry.size == 0);
  }
  // Start trim 100: 412 samples = 824 bytes -> 3 blocks + 56 bytes carried.
  std::vector<uint8_t> f = RampFrame(1);
  FrameTrimApplied r = ApplyFrameTrim(f, 1, 100, 0, false, carry);
  CHECK(r.blocks == 3);
  CHECK(r.dropped_samples == 100);
  CHECK(carry.size == 56);
  CHECK(SampleAt(f, 1024 - 3 * 256) == 100);
  CHECK(SampleAt(f, 1022) == 100 + 383);
  // Next frame (no trim): the 28 carried samples come first.
  std::vector<uint8_t> g = RampFrame(1, 1000);
  r = ApplyFrameTrim(g, 1, 0, 0, false, carry);
  CHECK(r.blocks == 4);
  CHECK(SampleAt(g, 0) == 484);
  CHECK(SampleAt(g, 2 * 27) == 511);
  CHECK(SampleAt(g, 2 * 28) == 1000);
  CHECK(carry.size == 56);

  // End trim beyond what is left after the start trim.
  carry.Reset();
  std::vector<uint8_t> h = RampFrame(1);
  r = ApplyFrameTrim(h, 1, 400, 300, false, carry);
  CHECK(r.dropped_samples == 512);
  CHECK(r.unapplied_end == 188);
  CHECK(r.blocks == 0);
  CHECK(carry.size == 0);

  // A start trim larger than one frame continues into the next frame.
  carry.Reset();
  std::vector<uint8_t> a = RampFrame(1);
  r = ApplyFrameTrim(a, 1, 700, 0, false, carry);
  CHECK(r.blocks == 0);
  CHECK(carry.pending_start == 188);
  std::vector<uint8_t> b = RampFrame(1, 2000);
  r = ApplyFrameTrim(b, 1, 0, 0, false, carry);
  CHECK(r.dropped_samples == 188);
  CHECK(carry.pending_start == 0);
  // 324 samples = 648 bytes -> 2 blocks + 136 bytes.
  CHECK(r.blocks == 2);
  CHECK(SampleAt(b, 1024 - 512) == 2188);
  CHECK(carry.size == 136);

  // Stereo end trim 32 pairs: 480 pairs = 1920 bytes -> 7 blocks + 128.
  carry.Reset();
  std::vector<uint8_t> s = RampFrame(2);
  r = ApplyFrameTrim(s, 2, 0, 32, false, carry);
  CHECK(r.blocks == 7);
  CHECK(carry.size == 128);
  CHECK(SampleAt(s, 2048 - 7 * 256) == 0);
  CHECK(SampleAt(s, 2048 - 7 * 256 + 2) == 0x4000);
}

TEST_CASE("XMA frame trim: block-granular application", "[audio][xma][ki071]") {
  FrameTrimCarry carry;
  // Stereo: 64 pairs per block. Start 100 -> 1 block dropped, 36 unaligned.
  std::vector<uint8_t> s = RampFrame(2);
  FrameTrimApplied r = ApplyFrameTrim(s, 2, 100, 0, true, carry);
  CHECK(r.blocks == 7);
  CHECK(r.dropped_samples == 64);
  CHECK(r.unaligned_samples == 36);
  CHECK(SampleAt(s, 2048 - 7 * 256) == 64);
  CHECK(carry.size == 0);
  // Mono: start 256 + end 128 -> 3 blocks dropped exactly.
  std::vector<uint8_t> m = RampFrame(1);
  r = ApplyFrameTrim(m, 1, 256, 128, true, carry);
  CHECK(r.blocks == 1);
  CHECK(r.dropped_samples == 384);
  CHECK(r.unaligned_samples == 0);
  CHECK(SampleAt(m, 1024 - 256) == 256);
  CHECK(SampleAt(m, 1022) == 383);
}

TEST_CASE("XMA PCM dump: WAV header and sample order", "[audio][xma][ki071]") {
  const auto h = MakeWavHeader(44100, 1, 88200);
  CHECK(std::memcmp(h.data(), "RIFF", 4) == 0);
  CHECK(std::memcmp(h.data() + 8, "WAVEfmt ", 8) == 0);
  CHECK(std::memcmp(h.data() + 36, "data", 4) == 0);
  auto u32 = [&h](size_t at) {
    return uint32_t(h[at]) | (uint32_t(h[at + 1]) << 8) | (uint32_t(h[at + 2]) << 16) |
           (uint32_t(h[at + 3]) << 24);
  };
  auto u16 = [&h](size_t at) { return uint16_t(h[at] | (h[at + 1] << 8)); };
  CHECK(u32(4) == 36 + 88200);
  CHECK(u32(16) == 16);
  CHECK(u16(20) == 1);
  CHECK(u16(22) == 1);
  CHECK(u32(24) == 44100);
  CHECK(u32(28) == 88200);
  CHECK(u16(32) == 2);
  CHECK(u16(34) == 16);
  CHECK(u32(40) == 88200);
  const auto st = MakeWavHeader(48000, 2, 0);
  CHECK((uint32_t(st[28]) | (uint32_t(st[29]) << 8) | (uint32_t(st[30]) << 16)) == 192000);
  CHECK(st[32] == 4);

  const uint8_t be[4] = {0x12, 0x34, 0xFF, 0x80};
  uint8_t le[4] = {};
  SwapPcm16(be, le, 4);
  CHECK(le[0] == 0x34);
  CHECK(le[1] == 0x12);
  CHECK(le[2] == 0x80);
  CHECK(le[3] == 0xFF);
  CHECK(DumpFileName(82, 4, 44100, false, 3) == "ctx82-g4-44100Hz-mono-3.wav");
  CHECK(DumpFileName(1, 0, 48000, true, 0) == "ctx1-g0-48000Hz-stereo-0.wav");
}

TEST_CASE("XMA first-decode burst window", "[audio][xma][ki071]") {
  StartBurstWindow w;
  std::vector<std::string> flush;
  CHECK_FALSE(w.IsBurst(0, 82, &flush));
  w.Record(0, 82, true, "a");
  CHECK_FALSE(w.IsBurst(1000, 83, &flush));
  w.Record(1000, 83, false, "b");
  CHECK_FALSE(w.IsBurst(2000, 84, &flush));
  w.Record(2000, 84, false, "c");
  CHECK(flush.empty());
  // Fourth start within 50 ms: burst; the two suppressed records are flushed.
  CHECK(w.IsBurst(3000, 85, &flush));
  w.Record(3000, 85, true, "d");
  REQUIRE(flush.size() == 2);
  CHECK(flush[0] == "b");
  CHECK(flush[1] == "c");
  flush.clear();
  CHECK(w.IsBurst(4000, 86, &flush));
  CHECK(flush.empty());
  // The same context again does not count as another.
  StartBurstWindow v;
  v.Record(0, 5, false, "x");
  v.Record(10, 5, false, "y");
  v.Record(20, 6, false, "z");
  CHECK_FALSE(v.IsBurst(30, 7, &flush));
  // Outside the 50 ms window.
  StartBurstWindow o;
  o.Record(0, 1, false, "p");
  o.Record(0, 2, false, "q");
  o.Record(0, 3, false, "r");
  CHECK_FALSE(o.IsBurst(StartBurstWindow::kWindowUs + 1, 4, &flush));
  CHECK(flush.empty());
}

TEST_CASE("XMA sticky sibling groups: start burst plus contiguous buffers", "[audio][xma][ki071]") {
  StickySiblingGrouper g;
  // l74 trace arm: pair 82,83 and five 84-88 from two reads, one kick burst;
  // radio pair 12/14 started earlier; a contiguous neighbour 90 from another
  // start must not join.
  std::vector<GroupMember> m;
  for (uint32_t c : {82u, 83u, 84u, 85u, 86u, 87u, 88u}) {
    m.push_back({c, 8513, 44100, false});
  }
  m.push_back({12, 100, 44100, false});
  m.push_back({14, 100, 44100, false});
  m.push_back({90, 9000, 44100, false});
  std::vector<BufferSlice> slices = {
      {82, 0x04000000, 3},  {83, 0x04001800, 3},  // pair
      {84, 0x05000000, 2},  {85, 0x05001000, 2}, {86, 0x05002000, 2},
      {87, 0x05003000, 2},  {88, 0x05004000, 2},  // five
      {90, 0x05005000, 2},                        // contiguous, other start
      {12, 0x03F61800, 31}, {14, 0x03F71000, 31}  // radio
  };
  std::vector<SiblingGroup> groups = g.Update(m, slices);
  REQUIRE(groups.size() == 2);
  const SiblingGroup* voice = nullptr;
  const SiblingGroup* radio = nullptr;
  for (const SiblingGroup& grp : groups) {
    (grp.ctxs.size() == 7 ? voice : radio) = &grp;
  }
  REQUIRE(voice);
  REQUIRE(radio);
  CHECK(voice->ctxs == std::vector<uint32_t>{82, 83, 84, 85, 86, 87, 88});
  CHECK(radio->ctxs == std::vector<uint32_t>{12, 14});
  const uint32_t voice_id = voice->id;
  const uint32_t radio_id = radio->id;

  // Buffers swapped: no longer contiguous, same starts -> same groups/ids.
  std::vector<BufferSlice> swapped = {
      {82, 0x06000000, 3}, {83, 0x06100000, 3}, {84, 0x06200000, 2}, {12, 0x07000000, 31}};
  groups = g.Update(m, swapped);
  REQUIRE(groups.size() == 2);
  for (const SiblingGroup& grp : groups) {
    if (grp.ctxs.size() == 7) {
      CHECK(grp.id == voice_id);
    } else {
      CHECK(grp.id == radio_id);
      CHECK(grp.ctxs == std::vector<uint32_t>{12, 14});
    }
  }

  // ctx 88 restarted (new start kick): it leaves the group.
  m[6].start_kick = 9100;
  groups = g.Update(m, swapped);
  for (const SiblingGroup& grp : groups) {
    if (grp.id == voice_id) {
      CHECK(grp.ctxs == std::vector<uint32_t>{82, 83, 84, 85, 86, 87});
    }
  }
  // Radio gone: its group disappears; voice id unchanged.
  m.erase(m.begin() + 7, m.begin() + 9);
  groups = g.Update(m, swapped);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0].id == voice_id);
}

TEST_CASE("XMA burst-only dump decision", "[audio][xma][ki071]") {
  DumpBurstTracker t;
  std::vector<uint32_t> joined;
  // Menu SFX: isolated starts are never dumped.
  CHECK(t.OnStart(0, 5, 44100, 11, &joined) == 0);
  CHECK(t.OnStart(200000, 6, 44100, 12, &joined) == 0);
  CHECK(joined.empty());
  // Cut-scene: 7 mono 44.1 kHz starts within a few ms plus a 48 kHz pair.
  const int64_t t0 = 1000000;
  CHECK(t.OnStart(t0, 82, 44100, 8513, &joined) == 0);
  CHECK(t.OnStart(t0 + 100, 83, 44100, 8513, &joined) == 0);
  CHECK(t.OnStart(t0 + 150, 1, 48000, 8513, &joined) == 0);
  CHECK(t.OnStart(t0 + 200, 84, 44100, 8513, &joined) == 0);
  CHECK(joined.empty());
  // 4th 44.1 kHz start: burst; the held-back three join with it.
  const uint32_t serial = t.OnStart(t0 + 300, 85, 44100, 8513, &joined);
  CHECK(serial == 1);
  CHECK(joined == std::vector<uint32_t>{82, 83, 84, 85});
  joined.clear();
  for (uint32_t ctx : {86u, 87u, 88u}) {
    CHECK(t.OnStart(t0 + 400, ctx, 44100, 8513, &joined) == serial);
  }
  CHECK(joined == std::vector<uint32_t>{86, 87, 88});
  joined.clear();
  // A sibling of the same guest kick burst started after the window joins.
  CHECK(t.OnStart(t0 + 500000, 89, 44100, 8513, &joined) == serial);
  CHECK(joined == std::vector<uint32_t>{89});
  joined.clear();
  // Start kick 0 (trace off) never joins by kick id.
  CHECK(t.OnStart(t0 + 600000, 90, 44100, 0, &joined) == 0);
  // A later burst gets a new serial.
  const int64_t t1 = 5000000;
  for (uint32_t ctx : {20u, 21u, 22u}) {
    CHECK(t.OnStart(t1, ctx, 44100, 9000, &joined) == 0);
  }
  CHECK(t.OnStart(t1 + 10, 23, 44100, 9000, &joined) == 2);
  CHECK(joined.size() == 4);
  // The same context restarting does not count as another start.
  DumpBurstTracker u;
  CHECK(u.OnStart(0, 7, 44100, 0, &joined) == 0);
  CHECK(u.OnStart(10, 7, 44100, 0, &joined) == 0);
  CHECK(u.OnStart(20, 8, 44100, 0, &joined) == 0);
  CHECK(u.OnStart(30, 9, 44100, 0, &joined) == 0);
}
