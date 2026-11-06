/**
 ******************************************************************************
 * ReXGlue : Runtime glue for static recompilation projects                   *
 ******************************************************************************
 * Copyright 2026 Tom Clay. All rights reserved.                              *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    MCLA-R, 2026 - KI-069 asynchronous logging
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/logging.h>
#include <rex/logging/async_queue.h>
#include <rex/thread.h>

#include <spdlog/sinks/base_sink.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using rex::logging::BoundedLogQueue;
using rex::logging::LogPushResult;
using rex::logging::LogQueueLimits;
using rex::logging::SinkOp;
using rex::logging::SinkTiming;
using rex::logging::SlowSinkOp;

constexpr uint64_t kMs = 1000000ull;

std::vector<int> Drain(BoundedLogQueue<int>& queue) {
  std::vector<BoundedLogQueue<int>::Entry> entries;
  queue.PopAll(entries);
  std::vector<int> items;
  for (auto& entry : entries) {
    items.push_back(entry.item);
  }
  return items;
}

}  // namespace

TEST_CASE("log queue keeps push order", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{8, 1000});
  for (int i = 0; i < 5; ++i) {
    REQUIRE(queue.Push(int(i), 10, false) == LogPushResult::kAccepted);
  }
  REQUIRE(queue.size() == 5);
  REQUIRE(queue.bytes() == 50);
  REQUIRE(Drain(queue) == std::vector<int>{0, 1, 2, 3, 4});
  REQUIRE(queue.empty());
  REQUIRE(queue.bytes() == 0);
  REQUIRE(queue.accepted_total() == 5);
  REQUIRE(queue.dropped_total() == 0);
}

TEST_CASE("log queue drops new ordinary lines when full by count", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{3, 1000});
  for (int i = 0; i < 3; ++i) {
    REQUIRE(queue.Push(int(i), 1, false) == LogPushResult::kAccepted);
  }
  REQUIRE(queue.Push(3, 1, false) == LogPushResult::kDroppedNew);
  REQUIRE(queue.Push(4, 1, false) == LogPushResult::kDroppedNew);
  REQUIRE(queue.dropped_total() == 2);
  REQUIRE(queue.dropped_new_total() == 2);
  REQUIRE(queue.evicted_total() == 0);
  // The queued run stays contiguous: the oldest lines survive.
  REQUIRE(Drain(queue) == std::vector<int>{0, 1, 2});
  // Space again after the writer drained it.
  REQUIRE(queue.Push(5, 1, false) == LogPushResult::kAccepted);
}

TEST_CASE("log queue drops new ordinary lines when full by bytes", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{100, 100});
  REQUIRE(queue.Push(0, 60, false) == LogPushResult::kAccepted);
  REQUIRE(queue.Push(1, 41, false) == LogPushResult::kDroppedNew);
  REQUIRE(queue.Push(2, 40, false) == LogPushResult::kAccepted);
  REQUIRE(queue.bytes() == 100);
  REQUIRE(Drain(queue) == std::vector<int>{0, 2});
}

TEST_CASE("important lines evict the oldest lines", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{3, 1000});
  for (int i = 0; i < 3; ++i) {
    queue.Push(int(i), 1, false);
  }
  REQUIRE(queue.Push(100, 1, true) == LogPushResult::kEvictedOldest);
  REQUIRE(queue.evicted_total() == 1);
  REQUIRE(queue.dropped_total() == 1);
  REQUIRE(Drain(queue) == std::vector<int>{1, 2, 100});

  // By bytes: evicts as many as needed.
  BoundedLogQueue<int> bytes_queue(LogQueueLimits{100, 100});
  bytes_queue.Push(0, 40, false);
  bytes_queue.Push(1, 40, false);
  bytes_queue.Push(2, 20, false);
  REQUIRE(bytes_queue.Push(3, 70, true) == LogPushResult::kEvictedOldest);
  REQUIRE(bytes_queue.evicted_total() == 2);
  REQUIRE(Drain(bytes_queue) == std::vector<int>{2, 3});
}

TEST_CASE("an oversized important line is accepted into an empty queue", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{4, 10});
  queue.Push(0, 5, false);
  REQUIRE(queue.Push(1, 50, true) == LogPushResult::kEvictedOldest);
  REQUIRE(queue.size() == 1);
  REQUIRE(queue.bytes() == 50);
  // An oversized ordinary line never fits.
  BoundedLogQueue<int> empty(LogQueueLimits{4, 10});
  REQUIRE(empty.Push(2, 50, false) == LogPushResult::kDroppedNew);
}

TEST_CASE("log queue high-water marks per window", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{100, 1000});
  for (int i = 0; i < 7; ++i) {
    queue.Push(int(i), 10, false);
  }
  Drain(queue);
  queue.Push(7, 10, false);
  size_t lines = 0, bytes = 0;
  queue.TakeHighWater(lines, bytes);
  REQUIRE(lines == 7);
  REQUIRE(bytes == 70);
  // The next window starts from the current depth.
  queue.TakeHighWater(lines, bytes);
  REQUIRE(lines == 1);
  REQUIRE(bytes == 10);
}

TEST_CASE("a zero line limit still holds one line", "[log-async]") {
  BoundedLogQueue<int> queue(LogQueueLimits{0, 1000});
  REQUIRE(queue.Push(0, 1, false) == LogPushResult::kAccepted);
  REQUIRE(queue.Push(1, 1, false) == LogPushResult::kDroppedNew);
}

TEST_CASE("sink timing flags operations over 100 ms", "[log-async]") {
  SinkTiming timing;
  SlowSinkOp report;
  REQUIRE_FALSE(timing.Note(SinkOp::kWrite, 100 * kMs, 1000 * kMs, 7, 10, report));
  REQUIRE(timing.Note(SinkOp::kWrite, 100 * kMs + 1, 2000 * kMs, 7, 123, report));
  REQUIRE(report.op == SinkOp::kWrite);
  REQUIRE(report.thread == 7);
  REQUIRE(report.bytes == 123);
  REQUIRE(report.suppressed == 0);
  REQUIRE(timing.slow() == 1);
  REQUIRE(timing.writes() == 2);
  REQUIRE(timing.write_max_ns() == 100 * kMs + 1);
  REQUIRE(rex::logging::FormatSlowSinkOp(report) ==
          "MCLA_LOG slow write: ms=100.0 thread=7 bytes=123 suppressed=0");
}

TEST_CASE("slow sink reports are rate limited and count the rest", "[log-async]") {
  SinkTiming timing;
  SlowSinkOp report;
  REQUIRE(timing.Note(SinkOp::kFlush, 5500 * kMs, 10000 * kMs, 0, 0, report));
  REQUIRE(report.op == SinkOp::kFlush);
  // Within one second: counted, not reported.
  REQUIRE_FALSE(timing.Note(SinkOp::kWrite, 200 * kMs, 10200 * kMs, 1, 1, report));
  REQUIRE_FALSE(timing.Note(SinkOp::kFlush, 300 * kMs, 10900 * kMs, 0, 0, report));
  REQUIRE(timing.Note(SinkOp::kWrite, 150 * kMs, 11000 * kMs, 2, 5, report));
  REQUIRE(report.suppressed == 2);
  REQUIRE(timing.slow() == 4);
  REQUIRE(timing.flush_max_ns() == 5500 * kMs);
  REQUIRE(rex::logging::FormatSlowSinkOp(report) ==
          "MCLA_LOG slow write: ms=150.0 thread=2 bytes=5 suppressed=2");
}

TEST_CASE("sink timing window reset keeps totals", "[log-async]") {
  SinkTiming timing;
  SlowSinkOp report;
  timing.Note(SinkOp::kWrite, 150 * kMs, 1000 * kMs, 1, 1, report);
  timing.Note(SinkOp::kFlush, 2 * kMs, 1001 * kMs, 0, 0, report);
  timing.ResetWindow();
  REQUIRE(timing.writes() == 0);
  REQUIRE(timing.flushes() == 0);
  REQUIRE(timing.write_max_ns() == 0);
  REQUIRE(timing.flush_max_ns() == 0);
  REQUIRE(timing.slow() == 0);
  REQUIRE(timing.slow_total() == 1);
}

TEST_CASE("log minute line format", "[log-async]") {
  rex::logging::LogMinuteWindow w;
  w.lines = 840;
  w.dropped = 3;
  w.dropped_total = 5;
  w.queue_max = 12;
  w.queue_max_bytes = 4097;
  w.writes = 2520;
  w.flushes = 180;
  w.write_max_ns = 1500000;
  w.flush_max_ns = 5530000000ull;
  w.slow = 1;
  REQUIRE(rex::logging::FormatLogMinuteLine(w) ==
          "MCLA_LOG v=1 kind=minute mode=async lines=840 dropped=3 dropped_total=5 "
          "queue_max=12 queue_max_kb=5 writes=2520 flushes=180 write_max_ms=1.5 "
          "flush_max_ms=5530.0 slow=1");
  w.async = false;
  REQUIRE(rex::logging::FormatLogMinuteLine(w).find("mode=sync") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Dispatcher (src/core/logging.cpp) end to end, through a real file sink.
// ---------------------------------------------------------------------------

namespace {

// Sleeps on lines that contain "SLOW" (a stand-in for a blocked disk write).
class SleepySink : public spdlog::sinks::base_sink<std::mutex> {
 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    const std::string_view text(msg.payload.data(), msg.payload.size());
    if (text.find("SLOW") != std::string_view::npos) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
  }
  void flush_() override {}
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

void RunDispatchCase(bool async) {
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("rex_log_async_test_" + std::to_string(rex::thread::current_thread_system_id()));
  std::filesystem::create_directories(dir);
  const auto path = dir / "async.log";
  std::filesystem::remove(path);
  const std::string path_utf8 = path.string();

  // Register the core category before InitLogging so it becomes the default
  // logger (the MCLA_LOG lines go there). Needs a process that has not
  // initialized logging yet (ctest runs each case in its own process).
  (void)rex::log::core();
  if (rex::GetLogRuntimeState().active) {
    SKIP("logging already initialized in this process");
  }
  REXCVAR_SET(log_async, async);
  rex::LogConfig config;
  config.log_file = path_utf8.c_str();
  config.default_level = spdlog::level::info;
  rex::InitLogging(config);
  REQUIRE(rex::GetLogRuntimeState().active);
  REQUIRE(rex::GetLogRuntimeState().async == async);

  auto sleepy = std::make_shared<SleepySink>();
  rex::AddSink(sleepy);

  // The producer returns at once although the sink sleeps 150 ms.
  const auto begin = std::chrono::steady_clock::now();
  REXLOG_INFO("SLOW line");
  const auto producer_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - begin)
                               .count();
  if (async) {
    CHECK(producer_ms < 100);
  } else {
    CHECK(producer_ms >= 100);  // sync mode writes on the producer thread
  }

  constexpr int kThreads = 4;
  constexpr int kLines = 500;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t] {
      for (int i = 0; i < kLines; ++i) {
        REXLOG_INFO("order t={} i={}", t, i);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  rex::FlushLogging();
  rex::RemoveSink(sleepy);
  rex::ShutdownLogging();

  const auto lines = ReadLines(path);
  int next[kThreads] = {};
  bool slow_logged = false;
  bool config_logged = false;
  for (const auto& line : lines) {
    slow_logged = slow_logged || line.find("MCLA_LOG slow write: ms=") != std::string::npos;
    config_logged = config_logged ||
                    line.find(async ? "MCLA_LOG v=1 kind=config mode=async"
                                    : "MCLA_LOG v=1 kind=config mode=sync") != std::string::npos;
    const size_t at = line.find("order t=");
    if (at == std::string::npos) {
      continue;
    }
    const size_t i_at = line.find(" i=", at);
    REQUIRE(i_at != std::string::npos);
    const int t = std::stoi(line.substr(at + 8));
    const int i = std::stoi(line.substr(i_at + 3));
    REQUIRE(t >= 0);
    REQUIRE(t < kThreads);
    REQUIRE(i == next[t]);
    ++next[t];
  }
  for (int t = 0; t < kThreads; ++t) {
    CHECK(next[t] == kLines);
  }
  CHECK(slow_logged);
  CHECK(config_logged);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace

TEST_CASE("async logging keeps per-thread order, does not block, times slow writes",
          "[log-async-dispatch]") {
  RunDispatchCase(true);
}

TEST_CASE("sync logging (log_async=false) keeps order and times slow writes",
          "[log-async-dispatch]") {
  RunDispatchCase(false);
}
