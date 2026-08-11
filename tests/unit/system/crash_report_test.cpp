/**
 * Regression coverage for privacy-safe guest crash diagnostics.
 */

#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include <rex/ppc/context.h>
#include <rex/system/crash_report.h>
#include <rex/system/thread_state.h>

using rex::diagnostics::CaptureGuestCrashReport;
using rex::diagnostics::FormatGuestCrashReport;
using rex::diagnostics::GuestCrashReport;
using rex::diagnostics::HostStackFrame;

TEST_CASE("Guest function breadcrumbs restore normally and retain the faulting scope",
          "[system][crash-report]") {
  PPCContext context{};
  {
    rex::ppc::GuestFunctionScope outer(context, 0x82001000);
    CHECK(context.current_function == 0x82001000);
    {
      rex::ppc::GuestFunctionScope inner(context, 0x82002000);
      rex::ppc::SetGuestProgramCounter(context, 0x82002040);
      CHECK(context.current_function == 0x82002000);
      CHECK(context.guest_pc == 0x82002040);
    }
    CHECK(context.current_function == 0x82001000);
    CHECK(context.guest_pc == 0x82001000);
  }
  CHECK(context.current_function == 0);
  CHECK(context.guest_pc == 0);

  try {
    rex::ppc::GuestFunctionScope outer(context, 0x82003000);
    rex::ppc::GuestFunctionScope inner(context, 0x82004000);
    rex::ppc::SetGuestProgramCounter(context, 0x82004080);
    throw std::runtime_error("synthetic");
  } catch (const std::runtime_error&) {}
  CHECK(context.current_function == 0x82004000);
  CHECK(context.guest_pc == 0x82004080);
}

TEST_CASE("Guest import breadcrumb persists without guest memory", "[system][crash-report]") {
  PPCContext context{};
  rex::ppc::RecordGuestImport(context, "__imp__XGetAVPack");
  CHECK(std::string_view(context.last_import) == "__imp__XGetAVPack");
}

TEST_CASE("Crash report formatter emits required metadata and no guest dump",
          "[system][crash-report]") {
  GuestCrashReport report;
  report.reason = "synthetic\nreason";
  report.guest_pc = 0x821322BC;
  report.current_function = 0x821322B8;
  report.thread_id = 0x4D434C41;
  report.last_import = "__imp__XGetAVPack";
  report.host_stack.push_back(HostStackFrame{0x7FF612341234, 0x1234, "mcla.exe"});

  const std::string text = FormatGuestCrashReport(report);
  CHECK(text.find("REX_GUEST_CRASH schema=1") != std::string::npos);
  CHECK(text.find("reason=synthetic?reason") != std::string::npos);
  CHECK(text.find("guest_pc=0x821322BC") != std::string::npos);
  CHECK(text.find("ppc_function=sub_821322B8") != std::string::npos);
  CHECK(text.find("thread_id=0x4D434C41") != std::string::npos);
  CHECK(text.find("last_import=__imp__XGetAVPack") != std::string::npos);
  CHECK(text.find("host_stack[0]=mcla.exe+0x1234") != std::string::npos);
  CHECK(text.find("guest_memory_included=false") != std::string::npos);
  CHECK(text.find("guest_stack") == std::string::npos);
  CHECK(text.find("memory_dump") == std::string::npos);
}

TEST_CASE("Crash capture reads bounded thread breadcrumbs and a host stack",
          "[system][crash-report]") {
  rex::runtime::ThreadState thread_state(0x42, 0, 0, nullptr);
  auto* context = thread_state.context();
  context->guest_pc = 0x82005020;
  context->current_function = 0x82005000;
  context->last_import = "__imp__KeQuerySystemTime";

  const auto report = CaptureGuestCrashReport("capture", &thread_state);
  CHECK(report.thread_id == 0x42);
  CHECK(report.guest_pc == 0x82005020);
  CHECK(report.current_function == 0x82005000);
  CHECK(report.last_import == "__imp__KeQuerySystemTime");
  CHECK_FALSE(report.host_stack.empty());
  CHECK(report.host_stack.size() <= GuestCrashReport::kMaxHostFrames);
  CHECK_FALSE(report.guest_memory_included);
}
