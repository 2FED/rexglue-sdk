#pragma once

#include <cstdint>

#include <rex/graphics/util/livery_diag2.h>

namespace rex::graphics {

// d3d12_readback_sync_composite (MCLA KI-047 fix candidate). The police livery
// is composited at EDRAM base 0 as a 1024x1024 k_8_8_8_8 image, resolved into
// a re-used scratch range together with its 512..8 mip chain (two read-back
// keys of different length start at the same address), and the CPU builds the
// atlas of the car from that guest memory. Under readback_resolve=some a
// resolve to a key that is still in the read-back cache is copied to guest
// memory only every 4th time, and then from the previous slot, so a composite
// can leave the content of the previous car (or of another mip) in guest
// memory.
//
// With the switch on, in the delayed modes (fast, some) a read-back resolve
// that matches any of these rules and is not a read-back cache miss (misses
// already wait and copy their own slot) is copied synchronously and fenced in
// the same frame, exactly as readback_resolve=full does it:
// - shared range (rule a): the destination overlaps another live read-back
//   key of a different length;
// - sporadic (rule b): the previous resolve of the same key was 8 or more
//   frames ago;
// - composite shape (rule c): a colour resolve from EDRAM base 0 of a square
//   power-of-two size from 64x64 to 1024x1024.
// Every other resolve keeps the delayed behaviour, so per-frame tiles stay
// cheap.
//
// Scope (d3d12_readback_sync_scope). The l54 cold 1440p log shows about
// 48,000 resolves per minute (about 21 per frame) overlapping another live
// key of a different length (scene, shadow and bloom scratch re-use), about
// 1,500 per minute with the previous same-key resolve 8 or more frames ago
// (k_8 128x128/256x256 tiles from EDRAM base 224 every 49 frames), and a
// per-frame k_32_FLOAT 128x128/64x64 chain from EDRAM base 0. Forcing a
// full queue wait for all of them would cost about what readback_resolve=full
// costs. So the default scope 0 ("composite") applies the rules only to the
// family of the livery composite: colour resolves to a k_8_8_8_8 destination,
// and rules a and c only from EDRAM base 0. Scope 1 ("all") applies them to
// every resolve as literally stated above (rule c then requires 32 bits per
// pixel instead of k_8_8_8_8). The rules are pure and unit-tested here.

constexpr uint64_t kReadbackSyncSporadicMinFrames = 8;
constexpr uint32_t kReadbackSyncCompositeMinSize = 64;
constexpr uint32_t kReadbackSyncCompositeMaxSize = 1024;
// xenos::ColorFormat::k_8_8_8_8 (copy destination format of the composite).
constexpr uint32_t kReadbackSyncFormat8888 = 6;

enum class ReadbackSyncScope : uint32_t {
  kComposite = 0,
  kAll = 1,
};

inline ReadbackSyncScope NormalizeReadbackSyncScope(int32_t value) {
  return value == 1 ? ReadbackSyncScope::kAll : ReadbackSyncScope::kComposite;
}

inline const char* ReadbackSyncScopeName(ReadbackSyncScope scope) {
  return scope == ReadbackSyncScope::kAll ? "all" : "composite";
}

// One read-back resolve as the rules see it.
struct ReadbackSyncResolve {
  bool source_is_color = false;
  uint32_t source_edram_base = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // xenos::ColorFormat of the copy destination and its bits per pixel.
  uint32_t dest_format = 0;
  uint32_t bits_per_pixel = 0;
  uint32_t address = 0;
  uint32_t length = 0;
  // The key (address, length) has a live read-back cache entry, last
  // resolved in `previous_frame`.
  bool key_live = false;
  uint64_t previous_frame = 0;
  uint64_t frame = 0;
};

inline bool IsSquarePowerOfTwoInRange(uint32_t width, uint32_t height, uint32_t min_size,
                                      uint32_t max_size) {
  return width == height && width >= min_size && width <= max_size && (width & (width - 1)) == 0;
}

// Colour resolve to a k_8_8_8_8 destination (scope "composite": rules a, b
// and c; from EDRAM base 0 for rules a and c).
inline bool IsReadbackSyncComposite8888(const ReadbackSyncResolve& r) {
  return r.source_is_color && r.dest_format == kReadbackSyncFormat8888;
}

// Rule b.
inline bool IsReadbackSyncSporadic(const ReadbackSyncResolve& r, ReadbackSyncScope scope) {
  if (!r.key_live) {
    // A new key is a read-back cache miss, which already waits.
    return false;
  }
  if (scope == ReadbackSyncScope::kComposite && !IsReadbackSyncComposite8888(r)) {
    return false;
  }
  return r.frame < r.previous_frame || r.frame - r.previous_frame >= kReadbackSyncSporadicMinFrames;
}

// Rule c.
inline bool IsReadbackSyncCompositeShape(const ReadbackSyncResolve& r, ReadbackSyncScope scope) {
  if (!r.source_is_color || r.source_edram_base != 0) {
    return false;
  }
  if (scope == ReadbackSyncScope::kComposite ? r.dest_format != kReadbackSyncFormat8888
                                             : r.bits_per_pixel != 32) {
    return false;
  }
  return IsSquarePowerOfTwoInRange(r.width, r.height, kReadbackSyncCompositeMinSize,
                                   kReadbackSyncCompositeMaxSize);
}

// Whether rule a is evaluated for this resolve (it needs a scan of the live
// keys).
inline bool ReadbackSyncSharedRangeApplies(const ReadbackSyncResolve& r, ReadbackSyncScope scope) {
  return scope == ReadbackSyncScope::kAll ||
         (IsReadbackSyncComposite8888(r) && r.source_edram_base == 0);
}

// Rule a, for one other live key.
inline bool IsReadbackSyncSharedRange(uint32_t address, uint32_t length, uint32_t other_address,
                                      uint32_t other_length) {
  return other_length != length && GuestRangesOverlap(address, length, other_address, other_length);
}

struct ReadbackSyncRules {
  bool shared_range = false;
  bool sporadic = false;
  bool composite_shape = false;
  // The most recently used live key that made shared_range true.
  uint64_t other_key = 0;
  uint64_t other_last_used_frame = 0;
  uint32_t other_keys = 0;

  bool any() const { return shared_range || sporadic || composite_shape; }
  // Rule a or c: the composite-range diagnostic lines.
  bool composite() const { return shared_range || composite_shape; }
};

// `live_keys` iterates (key, entry) pairs with entry.last_used_frame, keys as
// MakeReadbackResolveKey ((address << 32) | length); `self_key` is skipped.
template <typename LiveKeys>
ReadbackSyncRules EvaluateReadbackSyncRules(const ReadbackSyncResolve& r, ReadbackSyncScope scope,
                                            const LiveKeys& live_keys, uint64_t self_key) {
  ReadbackSyncRules rules;
  rules.sporadic = IsReadbackSyncSporadic(r, scope);
  rules.composite_shape = IsReadbackSyncCompositeShape(r, scope);
  if (ReadbackSyncSharedRangeApplies(r, scope)) {
    for (const auto& [other_key, other] : live_keys) {
      if (other_key == self_key ||
          !IsReadbackSyncSharedRange(r.address, r.length, ReadbackKeyAddress(other_key),
                                     ReadbackKeyLength(other_key))) {
        continue;
      }
      if (!rules.other_keys || other.last_used_frame > rules.other_last_used_frame) {
        rules.other_key = other_key;
        rules.other_last_used_frame = other.last_used_frame;
      }
      ++rules.other_keys;
    }
    rules.shared_range = rules.other_keys != 0;
  }
  return rules;
}

// Forced only in the delayed modes (full already waits, none does not read
// back) and not for cache misses (they already wait and read their own slot).
inline bool ShouldForceReadbackSync(bool enabled, bool delayed_mode, bool cache_miss,
                                    const ReadbackSyncRules& rules) {
  return enabled && delayed_mode && !cache_miss && rules.any();
}

inline const char* ReadbackSyncRuleNames(const ReadbackSyncRules& rules) {
  static constexpr const char* kNames[8] = {"none", "a", "b", "ab", "c", "ac", "bc", "abc"};
  return kNames[(rules.shared_range ? 1 : 0) | (rules.sporadic ? 2 : 0) |
                (rules.composite_shape ? 4 : 0)];
}

// Per-minute counters of the MCLA_READBACK_SYNC line. A resolve matching
// several rules counts once in `matched`/`forced` and once per matching rule.
struct ReadbackSyncCounters {
  // Delayed-mode resolves the rules were evaluated for.
  uint64_t resolves = 0;
  // Of those, read-back cache misses (already synchronous).
  uint64_t misses = 0;
  // Non-miss resolves matching a rule (forced or not: with the switch off
  // these are the resolves that would have been forced).
  uint64_t matched = 0;
  uint64_t matched_rule_a = 0;
  uint64_t matched_rule_b = 0;
  uint64_t matched_rule_c = 0;
  uint64_t forced = 0;
  uint64_t forced_rule_a = 0;
  uint64_t forced_rule_b = 0;
  uint64_t forced_rule_c = 0;
  uint64_t forced_wait_failed = 0;
  uint64_t wait_us = 0;
  uint64_t max_wait_us = 0;

  void CountMatched(const ReadbackSyncRules& rules) {
    ++matched;
    matched_rule_a += rules.shared_range ? 1 : 0;
    matched_rule_b += rules.sporadic ? 1 : 0;
    matched_rule_c += rules.composite_shape ? 1 : 0;
  }

  void CountForced(const ReadbackSyncRules& rules, uint64_t elapsed_us) {
    ++forced;
    forced_rule_a += rules.shared_range ? 1 : 0;
    forced_rule_b += rules.sporadic ? 1 : 0;
    forced_rule_c += rules.composite_shape ? 1 : 0;
    wait_us += elapsed_us;
    max_wait_us = elapsed_us > max_wait_us ? elapsed_us : max_wait_us;
  }
};

}  // namespace rex::graphics
