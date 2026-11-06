/**
 * @file        content_log.cpp
 * @brief       Rate-limited content/save WARN lines and the transaction
 *              indicator (MCLA KI-065). See rex/content_log.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/content_log.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(
    xam_content_trace, true, "XAM",
    "Log one INFO line per content/save call on the success path (XamContentCreate*, "
    "Close/Flush, device queries, device selector, enumerators, XN_SYS_* broadcasts, "
    "content-device file opens/closes); rate-limited, no host paths (M11-001)");

REXCVAR_DEFINE_BOOL(xam_content_stable_creation_time, true, "XAM",
                    "Report a constant creation time for each content (save) file within a "
                    "process, as NTFS does, even when the host (Wine on ext4) reports a value that "
                    "changes with writes; MCLA treats a changed creation time as a removed storage "
                    "device (M11-001, KI-068)");

namespace rex::content_log {

bool LineBudget::Allow(uint64_t now_ms) {
  // Tokens are kept in thousandths so a 1 line/s refill is exact per ms.
  const uint64_t burst_milli = uint64_t(burst_) * 1000;
  if (!started_) {
    started_ = true;
    milli_tokens_ = burst_milli;
    last_ms_ = now_ms;
  } else if (now_ms > last_ms_) {
    const uint64_t elapsed = now_ms - last_ms_;
    const uint64_t refill = elapsed >= burst_milli ? burst_milli : elapsed * per_second_;
    milli_tokens_ = milli_tokens_ + refill >= burst_milli ? burst_milli : milli_tokens_ + refill;
    last_ms_ = now_ms;
  } else if (now_ms < last_ms_) {
    // The clock went backwards: restart the reference without refilling.
    last_ms_ = now_ms;
  }
  if (milli_tokens_ >= 1000) {
    milli_tokens_ -= 1000;
    return true;
  }
  ++suppressed_;
  return false;
}

uint64_t LineBudget::TakeSuppressed() {
  const uint64_t suppressed = suppressed_;
  suppressed_ = 0;
  return suppressed;
}

namespace {

constexpr size_t kKindCount = static_cast<size_t>(Kind::kCount);

struct Budgets {
  std::mutex mutex;
  std::array<LineBudget, kKindCount> budgets{};
};

Budgets& GetBudgets() {
  static Budgets budgets;
  return budgets;
}

uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

std::atomic<uint32_t> g_active_transactions{0};
std::atomic<uint32_t> g_busy_operations{0};

}  // namespace

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kCreate:
      return "create";
    case Kind::kClose:
      return "close";
    case Kind::kFlush:
      return "flush";
    case Kind::kDelete:
      return "delete";
    case Kind::kTransaction:
      return "transaction";
    case Kind::kHeader:
      return "header";
    case Kind::kFileOpen:
      return "file-open";
    case Kind::kFileWrite:
      return "file-write";
    case Kind::kFileSetInfo:
      return "file-setinfo";
    case Kind::kDevice:
      return "device";
    case Kind::kUnmount:
      return "unmount";
    case Kind::kStaleIo:
      return "stale-io";
    case Kind::kMoveAside:
      return "move-aside";
    case Kind::kSaveOk:
      return "save-ok";
    case Kind::kTraceCreate:
      return "trace-create";
    case Kind::kTraceClose:
      return "trace-close";
    case Kind::kTraceDevice:
      return "trace-device";
    case Kind::kTraceNotify:
      return "trace-notify";
    case Kind::kTraceUi:
      return "trace-ui";
    case Kind::kTraceEnum:
      return "trace-enum";
    case Kind::kTraceFile:
      return "trace-file";
    case Kind::kTimestamp:
      return "timestamp";
    default:
      return "unknown";
  }
}

bool AllowAt(Kind kind, uint64_t now_ms, uint64_t* suppressed) {
  const size_t index = static_cast<size_t>(kind);
  if (index >= kKindCount) {
    return false;
  }
  auto& state = GetBudgets();
  std::lock_guard<std::mutex> lock(state.mutex);
  auto& budget = state.budgets[index];
  if (!budget.Allow(now_ms)) {
    return false;
  }
  const uint64_t dropped = budget.TakeSuppressed();
  if (suppressed) {
    *suppressed = dropped;
  }
  return true;
}

bool Allow(Kind kind, uint64_t* suppressed) {
  return AllowAt(kind, NowMs(), suppressed);
}

namespace {

struct ChangeEntry {
  uint64_t value = 0;
  uint64_t last_call_ms = 0;
  uint64_t swallowed = 0;
};

struct ChangeState {
  std::mutex mutex;
  std::unordered_map<std::string, ChangeEntry> entries;
};

ChangeState& GetChangeState() {
  static ChangeState state;
  return state;
}

std::atomic<int> g_trace_override{-1};

}  // namespace

void ResetForTesting() {
  {
    auto& state = GetBudgets();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& budget : state.budgets) {
      budget = LineBudget();
    }
  }
  auto& changes = GetChangeState();
  std::lock_guard<std::mutex> lock(changes.mutex);
  changes.entries.clear();
}

bool TraceEnabled() {
  const int override_value = g_trace_override.load(std::memory_order_relaxed);
  if (override_value >= 0) {
    return override_value != 0;
  }
  return REXCVAR_GET(xam_content_trace);
}

void SetTraceEnabledForTesting(bool enabled) {
  g_trace_override.store(enabled ? 1 : 0, std::memory_order_relaxed);
}

bool AllowChangedAt(std::string_view key, uint64_t value, uint64_t now_ms, uint64_t gap_ms,
                    uint64_t* repeats) {
  auto& state = GetChangeState();
  std::lock_guard<std::mutex> lock(state.mutex);
  auto [it, inserted] = state.entries.try_emplace(std::string(key));
  ChangeEntry& entry = it->second;
  const bool gap =
      !inserted && (now_ms < entry.last_call_ms || now_ms - entry.last_call_ms >= gap_ms);
  const bool allow = inserted || entry.value != value || gap;
  entry.last_call_ms = now_ms;
  if (!allow) {
    ++entry.swallowed;
    return false;
  }
  entry.value = value;
  if (repeats) {
    *repeats = entry.swallowed;
  }
  entry.swallowed = 0;
  return true;
}

bool AllowChanged(std::string_view key, uint64_t value, uint64_t* repeats) {
  return AllowChangedAt(key, value, NowMs(), kDefaultTraceGapMs, repeats);
}

namespace {

struct StableTimes {
  std::mutex mutex;
  std::unordered_map<std::string, uint64_t> times;
};

StableTimes& GetStableTimes() {
  static StableTimes state;
  return state;
}

}  // namespace

bool StableCreationTimeEnabled() {
  return REXCVAR_GET(xam_content_stable_creation_time);
}

uint64_t StableCreationTime(const std::string& key, uint64_t observed, bool* remembered) {
  auto& state = GetStableTimes();
  std::lock_guard<std::mutex> lock(state.mutex);
  auto [it, inserted] = state.times.try_emplace(key, observed);
  if (remembered) {
    *remembered = !inserted;
  }
  return it->second;
}

void SetStableCreationTime(const std::string& key, uint64_t value) {
  auto& state = GetStableTimes();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.times[key] = value;
}

void ResetStableCreationTimesForTesting() {
  auto& state = GetStableTimes();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.times.clear();
}

bool RunningUnderWine() {
#if REX_PLATFORM_WIN32
  static const bool under_wine = [] {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
  }();
  return under_wine;
#else
  return false;
#endif
}

uint32_t LastSystemError() {
#if REX_PLATFORM_WIN32
  return uint32_t(GetLastError());
#else
  return uint32_t(errno);
#endif
}

std::string SystemErrorText(uint32_t error) {
  if (error == 0) {
    return "none";
  }
#if REX_PLATFORM_WIN32
  char buffer[256] = {};
  const DWORD length = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                                      buffer, DWORD(sizeof(buffer)), nullptr);
  std::string text(buffer, length);
#else
  std::string text = std::strerror(int(error));
#endif
  while (!text.empty() &&
         (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
    text.pop_back();
  }
  return text.empty() ? "unknown" : text;
}

bool IsContentDevicePath(std::string_view guest_path) {
  constexpr std::string_view kPrefix = "\\Device\\Content\\";
  if (guest_path.size() < kPrefix.size()) {
    return false;
  }
  for (size_t i = 0; i < kPrefix.size(); ++i) {
    char c = guest_path[i];
    char p = kPrefix[i];
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
    if (p >= 'A' && p <= 'Z') {
      p = char(p - 'A' + 'a');
    }
    if (c != p) {
      return false;
    }
  }
  return true;
}

uint32_t TransactionsActive() {
  return g_active_transactions.load(std::memory_order_acquire) +
         g_busy_operations.load(std::memory_order_acquire);
}

void NoteTransactionBegun() {
  g_active_transactions.fetch_add(1, std::memory_order_acq_rel);
}

void NoteTransactionsEnded(uint32_t count) {
  uint32_t current = g_active_transactions.load(std::memory_order_acquire);
  uint32_t next;
  do {
    next = current >= count ? current - count : 0;
  } while (!g_active_transactions.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                                        std::memory_order_acquire));
}

BusyScope::BusyScope() {
  g_busy_operations.fetch_add(1, std::memory_order_acq_rel);
}

BusyScope::~BusyScope() {
  g_busy_operations.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace rex::content_log

extern "C" uint32_t rex_xam_content_transactions_active(void) {
  return rex::content_log::TransactionsActive();
}
