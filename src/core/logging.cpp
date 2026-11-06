/**
 * @file        core/logging.cpp
 * @brief       Logging infrastructure implementation
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <spdlog/details/log_msg_buffer.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/logging/async_queue.h>
#include <rex/platform.h>
#include <rex/platform/env.h>
#include <rex/thread.h>

#if REX_PLATFORM_WIN32
#include <spdlog/sinks/msvc_sink.h>
#else
#include <spdlog/sinks/stdout_sinks.h>
#endif

REXCVAR_DEFINE_STRING(log_level, "info", "Log",
                      "Global log level: trace, debug, info, warn, error, critical, off")
    .allowed({"trace", "debug", "info", "warn", "error", "critical", "off"});

REXCVAR_DEFINE_STRING(log_file, "", "Log", "Log file path (empty = auto sequential naming)");

REXCVAR_DEFINE_BOOL(log_verbose, false, "Log", "Enable verbose logging (sets level to trace)")
    .debug_only();

REXCVAR_DEFINE_BOOL(log_noisy, false, "Log", "Enable noisy/high-frequency log macros");

REXCVAR_DEFINE_INT32(log_flush_interval, 0, "Log", "Periodic flush interval in seconds (0 = off)")
    .range(0, 60)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_max_file_size_mb, 5, "Log", "Max log file size in MB before rotation")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_max_files, 20, "Log", "Max number of rotated log files to keep")
    .range(1, 100)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(log_async, true, "Log",
                    "Write the log sinks on a writer thread (bounded queue, never blocks the "
                    "logging thread; KI-069). false = write and flush on the logging thread")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_async_flush_ms, 1000, "Log",
                     "log_async: periodic flush interval in ms (warn and above flush at once)")
    .range(100, 10000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_async_queue_lines, 16384, "Log",
                     "log_async: queue bound in lines; when full, new lines below warn are "
                     "dropped and counted (MCLA_LOG minute line)")
    .range(256, 1048576)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(log_async_queue_mb, 16, "Log", "log_async: queue bound in MB")
    .range(1, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace rex {

namespace {

std::vector<LogCategoryEntry> g_registry;
spdlog::sink_ptr g_console_sink;
spdlog::sink_ptr g_file_sink;
spdlog::sink_ptr g_early_sink;
std::vector<spdlog::sink_ptr> g_extra_sinks;
bool g_early_initialized = false;
bool g_initialized = false;
std::mutex g_mutex;
LogConfig g_config;

// KI-069 asynchronous logging. The default sinks (platform debug, console,
// file and every sink added with AddSink) sit behind one dispatcher sink.
// In async mode a producer only copies the line into a bounded queue (never
// blocks on a full queue, see rex/logging/async_queue.h) and one writer thread
// writes and flushes the sinks. In sync mode (log_async=false) the producer
// writes and flushes on its own thread, as before.
class LogDispatchSink final : public spdlog::sinks::sink {
 public:
  using SinkList = std::vector<spdlog::sink_ptr>;

  LogDispatchSink(bool async, logging::LogQueueLimits limits, uint64_t flush_interval_ns)
      : async_(async), queue_(limits), flush_interval_ns_(flush_interval_ns) {
    sinks_ = std::make_shared<const SinkList>();
    set_level(spdlog::level::trace);
    worker_ = std::thread([this] { WorkerMain(); });
  }

  ~LogDispatchSink() override {
    if (worker_.joinable()) {
      worker_.detach();
    }
  }

  bool async() const { return async_; }

  void log(const spdlog::details::log_msg& msg) override {
    if (!async_) {
      WriteSync(msg);
      return;
    }
    const bool important = msg.level >= spdlog::level::warn;
    const size_t bytes =
        logging::LogEntryBytes(sizeof(QueuedLine), msg.payload.size(), msg.logger_name.size());
    bool notify = important;
    {
      std::lock_guard lock(queue_mutex_);
      if (stopped_) {
        return;
      }
      notify = notify || queue_.empty();
      const uint64_t seq = pushed_seq_ + 1;
      if (queue_.Push(QueuedLine{spdlog::details::log_msg_buffer(msg), seq}, bytes, important) !=
          logging::LogPushResult::kDroppedNew) {
        pushed_seq_ = seq;
      }
      queue_depth_.store(queue_.size(), std::memory_order_relaxed);
      dropped_total_.store(queue_.dropped_total(), std::memory_order_relaxed);
    }
    if (notify) {
      queue_cv_.notify_one();
    }
    // A critical line usually precedes an abort: give the writer a bounded
    // chance to put it on disk.
    if (msg.level >= spdlog::level::critical && !OnWorker()) {
      Drain(kCriticalDrainMs);
    }
  }

  void flush() override {
    if (!async_) {
      FlushSync();
      return;
    }
    {
      std::lock_guard lock(queue_mutex_);
      flush_requested_ = true;
    }
    queue_cv_.notify_one();
  }

  // The downstream sinks keep their own patterns.
  void set_pattern(const std::string&) override {}
  void set_formatter(std::unique_ptr<spdlog::formatter>) override {}

  void SetSinks(SinkList sinks) {
    std::lock_guard lock(sinks_mutex_);
    sinks_ = std::make_shared<const SinkList>(std::move(sinks));
  }

  void AddDownstream(const spdlog::sink_ptr& sink) {
    std::lock_guard lock(sinks_mutex_);
    auto next = std::make_shared<SinkList>(*sinks_);
    next->push_back(sink);
    sinks_ = std::move(next);
  }

  // Lines queued before the removal still reach the sink (bounded wait).
  void RemoveDownstream(const spdlog::sink_ptr& sink) {
    if (async_ && !OnWorker()) {
      Drain(kFlushDrainMs);
    }
    std::lock_guard lock(sinks_mutex_);
    auto next = std::make_shared<SinkList>(*sinks_);
    std::erase(*next, sink);
    sinks_ = std::move(next);
  }

  void ReplaceDownstream(const spdlog::sink_ptr& old_sink, const spdlog::sink_ptr& new_sink) {
    std::lock_guard lock(sinks_mutex_);
    auto next = std::make_shared<SinkList>(*sinks_);
    if (old_sink) {
      std::erase(*next, old_sink);
    }
    if (new_sink) {
      next->push_back(new_sink);
    }
    sinks_ = std::move(next);
  }

  // Waits (at most timeout_ms) until every line queued before the call is
  // written and the sinks are flushed. Returns false on timeout. Sync mode
  // flushes on the calling thread.
  bool Drain(uint32_t timeout_ms) {
    if (!async_) {
      FlushSync();
      return true;
    }
    if (OnWorker()) {
      return false;
    }
    uint64_t target;
    {
      std::lock_guard lock(queue_mutex_);
      if (stopped_) {
        return false;
      }
      target = pushed_seq_;
      flush_requested_ = true;
    }
    queue_cv_.notify_one();
    std::unique_lock lock(done_mutex_);
    done_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&] { return flushed_seq_ >= target || worker_exited_; });
    return flushed_seq_ >= target;
  }

  // Last-chance drain on the calling thread (process exit, crash handling):
  // only when no other thread holds the write or the queue lock, so it never
  // waits on a thread that may be dead or suspended.
  void EmergencyDrain() {
    std::unique_lock write_lock(write_mutex_, std::try_to_lock);
    if (!write_lock.owns_lock()) {
      return;
    }
    std::vector<QueueEntry> batch;
    {
      std::unique_lock lock(queue_mutex_, std::try_to_lock);
      if (!lock.owns_lock()) {
        return;
      }
      queue_.PopAll(batch);
      queue_depth_.store(0, std::memory_order_relaxed);
    }
    std::shared_ptr<const SinkList> sinks;
    {
      std::unique_lock lock(sinks_mutex_, std::try_to_lock);
      if (!lock.owns_lock()) {
        return;
      }
      sinks = sinks_;
    }
    for (auto& entry : batch) {
      for (const auto& sink : *sinks) {
        if (sink->should_log(entry.item.msg.level)) {
          SafeSinkLog(*sink, entry.item.msg);
        }
      }
    }
    for (const auto& sink : *sinks) {
      SafeSinkFlush(*sink);
    }
  }

  // Writes the remaining lines and stops the writer thread.
  void Stop() {
    {
      std::lock_guard lock(queue_mutex_);
      if (stop_requested_) {
        return;
      }
      stop_requested_ = true;
    }
    queue_cv_.notify_one();
    if (worker_.joinable() && !OnWorker()) {
      worker_.join();
    }
  }

  LogRuntimeState State() const {
    LogRuntimeState state;
    state.active = true;
    state.async = async_;
    state.queue_lines = queue_depth_.load(std::memory_order_relaxed);
    state.dropped_total = dropped_total_.load(std::memory_order_relaxed);
    state.slow_total = slow_total_.load(std::memory_order_relaxed);
    const uint64_t begin = op_begin_ns_.load(std::memory_order_acquire);
    if (begin) {
      const uint64_t now = NowNs();
      state.op_busy_ms = now > begin ? (now - begin) / 1000000ull : 0;
      state.op_thread = op_thread_.load(std::memory_order_relaxed);
      state.op_flush = op_flush_.load(std::memory_order_relaxed);
    }
    state.writer_thread = worker_tid_.load(std::memory_order_relaxed);
    return state;
  }

 private:
  struct QueuedLine {
    spdlog::details::log_msg_buffer msg;
    uint64_t seq = 0;
  };
  using Queue = logging::BoundedLogQueue<QueuedLine>;
  using QueueEntry = Queue::Entry;

  static constexpr uint32_t kCriticalDrainMs = 500;
  static constexpr uint32_t kFlushDrainMs = 2000;
  static constexpr uint64_t kMinuteNs = 60ull * 1000ull * 1000ull * 1000ull;
  static constexpr uint64_t kIdleTickNs = 1000ull * 1000ull * 1000ull;

  static uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
  }

  bool OnWorker() const {
    const uint32_t tid = worker_tid_.load(std::memory_order_relaxed);
    return tid && tid == rex::thread::current_thread_system_id();
  }

  std::shared_ptr<const SinkList> Sinks() {
    std::lock_guard lock(sinks_mutex_);
    return sinks_;
  }

  static void SafeSinkLog(spdlog::sinks::sink& sink, const spdlog::details::log_msg& msg) {
    try {
      sink.log(msg);
    } catch (...) {}
  }

  static void SafeSinkFlush(spdlog::sinks::sink& sink) {
    try {
      sink.flush();
    } catch (...) {}
  }

  void BeginOp(uint32_t thread, bool flush) {
    op_thread_.store(thread, std::memory_order_relaxed);
    op_flush_.store(flush, std::memory_order_relaxed);
    op_begin_ns_.store(NowNs(), std::memory_order_release);
  }

  void EndOp() { op_begin_ns_.store(0, std::memory_order_release); }

  // Caller holds write_mutex_. Appends slow operations to `slow`.
  void WriteLocked(const spdlog::details::log_msg& msg, const SinkList& sinks,
                   std::vector<logging::SlowSinkOp>& slow) {
    const uint32_t thread = uint32_t(msg.thread_id);
    for (const auto& sink : sinks) {
      if (!sink->should_log(msg.level)) {
        continue;
      }
      BeginOp(thread, false);
      const uint64_t begin = NowNs();
      SafeSinkLog(*sink, msg);
      const uint64_t end = NowNs();
      EndOp();
      logging::SlowSinkOp report;
      if (timing_.Note(logging::SinkOp::kWrite, end - begin, end, thread, msg.payload.size(),
                       report)) {
        slow.push_back(report);
      }
    }
  }

  // Caller holds write_mutex_.
  void FlushLocked(const SinkList& sinks, std::vector<logging::SlowSinkOp>& slow) {
    for (const auto& sink : sinks) {
      BeginOp(0, true);
      const uint64_t begin = NowNs();
      SafeSinkFlush(*sink);
      const uint64_t end = NowNs();
      EndOp();
      logging::SlowSinkOp report;
      if (timing_.Note(logging::SinkOp::kFlush, end - begin, end, 0, 0, report)) {
        slow.push_back(report);
      }
    }
  }

  // Logged after the write lock is released (in async mode the line is only
  // queued; in sync mode the slow-line rate limit bounds the recursion).
  void ReportSlow(const std::vector<logging::SlowSinkOp>& slow) {
    if (slow.empty()) {
      return;
    }
    slow_total_.fetch_add(slow.size(), std::memory_order_relaxed);
    if (auto* logger = spdlog::default_logger_raw()) {
      for (const auto& op : slow) {
        logger->warn("{}", logging::FormatSlowSinkOp(op));
      }
    }
  }

  void WriteSync(const spdlog::details::log_msg& msg) {
    std::vector<logging::SlowSinkOp> slow;
    {
      std::lock_guard lock(write_mutex_);
      ++sync_lines_;
      WriteLocked(msg, *Sinks(), slow);
    }
    ReportSlow(slow);
  }

  void FlushSync() {
    std::vector<logging::SlowSinkOp> slow;
    {
      std::lock_guard lock(write_mutex_);
      FlushLocked(*Sinks(), slow);
    }
    ReportSlow(slow);
  }

  void EmitMinute() {
    logging::LogMinuteWindow w;
    w.async = async_;
    {
      std::lock_guard lock(queue_mutex_);
      const uint64_t accepted = queue_.accepted_total();
      const uint64_t dropped = queue_.dropped_total();
      w.lines = accepted - minute_accepted_;
      w.dropped = dropped - minute_dropped_;
      w.dropped_total = dropped;
      minute_accepted_ = accepted;
      minute_dropped_ = dropped;
      queue_.TakeHighWater(w.queue_max, w.queue_max_bytes);
    }
    {
      std::lock_guard lock(write_mutex_);
      if (!async_) {
        w.lines = sync_lines_;
        sync_lines_ = 0;
      }
      w.writes = timing_.writes();
      w.flushes = timing_.flushes();
      w.write_max_ns = timing_.write_max_ns();
      w.flush_max_ns = timing_.flush_max_ns();
      w.slow = timing_.slow();
      timing_.ResetWindow();
    }
    if (auto* logger = spdlog::default_logger_raw()) {
      logger->info("{}", logging::FormatLogMinuteLine(w));
    }
  }

  void WorkerMain() {
    rex::thread::set_current_thread_name("Log Writer");
    worker_tid_.store(rex::thread::current_thread_system_id(), std::memory_order_relaxed);
    std::vector<QueueEntry> batch;
    std::vector<logging::SlowSinkOp> slow;
    uint64_t now = NowNs();
    uint64_t next_flush = now + flush_interval_ns_;
    uint64_t next_minute = now + kMinuteNs;
    bool dirty = false;
    uint64_t written_seq = 0;
    for (;;) {
      bool flush_requested = false;
      bool stopping = false;
      {
        std::unique_lock lock(queue_mutex_);
        const uint64_t deadline = std::min(next_minute, dirty ? next_flush : now + kIdleTickNs);
        const uint64_t wait_ns = deadline > now ? deadline - now : 0;
        queue_cv_.wait_for(lock, std::chrono::nanoseconds(wait_ns),
                           [&] { return stop_requested_ || flush_requested_ || !queue_.empty(); });
        queue_.PopAll(batch);
        queue_depth_.store(0, std::memory_order_relaxed);
        flush_requested = flush_requested_;
        flush_requested_ = false;
        stopping = stop_requested_;
      }
      bool want_flush = flush_requested || stopping;
      if (!batch.empty()) {
        std::lock_guard lock(write_mutex_);
        const auto sinks = Sinks();
        for (auto& entry : batch) {
          WriteLocked(entry.item.msg, *sinks, slow);
          want_flush = want_flush || entry.item.msg.level >= spdlog::level::warn;
          written_seq = entry.item.seq;
        }
        dirty = true;
        batch.clear();
      }
      now = NowNs();
      if (dirty && (want_flush || now >= next_flush)) {
        std::lock_guard lock(write_mutex_);
        FlushLocked(*Sinks(), slow);
        dirty = false;
        now = NowNs();
        next_flush = now + flush_interval_ns_;
      }
      if (!dirty) {
        std::lock_guard lock(done_mutex_);
        flushed_seq_ = written_seq;
      }
      done_cv_.notify_all();
      ReportSlow(slow);
      slow.clear();
      if (now >= next_minute) {
        next_minute = now + kMinuteNs;
        EmitMinute();
      }
      if (stopping) {
        std::lock_guard lock(queue_mutex_);
        if (queue_.empty() && !dirty) {
          stopped_ = true;
          break;
        }
      }
    }
    {
      std::lock_guard lock(done_mutex_);
      worker_exited_ = true;
    }
    done_cv_.notify_all();
  }

  const bool async_;

  // Producer side (queue_mutex_).
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  Queue queue_;
  uint64_t pushed_seq_ = 0;
  bool flush_requested_ = false;
  bool stop_requested_ = false;
  bool stopped_ = false;
  uint64_t minute_accepted_ = 0;
  uint64_t minute_dropped_ = 0;

  // Writer side (write_mutex_).
  std::mutex write_mutex_;
  logging::SinkTiming timing_;
  uint64_t sync_lines_ = 0;
  const uint64_t flush_interval_ns_;

  std::mutex sinks_mutex_;
  std::shared_ptr<const SinkList> sinks_;

  std::mutex done_mutex_;
  std::condition_variable done_cv_;
  uint64_t flushed_seq_ = 0;
  bool worker_exited_ = false;

  // Read without locks (State(), the CP freeze watchdog).
  std::atomic<size_t> queue_depth_{0};
  std::atomic<uint64_t> dropped_total_{0};
  std::atomic<uint64_t> slow_total_{0};
  std::atomic<uint64_t> op_begin_ns_{0};
  std::atomic<uint32_t> op_thread_{0};
  std::atomic<bool> op_flush_{false};
  std::atomic<uint32_t> worker_tid_{0};

  std::thread worker_;
};

// The dispatcher is never destroyed implicitly: a joinable writer thread must
// not meet a destructor at static teardown. ShutdownLogging stops it.
std::shared_ptr<LogDispatchSink>* g_dispatch_holder = nullptr;

LogDispatchSink* Dispatch() {
  return g_dispatch_holder ? g_dispatch_holder->get() : nullptr;
}

// Process exit without ShutdownLogging (the usual path): write what the
// writer thread left in the queue, if no dead thread holds a lock.
struct LogExitFlusher {
  ~LogExitFlusher() {
    if (auto* dispatch = Dispatch()) {
      dispatch->EmergencyDrain();
    }
  }
} g_log_exit_flusher;

spdlog::level::level_enum EffectiveFlushLevel() {
  auto* dispatch = Dispatch();
  if (dispatch && dispatch->async() && g_config.flush_level < spdlog::level::warn) {
    // The writer flushes on warn and above itself, and periodically.
    return spdlog::level::warn;
  }
  return g_config.flush_level;
}

std::filesystem::path NextSequentialLogPath(const std::filesystem::path& logs_dir,
                                            std::string_view app_name) {
  std::filesystem::create_directories(logs_dir);

  int max_seq = 0;
  std::string prefix = std::string(app_name) + "_";
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(logs_dir, ec)) {
    if (!entry.is_regular_file())
      continue;
    auto stem = entry.path().stem().string();
    if (stem.starts_with(prefix)) {
      auto num_str = stem.substr(prefix.size());
      int num = 0;
      auto [ptr, parse_ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), num);
      if (parse_ec == std::errc() && ptr == num_str.data() + num_str.size())
        max_seq = std::max(max_seq, num);
    }
  }

  return logs_dir / fmt::format("{}_{:03d}.log", app_name, max_seq + 1);
}

std::vector<spdlog::sink_ptr> BuildDownstreamSinks() {
  std::vector<spdlog::sink_ptr> sinks;
  if (g_early_sink)
    sinks.push_back(g_early_sink);
  if (g_console_sink)
    sinks.push_back(g_console_sink);
  if (g_file_sink)
    sinks.push_back(g_file_sink);
  for (auto& s : g_extra_sinks)
    sinks.push_back(s);
  return sinks;
}

// The sinks a logger holds: the dispatcher alone after InitLogging, the
// downstream sinks directly before it (early phase).
std::vector<spdlog::sink_ptr> BuildDefaultSinks() {
  if (g_dispatch_holder && *g_dispatch_holder)
    return {*g_dispatch_holder};
  return BuildDownstreamSinks();
}

// Build sinks for a specific category (handles per-category sinks from config)
std::vector<spdlog::sink_ptr> BuildCategorySinks(const std::string& name) {
  auto it = g_config.category_sinks.find(name);
  if (it != g_config.category_sinks.end()) {
    if (g_config.category_sinks_exclusive) {
      // Replace default sinks entirely
      return it->second;
    }
    // Additive: default sinks + category-specific sinks
    auto sinks = BuildDefaultSinks();
    for (auto& s : it->second)
      sinks.push_back(s);
    return sinks;
  }
  return BuildDefaultSinks();
}

// Resolve per-category level from config, or return default
spdlog::level::level_enum ResolveCategoryLevel(const std::string& name) {
  auto it = g_config.category_levels.find(name);
  if (it != g_config.category_levels.end())
    return it->second;
  return g_config.default_level;
}

// Create a logger and register it
std::shared_ptr<spdlog::logger> CreateCategoryLogger(const std::string& name) {
  auto sinks = BuildCategorySinks(name);
  auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
  logger->set_level(ResolveCategoryLevel(name));
  logger->flush_on(EffectiveFlushLevel());
  spdlog::register_logger(logger);
  return logger;
}

}  // namespace

void InitLoggingEarly() {
  std::lock_guard lock(g_mutex);
  if (g_early_initialized || g_initialized)
    return;

#if REX_PLATFORM_WIN32
  auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
  auto sink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
#endif
  sink->set_level(spdlog::level::trace);
  sink->set_pattern("[%l] [%n] %v");
  g_early_sink = sink;

  g_config.default_level = kDefaultLogLevel;

  // Create loggers for any categories already registered during static init
  for (auto& entry : g_registry) {
    if (!entry.name.empty() && !entry.logger)
      entry.logger = CreateCategoryLogger(entry.name);
  }

  // Set default logger to "core" if registered
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_early_initialized = true;
}

void InitLogging(const LogConfig& config) {
  std::lock_guard lock(g_mutex);

  if (g_initialized) {
    g_config = config;
    for (auto& entry : g_registry) {
      if (!entry.logger)
        continue;
      entry.logger->set_level(ResolveCategoryLevel(entry.name));
      entry.logger->flush_on(EffectiveFlushLevel());
      if (g_config.category_levels.count(entry.name))
        entry.has_explicit_level = true;
    }
    return;
  }

  g_config = config;

  // Early sink handling:
  //   Windows: the early msvc_sink is the persistent debug channel for GUI
  //     apps and does not conflict with the stdout console sink, so keep it.
  //   Non-Windows: drop the early stdout sink unconditionally so file-only
  //     configs don't leak to stdout and console configs don't duplicate.
#if !REX_PLATFORM_WIN32
  if (g_early_sink) {
    for (auto& entry : g_registry) {
      if (entry.logger)
        std::erase(entry.logger->sinks(), g_early_sink);
    }
    g_early_sink.reset();
  }
#endif

  // Console sink (stdout, colored). Intended for console-subsystem processes.
  if (config.log_to_console) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.console_pattern);
    g_console_sink = sink;
  }

  // File sink (rotating) with sequential naming fallback
  std::filesystem::path resolved_path;
  if (config.log_file) {
    resolved_path = rex::to_path(config.log_file);
  } else if (!config.app_name.empty()) {
    auto log_dir = config.log_dir.empty() ? std::filesystem::current_path() / "logs"
                                          : rex::to_path(config.log_dir);
    resolved_path = NextSequentialLogPath(log_dir, config.app_name);
  }
  if (!resolved_path.empty()) {
    auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        resolved_path.native(),
        static_cast<size_t>(REXCVAR_GET(log_max_file_size_mb)) * 1024 * 1024,
        static_cast<size_t>(REXCVAR_GET(log_max_files)), false);
    sink->set_level(spdlog::level::trace);
    sink->set_pattern(config.file_pattern);
    g_file_sink = sink;
  }

  g_extra_sinks = config.extra_sinks;

  // KI-069: one dispatcher in front of the default sinks.
  {
    logging::LogQueueLimits limits;
    limits.max_lines = static_cast<size_t>(REXCVAR_GET(log_async_queue_lines));
    limits.max_bytes = static_cast<size_t>(REXCVAR_GET(log_async_queue_mb)) * 1024 * 1024;
    const uint64_t flush_ns = static_cast<uint64_t>(REXCVAR_GET(log_async_flush_ms)) * 1000000ull;
    auto dispatch = std::make_shared<LogDispatchSink>(REXCVAR_GET(log_async), limits, flush_ns);
    dispatch->SetSinks(BuildDownstreamSinks());
    g_dispatch_holder = new std::shared_ptr<LogDispatchSink>(std::move(dispatch));
  }

  // Rebuild all loggers with new sinks
  for (auto& entry : g_registry) {
    if (entry.name.empty())
      continue;
    if (entry.logger)
      spdlog::drop(entry.name);
    entry.logger = CreateCategoryLogger(entry.name);
    if (g_config.category_levels.count(entry.name))
      entry.has_explicit_level = true;
  }

  // Set default logger to "core"
  for (auto& entry : g_registry) {
    if (entry.name == "core") {
      spdlog::set_default_logger(entry.logger);
      break;
    }
  }
  g_initialized = true;

  if (auto* logger = spdlog::default_logger_raw()) {
    logger->info(
        "MCLA_LOG v=1 kind=config mode={} queue_lines={} queue_mb={} flush_ms={} "
        "flush_level={}",
        Dispatch()->async() ? "async" : "sync", REXCVAR_GET(log_async_queue_lines),
        REXCVAR_GET(log_async_queue_mb), REXCVAR_GET(log_async_flush_ms),
        spdlog::level::to_string_view(EffectiveFlushLevel()));
  }

  // Periodic flush
  int flush_interval = REXCVAR_GET(log_flush_interval);
  if (flush_interval > 0)
    spdlog::flush_every(std::chrono::seconds(flush_interval));
}

void InitLogging(const char* log_file, spdlog::level::level_enum level) {
  LogConfig config;
  config.log_file = log_file;
  config.default_level = level;
  InitLogging(config);
}

void ShutdownLogging() {
  std::lock_guard lock(g_mutex);
  if (!g_initialized && !g_early_initialized)
    return;

  for (auto& entry : g_registry)
    if (entry.logger)
      entry.logger->flush();

  if (g_dispatch_holder) {
    (*g_dispatch_holder)->Stop();
    delete g_dispatch_holder;
    g_dispatch_holder = nullptr;
  }

  spdlog::shutdown();
  g_registry.clear();
  g_console_sink.reset();
  g_file_sink.reset();
  g_early_sink.reset();
  g_extra_sinks.clear();
  g_initialized = false;
  g_early_initialized = false;
}

void FlushLogging() {
  std::lock_guard lock(g_mutex);
  for (auto& entry : g_registry)
    if (entry.logger)
      entry.logger->flush();
  if (auto* dispatch = Dispatch())
    dispatch->Drain(2000);
}

bool FlushLoggingFor(uint32_t timeout_ms) {
  // No registry lock: usable from crash handling while another thread holds it.
  auto* dispatch = Dispatch();
  if (!dispatch)
    return true;
  if (dispatch->Drain(timeout_ms))
    return true;
  dispatch->EmergencyDrain();
  return false;
}

LogRuntimeState GetLogRuntimeState() {
  auto* dispatch = Dispatch();
  return dispatch ? dispatch->State() : LogRuntimeState{};
}

LogCategoryId RegisterLogCategory(const char* name) {
  std::lock_guard lock(g_mutex);

  // Check for duplicates
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = name;
  if (g_initialized || g_early_initialized) {
    entry.logger = CreateCategoryLogger(name);
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

LogCategoryId RegisterLogSubcategory(const char* name, LogCategoryId parent) {
  std::lock_guard lock(g_mutex);

  std::string parent_name;
  if (parent.id < g_registry.size())
    parent_name = g_registry[parent.id].name;
  std::string full_name = parent_name + "." + name;

  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == full_name)
      return LogCategoryId{static_cast<uint16_t>(i)};
  }

  uint16_t id = static_cast<uint16_t>(g_registry.size());
  LogCategoryEntry entry;
  entry.name = full_name;
  entry.parent = parent;
  if (g_initialized || g_early_initialized) {
    entry.logger = CreateCategoryLogger(full_name);
    if (g_config.category_levels.count(full_name))
      entry.has_explicit_level = true;
  }
  g_registry.push_back(std::move(entry));
  return LogCategoryId{id};
}

std::optional<LogCategoryId> FindCategory(const std::string& name) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_registry.size(); ++i) {
    if (g_registry[i].name == name) {
      return LogCategoryId{static_cast<uint16_t>(i)};
    }
  }
  return std::nullopt;
}

std::span<const LogCategoryEntry> GetAllCategories() {
  // No lock: only safe to call from main thread or after init.
  return {g_registry.data(), g_registry.size()};
}

spdlog::logger* GetLoggerRaw(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger.get();
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger(LogCategoryId category) {
  if (!g_initialized && !g_early_initialized)
    InitLoggingEarly();
  if (category.id < g_registry.size()) {
    return g_registry[category.id].logger;
  }
  return nullptr;
}

std::shared_ptr<spdlog::logger> GetLogger() {
  return GetLogger(rex::log::core());
}

void SetCategoryLevel(LogCategoryId category, spdlog::level::level_enum level) {
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->set_level(level);
    }
  }
}

void SetAllLevels(spdlog::level::level_enum level) {
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->set_level(level);
  }
}

void SetRootLevel(LogCategoryId root, spdlog::level::level_enum level) {
  SetCategoryLevel(root, level);
  for (auto& entry : g_registry) {
    if (entry.parent.has_value() && entry.parent->id == root.id && !entry.has_explicit_level &&
        entry.logger)
      entry.logger->set_level(level);
  }
}

void RegisterLogLevelCallback() {
  rex::cvar::RegisterChangeCallback("log_level", [](std::string_view, std::string_view value) {
    if (auto level = ParseLogLevel(std::string(value))) {
      for (size_t i = 0; i < g_registry.size(); ++i) {
        auto& entry = g_registry[i];
        if (!entry.parent.has_value() && entry.logger && !entry.has_explicit_level)
          SetRootLevel(LogCategoryId{static_cast<uint16_t>(i)}, *level);
      }
    }
  });
}

void AddSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  g_extra_sinks.push_back(sink);
  if (auto* dispatch = Dispatch()) {
    dispatch->AddDownstream(sink);
    return;
  }
  for (auto& entry : g_registry) {
    if (entry.logger)
      entry.logger->sinks().push_back(sink);
  }
}

void AddSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      logger->sinks().push_back(sink);
    }
  }
}

void RemoveSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  std::erase(g_extra_sinks, sink);
  if (auto* dispatch = Dispatch())
    dispatch->RemoveDownstream(sink);
  for (auto& entry : g_registry) {
    if (entry.logger)
      std::erase(entry.logger->sinks(), sink);
  }
}

void RemoveSink(LogCategoryId category, spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (category.id < g_registry.size()) {
    if (auto& logger = g_registry[category.id].logger) {
      std::erase(logger->sinks(), sink);
    }
  }
}

void ReplaceConsoleSink(spdlog::sink_ptr sink) {
  std::lock_guard lock(g_mutex);
  if (auto* dispatch = Dispatch()) {
    dispatch->ReplaceDownstream(g_console_sink, sink);
    g_console_sink = sink;
    return;
  }
  for (auto& entry : g_registry) {
    if (!entry.logger)
      continue;
    auto& sinks = entry.logger->sinks();
    if (g_console_sink)
      std::erase(sinks, g_console_sink);
    if (sink)
      sinks.push_back(sink);
  }
  g_console_sink = sink;
}

void SetConsolePattern(const std::string& pattern) {
  if (g_console_sink)
    g_console_sink->set_pattern(pattern);
}

void SetFilePattern(const std::string& pattern) {
  if (g_file_sink)
    g_file_sink->set_pattern(pattern);
}

// ==========================================================================
// CLI Helpers
// ==========================================================================

std::optional<spdlog::level::level_enum> ParseLogLevel(const std::string& level_str) {
  static const std::unordered_map<std::string, spdlog::level::level_enum> level_map = {
      {"trace", spdlog::level::trace},  {"debug", spdlog::level::debug},
      {"info", spdlog::level::info},    {"warn", spdlog::level::warn},
      {"warning", spdlog::level::warn}, {"error", spdlog::level::err},
      {"err", spdlog::level::err},      {"critical", spdlog::level::critical},
      {"off", spdlog::level::off},
  };

  std::string lower = level_str;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  auto it = level_map.find(lower);
  if (it != level_map.end())
    return it->second;
  return std::nullopt;
}

spdlog::level::level_enum ParseLogLevelOr(const std::string& level_str,
                                          spdlog::level::level_enum default_level) {
  return ParseLogLevel(level_str).value_or(default_level);
}

LogConfig BuildLogConfig(const char* log_file, const std::string& cli_level,
                         const std::map<std::string, std::string>& category_levels) {
  LogConfig config;
  config.log_file = log_file;

  // Build-type default
  config.default_level = kDefaultLogLevel;

  // Environment variable
  if (auto env_level = rex::platform::env::get("REX_LOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level))
      config.default_level = *level;
  } else if (auto env_level2 = rex::platform::env::get("SPDLOG_LEVEL")) {
    if (auto level = ParseLogLevel(*env_level2))
      config.default_level = *level;
  }

  // CLI global level overrides environment
  if (!cli_level.empty()) {
    if (auto level = ParseLogLevel(cli_level))
      config.default_level = *level;
  }

  // Per-category CLI levels (string-keyed)
  for (const auto& [cat_name, level_str] : category_levels) {
    if (level_str.empty())
      continue;
    if (auto level = ParseLogLevel(level_str)) {
      config.category_levels[cat_name] = *level;
    }
  }

  return config;
}

std::map<std::string, std::string> ParseCategoryLevelsFromConfig(
    const std::filesystem::path& config_path) {
  std::map<std::string, std::string> result;
  if (!std::filesystem::exists(config_path))
    return result;

  try {
    auto config = toml::parse_file(config_path.string());
    auto* log_table = config["log"].as_table();
    if (!log_table)
      return result;
    auto* levels_table = (*log_table)["levels"].as_table();
    if (!levels_table)
      return result;

    for (const auto& [key, value] : *levels_table) {
      if (value.is_string())
        result[std::string(key)] = value.as_string()->get();
    }
  } catch (const toml::parse_error&) {}

  return result;
}

}  // namespace rex
