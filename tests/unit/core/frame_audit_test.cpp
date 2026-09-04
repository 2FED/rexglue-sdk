/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/perf/frame_audit.h>

TEST_CASE("Performance telemetry classifies streaming stalls at the boundary",
          "[perf][frame-audit]") {
  CHECK_FALSE(rex::perf::IsFrameAuditStreamingStall(0));
  CHECK_FALSE(rex::perf::IsFrameAuditStreamingStall(4999));
  CHECK(rex::perf::IsFrameAuditStreamingStall(5000));
  CHECK(rex::perf::IsFrameAuditStreamingStall(5001));
}

TEST_CASE("Performance telemetry converts D3D12 ticks without host floating point",
          "[perf][frame-audit]") {
  CHECK(rex::perf::ConvertGpuTicksToMicroseconds(0, 10000000) == 0);
  CHECK(rex::perf::ConvertGpuTicksToMicroseconds(100000, 10000000) == 10000);
  CHECK(rex::perf::ConvertGpuTicksToMicroseconds(12345, 0) == 0);
}

TEST_CASE("Performance telemetry diagnostic trigger requires timing and cannot reset",
          "[perf][frame-audit]") {
  rex::perf::SetFrameAuditGpuTimingAvailable(false);
  CHECK_FALSE(rex::perf::ArmFrameAuditForDiagnostics());
  rex::perf::SetFrameAuditGpuTimingAvailable(true);
  CHECK(rex::perf::ArmFrameAuditForDiagnostics());
  CHECK(rex::perf::IsFrameAuditEnabled());
  CHECK_FALSE(rex::perf::ArmFrameAuditForDiagnostics());
}
