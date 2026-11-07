#pragma once

#include <cstdint>

// M11-007c (KI-047 / M11-007b): host-visible read-back range queries.
//
// The guest's own synchronisation for a CPU read of a GPU-written texture is
// the D3D resource fence plus the lock core (sub_82421CA0 in MCLA). On the
// host the fence word is stored when the command processor parses the
// EVENT_WRITE_SHD packet, not when the host GPU has finished the read-back
// copy, so a lock of a read-back key whose copy is still in flight (a
// delayed-mode resolve: exempt steady key or the some cadence) returns
// before the bytes are in guest RAM. A host hook on the lock core can call
// ReadbackSyncEnsureRange for the locked range after the guest fence wait
// and before the original lock body.
//
// Exported from rexruntime (like rex::graphics::MarkPerfSplit); the GPU
// plugin (rexgpu-xenos, D3D12 command processor) registers the provider at
// initialisation and removes it at shutdown. Without a provider both calls
// report nothing and return at once.
//
// Thread safety: callable from any thread, including guest threads that
// hold the game's own locks. The command processor never waits for a
// caller: the provider's table lock is held by either side only for a
// table update or one memcpy of a read-back slot (no wait inside it), and a
// caller only waits on the host GPU fence, bounded by its timeout. See
// D3D12CommandProcessor::ReadbackRangeBridge for the deadlock analysis.

namespace rex::gpu {

struct ReadbackRangeInfo {
  // The overlapping read-back key (the most recently resolved one when
  // several overlap): guest physical base and length of the resolve, and
  // the size of its read-back slot buffers.
  uint32_t key_base = 0;
  uint32_t key_length = 0;
  uint32_t slot_size = 0;
  // The key's last resolve: width, height and copy destination format.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format = 0;
  // Its last resolve was steady-exempt (d3d12_readback_sync_steady_exempt).
  bool exempt = false;
  // A recorded GPU copy of the key is not yet in guest RAM, and whether its
  // submission is still open on the command processor (not yet executed).
  bool pending = false;
  bool unsubmitted = false;
  // Frames since the resolve whose bytes guest RAM holds (UINT32_MAX: none
  // delivered since the key was created), and since the key's last resolve.
  uint32_t copy_age_frames = UINT32_MAX;
  uint32_t resolve_age_frames = 0;
};

struct ReadbackSyncEnsureResult {
  // Time spent waiting for the host GPU (milliseconds, rounded up, and
  // microseconds).
  uint32_t waited_ms = 0;
  uint32_t waited_us = 0;
  // An overlapping key had a pending copy at the call.
  bool pending = false;
  // Guest RAM now holds that copy (delivered by this call or, meanwhile, by
  // the command processor).
  bool delivered = false;
  // The copy did not complete within timeout_ms (nothing was copied).
  bool timed_out = false;
};

// Whether [guest_phys, guest_phys + len) overlaps a live read-back key;
// fills `out` (may be nullptr) for it. Takes the provider's table lock
// briefly; never waits.
bool ReadbackSyncQueryRange(uint32_t guest_phys, uint32_t len, ReadbackRangeInfo* out);

// If an overlapping key has a pending copy: when its submission is still
// open, asks the command processor to execute it at its next opportunity
// (a flag; no command processor work runs on the calling thread), waits at
// most timeout_ms on the copy's submission fence, then copies the slot into
// guest RAM (the same memcpy as the fast delivery). Overlapping keys are
// handled newest first within the one timeout.
ReadbackSyncEnsureResult ReadbackSyncEnsureRange(uint32_t guest_phys, uint32_t len,
                                                 uint32_t timeout_ms);

// The provider interface (implemented by the GPU plugin).
class IReadbackRangeProvider {
 public:
  virtual ~IReadbackRangeProvider() = default;
  virtual bool QueryRange(uint32_t guest_phys, uint32_t len, ReadbackRangeInfo* out) = 0;
  virtual ReadbackSyncEnsureResult EnsureRange(uint32_t guest_phys, uint32_t len,
                                               uint32_t timeout_ms) = 0;
};

// Registers (or, with nullptr, removes) the provider. Removing waits until
// no call is inside the provider (at most one caller timeout).
void SetReadbackRangeProvider(IReadbackRangeProvider* provider);

}  // namespace rex::gpu
