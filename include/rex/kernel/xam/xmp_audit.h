/**
 ******************************************************************************
 * ReXGlue - bounded privacy-safe XMP route audit                             *
 ******************************************************************************
 */

#pragma once

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, xmp_route_audit);

namespace rex::kernel::xam {

struct XmpFallbackState {
  uint32_t state = 0;
  uint32_t playback_client = 1;
  uint32_t playlist_count = 0;
  uint32_t active_song_count = 0;
  bool decoder_available = false;
};

bool IsXmpKnownMessage(uint32_t message);
bool IsXmpPlaybackMessage(uint32_t message);
bool IsXmpQueryMessage(uint32_t message);
bool IsXmpStateConsistent(const XmpFallbackState& state);

bool BeginXmpRouteMessage();
void EndXmpRouteMessage(bool audited, uint32_t message, uint32_t result,
                        const XmpFallbackState& before, const XmpFallbackState& after);
void EmitXmpRouteAuditSummary(const char* phase);

}  // namespace rex::kernel::xam
