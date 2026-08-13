/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe offline-service audit                       *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, xam_offline_service_audit);
REXCVAR_DECLARE(bool, xam_offline_network_block);

namespace rex::kernel::xam {

bool IsOfflineServiceKnownMessage(uint32_t message);
bool IsOfflineServiceLocalMessage(uint32_t message);
bool IsOfflineServiceExpectedResult(uint32_t message, uint32_t result);

bool BeginOfflineServiceMessage();
void EndOfflineServiceMessage(bool audited, uint32_t message, uint32_t result);

bool ShouldBlockOfflineGuestSocket();
void RecordOfflineGuestSocketAttempt(bool blocked);

void EmitOfflineServiceAuditSummary(const char* phase);

}  // namespace rex::kernel::xam
