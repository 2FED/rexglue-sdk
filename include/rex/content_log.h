/**
 * @file        rex/content_log.h
 * @brief       Rate-limited WARN lines for the content/save path and the
 *              "content transaction active" indicator (MCLA KI-065).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * Every non-success in the XAM content/save path (XamContentCreate*,
 * XamContentClose, XamContentFlush, the saved-game transaction layer, failed
 * host file operations on content devices and the storage-device queries)
 * writes one WARN line with the prefix "[content]". Each kind is paced
 * independently: a burst of 16 lines, then 1 line per second; the next line
 * written after a suppression reports how many lines were dropped.
 *
 * Lines name the content package (its file name and root name), never the host
 * path, so they carry no Windows/Steam user name.
 *
 * The transaction indicator counts saved-game transactions between their
 * Begin and Commit/Rollback plus content-layer operations that are mutating
 * the save tree right now (recovery, backup copies). A host that copies the
 * live save tree (e.g. a diagnostics export) can poll it and stay out of the
 * way. The C entry point rex_xam_content_transactions_active() is stable for
 * hosts that declare it themselves with a link-time fallback.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <rex/logging.h>

namespace rex::content_log {

constexpr uint32_t kDefaultBurst = 16;
constexpr uint32_t kDefaultPerSecond = 1;

// Token bucket: up to `burst` lines at once, refilled at `per_second` lines
// per second. Not thread-safe; Allow(Kind, ...) serializes access.
class LineBudget {
 public:
  LineBudget() = default;
  LineBudget(uint32_t burst, uint32_t per_second) : burst_(burst), per_second_(per_second) {}

  bool Allow(uint64_t now_ms);
  // Lines refused since the last call; resets the count.
  uint64_t TakeSuppressed();

  uint32_t burst() const { return burst_; }
  uint32_t per_second() const { return per_second_; }

 private:
  uint32_t burst_ = kDefaultBurst;
  uint32_t per_second_ = kDefaultPerSecond;
  bool started_ = false;
  uint64_t last_ms_ = 0;
  uint64_t milli_tokens_ = 0;
  uint64_t suppressed_ = 0;
};

enum class Kind : uint32_t {
  kCreate = 0,   // XamContentCreate / CreateEx / CreateInternal
  kClose,        // XamContentClose
  kFlush,        // XamContentFlush
  kDelete,       // XamContentDelete*, ContentManager::DeleteContent
  kTransaction,  // Begin / Recover / Commit / Rollback
  kHeader,       // .header write / package directory create / thumbnail
  kFileOpen,     // host file open/create/delete on a content device
  kFileWrite,    // NtWriteFile / host WriteFile on a content device
  kFileSetInfo,  // NtSetInformationFile / host SetLength on a content device
  kDevice,       // storage device queries and enumerators
  kCount,
};

const char* KindName(Kind kind);

// Thread-safe pacing shared by every content log site. Returns true when a
// line of `kind` may be written now; `*suppressed` receives the number of lines
// of that kind refused since the previous allowed one.
bool Allow(Kind kind, uint64_t* suppressed);
bool AllowAt(Kind kind, uint64_t now_ms, uint64_t* suppressed);
// Tests only: restores every kind to a fresh budget.
void ResetForTesting();

// The last OS error of the calling thread (GetLastError on Windows, errno
// elsewhere). Call it immediately after the failed call.
uint32_t LastSystemError();
std::string SystemErrorText(uint32_t error);

// True for guest absolute paths and mount paths on a content package device
// ("\Device\Content\<n>\...").
bool IsContentDevicePath(std::string_view guest_path);

template <typename... Args>
void Warn(Kind kind, fmt::format_string<Args...> format, Args&&... args) {
  uint64_t suppressed = 0;
  if (!Allow(kind, &suppressed)) {
    return;
  }
  const std::string text = fmt::format(format, std::forward<Args>(args)...);
  if (suppressed) {
    REXSYS_WARN("[content] {} (+{} earlier {} lines suppressed)", text, suppressed, KindName(kind));
  } else {
    REXSYS_WARN("[content] {}", text);
  }
}

// --- Transaction indicator ------------------------------------------------

// Saved-game transactions begun and not yet committed or rolled back, plus
// content operations currently mutating the save tree.
uint32_t TransactionsActive();
void NoteTransactionBegun();
void NoteTransactionsEnded(uint32_t count = 1);

// Marks a content-layer operation that mutates the save tree (recovery,
// backup copies, commit) for its lifetime.
class BusyScope {
 public:
  BusyScope();
  ~BusyScope();
  BusyScope(const BusyScope&) = delete;
  BusyScope& operator=(const BusyScope&) = delete;
};

}  // namespace rex::content_log

// Stable C entry point of the indicator for hosts (exported from rexruntime).
extern "C" uint32_t rex_xam_content_transactions_active(void);
