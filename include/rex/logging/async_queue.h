/**
 * @file        rex/logging/async_queue.h
 * @brief       Bounded, non-blocking log queue and sink timing (pure parts)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @modified    MCLA-R, 2026 - KI-069 asynchronous logging
 */

#pragma once

// KI-069: the default sinks are written by one writer thread. Producers only
// append to a bounded queue; a full queue never blocks them. Everything here
// is pure (no locks, no clocks, no OS calls); the dispatcher in
// src/core/logging.cpp adds the mutex, the clock and the writer thread.
// Unit-tested in tests/unit/core/log_async_test.cpp.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace rex::logging {

// Outcome of one push.
enum class LogPushResult : uint8_t {
  kAccepted,       // queued; nothing was dropped
  kDroppedNew,     // queue full: this (ordinary) line was dropped
  kEvictedOldest,  // queue full: an important line evicted the oldest lines
};

struct LogQueueLimits {
  size_t max_lines = 16384;
  size_t max_bytes = 16u * 1024u * 1024u;
};

// Overflow policy:
// - An ordinary line (below warn) that does not fit is dropped (drop newest),
//   so the queued lines stay one contiguous, ordered run.
// - An important line (warn and above) that does not fit evicts the oldest
//   queued lines until it fits, so a warning or error near a stall survives.
// - An important line is always accepted into an empty queue, even when it
//   alone exceeds max_bytes.
// The queue is FIFO, so the order of the lines of every producer thread is
// kept (and the global order of the pushes).
template <typename T>
class BoundedLogQueue {
 public:
  struct Entry {
    T item;
    size_t bytes = 0;
  };

  explicit BoundedLogQueue(LogQueueLimits limits = {}) : limits_(limits) {
    if (!limits_.max_lines) {
      limits_.max_lines = 1;
    }
  }

  LogPushResult Push(T&& item, size_t bytes, bool important) {
    LogPushResult result = LogPushResult::kAccepted;
    if (!Fits(bytes)) {
      if (!important) {
        ++dropped_total_;
        ++dropped_new_total_;
        return LogPushResult::kDroppedNew;
      }
      while (!entries_.empty() && !Fits(bytes)) {
        bytes_ -= entries_.front().bytes;
        entries_.pop_front();
        ++dropped_total_;
        ++evicted_total_;
      }
      result = LogPushResult::kEvictedOldest;
    }
    entries_.push_back(Entry{std::move(item), bytes});
    bytes_ += bytes;
    ++accepted_total_;
    if (entries_.size() > max_lines_seen_) {
      max_lines_seen_ = entries_.size();
    }
    if (bytes_ > max_bytes_seen_) {
      max_bytes_seen_ = bytes_;
    }
    return result;
  }

  // Moves every queued entry, in order, to the end of `out`.
  size_t PopAll(std::vector<Entry>& out) {
    const size_t count = entries_.size();
    for (auto& entry : entries_) {
      out.push_back(std::move(entry));
    }
    entries_.clear();
    bytes_ = 0;
    return count;
  }

  size_t size() const { return entries_.size(); }
  size_t bytes() const { return bytes_; }
  bool empty() const { return entries_.empty(); }
  const LogQueueLimits& limits() const { return limits_; }

  uint64_t accepted_total() const { return accepted_total_; }
  // Lines lost to overflow (dropped new + evicted old).
  uint64_t dropped_total() const { return dropped_total_; }
  uint64_t dropped_new_total() const { return dropped_new_total_; }
  uint64_t evicted_total() const { return evicted_total_; }

  // High-water marks since the last TakeHighWater (one report window).
  size_t max_lines_seen() const { return max_lines_seen_; }
  size_t max_bytes_seen() const { return max_bytes_seen_; }
  void TakeHighWater(size_t& lines, size_t& bytes_out) {
    lines = max_lines_seen_;
    bytes_out = max_bytes_seen_;
    max_lines_seen_ = entries_.size();
    max_bytes_seen_ = bytes_;
  }

 private:
  bool Fits(size_t bytes) const {
    return entries_.size() < limits_.max_lines && bytes_ + bytes <= limits_.max_bytes;
  }

  LogQueueLimits limits_;
  std::deque<Entry> entries_;
  size_t bytes_ = 0;
  uint64_t accepted_total_ = 0;
  uint64_t dropped_total_ = 0;
  uint64_t dropped_new_total_ = 0;
  uint64_t evicted_total_ = 0;
  size_t max_lines_seen_ = 0;
  size_t max_bytes_seen_ = 0;
};

// Slow sink operation threshold (KI-069: any single sink write or flush over
// this logs an MCLA_LOG slow line).
inline constexpr uint64_t kSlowSinkOpNs = 100ull * 1000ull * 1000ull;
// At most one slow line per this interval; the rest are counted.
inline constexpr uint64_t kSlowReportIntervalNs = 1000ull * 1000ull * 1000ull;

enum class SinkOp : uint8_t { kWrite, kFlush };

inline const char* SinkOpName(SinkOp op) {
  return op == SinkOp::kWrite ? "write" : "flush";
}

// One slow sink operation, to be reported.
struct SlowSinkOp {
  SinkOp op = SinkOp::kWrite;
  uint64_t ns = 0;
  uint32_t thread = 0;      // producer thread id of the line (0 for a flush)
  size_t bytes = 0;         // payload bytes of the line (0 for a flush)
  uint64_t suppressed = 0;  // slow operations not reported since the last line
};

// Per-window sink timing (one MCLA_LOG minute line) and the slow-op rate
// limit. Single writer (the writer thread, or the producer in sync mode under
// the write lock).
class SinkTiming {
 public:
  explicit SinkTiming(uint64_t slow_ns = kSlowSinkOpNs,
                      uint64_t report_interval_ns = kSlowReportIntervalNs)
      : slow_ns_(slow_ns), report_interval_ns_(report_interval_ns) {}

  // Records one operation that took `ns` at monotonic time `now_ns`. Returns
  // true and fills `report` when a slow line should be logged now.
  bool Note(SinkOp op, uint64_t ns, uint64_t now_ns, uint32_t thread, size_t bytes,
            SlowSinkOp& report) {
    if (op == SinkOp::kWrite) {
      ++writes_;
      write_max_ns_ = ns > write_max_ns_ ? ns : write_max_ns_;
    } else {
      ++flushes_;
      flush_max_ns_ = ns > flush_max_ns_ ? ns : flush_max_ns_;
    }
    if (ns <= slow_ns_) {
      return false;
    }
    ++slow_;
    ++slow_total_;
    if (has_reported_ && now_ns - last_report_ns_ < report_interval_ns_) {
      ++suppressed_;
      return false;
    }
    has_reported_ = true;
    last_report_ns_ = now_ns;
    report = SlowSinkOp{op, ns, thread, bytes, suppressed_};
    suppressed_ = 0;
    return true;
  }

  uint64_t writes() const { return writes_; }
  uint64_t flushes() const { return flushes_; }
  uint64_t write_max_ns() const { return write_max_ns_; }
  uint64_t flush_max_ns() const { return flush_max_ns_; }
  uint64_t slow() const { return slow_; }
  uint64_t slow_total() const { return slow_total_; }

  // Ends a report window (the minute line); the slow rate limit carries on.
  void ResetWindow() {
    writes_ = 0;
    flushes_ = 0;
    write_max_ns_ = 0;
    flush_max_ns_ = 0;
    slow_ = 0;
  }

 private:
  uint64_t slow_ns_;
  uint64_t report_interval_ns_;
  uint64_t writes_ = 0;
  uint64_t flushes_ = 0;
  uint64_t write_max_ns_ = 0;
  uint64_t flush_max_ns_ = 0;
  uint64_t slow_ = 0;
  uint64_t slow_total_ = 0;
  uint64_t suppressed_ = 0;
  uint64_t last_report_ns_ = 0;
  bool has_reported_ = false;
};

inline double NsToMs(uint64_t ns) {
  return double(ns) / 1e6;
}

inline std::string FormatSlowSinkOp(const SlowSinkOp& op) {
  return fmt::format("MCLA_LOG slow {}: ms={:.1f} thread={} bytes={} suppressed={}",
                     SinkOpName(op.op), NsToMs(op.ns), op.thread, op.bytes, op.suppressed);
}

// Values of one MCLA_LOG v=1 kind=minute line.
struct LogMinuteWindow {
  bool async = true;
  uint64_t lines = 0;          // lines accepted in the window
  uint64_t dropped = 0;        // lines lost to overflow in the window
  uint64_t dropped_total = 0;  // since start
  size_t queue_max = 0;        // queue high-water mark (lines)
  size_t queue_max_bytes = 0;  // queue high-water mark (bytes)
  uint64_t writes = 0;         // sink writes in the window
  uint64_t flushes = 0;        // sink flushes in the window
  uint64_t write_max_ns = 0;
  uint64_t flush_max_ns = 0;
  uint64_t slow = 0;  // sink operations over the slow threshold in the window
};

inline std::string FormatLogMinuteLine(const LogMinuteWindow& w) {
  return fmt::format(
      "MCLA_LOG v=1 kind=minute mode={} lines={} dropped={} dropped_total={} queue_max={} "
      "queue_max_kb={} writes={} flushes={} write_max_ms={:.1f} flush_max_ms={:.1f} slow={}",
      w.async ? "async" : "sync", w.lines, w.dropped, w.dropped_total, w.queue_max,
      (w.queue_max_bytes + 1023) / 1024, w.writes, w.flushes, NsToMs(w.write_max_ns),
      NsToMs(w.flush_max_ns), w.slow);
}

// Approximate queue cost of one line: the fixed entry plus its text.
inline size_t LogEntryBytes(size_t fixed, size_t payload, size_t logger_name) {
  return fixed + payload + logger_name;
}

}  // namespace rex::logging
