#pragma once

// M10 Deck watch/upload path (docs/evidence/M10-deck-watch-upload-design-20261101.md
// in the MCLA-R host repository), steps 0-2.
//
// Step 0 (memory_watch_census, default on): counters only. Every counter is a
// plain increment or an rdtsc pair around an existing protection call; nothing
// changes behaviour. The command processor writes one
// "MCLA_WATCH v=1 kind=minute|mark" line per pacing minute and per F10 mark.
// The F10 probes (memory_watch_probes, default off) add one
// "MCLA_WATCH v=1 kind=probe" line.
//
// Step 1 (memory_watch_disarm_bridge_safe_gaps, default off): the write-fault
// disarm issues one PAGE_READWRITE call across gaps of pages whose current
// state is known to be host read-write already (unwatched, committed,
// guest-RW, classified under the alias heap try-lock). DisarmRunBuilder is the
// pure run building.
//
// Step 2 (shared_memory_upload_ahead_pages, 0 = off, 16 or 64): an upload of
// an invalid run is extended to the contiguous invalid, CPU-owned, writable
// pages around it inside the same 256 KiB block, up to the cap.
// ExtendUploadRun is the pure range extension.
//
// The pure helpers below are unit-tested in
// tests/unit/memory/watch_census_test.cpp.

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, memory_watch_census);
REXCVAR_DECLARE(bool, memory_watch_probes);
REXCVAR_DECLARE(bool, memory_watch_disarm_bridge_safe_gaps);

namespace rex::memory::watch_census {

inline constexpr uint32_t kVersion = 1;

// ---------------------------------------------------------------------------
// Pages-per-call buckets: 1, 2-4, 5-16, 17-64, >64.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBucketCount = 5;
inline constexpr std::array<std::string_view, kBucketCount> kBucketNames = {"1", "2_4", "5_16",
                                                                            "17_64", "65p"};

constexpr uint32_t PagesBucket(uint64_t pages) {
  if (pages <= 1) {
    return 0;
  }
  if (pages <= 4) {
    return 1;
  }
  if (pages <= 16) {
    return 2;
  }
  if (pages <= 64) {
    return 3;
  }
  return 4;
}

// ---------------------------------------------------------------------------
// Faulting thread classes.
// ---------------------------------------------------------------------------
enum class ThreadClass : uint8_t { kCp = 0, kRender = 1, kOther = 2 };
inline constexpr uint32_t kThreadClassCount = 3;
inline constexpr std::array<std::string_view, kThreadClassCount> kThreadClassNames = {
    "cp", "render", "other"};

constexpr ThreadClass ClassifyThread(uint32_t tid, uint32_t cp_tid, uint32_t render_tid) {
  if (tid != 0 && tid == cp_tid) {
    return ThreadClass::kCp;
  }
  if (tid != 0 && tid == render_tid) {
    return ThreadClass::kRender;
  }
  return ThreadClass::kOther;
}

// ---------------------------------------------------------------------------
// Per-256 KiB-block hotness: an 8-bit history, bit 0 = the frame that just
// closed, set when a CPU write fault invalidated the block in that frame.
// Classes: 0 = faulted in 8 of the last 8 frames, 1 = 4-7, 2 = 1-3, 3 = 0.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kHotnessClassCount = 4;
inline constexpr std::array<std::string_view, kHotnessClassCount> kHotnessClassNames = {
    "h8", "h4_7", "h1_3", "h0"};

constexpr uint32_t PopCount8(uint8_t v) {
  uint32_t n = 0;
  for (; v; v &= uint8_t(v - 1)) {
    ++n;
  }
  return n;
}

constexpr uint32_t HotnessClass(uint8_t history) {
  const uint32_t k = PopCount8(history);
  if (k == 8) {
    return 0;
  }
  if (k >= 4) {
    return 1;
  }
  if (k >= 1) {
    return 2;
  }
  return 3;
}

constexpr uint8_t AdvanceHistory(uint8_t history, bool faulted_this_frame) {
  return uint8_t((history << 1) | (faulted_this_frame ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Step 1: disarm run building. Pages are fed in ascending order, every page of
// the window exactly once.
// - kDisarm: watched and guest read-write: the legacy loop makes it RW.
// - kSafeGap: unwatched, committed and guest read-write, classified under the
//   alias heap try-lock: its host protection is already read-write, so a
//   PAGE_READWRITE call over it changes nothing.
// - kBarrier: anything else, including every unwatched page whose state could
//   not be classified (try-lock miss). Never inside a run.
// With bridging off a kSafeGap page is a barrier, which is exactly the legacy
// run split. With bridging on, a gap of kSafeGap pages between two kDisarm
// pages joins their runs; leading and trailing gap pages are never included.
// ---------------------------------------------------------------------------
enum class DisarmPage : uint8_t { kDisarm, kSafeGap, kBarrier };

struct PageRun {
  uint32_t first = 0;
  uint32_t last = 0;  // inclusive
  constexpr uint32_t count() const { return last - first + 1; }
  constexpr bool operator==(const PageRun&) const = default;
};

class DisarmRunBuilder {
 public:
  constexpr explicit DisarmRunBuilder(bool bridge) : bridge_(bridge) {}

  // Returns true when a run ended before this page; *out is that run and must
  // be emitted (one protection call).
  constexpr bool Feed(uint32_t page, DisarmPage kind, PageRun* out) {
    if (kind == DisarmPage::kSafeGap && bridge_) {
      return false;
    }
    if (kind == DisarmPage::kDisarm) {
      if (!open_) {
        open_ = true;
        run_.first = page;
      } else if (page > run_.last + 1) {
        ++bridged_gaps_;
        bridged_pages_ += page - run_.last - 1;
      }
      run_.last = page;
      return false;
    }
    // kBarrier, or kSafeGap without bridging.
    return Close(out);
  }

  // Returns true when a run is still open; *out is that run.
  constexpr bool Finish(PageRun* out) { return Close(out); }

  constexpr uint64_t bridged_gaps() const { return bridged_gaps_; }
  constexpr uint64_t bridged_pages() const { return bridged_pages_; }

 private:
  constexpr bool Close(PageRun* out) {
    if (!open_) {
      return false;
    }
    open_ = false;
    *out = run_;
    return true;
  }

  bool bridge_;
  bool open_ = false;
  PageRun run_{};
  uint64_t bridged_gaps_ = 0;
  uint64_t bridged_pages_ = 0;
};

// ---------------------------------------------------------------------------
// Step 2: upload-ahead range extension. [page_first, page_last] is an invalid
// run that is being uploaded. blocked_first_block and blocked_last_block are
// the 64-page masks (bit i = page i of the block) of the blocks holding
// page_first and page_last; a set bit is a page that must not be added (valid,
// GPU-written, not committed guest-writable, or of unknown state). The run
// grows forward inside the block of page_last, then backward inside the block
// of page_first, only over contiguous unblocked pages, never past min_page /
// max_page (neighbouring runs of the same request) and only while its length
// is below cap. A run that is already cap pages or longer is returned as is;
// a run never shrinks.
// ---------------------------------------------------------------------------
inline constexpr uint32_t kBlockPagesLog2 = 6;
inline constexpr uint32_t kBlockPages = 1u << kBlockPagesLog2;

constexpr PageRun ExtendUploadRun(uint32_t page_first, uint32_t page_last,
                                  uint64_t blocked_first_block, uint64_t blocked_last_block,
                                  uint32_t min_page, uint32_t max_page, uint32_t cap) {
  PageRun run{page_first, page_last};
  if (cap == 0 || page_last < page_first) {
    return run;
  }
  // Constant time: the contiguous unblocked pages next to the run are counted
  // with one bit scan per direction.
  const uint32_t forward_limit = std::min<uint32_t>(page_last | (kBlockPages - 1), max_page);
  if (run.count() < cap && run.last < forward_limit) {
    // run.last is below the block end here, so the shift is 1..63.
    const uint64_t free_above = ~blocked_last_block >> ((run.last & (kBlockPages - 1)) + 1);
    const uint32_t contiguous = uint32_t(std::countr_one(free_above));
    run.last += std::min({contiguous, forward_limit - run.last, cap - run.count()});
  }
  const uint32_t backward_limit = std::max<uint32_t>(page_first & ~(kBlockPages - 1), min_page);
  if (run.count() < cap && run.first > backward_limit) {
    // run.first is above the block start here, so the shift is 1..63.
    const uint64_t free_below = ~blocked_first_block
                                << (kBlockPages - (run.first & (kBlockPages - 1)));
    const uint32_t contiguous = uint32_t(std::countl_one(free_below));
    run.first -= std::min({contiguous, run.first - backward_limit, cap - run.count()});
  }
  return run;
}

// shared_memory_upload_ahead_pages: only 16 and 64 enable it; anything else is
// off.
constexpr uint32_t SanitizeUploadAheadPages(int32_t value) {
  return (value == 16 || value == 64) ? uint32_t(value) : 0u;
}

constexpr uint64_t WordMask(uint32_t word, uint32_t first, uint32_t last) {
  uint64_t mask = UINT64_MAX;
  if (word == (first >> 6)) {
    mask &= ~((uint64_t(1) << (first & 63)) - 1);
  }
  if (word == (last >> 6) && (last & 63) != 63) {
    mask &= (uint64_t(1) << ((last & 63) + 1)) - 1;
  }
  return mask;
}

// Whether every page of [first, last] is set in a page bitmap.
constexpr bool AllPagesSet(const uint64_t* bits, uint32_t first, uint32_t last) {
  for (uint32_t word = first >> 6; word <= (last >> 6); ++word) {
    const uint64_t mask = WordMask(word, first, last);
    if ((bits[word] & mask) != mask) {
      return false;
    }
  }
  return true;
}

// Sets [first, last] in a page bitmap; returns how many bits were newly set.
constexpr uint64_t SetPages(uint64_t* bits, uint32_t first, uint32_t last) {
  uint64_t added = 0;
  for (uint32_t word = first >> 6; word <= (last >> 6); ++word) {
    const uint64_t mask = WordMask(word, first, last);
    for (uint64_t fresh = mask & ~bits[word]; fresh; fresh &= fresh - 1) {
      ++added;
    }
    bits[word] |= mask;
  }
  return added;
}

constexpr void ClearPages(uint64_t* bits, uint32_t first, uint32_t last) {
  for (uint32_t word = first >> 6; word <= (last >> 6); ++word) {
    bits[word] &= ~WordMask(word, first, last);
  }
}

// ---------------------------------------------------------------------------
// Runtime counters of the memory side (rexruntime). Relaxed atomics; ticks are
// rdtsc ticks, converted at report time.
// ---------------------------------------------------------------------------
struct Snapshot {
  // [0] = arm (read-only), [1] = disarm (read-write).
  uint64_t protect_calls[2][kBucketCount] = {};
  uint64_t protect_ticks[2][kBucketCount] = {};
  uint64_t protect_pages[2] = {};
  // Global critical region held: from the arm walk's lock acquisition to its
  // end, and the disarm walk (the fault handler holds the lock throughout).
  uint64_t arm_hold_ticks = 0;
  uint64_t arm_walks = 0;
  uint64_t disarm_hold_ticks = 0;
  uint64_t disarm_walks = 0;
  // Step 1: actual bridged gaps (bridging on) and the shadow projection
  // (bridging off: disarm calls bridging would have saved).
  uint64_t bridged_gaps = 0;
  uint64_t bridged_pages = 0;
  uint64_t bridge_shadow_saved_calls = 0;
  uint64_t bridge_shadow_pages = 0;
  uint64_t gap_lock_misses = 0;
  // Successfully handled non-MMIO write faults by faulting thread class.
  uint64_t faults[kThreadClassCount] = {};
  uint64_t fault_us[kThreadClassCount] = {};
  uint64_t fault_query_us[kThreadClassCount] = {};
  uint64_t fault_lock_wait_us[kThreadClassCount] = {};
};

// Field-wise a - b (counters only grow).
Snapshot Difference(const Snapshot& a, const Snapshot& b);

// Formats the memory-side fields of an MCLA_WATCH line (no leading space).
// tsc_mhz converts ticks to ms (0: ticks are printed as 0 ms).
std::string FormatMemoryFields(const Snapshot& window, double tsc_mhz);

// Runtime state (rexruntime). Enabled() reads memory_watch_census.
bool Enabled();
void RecordProtect(bool arm, uint64_t pages, uint64_t ticks);
void RecordArmHold(uint64_t ticks);
void RecordDisarmHold(uint64_t ticks);
void RecordBridge(uint64_t gaps, uint64_t pages);
void RecordBridgeShadow(uint64_t gaps, uint64_t pages);
void RecordGapLockMiss();
// Called by the fault handler for every successfully handled write fault.
void RecordFault(uint64_t total_us, uint64_t query_us, uint64_t lock_wait_us);
// The command processor thread (its system thread id).
void SetCommandProcessorThread(uint32_t tid);
uint32_t CommandProcessorThread();
Snapshot GetSnapshot();

// F10 probes (memory_watch_probes): protect round trips on a private page and
// on section_page (a host page of the guest physical section view that the
// guest never uses), timed write faults on a private page (rdtsc around the
// store, including the OS/Wine exception dispatch), and under Wine the line
// count of /proc/self/maps. Returns the fields of the probe line.
std::string RunProbes(void* section_page, double tsc_mhz);

}  // namespace rex::memory::watch_census
