#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/graphics/util/livery_diag.h>

namespace rex::graphics {

// Pure helpers of the d3d12_livery_diag2 diagnostics (MCLA KI-047 "diag v2":
// the police livery broke on a cold pipeline store with zero skipped draws).
// The v1 lines (d3d12_livery_diag) log a read-back resolve only for a fresh
// key or after skips, so a deferred or previous-slot copy of a re-used scratch
// range and texture-side staleness were invisible. v2 adds these line kinds,
// each with its own per-minute budget:
// - resolve2: every read-back resolve of a sporadic key (the previous resolve
//   of the same key was 2 or more frames ago, or never), whatever the copy
//   decision;
// - overlap: a resolve whose destination overlaps a different live read-back
//   key;
// - tex_load / tex_inval: texture-cache loads of ranges that were read-back
//   destinations within the last 600 frames, and of livery-atlas-shaped
//   textures, and the first write-watch invalidation after such a load;
// - pso_wait: pipeline waits of 1 ms or more, and wait_resolve: the next
//   read-back resolve from the RT0 EDRAM base of such waits.
// Bounded and privacy-safe like v1: frame and draw numbers, guest addresses,
// sizes, formats and flags only. No GPU is needed, so this is unit-tested.

constexpr uint32_t kLiveryDiag2LinesPerMinute = 64;
// A resolve whose previous same-key resolve is at least this many frames ago
// is "sporadic" (per-frame scene, shadow and bloom tiles are not).
constexpr uint64_t kLiveryDiag2SporadicMinFrames = 2;
// Texture loads overlapping a read-back destination of this many recent
// frames are reported.
constexpr uint64_t kLiveryDiag2ReadbackWindowFrames = 600;
// Pipeline waits of at least this many microseconds are reported.
constexpr uint64_t kLiveryDiag2LongWaitUs = 1000;
// Long pipeline waits are matched to the next read-back resolve from the same
// RT0 EDRAM base within this many frames.
constexpr uint64_t kLiveryDiag2WaitMatchFrames = 600;
// An overlap between the same two keys is reported again only after this many
// quiet frames.
constexpr uint64_t kLiveryDiag2OverlapQuietFrames = 120;

inline bool IsSporadicReadbackResolve(bool seen_before, uint64_t previous_frame, uint64_t frame) {
  return !seen_before || frame < previous_frame ||
         frame - previous_frame >= kLiveryDiag2SporadicMinFrames;
}

inline bool IsLongPipelineWait(uint64_t elapsed_us) {
  return elapsed_us >= kLiveryDiag2LongWaitUs;
}

// Wait time (microseconds) of the last kFrames frames, the current one
// included, for "how much did the command processor wait around this
// resolve".
class FrameWaitWindow {
 public:
  static constexpr uint64_t kFrames = 5;

  void Add(uint64_t frame, uint64_t us) {
    Slot& slot = slots_[frame % kFrames];
    if (!slot.valid || slot.frame != frame) {
      slot.valid = true;
      slot.frame = frame;
      slot.us = 0;
    }
    slot.us += us;
  }

  uint64_t SumUs(uint64_t frame) const {
    uint64_t sum = 0;
    for (const Slot& slot : slots_) {
      if (slot.valid && slot.frame <= frame && frame - slot.frame < kFrames) {
        sum += slot.us;
      }
    }
    return sum;
  }

  void Clear() { slots_ = {}; }

 private:
  struct Slot {
    bool valid = false;
    uint64_t frame = 0;
    uint64_t us = 0;
  };
  std::array<Slot, kFrames> slots_ = {};
};

// Long pipeline waits waiting for the next read-back resolve from the same
// RT0 EDRAM base. Waits into one base are merged (first frame and draw kept,
// count, total and longest accumulated). Bounded: when full, the oldest entry
// is dropped.
class PendingLongWaits {
 public:
  struct Wait {
    uint32_t rt0_base = 0;
    uint64_t first_frame = 0;
    uint32_t first_draw = 0;
    uint64_t last_frame = 0;
    uint32_t count = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;
  };
  static constexpr size_t kCapacity = 16;

  void Add(uint32_t rt0_base, uint64_t frame, uint32_t draw, uint64_t us) {
    for (size_t i = 0; i < size_; ++i) {
      Wait& wait = waits_[i];
      if (wait.rt0_base == rt0_base) {
        wait.last_frame = frame;
        ++wait.count;
        wait.total_us += us;
        wait.max_us = us > wait.max_us ? us : wait.max_us;
        return;
      }
    }
    if (size_ == kCapacity) {
      Erase(0);
    }
    Wait& wait = waits_[size_++];
    wait.rt0_base = rt0_base;
    wait.first_frame = frame;
    wait.first_draw = draw;
    wait.last_frame = frame;
    wait.count = 1;
    wait.total_us = us;
    wait.max_us = us;
  }

  // Takes the waits into `base` if any (expired entries are dropped).
  bool TakeForBase(uint32_t base, uint64_t frame, Wait* wait_out) {
    bool found = false;
    for (size_t i = 0; i < size_;) {
      Wait& wait = waits_[i];
      bool expired =
          frame >= wait.last_frame && frame - wait.last_frame > kLiveryDiag2WaitMatchFrames;
      if (!expired && !found && wait.rt0_base == base) {
        if (wait_out) {
          *wait_out = wait;
        }
        found = true;
        Erase(i);
        continue;
      }
      if (expired) {
        Erase(i);
        continue;
      }
      ++i;
    }
    return found;
  }

  size_t size() const { return size_; }
  void Clear() { size_ = 0; }

 private:
  void Erase(size_t index) {
    for (size_t i = index + 1; i < size_; ++i) {
      waits_[i - 1] = waits_[i];
    }
    --size_;
  }

  std::array<Wait, kCapacity> waits_ = {};
  size_t size_ = 0;
};

// Half-open byte ranges [a, a + a_length) and [b, b + b_length).
inline bool GuestRangesOverlap(uint32_t a, uint32_t a_length, uint32_t b, uint32_t b_length) {
  return a_length && b_length && uint64_t(a) < uint64_t(b) + b_length &&
         uint64_t(b) < uint64_t(a) + a_length;
}

// Read-back keys are (address << 32) | length, as MakeReadbackResolveKey.
inline uint32_t ReadbackKeyAddress(uint64_t key) {
  return uint32_t(key >> 32);
}
inline uint32_t ReadbackKeyLength(uint64_t key) {
  return uint32_t(key);
}

// Order-independent identity of a pair of read-back keys for the overlap
// recency filter.
inline uint64_t ReadbackKeyPairId(uint64_t a, uint64_t b) {
  uint64_t lo = a < b ? a : b;
  uint64_t hi = a < b ? b : a;
  uint64_t h = lo * 0x9E3779B97F4A7C15ull;
  h ^= hi + 0x7F4A7C159E3779B9ull + (h << 6) + (h >> 2);
  return h;
}

// Xenos texture formats (xenos::TextureFormat values) of the livery atlas
// candidates: 1024x1024 DXT4/5 (BC3) and 2048x2048 8_8_8_8.
constexpr uint32_t kLiveryDiag2Format8888 = 6;
constexpr uint32_t kLiveryDiag2Format8888A = 14;
constexpr uint32_t kLiveryDiag2FormatDxt45 = 20;
constexpr uint32_t kLiveryDiag2Format8888As16161616 = 50;
constexpr uint32_t kLiveryDiag2FormatDxt45As16161616 = 53;

inline bool IsLiveryAtlasShape(uint32_t width, uint32_t height, uint32_t format) {
  if (width == 1024 && height == 1024) {
    return format == kLiveryDiag2FormatDxt45 || format == kLiveryDiag2FormatDxt45As16161616;
  }
  if (width == 2048 && height == 2048) {
    return format == kLiveryDiag2Format8888 || format == kLiveryDiag2Format8888A ||
           format == kLiveryDiag2Format8888As16161616;
  }
  return false;
}

// Per-key history of read-back resolves, independent of the read-back buffer
// cache (which forgets a key 60 frames after its last use). Bounded: when it
// is full, entries older than `keep_frames` are dropped, and if that frees
// nothing the table is cleared (at worst a key looks new once more).
class ReadbackKeyHistory {
 public:
  struct Lookup {
    bool seen = false;
    uint64_t previous_resolve_frame = 0;
    bool copied_before = false;
    uint64_t last_copy_frame = 0;
  };

  explicit ReadbackKeyHistory(size_t capacity = 8192, uint64_t keep_frames = 3600)
      : capacity_(capacity), keep_frames_(keep_frames) {}

  Lookup Get(uint64_t key) const {
    Lookup lookup;
    auto it = entries_.find(key);
    if (it != entries_.end()) {
      lookup.seen = true;
      lookup.previous_resolve_frame = it->second.last_resolve_frame;
      lookup.copied_before = it->second.copied_before;
      lookup.last_copy_frame = it->second.last_copy_frame;
    }
    return lookup;
  }

  void Record(uint64_t key, uint64_t frame, bool copied) {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      if (entries_.size() >= capacity_) {
        Prune(frame);
      }
      it = entries_.emplace(key, Entry()).first;
    }
    it->second.last_resolve_frame = frame;
    if (copied) {
      it->second.copied_before = true;
      it->second.last_copy_frame = frame;
    }
  }

  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    uint64_t last_resolve_frame = 0;
    bool copied_before = false;
    uint64_t last_copy_frame = 0;
  };

  void Prune(uint64_t frame) {
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (frame < it->second.last_resolve_frame ||
          frame - it->second.last_resolve_frame > keep_frames_) {
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    if (entries_.size() >= capacity_) {
      entries_.clear();
    }
  }

  size_t capacity_;
  uint64_t keep_frames_;
  std::unordered_map<uint64_t, Entry> entries_;
};

// Read-back destination ranges of the recent frames, for matching texture
// loads against them. Keyed like the read-back cache; bounded like
// ReadbackKeyHistory.
class RecentReadbackRanges {
 public:
  struct Match {
    bool found = false;
    uint32_t address = 0;
    uint32_t length = 0;
    uint64_t last_resolve_frame = 0;
    bool copied = false;
    uint64_t last_copy_frame = 0;
    // Number of recent destinations overlapping the queried range.
    uint32_t count = 0;
  };

  explicit RecentReadbackRanges(uint64_t window_frames = kLiveryDiag2ReadbackWindowFrames,
                                size_t capacity = 2048)
      : window_frames_(window_frames), capacity_(capacity) {}

  void Note(uint32_t address, uint32_t length, uint64_t frame, bool copied) {
    if (!length) {
      return;
    }
    uint64_t key = (uint64_t(address) << 32) | length;
    auto it = ranges_.find(key);
    if (it == ranges_.end()) {
      if (ranges_.size() >= capacity_) {
        Prune(frame);
      }
      it = ranges_.emplace(key, Entry()).first;
    }
    it->second.last_resolve_frame = frame;
    if (copied) {
      it->second.copied = true;
      it->second.last_copy_frame = frame;
    }
  }

  // The most recently resolved destination within the window that overlaps
  // [address, address + length).
  Match Find(uint32_t address, uint32_t length, uint64_t frame) const {
    Match match;
    for (const auto& [key, entry] : ranges_) {
      if (frame >= entry.last_resolve_frame && frame - entry.last_resolve_frame > window_frames_) {
        continue;
      }
      uint32_t range_address = ReadbackKeyAddress(key);
      uint32_t range_length = ReadbackKeyLength(key);
      if (!GuestRangesOverlap(address, length, range_address, range_length)) {
        continue;
      }
      ++match.count;
      if (!match.found || entry.last_resolve_frame > match.last_resolve_frame) {
        match.found = true;
        match.address = range_address;
        match.length = range_length;
        match.last_resolve_frame = entry.last_resolve_frame;
        match.copied = entry.copied;
        match.last_copy_frame = entry.last_copy_frame;
      }
    }
    return match;
  }

  void Clear() { ranges_.clear(); }
  size_t size() const { return ranges_.size(); }

 private:
  struct Entry {
    uint64_t last_resolve_frame = 0;
    bool copied = false;
    uint64_t last_copy_frame = 0;
  };

  void Prune(uint64_t frame) {
    for (auto it = ranges_.begin(); it != ranges_.end();) {
      if (frame < it->second.last_resolve_frame ||
          frame - it->second.last_resolve_frame > window_frames_) {
        it = ranges_.erase(it);
      } else {
        ++it;
      }
    }
    if (ranges_.size() >= capacity_) {
      ranges_.clear();
    }
  }

  uint64_t window_frames_;
  size_t capacity_;
  std::unordered_map<uint64_t, Entry> ranges_;
};

// Texture parts (base or mips of one texture) loaded under watch, waiting
// for their first write-watch invalidation. Bounded: when full, the older
// half (by load frame) is dropped and counted as expired.
class TextureLoadWatchList {
 public:
  explicit TextureLoadWatchList(size_t capacity = 1024) : capacity_(capacity) {}

  // Returns true if an earlier load of the same part was still waiting (a
  // reload without an observed invalidation).
  bool OnLoad(uint64_t id, uint64_t frame) {
    auto it = loads_.find(id);
    if (it != loads_.end()) {
      it->second = frame;
      return true;
    }
    if (loads_.size() >= capacity_) {
      DropOldest();
    }
    loads_.emplace(id, frame);
    return false;
  }

  // Returns true and the load frame if the part was waiting; it stops
  // waiting.
  bool TakeOnInvalidate(uint64_t id, uint64_t* load_frame_out) {
    auto it = loads_.find(id);
    if (it == loads_.end()) {
      return false;
    }
    if (load_frame_out) {
      *load_frame_out = it->second;
    }
    loads_.erase(it);
    return true;
  }

  uint64_t TakeExpired() {
    uint64_t expired = expired_;
    expired_ = 0;
    return expired;
  }

  void Clear() { loads_.clear(); }
  size_t size() const { return loads_.size(); }

 private:
  void DropOldest() {
    uint64_t newest = 0;
    uint64_t oldest = UINT64_MAX;
    for (const auto& [id, frame] : loads_) {
      newest = frame > newest ? frame : newest;
      oldest = frame < oldest ? frame : oldest;
    }
    uint64_t cut = oldest + (newest - oldest) / 2;
    for (auto it = loads_.begin(); it != loads_.end();) {
      if (it->second <= cut) {
        it = loads_.erase(it);
        ++expired_;
      } else {
        ++it;
      }
    }
    if (loads_.size() >= capacity_) {
      expired_ += loads_.size();
      loads_.clear();
    }
  }

  size_t capacity_;
  uint64_t expired_ = 0;
  std::unordered_map<uint64_t, uint64_t> loads_;
};

// Identity of one texture part for TextureLoadWatchList: base page, size,
// format and whether it is the mip chain.
inline uint64_t LiveryDiag2TexturePartId(uint32_t page, uint32_t width, uint32_t height,
                                         uint32_t format, bool is_mip) {
  return (uint64_t(page) << 40) ^ (uint64_t(width & 0x1FFF) << 27) ^
         (uint64_t(height & 0x1FFF) << 14) ^ (uint64_t(format & 0x3F) << 1) ^ uint64_t(is_mip);
}

// Texture-side state of the v2 diagnostics, shared by the command processor
// (which owns it, sets the frame and notes read-back destinations) and the
// texture cache (loads on the command processor thread, invalidations from
// any thread under the texture cache's global lock). Internally locked.
class LiveryDiag2TextureTracker {
 public:
  struct Counters {
    uint64_t loads_tracked = 0;
    uint64_t loads_readback_overlap = 0;
    uint64_t loads_atlas_shape = 0;
    uint64_t reloads_without_invalidation = 0;
    uint64_t invalidations = 0;
    uint64_t invalidations_by_gpu = 0;
    uint64_t expired = 0;
    uint64_t load_lines_suppressed = 0;
    uint64_t invalidation_lines_suppressed = 0;
  };

  struct LoadDecision {
    bool tracked = false;
    bool log = false;
    bool atlas_shape = false;
    bool reload_without_invalidation = false;
    // First load of this part since the diagnostics were enabled.
    bool first_upload = false;
    RecentReadbackRanges::Match readback;
    uint64_t frame = 0;
  };

  struct InvalidationDecision {
    bool tracked = false;
    bool log = false;
    uint64_t load_frame = 0;
    uint64_t frame = 0;
  };

  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Enabling or disabling clears the state and the counters.
  void SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled == enabled_.load(std::memory_order_relaxed)) {
      return;
    }
    ranges_.Clear();
    loads_.Clear();
    seen_parts_.clear();
    loads_.TakeExpired();
    load_lines_.TakeSuppressed();
    invalidation_lines_.TakeSuppressed();
    counters_ = {};
    enabled_.store(enabled, std::memory_order_relaxed);
  }

  void SetFrame(uint64_t frame) { frame_.store(frame, std::memory_order_relaxed); }
  uint64_t frame() const { return frame_.load(std::memory_order_relaxed); }

  void NoteReadbackDestination(uint32_t address, uint32_t length, bool copied) {
    std::lock_guard<std::mutex> lock(mutex_);
    ranges_.Note(address, length, frame(), copied);
  }

  // One loaded texture part [address, address + length).
  LoadDecision OnTextureLoad(uint64_t part_id, uint32_t address, uint32_t length, uint32_t width,
                             uint32_t height, uint32_t format, uint64_t now_ms) {
    LoadDecision decision;
    decision.frame = frame();
    decision.atlas_shape = IsLiveryAtlasShape(width, height, format);
    std::lock_guard<std::mutex> lock(mutex_);
    decision.readback = ranges_.Find(address, length, decision.frame);
    if (!decision.atlas_shape && !decision.readback.found) {
      return decision;
    }
    decision.tracked = true;
    ++counters_.loads_tracked;
    counters_.loads_readback_overlap += decision.readback.found ? 1 : 0;
    counters_.loads_atlas_shape += decision.atlas_shape ? 1 : 0;
    decision.reload_without_invalidation = loads_.OnLoad(part_id, decision.frame);
    if (seen_parts_.size() >= kSeenPartsCapacity) {
      seen_parts_.clear();
    }
    decision.first_upload = seen_parts_.insert(part_id).second;
    counters_.reloads_without_invalidation += decision.reload_without_invalidation ? 1 : 0;
    decision.log = load_lines_.Allow(now_ms);
    return decision;
  }

  InvalidationDecision OnTextureInvalidated(uint64_t part_id, bool by_gpu, uint64_t now_ms) {
    InvalidationDecision decision;
    decision.frame = frame();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!loads_.TakeOnInvalidate(part_id, &decision.load_frame)) {
      return decision;
    }
    decision.tracked = true;
    ++counters_.invalidations;
    counters_.invalidations_by_gpu += by_gpu ? 1 : 0;
    decision.log = invalidation_lines_.Allow(now_ms);
    return decision;
  }

  Counters TakeCounters() {
    std::lock_guard<std::mutex> lock(mutex_);
    Counters counters = counters_;
    counters.expired = loads_.TakeExpired();
    counters.load_lines_suppressed = load_lines_.TakeSuppressed();
    counters.invalidation_lines_suppressed = invalidation_lines_.TakeSuppressed();
    counters_ = {};
    return counters;
  }

  size_t waiting_loads() {
    std::lock_guard<std::mutex> lock(mutex_);
    return loads_.size();
  }

 private:
  static constexpr size_t kSeenPartsCapacity = 8192;
  std::atomic<bool> enabled_{false};
  std::atomic<uint64_t> frame_{0};
  std::mutex mutex_;
  RecentReadbackRanges ranges_;
  TextureLoadWatchList loads_;
  std::unordered_set<uint64_t> seen_parts_;
  WindowRateLimiter load_lines_{kLiveryDiag2LinesPerMinute};
  WindowRateLimiter invalidation_lines_{kLiveryDiag2LinesPerMinute};
  Counters counters_;
};

// Per-minute counters of the command-processor side, reported in one
// MCLA_LIVERY_DIAG2 minute line together with the tracker's counters.
struct LiveryDiag2Counters {
  uint64_t resolves = 0;
  uint64_t sporadic_resolves = 0;
  uint64_t sporadic_not_copied = 0;
  uint64_t sporadic_read_prev = 0;
  uint64_t sporadic_read_prev_unfenced = 0;
  uint64_t sporadic_new_slot = 0;
  uint64_t overlapping_resolves = 0;
  uint64_t pipeline_waits = 0;
  uint64_t long_pipeline_waits = 0;
  uint64_t long_pipeline_wait_us = 0;
  uint64_t wait_resolves = 0;
};

}  // namespace rex::graphics
