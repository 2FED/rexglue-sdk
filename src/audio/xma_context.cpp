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

#include <fmt/format.h>

#include <rex/audio/xma/context.h>
#include <rex/audio/audio_route_audit.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/doubling_fixes.h>
#include <rex/audio/xma/helpers.h>
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

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
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

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);

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
    Consume(&output_rb, &data);

    if (!data.IsAnyInputBufferValid() || data.error_status == 4) {
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
  raw_frame_.fill(0);
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
  pending_swap_carry_packet_ = 0;
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
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    if (packet_frame_offset <= kMaxFrameSizeinBits) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  return kBitsPerPacketHeader;
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

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
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
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
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

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
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
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

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

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    uint32_t next_input_offset =
        GetNextPacketReadOffset(current_input_buffer, packet_index + 1, current_input_packet_count);
    if (next_input_offset == kBitsPerPacketHeader) {
      SwapInputBuffer(data);
    }
    data->input_buffer_read_offset = next_input_offset;
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
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
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
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

  raw_frame_.fill(0);

  PrepareDecoder(data->sample_rate, bool(data->is_stereo));
  PreparePacket(packet_info.current_frame_size_, padding_start);
  if (DecodePacket(av_context_, av_packet_, av_frame_)) {
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 raw_frame_.data());
    RecordAudioRouteXmaFrame(raw_frame_.data(), raw_frame_.size());
    current_frame_remaining_subframes_ = 4 << data->is_stereo;

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
  }

  // Compute where to go next.
  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  uint32_t next_input_offset =
      GetNextPacketReadOffset(current_input_buffer, next_packet_index, current_input_packet_count);

  if (next_input_offset == kBitsPerPacketHeader) {
    SwapInputBuffer(data);
    // KI-071 C1: 0 (upstream) unless xma_swap_carry_packet carries the stream.
    const uint32_t start_packet = OnInputBufferSwap(data, uint32_t(packet_index), next_packet_index,
                                                    current_input_packet_count);
    if (data->IsAnyInputBufferValid()) {
      next_input_offset = xma::GetPacketFrameOffset(
          memory()->TranslatePhysical(data->GetCurrentInputBufferAddress()) +
          start_packet * kBytesPerPacket);

      if (next_input_offset > kMaxFrameSizeinBits) {
        SwapInputBuffer(data);
        return;
      }
      next_input_offset += start_packet * kBitsPerPacket;
    }
  }
  data->input_buffer_read_offset = next_input_offset;
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
