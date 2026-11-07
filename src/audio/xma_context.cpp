/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fmt/format.h>

#include <rex/audio/xma/context.h>
#include <rex/audio/audio_route_audit.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/doubling_fixes.h>
#include <rex/audio/xma/frame_trim.h>
#include <rex/audio/xma/frame_walk.h>
#include <rex/audio/xma/helpers.h>
#include <rex/audio/xma/sibling_trace.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

// KI-071 (doubled cut-scene dialogue). All default off; see
// rex/audio/xma/doubling_fixes.h for the rules.
REXCVAR_DEFINE_BOOL(xma_swap_carry_packet, false, "Audio",
                    "KI-071 C1: after an XMA input-buffer swap continue at packet "
                    "next_packet_index - packet_count of the new buffer instead of packet 0");
REXCVAR_DEFINE_BOOL(xma_valid_only_when_written, false, "Audio",
                    "KI-071 C2: clear the XMA output_buffer_valid flag only when the Work wrote "
                    "at least one block");
REXCVAR_DEFINE_BOOL(xma_work_lock_recheck, false, "Audio",
                    "KI-071 C3: re-check the XMA context enable flag after taking the lock and "
                    "do not write back the guest-owned output_buffer_read_offset");
REXCVAR_DEFINE_BOOL(xma_swap_trace, false, "Audio",
                    "KI-071: log XMA input-buffer swaps (paced) and a per-minute MCLA_XMA line");
// KI-071 step 2 (sibling contexts); see rex/audio/xma/sibling_trace.h.
REXCVAR_DEFINE_BOOL(xma_sibling_trace, false, "Audio",
                    "KI-071: per-context XMA frame/drop counters, first-decode record, paced "
                    "sibling group lines and a per-minute MCLA_XMA_SIB line");
REXCVAR_DEFINE_BOOL(xma_stall_on_invalid_next, false, "Audio",
                    "KI-071 D2: at a buffer boundary with the next input buffer not yet valid, "
                    "keep the read offset and retry later instead of swapping and dropping the "
                    "frame; clear error_status 4 after a successful decode");
REXCVAR_DEFINE_BOOL(xma_honor_start_skip, false, "Audio",
                    "KI-071 D1: discard dword1 bits 24-26 subframes of the first frame decoded "
                    "after a context Clear (guest start subframe skip)");
REXCVAR_DEFINE_BOOL(xma_reset_codec_on_clear, false, "Audio",
                    "KI-071: re-create the FFmpeg XMA decoder on context Clear/Release so a "
                    "reused context does not carry the previous wave's overlap tail");
// KI-071 step 3 (per-frame trim, PCM dump); see rex/audio/xma/frame_trim.h.
REXCVAR_DEFINE_BOOL(xma_honor_frame_trim, false, "Audio",
                    "KI-071: drop the XMA frame header start/end skip samples from each decoded "
                    "frame (FFmpeg parses and ignores them); sample-exact unless "
                    "xma_frame_trim_block_granular");
REXCVAR_DEFINE_BOOL(xma_frame_trim_block_granular, false, "Audio",
                    "KI-071: with xma_honor_frame_trim, drop only whole 256-byte output blocks "
                    "(trim rounded down) instead of shifting the following samples");
REXCVAR_DEFINE_STRING(xma_dump_contexts, "", "Audio",
                      "KI-071: directory for per-context decoded PCM dumps "
                      "(ctx<id>-<rate>Hz-<mono|stereo>-<n>.wav, as written to the output ring; "
                      "44.1 kHz only unless xma_dump_all_rates; 90 s per stream, 128 files)");
REXCVAR_DEFINE_BOOL(xma_dump_all_rates, false, "Audio",
                    "KI-071: with xma_dump_contexts, dump every sample rate, not only 44.1 kHz");
REXCVAR_DEFINE_BOOL(xma_dump_burst_only, true, "Audio",
                    "KI-071: with xma_dump_contexts, dump only contexts whose first decode falls "
                    "within 50 ms of first decodes of 3+ other contexts of the same rate, and "
                    "their siblings (same start kick burst), for the whole wave");
// KI-071 step 4 (frame walk continuity); see rex/audio/xma/frame_walk.h.
REXCVAR_DEFINE_BOOL(xma_frame_walk_trace, false, "Audio",
                    "KI-071: log the first 8 XMA frame-walk anomalies per context (a frame "
                    "starting before the previous frame's end or after a skipped frame start); "
                    "the counters in MCLA_XMA_SIB always run");
REXCVAR_DEFINE_BOOL(xma_frame_walk_fix, true, "Audio",
                    "KI-071 (on by default since l77, verified fixed): count an XMA frame whose "
                    "15-bit header straddles the packet end from the previous frame's trailer "
                    "bit (upstream relies on the packet header frame count and skips the "
                    "frame), and never re-decode bits before the previous frame's end within a "
                    "packet; false restores the upstream walk. In MCLA_XMA_SIB, "
                    "walk_split_lost counts unclaimed split tails (unconfirmed: real or "
                    "padding); the real loss is walk_split_real - walk_split_claimed - "
                    "walk_split_counted");
REXCVAR_DEFINE_BOOL(xma_dump_input, false, "Audio",
                    "KI-071: with xma_dump_contexts, also write each dumped context's input "
                    "buffers (<wav>-input.bin, 2 KiB packets in decode order, index "
                    "<wav>-input.csv) and a per-frame walk record (<wav>-walk.csv)");

namespace rex::audio {

using stream::BitStream;

namespace {

// KI-071 counters, reported once per minute (MCLA_XMA kind=minute).
struct XmaDoublingCounters {
  std::atomic<uint64_t> swaps{0};
  std::atomic<uint64_t> carry_nonzero{0};
  // Same two counts for 44.1 kHz contexts only (the measured doubling is
  // 512/1024 samples of a 44.1 kHz stream).
  std::atomic<uint64_t> swaps_44k{0};
  std::atomic<uint64_t> carry_nonzero_44k{0};
  std::atomic<uint64_t> carry_fallbacks{0};
  std::atomic<uint64_t> stale_ring_replays{0};
  std::atomic<uint64_t> lock_races{0};
  std::atomic<uint64_t> read_offset_reverts{0};
  std::atomic<uint64_t> trace_suppressed{0};
};
XmaDoublingCounters g_xma_counters;

// KI-071 step 2 event totals (MCLA_XMA_SIB kind=minute), all contexts.
struct XmaSiblingCounters {
  std::atomic<uint64_t> drops_split{0};
  std::atomic<uint64_t> drops_ff{0};
  std::atomic<uint64_t> err4{0};
  std::atomic<uint64_t> decode_fail{0};
  std::atomic<uint64_t> stalls{0};
  std::atomic<uint64_t> stall_timeouts{0};
  std::atomic<uint64_t> err4_cleared{0};
  std::atomic<uint64_t> swapbacks{0};
  std::atomic<uint64_t> no_bits{0};
  std::atomic<uint64_t> start_skips{0};
  std::atomic<uint64_t> start_skips_applied{0};
  std::atomic<uint64_t> codec_resets{0};
  std::atomic<uint64_t> first_decodes{0};
  std::atomic<uint64_t> trim_frames{0};
  std::atomic<uint64_t> trim_samples{0};
  std::atomic<uint64_t> trim_ctxs{0};
  std::atomic<uint64_t> trim_applied_samples{0};
  std::atomic<uint64_t> trim_unapplied{0};
  std::atomic<uint64_t> trim_parse_errors{0};
  std::atomic<uint64_t> burst_first_decodes{0};
};
XmaSiblingCounters g_xma_sib;

// KI-071 step 4 frame-walk totals (MCLA_XMA_SIB v=3), all contexts.
struct XmaWalkCounters {
  std::atomic<uint64_t> dup{0};
  std::atomic<uint64_t> gap{0};
  std::atomic<uint64_t> ctxs{0};
  std::atomic<uint64_t> boundary{0};
  std::atomic<uint64_t> split_tail{0};
  std::atomic<uint64_t> split_counted{0};
  std::atomic<uint64_t> split_lost{0};
  std::atomic<uint64_t> split_real{0};
  std::atomic<uint64_t> split_claimed{0};
  std::atomic<uint64_t> split_padding{0};
  std::atomic<uint64_t> dup_prevented{0};
  std::atomic<uint64_t> resyncs{0};
  std::atomic<uint64_t> input_files{0};
};
XmaWalkCounters g_xma_walk;
// Minute epoch for walk ctxs (incremented by TakeWalkMinuteTotals); starts at
// 1 so a fresh context (epoch 0) counts.
std::atomic<uint32_t> g_xma_walk_epoch{1};
// xma_dump_input caps.
constexpr uint64_t kInputDumpMaxBytes = 32ull * 1024 * 1024;
constexpr size_t kInputPendingMaxBytes = 2 * 1024 * 1024;
constexpr size_t kWalkCsvPendingMaxBytes = 1024 * 1024;
// Minute epoch for trim_ctxs (incremented by TakeSiblingMinuteTotals).
std::atomic<uint32_t> g_xma_trim_epoch{1};

void Bump(std::atomic<uint64_t>& counter, uint64_t n = 1) {
  counter.fetch_add(n, std::memory_order_relaxed);
}
std::atomic<int64_t> g_xma_minute_start_ms{-1};
std::mutex g_xma_trace_mutex;
int64_t g_xma_trace_last_ms = -1;
uint32_t g_xma_trace_milli_tokens = 0;

// Swap trace pacing: up to kTraceBurst lines at once, kTracePerSecond refill.
constexpr uint32_t kTraceBurst = 64;
constexpr uint32_t kTracePerSecond = 8;

int64_t XmaSteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool AllowXmaTraceLine() {
  const int64_t now = XmaSteadyNowMs();
  std::lock_guard<std::mutex> guard(g_xma_trace_mutex);
  constexpr uint32_t kBurstMilli = kTraceBurst * 1000;
  if (g_xma_trace_last_ms < 0) {
    g_xma_trace_milli_tokens = kBurstMilli;
  } else if (now > g_xma_trace_last_ms) {
    const uint64_t refill = uint64_t(now - g_xma_trace_last_ms) * kTracePerSecond;
    g_xma_trace_milli_tokens =
        uint32_t(std::min<uint64_t>(kBurstMilli, g_xma_trace_milli_tokens + refill));
  }
  g_xma_trace_last_ms = now;
  if (g_xma_trace_milli_tokens >= 1000) {
    g_xma_trace_milli_tokens -= 1000;
    return true;
  }
  g_xma_counters.trace_suppressed.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// KI-071 step 3: separate budgets (burst, per second) so the shared swap /
// first-decode bucket, exhausted during gameplay in l74, cannot hide them.
class XmaLineBudget {
 public:
  constexpr XmaLineBudget(uint32_t burst, uint32_t per_second)
      : burst_(burst), per_second_(per_second) {}
  bool Allow() {
    const int64_t now = XmaSteadyNowMs();
    std::lock_guard<std::mutex> guard(mutex_);
    const uint32_t burst_milli = burst_ * 1000;
    if (last_ms_ < 0) {
      milli_tokens_ = burst_milli;
    } else if (now > last_ms_) {
      const uint64_t refill = uint64_t(now - last_ms_) * per_second_;
      milli_tokens_ = uint32_t(std::min<uint64_t>(burst_milli, milli_tokens_ + refill));
    }
    last_ms_ = now;
    if (milli_tokens_ >= 1000) {
      milli_tokens_ -= 1000;
      return true;
    }
    g_xma_counters.trace_suppressed.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

 private:
  uint32_t burst_;
  uint32_t per_second_;
  std::mutex mutex_;
  int64_t last_ms_ = -1;
  uint32_t milli_tokens_ = 0;
};
// First-decode records of a multi-context 44.1 kHz start.
XmaLineBudget g_xma_burst_budget(128, 32);
// Per-frame trim lines (first 4 non-zero frames per context).
XmaLineBudget g_xma_trim_budget(64, 16);
// Frame-walk anomaly and summary lines (first 8 per context).
XmaLineBudget g_xma_walk_budget(96, 8);
// First decodes of 44.1 kHz contexts in the last 50 ms.
std::mutex g_xma_start_window_mutex;
xma::StartBurstWindow g_xma_start_window;

// Dump files opened this session (cap kDumpMaxFiles).
std::atomic<uint32_t> g_xma_dump_files{0};
std::atomic<bool> g_xma_dump_limit_logged{false};
// xma_dump_burst_only: burst serial per context id (0 = none), set by the
// tracker from the starting context's thread, reset on Clear/Release.
std::mutex g_xma_dump_burst_mutex;
xma::DumpBurstTracker g_xma_dump_burst;
std::array<std::atomic<uint32_t>, XmaContext::kTraceContextArrayCount> g_xma_dump_serial{};

int64_t XmaSteadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void MaybeEmitXmaMinute() {
  const int64_t now = XmaSteadyNowMs();
  int64_t start = g_xma_minute_start_ms.load(std::memory_order_relaxed);
  if (start < 0) {
    g_xma_minute_start_ms.compare_exchange_strong(start, now, std::memory_order_relaxed);
    return;
  }
  if (now - start < 60000 ||
      !g_xma_minute_start_ms.compare_exchange_strong(start, now, std::memory_order_relaxed)) {
    return;
  }
  auto take = [](std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
  };
  const uint64_t swaps = take(g_xma_counters.swaps);
  const uint64_t carry_nonzero = take(g_xma_counters.carry_nonzero);
  const uint64_t swaps_44k = take(g_xma_counters.swaps_44k);
  const uint64_t carry_nonzero_44k = take(g_xma_counters.carry_nonzero_44k);
  const uint64_t carry_fallbacks = take(g_xma_counters.carry_fallbacks);
  const uint64_t stale_ring_replays = take(g_xma_counters.stale_ring_replays);
  const uint64_t lock_races = take(g_xma_counters.lock_races);
  const uint64_t read_offset_reverts = take(g_xma_counters.read_offset_reverts);
  const uint64_t trace_suppressed = take(g_xma_counters.trace_suppressed);
  const bool any = swaps || carry_nonzero || carry_fallbacks || stale_ring_replays || lock_races ||
                   read_offset_reverts || trace_suppressed;
  if (!any && !REXCVAR_GET(xma_swap_trace)) {
    return;
  }
  REXAPU_INFO(
      "MCLA_XMA v=1 kind=minute swaps={} carry_nonzero={} stale_ring_replays={} lock_races={} "
      "swaps_44k={} carry_nonzero_44k={} carry_fallbacks={} read_offset_reverts={} "
      "trace_suppressed={} carry_fix={} valid_fix={} lock_fix={}",
      swaps, carry_nonzero, stale_ring_replays, lock_races, swaps_44k, carry_nonzero_44k,
      carry_fallbacks, read_offset_reverts, trace_suppressed,
      int(REXCVAR_GET(xma_swap_carry_packet)), int(REXCVAR_GET(xma_valid_only_when_written)),
      int(REXCVAR_GET(xma_work_lock_recheck)));
}

}  // namespace

// KI-071 step 3: one 16-bit PCM WAV file per context stream (xma_dump_contexts).
// Holds the blocks exactly as written to the guest output ring (post-trim),
// byte-swapped to little-endian. The header is fixed up about once per second
// of audio and on close, so a killed process still leaves a readable file.
class XmaPcmDump {
 public:
  XmaPcmDump(std::filesystem::path path, uint32_t rate, uint16_t channels)
      : path_(std::move(path)), rate_(rate), channels_(channels) {
    max_bytes_ = xma::kDumpMaxSeconds * rate * channels * 2u;
    fix_every_ = rate * channels * 2u;
    file_.open(path_, std::ios::binary | std::ios::trunc);
    if (file_) {
      const auto header = xma::MakeWavHeader(rate_, channels_, 0);
      file_.write(reinterpret_cast<const char*>(header.data()), header.size());
    }
  }
  ~XmaPcmDump() { Close(); }

  bool ok() const { return bool(file_); }
  const std::filesystem::path& path() const { return path_; }
  uint32_t data_bytes() const { return data_bytes_; }
  double seconds() const { return double(data_bytes_) / double(rate_ * channels_ * 2u); }

  // Returns false once the 60 s cap is reached.
  bool Write(const uint8_t* big_endian, size_t bytes) {
    if (!file_) {
      return false;
    }
    const size_t room = max_bytes_ - data_bytes_;
    const size_t n = std::min(bytes, room) & ~size_t(1);
    std::array<uint8_t, 2048> le;
    for (size_t done = 0; done < n;) {
      const size_t chunk = std::min(n - done, le.size());
      xma::SwapPcm16(big_endian + done, le.data(), chunk);
      file_.write(reinterpret_cast<const char*>(le.data()), std::streamsize(chunk));
      done += chunk;
    }
    data_bytes_ += uint32_t(n);
    if (data_bytes_ - last_fix_ >= fix_every_) {
      FixHeader();
    }
    return data_bytes_ < max_bytes_;
  }

  void Close() {
    if (!file_) {
      return;
    }
    FixHeader();
    file_.close();
  }

 private:
  void FixHeader() {
    const auto header = xma::MakeWavHeader(rate_, channels_, data_bytes_);
    const auto end = file_.tellp();
    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(header.data()), header.size());
    file_.seekp(end);
    file_.flush();
    last_fix_ = data_bytes_;
  }

  std::filesystem::path path_;
  uint32_t rate_;
  uint16_t channels_;
  uint32_t max_bytes_ = 0;
  uint32_t fix_every_ = 0;
  uint32_t data_bytes_ = 0;
  uint32_t last_fix_ = 0;
  std::ofstream file_;
};

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  // dump_ (if any) fixes its WAV header in its destructor; no logging here.
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }
  MaybeEmitXmaMinute();
  const uint32_t disables_before_lock = disable_count_.load(std::memory_order_acquire);

  std::lock_guard<std::mutex> lock(lock_);
  // KI-071 C3: the enable check above ran without the lock. A guest lock
  // (Disable + Block) or another Work may have run in between.
  const bool enabled_after_lock = is_enabled();
  if (!enabled_after_lock) {
    if (disable_count_.load(std::memory_order_acquire) != disables_before_lock) {
      g_xma_counters.lock_races.fetch_add(1, std::memory_order_relaxed);
    }
    if (!xma::ShouldRunWorkAfterLock(enabled_after_lock, REXCVAR_GET(xma_work_lock_recheck))) {
      return false;
    }
  }
  set_is_enabled(false);
  blocks_written_this_work_ = 0;
  stalled_this_work_ = false;

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);
  {
    // KI-071 trace: blocks the guest has read = written - still in the ring.
    const int64_t in_ring = int64_t(data.output_buffer_block_count) -
                            int64_t(remaining_subframe_blocks_in_output_buffer_);
    const uint64_t written = sib_blocks_written_.load(std::memory_order_relaxed);
    const uint64_t buffered = uint64_t(std::max<int64_t>(0, in_ring));
    sib_blocks_consumed_.store(written > buffered ? written - buffered : 0,
                               std::memory_order_relaxed);
  }

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    Consume(&output_rb, &data);
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    Decode(&data);
    // KI-071 step 4: where this Decode left the walk (guest seek detection).
    walk_left_read_offset_ = data.input_buffer_read_offset;
    walk_left_buffer_ = data.current_buffer;
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
      break;
    }
    // KI-071 D2: stalled at a boundary until the guest validates the next buffer.
    if (stalled_this_work_) {
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  // KI-071 C2: read == write is "full" only when this Work's writes closed the
  // ring; upstream also cleared valid on a ring the guest had drained, which
  // the guest reads as "whole ring readable" and replays.
  const bool ring_read_equals_write = output_rb.empty();
  if (xma::IsStaleRingReplay(initial_data.output_buffer_valid, ring_read_equals_write,
                             blocks_written_this_work_)) {
    g_xma_counters.stale_ring_replays.fetch_add(1, std::memory_order_relaxed);
  }
  if (xma::ShouldClearOutputValid(ring_read_equals_write, blocks_written_this_work_,
                                  REXCVAR_GET(xma_valid_only_when_written))) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

void XmaContext::ResetDecoderState() {
  // A freed or re-initialized context is a new logical stream. Drop the previous
  // wave's MDCT overlap-add tail (persistent av_context_) and any buffered
  // subframes so frame 0 of the next same-format wave is not contaminated.
  if (av_context_ && avcodec_is_open(av_context_)) {
    avcodec_flush_buffers(av_context_);
  }
  // KI-071: the xmaframes codec has no .flush callback (wmaprodec.c
  // ff_xmaframes_decoder), so the flush above keeps the IMDCT overlap of the
  // previous wave. Re-create the context; PrepareDecoder re-opens it at the
  // next decode (rate/channels 0 never match).
  if (REXCVAR_GET(xma_reset_codec_on_clear) && av_context_ && avcodec_is_open(av_context_)) {
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);
    if (av_context_) {
      av_context_->channels = 0;
      av_context_->sample_rate = 0;
    } else {
      REXAPU_ERROR("XmaContext {}: Couldn't re-allocate codec context", id());
    }
    Bump(g_xma_sib.codec_resets);
  }
  raw_frame_.fill(0);
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
  pending_swap_carry_packet_ = 0;
  stalled_this_work_ = false;
  consecutive_stalls_ = 0;
  start_pending_ = true;
  trim_carry_.Reset();
  sib_frame_index_ = 0;
  if ((walk_dup_ || walk_gap_) && REXCVAR_GET(xma_frame_walk_trace) && g_xma_walk_budget.Allow()) {
    REXAPU_INFO("[xma] walk-summary: ctx={} frames={} dup={} gap={} fix={}", id(), walk_frames_,
                walk_dup_, walk_gap_, int(REXCVAR_GET(xma_frame_walk_fix)));
  }
  walk_prev_ = {};
  walk_after_swap_ = false;
  walk_after_ff_ = false;
  walk_frames_ = 0;
  walk_dup_ = 0;
  walk_gap_ = 0;
  walk_logged_ = 0;
  CloseDumpLocked("clear");
  input_pending_.clear();
  input_pending_.shrink_to_fit();
  input_idx_pending_.clear();
  walk_csv_pending_.clear();
  input_bytes_ = 0;
  input_dumped_epoch_ = UINT32_MAX;
  input_seq_ = 0;
  input_blocked_ = false;
  dump_blocked_ = false;
  dump_pending_.clear();
  if (id_ < g_xma_dump_serial.size()) {
    g_xma_dump_serial[id_].store(0, std::memory_order_relaxed);
  }
  ResetSiblingCounters();
}

void XmaContext::ResetSiblingCounters() {
  for (auto* counter : {&sib_frames_parsed_, &sib_frames_emitted_, &sib_blocks_written_,
                        &sib_blocks_consumed_, &sib_blocks_skipped_, &sib_drops_split_,
                        &sib_drops_ff_, &sib_err4_, &sib_decode_fail_, &sib_trim_start_frames_,
                        &sib_trim_start_samples_, &sib_trim_end_frames_, &sib_trim_end_samples_}) {
    counter->store(0, std::memory_order_relaxed);
  }
  sib_start_skip_.store(0, std::memory_order_relaxed);
  sib_start_kick_.store(0, std::memory_order_relaxed);
  sib_rate_.store(0, std::memory_order_relaxed);
  sib_stereo_.store(false, std::memory_order_relaxed);
  sib_trim_first_count_.store(0, std::memory_order_relaxed);
  for (auto& pair : sib_trim_first_) {
    pair.store(0, std::memory_order_relaxed);
  }
}

void XmaContext::OnFrameTrim(const xma::FrameTrimInfo& trim, const XMA_CONTEXT_DATA* data) {
  const uint64_t frame = sib_frame_index_++;
  if (!trim.ok) {
    Bump(g_xma_sib.trim_parse_errors);
    return;
  }
  if (trim.start == 0 && trim.end == 0) {
    return;
  }
  if (trim.start) {
    Bump(sib_trim_start_frames_);
    Bump(sib_trim_start_samples_, trim.start);
  }
  if (trim.end) {
    Bump(sib_trim_end_frames_);
    Bump(sib_trim_end_samples_, trim.end);
  }
  Bump(g_xma_sib.trim_frames);
  Bump(g_xma_sib.trim_samples, uint64_t(trim.start) + trim.end);
  const uint32_t epoch = g_xma_trim_epoch.load(std::memory_order_relaxed);
  if (sib_trim_epoch_.exchange(epoch, std::memory_order_relaxed) != epoch) {
    Bump(g_xma_sib.trim_ctxs);
  }
  const uint32_t index = sib_trim_first_count_.load(std::memory_order_relaxed);
  if (index < sib_trim_first_.size()) {
    sib_trim_first_[index].store((trim.start << 16) | trim.end, std::memory_order_relaxed);
    sib_trim_first_count_.store(index + 1, std::memory_order_relaxed);
    if (REXCVAR_GET(xma_sibling_trace) && g_xma_trim_budget.Allow()) {
      REXAPU_INFO(
          "[xma] frame-trim: ctx={} rate={} stereo={} frame={} start={} end={} start_flag={} "
          "end_flag={} header_bits={} read_offset={} kick_group={} trim_fix={}",
          id(), GetSampleRate(data->sample_rate), uint32_t(data->is_stereo), frame, trim.start,
          trim.end, int(trim.has_start), int(trim.has_end), trim.header_bits,
          uint32_t(data->input_buffer_read_offset), sib_kick_group_.load(std::memory_order_relaxed),
          int(REXCVAR_GET(xma_honor_frame_trim)));
    }
  }
}

void XmaContext::DumpBlocks(const uint8_t* blocks, size_t bytes, const XMA_CONTEXT_DATA* data) {
  if (dump_blocked_ || !bytes) {
    return;
  }
  if (!dump_) {
    const uint32_t rate = uint32_t(GetSampleRate(data->sample_rate));
    if (rate != 44100 && !REXCVAR_GET(xma_dump_all_rates)) {
      dump_blocked_ = true;
      return;
    }
    const uint32_t serial =
        id_ < g_xma_dump_serial.size() ? g_xma_dump_serial[id_].load(std::memory_order_relaxed) : 0;
    if (REXCVAR_GET(xma_dump_burst_only) && !serial) {
      // Hold the first blocks until the burst is known; give up after
      // kDumpPendingUs (not a burst start).
      if (XmaSteadyNowUs() - dump_start_us_ > xma::kDumpPendingUs ||
          dump_pending_.size() + bytes > xma::kDumpPendingMaxBytes) {
        dump_pending_.clear();
        dump_pending_.shrink_to_fit();
        dump_blocked_ = true;
        return;
      }
      dump_pending_.insert(dump_pending_.end(), blocks, blocks + bytes);
      return;
    }
    const uint32_t n = g_xma_dump_files.fetch_add(1, std::memory_order_relaxed);
    if (n >= xma::kDumpMaxFiles) {
      dump_blocked_ = true;
      if (!g_xma_dump_limit_logged.exchange(true, std::memory_order_relaxed)) {
        REXAPU_INFO("[xma] dump: file limit {} reached; no further dumps this session",
                    xma::kDumpMaxFiles);
      }
      return;
    }
    const std::filesystem::path dir(REXCVAR_GET(xma_dump_contexts));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const bool stereo = bool(data->is_stereo);
    dump_ = std::make_unique<XmaPcmDump>(dir / xma::DumpFileName(id(), serial, rate, stereo, n),
                                         rate, uint16_t(stereo ? 2 : 1));
    if (!dump_->ok()) {
      REXAPU_WARN("[xma] dump: cannot open {} ({})", dump_->path().string(), ec.message());
      dump_.reset();
      dump_blocked_ = true;
      return;
    }
    REXAPU_INFO(
        "[xma] dump: open ctx={} group={} rate={} stereo={} n={} start_kick={} held_bytes={} "
        "file={}",
        id(), serial, rate, uint32_t(stereo), n, sib_start_kick_.load(std::memory_order_relaxed),
        dump_pending_.size(), dump_->path().string());
    if (REXCVAR_GET(xma_dump_input) && !input_blocked_) {
      OpenInputDumpLocked(dump_->path());
    }
    if (!dump_pending_.empty()) {
      const bool more = dump_->Write(dump_pending_.data(), dump_pending_.size());
      dump_pending_.clear();
      dump_pending_.shrink_to_fit();
      if (!more) {
        CloseDumpLocked("cap");
        dump_blocked_ = true;
        return;
      }
    }
  }
  if (!dump_->Write(blocks, bytes)) {
    CloseDumpLocked("cap");
    dump_blocked_ = true;
  }
}

void XmaContext::CloseDumpLocked(const char* reason) {
  if (!dump_) {
    return;
  }
  dump_->Close();
  REXAPU_INFO("[xma] dump: close ctx={} reason={} bytes={} seconds={:.2f} file={}", id(), reason,
              dump_->data_bytes(), dump_->seconds(), dump_->path().string());
  dump_.reset();
  CloseInputDumpLocked();
}

void XmaContext::OpenInputDumpLocked(const std::filesystem::path& wav_path) {
  std::filesystem::path base = wav_path;
  base.replace_extension();
  const std::string stem = base.string();
  input_bin_ =
      std::make_unique<std::ofstream>(stem + "-input.bin", std::ios::binary | std::ios::trunc);
  input_idx_ = std::make_unique<std::ofstream>(stem + "-input.csv", std::ios::trunc);
  walk_csv_ = std::make_unique<std::ofstream>(stem + "-walk.csv", std::ios::trunc);
  if (!*input_bin_ || !*input_idx_ || !*walk_csv_) {
    REXAPU_WARN("[xma] input dump: cannot open {}-input.bin/.csv/-walk.csv", stem);
    CloseInputDumpLocked();
    input_blocked_ = true;
    return;
  }
  Bump(g_xma_walk.input_files);
  *input_idx_ << "seq,buffer_epoch,buffer_index,buffer_ptr,packet_count,packet,file_offset,"
                 "first_frame_offset,header_frame_count,skip,read_offset\n";
  *walk_csv_ << "frame,buffer_epoch,buffer_ptr,packet,packet_addr,start_bit,frame_bits,"
                "bits_in_packet,path,verdict,prev_trailer,hdr_count,walked,frame_count,current,"
                "first_frame_offset,skip,split_tail_bits,split_counted,split_claimed,"
                "entry_read_offset,read_after,after_swap,after_ff,dup_prevented\n";
  input_bin_->write(reinterpret_cast<const char*>(input_pending_.data()),
                    std::streamsize(input_pending_.size()));
  *input_idx_ << input_idx_pending_;
  *walk_csv_ << walk_csv_pending_;
  REXAPU_INFO("[xma] input dump: open ctx={} held_bytes={} file={}-input.bin", id(),
              input_pending_.size(), stem);
  input_pending_.clear();
  input_pending_.shrink_to_fit();
  input_idx_pending_.clear();
  input_idx_pending_.shrink_to_fit();
  walk_csv_pending_.clear();
  walk_csv_pending_.shrink_to_fit();
}

void XmaContext::CloseInputDumpLocked() {
  if (!input_bin_) {
    return;
  }
  input_bin_->close();
  input_idx_->close();
  walk_csv_->close();
  REXAPU_INFO("[xma] input dump: close ctx={} bytes={}", id(), input_bytes_);
  input_bin_.reset();
  input_idx_.reset();
  walk_csv_.reset();
  input_blocked_ = true;
}

// Appends to the open dump files, or holds the data until the PCM dump opens.
// Stops once the stream is not (or no longer) dumped.
void XmaContext::WriteInputDump(const uint8_t* bytes, size_t size, const std::string& index_row,
                                const std::string& walk_row) {
  if (input_blocked_) {
    return;
  }
  if (input_bin_) {
    if (size && input_bytes_ + size <= kInputDumpMaxBytes) {
      input_bin_->write(reinterpret_cast<const char*>(bytes), std::streamsize(size));
      input_bytes_ += size;
      *input_idx_ << index_row;
    }
    *walk_csv_ << walk_row;
    return;
  }
  if (dump_blocked_ || input_pending_.size() + size > kInputPendingMaxBytes ||
      walk_csv_pending_.size() + walk_row.size() > kWalkCsvPendingMaxBytes) {
    // Not a dumped stream (rate, burst rule, file cap) or held too long.
    input_pending_.clear();
    input_pending_.shrink_to_fit();
    input_idx_pending_.clear();
    input_idx_pending_.shrink_to_fit();
    walk_csv_pending_.clear();
    walk_csv_pending_.shrink_to_fit();
    input_blocked_ = true;
    return;
  }
  if (size) {
    input_pending_.insert(input_pending_.end(), bytes, bytes + size);
    input_bytes_ += size;
  }
  input_idx_pending_ += index_row;
  walk_csv_pending_ += walk_row;
}

void XmaContext::DumpInputBuffer(XMA_CONTEXT_DATA* data) {
  if (input_blocked_ || input_dumped_epoch_ == walk_buffer_epoch_) {
    return;
  }
  input_dumped_epoch_ = walk_buffer_epoch_;
  const uint32_t rate = uint32_t(GetSampleRate(data->sample_rate));
  if (rate != 44100 && !REXCVAR_GET(xma_dump_all_rates)) {
    input_blocked_ = true;
    return;
  }
  const uint32_t ptr = data->GetCurrentInputBufferAddress();
  const uint32_t packets = data->GetCurrentInputBufferPacketCount();
  const uint8_t* buffer = memory()->TranslatePhysical(ptr);
  std::string rows;
  for (uint32_t i = 0; i < packets; ++i) {
    const uint8_t* packet = buffer + size_t(i) * kBytesPerPacket;
    rows += fmt::format("{},{},{},{:08X},{},{},{},{},{},{},{}\n", input_seq_, walk_buffer_epoch_,
                        uint32_t(data->current_buffer), ptr, packets, i,
                        input_bytes_ + uint64_t(i) * kBytesPerPacket,
                        xma::WalkFirstFrameOffset(packet), xma::WalkHeaderFrameCount(packet),
                        xma::WalkSkipCount(packet), uint32_t(data->input_buffer_read_offset));
  }
  ++input_seq_;
  WriteInputDump(buffer, size_t(packets) * kBytesPerPacket, rows, std::string());
}

void XmaContext::FinalizeDump() {
  std::lock_guard<std::mutex> lock(lock_);
  CloseDumpLocked("exit");
  dump_blocked_ = true;
}

xma::SiblingCtxSample XmaContext::SiblingSnapshot() const {
  xma::SiblingCtxSample s;
  s.ctx = id_;
  s.frames_parsed = sib_frames_parsed_.load(std::memory_order_relaxed);
  s.frames_emitted = sib_frames_emitted_.load(std::memory_order_relaxed);
  s.blocks_written = sib_blocks_written_.load(std::memory_order_relaxed);
  s.blocks_consumed = sib_blocks_consumed_.load(std::memory_order_relaxed);
  s.blocks_skipped = sib_blocks_skipped_.load(std::memory_order_relaxed);
  s.drops_split = sib_drops_split_.load(std::memory_order_relaxed);
  s.drops_ff = sib_drops_ff_.load(std::memory_order_relaxed);
  s.err4 = sib_err4_.load(std::memory_order_relaxed);
  s.decode_fail = sib_decode_fail_.load(std::memory_order_relaxed);
  s.start_skip = sib_start_skip_.load(std::memory_order_relaxed);
  s.kick_group = sib_kick_group_.load(std::memory_order_relaxed);
  s.start_kick = sib_start_kick_.load(std::memory_order_relaxed);
  s.rate = sib_rate_.load(std::memory_order_relaxed);
  s.stereo = sib_stereo_.load(std::memory_order_relaxed);
  s.trim_start_frames = sib_trim_start_frames_.load(std::memory_order_relaxed);
  s.trim_start_samples = sib_trim_start_samples_.load(std::memory_order_relaxed);
  s.trim_end_frames = sib_trim_end_frames_.load(std::memory_order_relaxed);
  s.trim_end_samples = sib_trim_end_samples_.load(std::memory_order_relaxed);
  s.trim_first_count = std::min<uint32_t>(sib_trim_first_count_.load(std::memory_order_relaxed), 4);
  for (size_t i = 0; i < s.trim_first.size(); ++i) {
    s.trim_first[i] = sib_trim_first_[i].load(std::memory_order_relaxed);
  }
  return s;
}

xma::SiblingMinuteTotals XmaContext::TakeSiblingMinuteTotals() {
  auto take = [](std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
  };
  xma::SiblingMinuteTotals t;
  t.drops_split = take(g_xma_sib.drops_split);
  t.drops_ff = take(g_xma_sib.drops_ff);
  t.err4 = take(g_xma_sib.err4);
  t.decode_fail = take(g_xma_sib.decode_fail);
  t.stalls = take(g_xma_sib.stalls);
  t.stall_timeouts = take(g_xma_sib.stall_timeouts);
  t.err4_cleared = take(g_xma_sib.err4_cleared);
  t.swapbacks = take(g_xma_sib.swapbacks);
  t.no_bits = take(g_xma_sib.no_bits);
  t.start_skips = take(g_xma_sib.start_skips);
  t.start_skips_applied = take(g_xma_sib.start_skips_applied);
  t.codec_resets = take(g_xma_sib.codec_resets);
  t.first_decodes = take(g_xma_sib.first_decodes);
  t.trim_frames = take(g_xma_sib.trim_frames);
  t.trim_samples = take(g_xma_sib.trim_samples);
  t.trim_ctxs = take(g_xma_sib.trim_ctxs);
  t.trim_applied_samples = take(g_xma_sib.trim_applied_samples);
  t.trim_unapplied = take(g_xma_sib.trim_unapplied);
  t.trim_parse_errors = take(g_xma_sib.trim_parse_errors);
  t.burst_first_decodes = take(g_xma_sib.burst_first_decodes);
  g_xma_trim_epoch.fetch_add(1, std::memory_order_relaxed);
  return t;
}

xma::WalkMinuteTotals XmaContext::TakeWalkMinuteTotals() {
  auto take = [](std::atomic<uint64_t>& counter) {
    return counter.exchange(0, std::memory_order_relaxed);
  };
  xma::WalkMinuteTotals w;
  w.dup = take(g_xma_walk.dup);
  w.gap = take(g_xma_walk.gap);
  w.ctxs = take(g_xma_walk.ctxs);
  w.boundary = take(g_xma_walk.boundary);
  w.split_tail = take(g_xma_walk.split_tail);
  w.split_counted = take(g_xma_walk.split_counted);
  w.split_lost = take(g_xma_walk.split_lost);
  w.split_real = take(g_xma_walk.split_real);
  w.split_claimed = take(g_xma_walk.split_claimed);
  w.split_padding = take(g_xma_walk.split_padding);
  w.dup_prevented = take(g_xma_walk.dup_prevented);
  w.resyncs = take(g_xma_walk.resyncs);
  w.input_files = take(g_xma_walk.input_files);
  g_xma_walk_epoch.fetch_add(1, std::memory_order_relaxed);
  return w;
}

xma::BoundaryAction XmaContext::OnBoundary(xma::BoundaryPath path, bool next_available) {
  const bool fix = REXCVAR_GET(xma_stall_on_invalid_next);
  const xma::BoundaryAction action =
      xma::DecideBoundary(path, next_available, fix, consecutive_stalls_);
  if (action == xma::BoundaryAction::kStall) {
    ++consecutive_stalls_;
    stalled_this_work_ = true;
    Bump(g_xma_sib.stalls);
    return action;
  }
  if (!next_available) {
    if (fix) {
      Bump(g_xma_sib.stall_timeouts);
    }
    switch (path) {
      case xma::BoundaryPath::kSplitHeader:
        Bump(sib_drops_split_);
        Bump(g_xma_sib.drops_split);
        break;
      case xma::BoundaryPath::kFullSkip:
        Bump(sib_drops_ff_);
        Bump(g_xma_sib.drops_ff);
        break;
      case xma::BoundaryPath::kSpanningFrame:
        Bump(sib_err4_);
        Bump(g_xma_sib.err4);
        break;
    }
  }
  consecutive_stalls_ = 0;
  return action;
}

void XmaContext::LogFirstDecode(const XMA_CONTEXT_DATA* data, uint32_t entry_read_offset,
                                int codec_reinit, const xma::FrameTrimInfo& trim) {
  Bump(g_xma_sib.first_decodes);
  if (!REXCVAR_GET(xma_sibling_trace)) {
    return;
  }
  const int rate = GetSampleRate(data->sample_rate);
  const uint32_t read_offset = data->input_buffer_read_offset;
  const uint8_t* packet = memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
                          (read_offset / kBitsPerPacket) * kBytesPerPacket;
  std::string body = fmt::format(
      "ctx={} rate={} stereo={} entry_read_offset={} read_offset={} "
      "packet={} first_frame_offset={} start_skip={} sdc={} codec_reinit={} buffer={:08X} "
      "packets={} current_buffer={} kick_group={} skip_fix={} trim={}:{} trim_ok={}",
      id(), rate, uint32_t(data->is_stereo), entry_read_offset, read_offset,
      read_offset / kBitsPerPacket, xma::GetPacketFrameOffset(packet),
      uint32_t(data->output_buffer_padding), uint32_t(data->subframe_decode_count), codec_reinit,
      data->GetCurrentInputBufferAddress(), data->GetCurrentInputBufferPacketCount(),
      uint32_t(data->current_buffer), sib_kick_group_.load(std::memory_order_relaxed),
      int(REXCVAR_GET(xma_honor_start_skip)), trim.start, trim.end, int(trim.ok));
  if (rate != 44100) {
    if (AllowXmaTraceLine()) {
      REXAPU_INFO("[xma] first-decode: {} budget=shared", body);
    }
    return;
  }
  // KI-071 step 3: a 44.1 kHz start within 50 ms of 3+ other 44.1 kHz starts
  // (a multi-context voice) is logged on its own budget, with the records of
  // the same window that the shared budget suppressed.
  const int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
  std::vector<std::string> flush;
  bool burst = false;
  bool logged = false;
  {
    std::lock_guard<std::mutex> guard(g_xma_start_window_mutex);
    burst = g_xma_start_window.IsBurst(now_us, id(), &flush);
    if (!burst) {
      logged = AllowXmaTraceLine();
    }
    g_xma_start_window.Record(now_us, id(), burst || logged, logged ? std::string() : body);
  }
  for (const std::string& line : flush) {
    if (g_xma_burst_budget.Allow()) {
      Bump(g_xma_sib.burst_first_decodes);
      REXAPU_INFO("[xma] first-decode: {} budget=burst-late", line);
    }
  }
  if (burst) {
    if (g_xma_burst_budget.Allow()) {
      Bump(g_xma_sib.burst_first_decodes);
      REXAPU_INFO("[xma] first-decode: {} budget=burst", body);
    }
  } else if (logged) {
    REXAPU_INFO("[xma] first-decode: {} budget=shared", body);
  }
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  disable_count_.fetch_add(1, std::memory_order_acq_rel);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  ResetDecoderState();
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  ++walk_buffer_epoch_;
  walk_after_swap_ = true;
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;
  // KI-071 step 4: a loop jump starts a new frame chain.
  walk_prev_.valid = false;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  if (next_packet_index < current_input_packet_count) {
    return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
           next_packet_index * kBytesPerPacket;
  }

  const uint8_t next_buffer_index = data->current_buffer ^ 1;
  if (!data->IsInputBufferValid(next_buffer_index)) {
    return nullptr;
  }

  const uint32_t next_buffer_address = data->GetInputBufferAddress(next_buffer_index);
  if (!next_buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return nullptr;
  }

  const uint8_t* next_buffer = memory()->TranslatePhysical(next_buffer_address);
  if (REXCVAR_GET(xma_swap_carry_packet)) {
    // KI-071 C1: a frame that spans the buffer boundary continues in this
    // stream's packet of the next buffer, not in packet 0 (stream 0's).
    const xma::SwapCarry carry =
        xma::ComputeSwapCarry(next_packet_index, current_input_packet_count,
                              data->GetInputBufferPacketCount(next_buffer_index));
    return next_buffer + carry.start_packet * kBytesPerPacket;
  }
  return next_buffer;
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  // KI-071 step 4: the same loop, shared with the frame-walk tests.
  return xma::FindNextPacketReadOffset(buffer, next_packet_index, current_input_packet_count);
}

uint32_t XmaContext::NextPacketGuestAddress(const XMA_CONTEXT_DATA* data,
                                            uint32_t next_packet_index,
                                            uint32_t current_input_packet_count) {
  if (next_packet_index < current_input_packet_count) {
    return data->GetCurrentInputBufferAddress() + next_packet_index * kBytesPerPacket;
  }
  const uint8_t next_buffer_index = data->current_buffer ^ 1;
  const uint32_t next_buffer_address = data->GetInputBufferAddress(next_buffer_index);
  if (REXCVAR_GET(xma_swap_carry_packet)) {
    const xma::SwapCarry carry =
        xma::ComputeSwapCarry(next_packet_index, current_input_packet_count,
                              data->GetInputBufferPacketCount(next_buffer_index));
    return next_buffer_address + carry.start_packet * kBytesPerPacket;
  }
  return next_buffer_address;
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  // KI-071 C3: the guest owns output_buffer_read_offset; Work never changes it.
  if (fresh.output_buffer_read_offset != data.output_buffer_read_offset) {
    g_xma_counters.read_offset_reverts.fetch_add(1, std::memory_order_relaxed);
  }
  fresh.output_buffer_read_offset =
      xma::MergeOutputReadOffset(fresh.output_buffer_read_offset, data.output_buffer_read_offset,
                                 REXCVAR_GET(xma_work_lock_recheck));

  fresh.Store(context_ptr);
}

void XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  output_rb->Write(raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
                   subframes_to_write * kOutputBytesPerBlock);
  if (subframes_to_write > 0) {
    blocks_written_this_work_ += uint32_t(subframes_to_write);
    Bump(sib_blocks_written_, uint64_t(subframes_to_write));
    // KI-071 step 3: the dump holds exactly the bytes the guest ring got.
    if (!REXCVAR_GET(xma_dump_contexts).empty()) {
      DumpBlocks(raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
                 size_t(subframes_to_write) * kOutputBytesPerBlock, data);
    }
  }

  const int8_t headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                              ? data->output_buffer_padding
                              : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (!av_context_ || av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);
    if (!av_context_) {
      REXAPU_ERROR("XmaContext {}: Couldn't allocate codec context", id());
      return -1;
    }

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  // KI-071 step 4: the guest moved the read offset (or buffer) since the last
  // Decode: a seek, not a walk step.
  if (walk_prev_.valid && (uint32_t(data->input_buffer_read_offset) != walk_left_read_offset_ ||
                           uint32_t(data->current_buffer) != walk_left_buffer_)) {
    walk_prev_.valid = false;
    Bump(g_xma_walk.resyncs);
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  // KI-071 C1: the end-of-buffer swap found no valid next buffer and recorded
  // the carry; continue at that packet once decoding resumes at the start.
  if (pending_swap_carry_packet_) {
    const uint32_t pending = pending_swap_carry_packet_;
    pending_swap_carry_packet_ = 0;
    if (REXCVAR_GET(xma_swap_carry_packet) &&
        data->input_buffer_read_offset == kBitsPerPacketHeader) {
      const uint32_t packet_count = data->GetCurrentInputBufferPacketCount();
      const uint32_t frame_offset =
          pending < packet_count
              ? xma::GetPacketFrameOffset(GetCurrentInputBuffer(data) + pending * kBytesPerPacket)
              : kMaxFrameSizeinBits + 1;
      if (frame_offset <= kMaxFrameSizeinBits) {
        data->input_buffer_read_offset = pending * kBitsPerPacket + frame_offset;
      } else {
        g_xma_counters.carry_fallbacks.fetch_add(1, std::memory_order_relaxed);
        if (AllowXmaTraceLine()) {
          REXAPU_WARN("[xma] swap carry {} out of range (packets={}) on ctx={}; packet 0", pending,
                      packet_count, id());
        }
      }
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // Loop-end frame: decode it here (output limited to loop_subframe_end),
  // jump to loop_start afterwards in the next-offset step.
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  const uint32_t entry_read_offset = data->input_buffer_read_offset;
  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  // KI-071 step 4: frame continuity. The start of this frame is compared with
  // the end of the previous one (walk_prev_).
  const bool walk_fix = REXCVAR_GET(xma_frame_walk_fix);
  const uint32_t packet_addr =
      data->GetCurrentInputBufferAddress() + packet_index * kBytesPerPacket;
  bool dup_prevented = false;
  if (walk_fix && walk_prev_.valid && walk_prev_.packet_addr == packet_addr &&
      relative_offset < walk_prev_.end_bit && walk_prev_.trailer &&
      walk_prev_.end_bit < kBitsPerPacket) {
    // Never decode bits of this packet before the previous frame's end again.
    data->input_buffer_read_offset = packet_index * kBitsPerPacket + walk_prev_.end_bit;
    relative_offset = walk_prev_.end_bit;
    dup_prevented = true;
    Bump(g_xma_walk.dup_prevented);
  }
  if (REXCVAR_GET(xma_dump_input) && !REXCVAR_GET(xma_dump_contexts).empty()) {
    DumpInputBuffer(data);
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    walk_after_ff_ = true;
    uint32_t next_input_offset =
        GetNextPacketReadOffset(current_input_buffer, packet_index + 1, current_input_packet_count);
    if (next_input_offset == kBitsPerPacketHeader) {
      // KI-071 D2: no later frame start in this buffer and the next buffer
      // not yet valid: upstream swaps now; the fix waits for the refill.
      if (!data->IsInputBufferValid(data->current_buffer ^ 1) &&
          OnBoundary(xma::BoundaryPath::kFullSkip, false) == xma::BoundaryAction::kStall) {
        return;
      }
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;
  // Upstream GetPacketInfo plus the split-tail record (frame_walk.h).
  xma::PacketFrames frames = xma::AnalyzePacketFrames(packet, relative_offset);
  // A frame header straddling the packet end that upstream does not count:
  // check the next packet's continuation (stats always, claim with the fix).
  xma::SplitTailCheck split_check = xma::SplitTailCheck::kNotReal;
  if (frames.split_tail_bits) {
    const uint8_t* tail_next = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (tail_next) {
      split_check = xma::CheckSplitTail(packet, tail_next, frames.split_tail_pos);
    }
    if (walk_fix && !frames.split_tail_counted &&
        (!tail_next || split_check != xma::SplitTailCheck::kNotReal)) {
      xma::ClaimSplitTail(&frames, relative_offset);
    }
  }
  // xma_frame_walk_fix: a tail claimed earlier (next packet not yet valid)
  // that the next packet now shows to be padding: continue as upstream does
  // after the packet's last frame instead of parsing the padding.
  if (walk_fix && frames.split_tail_bits && relative_offset == frames.split_tail_pos &&
      !frames.split_tail_counted && !frames.split_tail_claimed) {
    Bump(g_xma_walk.split_padding);
    AdvanceAfterLastFrame(data, uint32_t(packet_index), next_packet_index, current_input_buffer,
                          current_input_packet_count);
    return;
  }
  kPacketInfo packet_info = {};
  packet_info.frame_count_ = uint8_t(frames.frame_count);
  packet_info.current_frame_ = uint8_t(frames.current_frame);
  packet_info.current_frame_size_ = frames.current_frame_size;
  const bool split_header_frame = packet_info.current_frame_size_ == 0;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // KI-071 D2: upstream swaps and resumes at packet 0's first frame of the
      // next buffer, losing this straddling frame; the fix keeps the offset.
      if (OnBoundary(xma::BoundaryPath::kSplitHeader, false) == xma::BoundaryAction::kStall) {
        return;
      }
      Bump(sib_frames_parsed_);
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      Bump(sib_err4_);
      Bump(g_xma_sib.err4);
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    Bump(g_xma_sib.no_bits);
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        // KI-071 D2: the fix stalls without the sticky error_status 4.
        if (OnBoundary(xma::BoundaryPath::kSpanningFrame, false) == xma::BoundaryAction::kStall) {
          return;
        }
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  // KI-071 step 4: continuity of this frame against the previous one, then
  // record where this frame ends (its last bit is its trailer).
  WalkFrameInfo walk;
  {
    const uint32_t frame_size = packet_info.current_frame_size_;
    walk.frame_index = walk_frames_++;
    walk.packet_index = uint32_t(packet_index);
    walk.packet_addr = packet_addr;
    walk.start_bit = relative_offset;
    walk.frame_size = frame_size;
    walk.bits_in_packet = uint32_t(bits_to_copy);
    walk.entry_read_offset = entry_read_offset;
    walk.first_frame_offset = packet_first_frame_offset;
    walk.skip = skip_count;
    walk.info = frames;
    walk.prev = walk_prev_;
    walk.after_swap = walk_after_swap_;
    walk.after_ff = walk_after_ff_;
    walk.dup_prevented = dup_prevented;
    const bool spans = bits_to_copy < frame_size;
    walk.path = split_header_frame                  ? xma::WalkPath::kSplitHeader
                : spans                             ? xma::WalkPath::kSpanning
                : packet_info.isLastFrameInPacket() ? xma::WalkPath::kLastInPacket
                                                    : xma::WalkPath::kInPacket;
    walk.verdict = xma::ClassifyFrameStart(walk_prev_, packet_addr, relative_offset,
                                           packet_first_frame_offset);
    walk_after_swap_ = false;
    walk_after_ff_ = false;

    xma::WalkFrameEnd end;
    end.valid = true;
    if (spans) {
      end.packet_addr = NextPacketGuestAddress(data, next_packet_index, current_input_packet_count);
      end.end_bit = kBitsPerPacketHeader + (frame_size - uint32_t(bits_to_copy));
    } else {
      end.packet_addr = packet_addr;
      end.end_bit = relative_offset + frame_size;
    }
    end.trailer = xma::ReadBitsMsb(xma_frame_.data() + 1, (xma_frame_.size() - 1) * 8,
                                   padding_start + frame_size - 1, 1) != 0;
    if (!spans) {
      end.next_is_marker = xma::LengthMarkerAt(packet, end.end_bit);
    } else if (const uint8_t* end_packet =
                   GetNextPacket(data, next_packet_index, current_input_packet_count)) {
      end.next_is_marker = xma::LengthMarkerAt(end_packet, end.end_bit);
    }
    walk_prev_ = end;

    const uint32_t minute_epoch = g_xma_walk_epoch.load(std::memory_order_relaxed);
    if (walk.verdict == xma::WalkVerdict::kDup || walk.verdict == xma::WalkVerdict::kGap) {
      if (walk.verdict == xma::WalkVerdict::kDup) {
        ++walk_dup_;
        Bump(g_xma_walk.dup);
      } else {
        ++walk_gap_;
        Bump(g_xma_walk.gap);
      }
      if (walk_minute_epoch_ != minute_epoch) {
        walk_minute_epoch_ = minute_epoch;
        Bump(g_xma_walk.ctxs);
      }
    }
    if (spans || split_header_frame) {
      Bump(g_xma_walk.boundary);
    }
    // This frame ends where a straddling header starts with trailer 1 (P of
    // the split-tail case): will the walk decode the next frame?
    if (!spans && frames.split_tail_bits && end.end_bit == frames.split_tail_pos && end.trailer) {
      Bump(g_xma_walk.split_tail);
      if (frames.split_tail_counted) {
        Bump(g_xma_walk.split_counted);
      } else if (frames.split_tail_claimed) {
        Bump(g_xma_walk.split_claimed);
      } else {
        Bump(g_xma_walk.split_lost);
      }
      if (split_check != xma::SplitTailCheck::kNotReal) {
        Bump(g_xma_walk.split_real);
      }
    }
  }

  raw_frame_.fill(0);

  const int codec_reinit = PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  PreparePacket(packet_info.current_frame_size_, padding_start);
  // KI-071 step 3: the frame header's start/end skip, read-only from the bits
  // handed to FFmpeg (which parses and discards them).
  const uint32_t channels = data->is_stereo ? 2u : 1u;
  const xma::FrameTrimInfo trim =
      xma::ParseFrameTrim(xma_frame_.data() + 1, xma_frame_.size() - 1, padding_start,
                          packet_info.current_frame_size_, channels);
  OnFrameTrim(trim, data);
  Bump(sib_frames_parsed_);
  const bool decoded = DecodePacket(av_context_, av_packet_, av_frame_);
  if (!decoded) {
    // The read offset still advances below: this frame is lost.
    Bump(sib_decode_fail_);
    Bump(g_xma_sib.decode_fail);
  }
  if (decoded) {
    Bump(sib_frames_emitted_);
    consecutive_stalls_ = 0;
    // KI-071 D2: error_status 4 is set only by this decoder and upstream
    // never clears it; with the fix a successful decode clears it.
    const uint32_t error_after = xma::ErrorStatusAfterDecode(
        data->error_status, true, REXCVAR_GET(xma_stall_on_invalid_next));
    if (error_after != data->error_status) {
      data->error_status = error_after;
      Bump(g_xma_sib.err4_cleared);
    }
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 raw_frame_.data());
    RecordAudioRouteXmaFrame(raw_frame_.data(), raw_frame_.size());
    current_frame_remaining_subframes_ = 4 << data->is_stereo;

    // KI-071 step 3: honour the frame trim. The kept blocks end up at the end
    // of raw_frame_, which is where Consume reads the remaining blocks from.
    if (REXCVAR_GET(xma_honor_frame_trim)) {
      const xma::FrameTrimApplied applied = xma::ApplyFrameTrim(
          std::span<uint8_t>(raw_frame_.data(), kBytesPerFrameChannel * channels), channels,
          trim.ok ? trim.start : 0, trim.ok ? trim.end : 0,
          REXCVAR_GET(xma_frame_trim_block_granular), trim_carry_);
      current_frame_remaining_subframes_ = uint8_t(applied.blocks);
      if (applied.dropped_samples) {
        Bump(g_xma_sib.trim_applied_samples, applied.dropped_samples);
      }
      if (applied.unapplied_end + applied.unaligned_samples) {
        Bump(g_xma_sib.trim_unapplied, uint64_t(applied.unapplied_end) + applied.unaligned_samples);
      }
    }

    // Loop end: limit output to subframes 0..loop_subframe_end.
    if (is_loop_end_frame) {
      loop_frame_output_limit_ = (data->loop_subframe_end + 1) << data->is_stereo;
    } else {
      loop_frame_output_limit_ = 0;
    }

    // Loop start: skip leading subframes per loop_subframe_skip. skip == 4
    // means the whole frame is a warm-up frame (frame-aligned loop start):
    // decode seeds the codec state, output is fully discarded.
    if (loop_start_skip_pending_) {
      const uint8_t skip = data->loop_subframe_skip << data->is_stereo;
      current_frame_remaining_subframes_ -= std::min(skip, current_frame_remaining_subframes_);
      loop_start_skip_pending_ = false;
    }

    // KI-071 D1: first frame after a Clear. dword1 bits 24-26 are written by
    // the guest setter sub_823EDC80 with the read offset (0 for a fresh start,
    // (start_sample >> 7) & 3 for a seek start); the host otherwise uses the
    // field only as ring headroom (output_buffer_padding).
    if (start_pending_) {
      start_pending_ = false;
      const uint32_t field = data->output_buffer_padding;
      sib_start_skip_.store(field, std::memory_order_relaxed);
      sib_start_kick_.store(sib_kick_group_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
      sib_rate_.store(uint32_t(GetSampleRate(data->sample_rate)), std::memory_order_relaxed);
      sib_stereo_.store(bool(data->is_stereo), std::memory_order_relaxed);
      LogFirstDecode(data, entry_read_offset, codec_reinit, trim);
      // KI-071: burst-only dump decision (the same 50 ms / 3+ others rule).
      if (!REXCVAR_GET(xma_dump_contexts).empty()) {
        dump_start_us_ = XmaSteadyNowUs();
        std::vector<uint32_t> joined;
        {
          std::lock_guard<std::mutex> guard(g_xma_dump_burst_mutex);
          const uint32_t serial = g_xma_dump_burst.OnStart(
              dump_start_us_, id(), uint32_t(GetSampleRate(data->sample_rate)),
              sib_start_kick_.load(std::memory_order_relaxed), &joined);
          for (uint32_t ctx : joined) {
            if (ctx < g_xma_dump_serial.size()) {
              g_xma_dump_serial[ctx].store(serial, std::memory_order_relaxed);
            }
          }
        }
      }
      if (field) {
        Bump(g_xma_sib.start_skips);
      }
      if (field && REXCVAR_GET(xma_honor_start_skip)) {
        const uint8_t skip =
            xma::StartSkipBlocks(field, bool(data->is_stereo), current_frame_remaining_subframes_);
        current_frame_remaining_subframes_ -= skip;
        Bump(sib_blocks_skipped_, skip);
        Bump(g_xma_sib.start_skips_applied);
      }
    }
  }

  // Compute where to go next.
  [&]() {
    if (is_loop_end_frame) {
      UpdateLoopStatus(data);
      return;
    }

    if (!packet_info.isLastFrameInPacket()) {
      // Upstream arithmetic: same packet, modulo the packet size.
      data->input_buffer_read_offset =
          xma::NextReadOffsetInPacket(data->input_buffer_read_offset, uint32_t(bits_to_copy));
      return;
    }

    AdvanceAfterLastFrame(data, uint32_t(packet_index), next_packet_index, current_input_buffer,
                          current_input_packet_count);
  }();
  LogWalkFrame(walk, data);
}

void XmaContext::AdvanceAfterLastFrame(XMA_CONTEXT_DATA* data, uint32_t packet_index,
                                       uint32_t next_packet_index, uint8_t* current_input_buffer,
                                       uint32_t current_input_packet_count) {
  uint32_t next_input_offset =
      GetNextPacketReadOffset(current_input_buffer, next_packet_index, current_input_packet_count);

  if (next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
    // KI-071 C1: 0 (upstream) unless xma_swap_carry_packet carries the stream.
    const uint32_t start_packet =
        OnInputBufferSwap(data, packet_index, next_packet_index, current_input_packet_count);
    if (data->IsAnyInputBufferValid()) {
      next_input_offset = xma::GetPacketFrameOffset(
          memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
          start_packet * kBytesPerPacket);

      if (next_input_offset > kMaxFrameSizeinBits) {
        Bump(g_xma_sib.swapbacks);
        SwapInputBuffer(data);
        return;
      }
      next_input_offset += start_packet * kBitsPerPacket;
    }
  }
  data->input_buffer_read_offset = next_input_offset;
}

void XmaContext::LogWalkFrame(const WalkFrameInfo& f, const XMA_CONTEXT_DATA* data) {
  const uint32_t read_after = data->input_buffer_read_offset;
  if (REXCVAR_GET(xma_dump_input) && !input_blocked_ && !REXCVAR_GET(xma_dump_contexts).empty()) {
    WriteInputDump(
        nullptr, 0, std::string(),
        fmt::format("{},{},{:08X},{},{:08X},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},{},"
                    "{},{}\n",
                    f.frame_index, walk_buffer_epoch_, data->GetCurrentInputBufferAddress(),
                    f.packet_index, f.packet_addr, f.start_bit, f.frame_size, f.bits_in_packet,
                    xma::WalkPathName(f.path), xma::WalkVerdictName(f.verdict),
                    f.prev.valid ? int(f.prev.trailer) : -1, f.info.header_count, f.info.walked,
                    f.info.frame_count, f.info.current_frame, f.first_frame_offset, f.skip,
                    f.info.split_tail_bits, int(f.info.split_tail_counted),
                    int(f.info.split_tail_claimed), f.entry_read_offset, read_after,
                    int(f.after_swap), int(f.after_ff), int(f.dup_prevented)));
  }
  if (f.verdict != xma::WalkVerdict::kDup && f.verdict != xma::WalkVerdict::kGap) {
    return;
  }
  if (!REXCVAR_GET(xma_frame_walk_trace) || walk_logged_ >= xma::kWalkLogPerContext ||
      !g_xma_walk_budget.Allow()) {
    return;
  }
  ++walk_logged_;
  const uint32_t left_bits = f.prev.end_bit < kBitsPerPacket ? kBitsPerPacket - f.prev.end_bit : 0;
  REXAPU_INFO(
      "[xma] walk: ctx={} kind={} frame={} path={} rate={} packet={} buffer={:08X} packets={} "
      "packet_addr={:08X} hdr_count={} xma2={} walked={} frame_count={} current={} ffo={} "
      "skip={} start_bit={} frame_bits={} bits_in_packet={} prev_end={:08X}:{} prev_trailer={} "
      "left_bits={} split_tail={}:{} counted={} claimed={} read_before={} read_after={} "
      "after_swap={} after_ff={} dup_prevented={} dup={} gap={} fix={}",
      id(), xma::WalkVerdictName(f.verdict), f.frame_index, xma::WalkPathName(f.path),
      GetSampleRate(data->sample_rate), f.packet_index, data->GetCurrentInputBufferAddress(),
      data->GetCurrentInputBufferPacketCount(), f.packet_addr, f.info.header_count,
      int(f.info.xma2), f.info.walked, f.info.frame_count, f.info.current_frame,
      f.first_frame_offset, f.skip, f.start_bit, f.frame_size, f.bits_in_packet, f.prev.packet_addr,
      f.prev.end_bit, int(f.prev.trailer), left_bits, f.info.split_tail_pos, f.info.split_tail_bits,
      int(f.info.split_tail_counted), int(f.info.split_tail_claimed), f.entry_read_offset,
      read_after, int(f.after_swap), int(f.after_ff), int(f.dup_prevented), walk_dup_, walk_gap_,
      int(REXCVAR_GET(xma_frame_walk_fix)));
}

uint32_t XmaContext::OnInputBufferSwap(const XMA_CONTEXT_DATA* data, uint32_t last_packet_index,
                                       uint32_t next_packet_index, uint32_t old_packet_count) {
  const bool fix = REXCVAR_GET(xma_swap_carry_packet);
  const bool new_valid = data->IsCurrentInputBufferValid();
  const uint32_t new_packet_count = data->GetCurrentInputBufferPacketCount();
  const xma::SwapCarry carry = xma::ComputeSwapCarry(next_packet_index, old_packet_count,
                                                     new_valid ? new_packet_count : UINT32_MAX);
  const int rate = GetSampleRate(data->sample_rate);
  g_xma_counters.swaps.fetch_add(1, std::memory_order_relaxed);
  if (rate == 44100) {
    g_xma_counters.swaps_44k.fetch_add(1, std::memory_order_relaxed);
  }
  if (carry.carry) {
    g_xma_counters.carry_nonzero.fetch_add(1, std::memory_order_relaxed);
    if (rate == 44100) {
      g_xma_counters.carry_nonzero_44k.fetch_add(1, std::memory_order_relaxed);
    }
  }
  if (new_valid && carry.fallback) {
    g_xma_counters.carry_fallbacks.fetch_add(1, std::memory_order_relaxed);
  }
  if (fix && !new_valid) {
    // The next buffer's packet count is not known yet; applied on resume.
    pending_swap_carry_packet_ = carry.carry;
  }
  const uint32_t start_packet = (fix && new_valid) ? carry.start_packet : 0;

  if (fix && new_valid && carry.fallback && AllowXmaTraceLine()) {
    REXAPU_WARN("[xma] swap carry {} out of range (packets={}) on ctx={}; packet 0", carry.carry,
                new_packet_count, id());
  }
  if (REXCVAR_GET(xma_swap_trace) && AllowXmaTraceLine()) {
    const uint8_t old_buffer = data->current_buffer ^ 1;
    const uint32_t old_ptr = data->GetInputBufferAddress(old_buffer);
    REXAPU_INFO(
        "[xma] swap: ctx={} stereo={} rate={} packets={} last_packet={} skip={} "
        "next_packet_index={} carry={} new_valid={} new_packets={} start_packet={} fix={} "
        "buffer={:08X} shared_buffer_ctxs={}",
        id(), uint32_t(data->is_stereo), rate, old_packet_count, last_packet_index,
        next_packet_index - last_packet_index - 1, next_packet_index, carry.carry, int(new_valid),
        new_valid ? new_packet_count : 0, start_packet, int(fix), old_ptr,
        FormatSharedBufferContexts(old_ptr));
  }
  return start_packet;
}

std::string XmaContext::FormatSharedBufferContexts(uint32_t ptr) {
  const uint32_t own_offset = id_ * uint32_t(sizeof(XMA_CONTEXT_DATA));
  if (!ptr || guest_ptr_ < own_offset) {
    return "-";
  }
  // Contexts are one contiguous guest array (XmaDecoder::Setup); read-only scan.
  const uint32_t base = guest_ptr_ - own_offset;
  std::string result;
  uint32_t listed = 0;
  for (uint32_t i = 0; i < kTraceContextArrayCount; ++i) {
    if (i == id_) {
      continue;
    }
    const XMA_CONTEXT_DATA other(
        memory()->TranslateVirtual(base + i * uint32_t(sizeof(XMA_CONTEXT_DATA))));
    if (other.input_buffer_0_ptr != ptr && other.input_buffer_1_ptr != ptr) {
      continue;
    }
    if (listed == 8) {
      result += ",...";
      break;
    }
    result += listed ? fmt::format(",{}", i) : fmt::format("{}", i);
    ++listed;
  }
  return result.empty() ? "-" : result;
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
