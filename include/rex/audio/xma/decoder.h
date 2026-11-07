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

#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <set>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/register_file.h>
#include <rex/bit.h>
#include <rex/kernel.h>
#include <rex/system/xthread.h>

namespace rex::runtime {
class FunctionDispatcher;
}  // namespace rex::runtime

namespace rex::audio {

struct XMA_CONTEXT_DATA;

class XmaDecoder {
 public:
  explicit XmaDecoder(runtime::FunctionDispatcher* function_dispatcher);
  ~XmaDecoder();

  memory::Memory* memory() const { return memory_; }
  runtime::FunctionDispatcher* function_dispatcher() const { return function_dispatcher_; }

  X_STATUS Setup(system::KernelState* kernel_state);
  void Shutdown();

  uint32_t context_array_ptr() const { return register_file_[XmaRegister::ContextArrayAddress]; }

  uint32_t AllocateContext();
  void ReleaseContext(uint32_t guest_ptr);
  bool BlockOnContext(uint32_t guest_ptr, bool poll);

  uint32_t ReadRegister(uint32_t addr);
  void WriteRegister(uint32_t addr, uint32_t value);

  bool is_paused() const { return paused_; }
  void Pause();
  void Resume();

 protected:
  int GetContextId(uint32_t guest_ptr);

 private:
  void WorkerThreadMain();
  // KI-071 sibling trace (xma_sibling_trace): kick burst ids, and once per
  // second one line per active sibling group plus the per-minute summary.
  uint32_t NextKickGroup();
  void MaybeEmitSiblingTrace();

  static uint32_t MMIOReadRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr) {
    return as->ReadRegister(addr);
  }
  static void MMIOWriteRegisterThunk(void* ppc_context, XmaDecoder* as, uint32_t addr,
                                     uint32_t value) {
    as->WriteRegister(addr, value);
  }

 protected:
  memory::Memory* memory_ = nullptr;
  runtime::FunctionDispatcher* function_dispatcher_ = nullptr;

  std::atomic<bool> worker_running_ = {false};
  system::object_ref<system::XHostThread> worker_thread_;
  std::unique_ptr<rex::thread::Event> work_event_ = nullptr;

  std::atomic<bool> paused_ = false;
  rex::thread::Fence pause_fence_;   // Signaled when worker paused.
  rex::thread::Fence resume_fence_;  // Signaled when resume requested.

  XmaRegisterFile register_file_;

  static const uint32_t kContextCount = 320;
  XmaContext contexts_[kContextCount];
  bit::BitMap context_bitmap_;

  uint32_t context_data_first_ptr_ = 0;
  uint32_t context_data_last_ptr_ = 0;

  // KI-071 sibling trace state. The kick fields are approximate under
  // concurrent kicks (trace only); the scan state is owned by the thread that
  // wins sib_second_ms_.
  std::atomic<uint32_t> sib_kick_group_ = 0;
  std::atomic<int64_t> sib_last_kick_us_ = -1;
  std::atomic<int64_t> sib_second_ms_ = -1;
  int64_t sib_minute_ms_ = -1;
  uint64_t sib_last_emitted_[kContextCount] = {};
  std::set<uint32_t> sib_minute_groups_;
  // KI-071 step 3: start-burst + contiguous-buffer groups with stable ids.
  xma::StickySiblingGrouper sib_grouper_;
  uint64_t sib_minute_max_frame_delta_ = 0;
  uint64_t sib_minute_max_lost_delta_ = 0;
  uint64_t sib_suppressed_groups_ = 0;
};

}  // namespace rex::audio
