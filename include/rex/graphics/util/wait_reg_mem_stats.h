#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <fmt/format.h>

namespace rex::graphics {

// M10-CPU-B: diagnostics and the event-wake loop of the command processor's
// PM4 WAIT_REG_MEM handler.
//
// MCLA waits on one guest word (the present/vblank release flag written by the
// guest graphics interrupt callback). With vsync on, a failed poll used to
// Sleep(wait / 0x100 ms), i.e. one 1 ms timer quantum per failed poll. With
// d3d12_wrm_event_wake on, the command processor instead waits on an
// auto-reset event that is set after every guest interrupt dispatch (the
// callback is the only writer of the flag), with the old sleep as the upper
// bound, and re-checks the predicate after every wake.
//
// Everything in this header is pure (no globals, no OS calls) and unit-tested
// in tests/unit/graphics/wait_reg_mem_stats_test.cpp.

// Version of the MCLA_WRM line.
inline constexpr uint32_t kWrmLineVersion = 1;

// Histogram of waited episodes (wall time from the first failed poll to the
// match): <0.5, 0.5-1, 1-2, 2-4, >=4 ms.
inline constexpr size_t kWrmHistogramBuckets = 5;
inline constexpr const char* kWrmHistogramNames[kWrmHistogramBuckets] = {
    "h_lt0_5ms", "h_0_5to1ms", "h_1to2ms", "h_2to4ms", "h_ge4ms"};

constexpr size_t WrmHistogramBucket(uint64_t wait_us) {
  return wait_us < 500 ? 0 : (wait_us < 1000 ? 1 : (wait_us < 2000 ? 2 : (wait_us < 4000 ? 3 : 4)));
}

// The awaited location and comparison of one WAIT_REG_MEM packet. `address`
// is the raw poll address for memory waits (with the endian bits) and the
// register index otherwise.
struct WrmPredicate {
  bool is_memory = false;
  uint32_t address = 0;
  uint32_t function = 0;  // wait_info & 7
  uint32_t ref = 0;
  uint32_t mask = 0;

  constexpr bool operator==(const WrmPredicate&) const = default;
};

struct WrmPredicateSlot {
  WrmPredicate predicate;
  uint64_t episodes = 0;
  uint64_t wait_us = 0;
};

inline constexpr size_t kWrmPredicateSlots = 8;

// How one failed poll blocked.
enum class WrmBlockKind : uint8_t {
  kYield,      // MaybeYield (wait < 0x100)
  kYieldLong,  // MaybeYield for wait >= 0x100 with vsync off
  kSleep,      // Sleep(wait / 0x100 ms) (vsync on, event wake off)
  kEvent,      // event wait bounded by wait / 0x100 ms (event wake on)
};

// Outcome of one blocking step, as reported to the loop below.
struct WrmBlockResult {
  WrmBlockKind kind = WrmBlockKind::kYield;
  // Event waits only: the event was signalled (false = timed out).
  bool signaled = false;
  // Time spent blocked (Sleep and event waits only; 0 for yields).
  uint64_t blocked_us = 0;
};

// Per-window counters. Command processor thread only (plain integers), except
// `signals`, which the caller fills from an atomic at emission.
struct WrmCounters {
  uint64_t packets = 0;    // WAIT_REG_MEM packets executed
  uint64_t immediate = 0;  // matched on the first poll
  uint64_t waited = 0;     // waited and matched
  uint64_t aborted = 0;    // left because the command processor stopped
  uint64_t failed_polls = 0;
  uint64_t yields = 0;
  uint64_t sleeps = 0;
  uint64_t event_waits = 0;
  uint64_t event_wakes = 0;     // event waits that returned signalled
  uint64_t event_timeouts = 0;  // event waits that hit the old sleep bound
  uint64_t stale_wakes = 0;     // signalled wakes followed by a failed poll
  uint64_t blocked_us = 0;      // time inside Sleep and event waits
  uint64_t wait_us = 0;         // waited episodes' wall time
  uint64_t max_wait_us = 0;
  uint64_t signals = 0;  // event sets by interrupt dispatch (filled at emission)
  std::array<uint64_t, kWrmHistogramBuckets> histogram{};
  std::array<WrmPredicateSlot, kWrmPredicateSlots> slots{};
  size_t slot_count = 0;
  // Waited episodes whose predicate did not fit in the slots.
  uint64_t other_predicate_episodes = 0;

  void CountBlock(const WrmBlockResult& block) {
    switch (block.kind) {
      case WrmBlockKind::kYield:
      case WrmBlockKind::kYieldLong:
        ++yields;
        break;
      case WrmBlockKind::kSleep:
        ++sleeps;
        blocked_us += block.blocked_us;
        break;
      case WrmBlockKind::kEvent:
        ++event_waits;
        blocked_us += block.blocked_us;
        if (block.signaled) {
          ++event_wakes;
        } else {
          ++event_timeouts;
        }
        break;
    }
  }

  // One finished packet. `waited_us` is only used when `failed_polls` > 0.
  void CountPacket(const WrmPredicate& predicate, bool matched, uint32_t packet_failed_polls,
                   uint32_t packet_stale_wakes, uint64_t waited_us) {
    ++packets;
    failed_polls += packet_failed_polls;
    stale_wakes += packet_stale_wakes;
    if (!matched) {
      ++aborted;
      return;
    }
    if (!packet_failed_polls) {
      ++immediate;
      return;
    }
    ++waited;
    wait_us += waited_us;
    if (waited_us > max_wait_us) {
      max_wait_us = waited_us;
    }
    ++histogram[WrmHistogramBucket(waited_us)];
    for (size_t i = 0; i < slot_count; ++i) {
      if (slots[i].predicate == predicate) {
        ++slots[i].episodes;
        slots[i].wait_us += waited_us;
        return;
      }
    }
    if (slot_count < slots.size()) {
      WrmPredicateSlot& slot = slots[slot_count++];
      slot.predicate = predicate;
      slot.episodes = 1;
      slot.wait_us = waited_us;
      return;
    }
    ++other_predicate_episodes;
  }

  // The predicate with the most waited time (ties: more episodes, then the
  // first seen). Null when nothing waited.
  const WrmPredicateSlot* TopPredicate() const {
    const WrmPredicateSlot* top = nullptr;
    for (size_t i = 0; i < slot_count; ++i) {
      const WrmPredicateSlot& slot = slots[i];
      if (!top || slot.wait_us > top->wait_us ||
          (slot.wait_us == top->wait_us && slot.episodes > top->episodes)) {
        top = &slot;
      }
    }
    return top;
  }
};

// Outcome of one packet's poll loop.
struct WrmLoopResult {
  bool matched = false;
  uint32_t failed_polls = 0;
  // Signalled event wakes after which the next poll still failed (a stale or
  // early signal: costs one extra poll).
  uint32_t stale_wakes = 0;
};

// The WAIT_REG_MEM poll loop. `poll()` reads the location and returns true
// when the predicate matches; it runs first and again after every blocking
// step, so a wake never skips a re-check and a stale signal only costs one
// more poll. `block(failed_polls)` performs one blocking step and returns a
// WrmBlockResult; `on_block(result)` lets the caller count it. After any step
// but a short yield, `running()` false leaves the loop unmatched (the command
// processor is shutting down). The bound of every event wait is the old sleep,
// so a signal that never comes costs no more than the old behaviour.
template <typename Poll, typename Block, typename OnBlock, typename Running>
WrmLoopResult RunWaitRegMemLoop(Poll&& poll, Block&& block, OnBlock&& on_block, Running&& running) {
  WrmLoopResult result;
  bool last_signaled = false;
  while (!poll()) {
    if (last_signaled) {
      ++result.stale_wakes;
    }
    ++result.failed_polls;
    const WrmBlockResult step = block(result.failed_polls);
    on_block(step);
    last_signaled = step.kind == WrmBlockKind::kEvent && step.signaled;
    if (step.kind != WrmBlockKind::kYield && !running()) {
      return result;
    }
  }
  result.matched = true;
  return result;
}

// Reason tokens are short ASCII words; anything else becomes '_'.
inline std::string SanitizeWrmReason(std::string_view reason) {
  std::string text;
  for (const char c : reason.substr(0, reason.size() < 24 ? reason.size() : 24)) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_' || c == '.';
    text.push_back(ok ? c : '_');
  }
  return text.empty() ? std::string("none") : text;
}

// One ASCII log line:
// MCLA_WRM v=1 kind=<minute|mark|exit> reason= frame= event_wake=0|1 packets=
// immediate= waited= aborted= failed_polls= yields= sleeps= event_waits=
// event_wakes= event_timeouts= stale_wakes= signals= wait_ms= blocked_ms=
// max_wait_ms= h_lt0_5ms= h_0_5to1ms= h_1to2ms= h_2to4ms= h_ge4ms=
// predicates= other_episodes= top=<mem|reg>:<address> top_func= top_ref=
// top_mask= top_episodes= top_wait_ms=
// Milliseconds with three decimals; top=none when nothing waited.
inline std::string FormatWrmLine(std::string_view kind, std::string_view reason, uint64_t frame,
                                 bool event_wake, const WrmCounters& c) {
  std::string line = fmt::format(
      "MCLA_WRM v={} kind={} reason={} frame={} event_wake={} packets={} immediate={} waited={} "
      "aborted={} failed_polls={} yields={} sleeps={} event_waits={} event_wakes={} "
      "event_timeouts={} stale_wakes={} signals={} wait_ms={:.3f} blocked_ms={:.3f} "
      "max_wait_ms={:.3f}",
      kWrmLineVersion, SanitizeWrmReason(kind), SanitizeWrmReason(reason), frame,
      event_wake ? 1 : 0, c.packets, c.immediate, c.waited, c.aborted, c.failed_polls, c.yields,
      c.sleeps, c.event_waits, c.event_wakes, c.event_timeouts, c.stale_wakes, c.signals,
      double(c.wait_us) / 1000.0, double(c.blocked_us) / 1000.0, double(c.max_wait_us) / 1000.0);
  auto out = std::back_inserter(line);
  for (size_t i = 0; i < kWrmHistogramBuckets; ++i) {
    fmt::format_to(out, " {}={}", kWrmHistogramNames[i], c.histogram[i]);
  }
  fmt::format_to(out, " predicates={} other_episodes={}", c.slot_count, c.other_predicate_episodes);
  if (const WrmPredicateSlot* top = c.TopPredicate()) {
    fmt::format_to(out,
                   " top={}:{:08X} top_func={} top_ref={:08X} top_mask={:08X} top_episodes={} "
                   "top_wait_ms={:.3f}",
                   top->predicate.is_memory ? "mem" : "reg", top->predicate.address,
                   top->predicate.function, top->predicate.ref, top->predicate.mask, top->episodes,
                   double(top->wait_us) / 1000.0);
  } else {
    fmt::format_to(out, " top=none");
  }
  return line;
}

}  // namespace rex::graphics
