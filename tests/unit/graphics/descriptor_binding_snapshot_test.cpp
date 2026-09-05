#include <array>
#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/descriptor_binding_snapshot.h>

namespace rex::graphics::test {
namespace {
struct Info {
  uint64_t sampler = 0;
  uint64_t imageView = 0;
  uint32_t imageLayout = 0;
};
using Snapshot = DescriptorBindingSnapshot<Info, uint64_t, uint64_t>;
const int pipeline_a = 0, pipeline_b = 0;
const Snapshot::Context context{1, 2, &pipeline_a, 3};
const std::array<Info, 3> infos{{{0, 10, 5}, {0, 20, 5}, {30, 0, 0}}};
}  // namespace

TEST_CASE("Descriptor reuse requires the current successful set and exact ordered values",
          "[graphics][descriptor-reuse]") {
  Snapshot snapshot;
  CHECK_FALSE(snapshot.Matches(context, 4, true, infos, 2));
  snapshot.Commit(context, 4, infos, 2);
  CHECK(snapshot.Matches(context, 4, true, infos, 2));
  CHECK_FALSE(snapshot.Matches(context, 4, false, infos, 2));
  CHECK_FALSE(snapshot.Matches(context, 0, true, infos, 2));
  CHECK_FALSE(snapshot.Matches(context, 9, true, infos, 2));
  CHECK_FALSE(snapshot.Matches(context, 4, true, infos, 1));
  CHECK_FALSE(snapshot.Matches(context, 4, true, std::span(infos).first(2), 2));
  CHECK_FALSE(snapshot.Matches(context, 4, true, {}, 0));
  auto changed = infos;
  changed[0].imageView = 11;
  CHECK_FALSE(snapshot.Matches(context, 4, true, changed, 2));
  changed = infos;
  changed[0].imageLayout = 6;
  CHECK_FALSE(snapshot.Matches(context, 4, true, changed, 2));
  changed = infos;
  changed[2].sampler = 31;
  CHECK_FALSE(snapshot.Matches(context, 4, true, changed, 2));
  changed = infos;
  std::swap(changed[0], changed[1]);
  CHECK_FALSE(snapshot.Matches(context, 4, true, changed, 2));
  // Inapplicable fields (and padding) have no descriptor meaning.
  changed = infos;
  changed[0].sampler = 99;
  changed[2].imageView = 99;
  changed[2].imageLayout = 99;
  CHECK(snapshot.Matches(context, 4, true, changed, 2));
}

TEST_CASE("Descriptor reuse never crosses frame submission or pipeline layout epochs",
          "[graphics][descriptor-reuse]") {
  Snapshot snapshot;
  snapshot.Commit(context, 4, infos, 2);
  for (unsigned field = 0; field < 4; ++field) {
    auto changed = context;
    if (field == 0) ++changed.frame;
    if (field == 1) ++changed.submission;
    if (field == 2) changed.pipeline_layout = &pipeline_b;
    if (field == 3) ++changed.stage_layout;
    CHECK_FALSE(snapshot.Matches(changed, 4, true, infos, 2));
  }
  // A -> B -> A, guest -> external -> guest, pool reset/handle recycling,
  // and submission restart must explicitly invalidate, even if values return.
  for (unsigned transition = 0; transition < 4; ++transition) {
    snapshot.Commit(context, 4, infos, 2);
    snapshot.Invalidate();
    CHECK_FALSE(snapshot.Matches(context, 4, true, infos, 2));
  }
}

TEST_CASE("Descriptor misses and partial writes cannot commit stale stages",
          "[graphics][descriptor-reuse]") {
  Snapshot vertex, pixel;
  vertex.Commit(context, 4, infos, 2);
  pixel.Commit(context, 5, infos, 2);
  auto changed = infos;
  changed[0].imageView = 44;
  CHECK_FALSE(vertex.Matches(context, 4, true, changed, 2));
  vertex.Invalidate();  // VS allocated new set 6, then PS allocation fails.
  pixel.Invalidate();
  CHECK_FALSE(vertex.Matches(context, 6, true, changed, 2));
  CHECK_FALSE(vertex.Matches(context, 4, true, infos, 2));
  CHECK_FALSE(pixel.Matches(context, 5, true, infos, 2));
  vertex.Commit(context, 6, changed, 2);
  pixel.Commit(context, 7, infos, 2);
  CHECK(vertex.Matches(context, 6, true, changed, 2));
  CHECK(pixel.Matches(context, 7, true, infos, 2));
  vertex.Invalidate();
  CHECK(pixel.Matches(context, 7, true, infos, 2));
}

TEST_CASE("Descriptor snapshots support null images and single resource kinds but not empty stages",
          "[graphics][descriptor-reuse]") {
  Snapshot snapshot;
  std::array<Info, 1> null_image{};
  snapshot.Commit(context, 4, null_image, 1);
  CHECK(snapshot.Matches(context, 4, true, null_image, 1));
  null_image[0].imageView = 9;
  CHECK_FALSE(snapshot.Matches(context, 4, true, null_image, 1));
  std::array<Info, 1> sampler{{{30, 0, 0}}};
  snapshot.Commit(context, 4, sampler, 0);
  CHECK(snapshot.Matches(context, 4, true, sampler, 0));
  snapshot.Commit(context, 4, {}, 0);
  CHECK_FALSE(snapshot.Matches(context, 4, true, sampler, 0));
  snapshot.Commit(context, 0, infos, 2);
  CHECK_FALSE(snapshot.Matches(context, 0, true, infos, 2));
  snapshot.Commit(context, 4, infos, 4);
  CHECK_FALSE(snapshot.Matches(context, 4, true, infos, 4));
}

TEST_CASE("Descriptor snapshots copy scratch values rather than aliasing later refreshes",
          "[graphics][descriptor-reuse]") {
  Snapshot snapshot;
  auto scratch = infos;
  snapshot.Commit(context, 4, scratch, 2);
  scratch[0].imageView = 100;
  CHECK_FALSE(snapshot.Matches(context, 4, true, scratch, 2));
  CHECK(snapshot.Matches(context, 4, true, infos, 2));
  // Refreshing contents with identical handles doesn't invalidate descriptors.
  // Callers must still request textures/samplers and run uploads/barriers.
  scratch = infos;
  CHECK(snapshot.Matches(context, 4, true, scratch, 2));
}
}  // namespace rex::graphics::test
