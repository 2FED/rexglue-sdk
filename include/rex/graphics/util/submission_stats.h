#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include <fmt/format.h>

namespace rex::graphics {

// l78 instrumentation (M11-007e analysis): host submissions counted by the
// reason they were ended, and a command-processor lead gauge sampled at every
// frame end. Appended to the always-on MCLA readback minute/mark line.
//
// Reasons (the D3D12 call sites):
// - frame_end: EndSubmission at the swap;
// - draw_full: d3d12_readback_sync_kick_draws executed the draws recorded so
//   far early (the open submission reached the draw threshold);
// - idle: a deferred fence write ended the open submission while the
//   command processor was idle or polling a waiting WAIT_REG_MEM;
// - fence_early: a deferred fence write ended the open submission at a draw
//   boundary because the GPU had run out of work;
// - forced_wait: a synchronous wait on the open submission (targeted
//   read-back wait, CheckSubmissionFence: forced read-back waits, queue
//   drains, reclamation);
// - primary_end: d3d12_submit_on_primary_buffer_end (the PM4 primary buffer
//   ended, i.e. the command processor caught up with the ring);
// - other: anything else (read-back ensure request, host occlusion query
//   disable, unlabelled call sites).
enum class SubmissionEndReason : uint8_t {
  kFrameEnd,
  kDrawFull,
  kIdle,
  kFenceEarly,
  kForcedWait,
  kPrimaryEnd,
  kOther,
  kCount,
};

inline constexpr size_t kSubmissionEndReasonCount = size_t(SubmissionEndReason::kCount);

// Command-processor lead at a frame end. subs: submissions ended but not yet
// completed by the GPU. frames: frames whose closing submission the GPU has
// not completed (the frame just closed included; at most the ring size).
struct CpLeadGauge {
  uint64_t samples = 0;
  uint64_t subs_min = 0;
  uint64_t subs_max = 0;
  uint64_t subs_sum = 0;
  uint64_t frames_min = 0;
  uint64_t frames_max = 0;
  uint64_t frames_sum = 0;
  uint64_t subs_now = 0;
  uint64_t frames_now = 0;

  void Sample(uint64_t subs, uint64_t frames) {
    if (!samples) {
      subs_min = subs_max = subs;
      frames_min = frames_max = frames;
    } else {
      subs_min = std::min(subs_min, subs);
      subs_max = std::max(subs_max, subs);
      frames_min = std::min(frames_min, frames);
      frames_max = std::max(frames_max, frames);
    }
    ++samples;
    subs_sum += subs;
    frames_sum += frames;
    subs_now = subs;
    frames_now = frames;
  }
};

struct SubmissionStats {
  std::array<uint64_t, kSubmissionEndReasonCount> by_reason = {};
  CpLeadGauge lead;

  void NoteEnd(SubmissionEndReason reason) {
    const size_t index = size_t(reason);
    ++by_reason[index < kSubmissionEndReasonCount ? index : size_t(SubmissionEndReason::kOther)];
  }
  uint64_t count(SubmissionEndReason reason) const { return by_reason[size_t(reason)]; }
  uint64_t total() const {
    uint64_t sum = 0;
    for (uint64_t v : by_reason) {
      sum += v;
    }
    return sum;
  }
};

// Submissions ended (signalled: below submission_current) and not completed.
inline uint64_t SubmissionsInFlight(uint64_t submission_current, uint64_t completed) {
  const uint64_t ended = submission_current ? submission_current - 1 : 0;
  return ended > completed ? ended - completed : 0;
}

// The closing submissions of the last N frames; InFlight counts the frames
// whose closing submission is above `completed`.
template <size_t N>
class FrameEndSubmissionRing {
 public:
  void Push(uint64_t submission) {
    submissions_[next_ % N] = submission;
    ++next_;
  }
  uint64_t InFlight(uint64_t completed) const {
    const size_t count = next_ < N ? size_t(next_) : N;
    uint64_t in_flight = 0;
    for (size_t i = 0; i < count; ++i) {
      in_flight += submissions_[i] > completed ? 1 : 0;
    }
    return in_flight;
  }

 private:
  std::array<uint64_t, N> submissions_ = {};
  uint64_t next_ = 0;
};

inline constexpr size_t kCpLeadFrameRing = 8;

inline std::string FormatSubmissionStatsFields(const SubmissionStats& s) {
  const CpLeadGauge& g = s.lead;
  const double n = double(std::max<uint64_t>(g.samples, 1));
  return fmt::format(
      " subm={} subm_frame_end={} subm_draw_full={} subm_idle={} subm_fence_early={} "
      "subm_forced_wait={} subm_primary_end={} subm_other={} cp_lead_samples={} "
      "cp_lead_subs_min={} cp_lead_subs_avg={:.2f} cp_lead_subs_max={} cp_lead_frames_min={} "
      "cp_lead_frames_avg={:.2f} cp_lead_frames_max={} cp_lead_subs_now={} cp_lead_frames_now={}",
      s.total(), s.count(SubmissionEndReason::kFrameEnd), s.count(SubmissionEndReason::kDrawFull),
      s.count(SubmissionEndReason::kIdle), s.count(SubmissionEndReason::kFenceEarly),
      s.count(SubmissionEndReason::kForcedWait), s.count(SubmissionEndReason::kPrimaryEnd),
      s.count(SubmissionEndReason::kOther), g.samples, g.subs_min, double(g.subs_sum) / n,
      g.subs_max, g.frames_min, double(g.frames_sum) / n, g.frames_max, g.subs_now, g.frames_now);
}

}  // namespace rex::graphics
