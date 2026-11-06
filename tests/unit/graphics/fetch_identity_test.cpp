#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/fetch_identity.h>
#include <rex/perf/perf_split.h>

#include <array>
#include <string>

namespace rex::graphics::test {

namespace {

uint32_t Be(uint32_t value) {
  return rex::byte_swap(value);
}

struct Payload {
  uint32_t value = 0;
};

// Model of the D3D12 fetch-constant write path with the cache on: the texture
// slot in-sync bits and the vertex-fetch residency bits that a write clears.
struct FetchWriteModel {
  std::array<uint32_t, 192> regs{};
  uint32_t texture_in_sync = UINT32_MAX;
  uint64_t vertex_in_sync = UINT64_MAX;
  bool fetch_cbuffer_up_to_date = true;

  void Write(uint32_t first_dword, const uint32_t* big_endian, uint32_t count) {
    const FetchDwordStoreResult r =
        StoreFetchDwordsCompare(regs.data() + first_dword, big_endian, count);
    if (!r.changed) {
      return;
    }
    const uint32_t first = first_dword + r.first_changed;
    const uint32_t last = first_dword + r.last_changed;
    fetch_cbuffer_up_to_date = false;
    for (uint32_t slot = first / 6; slot <= last / 6; ++slot) {
      texture_in_sync &= ~(UINT32_C(1) << slot);
    }
    // The model tracks the first 64 vertex fetch slots only.
    for (uint32_t vfetch = first / 2; vfetch <= last / 2 && vfetch < 64; ++vfetch) {
      vertex_in_sync &= ~(UINT64_C(1) << vfetch);
    }
  }
};

}  // namespace

TEST_CASE("Fetch dword store reports unchanged and changed dwords", "[graphics][fetch-identity]") {
  uint32_t regs[6] = {1, 2, 3, 4, 5, 6};
  const uint32_t same[6] = {Be(1), Be(2), Be(3), Be(4), Be(5), Be(6)};
  FetchDwordStoreResult r = StoreFetchDwordsCompare(regs, same, 6);
  CHECK_FALSE(r.changed);
  CHECK(r.unchanged == 6);

  const uint32_t two_changed[6] = {Be(1), Be(20), Be(3), Be(4), Be(50), Be(6)};
  r = StoreFetchDwordsCompare(regs, two_changed, 6);
  CHECK(r.changed);
  CHECK(r.first_changed == 1);
  CHECK(r.last_changed == 4);
  CHECK(r.unchanged == 4);
  CHECK(regs[1] == 20);
  CHECK(regs[4] == 50);
  CHECK(regs[0] == 1);
}

TEST_CASE("Unchanged fetch block re-sends keep every binding in sync",
          "[graphics][fetch-identity]") {
  FetchWriteModel model;
  std::array<uint32_t, 192> block{};
  for (uint32_t i = 0; i < 192; ++i) {
    block[i] = Be(0x1000 + i);
  }
  model.Write(0, block.data(), 192);
  CHECK(model.texture_in_sync == 0);
  CHECK(model.vertex_in_sync == 0);

  // The guest re-sends the identical block before the next draw.
  model.texture_in_sync = UINT32_MAX;
  model.vertex_in_sync = UINT64_MAX;
  model.fetch_cbuffer_up_to_date = true;
  model.Write(0, block.data(), 192);
  CHECK(model.texture_in_sync == UINT32_MAX);
  CHECK(model.vertex_in_sync == UINT64_MAX);
  CHECK(model.fetch_cbuffer_up_to_date);

  // One dword of fetch constant 2 (dword 13) changes: only texture slot 2 and
  // vertex fetch 6 (dwords 12-13) are invalidated, plus the fetch cbuffer.
  block[13] = Be(0xABCD);
  model.Write(0, block.data(), 192);
  CHECK(model.texture_in_sync == (UINT32_MAX & ~(UINT32_C(1) << 2)));
  CHECK(model.vertex_in_sync == (UINT64_MAX & ~(UINT64_C(1) << 6)));
  CHECK_FALSE(model.fetch_cbuffer_up_to_date);
  CHECK(model.regs[13] == 0xABCD);

  // Single-dword writes (the WriteRegister path) behave the same way.
  model.texture_in_sync = UINT32_MAX;
  const uint32_t same = Be(0xABCD);
  model.Write(13, &same, 1);
  CHECK(model.texture_in_sync == UINT32_MAX);
  const uint32_t other = Be(7);
  model.Write(191, &other, 1);
  CHECK(model.texture_in_sync == (UINT32_MAX & ~(UINT32_C(1) << 31)));
}

TEST_CASE("Changes spanning two fetch constants invalidate both", "[graphics][fetch-identity]") {
  FetchWriteModel model;
  std::array<uint32_t, 12> block{};
  block[5] = Be(1);  // last dword of fetch constant 0
  block[6] = Be(2);  // first dword of fetch constant 1
  model.Write(0, block.data(), 12);
  CHECK(model.texture_in_sync == (UINT32_MAX & ~UINT32_C(0b11)));
  CHECK(model.vertex_in_sync == (UINT64_MAX & ~(UINT64_C(0b11) << 2)));
}

TEST_CASE("Fetch identity generation starts above zero and bumps", "[graphics][fetch-identity]") {
  FetchIdentityGeneration generation;
  CHECK(generation.Current() == 1);
  generation.Bump();
  generation.Bump();
  CHECK(generation.Current() == 3);
}

TEST_CASE("Fetch memo hits only with equal dwords and generation", "[graphics][fetch-identity]") {
  FetchIdentityMemo<Payload, 4> memo;
  FetchIdentityGeneration generation;
  const FetchDwords a = {0x12, 0x80001000, 0x00FF00FF, 0x1, 0x2, 0x3};
  // A zero-initialised table never hits, even for all-zero dwords.
  CHECK(memo.Find(FetchDwords{}, generation.Current()) == nullptr);
  CHECK(memo.Find(a, generation.Current()) == nullptr);

  memo.Store(a, generation.Current(), Payload{42});
  const Payload* hit = memo.Find(a, generation.Current());
  REQUIRE(hit != nullptr);
  CHECK(hit->value == 42);

  FetchDwords b = a;
  b[1] ^= 0x1000;
  CHECK(memo.Find(b, generation.Current()) == nullptr);

  // Any change of the key -> texture mapping (creation, eviction, scaled
  // resolve page change) bumps the generation: the entry is stale.
  generation.Bump();
  CHECK(memo.Find(a, generation.Current()) == nullptr);

  memo.Store(a, generation.Current(), Payload{43});
  REQUIRE(memo.Find(a, generation.Current()) != nullptr);
  memo.Clear();
  CHECK(memo.Find(a, generation.Current()) == nullptr);
}

TEST_CASE("Fetch memo entry stamped before a racing bump is stale", "[graphics][fetch-identity]") {
  FetchIdentityMemo<Payload, 4> memo;
  FetchIdentityGeneration generation;
  const FetchDwords a = {1, 2, 3, 4, 5, 6};
  // The lookup reads the generation, a texture is created (or a write fault
  // clears a scaled-resolve page) during the slow path, then the entry is
  // stored with the generation read before the lookup.
  const uint64_t before = generation.Current();
  generation.Bump();
  memo.Store(a, before, Payload{1});
  CHECK(memo.Find(a, generation.Current()) == nullptr);
}

TEST_CASE("Fetch memo collisions replace, never alias", "[graphics][fetch-identity]") {
  FetchIdentityMemo<Payload, 1> memo;  // two entries: many collisions
  const uint64_t gen = 1;
  uint32_t stored = 0;
  for (uint32_t i = 0; i < 64; ++i) {
    const FetchDwords d = {i, i * 3, 0, 0, 0, 0};
    memo.Store(d, gen, Payload{i});
    const Payload* hit = memo.Find(d, gen);
    REQUIRE(hit != nullptr);
    CHECK(hit->value == i);
    ++stored;
  }
  // Every key either misses or returns its own payload.
  for (uint32_t i = 0; i < 64; ++i) {
    const FetchDwords d = {i, i * 3, 0, 0, 0, 0};
    const Payload* hit = memo.Find(d, gen);
    if (hit) {
      CHECK(hit->value == i);
    }
  }
  CHECK(stored == 64);
}

TEST_CASE("Sampler memo slot matches only identical inputs", "[graphics][fetch-identity]") {
  SamplerMemoSlot slot;
  const FetchDwords zero{};
  const uint32_t binding = PackSamplerBindingWord(0, 0, 0, 0, 0);
  CHECK((binding >> 31) == 1);
  // An empty slot never matches.
  CHECK_FALSE(slot.Matches(zero, binding, 0));
  const FetchDwords d = {1, 2, 3, 4, 5, 6};
  slot.Store(d, binding, 3, 0x1234);
  CHECK(slot.Matches(d, binding, 3));
  CHECK(slot.result == 0x1234);
  CHECK_FALSE(slot.Matches(d, binding, 4));
  CHECK_FALSE(slot.Matches(d, PackSamplerBindingWord(1, 0, 0, 0, 0), 3));
  CHECK_FALSE(slot.Matches(d, PackSamplerBindingWord(0, 3, 0, 0, 0), 3));
  FetchDwords e = d;
  e[5] = 7;
  CHECK_FALSE(slot.Matches(e, binding, 3));
  // Field packing keeps the overrides apart.
  CHECK(PackSamplerBindingWord(0, 1, 0, 0, 0) != PackSamplerBindingWord(0, 0, 1, 0, 0));
  CHECK(PackSamplerBindingWord(0, 0, 0, 1, 0) != PackSamplerBindingWord(0, 0, 0, 0, 1));
  CHECK(PackSamplerBindingWord(31, 3, 3, 3, 7) != PackSamplerBindingWord(31, 3, 3, 3, 6));
}

TEST_CASE("Fetch identity counters add up", "[graphics][fetch-identity]") {
  FetchIdentityCounters a;
  a.fetch_dw_written = 10;
  a.smp_memo_hit = 2;
  FetchIdentityCounters b;
  b.fetch_dw_written = 5;
  b.tex_memo_hit = 7;
  a += b;
  CHECK(a.fetch_dw_written == 15);
  CHECK(a.tex_memo_hit == 7);
  CHECK(a.smp_memo_hit == 2);
}

TEST_CASE("Perf split appends the fetch identity fields", "[graphics][fetch-identity][perf]") {
  rex::perf::PerfSplitInputs previous;
  rex::perf::PerfSplitInputs current;
  previous.counters.fetch_dw_written = 100;
  current.counters.fetch_dw_written = 1100;
  current.counters.fetch_dw_unchanged = 900;
  current.counters.tex_memo_hit = 40;
  current.counters.tex_memo_miss = 10;
  current.counters.tex_gen_bumps = 3;
  current.counters.smp_memo_miss = 5;
  const rex::perf::PerfSplitWindow w = rex::perf::ComputePerfSplitWindow(previous, current, 1);
  CHECK(w.fetch_dw_written == 1000);
  CHECK(w.fetch_dw_unchanged == 900);
  CHECK(w.tex_memo_hit == 40);
  CHECK(w.tex_memo_miss == 10);
  CHECK(w.tex_gen_bumps == 3);
  CHECK(w.smp_memo_miss == 5);

  const std::string line = rex::perf::FormatPerfSplitLine(w);
  CHECK(line.rfind("MCLA_PERF_SPLIT v=1 ", 0) == 0);
  const size_t gpu = line.find(" gpu_submissions=");
  const size_t written = line.find(" fetch_dw_written=1000 ");
  const size_t last = line.find(" smp_memo_miss=5 ");
  const size_t tid = line.find(" render_tid=");
  REQUIRE(gpu != std::string::npos);
  REQUIRE(written != std::string::npos);
  REQUIRE(last != std::string::npos);
  REQUIRE(tid != std::string::npos);
  CHECK(gpu < written);
  CHECK(written < last);
  CHECK(last < tid);
  const char* const keys[] = {
      " fetch_dw_unchanged=", " tex_slots=",         " tex_key_changed=", " tex_memo_hit=",
      " tex_memo_miss=",      " tex_memo_mismatch=", " tex_gen_bumps=",   " smp_memo_hit="};
  for (const char* key : keys) {
    const size_t first = line.find(key);
    REQUIRE(first != std::string::npos);
    CHECK(line.find(key, first + 1) == std::string::npos);
  }
}

}  // namespace rex::graphics::test
