/**
 * @file        rex/graphics/util/placed_heap_allocator.h
 * @brief       l79: sub-allocator for D3D12 placed texture heaps.
 *
 * d3d12_texture_placed_heaps places texture resources inside large shared
 * ID3D12Heap blocks (CreatePlacedResource) instead of one committed resource
 * (one kernel allocation, NtGdiDdDDICreateAllocation) per texture. This class
 * is the API-free bookkeeping: fixed-size blocks, each with an offset-sorted
 * free list (first fit, per-allocation power-of-two alignment, coalescing on
 * free). The D3D12 texture cache owns the heaps (one per block index).
 *
 * Design from the idea in docs/evidence/M11-external-repos-survey-20261108.md
 * (first fit in large heaps, committed fallback for oversized resources); no
 * third-party code. Header-only, unit-tested
 * (tests/unit/graphics/placed_heap_allocator_test.cpp).
 */

#pragma once

#include <cstdint>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace rex::graphics {

struct PlacedHeapAllocation {
  static constexpr uint32_t kNoBlock = UINT32_MAX;
  uint32_t block = kNoBlock;
  uint64_t offset = 0;
  uint64_t size = 0;
  bool valid() const { return block != kNoBlock; }
};

class PlacedHeapAllocator {
 public:
  explicit PlacedHeapAllocator(uint64_t block_size) : block_size_(block_size) {}

  uint64_t block_size() const { return block_size_; }

  // Whether a resource of this size may be placed at all (else: committed).
  // A resource larger than half a block would waste most of a block.
  bool Fits(uint64_t size) const { return size != 0 && size <= block_size_ / 2; }

  // First fit over the live blocks, lowest block and lowest offset first.
  // alignment: a power of two (0 or 1 = none). Invalid when no block has room
  // (the caller may AddBlock() and retry) or the size does not fit.
  PlacedHeapAllocation Allocate(uint64_t size, uint64_t alignment) {
    PlacedHeapAllocation result;
    if (!Fits(size) || !IsPowerOfTwoOrZero(alignment)) {
      return result;
    }
    if (alignment == 0) {
      alignment = 1;
    }
    for (uint32_t i = 0; i < uint32_t(blocks_.size()); ++i) {
      Block& block = blocks_[i];
      if (!block.live || block_size_ - block.used < size) {
        continue;
      }
      for (auto it = block.free.begin(); it != block.free.end(); ++it) {
        const uint64_t range_start = it->first;
        const uint64_t range_end = it->first + it->second;
        const uint64_t aligned = AlignUp(range_start, alignment);
        if (aligned >= range_end || range_end - aligned < size) {
          continue;
        }
        block.free.erase(it);
        if (aligned > range_start) {
          block.free.emplace(range_start, aligned - range_start);
        }
        if (aligned + size < range_end) {
          block.free.emplace(aligned + size, range_end - (aligned + size));
        }
        block.used += size;
        ++block.allocations;
        used_bytes_ += size;
        result.block = i;
        result.offset = aligned;
        result.size = size;
        return result;
      }
    }
    return result;
  }

  // A new empty block (the caller has created its heap). Reuses the lowest
  // released slot, so block indices stay small.
  uint32_t AddBlock() {
    uint32_t index = 0;
    while (index < uint32_t(blocks_.size()) && blocks_[index].live) {
      ++index;
    }
    if (index == uint32_t(blocks_.size())) {
      blocks_.emplace_back();
    }
    Block& block = blocks_[index];
    block.live = true;
    block.used = 0;
    block.allocations = 0;
    block.free.clear();
    block.free.emplace(0, block_size_);
    ++live_blocks_;
    return index;
  }

  // Returns the space of a valid allocation; merges it with free neighbours.
  // false for an allocation this allocator does not hold.
  bool Free(const PlacedHeapAllocation& allocation) {
    if (!allocation.valid() || allocation.block >= blocks_.size()) {
      return false;
    }
    Block& block = blocks_[allocation.block];
    if (!block.live || !allocation.size || allocation.offset + allocation.size > block_size_ ||
        block.used < allocation.size || !block.allocations) {
      return false;
    }
    uint64_t start = allocation.offset;
    uint64_t length = allocation.size;
    auto next = block.free.lower_bound(start);
    // Overlap with a free range = double free / foreign allocation.
    if (next != block.free.end() && next->first < start + length) {
      return false;
    }
    if (next != block.free.begin()) {
      auto prev = std::prev(next);
      if (prev->first + prev->second > start) {
        return false;
      }
      if (prev->first + prev->second == start) {
        start = prev->first;
        length += prev->second;
        block.free.erase(prev);
      }
    }
    if (next != block.free.end() && next->first == start + length) {
      length += next->second;
      block.free.erase(next);
    }
    block.free.emplace(start, length);
    block.used -= allocation.size;
    --block.allocations;
    used_bytes_ -= allocation.size;
    return true;
  }

  // Empty live blocks beyond the first keep_empty (lowest indices) are marked
  // released and returned; the caller releases their heaps.
  std::vector<uint32_t> TakeReleasableBlocks(uint32_t keep_empty) {
    std::vector<uint32_t> released;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < uint32_t(blocks_.size()); ++i) {
      Block& block = blocks_[i];
      if (!block.live || block.allocations) {
        continue;
      }
      if (kept < keep_empty) {
        ++kept;
        continue;
      }
      block.live = false;
      block.free.clear();
      --live_blocks_;
      released.push_back(i);
    }
    return released;
  }

  uint32_t live_blocks() const { return live_blocks_; }
  uint64_t used_bytes() const { return used_bytes_; }
  uint64_t reserved_bytes() const { return uint64_t(live_blocks_) * block_size_; }
  uint32_t block_slots() const { return uint32_t(blocks_.size()); }
  bool block_live(uint32_t index) const { return index < blocks_.size() && blocks_[index].live; }
  uint64_t block_used(uint32_t index) const {
    return index < blocks_.size() ? blocks_[index].used : 0;
  }
  // Free ranges in a block (fragmentation checks).
  size_t block_free_ranges(uint32_t index) const {
    return index < blocks_.size() ? blocks_[index].free.size() : 0;
  }

  static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return alignment <= 1 ? value : (value + alignment - 1) & ~(alignment - 1);
  }
  static bool IsPowerOfTwoOrZero(uint64_t value) { return (value & (value - 1)) == 0; }

 private:
  struct Block {
    bool live = false;
    uint64_t used = 0;
    uint32_t allocations = 0;
    std::map<uint64_t, uint64_t> free;  // offset -> length
  };

  uint64_t block_size_;
  std::vector<Block> blocks_;
  uint32_t live_blocks_ = 0;
  uint64_t used_bytes_ = 0;
};

// Per-minute counters of the placed texture heaps (MCLA_TEXHEAP kind=minute).
struct TextureHeapMinuteStats {
  uint64_t creates = 0;     // texture resources created (placed + committed)
  uint64_t placements = 0;  // ... placed in a heap
  uint64_t fallbacks = 0;   // ... committed while placed heaps were on
  uint64_t oversized = 0;   // ... of the fallbacks: larger than half a block
  uint64_t heaps_created = 0;
  uint64_t heaps_released = 0;
  uint64_t heap_failures = 0;  // CreateHeap or CreatePlacedResource failed
  uint64_t create_us_total = 0;
  uint64_t create_us_max = 0;
  uint64_t heap_create_us_max = 0;

  void NoteCreate(uint64_t us) {
    ++creates;
    create_us_total += us;
    if (us > create_us_max) {
      create_us_max = us;
    }
  }
  uint64_t create_us_avg() const { return creates ? create_us_total / creates : 0; }
};

// enabled: d3d12_texture_placed_heaps in effect; heaps/used/reserved: the
// allocator state at the report.
inline std::string FormatTextureHeapMinuteFields(const TextureHeapMinuteStats& s, bool enabled,
                                                 uint32_t heaps, uint64_t used_bytes,
                                                 uint64_t reserved_bytes) {
  return fmt::format(
      "placed={} heaps={} used_mb={} reserved_mb={} creates={} placements={} fallbacks={} "
      "oversized={} heaps_created={} heaps_released={} heap_failures={} create_us_avg={} "
      "create_us_max={} heap_create_us_max={}",
      int(enabled), heaps, (used_bytes + (1u << 20) - 1) >> 20,
      (reserved_bytes + (1u << 20) - 1) >> 20, s.creates, s.placements, s.fallbacks, s.oversized,
      s.heaps_created, s.heaps_released, s.heap_failures, s.create_us_avg(), s.create_us_max,
      s.heap_create_us_max);
}

}  // namespace rex::graphics
