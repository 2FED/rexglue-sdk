/**
 * Regression coverage for deterministic guest-thread startup dispatch.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/system/xthread.h>

using rex::system::XThread;

TEST_CASE("Raw guest threads start at the requested routine with context in r3",
          "[system][thread][startup]") {
  XThread::CreationParams params{};
  params.start_address = 0x82001000;
  params.start_context = 0x82002000;

  const auto plan = XThread::BuildStartPlan(params);

  CHECK(plan.address == params.start_address);
  CHECK(plan.argument_count == 1);
  CHECK(plan.arguments[0] == params.start_context);
  CHECK(plan.use_return_value_as_exit_code);
}

TEST_CASE("XAPI guest threads start at the trampoline with routine and context arguments",
          "[system][thread][startup]") {
  XThread::CreationParams params{};
  params.xapi_thread_startup = 0x82003000;
  params.start_address = 0x82004000;
  params.start_context = 0x82005000;

  const auto plan = XThread::BuildStartPlan(params);

  CHECK(plan.address == params.xapi_thread_startup);
  CHECK(plan.argument_count == 2);
  CHECK(plan.arguments[0] == params.start_address);
  CHECK(plan.arguments[1] == params.start_context);
  CHECK_FALSE(plan.use_return_value_as_exit_code);
}

TEST_CASE("Guest thread startup retains the compatibility grace period",
          "[system][thread][startup]") {
  CHECK(XThread::kGuestThreadStartupDelay == std::chrono::milliseconds(10));
}
