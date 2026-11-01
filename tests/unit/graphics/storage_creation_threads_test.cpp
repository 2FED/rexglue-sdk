#include <catch2/catch_test_macros.hpp>
#include <rex/graphics/util/storage_creation_threads.h>

namespace rex::graphics::test {

TEST_CASE("Storage creation adds threads up to the processors minus one",
          "[graphics][storage-creation]") {
  // 16 logical processors, 12 persistent (3/4): 15 threads while creating.
  CHECK(StorageCreationThreadCount(3605, 16, 12) == 15);
  // No persistent threads (d3d12_pipeline_creation_threads=0): still uses
  // startup threads, as upstream.
  CHECK(StorageCreationThreadCount(3605, 16, 0) == 15);
  CHECK(StorageCreationThreadCount(3605, 8, 6) == 7);
}

TEST_CASE("Storage creation never goes below the persistent threads",
          "[graphics][storage-creation]") {
  CHECK(StorageCreationThreadCount(2, 16, 12) == 12);
  CHECK(StorageCreationThreadCount(1, 16, 12) == 12);
  CHECK(StorageCreationThreadCount(1, 16, 0) == 0);
  CHECK(StorageCreationThreadCount(0, 16, 12) == 12);
  CHECK(StorageCreationThreadCount(0, 16, 0) == 0);
  // A forced count above the processors is kept.
  CHECK(StorageCreationThreadCount(500, 4, 6) == 6);
}

TEST_CASE("Storage creation with few pipelines uses at most pipelines minus one",
          "[graphics][storage-creation]") {
  CHECK(StorageCreationThreadCount(5, 16, 0) == 4);
  CHECK(StorageCreationThreadCount(5, 16, 3) == 4);
  // Unknown processor count is treated as one (no extra threads).
  CHECK(StorageCreationThreadCount(500, 0, 0) == 0);
  CHECK(StorageCreationThreadCount(500, 1, 0) == 0);
}

TEST_CASE("Blocking storage creation waits for busy threads", "[graphics][storage-creation]") {
  CHECK(StorageCreationMustAwait(true, 3));
  CHECK_FALSE(StorageCreationMustAwait(true, 0));
  CHECK_FALSE(StorageCreationMustAwait(false, 3));
  CHECK_FALSE(StorageCreationMustAwait(false, 0));
}

}  // namespace rex::graphics::test
