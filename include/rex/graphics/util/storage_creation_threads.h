#pragma once

#include <algorithm>
#include <cstddef>

namespace rex::graphics {

// Pipeline creation from the shader storage at startup (M10-003 step 1).
// Pure logic, unit-tested without a GPU.

// Number of creation threads to run while the pipelines read from the storage
// are created: up to one per logical processor minus one (the calling thread
// creates pipelines too), never fewer than the persistent creation threads and
// never more threads than stored pipelines minus one. With no stored pipelines
// nothing is added.
constexpr size_t StorageCreationThreadCount(size_t stored_pipelines, size_t logical_processors,
                                            size_t persistent_threads) {
  if (!stored_pipelines) {
    return persistent_threads;
  }
  const size_t processors = logical_processors ? logical_processors : size_t(1);
  return std::max(std::min(stored_pipelines, processors) - size_t(1), persistent_threads);
}

// Whether a startup creation must wait for pipelines still being created on
// the creation threads before the guest starts. A blocking initialisation
// waits whenever any thread is still busy, whether or not extra threads were
// started, so "Created N ... in X ms" covers the whole creation.
constexpr bool StorageCreationMustAwait(bool blocking, size_t busy_threads) {
  return blocking && busy_threads != 0;
}

}  // namespace rex::graphics
