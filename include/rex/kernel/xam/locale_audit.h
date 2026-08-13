/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe locale selection audit                      *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, xam_locale_audit);

namespace rex::kernel::xam {

uint32_t NormalizeUserLanguage(uint32_t language);
bool IsUserCountryValid(uint32_t country);

void RecordXGetLanguage(uint32_t result);
void RecordLocaleXConfig(uint16_t category, uint16_t setting, uint32_t result, bool value_present,
                         uint32_t value);
void EmitLocaleAuditSummary(const char* phase);

}  // namespace rex::kernel::xam
