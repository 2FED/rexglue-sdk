// M11-007c: host-visible read-back range queries (see
// include/rex/graphics/readback_range.h). Lives in rexcore so that it is
// exported from rexruntime and shared by the host and the GPU plugin.

#include <rex/graphics/readback_range.h>

#include <shared_mutex>

namespace rex::gpu {

namespace {
// Readers (Query/Ensure) hold it shared for the call; registration takes it
// exclusively, so a provider is never removed while a call is inside it.
std::shared_mutex g_provider_mutex;
IReadbackRangeProvider* g_provider = nullptr;
}  // namespace

bool ReadbackSyncQueryRange(uint32_t guest_phys, uint32_t len, ReadbackRangeInfo* out) {
  std::shared_lock<std::shared_mutex> lock(g_provider_mutex);
  if (!g_provider) {
    return false;
  }
  return g_provider->QueryRange(guest_phys, len, out);
}

ReadbackSyncEnsureResult ReadbackSyncEnsureRange(uint32_t guest_phys, uint32_t len,
                                                 uint32_t timeout_ms) {
  std::shared_lock<std::shared_mutex> lock(g_provider_mutex);
  if (!g_provider) {
    return ReadbackSyncEnsureResult();
  }
  return g_provider->EnsureRange(guest_phys, len, timeout_ms);
}

void SetReadbackRangeProvider(IReadbackRangeProvider* provider) {
  std::unique_lock<std::shared_mutex> lock(g_provider_mutex);
  g_provider = provider;
}

}  // namespace rex::gpu
