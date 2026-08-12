/**
 ******************************************************************************
 * ReXGlue : Xbox 360 recompilation runtime                                  *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#pragma once

namespace rex::kernel::xam {

// Emits one bounded snapshot of the opt-in XAM user/profile audit. Repeated
// calls are ignored so a project may safely request the snapshot from a
// capture/checkpoint path that can be entered more than once.
void EmitXamProfileAuditSummary(const char* phase);

}  // namespace rex::kernel::xam
