#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <rex/types.h>

namespace rex::graphics {

// M11-016 (d3d12_fetch_identity_cache): fetch-constant identity cache of the
// D3D12 command processor. Pure building blocks (no globals, no OS or D3D12
// calls), unit-tested in tests/unit/graphics/fetch_identity_test.cpp.
//
// 1. Compare-before-invalidate (StoreFetchDwordsCompare). The guest re-sends
//    whole fetch-constant blocks per draw; before M11-016 every fetch-constant
//    register write cleared the texture-binding in-sync bits, the vertex
//    buffer residency bits and the fetch constant buffer, even when the value
//    was identical. Everything those bits cache is a pure function of the
//    register values (the binding decode reads the register file, the fetch
//    constant buffer is a copy of it, the vertex range is decoded from it), so
//    a write that leaves the stored value unchanged leaves every cached result
//    valid. Only the dwords that actually change are invalidated.
//
// 2. Raw-fetch memo (FetchIdentityMemo). A direct-mapped table keyed by the 6
//    raw dwords of a texture fetch constant, holding the decoded binding and
//    the resolved textures. An entry is valid only while the texture cache's
//    generation counter equals the generation stamped into it; the texture
//    cache bumps the generation whenever the key -> texture mapping could
//    change (texture creation and destruction, scaled-resolve page bits
//    changing). An entry is stamped with the generation read before the
//    lookup that produced it, so a bump that races with the lookup makes the
//    entry stale at once (conservative).
//
// 3. Sampler memo (SamplerMemoSlot). Sampler parameters are a pure function of
//    the 6 fetch dwords, the shader's sampler binding fields and the
//    anisotropic_override cvar; one slot per sampler binding position keeps
//    the last inputs and result.

// Cumulative counters (CP thread, plain integers). The command processor and
// the texture cache each own one; the perf split sums them.
struct FetchIdentityCounters {
  // Fetch-constant dwords written by PM4 and how many of them did not change
  // the stored value.
  uint64_t fetch_dw_written = 0;
  uint64_t fetch_dw_unchanged = 0;
  // Texture slots decoded in RequestTextures (not in sync) and how many of
  // them changed their texture key.
  uint64_t tex_slots = 0;
  uint64_t tex_key_changed = 0;
  // Raw-fetch memo lookups.
  uint64_t tex_memo_hit = 0;
  uint64_t tex_memo_miss = 0;
  // d3d12_fetch_identity_verify: memo hits whose slow-path result differed.
  uint64_t tex_memo_mismatch = 0;
  // Sampler memo lookups.
  uint64_t smp_memo_hit = 0;
  uint64_t smp_memo_miss = 0;

  FetchIdentityCounters& operator+=(const FetchIdentityCounters& other) {
    fetch_dw_written += other.fetch_dw_written;
    fetch_dw_unchanged += other.fetch_dw_unchanged;
    tex_slots += other.tex_slots;
    tex_key_changed += other.tex_key_changed;
    tex_memo_hit += other.tex_memo_hit;
    tex_memo_miss += other.tex_memo_miss;
    tex_memo_mismatch += other.tex_memo_mismatch;
    smp_memo_hit += other.smp_memo_hit;
    smp_memo_miss += other.smp_memo_miss;
    return *this;
  }
};

// Result of storing a run of fetch-constant dwords: the relative indices of
// the first and last dword whose value changed (only meaningful if changed).
struct FetchDwordStoreResult {
  bool changed = false;
  uint32_t first_changed = 0;
  uint32_t last_changed = 0;
  uint32_t unchanged = 0;
};

// Stores `count` big-endian dwords from `source_big_endian` into `dest`
// (host-endian register file values) and reports which ones changed.
inline FetchDwordStoreResult StoreFetchDwordsCompare(uint32_t* dest,
                                                     const uint32_t* source_big_endian,
                                                     uint32_t count) {
  FetchDwordStoreResult result;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t value = rex::byte_swap(source_big_endian[i]);
    if (dest[i] == value) {
      ++result.unchanged;
      continue;
    }
    dest[i] = value;
    if (!result.changed) {
      result.changed = true;
      result.first_changed = i;
    }
    result.last_changed = i;
  }
  return result;
}

// Texture-cache generation: bumped (from any thread) whenever the mapping from
// a fetch constant to a texture may change. Readers compare a stamp.
class FetchIdentityGeneration {
 public:
  uint64_t Current() const { return value_.load(std::memory_order_acquire); }
  void Bump() { value_.fetch_add(1, std::memory_order_acq_rel); }

 private:
  // Starts at 1 so that a zero-initialised memo entry is never valid.
  std::atomic<uint64_t> value_{1};
};

inline constexpr size_t kFetchDwordCount = 6;
using FetchDwords = std::array<uint32_t, kFetchDwordCount>;

inline FetchDwords LoadFetchDwords(const uint32_t* values) {
  FetchDwords dwords;
  std::memcpy(dwords.data(), values, sizeof(dwords));
  return dwords;
}

inline uint32_t HashFetchDwords(const FetchDwords& dwords) {
  // Multiplicative mix; dword 1 (base address) and dword 2 (size) vary most.
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (uint32_t dword : dwords) {
    h = (h ^ dword) * 0xFF51AFD7ED558CCDull;
  }
  h ^= h >> 32;
  return uint32_t(h);
}

// Direct-mapped memo from 6 raw fetch dwords to a payload, valid while the
// generation stamp matches.
template <typename Payload, uint32_t kLog2Entries = 9>
class FetchIdentityMemo {
 public:
  static constexpr uint32_t kEntryCount = UINT32_C(1) << kLog2Entries;

  // Returns the payload when an entry with these dwords was stored with the
  // current generation, else nullptr.
  const Payload* Find(const FetchDwords& dwords, uint64_t generation) const {
    const Entry& entry = entries_[Index(dwords)];
    if (entry.generation != generation || entry.dwords != dwords) {
      return nullptr;
    }
    return &entry.payload;
  }

  // `generation` must be the generation read before the work that produced
  // the payload started.
  void Store(const FetchDwords& dwords, uint64_t generation, const Payload& payload) {
    Entry& entry = entries_[Index(dwords)];
    entry.dwords = dwords;
    entry.generation = generation;
    entry.payload = payload;
  }

  void Clear() {
    for (Entry& entry : entries_) {
      entry.generation = 0;
    }
  }

  static uint32_t Index(const FetchDwords& dwords) {
    return HashFetchDwords(dwords) >> (32 - kLog2Entries);
  }

 private:
  struct Entry {
    FetchDwords dwords{};
    uint64_t generation = 0;
    Payload payload{};
  };
  std::array<Entry, kEntryCount> entries_{};
};

// Packs the shader-side sampler binding fields (fetch constant index and the
// filter overrides) into one word for the sampler memo.
constexpr uint32_t PackSamplerBindingWord(uint32_t fetch_constant, uint32_t mag_filter,
                                          uint32_t min_filter, uint32_t mip_filter,
                                          uint32_t aniso_filter) {
  return (fetch_constant & 0xFF) | ((mag_filter & 0xF) << 8) | ((min_filter & 0xF) << 12) |
         ((mip_filter & 0xF) << 16) | ((aniso_filter & 0xF) << 20) | (UINT32_C(1) << 31);
}

// Last inputs and result of one sampler binding position.
struct SamplerMemoSlot {
  FetchDwords dwords{};
  uint32_t binding_word = 0;  // 0 = empty (a packed word always has bit 31)
  int32_t anisotropic_override = 0;
  uint32_t result = 0;

  bool Matches(const FetchDwords& fetch, uint32_t binding, int32_t aniso) const {
    return binding_word == binding && anisotropic_override == aniso && dwords == fetch;
  }
  void Store(const FetchDwords& fetch, uint32_t binding, int32_t aniso, uint32_t value) {
    dwords = fetch;
    binding_word = binding;
    anisotropic_override = aniso;
    result = value;
  }
};

}  // namespace rex::graphics
