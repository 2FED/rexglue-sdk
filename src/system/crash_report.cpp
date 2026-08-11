/**
 * Privacy-safe diagnostic reports for failures while executing guest code.
 */

#include <rex/system/crash_report.h>

#include <algorithm>
#include <filesystem>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#endif

namespace rex::diagnostics {
namespace {

std::string SanitizeField(std::string_view value) {
  constexpr size_t kMaxFieldLength = 160;
  std::string result;
  result.reserve(std::min(value.size(), kMaxFieldLength));
  for (char ch : value.substr(0, kMaxFieldLength)) {
    const auto byte = static_cast<unsigned char>(ch);
    result.push_back(byte < 0x20 || byte == 0x7F ? '?' : ch);
  }
  return result;
}

HostStackFrame ResolveFrame(uintptr_t address) {
  HostStackFrame frame;
  frame.address = address;
#if REX_PLATFORM_WIN32
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module)) {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)))) {
      frame.module = rex::path_to_utf8(std::filesystem::path(path).filename());
    }
    frame.module_offset = address - reinterpret_cast<uintptr_t>(module);
  }
#else
  Dl_info info = {};
  if (dladdr(reinterpret_cast<void*>(address), &info) && info.dli_fbase) {
    frame.module = std::filesystem::path(info.dli_fname ? info.dli_fname : "").filename().string();
    frame.module_offset = address - reinterpret_cast<uintptr_t>(info.dli_fbase);
  }
#endif
  if (frame.module.empty()) {
    frame.module = "unknown";
    frame.module_offset = address;
  }
  return frame;
}

std::vector<HostStackFrame> CaptureHostStack(uint32_t frames_to_skip) {
  std::vector<HostStackFrame> result;
  result.reserve(GuestCrashReport::kMaxHostFrames);
#if REX_PLATFORM_WIN32
  void* frames[GuestCrashReport::kMaxHostFrames] = {};
  const USHORT count = CaptureStackBackTrace(
      frames_to_skip + 1, static_cast<DWORD>(std::size(frames)), frames, nullptr);
  for (USHORT i = 0; i < count; ++i) {
    result.push_back(ResolveFrame(reinterpret_cast<uintptr_t>(frames[i])));
  }
#else
  void* frames[GuestCrashReport::kMaxHostFrames + 8] = {};
  const int count = backtrace(frames, static_cast<int>(std::size(frames)));
  const int first = std::min(count, static_cast<int>(frames_to_skip + 1));
  for (int i = first; i < count && result.size() < GuestCrashReport::kMaxHostFrames; ++i) {
    result.push_back(ResolveFrame(reinterpret_cast<uintptr_t>(frames[i])));
  }
#endif
  return result;
}

}  // namespace

GuestCrashReport CaptureGuestCrashReport(std::string_view reason,
                                         runtime::ThreadState* thread_state,
                                         uint32_t frames_to_skip) {
  GuestCrashReport report;
  report.reason = SanitizeField(reason);
  if (thread_state) {
    report.thread_id = thread_state->thread_id();
    if (const auto* context = thread_state->context()) {
      report.guest_pc = context->guest_pc;
      report.current_function = context->current_function;
      report.last_import = context->last_import ? SanitizeField(context->last_import) : "none";
    }
  }
  if (report.last_import.empty()) {
    report.last_import = "none";
  }
  report.host_stack = CaptureHostStack(frames_to_skip + 1);
  return report;
}

std::string FormatGuestCrashReport(const GuestCrashReport& report) {
  std::string output = fmt::format(
      "REX_GUEST_CRASH schema={}\n"
      "reason={}\n"
      "guest_pc=0x{:08X}\n"
      "ppc_function=sub_{:08X}\n"
      "thread_id=0x{:08X}\n"
      "last_import={}\n"
      "host_stack_count={}\n",
      GuestCrashReport::kSchemaVersion, SanitizeField(report.reason), report.guest_pc,
      report.current_function, report.thread_id, SanitizeField(report.last_import),
      report.host_stack.size());
  for (size_t i = 0; i < report.host_stack.size(); ++i) {
    const auto& frame = report.host_stack[i];
    fmt::format_to(std::back_inserter(output), "host_stack[{}]={}+0x{:X} (0x{:X})\n", i,
                   SanitizeField(frame.module), frame.module_offset, frame.address);
  }
  fmt::format_to(std::back_inserter(output), "guest_memory_included={}\n",
                 report.guest_memory_included ? "true" : "false");
  return output;
}

}  // namespace rex::diagnostics
