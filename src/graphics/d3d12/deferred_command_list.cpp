/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 * @modified    MCLA-R, 2026 - Chunked stream and streaming replay worker
 */

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/deferred_command_list.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/thread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

REXCVAR_DEFINE_BOOL(d3d12_replay_batched_wake, false, "GPU/D3D12",
                    "M10-CPU-F: the streaming replay worker spins 128 instead of 4096 pauses "
                    "before sleeping, and the command processor wakes it only every 512 "
                    "commands (at once after a block seal and at the end of a submission). "
                    "Read at the start of each submission")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::graphics::d3d12 {

namespace {
// Replay statistics, reported once a minute. Commands are replayed by one
// thread at a time (the command processor thread, or the streaming worker
// while the command processor thread is not replaying).
struct ReplayStats {
  std::array<uint64_t, 64> command_count{};
  uint64_t barrier_count = 0;
  uint64_t submissions = 0;
  uint64_t streamed_submissions = 0;
  uint64_t replay_us = 0;
  uint64_t finish_wait_us = 0;
  uint64_t worker_sleeps = 0;
  // M10-CPU-F idle and wake counters (MCLA_REPLAY_WAKE).
  ReplayWakeCounters wake;
  std::chrono::steady_clock::time_point next_report{};
};
ReplayStats g_replay_stats;
// M10-001: never reset (the per-minute stats above are).
std::atomic<uint64_t> g_replay_total_us{0};

void ReportReplayStats() {
  ReplayStats& s = g_replay_stats;
  const auto now = std::chrono::steady_clock::now();
  if (s.next_report.time_since_epoch().count() == 0) {
    s.next_report = now + std::chrono::seconds(60);
    return;
  }
  if (now < s.next_report) {
    return;
  }
  s.next_report = now + std::chrono::seconds(60);
  std::array<std::pair<uint64_t, size_t>, 64> sorted;
  uint64_t total = 0;
  for (size_t i = 0; i < sorted.size(); ++i) {
    sorted[i] = {s.command_count[i], i};
    total += s.command_count[i];
  }
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string top;
  for (size_t i = 0; i < 12 && sorted[i].first; ++i) {
    top += " " + std::to_string(sorted[i].second) + ":" + std::to_string(sorted[i].first);
  }
  REXGPU_INFO(
      "MCLA replay/min: submissions {} (streamed {}), replay {} us, end-of-submission wait {} us, "
      "worker sleeps {}, commands {}, barriers {}; by id{}",
      s.submissions, s.streamed_submissions, s.replay_us, s.finish_wait_us, s.worker_sleeps, total,
      s.barrier_count, top);
  s.wake.sleeps = s.worker_sleeps;
  REXGPU_INFO("{}", FormatReplayWakeLine(REXCVAR_GET(d3d12_replay_batched_wake), s.wake));
  s.wake = {};
  s.command_count.fill(0);
  s.barrier_count = s.submissions = s.streamed_submissions = 0;
  s.replay_us = s.finish_wait_us = s.worker_sleeps = 0;
}

inline void SpinPause() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}
}  // namespace

DeferredCommandList::DeferredCommandList(const D3D12CommandProcessor& command_processor,
                                         size_t initial_size)
    : command_processor_(command_processor) {
  EnsureBlock(0, std::max(initial_size / sizeof(uintmax_t), kBlockElements));
}

DeferredCommandList::~DeferredCommandList() {
  if (worker_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      worker_state_ = WorkerState::kStop;
    }
    worker_cond_.notify_all();
    worker_.join();
  }
}

void DeferredCommandList::EnsureBlock(size_t index, size_t min_elements) {
  assert_true(index < kMaxBlocks);
  std::unique_ptr<Block>& block = blocks_[index];
  if (!block) {
    block = std::make_unique<Block>();
  }
  if (block->capacity < min_elements) {
    block->data = std::make_unique<uintmax_t[]>(min_elements);
    block->capacity = min_elements;
  }
  block->used.store(0, std::memory_order_relaxed);
}

void DeferredCommandList::Reset() {
  assert_false(streaming_);
  write_block_ = 0;
  write_used_ = 0;
  unpublished_commands_ = 0;
  blocks_[0]->used.store(0, std::memory_order_relaxed);
  published_.store(0, std::memory_order_relaxed);
}

void DeferredCommandList::Publish(bool force_wake) {
  // Everything up to the current write position is complete. The fence pairs
  // with the worker's seq_cst store of worker_sleeping_ before it re-checks the
  // published position, so a wake-up cannot be lost. With the batched wake a
  // sleeping worker may be left asleep for up to one wake batch; it is always
  // woken after a block seal and by FinishStreaming.
  const uint32_t commands = unpublished_commands_;
  unpublished_commands_ = 0;
  published_.store((uint64_t(write_block_) << 32) | uint64_t(write_used_),
                   std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!streaming_) {
    return;
  }
  switch (wake_batcher_.OnPublish(commands, worker_sleeping_.load(std::memory_order_relaxed),
                                  force_wake)) {
    case ReplayWakeBatcher::Decision::kNone:
      return;
    case ReplayWakeBatcher::Decision::kDeferred:
      ++g_replay_stats.wake.deferred_wakes;
      return;
    case ReplayWakeBatcher::Decision::kWake:
      ++g_replay_stats.wake.wakes;
      break;
    case ReplayWakeBatcher::Decision::kForced:
      ++g_replay_stats.wake.forced_wakes;
      break;
  }
  std::lock_guard<std::mutex> lock(worker_mutex_);
  worker_cond_.notify_all();
}

void* DeferredCommandList::WriteCommand(Command command, size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t command_size_elements = kCommandHeaderSizeElements + arguments_size_elements;
  // The previous commands' arguments have been fully written by their callers
  // by now - make them visible to the streaming worker every few commands.
  if (streaming_ && ++unpublished_commands_ >= kPublishBatchCommands) {
    Publish();
  }
  Block* block = blocks_[write_block_].get();
  if (write_used_ + command_size_elements > block->capacity) {
    // Seal the current block before moving to the next one.
    block->used.store(write_used_, std::memory_order_release);
    if (streaming_) {
      wake_batcher_.NoteSeal();
    }
    EnsureBlock(write_block_ + 1, std::max(kBlockElements, command_size_elements));
    ++write_block_;
    write_used_ = 0;
    block = blocks_[write_block_].get();
  }
  uintmax_t* command_data = block->data.get() + write_used_;
  CommandHeader& header = *reinterpret_cast<CommandHeader*>(command_data);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  write_used_ += command_size_elements;
  return command_data + kCommandHeaderSizeElements;
}

void DeferredCommandList::Execute(ID3D12GraphicsCommandList* command_list,
                                  ID3D12GraphicsCommandList1* command_list_1) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  assert_false(streaming_);
  const auto replay_begin = std::chrono::steady_clock::now();
  ReplayState state;
  state.command_list = command_list;
  state.command_list_1 = command_list_1;
  for (size_t i = 0; i <= write_block_; ++i) {
    size_t used =
        i < write_block_ ? blocks_[i]->used.load(std::memory_order_relaxed) : write_used_;
    Replay(blocks_[i]->data.get(), used, state);
  }
  ++g_replay_stats.submissions;
  const uint64_t replay_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - replay_begin)
                                          .count());
  g_replay_stats.replay_us += replay_us;
  g_replay_total_us.fetch_add(replay_us, std::memory_order_relaxed);
  ReportReplayStats();
}

uint64_t DeferredCommandList::TotalReplayMicroseconds() {
  return g_replay_total_us.load(std::memory_order_relaxed);
}

void DeferredCommandList::BeginStreaming(ID3D12GraphicsCommandList* command_list,
                                         ID3D12GraphicsCommandList1* command_list_1) {
  assert_false(streaming_);
  if (!worker_.joinable()) {
    worker_ = std::thread([this] { WorkerMain(); });
  }
  const bool batched_wake = REXCVAR_GET(d3d12_replay_batched_wake);
  wake_batcher_.Begin(batched_wake);
  ++g_replay_stats.wake.submissions;
  if (batched_wake) {
    ++g_replay_stats.wake.batched_submissions;
  }
  {
    std::lock_guard<std::mutex> lock(worker_mutex_);
    assert_true(worker_state_ == WorkerState::kIdle);
    worker_command_list_ = command_list;
    worker_command_list_1_ = command_list_1;
    worker_batched_wake_ = batched_wake;
    worker_finish_requested_.store(false, std::memory_order_relaxed);
    worker_state_ = WorkerState::kStreaming;
  }
  streaming_ = true;
  // Commands written before streaming started (after Reset) become visible.
  Publish(true);
  worker_cond_.notify_all();
}

void DeferredCommandList::FinishStreaming() {
  assert_true(streaming_);
  const auto wait_begin = std::chrono::steady_clock::now();
  blocks_[write_block_]->used.store(write_used_, std::memory_order_release);
  Publish(true);
  {
    std::unique_lock<std::mutex> lock(worker_mutex_);
    worker_finish_requested_.store(true, std::memory_order_seq_cst);
    worker_cond_.notify_all();
    worker_done_cond_.wait(lock, [this] { return worker_state_ == WorkerState::kIdle; });
  }
  streaming_ = false;
  g_replay_stats.finish_wait_us +=
      uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - wait_begin)
                   .count());
  ++g_replay_stats.submissions;
  ++g_replay_stats.streamed_submissions;
  ReportReplayStats();
}

void DeferredCommandList::WorkerMain() {
  rex::thread::set_current_thread_name("D3D12 Replay");
  while (true) {
    ReplayState state;
    uint32_t spin_limit = kReplaySpinPausesDefault;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_cond_.wait(lock, [this] { return worker_state_ != WorkerState::kIdle; });
      if (worker_state_ == WorkerState::kStop) {
        return;
      }
      state.command_list = worker_command_list_;
      state.command_list_1 = worker_command_list_1_;
      spin_limit = ReplaySpinLimit(worker_batched_wake_);
    }
    size_t cursor_block = 0;
    size_t cursor_offset = 0;
    uint64_t replay_ns = 0;
    uint32_t idle_spins = 0;
    // M10-CPU-F: idle-spin accounting (one clock read at each end of an idle
    // episode, none per pause).
    uint64_t idle_episodes = 0;
    uint64_t spin_pauses = 0;
    uint64_t spin_ns = 0;
    std::chrono::steady_clock::time_point spin_begin{};
    const auto end_spin = [&]() {
      if (idle_spins) {
        spin_pauses += idle_spins;
        spin_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - spin_begin)
                                .count());
        idle_spins = 0;
      }
    };
    while (true) {
      uint64_t published = published_.load(std::memory_order_acquire);
      size_t published_block = size_t(published >> 32);
      size_t published_offset = size_t(published & UINT32_MAX);
      if (cursor_block < published_block) {
        // The block was sealed before the position moved past it.
        size_t block_end = blocks_[cursor_block]->used.load(std::memory_order_acquire);
        if (cursor_offset < block_end) {
          auto begin = std::chrono::steady_clock::now();
          Replay(blocks_[cursor_block]->data.get() + cursor_offset, block_end - cursor_offset,
                 state);
          replay_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count());
        }
        ++cursor_block;
        cursor_offset = 0;
        end_spin();
        continue;
      }
      if (cursor_offset < published_offset) {
        auto begin = std::chrono::steady_clock::now();
        Replay(blocks_[cursor_block]->data.get() + cursor_offset,
               published_offset - cursor_offset, state);
        replay_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - begin)
                                  .count());
        cursor_offset = published_offset;
        end_spin();
        continue;
      }
      // Caught up with the writer.
      if (worker_finish_requested_.load(std::memory_order_seq_cst)) {
        // FinishStreaming publishes the final position before requesting the
        // finish, so if nothing new is visible now, everything is replayed.
        if (published_.load(std::memory_order_acquire) == published) {
          end_spin();
          break;
        }
        continue;
      }
      if (!idle_spins) {
        ++idle_episodes;
        spin_begin = std::chrono::steady_clock::now();
      }
      if (++idle_spins < spin_limit) {
        SpinPause();
        continue;
      }
      end_spin();
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_sleeping_.store(true, std::memory_order_seq_cst);
      if (published_.load(std::memory_order_seq_cst) == published &&
          !worker_finish_requested_.load(std::memory_order_seq_cst)) {
        ++g_replay_stats.worker_sleeps;
        worker_cond_.wait(lock, [this, published] {
          return published_.load(std::memory_order_seq_cst) != published ||
                 worker_finish_requested_.load(std::memory_order_seq_cst);
        });
      }
      worker_sleeping_.store(false, std::memory_order_relaxed);
    }
    {
      std::lock_guard<std::mutex> lock(worker_mutex_);
      g_replay_stats.replay_us += replay_ns / 1000;
      g_replay_total_us.fetch_add(replay_ns / 1000, std::memory_order_relaxed);
      g_replay_stats.wake.idle_episodes += idle_episodes;
      g_replay_stats.wake.spin_pauses += spin_pauses;
      g_replay_stats.wake.spin_us += spin_ns / 1000;
      worker_state_ = WorkerState::kIdle;
    }
    worker_done_cond_.notify_all();
  }
}

void DeferredCommandList::Replay(const uintmax_t* stream, size_t stream_remaining,
                                 ReplayState& state) {
  ID3D12GraphicsCommandList* command_list = state.command_list;
  ID3D12GraphicsCommandList1* command_list_1 = state.command_list_1;
  ID3D12PipelineState*& current_pipeline_state = state.current_pipeline_state;
  while (stream_remaining != 0) {
    const CommandHeader& header = *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;
    ++g_replay_stats.command_count[size_t(header.command) & 63];
    switch (header.command) {
      case Command::kD3DClearDepthStencilView: {
        auto& args = *reinterpret_cast<const ClearDepthStencilViewHeader*>(stream);
        command_list->ClearDepthStencilView(
            args.depth_stencil_view, args.clear_flags, args.depth, args.stencil, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearRenderTargetView: {
        auto& args = *reinterpret_cast<const ClearRenderTargetViewHeader*>(stream);
        command_list->ClearRenderTargetView(
            args.render_target_view, args.color_rgba, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DClearUnorderedAccessViewUint: {
        auto& args = *reinterpret_cast<const ClearUnorderedAccessViewHeader*>(stream);
        command_list->ClearUnorderedAccessViewUint(
            args.view_gpu_handle_in_current_heap, args.view_cpu_handle, args.resource,
            args.values_uint, args.num_rects,
            args.num_rects ? reinterpret_cast<const D3D12_RECT*>(&args + 1) : nullptr);
      } break;
      case Command::kD3DCopyBufferRegion: {
        auto& args = *reinterpret_cast<const D3DCopyBufferRegionArguments*>(stream);
        command_list->CopyBufferRegion(args.dst_buffer, args.dst_offset, args.src_buffer,
                                       args.src_offset, args.num_bytes);
      } break;
      case Command::kD3DCopyResource: {
        auto& args = *reinterpret_cast<const D3DCopyResourceArguments*>(stream);
        command_list->CopyResource(args.dst_resource, args.src_resource);
      } break;
      case Command::kCopyTexture: {
        auto& args = *reinterpret_cast<const CopyTextureArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, 0, 0, 0, &args.src, nullptr);
      } break;
      case Command::kD3DCopyTextureRegion: {
        auto& args = *reinterpret_cast<const D3DCopyTextureRegionArguments*>(stream);
        command_list->CopyTextureRegion(&args.dst, args.dst_x, args.dst_y, args.dst_z, &args.src,
                                        args.has_src_box ? &args.src_box : nullptr);
      } break;
      case Command::kD3DDispatch: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDispatchArguments*>(stream);
          command_list->Dispatch(args.thread_group_count_x, args.thread_group_count_y,
                                 args.thread_group_count_z);
        }
      } break;
      case Command::kD3DDrawIndexedInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawIndexedInstancedArguments*>(stream);
          command_list->DrawIndexedInstanced(args.index_count_per_instance, args.instance_count,
                                             args.start_index_location, args.base_vertex_location,
                                             args.start_instance_location);
        }
      } break;
      case Command::kD3DDrawInstanced: {
        if (current_pipeline_state != nullptr) {
          auto& args = *reinterpret_cast<const D3DDrawInstancedArguments*>(stream);
          command_list->DrawInstanced(args.vertex_count_per_instance, args.instance_count,
                                      args.start_vertex_location, args.start_instance_location);
        }
      } break;
      case Command::kD3DBeginQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->BeginQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DEndQuery: {
        auto& args = *reinterpret_cast<const D3DQueryArguments*>(stream);
        command_list->EndQuery(args.query_heap, args.type, args.index);
      } break;
      case Command::kD3DResolveQueryData: {
        auto& args = *reinterpret_cast<const D3DResolveQueryDataArguments*>(stream);
        command_list->ResolveQueryData(args.query_heap, args.type, args.start_index,
                                       args.num_queries, args.destination_buffer,
                                       args.aligned_destination_buffer_offset);
      } break;
      case Command::kD3DIASetIndexBuffer: {
        auto view = reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(stream);
        command_list->IASetIndexBuffer(view->Format != DXGI_FORMAT_UNKNOWN ? view : nullptr);
      } break;
      case Command::kD3DIASetPrimitiveTopology: {
        command_list->IASetPrimitiveTopology(
            *reinterpret_cast<const D3D12_PRIMITIVE_TOPOLOGY*>(stream));
      } break;
      case Command::kD3DIASetVertexBuffers: {
        static_assert(alignof(D3D12_VERTEX_BUFFER_VIEW) <= alignof(uintmax_t));
        auto& args = *reinterpret_cast<const D3DIASetVertexBuffersHeader*>(stream);
        command_list->IASetVertexBuffers(args.start_slot, args.num_views,
                                         reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(
                                             reinterpret_cast<const uint8_t*>(stream) +
                                             rex::align(sizeof(D3DIASetVertexBuffersHeader),
                                                        alignof(D3D12_VERTEX_BUFFER_VIEW))));
      } break;
      case Command::kD3DOMSetBlendFactor: {
        command_list->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(stream));
      } break;
      case Command::kD3DOMSetRenderTargets: {
        auto& args = *reinterpret_cast<const D3DOMSetRenderTargetsArguments*>(stream);
        command_list->OMSetRenderTargets(
            args.num_render_target_descriptors, args.render_target_descriptors,
            args.rts_single_handle_to_descriptor_range ? TRUE : FALSE,
            args.depth_stencil ? &args.depth_stencil_descriptor : nullptr);
      } break;
      case Command::kD3DOMSetStencilRef: {
        command_list->OMSetStencilRef(*reinterpret_cast<const UINT*>(stream));
      } break;
      case Command::kD3DResourceBarrier: {
        static_assert(alignof(D3D12_RESOURCE_BARRIER) <= alignof(uintmax_t));
        g_replay_stats.barrier_count += *reinterpret_cast<const UINT*>(stream);
        command_list->ResourceBarrier(
            *reinterpret_cast<const UINT*>(stream),
            reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(
                reinterpret_cast<const uint8_t*>(stream) +
                rex::align(sizeof(UINT), alignof(D3D12_RESOURCE_BARRIER))));
      } break;
      case Command::kRSSetScissorRect: {
        command_list->RSSetScissorRects(1, reinterpret_cast<const D3D12_RECT*>(stream));
      } break;
      case Command::kRSSetViewport: {
        command_list->RSSetViewports(1, reinterpret_cast<const D3D12_VIEWPORT*>(stream));
      } break;
      case Command::kD3DSetComputeRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetComputeRoot32BitConstants(args->root_parameter_index,
                                                   args->num_32bit_values_to_set, args + 1,
                                                   args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetGraphicsRoot32BitConstants: {
        auto args = reinterpret_cast<const SetRoot32BitConstantsHeader*>(stream);
        command_list->SetGraphicsRoot32BitConstants(args->root_parameter_index,
                                                    args->num_32bit_values_to_set, args + 1,
                                                    args->dest_offset_in_32bit_values);
      } break;
      case Command::kD3DSetComputeRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootConstantBufferView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootConstantBufferView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootConstantBufferView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetComputeRootDescriptorTable(args.root_parameter_index,
                                                    args.base_descriptor);
      } break;
      case Command::kD3DSetGraphicsRootDescriptorTable: {
        auto& args = *reinterpret_cast<const SetRootDescriptorTableArguments*>(stream);
        command_list->SetGraphicsRootDescriptorTable(args.root_parameter_index,
                                                     args.base_descriptor);
      } break;
      case Command::kD3DSetComputeRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootShaderResourceView(args.root_parameter_index,
                                                       args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootShaderResourceView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootShaderResourceView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetComputeRootSignature: {
        command_list->SetComputeRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetGraphicsRootSignature: {
        command_list->SetGraphicsRootSignature(
            *reinterpret_cast<ID3D12RootSignature* const*>(stream));
      } break;
      case Command::kD3DSetComputeRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetComputeRootUnorderedAccessView(args.root_parameter_index,
                                                        args.buffer_location);
      } break;
      case Command::kD3DSetGraphicsRootUnorderedAccessView: {
        auto& args = *reinterpret_cast<const SetRootConstantBufferViewArguments*>(stream);
        command_list->SetGraphicsRootUnorderedAccessView(args.root_parameter_index,
                                                         args.buffer_location);
      } break;
      case Command::kSetDescriptorHeaps: {
        auto& args = *reinterpret_cast<const SetDescriptorHeapsArguments*>(stream);
        UINT num_descriptor_heaps = 0;
        ID3D12DescriptorHeap* descriptor_heaps[2];
        if (args.cbv_srv_uav_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.cbv_srv_uav_descriptor_heap;
        }
        if (args.sampler_descriptor_heap != nullptr) {
          descriptor_heaps[num_descriptor_heaps++] = args.sampler_descriptor_heap;
        }
        command_list->SetDescriptorHeaps(num_descriptor_heaps, descriptor_heaps);
      } break;
      case Command::kD3DSetPipelineState: {
        current_pipeline_state = *reinterpret_cast<ID3D12PipelineState* const*>(stream);
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kSetPipelineStateHandle: {
        current_pipeline_state =
            command_processor_.GetD3D12PipelineByHandle(*reinterpret_cast<void* const*>(stream));
        if (current_pipeline_state) {
          command_list->SetPipelineState(current_pipeline_state);
        }
      } break;
      case Command::kD3DSetSamplePositions: {
        if (command_list_1 != nullptr) {
          auto& args = *reinterpret_cast<const D3DSetSamplePositionsArguments*>(stream);
          command_list_1->SetSamplePositions(
              args.num_samples_per_pixel, args.num_pixels,
              (args.num_samples_per_pixel && args.num_pixels)
                  ? const_cast<D3D12_SAMPLE_POSITION*>(args.sample_positions)
                  : nullptr);
        }
      } break;
      case Command::kBeginDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(stream) + sizeof(DebugMarkerHeader));
        command_list->BeginEvent(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      case Command::kEndDebugMarker: {
        command_list->EndEvent();
      } break;
      case Command::kInsertDebugMarker: {
        auto& args = *reinterpret_cast<const DebugMarkerHeader*>(stream);
        const char* label_name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(stream) + sizeof(DebugMarkerHeader));
        command_list->SetMarker(1, label_name, static_cast<UINT>(args.label_length + 1));
      } break;
      default:
        assert_unhandled_case(header.command);
        break;
    }
    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }
}

}  // namespace rex::graphics::d3d12
