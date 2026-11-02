#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/readback_slots.h>

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace rex::graphics::test {

namespace {

// Model of the D3D12 read-back path around the pure planner: slot presence,
// mapping, the slot contents (the id of the resolve whose GPU copy was last
// recorded into the slot), the guest copy and the 60-frame / key-cap
// eviction of D3D12CommandProcessor::EvictOldReadbackBuffers.
constexpr uint64_t kEvictionAgeFrames = 60;

struct Key {
  uint32_t address;  // in model units
  uint32_t length;
};

struct Event {
  enum class Kind : uint8_t { kResolve, kFrame };
  Kind kind = Kind::kResolve;
  uint32_t key = 0;
  uint64_t frames = 1;  // kFrame: frames to advance
  bool forced = false;  // the l55 sync rules match (forced unless a miss)
  bool map_fail = false;
  bool alloc_fail = false;
  ReadbackPlanMode mode = ReadbackPlanMode::kSome;
};

struct Delivery {
  uint64_t step;
  uint32_t key;
  uint64_t value;
  bool guard;
  bool operator==(const Delivery& o) const {
    return step == o.step && key == o.key && value == o.value;
  }
};

class Model {
 public:
  Model(bool elide, std::vector<Key> keys, size_t max_keys, uint32_t memory_units)
      : elide_(elide), keys_(std::move(keys)), max_keys_(max_keys), guest_(memory_units, 0) {}

  void Run(const std::vector<Event>& events) {
    for (const Event& e : events) {
      ++step_;
      if (e.kind == Event::Kind::kFrame) {
        frame_ += e.frames;
        Evict();
      } else {
        Resolve(e);
      }
    }
  }

  const std::vector<Delivery>& delivered() const { return delivered_; }
  const std::vector<uint64_t>& guest() const { return guest_; }
  uint64_t recorded = 0;
  uint64_t elided = 0;
  uint64_t guard_misses = 0;
  uint64_t misses = 0;
  uint64_t forced = 0;
  uint64_t copies_now = 0;
  uint64_t evictions = 0;
  uint64_t elided_reads = 0;
  // Resolves whose read_index is the previous slot, by the diagnostic class
  // of that slot (ClassifyReadbackPreviousSlot; no submission is ever known
  // complete in the model, so a written slot is kUnfenced), split by whether
  // the guest copy happened.
  uint64_t prev_unwritten_deferred = 0;
  uint64_t prev_unwritten_copied = 0;
  uint64_t prev_written_deferred = 0;
  uint64_t prev_written_copied = 0;
  // The diagnostic class disagrees with the slot contents (written iff the
  // contents are the bytes of the last resolve that used the slot).
  uint64_t class_mismatches = 0;

 private:
  struct Entry {
    ReadbackSlotState state;
    bool exists[2] = {false, false};
    bool mapped[2] = {false, false};
    uint64_t content[2] = {0, 0};
    // The id of the last resolve that used the slot as its own slot.
    uint64_t last_writer[2] = {0, 0};
    // D3D12 submission_written: the last recorded copy, 0 after an elided
    // copy or a new buffer.
    uint64_t submission_written[2] = {0, 0};
    uint64_t last_used = 0;
  };

  void Evict() {
    uint64_t floor = frame_ > kEvictionAgeFrames ? frame_ - kEvictionAgeFrames : 0;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (entries_.size() > max_keys_ || it->second.last_used < floor) {
        it = entries_.erase(it);
        ++evictions;
      } else {
        ++it;
      }
    }
  }

  void Resolve(const Event& e) {
    const uint64_t id = step_;
    Entry& entry = entries_[e.key];
    entry.last_used = frame_;
    ReadbackPlanInput in;
    in.mode = e.mode;
    uint32_t previous = 1 - entry.state.current_index;
    in.previous_slot_usable = entry.exists[previous] && entry.mapped[previous];
    in.force_unless_miss = e.forced && e.mode != ReadbackPlanMode::kFull;
    in.elide_unread = elide_;
    ReadbackPlan plan = PlanReadbackResolve(entry.state, in);
    uint32_t w = plan.write_index;
    if (!entry.exists[w]) {
      if (e.alloc_fail) {
        return;  // early exit: no state change
      }
      entry.exists[w] = true;
      entry.mapped[w] = !e.map_fail;
      entry.content[w] = 0;  // CreateNotZeroed: unknown bytes
      entry.submission_written[w] = 0;
    }
    if (plan.record_gpu) {
      entry.content[w] = id;
      ++recorded;
    } else {
      ++elided;
    }
    entry.last_writer[w] = id;
    entry.submission_written[w] = plan.record_gpu ? id : 0;
    NoteReadbackSlotWrite(entry.state, w, plan.record_gpu);
    misses += plan.miss ? 1 : 0;
    guard_misses += plan.guard_miss ? 1 : 0;
    forced += plan.forced ? 1 : 0;
    uint32_t r = plan.read_index;
    if (r != w) {
      ReadbackPreviousSlot c =
          ClassifyReadbackPreviousSlot(entry.state.slot_elided[r], entry.submission_written[r], 0);
      bool written = c != ReadbackPreviousSlot::kUnwritten;
      bool holds_last =
          entry.exists[r] && entry.last_writer[r] && entry.content[r] == entry.last_writer[r];
      class_mismatches += written != holds_last ? 1 : 0;
      if (plan.copies_now) {
        (written ? prev_written_copied : prev_unwritten_copied) += 1;
      } else {
        (written ? prev_written_deferred : prev_unwritten_deferred) += 1;
      }
    }
    if (plan.copies_now && entry.exists[r] && entry.mapped[r]) {
      if (entry.state.slot_elided[r]) {
        ++elided_reads;
      }
      const Key& key = keys_[e.key];
      for (uint32_t i = 0; i < key.length; ++i) {
        guest_[key.address + i] = entry.content[r];
      }
      delivered_.push_back({step_, e.key, entry.content[r], plan.guard_miss});
      ++copies_now;
    }
    CommitReadbackResolve(entry.state, plan, e.mode);
  }

  bool elide_;
  std::vector<Key> keys_;
  size_t max_keys_;
  std::vector<uint64_t> guest_;
  std::map<uint32_t, Entry> entries_;
  std::vector<Delivery> delivered_;
  uint64_t frame_ = 1;
  uint64_t step_ = 0;
};

// Overlapping keys: 0/1 and 2/3 share an address with different lengths
// (the livery mip chain), 4 overlaps 0 partially.
std::vector<Key> TestKeys() {
  return {{0, 16}, {0, 8}, {32, 8}, {32, 4}, {12, 10}, {48, 6}, {56, 3}, {40, 2}};
}

struct GenOptions {
  double forced = 0.08;
  double map_fail = 0.01;
  double alloc_fail = 0.01;
  double frame = 0.2;
  double long_gap = 0.02;
  double mode_switch = 0.0;
};

std::vector<Event> Generate(uint64_t seed, size_t count, const GenOptions& o) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> u(0.0, 1.0);
  std::uniform_int_distribution<uint32_t> key_pick(0, 7);
  std::vector<Event> events;
  events.reserve(count);
  ReadbackPlanMode mode = ReadbackPlanMode::kSome;
  for (size_t i = 0; i < count; ++i) {
    Event e;
    if (o.mode_switch > 0.0 && u(rng) < o.mode_switch) {
      double m = u(rng);
      mode = m < 0.5 ? ReadbackPlanMode::kSome
                     : (m < 0.85 ? ReadbackPlanMode::kFast : ReadbackPlanMode::kFull);
    }
    double x = u(rng);
    if (x < o.frame) {
      e.kind = Event::Kind::kFrame;
      e.frames = u(rng) < o.long_gap ? 61 + (rng() % 40) : 1 + (rng() % 3);
    } else {
      e.key = key_pick(rng);
      // Bias towards a few hot keys, as the per-frame families are.
      if (u(rng) < 0.5) {
        e.key %= 3;
      }
      e.forced = u(rng) < o.forced;
      e.map_fail = u(rng) < o.map_fail;
      e.alloc_fail = u(rng) < o.alloc_fail;
    }
    e.mode = mode;
    events.push_back(e);
  }
  return events;
}

}  // namespace

TEST_CASE("Readback slots: steady key records one GPU copy in four", "[graphics][readback-slots]") {
  ReadbackSlotState s;
  ReadbackPlanInput in;
  in.mode = ReadbackPlanMode::kSome;
  in.elide_unread = true;
  // First resolve: the previous slot does not exist (miss, own slot).
  in.previous_slot_usable = false;
  ReadbackPlan p = PlanReadbackResolve(s, in);
  CHECK(p.miss);
  CHECK(p.copies_now);
  CHECK(p.record_gpu);
  CHECK(p.own_slot());
  NoteReadbackSlotWrite(s, p.write_index, p.record_gpu);
  CommitReadbackResolve(s, p, in.mode);
  in.previous_slot_usable = true;
  // Then: counters 1, 2, 3 (deferred; only the third is read by the next
  // resolve), 0 (every-4th copy from the previous slot), and so on.
  const bool expected_record[] = {false, false, true, false, false, false, true, false};
  const bool expected_copy[] = {false, false, false, true, false, false, false, true};
  for (int i = 0; i < 8; ++i) {
    p = PlanReadbackResolve(s, in);
    CHECK(p.record_gpu == expected_record[i]);
    CHECK(p.copies_now == expected_copy[i]);
    CHECK_FALSE(p.guard_miss);
    if (p.copies_now) {
      CHECK(p.read_index == 1 - p.write_index);
      CHECK_FALSE(s.slot_elided[p.read_index]);
    }
    NoteReadbackSlotWrite(s, p.write_index, p.record_gpu);
    CommitReadbackResolve(s, p, in.mode);
  }
}

TEST_CASE("Readback slots: elision off, fast and full always record",
          "[graphics][readback-slots]") {
  for (ReadbackPlanMode mode :
       {ReadbackPlanMode::kSome, ReadbackPlanMode::kFast, ReadbackPlanMode::kFull}) {
    ReadbackSlotState s;
    ReadbackPlanInput in;
    in.mode = mode;
    in.previous_slot_usable = true;
    in.elide_unread = mode != ReadbackPlanMode::kSome;
    for (int i = 0; i < 12; ++i) {
      ReadbackPlan p = PlanReadbackResolve(s, in);
      CHECK(p.record_gpu);
      if (mode == ReadbackPlanMode::kFull) {
        CHECK(p.wait_full);
        CHECK(p.own_slot());
        CHECK(p.copies_now);
      } else if (mode == ReadbackPlanMode::kFast) {
        CHECK(p.copies_now);
        CHECK_FALSE(p.own_slot());
      }
      NoteReadbackSlotWrite(s, p.write_index, p.record_gpu);
      CommitReadbackResolve(s, p, in.mode);
    }
  }
}

TEST_CASE("Readback slots: forced and miss read and record the own slot",
          "[graphics][readback-slots]") {
  ReadbackSlotState s;
  s.current_index = 1;
  s.resolves_since_copy = 1;
  ReadbackPlanInput in;
  in.mode = ReadbackPlanMode::kSome;
  in.elide_unread = true;
  in.previous_slot_usable = true;
  in.force_unless_miss = true;
  ReadbackPlan p = PlanReadbackResolve(s, in);
  CHECK(p.forced);
  CHECK_FALSE(p.miss);
  CHECK(p.copies_now);
  CHECK(p.record_gpu);
  CHECK(p.read_index == 1);
  CHECK(p.next_resolves_since_copy == 0);
  // A miss is never forced (ShouldForceReadbackSync excludes misses).
  in.previous_slot_usable = false;
  p = PlanReadbackResolve(s, in);
  CHECK(p.miss);
  CHECK_FALSE(p.forced);
  CHECK(p.record_gpu);
  CHECK(p.own_slot());
}

TEST_CASE("Readback slots: a switch from some to fast hits the guard",
          "[graphics][readback-slots]") {
  ReadbackSlotState s;
  ReadbackPlanInput in;
  in.mode = ReadbackPlanMode::kSome;
  in.elide_unread = true;
  in.previous_slot_usable = true;
  // Counter 0: this resolve defers and is not read by the next one.
  ReadbackPlan p = PlanReadbackResolve(s, in);
  CHECK_FALSE(p.record_gpu);
  NoteReadbackSlotWrite(s, p.write_index, p.record_gpu);
  CommitReadbackResolve(s, p, in.mode);
  in.mode = ReadbackPlanMode::kFast;
  p = PlanReadbackResolve(s, in);
  CHECK(p.guard_miss);
  CHECK(p.own_slot());
  CHECK(p.record_gpu);
  CHECK(p.copies_now);
}

TEST_CASE("Readback slots: previous slot classes", "[graphics][readback-slots]") {
  CHECK(ClassifyReadbackPreviousSlot(false, 0, 100) == ReadbackPreviousSlot::kUnwritten);
  CHECK(ClassifyReadbackPreviousSlot(true, 5, 100) == ReadbackPreviousSlot::kUnwritten);
  CHECK(ClassifyReadbackPreviousSlot(true, 0, 0) == ReadbackPreviousSlot::kUnwritten);
  CHECK(ClassifyReadbackPreviousSlot(false, 5, 4) == ReadbackPreviousSlot::kUnfenced);
  CHECK(ClassifyReadbackPreviousSlot(false, 5, 5) == ReadbackPreviousSlot::kComplete);
}

// The l63 "all" loading-minute line: read=prev prev_age=-1 decision=deferred
// reason=some_interval. A key resolved for the first time (miss, own slot,
// recorded), then: counter 1 (deferred, elided), counter 2 (deferred, elided,
// read_index on the slot elided by counter 1: never written), counter 3
// (deferred, recorded, read_index on the slot elided by counter 2), then the
// every-4th copy, which reads the slot recorded by counter 3. Only deferred
// resolves point at an unwritten slot; they copy nothing.
TEST_CASE("Readback slots: a deferred resolve may point at an elided slot, never copies from it",
          "[graphics][readback-slots]") {
  ReadbackSlotState s;
  uint64_t submission_written[2] = {0, 0};
  uint64_t submission = 0;
  ReadbackPlanInput in;
  in.mode = ReadbackPlanMode::kSome;
  in.elide_unread = true;
  auto resolve = [&](bool previous_usable) {
    in.previous_slot_usable = previous_usable;
    ReadbackPlan p = PlanReadbackResolve(s, in);
    ++submission;
    ReadbackPreviousSlot c = ReadbackPreviousSlot::kComplete;
    if (!p.own_slot()) {
      c = ClassifyReadbackPreviousSlot(s.slot_elided[p.read_index],
                                       submission_written[p.read_index], 0);
    }
    submission_written[p.write_index] = p.record_gpu ? submission : 0;
    NoteReadbackSlotWrite(s, p.write_index, p.record_gpu);
    CommitReadbackResolve(s, p, in.mode);
    return std::pair<ReadbackPlan, ReadbackPreviousSlot>(p, c);
  };
  // Slot 0: miss, recorded, copied.
  auto [p0, c0] = resolve(false);
  CHECK(p0.miss);
  CHECK(p0.record_gpu);
  CHECK(p0.copies_now);
  // Counter 1: writes slot 1 (new, elided), points at slot 0 (recorded).
  auto [p1, c1] = resolve(true);
  CHECK_FALSE(p1.copies_now);
  CHECK_FALSE(p1.record_gpu);
  CHECK(p1.read_index == 0);
  CHECK(c1 == ReadbackPreviousSlot::kUnfenced);
  // Counter 2: writes slot 0 (elided), points at slot 1 (elided): the l63 line.
  auto [p2, c2] = resolve(true);
  CHECK_FALSE(p2.copies_now);
  CHECK_FALSE(p2.record_gpu);
  CHECK(p2.read_index == 1);
  CHECK(c2 == ReadbackPreviousSlot::kUnwritten);
  // Counter 3: writes slot 1 (recorded: the next resolve reads it), points
  // at slot 0 (elided by counter 2).
  auto [p3, c3] = resolve(true);
  CHECK_FALSE(p3.copies_now);
  CHECK(p3.record_gpu);
  CHECK(p3.read_index == 0);
  CHECK(c3 == ReadbackPreviousSlot::kUnwritten);
  // Every 4th: copies from slot 1, written by counter 3.
  auto [p4, c4] = resolve(true);
  CHECK(p4.copies_now);
  CHECK_FALSE(p4.guard_miss);
  CHECK(p4.read_index == 1);
  CHECK(c4 == ReadbackPreviousSlot::kUnfenced);
  CHECK_FALSE(p4.record_gpu);
  // A switch to fast right after an elided resolve: the guard turns the copy
  // into a miss on the own slot, so an unwritten slot never goes with a guest
  // copy.
  in.mode = ReadbackPlanMode::kFast;
  auto [p5, c5] = resolve(true);
  CHECK(p5.guard_miss);
  CHECK(p5.own_slot());
  CHECK(p5.record_gpu);
  CHECK(p5.copies_now);
  (void)c0;
  (void)c5;
}

TEST_CASE("Readback slots: elision delivers identical guest bytes (random sequences)",
          "[graphics][readback-slots]") {
  uint64_t total_elided = 0;
  uint64_t total_recorded = 0;
  uint64_t total_evictions = 0;
  uint64_t total_forced = 0;
  uint64_t total_misses = 0;
  uint64_t total_copies = 0;
  uint64_t total_unwritten_deferred = 0;
  for (uint64_t seed = 1; seed <= 300; ++seed) {
    GenOptions o;
    if (seed % 3 == 0) {
      o.forced = 0.0;
      o.map_fail = 0.0;
      o.alloc_fail = 0.0;
    }
    if (seed % 5 == 0) {
      o.frame = 0.6;
      o.long_gap = 0.1;
    }
    std::vector<Event> events = Generate(seed, 4000, o);
    Model off(false, TestKeys(), 5, 64);
    Model on(true, TestKeys(), 5, 64);
    off.Run(events);
    on.Run(events);
    REQUIRE(off.delivered().size() == on.delivered().size());
    for (size_t i = 0; i < off.delivered().size(); ++i) {
      REQUIRE(off.delivered()[i] == on.delivered()[i]);
    }
    REQUIRE(off.guest() == on.guest());
    REQUIRE(on.guard_misses == 0);
    REQUIRE(on.elided_reads == 0);
    REQUIRE(off.elided == 0);
    // A copy never reads an unwritten slot; without elision no slot that a
    // resolve points at is unwritten; the class matches the contents.
    REQUIRE(on.prev_unwritten_copied == 0);
    REQUIRE(off.prev_unwritten_copied == 0);
    REQUIRE(off.prev_unwritten_deferred == 0);
    REQUIRE(on.class_mismatches == 0);
    REQUIRE(off.class_mismatches == 0);
    total_unwritten_deferred += on.prev_unwritten_deferred;
    REQUIRE(on.misses == off.misses);
    REQUIRE(on.forced == off.forced);
    REQUIRE(on.copies_now == off.copies_now);
    total_elided += on.elided;
    total_recorded += on.recorded;
    total_evictions += on.evictions;
    total_forced += on.forced;
    total_misses += on.misses;
    total_copies += on.copies_now;
  }
  // The sequences exercised every path.
  CHECK(total_elided > total_recorded / 2);
  CHECK(total_evictions > 1000);
  CHECK(total_forced > 1000);
  CHECK(total_misses > 1000);
  CHECK(total_copies > 10000);
  // The l63 artifact occurs (deferred resolves pointing at elided slots).
  CHECK(total_unwritten_deferred > 1000);
}

TEST_CASE("Readback slots: steady sequences elide about three copies in four",
          "[graphics][readback-slots]") {
  GenOptions o;
  o.forced = 0.0;
  o.map_fail = 0.0;
  o.alloc_fail = 0.0;
  o.long_gap = 0.0;
  std::vector<Event> events = Generate(7, 20000, o);
  Model off(false, TestKeys(), 256, 64);
  Model on(true, TestKeys(), 256, 64);
  off.Run(events);
  on.Run(events);
  REQUIRE(off.delivered() == on.delivered());
  REQUIRE(off.guest() == on.guest());
  double share = double(on.elided) / double(on.elided + on.recorded);
  CHECK(share > 0.70);
  CHECK(share < 0.76);
}

TEST_CASE("Readback slots: mode switches never read an elided slot", "[graphics][readback-slots]") {
  uint64_t guards = 0;
  for (uint64_t seed = 1001; seed <= 1100; ++seed) {
    GenOptions o;
    o.mode_switch = 0.02;
    std::vector<Event> events = Generate(seed, 3000, o);
    Model off(false, TestKeys(), 5, 64);
    Model on(true, TestKeys(), 5, 64);
    off.Run(events);
    on.Run(events);
    REQUIRE(on.elided_reads == 0);
    REQUIRE(on.prev_unwritten_copied == 0);
    REQUIRE(on.class_mismatches == 0);
    REQUIRE(off.guard_misses == 0);
    REQUIRE(off.delivered().size() == on.delivered().size());
    for (size_t i = 0; i < off.delivered().size(); ++i) {
      const Delivery& a = off.delivered()[i];
      const Delivery& b = on.delivered()[i];
      REQUIRE(a.step == b.step);
      if (b.guard) {
        // The guard reads the own, freshly recorded slot.
        REQUIRE(b.value == b.step);
      } else {
        REQUIRE(a == b);
      }
    }
    guards += on.guard_misses;
  }
  CHECK(guards > 0);
}

TEST_CASE("Readback slots: counters and report line", "[graphics][readback-slots]") {
  ReadbackCounters c;
  ReadbackPlan p;
  p.record_gpu = true;
  p.miss = true;
  c.CountPlan(p, 100);
  p.record_gpu = false;
  p.miss = false;
  c.CountPlan(p, 300);
  c.frames = 2;
  c.memcpy_ns = 1500000;
  c.gpu_ticks = 5000;
  std::string line = ReadbackReport(c, "minute", "minute", 42, "some", true, true, 1000000);
  CHECK(line.rfind("MCLA_READBACK v=1 kind=minute reason=minute frame=42 frames=2 mode=some "
                   "elide=1 timing=1 resolves=2 ",
                   0) == 0);
  CHECK(line.find(" misses=1 ") != std::string::npos);
  CHECK(line.find(" gpu_copies_recorded=1 gpu_copies_elided=1 ") != std::string::npos);
  CHECK(line.find(" bytes_gpu_copied=100 bytes_gpu_elided=300 ") != std::string::npos);
  CHECK(line.find(" memcpy_ms=1.500 ") != std::string::npos);
  CHECK(line.find(" gpu_readback_ms=5.000 gpu_readback_ms_per_frame=2.500 ") != std::string::npos);
  CHECK(ReadbackPlanModeName(ReadbackPlanMode::kFull) == std::string("full"));
}

}  // namespace rex::graphics::test
