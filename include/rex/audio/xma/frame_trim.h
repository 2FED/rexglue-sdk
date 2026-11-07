/**
 ******************************************************************************
 * ReXGlue - KI-071 XMA per-frame trim, PCM dump and start-burst helpers      *
 ******************************************************************************
 */

// KI-071 step 3. The cut-scene voice siblings decode and are consumed in exact
// lock-step, yet the mix carries copies at exactly 1 and 2 XMA frames. The
// FFmpeg xmaframes decoder (thirdparty/FFmpeg/libavcodec/wmaprodec.c,
// decode_frame, "no idea what these are for" block) reads the optional start
// and end skip fields of every frame header into `int av_unused skip` and
// always outputs the full 512 samples. This header re-parses those fields
// read-only from the frame bits the SDK hands to FFmpeg (FFmpeg is not
// touched), applies them to the converted frame when xma_honor_frame_trim is
// on, and holds the pure parts of the per-context WAV dump and the
// first-decode burst budget. All pure so the unit tests can cover them.

#pragma once

#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>

// Apply the per-frame start/end trim to the decoded output (default off).
REXCVAR_DECLARE(bool, xma_honor_frame_trim);
// With xma_honor_frame_trim: drop whole 256-byte output blocks only (trim
// rounded down, remainder counted) instead of the sample-exact shift.
REXCVAR_DECLARE(bool, xma_frame_trim_block_granular);
// Directory for per-context decoded PCM dumps (empty = off).
REXCVAR_DECLARE(std::string, xma_dump_contexts);
// Dump every sample rate, not only 44.1 kHz.
REXCVAR_DECLARE(bool, xma_dump_all_rates);

namespace rex::audio::xma {

// ---------------------------------------------------------------------------
// Frame header trim fields.
//
// The xmaframes decoder is initialised with decode_flags 0x10d6
// (wmaprodec.c decode_init, AV_CODEC_ID_XMAFRAMES) and block_align 2048, so:
//   len_prefix (0x40)        -> 15-bit frame length (log2_frame_size = 11 + 4)
//   log2_max_num_subframes 2 -> max_num_subframes 4, max_subframe_len_bit 1,
//                               subframe_len_bits 2, min subframe 128 samples
//   DRC (0x80)               -> 8-bit drc_gain
//   samples_per_frame 512    -> trim fields av_log2(1024) = 10 bits
// decode_frame order: length, tile header (decode_tilehdr), postproc
// transform (stereo only), drc gain, then
//   1 bit  trim present
//     1 bit start present, 10 bits start skip   ("usually true for the first frame")
//     1 bit end present,   10 bits end skip     ("sometimes true for the last frame")

inline constexpr uint32_t kTrimSamplesPerFrame = 512;
inline constexpr uint32_t kTrimMinSubframe = 128;
inline constexpr uint32_t kTrimFrameLengthBits = 15;
inline constexpr uint32_t kTrimFieldBits = 10;
inline constexpr uint32_t kTrimMaxSubframes = 32;  // wmaprodec.c MAX_SUBFRAMES

// MSB-first bit reader with a hard end (FFmpeg get_bits order).
class MsbBitReader {
 public:
  MsbBitReader(const uint8_t* data, uint64_t bit_offset, uint64_t bit_end)
      : data_(data), pos_(bit_offset), end_(bit_end) {}
  bool ok() const { return ok_; }
  uint64_t pos() const { return pos_; }
  uint64_t left() const { return pos_ < end_ ? end_ - pos_ : 0; }
  uint32_t Read(uint32_t bits) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bits; ++i) {
      if (pos_ >= end_) {
        ok_ = false;
        return 0;
      }
      v = (v << 1) | ((data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1u);
      ++pos_;
    }
    return v;
  }

 private:
  const uint8_t* data_;
  uint64_t pos_;
  uint64_t end_;
  bool ok_ = true;
};

struct FrameTrimInfo {
  bool ok = false;    // header parsed up to the end of the trim fields
  bool flag = false;  // trim-present bit
  bool has_start = false;
  bool has_end = false;
  uint32_t start = 0;        // start skip, samples per channel (0..1023)
  uint32_t end = 0;          // end skip, samples per channel (0..1023)
  uint32_t header_bits = 0;  // frame start to the end of the trim fields
};

namespace detail {
// wmaprodec.c decode_subframe_length with the xmaframes constants.
inline int TrimSubframeLength(MsbBitReader& br, uint32_t offset) {
  if (offset == kTrimSamplesPerFrame - kTrimMinSubframe) {
    return int(kTrimMinSubframe);
  }
  if (br.left() < 1) {
    return -1;
  }
  uint32_t shift = 0;
  if (br.Read(1)) {
    shift = 1 + br.Read(1);  // subframe_len_bits - 1 = 1
  }
  if (!br.ok()) {
    return -1;
  }
  return int(kTrimSamplesPerFrame >> shift);  // 512, 256 or 128: always in range
}

// wmaprodec.c decode_tilehdr (bit consumption only).
inline bool TrimSkipTileHeader(MsbBitReader& br, uint32_t channels) {
  uint32_t num_samples[2] = {0, 0};
  uint32_t num_subframes[2] = {0, 0};
  uint8_t contains[2] = {0, 0};
  uint32_t channels_for_cur = channels;
  uint32_t min_len = 0;
  // max_num_subframes is 4 (not 1): the fixed-layout bit is always present.
  const bool fixed = br.Read(1) != 0;
  if (!br.ok()) {
    return false;
  }
  for (uint32_t guard = 0; guard < 4096; ++guard) {
    for (uint32_t c = 0; c < channels; ++c) {
      if (num_samples[c] == min_len) {
        if (fixed || channels_for_cur == 1 || min_len == kTrimSamplesPerFrame - kTrimMinSubframe) {
          contains[c] = 1;
        } else {
          contains[c] = uint8_t(br.Read(1));
        }
      } else {
        contains[c] = 0;
      }
    }
    if (!br.ok()) {
      return false;
    }
    const int len = TrimSubframeLength(br, min_len);
    if (len <= 0) {
      return false;
    }
    min_len += uint32_t(len);
    for (uint32_t c = 0; c < channels; ++c) {
      if (contains[c]) {
        if (num_subframes[c] >= kTrimMaxSubframes) {
          return false;
        }
        num_samples[c] += uint32_t(len);
        ++num_subframes[c];
        if (num_samples[c] > kTrimSamplesPerFrame) {
          return false;
        }
      } else if (num_samples[c] <= min_len) {
        if (num_samples[c] < min_len) {
          channels_for_cur = 0;
          min_len = num_samples[c];
        }
        ++channels_for_cur;
      }
    }
    if (min_len >= kTrimSamplesPerFrame) {
      return true;
    }
  }
  return false;
}
}  // namespace detail

// Parses the trim fields of one XMA frame. `data` holds the frame bits
// starting at `bit_offset` (the 15-bit length field); `frame_bits` is the
// frame size. channels is 1 or 2. Read-only; mirrors decode_frame exactly up
// to the trim fields.
inline FrameTrimInfo ParseFrameTrim(const uint8_t* data, size_t data_bytes, uint32_t bit_offset,
                                    uint32_t frame_bits, uint32_t channels) {
  FrameTrimInfo info;
  if (!data || (channels != 1 && channels != 2)) {
    return info;
  }
  const uint64_t end = std::min<uint64_t>(uint64_t(bit_offset) + frame_bits, data_bytes * 8ull);
  MsbBitReader br(data, bit_offset, end);
  br.Read(kTrimFrameLengthBits);
  if (!br.ok() || !detail::TrimSkipTileHeader(br, channels)) {
    return info;
  }
  // Postproc transform (nb_channels > 1).
  if (channels > 1 && br.Read(1)) {
    if (br.Read(1)) {
      br.Read(4 * channels * channels);
    }
  }
  br.Read(8);  // drc_gain (decode_flags 0x80)
  info.flag = br.Read(1) != 0;
  if (info.flag) {
    info.has_start = br.Read(1) != 0;
    if (info.has_start) {
      info.start = br.Read(kTrimFieldBits);
    }
    info.has_end = br.Read(1) != 0;
    if (info.has_end) {
      info.end = br.Read(kTrimFieldBits);
    }
  }
  if (!br.ok()) {
    return FrameTrimInfo{};
  }
  info.ok = true;
  info.header_bits = uint32_t(br.pos() - bit_offset);
  return info;
}

// ---------------------------------------------------------------------------
// Trim application on one converted frame.
//
// The frame buffer holds 512 samples per channel as big-endian int16,
// interleaved for stereo (XmaContext::ConvertFrame). The output ring and the
// guest's write offset are in 256-byte blocks (128 mono samples, 64 stereo
// sample pairs), so only whole blocks can leave a frame. Two modes:
//  - sample-exact (default): the kept samples are appended to a carry of less
//    than one block left by the previous frame; whole blocks are emitted and
//    the rest is carried, so every later sample shifts by exactly the trim.
//  - block-granular: the head/tail trims are rounded down to whole blocks and
//    the remainder is counted (not dropped); no carry.
// The emitted blocks are moved to the END of the frame buffer so that
// XmaContext::Consume, which reads block (total - remaining), sees them as
// the remaining blocks. A start trim larger than one frame (10-bit fields go
// to 1023) continues into the next frames; an end trim larger than what is
// left of the frame is counted as unapplied.

inline constexpr uint32_t kTrimBlockBytes = 256;

struct FrameTrimCarry {
  std::array<uint8_t, kTrimBlockBytes> bytes{};
  uint32_t size = 0;           // bytes of a partial block carried to the next frame
  uint32_t pending_start = 0;  // start-trim samples still to drop from later frames
  void Reset() {
    size = 0;
    pending_start = 0;
  }
};

struct FrameTrimApplied {
  uint32_t blocks = 0;             // output blocks, at the end of the frame buffer
  uint32_t dropped_samples = 0;    // per channel, dropped from this frame
  uint32_t unapplied_end = 0;      // end trim beyond what the frame had left
  uint32_t unaligned_samples = 0;  // block mode: trim samples not dropped
};

// frame: the converted frame, at least 512 * 2 * channels bytes.
inline FrameTrimApplied ApplyFrameTrim(std::span<uint8_t> frame, uint32_t channels, uint32_t start,
                                       uint32_t end, bool block_granular, FrameTrimCarry& carry) {
  FrameTrimApplied r;
  const uint32_t bpf = 2 * channels;  // bytes per sample frame
  const uint32_t frame_bytes = kTrimSamplesPerFrame * bpf;
  const uint32_t frame_blocks = frame_bytes / kTrimBlockBytes;
  if (frame.size() < frame_bytes || (channels != 1 && channels != 2)) {
    r.blocks = frame_blocks;
    return r;
  }
  const uint32_t total_start = carry.pending_start + start;
  uint32_t head = std::min(total_start, kTrimSamplesPerFrame);
  carry.pending_start = total_start - head;
  const uint32_t avail = kTrimSamplesPerFrame - head;
  uint32_t tail = std::min(end, avail);
  r.unapplied_end = end - tail;

  if (block_granular) {
    const uint32_t block_samples = kTrimBlockBytes / bpf;
    const uint32_t head_blocks = head / block_samples;
    const uint32_t tail_blocks = tail / block_samples;
    r.unaligned_samples = (head % block_samples) + (tail % block_samples);
    const uint32_t keep_blocks = frame_blocks - head_blocks - tail_blocks;
    r.dropped_samples = (head_blocks + tail_blocks) * block_samples;
    if (keep_blocks && head_blocks + tail_blocks) {
      std::memmove(frame.data() + (frame_bytes - keep_blocks * kTrimBlockBytes),
                   frame.data() + head_blocks * kTrimBlockBytes, keep_blocks * kTrimBlockBytes);
    }
    r.blocks = keep_blocks;
    return r;
  }

  r.dropped_samples = head + tail;
  if (carry.size == 0 && head == 0 && tail == 0) {
    r.blocks = frame_blocks;
    return r;
  }
  const uint32_t keep = avail - tail;
  std::array<uint8_t, kTrimBlockBytes + kTrimSamplesPerFrame * 4> tmp{};
  std::memcpy(tmp.data(), carry.bytes.data(), carry.size);
  std::memcpy(tmp.data() + carry.size, frame.data() + head * bpf, keep * bpf);
  const uint32_t total = carry.size + keep * bpf;
  const uint32_t blocks = total / kTrimBlockBytes;  // <= frame_blocks (carry < 1 block)
  const uint32_t rem = total % kTrimBlockBytes;
  std::memset(frame.data(), 0, frame_bytes - blocks * kTrimBlockBytes);
  std::memcpy(frame.data() + (frame_bytes - blocks * kTrimBlockBytes), tmp.data(),
              blocks * kTrimBlockBytes);
  std::memcpy(carry.bytes.data(), tmp.data() + blocks * kTrimBlockBytes, rem);
  carry.size = rem;
  r.blocks = blocks;
  return r;
}

// ---------------------------------------------------------------------------
// WAV dump helpers.

// 44-byte canonical RIFF/WAVE header, 16-bit PCM, little-endian.
inline std::array<uint8_t, 44> MakeWavHeader(uint32_t sample_rate, uint16_t channels,
                                             uint32_t data_bytes) {
  std::array<uint8_t, 44> h{};
  auto put32 = [&h](size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
      h[at + i] = uint8_t(v >> (8 * i));
    }
  };
  auto put16 = [&h](size_t at, uint16_t v) {
    h[at] = uint8_t(v);
    h[at + 1] = uint8_t(v >> 8);
  };
  std::memcpy(h.data(), "RIFF", 4);
  put32(4, 36 + data_bytes);
  std::memcpy(h.data() + 8, "WAVEfmt ", 8);
  put32(16, 16);
  put16(20, 1);  // PCM
  put16(22, channels);
  put32(24, sample_rate);
  put32(28, sample_rate * channels * 2);
  put16(32, uint16_t(channels * 2));
  put16(34, 16);
  std::memcpy(h.data() + 36, "data", 4);
  put32(40, data_bytes);
  return h;
}

// Ring blocks are big-endian int16; WAV wants little-endian.
inline void SwapPcm16(const uint8_t* in, uint8_t* out, size_t bytes) {
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    out[i] = in[i + 1];
    out[i + 1] = in[i];
  }
}

inline std::string DumpFileName(uint32_t ctx, uint32_t rate, bool stereo, uint32_t n) {
  return fmt::format("ctx{}-{}Hz-{}-{}.wav", ctx, rate, stereo ? "stereo" : "mono", n);
}

inline constexpr uint32_t kDumpMaxSeconds = 60;
inline constexpr uint32_t kDumpMaxFiles = 64;

// ---------------------------------------------------------------------------
// First-decode burst budget: a context whose first decode comes within 50 ms
// of first decodes of 3 or more other 44.1 kHz contexts belongs to a
// multi-context start (the cut-scene voice is 7). Its record is always logged
// (own budget), and earlier records of the window that the shared budget
// suppressed are flushed with it.
class StartBurstWindow {
 public:
  static constexpr int64_t kWindowUs = 50000;
  static constexpr size_t kMinOthers = 3;
  static constexpr size_t kMaxEntries = 128;

  // True when this start is part of a burst; `flush` then receives the lines
  // of earlier starts in the window that were not logged (marked logged).
  bool IsBurst(int64_t now_us, uint32_t ctx, std::vector<std::string>* flush) {
    while (!entries_.empty() && now_us - entries_.front().us > kWindowUs) {
      entries_.pop_front();
    }
    std::vector<uint32_t> others;
    for (const Entry& e : entries_) {
      if (e.ctx != ctx && std::find(others.begin(), others.end(), e.ctx) == others.end()) {
        others.push_back(e.ctx);
      }
    }
    if (others.size() < kMinOthers) {
      return false;
    }
    for (Entry& e : entries_) {
      if (!e.logged) {
        e.logged = true;
        if (flush) {
          flush->push_back(std::move(e.line));
        }
        e.line.clear();
      }
    }
    return true;
  }

  void Record(int64_t now_us, uint32_t ctx, bool logged, std::string line) {
    if (entries_.size() == kMaxEntries) {
      entries_.pop_front();
    }
    entries_.push_back({now_us, ctx, logged, logged ? std::string() : std::move(line)});
  }

 private:
  struct Entry {
    int64_t us;
    uint32_t ctx;
    bool logged;
    std::string line;
  };
  std::deque<Entry> entries_;
};

}  // namespace rex::audio::xma
