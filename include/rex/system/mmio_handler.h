/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include <rex/platform.h>
#include <rex/thread/mutex.h>

namespace rex::arch {
class Exception;
class HostThreadContext;
}  // namespace rex::arch

namespace rex::runtime {

typedef uint32_t (*MMIOReadCallback)(void* ppc_context, void* callback_context, uint32_t addr);
typedef void (*MMIOWriteCallback)(void* ppc_context, void* callback_context, uint32_t addr,
                                  uint32_t value);

struct MMIORange {
  uint32_t address;
  uint32_t mask;
  uint32_t size;
  void* callback_context;
  MMIOReadCallback read;
  MMIOWriteCallback write;
};

struct MemoryAccessDiagnosticSnapshot {
  // Relaxed cumulative samples, not an atomic transaction. Durations can
  // overlap across threads. Fault phases count successful non-MMIO write
  // recovery only and exclude OS exception delivery before our handler.
  uint64_t non_mmio_write_fault_count = 0;
  uint64_t non_mmio_write_fault_us = 0;
  uint64_t write_fault_lock_wait_us = 0;
  uint64_t write_fault_query_us = 0;
  uint64_t write_fault_callback_us = 0;
  uint64_t write_fault_already_cleared_count = 0;
  // Watch protection calls include attempts that fail. Memory fills these
  // fields; a standalone MMIOHandler snapshot leaves them zero.
  uint64_t watch_arm_calls = 0;
  uint64_t watch_arm_us = 0;
  // Entry lock wait for each nonempty alias arming range, including unchanged
  // ranges. Nested callback/protection timing must not be summed as CPU time.
  uint64_t watch_arm_lock_wait_us = 0;
  uint64_t watch_disarm_calls = 0;
  uint64_t watch_disarm_us = 0;
  uint64_t watch_protect_failures = 0;
  uint64_t watch_arm_gap_splits = 0;
  // Gaps between complete committed guest-RW disarm runs within one approved
  // alias range. Gap pages are unwatched, committed and guest-RW. This counts
  // structural opportunities only; native protection calls are unchanged.
  uint64_t watch_disarm_safe_gap_splits = 0;
  // Unprotect episodes whose optional gap classification could not acquire the
  // alias heap mutex immediately. Legacy protection behavior still executes.
  uint64_t watch_disarm_gap_lock_misses = 0;
};

// NOTE: only one can exist at a time!
class MMIOHandler {
 public:
  virtual ~MMIOHandler();

  typedef uint32_t (*HostToGuestVirtual)(const void* context, const void* host_address);
  typedef bool (*AccessViolationCallback)(
      std::unique_lock<std::recursive_mutex> global_lock_locked_once, void* context,
      void* host_address, bool is_write);

  // access_violation_callback is called with global_critical_region locked once
  // on the thread, so if multiple threads trigger an access violation in the
  // same page, the callback will be called only once.
  static std::unique_ptr<MMIOHandler> Install(uint8_t* virtual_membase, uint8_t* physical_membase,
                                              uint8_t* membase_end,
                                              HostToGuestVirtual host_to_guest_virtual,
                                              const void* host_to_guest_virtual_context,
                                              AccessViolationCallback access_violation_callback,
                                              void* access_violation_callback_context);
  static MMIOHandler* global_handler();

  bool RegisterRange(uint32_t virtual_address, uint32_t mask, uint32_t size, void* context,
                     MMIOReadCallback read_callback, MMIOWriteCallback write_callback);
  MMIORange* LookupRange(uint32_t virtual_address);

  bool CheckLoad(uint32_t virtual_address, uint32_t* out_value);
  bool CheckStore(uint32_t virtual_address, uint32_t value);

  // Relaxed totals of successfully handled non-MMIO write faults. Includes
  // lock contention and watch recovery, but not OS exception delivery time.
  MemoryAccessDiagnosticSnapshot GetAccessDiagnosticSnapshot() const;

 protected:
  MMIOHandler(uint8_t* virtual_membase, uint8_t* physical_membase, uint8_t* membase_end,
              HostToGuestVirtual host_to_guest_virtual, const void* host_to_guest_virtual_context,
              AccessViolationCallback access_violation_callback,
              void* access_violation_callback_context);

  static bool ExceptionCallbackThunk(arch::Exception* ex, void* data);
  bool ExceptionCallback(arch::Exception* ex);

  uint8_t* virtual_membase_;
  uint8_t* physical_membase_;
  uint8_t* memory_end_;

  std::vector<MMIORange> mapped_ranges_;

  HostToGuestVirtual host_to_guest_virtual_;
  const void* host_to_guest_virtual_context_;

  AccessViolationCallback access_violation_callback_;
  void* access_violation_callback_context_;

  static MMIOHandler* global_handler_;

  rex::thread::global_critical_region global_critical_region_;

 private:
  std::atomic<uint64_t> non_mmio_write_fault_count_{0};
  std::atomic<uint64_t> non_mmio_write_fault_us_{0};
  std::atomic<uint64_t> write_fault_lock_wait_us_{0};
  std::atomic<uint64_t> write_fault_query_us_{0};
  std::atomic<uint64_t> write_fault_callback_us_{0};
  std::atomic<uint64_t> write_fault_already_cleared_count_{0};

  struct DecodedLoadStore {
    // Matches the Xn/Wn register number for 0 reads and ignored writes in many
    // usage cases.
    static constexpr uint8_t kArm64RegZero = 31;

    // Matches the actual register number encoding for an SP base in AArch64
    // load and store instructions.
    static constexpr uint8_t kArm64MemBaseRegSp = kArm64RegZero;

    static constexpr uint8_t kArm64ValueRegX0 = 0;
    static constexpr uint8_t kArm64ValueRegZero = kArm64ValueRegX0 + kArm64RegZero;
    static constexpr uint8_t kArm64ValueRegV0 = 32;

    size_t length;
    // Inidicates this is a load (or conversely a store).
    bool is_load;
    // Indicates the memory must be swapped.
    bool byte_swap;
    // Source (for store) or target (for load) register.
    // For x86-64:
    // AX  CX  DX  BX  SP  BP  SI  DI   // REX.R=0
    // R8  R9  R10 R11 R12 R13 R14 R15  // REX.R=1
    // For AArch64:
    // - kArm64ValueRegX0 + [0...30]: Xn (Wn for 32 bits - upper 32 bits of Xn
    //   are zeroed on Wn write).
    // - kArm64ValueRegZero: Zero constant for register read, ignored register
    //   write (though memory must still be accessed - a MMIO load may have side
    //   effects even if the result is discarded).
    // - kArm64ValueRegV0 + [0...31]: Vn (Sn for 32 bits).
    uint8_t value_reg;
    // [base + (index * scale) + displacement]
    bool mem_has_base;
    // On AArch64, if mem_base_reg is kArm64MemBaseRegSp, the base register is
    // SP, not Xn.
    uint8_t mem_base_reg;
    // For AArch64 pre- and post-indexing. In case of a load, the base register
    // is written back after the loaded data is written to the register,
    // overwriting the value register if it's the same.
    bool mem_base_writeback;
    int32_t mem_base_writeback_offset;
    bool mem_has_index;
    uint8_t mem_index_reg;
    uint8_t mem_index_size;
    bool mem_index_sign_extend;
    uint8_t mem_scale;
    ptrdiff_t mem_displacement;
    bool is_constant;
    int32_t constant;
  };

  static bool TryDecodeLoadStore(const uint8_t* p, DecodedLoadStore& decoded_out);
};

}  // namespace rex::runtime
