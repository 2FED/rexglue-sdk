#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

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
  // d3d12_readback_sync_steady_exempt: consecutive frames, this one
  // included, in which the key was resolved (NextReadbackConsecutiveFrames).
  uint32_t consecutive_frames = 0;
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
  // d3d12_readback_sync_steady_exempt: rule c matched, but the key is a
  // steady per-frame resolve (IsReadbackSyncSteadyExempt), so rule c does
  // not force it. The rule bits above stay set for the diagnostics.
  bool steady_exempt = false;

  bool any() const { return shared_range || sporadic || composite_shape; }
  // The rules that force the resolve (any() without an exempt rule c).
  bool forces() const { return shared_range || sporadic || (composite_shape && !steady_exempt); }
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
  return enabled && delayed_mode && !cache_miss && rules.forces();
}

// d3d12_readback_sync_steady_exempt (M11-007a). Rule c also matches a steady
// 256x256 k_8_8_8_8 resolve to 03702000 that the game issues in every frame
// and the GPU samples in the same frame (no known CPU reader). It forced
// about one full GPU drain per frame (6.3 ms PC, 7.1 ms Deck) although it
// was never forced before l55 and never tied to a defect. The livery
// composite keys match rules a or b and run at most 3 consecutive frames.
// With the switch on, rule c does not force a key that was resolved in each
// of the last kReadbackSyncSteadyMinFrames frames (this one included) and
// that matches neither rule a nor rule b now; such a resolve keeps the
// delayed behaviour of its mode. A gap of one frame resets the run, so a key
// re-used by a one-shot composite is forced again on its first resolve.
constexpr uint32_t kReadbackSyncSteadyMinFrames = 30;

// Consecutive frames, `frame` included, in which a key was resolved, from
// the key's previous state (a key without a live entry starts at 1; several
// resolves in one frame keep the run).
inline uint32_t NextReadbackConsecutiveFrames(bool key_live, uint64_t previous_frame,
                                              uint32_t previous_run, uint64_t frame) {
  if (!key_live || !previous_run) {
    return 1;
  }
  if (previous_frame == frame) {
    return previous_run;
  }
  if (previous_frame + 1 == frame) {
    return previous_run == UINT32_MAX ? previous_run : previous_run + 1;
  }
  return 1;
}

// Rule c only (neither a nor b) and a run of at least the minimum.
inline bool IsReadbackSyncSteadyExempt(const ReadbackSyncRules& rules, uint32_t run) {
  return rules.composite_shape && !rules.shared_range && !rules.sporadic &&
         run >= kReadbackSyncSteadyMinFrames;
}

// Why a key that was exempt is not exempt any more.
inline const char* ReadbackSyncSteadyEndReason(bool enabled, const ReadbackSyncRules& rules,
                                               uint32_t run) {
  if (!enabled) {
    return "disabled";
  }
  if (rules.shared_range) {
    return "rule_a";
  }
  if (rules.sporadic) {
    return "rule_b";
  }
  if (!rules.composite_shape) {
    return "not_c";
  }
  return run < kReadbackSyncSteadyMinFrames ? "gap" : "none";
}

// d3d12_readback_sync_steady_exempt filters (M11-007b). The l74 PC A/B showed
// that the player-car livery composite (a 512..64 k_8_8_8_8 mip chain laid
// out back to back after its 1024 level, a CPU-read composite) is resolved
// in every frame for 30-80+ frames in the pre-race showcase, so the run
// alone exempted it. A candidate (IsReadbackSyncSteadyExempt) is therefore
// exempt only if, in this order:
// - sticky: its key never matched rule a or rule b in this session (the
//   identity is address, length, format and size; the set is cleared only
//   when the read-back cache is torn down with the device);
// - shape: it has no mip-chain neighbour among the live read-back keys
//   (IsReadbackSyncChainNeighbour);
// - cap: no other key was a candidate that passed the two filters above in
//   this frame or in the previous frame (ReadbackSyncSteadyCap).
// Each refusal leaves the resolve forced by rule c, as without the switch.

// The shape of a read-back key as the filters see it (its last resolve).
struct ReadbackSyncKeyShape {
  uint32_t address = 0;
  uint32_t length = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dest_format = 0;
  uint32_t source_edram_base = 0;
};

// Whether `other` is a mip-chain neighbour of `key`. Only keys of the same
// destination format count. A neighbour is
// - any other key whose range overlaps the key's range (a larger level whose
//   chain starts at the same address: the 1024 + 512 livery layout), or,
//   with the same EDRAM source base,
// - the next SMALLER level after the key: it starts exactly at
//   key.address + key.length with half the width and half the height, or
// - the next LARGER level before the key: it ends exactly at key.address
//   with twice the width and twice the height.
// A chain is laid out from the largest level down (each level follows the
// previous, larger one), so a half-size key that ENDS at the key's address
// is not a level of the key's chain: in a chain the level before a 256x256
// level is 512x512. The symmetric definition ("half size and adjacent")
// would make the unrelated 128x128 key at 039BA000, which ends exactly at
// the steady 256x256 per-frame key at 039CA000 (PC l74), a neighbour and
// remove the whole gain; with the direction 039CA000 stays exempt, while
// every level of the livery chain 05368000 512x512 -> 05468000 256x256 ->
// 054A8000 128x128 -> 054B8000 64x64 has a neighbour (the next smaller level
// after it, the next larger one before it, or both) and is never exempt.
inline bool IsReadbackSyncChainNeighbour(const ReadbackSyncKeyShape& key,
                                         const ReadbackSyncKeyShape& other) {
  if (other.dest_format != key.dest_format) {
    return false;
  }
  if (other.address == key.address && other.length == key.length) {
    // The key itself.
    return false;
  }
  if (GuestRangesOverlap(key.address, key.length, other.address, other.length)) {
    return true;
  }
  if (other.source_edram_base != key.source_edram_base) {
    return false;
  }
  uint64_t key_end = uint64_t(key.address) + key.length;
  if (uint64_t(other.address) == key_end && uint64_t(other.width) * 2 == key.width &&
      uint64_t(other.height) * 2 == key.height) {
    return true;
  }
  uint64_t other_end = uint64_t(other.address) + other.length;
  return other_end == key.address && other.width == uint64_t(key.width) * 2 &&
         other.height == uint64_t(key.height) * 2;
}

// The shape filter over the known keys: `keys` iterates (key, entry) pairs
// whose entry has `shape_known` and `shape` (ReadbackSyncKeyShape).
template <typename Keys>
bool HasReadbackSyncChainNeighbour(const ReadbackSyncKeyShape& key, const Keys& keys) {
  for (const auto& entry : keys) {
    if (entry.second.shape_known && IsReadbackSyncChainNeighbour(key, entry.second.shape)) {
      return true;
    }
  }
  return false;
}

// Sticky identity of a key (address, length, format, size).
struct ReadbackSyncStickyKey {
  uint32_t address = 0;
  uint32_t length = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dest_format = 0;

  bool operator==(const ReadbackSyncStickyKey&) const = default;
};

struct ReadbackSyncStickyKeyHash {
  size_t operator()(const ReadbackSyncStickyKey& k) const {
    uint64_t h = (uint64_t(k.address) << 32) | k.length;
    h ^= (uint64_t(k.width) << 40) ^ (uint64_t(k.height) << 20) ^ uint64_t(k.dest_format);
    h *= 0x9E3779B97F4A7C15ull;
    return size_t(h ^ (h >> 29));
  }
};

inline ReadbackSyncStickyKey MakeReadbackSyncStickyKey(const ReadbackSyncKeyShape& s) {
  return ReadbackSyncStickyKey{s.address, s.length, s.width, s.height, s.dest_format};
}

// Keys that ever matched rule a or rule b. Bounded: a key that would grow it
// past kReadbackSyncStickyMaxKeys saturates it, and a saturated set excludes
// every key (no exemption at all is the conservative side).
constexpr size_t kReadbackSyncStickyMaxKeys = 4096;

class ReadbackSyncStickySet {
 public:
  // Records a resolve whose rules matched a or b.
  void Note(const ReadbackSyncStickyKey& key) {
    if (saturated_ || keys_.count(key)) {
      return;
    }
    if (keys_.size() >= kReadbackSyncStickyMaxKeys) {
      saturated_ = true;
      return;
    }
    keys_.insert(key);
  }
  bool Excludes(const ReadbackSyncStickyKey& key) const {
    return saturated_ || keys_.count(key) != 0;
  }
  void Clear() {
    keys_.clear();
    saturated_ = false;
  }
  size_t size() const { return keys_.size(); }
  bool saturated() const { return saturated_; }

 private:
  std::unordered_set<ReadbackSyncStickyKey, ReadbackSyncStickyKeyHash> keys_;
  bool saturated_ = false;
};

// Single-key cap. Admit is called for every candidate resolve that passed
// the sticky and shape filters; it allows the exemption only while at most
// one distinct key was such a candidate in this frame and in the previous
// frame. The decision is made per resolve, so when a second key appears
// mid-frame the first key's earlier resolves of that frame stay exempt;
// every later resolve of either key is refused until a frame that follows a
// frame with at most one key sees only one key again. An episode is a run of
// refusals; `episode_started` is set on its first refusal.
struct ReadbackSyncSteadyCap {
  struct Result {
    bool allowed = true;
    bool episode_started = false;
    // Distinct candidate keys (2 = more than one) in this or the previous
    // frame, whichever is larger.
    uint32_t keys = 0;
  };

  bool has_frame = false;
  uint64_t frame = 0;
  uint64_t first_key = 0;
  // Distinct candidate keys in `frame` (saturates at 2) and in the frame
  // before it (0 after a gap).
  uint32_t keys = 0;
  uint32_t previous_keys = 0;
  bool episode = false;

  Result Admit(uint64_t frame_now, uint64_t key) {
    if (!has_frame || frame_now != frame) {
      previous_keys = (has_frame && frame + 1 == frame_now) ? keys : 0;
      if (previous_keys <= 1) {
        episode = false;
      }
      has_frame = true;
      frame = frame_now;
      keys = 0;
    }
    if (!keys) {
      keys = 1;
      first_key = key;
    } else if (key != first_key) {
      keys = 2;
    }
    Result result;
    result.keys = keys > previous_keys ? keys : previous_keys;
    if (keys <= 1 && previous_keys <= 1) {
      episode = false;
      return result;
    }
    result.allowed = false;
    result.episode_started = !episode;
    episode = true;
    return result;
  }

  void Reset() { *this = ReadbackSyncSteadyCap(); }
};

// The filter decision for one evaluated resolve.
enum class ReadbackSyncSteadyFilter : uint32_t {
  // Not a candidate (rules or run), or the switch is off.
  kNotCandidate,
  kExempt,
  kSticky,
  kShape,
  kCap,
};

inline const char* ReadbackSyncSteadyFilterReason(ReadbackSyncSteadyFilter f) {
  switch (f) {
    case ReadbackSyncSteadyFilter::kSticky:
      return "sticky";
    case ReadbackSyncSteadyFilter::kShape:
      return "shape";
    case ReadbackSyncSteadyFilter::kCap:
      return "cap";
    default:
      return "none";
  }
}

// The filters in order for a candidate; `has_chain_neighbour` and `cap` are
// evaluated only when needed (the cap sees only resolves that passed the
// sticky and shape filters).
template <typename ChainCheck>
ReadbackSyncSteadyFilter FilterReadbackSyncSteady(bool candidate, bool sticky,
                                                  ChainCheck&& has_chain_neighbour,
                                                  ReadbackSyncSteadyCap& cap, uint64_t frame,
                                                  uint64_t key,
                                                  ReadbackSyncSteadyCap::Result* cap_result) {
  if (!candidate) {
    return ReadbackSyncSteadyFilter::kNotCandidate;
  }
  if (sticky) {
    return ReadbackSyncSteadyFilter::kSticky;
  }
  if (has_chain_neighbour()) {
    return ReadbackSyncSteadyFilter::kShape;
  }
  ReadbackSyncSteadyCap::Result r = cap.Admit(frame, key);
  if (cap_result) {
    *cap_result = r;
  }
  return r.allowed ? ReadbackSyncSteadyFilter::kExempt : ReadbackSyncSteadyFilter::kCap;
}

// End reason of a key that was exempt: the rule reasons first, then the
// filter that refused it.
inline const char* ReadbackSyncSteadyEndReason(bool enabled, const ReadbackSyncRules& rules,
                                               uint32_t run, ReadbackSyncSteadyFilter filter) {
  const char* reason = ReadbackSyncSteadyEndReason(enabled, rules, run);
  if (std::string_view(reason) != "none") {
    return reason;
  }
  return ReadbackSyncSteadyFilterReason(filter);
}

// One kind=steady_key line (the MCLA_READBACK_SYNC v=1 prefix is added by the
// caller): a key became exempt (event=exempt) or stopped being exempt
// (event=end, run = the run of its last exempt resolve).
struct ReadbackSyncSteadyKeyEvent {
  bool exempt = false;
  const char* reason = "none";
  uint64_t frame = 0;
  uint32_t draw = 0;
  uint32_t address = 0;
  uint32_t length = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t dest_format = 0;
  uint32_t source_edram_base = 0;
  uint32_t run = 0;
  uint64_t suppressed = 0;
};

inline std::string FormatReadbackSyncSteadyKeyEvent(const ReadbackSyncSteadyKeyEvent& e) {
  return fmt::format(
      "kind=steady_key event={} reason={} frame={} draw={} base={:08X} len={} size={}x{} fmt={} "
      "src_base={} run={} min_frames={} suppressed={}",
      e.exempt ? "exempt" : "end", e.reason, e.frame, e.draw, e.address, e.length, e.width,
      e.height, e.dest_format, e.source_edram_base, e.run, kReadbackSyncSteadyMinFrames,
      e.suppressed);
}

inline const char* ReadbackSyncRuleNames(const ReadbackSyncRules& rules) {
  static constexpr const char* kNames[8] = {"none", "a", "b", "ab", "c", "ac", "bc", "abc"};
  return kNames[(rules.shared_range ? 1 : 0) | (rules.sporadic ? 2 : 0) |
                (rules.composite_shape ? 4 : 0)];
}

// d3d12_readback_sync_targeted (M10 KI-047 cost). A forced resolve records
// its GPU copy into the open submission, ends that submission (the replay or
// deferred list is closed and executed, then the submission fence is
// signalled with the copy's submission index) and waits for exactly that
// fence value, then copies its own slot to guest memory. The full wait
// (AwaitAllQueueOperationsCompletion) did the same two steps and, only when
// no submission was open or ending it failed, an extra wait for queue
// operations outside submissions; the direct queue executes in order, so the
// fence of the copy's submission already covers every earlier submission.
// The targeted mode therefore only drops that never-needed extra step and
// falls back to the full wait in exactly those cases.
//
// What can make the wait shorter is a smaller backlog: the GPU only starts a
// submission when it is executed, so the fence wait pays for every draw
// recorded since the last submission. With the targeted mode on and
// d3d12_readback_sync_kick_draws > 0, the open submission is executed early
// ("kick") before a draw when it already holds that many draws, the GPU has
// finished every earlier submission (it would otherwise idle), no pipeline is
// being created (the primary-buffer-end submission uses the same test) and no
// host occlusion query is active (ending a submission ends the query). A kick
// is a host scheduling point only: it adds no guest-visible write, and mid-
// frame submissions already happen at every primary buffer end.

// The copy's own submission was ended: the submission index advanced past it.
inline bool ReadbackTargetedSubmissionEnded(uint64_t copy_submission,
                                            uint64_t submission_current_after) {
  return submission_current_after > copy_submission;
}

// The fence covers the copy: its submission (or a later one) completed.
inline bool ReadbackTargetedWaitSatisfied(uint64_t submission_completed, uint64_t copy_submission) {
  return submission_completed >= copy_submission;
}

// Whether a forced resolve may take the targeted wait (else the full wait).
inline bool ShouldUseReadbackTargetedWait(bool targeted, bool submission_open) {
  return targeted && submission_open;
}

enum class ReadbackTargetedResult : uint32_t {
  // The copy's submission completed: read the own slot.
  kCompleted,
  // Ending the submission failed (it is still open): take the full wait, as
  // without the targeted mode.
  kFallbackToFull,
  // The submission was executed but its fence did not complete: the failure
  // the full wait reports too (the resolve is not copied).
  kFenceFailed,
};

inline ReadbackTargetedResult ClassifyReadbackTargetedWait(uint64_t copy_submission,
                                                           uint64_t submission_current_after,
                                                           uint64_t submission_completed) {
  if (!ReadbackTargetedSubmissionEnded(copy_submission, submission_current_after)) {
    return ReadbackTargetedResult::kFallbackToFull;
  }
  return ReadbackTargetedWaitSatisfied(submission_completed, copy_submission)
             ? ReadbackTargetedResult::kCompleted
             : ReadbackTargetedResult::kFenceFailed;
}

struct ReadbackKickInput {
  bool targeted = false;
  bool sync_enabled = false;
  uint32_t kick_draws = 0;
  bool submission_open = false;
  uint64_t draws_in_submission = 0;
  // CanEndSubmissionImmediately (no pipeline creation pending).
  bool can_end_immediately = false;
  bool occlusion_query_active = false;
  // Submission fence: completed value and the open submission's index.
  uint64_t submission_completed = 0;
  uint64_t submission_current = 0;
};

inline bool ShouldKickReadbackSyncSubmission(const ReadbackKickInput& in) {
  if (!in.targeted || !in.sync_enabled || !in.kick_draws || !in.submission_open) {
    return false;
  }
  if (in.draws_in_submission < in.kick_draws || !in.can_end_immediately ||
      in.occlusion_query_active) {
    return false;
  }
  // Every submission before the open one has completed: the GPU is idle.
  return in.submission_completed + 1 >= in.submission_current;
}

struct ReadbackTargetedWait {
  uint64_t submit_us = 0;
  uint64_t fence_us = 0;
  bool gpu_busy_at_submit = false;
  uint64_t backlog_draws = 0;
};

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

  // d3d12_readback_sync_targeted (all zero while it is off).
  // Forced resolves that waited on their own copy's submission fence.
  uint64_t targeted_waits = 0;
  // Their whole wait (submit + fence), and the two parts.
  uint64_t targeted_wait_us = 0;
  uint64_t targeted_max_wait_us = 0;
  uint64_t targeted_submit_us = 0;
  uint64_t targeted_fence_us = 0;
  // Forced resolves of the targeted mode that took the full wait instead
  // (no open submission, or ending it failed).
  uint64_t fallbacks_to_full = 0;
  // Of the targeted waits: the previous submission was still running on the
  // GPU when the copy was submitted, and the draws recorded in the copy's
  // submission (the unsubmitted backlog the fence wait has to execute).
  uint64_t targeted_gpu_busy = 0;
  uint64_t targeted_backlog_draws = 0;
  // Early submissions (kicks) and their CPU time.
  uint64_t kicks = 0;
  uint64_t kick_us = 0;

  // d3d12_readback_sync_steady_exempt: non-miss resolves that rule c
  // matched alone and that were not forced because their key is steady,
  // the distinct keys among them, and the longest consecutive-frame run of
  // any non-miss rule c resolve in the minute (counted with the switch off
  // too; reported only while it is on or has counted).
  uint64_t steady_exempt = 0;
  uint64_t steady_exempt_keys = 0;
  uint32_t steady_max_run = 0;

  // d3d12_readback_sync_steady_exempt filters (M11-007b): candidates
  // refused by the sticky, shape and cap filters; and with
  // d3d12_readback_sync_steady_fast, exempt resolves copied from the
  // complete previous slot (and their bytes), and exempt resolves whose
  // previous slot was not known complete (kept the some cadence).
  uint64_t steady_sticky_excluded = 0;
  uint64_t steady_shape_excluded = 0;
  uint64_t steady_cap_skipped = 0;
  uint64_t steady_fast_copies = 0;
  uint64_t steady_fast_bytes = 0;
  uint64_t steady_fast_skipped = 0;

  // d3d12_readback_sync_steady_fast_wait_ms (M11-007c, readback_slots.h
  // ReadbackSteadyDelivery): exempt resolves that waited on the previous
  // slot's fence, the summed and the longest wait, the waits that timed out
  // (forced the own slot), all forced exempt resolves (timeouts and
  // previous slots that cannot be waited for), and the every-4th copies of
  // an unfenced previous slot that the l75 cadence would have made here.
  uint64_t steady_fast_waits = 0;
  uint64_t steady_fast_wait_us = 0;
  uint64_t steady_fast_wait_max_us = 0;
  uint64_t steady_fast_wait_timeouts = 0;
  uint64_t steady_fast_forced = 0;
  uint64_t steady_unfenced_avoided = 0;
  // Skip episodes: runs of consecutive exempt resolves of a key whose
  // previous slot was not complete at the poll (waited, forced or cadence);
  // the runs that started in this minute and the longest run seen in it (a
  // run that continues across a minute line counts in both minutes'
  // longest, once in the episodes).
  uint64_t steady_skip_episodes = 0;
  uint32_t steady_skip_longest = 0;

  // One exempt steady fast resolve. `skip_run` is the key's current skip
  // run (kept with the key; reset by a resolve that is not a skip).
  void CountSteadyDelivery(bool waited, bool timed_out, bool forced, bool skip,
                           bool unfenced_avoided, uint64_t wait_us, uint32_t& skip_run) {
    if (waited) {
      ++steady_fast_waits;
      steady_fast_wait_us += wait_us;
      steady_fast_wait_max_us =
          wait_us > steady_fast_wait_max_us ? wait_us : steady_fast_wait_max_us;
    }
    steady_fast_wait_timeouts += timed_out ? 1 : 0;
    steady_fast_forced += forced ? 1 : 0;
    steady_unfenced_avoided += unfenced_avoided ? 1 : 0;
    if (!skip) {
      skip_run = 0;
      return;
    }
    if (!skip_run) {
      ++steady_skip_episodes;
    }
    ++skip_run;
    steady_skip_longest = skip_run > steady_skip_longest ? skip_run : steady_skip_longest;
  }

  bool SteadyWaitCounted() const {
    return steady_fast_waits || steady_fast_wait_timeouts || steady_fast_forced ||
           steady_unfenced_avoided || steady_skip_episodes || steady_skip_longest;
  }

  void CountSteadyFilter(ReadbackSyncSteadyFilter filter) {
    steady_sticky_excluded += filter == ReadbackSyncSteadyFilter::kSticky ? 1 : 0;
    steady_shape_excluded += filter == ReadbackSyncSteadyFilter::kShape ? 1 : 0;
    steady_cap_skipped += filter == ReadbackSyncSteadyFilter::kCap ? 1 : 0;
  }

  bool SteadyFilterCounted() const {
    return steady_sticky_excluded || steady_shape_excluded || steady_cap_skipped ||
           steady_fast_copies || steady_fast_skipped;
  }

  void CountSteadyRun(uint32_t run) {
    steady_max_run = run > steady_max_run ? run : steady_max_run;
  }

  // `first_in_minute`: the first exempt resolve of this key since the
  // counters were last reset.
  void CountSteadyExempt(bool first_in_minute) {
    ++steady_exempt;
    steady_exempt_keys += first_in_minute ? 1 : 0;
  }

  void CountTargeted(const ReadbackTargetedWait& wait) {
    ++targeted_waits;
    uint64_t total_us = wait.submit_us + wait.fence_us;
    targeted_wait_us += total_us;
    targeted_max_wait_us = total_us > targeted_max_wait_us ? total_us : targeted_max_wait_us;
    targeted_submit_us += wait.submit_us;
    targeted_fence_us += wait.fence_us;
    targeted_gpu_busy += wait.gpu_busy_at_submit ? 1 : 0;
    targeted_backlog_draws += wait.backlog_draws;
  }
};

// Fields appended to the MCLA_READBACK_SYNC minute and mark lines: empty
// while d3d12_readback_sync_steady_exempt is off and nothing was exempt (the
// line is unchanged then).
inline std::string ReadbackSyncSteadyReport(const ReadbackSyncCounters& c, bool enabled) {
  if (!enabled && !c.steady_exempt) {
    return std::string();
  }
  return fmt::format(
      " steady={} steady_min_frames={} steady_exempt={} steady_exempt_keys={} "
      "steady_max_run={}",
      int(enabled), kReadbackSyncSteadyMinFrames, c.steady_exempt, c.steady_exempt_keys,
      c.steady_max_run);
}

// The M11-007b fields appended after ReadbackSyncSteadyReport: empty while
// the exemption is off and none of them counted (the line is unchanged then).
inline std::string ReadbackSyncSteadyFilterReport(const ReadbackSyncCounters& c, bool enabled,
                                                  bool fast_enabled) {
  if (!enabled && !c.SteadyFilterCounted()) {
    return std::string();
  }
  return fmt::format(
      " steady_fast={} steady_sticky_excluded={} steady_shape_excluded={} steady_cap_skipped={} "
      "steady_fast_copies={} steady_fast_kb={} steady_fast_skipped={}",
      int(fast_enabled), c.steady_sticky_excluded, c.steady_shape_excluded, c.steady_cap_skipped,
      c.steady_fast_copies, c.steady_fast_bytes / 1024, c.steady_fast_skipped);
}

// The M11-007c fields appended after ReadbackSyncSteadyFilterReport: empty
// while the exemption is off and none of them counted (the line is unchanged
// then). steady_fast_wait_ms is the cvar (0: the l75 cadence).
inline std::string ReadbackSyncSteadyWaitReport(const ReadbackSyncCounters& c, bool enabled,
                                                uint32_t wait_ms) {
  if (!enabled && !c.SteadyWaitCounted()) {
    return std::string();
  }
  return fmt::format(
      " steady_fast_wait_ms={} steady_fast_waits={} steady_fast_wait_ms_total={:.3f} "
      "steady_fast_wait_ms_max={:.3f} steady_fast_wait_timeouts={} steady_fast_forced={} "
      "steady_unfenced_avoided={} steady_skip_episodes={} steady_skip_longest={}",
      wait_ms, c.steady_fast_waits, double(c.steady_fast_wait_us) / 1000.0,
      double(c.steady_fast_wait_max_us) / 1000.0, c.steady_fast_wait_timeouts, c.steady_fast_forced,
      c.steady_unfenced_avoided, c.steady_skip_episodes, c.steady_skip_longest);
}

// One kind=steady_cap line (prefix added by the caller): the first refusal
// of a cap episode.
inline std::string FormatReadbackSyncSteadyCapLine(uint64_t frame, uint32_t draw, uint32_t address,
                                                   uint32_t length, uint32_t keys,
                                                   uint64_t suppressed) {
  return fmt::format(
      "kind=steady_cap event=start frame={} draw={} base={:08X} len={} keys={} suppressed={}",
      frame, draw, address, length, keys, suppressed);
}

}  // namespace rex::graphics
