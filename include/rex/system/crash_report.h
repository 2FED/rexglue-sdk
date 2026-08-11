/**
 * Privacy-safe diagnostic reports for failures while executing guest code.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rex::runtime {
class ThreadState;
}

namespace rex::diagnostics {

struct HostStackFrame {
  uintptr_t address = 0;
  uintptr_t module_offset = 0;
  std::string module;
};

struct GuestCrashReport {
  static constexpr uint32_t kSchemaVersion = 1;
  static constexpr size_t kMaxHostFrames = 16;

  std::string reason;
  uint32_t guest_pc = 0;
  uint32_t current_function = 0;
  uint32_t thread_id = 0xFFFFFFFFu;
  std::string last_import;
  std::vector<HostStackFrame> host_stack;
  bool guest_memory_included = false;
};

GuestCrashReport CaptureGuestCrashReport(std::string_view reason,
                                         runtime::ThreadState* thread_state,
                                         uint32_t frames_to_skip = 0);
std::string FormatGuestCrashReport(const GuestCrashReport& report);

}  // namespace rex::diagnostics
