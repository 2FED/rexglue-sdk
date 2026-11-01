/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    MCLA-R, 2026 - M10 Deck watch/upload census (steps 0-2)
 */

#include <rex/system/watch_census.h>

#include <atomic>
#include <vector>

#include <fmt/format.h>

#include <rex/exception_handler.h>
#include <rex/memory/utils.h>
#include <rex/perf/perf_split.h>
#include <rex/thread.h>

#if defined(_WIN32)
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(memory_watch_census, true, "Memory",
                    "M10 watch census (counters only): protect calls and time by kind and "
                    "pages per call, global-lock hold, faults by thread class, per-256 KiB "
                    "block hotness, request sources and the step 1/2 shadow projections; one "
                    "MCLA_WATCH v=1 line per minute and per F10 mark")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(memory_watch_probes, false, "Memory",
                    "With memory_watch_census: at each F10 mark also run the watch probes "
                    "(1000 protect round trips on a private page and on an unused page of the "
                    "guest physical section view, 8 timed write faults on a private page, and "
                    "under Wine the /proc/self/maps line count); one MCLA_WATCH kind=probe line")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(memory_watch_disarm_bridge_safe_gaps, false, "Memory",
                    "M10 step 1: a write-fault disarm makes one read-write protection call "
                    "across gaps of unwatched, committed, guest read-write pages (already host "
                    "read-write, classified under the alias heap try-lock; on a try-lock miss "
                    "the legacy split is kept)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::memory::watch_census {
namespace {

struct Counters {
  std::atomic<uint64_t> protect_calls[2][kBucketCount] = {};
  std::atomic<uint64_t> protect_ticks[2][kBucketCount] = {};
  std::atomic<uint64_t> protect_pages[2] = {};
  std::atomic<uint64_t> arm_hold_ticks{0};
  std::atomic<uint64_t> arm_walks{0};
  std::atomic<uint64_t> disarm_hold_ticks{0};
  std::atomic<uint64_t> disarm_walks{0};
  std::atomic<uint64_t> bridged_gaps{0};
  std::atomic<uint64_t> bridged_pages{0};
  std::atomic<uint64_t> bridge_shadow_saved_calls{0};
  std::atomic<uint64_t> bridge_shadow_pages{0};
  std::atomic<uint64_t> gap_lock_misses{0};
  std::atomic<uint64_t> faults[kThreadClassCount] = {};
  std::atomic<uint64_t> fault_us[kThreadClassCount] = {};
  std::atomic<uint64_t> fault_query_us[kThreadClassCount] = {};
  std::atomic<uint64_t> fault_lock_wait_us[kThreadClassCount] = {};
};

Counters g_counters;
std::atomic<uint32_t> g_cp_tid{0};

inline void Add(std::atomic<uint64_t>& counter, uint64_t value) {
  counter.fetch_add(value, std::memory_order_relaxed);
}

inline double TicksMs(uint64_t ticks, double tsc_mhz) {
  return tsc_mhz > 0.0 ? double(ticks) / (tsc_mhz * 1000.0) : 0.0;
}

// Probe write-fault state. The probe handler is installed once and stays
// installed; it handles only an access violation inside g_probe_page.
std::atomic<uint8_t*> g_probe_page{nullptr};
std::atomic<uint64_t> g_probe_handler_ticks{0};
std::atomic<bool> g_probe_handler_installed{false};

bool ProbeExceptionHandler(rex::arch::Exception* ex, void*) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) {
    return false;
  }
  uint8_t* page = g_probe_page.load(std::memory_order_acquire);
  if (!page) {
    return false;
  }
  const uint64_t address = ex->fault_address();
  const size_t page_size = rex::memory::page_size();
  if (address < uint64_t(reinterpret_cast<uintptr_t>(page)) ||
      address >= uint64_t(reinterpret_cast<uintptr_t>(page)) + page_size) {
    return false;
  }
  const uint64_t begin = rex::perf::ReadTsc();
  const bool restored = rex::memory::Protect(page, page_size, rex::memory::PageAccess::kReadWrite);
  g_probe_handler_ticks.store(rex::perf::ReadTsc() - begin, std::memory_order_relaxed);
  return restored;
}

bool UnderWine() {
#if defined(_WIN32)
  HMODULE ntdll = GetModuleHandleA("ntdll.dll");
  return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
#else
  return false;
#endif
}

// Line count of /proc/self/maps (VMA count) through Wine's Z: drive; -1 when
// not under Wine or unreadable.
int64_t ProcSelfMapsLines() {
  if (!UnderWine()) {
    return -1;
  }
#if defined(_WIN32)
  HANDLE file = CreateFileW(L"Z:\\proc\\self\\maps", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return -1;
  }
  int64_t lines = 0;
  std::vector<char> buffer(65536);
  DWORD read = 0;
  while (ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr) && read != 0) {
    for (DWORD i = 0; i < read; ++i) {
      lines += buffer[i] == '\n';
    }
  }
  CloseHandle(file);
  return lines;
#else
  return -1;
#endif
}

// Average microseconds per protection call over count read-only/read-write
// round trips (2 calls each); -1 on any failure. Leaves the page read-write.
double ProtectRoundTripUs(void* page, uint32_t count, double tsc_mhz) {
  const size_t page_size = rex::memory::page_size();
  const uint64_t begin = rex::perf::ReadTsc();
  for (uint32_t i = 0; i < count; ++i) {
    if (!rex::memory::Protect(page, page_size, rex::memory::PageAccess::kReadOnly) ||
        !rex::memory::Protect(page, page_size, rex::memory::PageAccess::kReadWrite)) {
      rex::memory::Protect(page, page_size, rex::memory::PageAccess::kReadWrite);
      return -1.0;
    }
  }
  const uint64_t ticks = rex::perf::ReadTsc() - begin;
  return TicksMs(ticks, tsc_mhz) * 1000.0 / double(count * 2);
}

}  // namespace

Snapshot Difference(const Snapshot& a, const Snapshot& b) {
  Snapshot d;
  for (uint32_t k = 0; k < 2; ++k) {
    for (uint32_t i = 0; i < kBucketCount; ++i) {
      d.protect_calls[k][i] = a.protect_calls[k][i] - b.protect_calls[k][i];
      d.protect_ticks[k][i] = a.protect_ticks[k][i] - b.protect_ticks[k][i];
    }
    d.protect_pages[k] = a.protect_pages[k] - b.protect_pages[k];
  }
  d.arm_hold_ticks = a.arm_hold_ticks - b.arm_hold_ticks;
  d.arm_walks = a.arm_walks - b.arm_walks;
  d.disarm_hold_ticks = a.disarm_hold_ticks - b.disarm_hold_ticks;
  d.disarm_walks = a.disarm_walks - b.disarm_walks;
  d.bridged_gaps = a.bridged_gaps - b.bridged_gaps;
  d.bridged_pages = a.bridged_pages - b.bridged_pages;
  d.bridge_shadow_saved_calls = a.bridge_shadow_saved_calls - b.bridge_shadow_saved_calls;
  d.bridge_shadow_pages = a.bridge_shadow_pages - b.bridge_shadow_pages;
  d.gap_lock_misses = a.gap_lock_misses - b.gap_lock_misses;
  for (uint32_t c = 0; c < kThreadClassCount; ++c) {
    d.faults[c] = a.faults[c] - b.faults[c];
    d.fault_us[c] = a.fault_us[c] - b.fault_us[c];
    d.fault_query_us[c] = a.fault_query_us[c] - b.fault_query_us[c];
    d.fault_lock_wait_us[c] = a.fault_lock_wait_us[c] - b.fault_lock_wait_us[c];
  }
  return d;
}

std::string FormatMemoryFields(const Snapshot& w, double tsc_mhz) {
  std::string out;
  static constexpr std::string_view kKinds[2] = {"arm", "disarm"};
  for (uint32_t k = 0; k < 2; ++k) {
    uint64_t calls = 0, ticks = 0;
    for (uint32_t i = 0; i < kBucketCount; ++i) {
      calls += w.protect_calls[k][i];
      ticks += w.protect_ticks[k][i];
    }
    fmt::format_to(std::back_inserter(out), "{}{}_calls={} {}_ms={:.1f} {}_pages={}", k ? " " : "",
                   kKinds[k], calls, kKinds[k], TicksMs(ticks, tsc_mhz), kKinds[k],
                   w.protect_pages[k]);
    fmt::format_to(std::back_inserter(out), " {}_calls_by_pages=", kKinds[k]);
    for (uint32_t i = 0; i < kBucketCount; ++i) {
      fmt::format_to(std::back_inserter(out), "{}{}:{}", i ? "," : "", kBucketNames[i],
                     w.protect_calls[k][i]);
    }
    fmt::format_to(std::back_inserter(out), " {}_ms_by_pages=", kKinds[k]);
    for (uint32_t i = 0; i < kBucketCount; ++i) {
      fmt::format_to(std::back_inserter(out), "{}{}:{:.1f}", i ? "," : "", kBucketNames[i],
                     TicksMs(w.protect_ticks[k][i], tsc_mhz));
    }
  }
  fmt::format_to(std::back_inserter(out),
                 " arm_hold_ms={:.1f} arm_walks={} disarm_hold_ms={:.1f} disarm_walks={}",
                 TicksMs(w.arm_hold_ticks, tsc_mhz), w.arm_walks,
                 TicksMs(w.disarm_hold_ticks, tsc_mhz), w.disarm_walks);
  fmt::format_to(std::back_inserter(out),
                 " bridged_gaps={} bridged_pages={} bridge_shadow_saved_calls={} "
                 "bridge_shadow_pages={} gap_lock_misses={}",
                 w.bridged_gaps, w.bridged_pages, w.bridge_shadow_saved_calls,
                 w.bridge_shadow_pages, w.gap_lock_misses);
  for (uint32_t c = 0; c < kThreadClassCount; ++c) {
    fmt::format_to(std::back_inserter(out),
                   " faults_{}={} fault_ms_{}={:.1f} fault_query_ms_{}={:.1f} "
                   "fault_lock_wait_ms_{}={:.1f}",
                   kThreadClassNames[c], w.faults[c], kThreadClassNames[c],
                   double(w.fault_us[c]) / 1000.0, kThreadClassNames[c],
                   double(w.fault_query_us[c]) / 1000.0, kThreadClassNames[c],
                   double(w.fault_lock_wait_us[c]) / 1000.0);
  }
  return out;
}

bool Enabled() {
  return REXCVAR_GET(memory_watch_census);
}

void RecordProtect(bool arm, uint64_t pages, uint64_t ticks) {
  const uint32_t k = arm ? 0 : 1;
  const uint32_t bucket = PagesBucket(pages);
  Add(g_counters.protect_calls[k][bucket], 1);
  Add(g_counters.protect_ticks[k][bucket], ticks);
  Add(g_counters.protect_pages[k], pages);
}

void RecordArmHold(uint64_t ticks) {
  Add(g_counters.arm_hold_ticks, ticks);
  Add(g_counters.arm_walks, 1);
}

void RecordDisarmHold(uint64_t ticks) {
  Add(g_counters.disarm_hold_ticks, ticks);
  Add(g_counters.disarm_walks, 1);
}

void RecordBridge(uint64_t gaps, uint64_t pages) {
  if (gaps) {
    Add(g_counters.bridged_gaps, gaps);
    Add(g_counters.bridged_pages, pages);
  }
}

void RecordBridgeShadow(uint64_t gaps, uint64_t pages) {
  if (gaps) {
    Add(g_counters.bridge_shadow_saved_calls, gaps);
    Add(g_counters.bridge_shadow_pages, pages);
  }
}

void RecordGapLockMiss() {
  Add(g_counters.gap_lock_misses, 1);
}

void RecordFault(uint64_t total_us, uint64_t query_us, uint64_t lock_wait_us) {
  const uint32_t c = uint32_t(ClassifyThread(rex::thread::current_thread_system_id(),
                                             g_cp_tid.load(std::memory_order_relaxed),
                                             rex::perf::PerfSplitRenderThreadId()));
  Add(g_counters.faults[c], 1);
  Add(g_counters.fault_us[c], total_us);
  Add(g_counters.fault_query_us[c], query_us);
  Add(g_counters.fault_lock_wait_us[c], lock_wait_us);
}

void SetCommandProcessorThread(uint32_t tid) {
  if (g_cp_tid.load(std::memory_order_relaxed) != tid) {
    g_cp_tid.store(tid, std::memory_order_relaxed);
  }
}

uint32_t CommandProcessorThread() {
  return g_cp_tid.load(std::memory_order_relaxed);
}

Snapshot GetSnapshot() {
  Snapshot s;
  for (uint32_t k = 0; k < 2; ++k) {
    for (uint32_t i = 0; i < kBucketCount; ++i) {
      s.protect_calls[k][i] = g_counters.protect_calls[k][i].load(std::memory_order_relaxed);
      s.protect_ticks[k][i] = g_counters.protect_ticks[k][i].load(std::memory_order_relaxed);
    }
    s.protect_pages[k] = g_counters.protect_pages[k].load(std::memory_order_relaxed);
  }
  s.arm_hold_ticks = g_counters.arm_hold_ticks.load(std::memory_order_relaxed);
  s.arm_walks = g_counters.arm_walks.load(std::memory_order_relaxed);
  s.disarm_hold_ticks = g_counters.disarm_hold_ticks.load(std::memory_order_relaxed);
  s.disarm_walks = g_counters.disarm_walks.load(std::memory_order_relaxed);
  s.bridged_gaps = g_counters.bridged_gaps.load(std::memory_order_relaxed);
  s.bridged_pages = g_counters.bridged_pages.load(std::memory_order_relaxed);
  s.bridge_shadow_saved_calls =
      g_counters.bridge_shadow_saved_calls.load(std::memory_order_relaxed);
  s.bridge_shadow_pages = g_counters.bridge_shadow_pages.load(std::memory_order_relaxed);
  s.gap_lock_misses = g_counters.gap_lock_misses.load(std::memory_order_relaxed);
  for (uint32_t c = 0; c < kThreadClassCount; ++c) {
    s.faults[c] = g_counters.faults[c].load(std::memory_order_relaxed);
    s.fault_us[c] = g_counters.fault_us[c].load(std::memory_order_relaxed);
    s.fault_query_us[c] = g_counters.fault_query_us[c].load(std::memory_order_relaxed);
    s.fault_lock_wait_us[c] = g_counters.fault_lock_wait_us[c].load(std::memory_order_relaxed);
  }
  return s;
}

std::string RunProbes(void* section_page, double tsc_mhz) {
  constexpr uint32_t kRoundTrips = 1000;
  constexpr uint32_t kFaults = 8;
  const size_t page_size = rex::memory::page_size();
  double private_us = -1.0;
  double section_us = -1.0;
  double fault_first_us = -1.0, fault_min_us = -1.0, fault_max_us = -1.0;
  double fault_handler_protect_us = -1.0;
  void* private_page =
      rex::memory::AllocFixed(nullptr, page_size, rex::memory::AllocationType::kReserveCommit,
                              rex::memory::PageAccess::kReadWrite);
  if (private_page) {
    private_us = ProtectRoundTripUs(private_page, kRoundTrips, tsc_mhz);
  }
  if (section_page) {
    // Only a page that is host read-write now (the reserved tail of the
    // physical view); it is left read-write.
    size_t length = page_size;
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    if (rex::memory::QueryProtect(section_page, length, access) &&
        access == rex::memory::PageAccess::kReadWrite) {
      section_us = ProtectRoundTripUs(section_page, kRoundTrips, tsc_mhz);
    }
  }
  if (private_page && tsc_mhz > 0.0) {
    if (!g_probe_handler_installed.exchange(true)) {
      rex::arch::ExceptionHandler::Install(ProbeExceptionHandler, nullptr);
    }
    uint64_t handler_ticks_sum = 0;
    uint32_t done = 0;
    for (uint32_t i = 0; i < kFaults; ++i) {
      if (!rex::memory::Protect(private_page, page_size, rex::memory::PageAccess::kReadOnly)) {
        break;
      }
      g_probe_page.store(static_cast<uint8_t*>(private_page), std::memory_order_release);
      const uint64_t begin = rex::perf::ReadTsc();
      *static_cast<volatile uint8_t*>(private_page) = uint8_t(i);
      const uint64_t ticks = rex::perf::ReadTsc() - begin;
      g_probe_page.store(nullptr, std::memory_order_release);
      const double us = TicksMs(ticks, tsc_mhz) * 1000.0;
      handler_ticks_sum += g_probe_handler_ticks.load(std::memory_order_relaxed);
      if (done == 0) {
        fault_first_us = fault_min_us = fault_max_us = us;
      } else {
        fault_min_us = std::min(fault_min_us, us);
        fault_max_us = std::max(fault_max_us, us);
      }
      ++done;
    }
    if (done) {
      fault_handler_protect_us = TicksMs(handler_ticks_sum, tsc_mhz) * 1000.0 / double(done);
    }
  }
  if (private_page) {
    rex::memory::DeallocFixed(private_page, 0, rex::memory::DeallocationType::kRelease);
  }
  return fmt::format(
      "wine={} protect_us_private={:.2f} protect_us_section={:.2f} round_trips={} "
      "fault_us_first={:.1f} fault_us_min={:.1f} fault_us_max={:.1f} faults={} "
      "fault_handler_protect_us={:.2f} maps_lines={}",
      UnderWine() ? 1 : 0, private_us, section_us, kRoundTrips, fault_first_us, fault_min_us,
      fault_max_us, kFaults, fault_handler_protect_us, ProcSelfMapsLines());
}

}  // namespace rex::memory::watch_census
