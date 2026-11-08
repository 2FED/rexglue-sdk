/**
 * @file        rex/audio/xma/frame_walk.h
 * @brief       KI-071 step 4: the XMA2 frame walk as pure functions.
 *
 * XmaContext::Decode walks a context's input buffer one XMA frame per call:
 * every 2048-byte packet starts with a 32-bit header (frame count 6 bits,
 * first-frame bit offset 15 bits, metadata 3 bits, skip count 8 bits), and the
 * frames are packed back to back across packets. Each frame starts with a
 * 15-bit length (in bits, the length field included) and ends with one trailer
 * bit, 1 = another frame starts right after this one in the same packet.
 *
 * l75 showed that at fixed stream positions a context walks one frame fewer
 * (or more) than its sibling, so its content is offset by 512 samples from
 * then on. The packet analysis below is the upstream GetPacketInfo, unchanged,
 * plus the split-tail record: when a frame with trailer 1 ends with 1-14 bits
 * left in the packet, the next frame's 15-bit header straddles the packet
 * boundary. The walk cannot read that length, so upstream counts that frame
 * only through the XMA2 header frame count; if the header count does not
 * include it, the frame before it is taken as the packet's last frame, the
 * next read offset jumps to the next packet's first-frame offset, which lies
 * after the straddling frame, and that frame is never decoded (one frame
 * lost, no counter). ClaimSplitTail (xma_frame_walk_fix) counts the frame from
 * the trailer bit instead.
 *
 * The functions here are used by XmaContext::Decode and by the unit tests
 * (SimulateBufferWalk drives them in Decode's order over one buffer).
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>

// Log the first frame-continuity anomalies per context (counters always run).
REXCVAR_DECLARE(bool, xma_frame_walk_trace);
// Count a frame whose header straddles the packet end from the trailer bit
// (ClaimSplitTail) and never re-decode bits before the previous frame's end.
// Default true (KI-071 verified fixed); false restores the upstream walk.
REXCVAR_DECLARE(bool, xma_frame_walk_fix);
// With xma_dump_contexts: also write each dumped context's input buffers and
// a per-frame walk record.
REXCVAR_DECLARE(bool, xma_dump_input);

namespace rex::audio::xma {

inline constexpr uint32_t kWalkBytesPerPacket = 2048;
inline constexpr uint32_t kWalkBitsPerPacket = kWalkBytesPerPacket * 8;
inline constexpr uint32_t kWalkPacketHeaderBits = 32;
inline constexpr uint32_t kWalkFrameHeaderBits = 15;
inline constexpr uint32_t kWalkMaxFrameLength = 0x7FFF;
// XmaContext::kMaxFrameSizeinBits: a first-frame offset above this means no
// frame starts in the packet.
inline constexpr uint32_t kWalkMaxFirstFrameOffset = 0x4000 - kWalkPacketHeaderBits;
// First anomalies logged per context (xma_frame_walk_trace).
inline constexpr uint32_t kWalkLogPerContext = 8;

// MSB-first read of `count` (<= 32) bits at bit `offset`; bits at or beyond
// size_bits read as 0.
inline uint32_t ReadBitsMsb(const uint8_t* data, size_t size_bits, size_t offset, uint32_t count) {
  uint32_t value = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t bit = offset + i;
    uint32_t b = 0;
    if (bit < size_bits) {
      b = (data[bit >> 3] >> (7 - (bit & 7))) & 1u;
    }
    value = (value << 1) | b;
  }
  return value;
}

inline uint32_t WalkHeaderFrameCount(const uint8_t* packet) {
  return packet[0] >> 2;
}
// Absolute bit offset in the packet (header included), as GetPacketFrameOffset.
inline uint32_t WalkFirstFrameOffset(const uint8_t* packet) {
  return uint32_t(((packet[0] & 0x3) << 13) | (packet[1] << 5) | (packet[2] >> 3)) +
         kWalkPacketHeaderBits;
}
inline bool WalkIsXma2(const uint8_t* packet) {
  return (packet[2] & 0x7) == 1;
}
inline uint32_t WalkSkipCount(const uint8_t* packet) {
  return packet[3];
}

// Upstream kPacketInfo plus the walk details the trace and fix need.
struct PacketFrames {
  uint32_t frame_count = 0;         // kPacketInfo::frame_count_ (after the header fix-up)
  uint32_t current_frame = 0;       // kPacketInfo::current_frame_
  uint32_t current_frame_size = 0;  // kPacketInfo::current_frame_size_ (0 = header split)
  uint32_t walked = 0;              // frames found by the 15-bit length walk
  uint32_t header_count = 0;        // XMA2 header frame count (0 when not XMA2)
  bool xma2 = false;
  uint32_t first_frame_offset = 0;
  // Split tail: a frame whose 15-bit header starts in the last 1-14 bits of the
  // packet, after a frame with trailer bit 1 (or at the first-frame offset).
  uint32_t split_tail_pos = 0;  // packet-relative bit offset; 0 = none
  uint32_t split_tail_bits = 0;
  // Upstream already counts the split-tail frame (header count above the
  // walked count, or it is the packet's only frame).
  bool split_tail_counted = false;
  bool split_tail_claimed = false;  // ClaimSplitTail applied
  // First frame start (walked, or the split tail) at or after frame_offset;
  // frame_offset itself when the offset lies before the first-frame offset or
  // no later frame start was found. Used by xma_canary_fixes (Xenia Canary
  // 5dd1cdbbf: a loop_start one bit short of a frame boundary).
  uint32_t next_frame_start = 0;

  bool IsLast() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

// Upstream XmaContext::GetPacketInfo (same walk, now replaced by this) plus
// the split tail.
inline PacketFrames AnalyzePacketFrames(const uint8_t* packet, uint32_t frame_offset) {
  PacketFrames r;
  r.first_frame_offset = WalkFirstFrameOffset(packet);
  r.next_frame_start = frame_offset;
  bool start_resolved = false;
  auto consider_start = [&](size_t start) {
    if (!start_resolved && start >= frame_offset) {
      start_resolved = true;
      r.next_frame_start = uint32_t(start);
    }
  };
  // BitStream::SetOffset clamps to the packet size.
  size_t pos = std::min<size_t>(r.first_frame_offset, kWalkBitsPerPacket);

  if (frame_offset < r.first_frame_offset) {
    r.current_frame = 0;
    r.current_frame_size = r.first_frame_offset - frame_offset;
    start_resolved = true;
  }

  bool ended_short = false;
  while (true) {
    const size_t remaining = kWalkBitsPerPacket - pos;
    if (remaining < kWalkFrameHeaderBits) {
      ended_short = true;
      break;
    }
    const uint32_t frame_size = ReadBitsMsb(packet, kWalkBitsPerPacket, pos, kWalkFrameHeaderBits);
    if (frame_size == 0 || frame_size == kWalkMaxFrameLength) {
      break;
    }
    consider_start(pos);
    if (pos == frame_offset) {
      r.current_frame = r.walked;
      r.current_frame_size = frame_size;
    }
    ++r.walked;
    if (frame_size > remaining) {
      break;
    }
    pos += frame_size - 1;
    const uint32_t trailer = ReadBitsMsb(packet, kWalkBitsPerPacket, pos, 1);
    pos += 1;
    if (trailer == 0) {
      break;
    }
  }
  // ended_short: the walk stopped at a frame start (the first-frame offset or
  // after a trailer 1) with fewer than 15 bits left.
  if (ended_short && pos < kWalkBitsPerPacket) {
    r.split_tail_pos = uint32_t(pos);
    r.split_tail_bits = uint32_t(kWalkBitsPerPacket - pos);
    consider_start(pos);
  }

  r.frame_count = r.walked;
  r.xma2 = WalkIsXma2(packet);
  if (r.xma2) {
    r.header_count = WalkHeaderFrameCount(packet);
    if (r.header_count > r.frame_count) {
      if (r.current_frame_size == 0) {
        r.current_frame = r.frame_count;
      }
      r.frame_count = r.header_count;
    }
  }
  r.split_tail_counted = r.split_tail_bits && (r.walked == 0 || r.frame_count > r.walked);
  return r;
}

// xma_frame_walk_fix: count the split-tail frame from the trailer bit when the
// header count does not include it, so the frame before it is not the last
// one and the split-header path decodes it.
inline void ClaimSplitTail(PacketFrames* r, uint32_t frame_offset) {
  if (!r->split_tail_bits || r->split_tail_counted) {
    return;
  }
  r->frame_count = std::max(r->frame_count, r->walked + 1);
  if (frame_offset == r->split_tail_pos) {
    r->current_frame = r->walked;
    r->current_frame_size = 0;
  }
  r->split_tail_claimed = true;
}

// 15-bit length of a frame whose header starts at packet-relative bit
// `relative_offset` and may continue in next_packet's data (upstream's
// combined bit stream of both packet payloads).
inline uint32_t ReadSplitFrameSize(const uint8_t* packet, const uint8_t* next_packet,
                                   uint32_t relative_offset) {
  const uint32_t in_packet =
      relative_offset < kWalkBitsPerPacket ? kWalkBitsPerPacket - relative_offset : 0;
  if (in_packet >= kWalkFrameHeaderBits) {
    return ReadBitsMsb(packet, kWalkBitsPerPacket, relative_offset, kWalkFrameHeaderBits);
  }
  const uint32_t rest = kWalkFrameHeaderBits - in_packet;
  const uint32_t high = ReadBitsMsb(packet, kWalkBitsPerPacket, relative_offset, in_packet);
  const uint32_t low = ReadBitsMsb(next_packet, kWalkBitsPerPacket, kWalkPacketHeaderBits, rest);
  return (high << rest) | low;
}

enum class SplitTailCheck : uint8_t {
  kNotReal,       // the next packet's continuation does not fit a frame here
  kExact,         // next packet's first frame starts right after this frame
  kAfterPadding,  // next packet's first frame starts later (frame trailer 0)
};

// Does next_packet carry the continuation of a frame starting at tail_pos?
inline SplitTailCheck CheckSplitTail(const uint8_t* packet, const uint8_t* next_packet,
                                     uint32_t tail_pos) {
  if (!next_packet || tail_pos >= kWalkBitsPerPacket) {
    return SplitTailCheck::kNotReal;
  }
  const uint32_t in_packet = kWalkBitsPerPacket - tail_pos;
  const uint32_t frame_size = ReadSplitFrameSize(packet, next_packet, tail_pos);
  if (frame_size == 0 || frame_size == kWalkMaxFrameLength || frame_size <= in_packet ||
      frame_size < kWalkFrameHeaderBits + 1) {
    return SplitTailCheck::kNotReal;
  }
  const uint32_t needed = frame_size - in_packet;
  const uint32_t next_first = WalkFirstFrameOffset(next_packet);
  if (next_first > kWalkMaxFirstFrameOffset) {
    // No frame starts in the next packet: the frame must cover all of it.
    return needed >= kWalkBitsPerPacket - kWalkPacketHeaderBits ? SplitTailCheck::kAfterPadding
                                                                : SplitTailCheck::kNotReal;
  }
  const uint32_t continuation = next_first - kWalkPacketHeaderBits;
  if (continuation == needed) {
    return SplitTailCheck::kExact;
  }
  return continuation > needed ? SplitTailCheck::kAfterPadding : SplitTailCheck::kNotReal;
}

// Upstream "!isLastFrameInPacket" next offset: the same packet, modulo the
// packet size (a frame ending exactly on the packet end wraps to bit 0).
inline uint32_t NextReadOffsetInPacket(uint32_t read_offset, uint32_t bits_to_copy) {
  const uint32_t packet_index = read_offset / kWalkBitsPerPacket;
  return packet_index * kWalkBitsPerPacket + (read_offset + bits_to_copy) % kWalkBitsPerPacket;
}

// XmaContext::GetNextPacketReadOffset: first frame start at or after packet
// next_packet_index of the buffer; kWalkPacketHeaderBits = none (swap).
//
// follow_skip (xma_canary_fixes; Xenia Canary 7015473a7, BSD-3, "Follow the
// sub-stream skip chain past frameless packets"): a packet in which no frame
// starts is the continuation of a frame of the same sub-stream, so the search
// continues at that packet's own skip count + 1 instead of the next sequential
// packet (which may belong to another interleaved sub-stream); a frameless
// packet with skip count 0xFF ends the search. Without follow_skip: the
// sequential upstream scan.
inline uint32_t FindNextPacketReadOffset(const uint8_t* buffer, uint32_t next_packet_index,
                                         uint32_t packet_count, bool follow_skip = false) {
  while (next_packet_index < packet_count) {
    const uint8_t* next_packet = buffer + size_t(next_packet_index) * kWalkBytesPerPacket;
    const uint32_t offset = WalkFirstFrameOffset(next_packet);
    if (offset <= kWalkMaxFirstFrameOffset) {
      return next_packet_index * kWalkBitsPerPacket + offset;
    }
    if (!follow_skip) {
      ++next_packet_index;
      continue;
    }
    const uint32_t next_skip = WalkSkipCount(next_packet);
    if (next_skip == 0xFF) {
      break;
    }
    next_packet_index += next_skip + 1;
  }
  return kWalkPacketHeaderBits;
}

// ---------------------------------------------------------------------------
// Frame continuity: each frame must start where the previous one ended.

enum class WalkPath : uint8_t {
  kInPacket,      // fits in the packet, more frames follow in it
  kLastInPacket,  // fits, last frame of the packet
  kSpanning,      // continues in the next packet
  kSplitHeader,   // its 15-bit header straddles the packet boundary
};

inline const char* WalkPathName(WalkPath path) {
  switch (path) {
    case WalkPath::kInPacket:
      return "in-packet";
    case WalkPath::kLastInPacket:
      return "last";
    case WalkPath::kSpanning:
      return "spanning";
    case WalkPath::kSplitHeader:
      return "split-header";
  }
  return "?";
}

// Where the previous decoded frame ended: guest address of the packet holding
// its last bit and the packet-relative bit after it (16384 = packet end).
struct WalkFrameEnd {
  bool valid = false;
  uint32_t packet_addr = 0;
  uint32_t end_bit = 0;
  bool trailer = false;  // the frame's trailer bit
  // The 15 bits after the frame read as length 0 or 0x7FFF (padding marker:
  // no further frame in that packet despite trailer 1).
  bool next_is_marker = false;
};

// next_is_marker for a frame ending at packet-relative bit end_bit.
inline bool LengthMarkerAt(const uint8_t* packet, uint32_t end_bit) {
  if (end_bit + kWalkFrameHeaderBits > kWalkBitsPerPacket) {
    return false;
  }
  const uint32_t v = ReadBitsMsb(packet, kWalkBitsPerPacket, end_bit, kWalkFrameHeaderBits);
  return v == 0 || v == kWalkMaxFrameLength;
}

enum class WalkVerdict : uint8_t {
  kFirst,       // no previous frame (start, Clear, loop or guest seek)
  kContinuous,  // starts where expected
  kDup,         // starts before the previous frame's end: bits decoded again
  kGap,         // a frame start was passed over
};

inline const char* WalkVerdictName(WalkVerdict verdict) {
  switch (verdict) {
    case WalkVerdict::kFirst:
      return "first";
    case WalkVerdict::kContinuous:
      return "ok";
    case WalkVerdict::kDup:
      return "dup";
    case WalkVerdict::kGap:
      return "gap";
  }
  return "?";
}

// start_first_frame_offset: the start packet's header first-frame offset.
inline WalkVerdict ClassifyFrameStart(const WalkFrameEnd& prev, uint32_t start_packet_addr,
                                      uint32_t start_bit, uint32_t start_first_frame_offset) {
  if (!prev.valid) {
    return WalkVerdict::kFirst;
  }
  if (start_packet_addr == prev.packet_addr) {
    if (start_bit < prev.end_bit) {
      return WalkVerdict::kDup;
    }
    if (start_bit == prev.end_bit || !prev.trailer) {
      return WalkVerdict::kContinuous;
    }
    return WalkVerdict::kGap;
  }
  // Another packet. With trailer 1 and bits left, a frame (whole or with a
  // straddling header) started in the previous end packet and was skipped.
  if (prev.trailer && prev.end_bit < kWalkBitsPerPacket && !prev.next_is_marker) {
    return WalkVerdict::kGap;
  }
  if (start_bit == start_first_frame_offset) {
    return WalkVerdict::kContinuous;
  }
  return start_bit > start_first_frame_offset ? WalkVerdict::kGap : WalkVerdict::kDup;
}

// ---------------------------------------------------------------------------
// Single-buffer walk in XmaContext::Decode's order (no swaps, loops or output
// limits), for the regression tests. Decode composes the same helpers.

struct WalkStep {
  uint32_t read_offset = 0;  // absolute bit offset of the frame start
  uint32_t frame_size = 0;
  WalkPath path = WalkPath::kInPacket;
  WalkVerdict verdict = WalkVerdict::kFirst;
  bool claimed = false;  // the split tail was claimed for this frame's packet
};

inline std::vector<WalkStep> SimulateBufferWalk(const uint8_t* buffer, uint32_t packet_count,
                                                bool fix, size_t max_steps = 4096) {
  std::vector<WalkStep> steps;
  uint32_t read_offset = kWalkPacketHeaderBits;
  WalkFrameEnd prev;
  while (steps.size() < max_steps) {
    const uint32_t packet_index = read_offset / kWalkBitsPerPacket;
    if (packet_index >= packet_count) {
      break;
    }
    const uint8_t* packet = buffer + size_t(packet_index) * kWalkBytesPerPacket;
    const uint32_t packet_addr = packet_index * kWalkBytesPerPacket;
    const uint32_t first = WalkFirstFrameOffset(packet);
    uint32_t relative = read_offset % kWalkBitsPerPacket;
    if (relative < first) {
      read_offset = packet_index * kWalkBitsPerPacket + first;
      relative = first;
    }
    if (relative >= kWalkBitsPerPacket) {
      break;  // no frame start in this packet (Decode would mis-walk here)
    }
    const uint32_t skip = WalkSkipCount(packet);
    if (skip == 0xFF) {
      read_offset = FindNextPacketReadOffset(buffer, packet_index + 1, packet_count);
      if (read_offset == kWalkPacketHeaderBits) {
        break;
      }
      continue;
    }
    const uint32_t next_packet_index = packet_index + skip + 1;
    const uint8_t* next_packet = next_packet_index < packet_count
                                     ? buffer + size_t(next_packet_index) * kWalkBytesPerPacket
                                     : nullptr;
    PacketFrames info = AnalyzePacketFrames(packet, relative);
    if (fix && info.split_tail_bits && !info.split_tail_counted &&
        (!next_packet ||
         CheckSplitTail(packet, next_packet, info.split_tail_pos) != SplitTailCheck::kNotReal)) {
      ClaimSplitTail(&info, relative);
    }
    WalkStep step;
    step.read_offset = read_offset;
    step.claimed = info.split_tail_claimed;
    const bool split = info.current_frame_size == 0;
    if (split) {
      if (!next_packet) {
        break;  // Decode: swap (frame lost) or D2 stall
      }
      info.current_frame_size = ReadSplitFrameSize(packet, next_packet, relative);
      if (info.current_frame_size == kWalkMaxFrameLength) {
        break;  // Decode: error_status 4
      }
    }
    const uint32_t frame_size = info.current_frame_size;
    const uint32_t in_packet = kWalkBitsPerPacket - relative;
    const uint32_t bits_to_copy = std::min(in_packet, frame_size);
    if (bits_to_copy == 0) {
      break;
    }
    const bool last = info.IsLast();
    const bool spans = last && in_packet < frame_size;
    if (spans && !next_packet) {
      break;  // Decode: error_status 4 until the next buffer arrives
    }
    step.frame_size = frame_size;
    step.path = split   ? WalkPath::kSplitHeader
                : spans ? WalkPath::kSpanning
                : last  ? WalkPath::kLastInPacket
                        : WalkPath::kInPacket;
    step.verdict = ClassifyFrameStart(prev, packet_addr, relative, first);
    // End of this frame and its trailer bit.
    if (bits_to_copy < frame_size) {
      prev.packet_addr = next_packet_index * kWalkBytesPerPacket;
      prev.end_bit = kWalkPacketHeaderBits + (frame_size - bits_to_copy);
      prev.trailer = ReadBitsMsb(next_packet, kWalkBitsPerPacket, prev.end_bit - 1, 1) != 0;
      prev.next_is_marker = LengthMarkerAt(next_packet, prev.end_bit);
    } else {
      prev.packet_addr = packet_addr;
      prev.end_bit = relative + frame_size;
      prev.trailer = ReadBitsMsb(packet, kWalkBitsPerPacket, prev.end_bit - 1, 1) != 0;
      prev.next_is_marker = LengthMarkerAt(packet, prev.end_bit);
    }
    prev.valid = true;
    steps.push_back(step);

    if (!last) {
      read_offset = NextReadOffsetInPacket(read_offset, bits_to_copy);
      continue;
    }
    read_offset = FindNextPacketReadOffset(buffer, next_packet_index, packet_count);
    if (read_offset == kWalkPacketHeaderBits) {
      break;  // Decode: swap to the other input buffer
    }
  }
  return steps;
}

// ---------------------------------------------------------------------------
// Counters (MCLA_XMA_SIB v=3 fields) and the anomaly line.

struct WalkMinuteTotals {
  uint64_t dup = 0;            // frames that started before the previous frame's end
  uint64_t gap = 0;            // frame starts passed over
  uint64_t ctxs = 0;           // distinct contexts with a dup or gap this minute
  uint64_t boundary = 0;       // frames spanning a packet boundary (incl. split header)
  uint64_t split_tail = 0;     // frames ending with 1-14 bits left and trailer 1
  uint64_t split_counted = 0;  // ... whose next frame upstream counts (header count)
  uint64_t split_lost = 0;     // ... not counted and not claimed: unconfirmed tails
                               // (real or padding); real loss = split_real - claimed - counted
  uint64_t split_real = 0;     // ... confirmed by the next packet's continuation
  uint64_t split_claimed = 0;  // split tails claimed by xma_frame_walk_fix
  uint64_t split_padding = 0;  // claimed tails found to be padding later (fix)
  uint64_t dup_prevented = 0;  // re-decodes skipped by xma_frame_walk_fix
  uint64_t resyncs = 0;        // guest moved the read offset between Works
  uint64_t input_files = 0;    // xma_dump_input files opened
};

inline std::string FormatWalkMinuteFields(const WalkMinuteTotals& w, bool fix) {
  return fmt::format(
      "walk_dup={} walk_gap={} walk_ctxs={} walk_boundary={} walk_split_tail={} "
      "walk_split_counted={} walk_split_lost={} walk_split_real={} walk_split_claimed={} "
      "walk_split_padding={} walk_dup_prevented={} walk_resyncs={} walk_fix={} input_dump_files={}",
      w.dup, w.gap, w.ctxs, w.boundary, w.split_tail, w.split_counted, w.split_lost, w.split_real,
      w.split_claimed, w.split_padding, w.dup_prevented, w.resyncs, int(fix), w.input_files);
}

}  // namespace rex::audio::xma
