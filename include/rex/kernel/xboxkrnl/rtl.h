/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <rex/system/xtypes.h>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, rtl_allow_cross_thread_critical_section_leave);

namespace rex::kernel::xboxkrnl {

struct X_RTL_CRITICAL_SECTION;

constexpr bool IsCriticalSectionLeaveAllowed(uint32_t owning_thread, uint32_t current_thread,
                                             bool allow_cross_thread_leave) {
  return owning_thread == current_thread || allow_cross_thread_leave;
}

void xeRtlInitializeCriticalSection(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr);
X_STATUS xeRtlInitializeCriticalSectionAndSpinCount(X_RTL_CRITICAL_SECTION* cs, uint32_t cs_ptr,
                                                    uint32_t spin_count);

}  // namespace rex::kernel::xboxkrnl
