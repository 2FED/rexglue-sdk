#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace rex::graphics {

// One successfully written immutable texture/sampler descriptor set. This is
// deliberately not a hash cache: only the immediately retained stage value is
// reusable. ImageInfo exposes imageView, imageLayout and sampler (as Vulkan
// does); compare fields, never object padding. No GPU calls or ownership here.
template <typename ImageInfo, typename DescriptorSet, typename DescriptorLayout>
class DescriptorBindingSnapshot {
 public:
  struct Context {
    uint64_t frame;
    uint64_t submission;
    const void* pipeline_layout;
    DescriptorLayout stage_layout;
    bool operator==(const Context&) const = default;
  };

  void Invalidate() { valid_ = false; }

  bool Matches(const Context& context, DescriptorSet set, bool values_valid,
               std::span<const ImageInfo> infos, uint32_t texture_count) const {
    if (!valid_ || !values_valid || set == DescriptorSet{} || set != set_ ||
        context != context_ || infos.empty() || infos.size() != infos_.size() ||
        texture_count != texture_count_ || texture_count > infos.size()) {
      return false;
    }
    for (size_t i = 0; i < infos.size(); ++i) {
      if (i < texture_count) {
        if (infos[i].imageView != infos_[i].imageView ||
            infos[i].imageLayout != infos_[i].imageLayout) {
          return false;
        }
      } else if (infos[i].sampler != infos_[i].sampler) {
        return false;
      }
    }
    return true;
  }

  // Call only after the entire descriptor-update transaction succeeded. A
  // miss invalidates before allocating, so a partial VS/PS allocation failure
  // can never publish an unwritten descriptor set as reusable.
  void Commit(const Context& context, DescriptorSet set,
              std::span<const ImageInfo> infos, uint32_t texture_count) {
    Invalidate();
    if (set == DescriptorSet{} || infos.empty() || texture_count > infos.size()) {
      return;
    }
    infos_.assign(infos.begin(), infos.end());
    context_ = context;
    set_ = set;
    texture_count_ = texture_count;
    valid_ = true;
  }

 private:
  bool valid_ = false;
  Context context_{};
  DescriptorSet set_{};
  uint32_t texture_count_ = 0;
  std::vector<ImageInfo> infos_;
};

}  // namespace rex::graphics
