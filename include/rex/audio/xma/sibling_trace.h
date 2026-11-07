/**
 ******************************************************************************
 * ReXGlue - KI-071 XMA sibling-context trace and frame-exact fixes (pure)    *
 ******************************************************************************
 */

// KI-071 step 2 (doubled cut-scene dialogue at exactly 1-2 XMA frames). The
// cut-scene voice plays through several mono 44.1 kHz sibling contexts whose
// input buffers are contiguous slices of one streaming read; the measured
// 512/1024-sample delay means one sibling gains or loses whole frames. This
// header holds the pure parts (decisions, accounting, grouping, formatting) of
// the trace and the three cvar-gated fixes so the unit tests can cover them;
// xma_context.cpp / xma_decoder.cpp apply them.

#pragma once

#include <stdint.h>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>

// Trace: per-context frame/drop counters, a first-Decode record, one paced
// group line per sibling group per second and a per-minute MCLA_XMA_SIB line.
REXCVAR_DECLARE(bool, xma_sibling_trace);
// D2: at a buffer boundary whose next input buffer is not yet valid, keep the
// read offset and return (stall) instead of swapping and dropping the frame;
// clear the host-set error_status 4 after the next successful decode.
REXCVAR_DECLARE(bool, xma_stall_on_invalid_next);
// D1: apply dword1 bits 24-26 as a start subframe skip at the first decode
// after a Clear.
REXCVAR_DECLARE(bool, xma_honor_start_skip);
// Hygiene: re-create the FFmpeg decoder on Clear/Release (the xmaframes codec
// has no flush callback, so avcodec_flush_buffers keeps the overlap tail).
REXCVAR_DECLARE(bool, xma_reset_codec_on_clear);

namespace rex::audio::xma {

// ---------------------------------------------------------------------------
// D2: boundary decision.

// Where Decode meets the end of the data it can see.
enum class BoundaryPath : uint8_t {
  // Frame header split across the last packet (xma_context.cpp "Frame header
  // split across packet boundary"); upstream swaps and returns.
  kSplitHeader,
  // 0xFF full-skip packet with no later frame start in this buffer; upstream
  // swaps.
  kFullSkip,
  // Last frame of the packet continues into the next packet; upstream sets
  // error_status 4 and returns without advancing.
  kSpanningFrame,
};

enum class BoundaryAction : uint8_t {
  kContinue,   // next data is available: proceed as upstream
  kSwap,       // swap input buffers (upstream kSplitHeader / kFullSkip)
  kSetError4,  // upstream kSpanningFrame: error_status = 4, keep the offset
  kStall,      // fix: keep offset and buffer, end this Work, retry later
};

// A context that stalls this many consecutive Works without the next buffer
// becoming valid falls back to the upstream action (end of stream or a guest
// that refills only after the release; avoids a stuck voice).
inline constexpr uint32_t kMaxConsecutiveStalls = 4;

constexpr BoundaryAction DecideBoundary(BoundaryPath path, bool next_available, bool stall_fix,
                                        uint32_t consecutive_stalls) {
  if (next_available) {
    return path == BoundaryPath::kFullSkip ? BoundaryAction::kSwap : BoundaryAction::kContinue;
  }
  if (stall_fix && consecutive_stalls < kMaxConsecutiveStalls) {
    return BoundaryAction::kStall;
  }
  return path == BoundaryPath::kSpanningFrame ? BoundaryAction::kSetError4 : BoundaryAction::kSwap;
}

// error_status 4 is only ever written by the host decoder (the two kSetError4
// sites); upstream never clears it, so after one late refill every later Work
// stops after one frame. With the fix a successful decode clears it.
constexpr uint32_t ErrorStatusAfterDecode(uint32_t current, bool decoded, bool clear_fix) {
  return (clear_fix && decoded && current == 4) ? 0 : current;
}

// ---------------------------------------------------------------------------
// D1: start subframe skip.

// field = dword1 bits 24-26 (host name output_buffer_padding). The guest
// setter sub_823EDC80 writes it together with the read offset; sub_823FB960
// passes 0 for a fresh start and ((start_sample >> 7) & 3) for a seek start
// (mode byte +292 == 2, offset from sub_823FA988), i.e. the 128-sample
// subframe of the start sample inside its 512-sample frame. Returns the output
// blocks to discard from the first decoded frame: min(field, 4) subframes
// (4 = whole frame, as the loop skip), doubled for stereo, clamped to the
// blocks the frame has.
constexpr uint8_t StartSkipBlocks(uint32_t field, bool is_stereo, uint8_t frame_blocks) {
  const uint32_t subframes = std::min<uint32_t>(field & 7u, 4u);
  const uint32_t blocks = subframes << (is_stereo ? 1 : 0);
  return static_cast<uint8_t>(std::min<uint32_t>(blocks, frame_blocks));
}

// ---------------------------------------------------------------------------
// Sibling grouping.

// One input buffer of one context.
struct BufferSlice {
  uint32_t ctx = 0;
  uint32_t ptr = 0;      // physical address
  uint32_t packets = 0;  // 2 KiB packets
};

// Contexts whose input buffers are contiguous slices of one read: slice b
// starts where slice a ends (a.ptr + a.packets * 2048), transitively. Returns
// groups of at least two contexts, each sorted by context id, groups ordered
// by their first id. When `link` is given, a contiguous pair is joined only
// if link(ctx_a, ctx_b) is true.
inline std::vector<std::vector<uint32_t>> GroupContiguousSlices(
    std::vector<BufferSlice> slices, const std::function<bool(uint32_t, uint32_t)>& link = {}) {
  constexpr uint32_t kPacketBytes = 2048;
  std::erase_if(slices, [](const BufferSlice& s) { return s.ptr == 0 || s.packets == 0; });
  std::vector<uint32_t> ids;
  ids.reserve(slices.size());
  for (const BufferSlice& s : slices) {
    ids.push_back(s.ctx);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  auto index_of = [&ids](uint32_t ctx) {
    return size_t(std::lower_bound(ids.begin(), ids.end(), ctx) - ids.begin());
  };
  std::vector<size_t> parent(ids.size());
  for (size_t i = 0; i < parent.size(); ++i) {
    parent[i] = i;
  }
  auto find = [&parent](size_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  };
  std::sort(slices.begin(), slices.end(),
            [](const BufferSlice& a, const BufferSlice& b) { return a.ptr < b.ptr; });
  for (const BufferSlice& a : slices) {
    const uint64_t end = uint64_t(a.ptr) + uint64_t(a.packets) * kPacketBytes;
    auto it = std::lower_bound(slices.begin(), slices.end(), end,
                               [](const BufferSlice& s, uint64_t p) { return s.ptr < p; });
    for (; it != slices.end() && it->ptr == end; ++it) {
      if (it->ctx == a.ctx || (link && !link(a.ctx, it->ctx))) {
        continue;
      }
      const size_t ra = find(index_of(a.ctx));
      const size_t rb = find(index_of(it->ctx));
      if (ra != rb) {
        parent[std::max(ra, rb)] = std::min(ra, rb);
      }
    }
  }
  std::vector<std::vector<uint32_t>> by_root(ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    by_root[find(i)].push_back(ids[i]);
  }
  std::vector<std::vector<uint32_t>> groups;
  for (auto& g : by_root) {
    if (g.size() >= 2) {
      groups.push_back(std::move(g));
    }
  }
  return groups;
}

// Largest difference between any two values (max - min); 0 for fewer than 2.
inline uint64_t MaxPairwiseDelta(std::span<const uint64_t> values) {
  if (values.size() < 2) {
    return 0;
  }
  const auto [lo, hi] = std::minmax_element(values.begin(), values.end());
  return *hi - *lo;
}

// ---------------------------------------------------------------------------
// Counters and formatting.

// Per-context counters since the last Clear/Release.
struct SiblingCtxSample {
  uint32_t ctx = 0;
  uint64_t frames_parsed = 0;    // frames the read offset moved past (decoded or dropped)
  uint64_t frames_emitted = 0;   // frames decoded to 512 samples per channel
  uint64_t blocks_written = 0;   // 128-sample blocks written to the output ring
  uint64_t blocks_consumed = 0;  // blocks the guest has read (written - in ring at Work start)
  uint64_t blocks_skipped = 0;   // blocks discarded by the start skip (D1)
  uint64_t drops_split = 0;      // split-header swap with the next buffer invalid
  uint64_t drops_ff = 0;         // 0xFF-path swap with the next buffer invalid
  uint64_t err4 = 0;             // error_status = 4 set
  uint64_t decode_fail = 0;      // DecodePacket returned false (frame skipped)
  uint32_t start_skip = 0;       // dword1 bits 24-26 at the first decode
  uint32_t kick_group = 0;       // kick burst id of the last kick
  uint32_t start_kick = 0;       // kick burst id at the first decode since Clear
  uint32_t rate = 0;             // sample rate at the first decode (Hz)
  bool stereo = false;
  // Per-frame trim fields (frame header start/end skip), since Clear.
  uint64_t trim_start_frames = 0;   // frames with a non-zero start skip
  uint64_t trim_start_samples = 0;  // sum of start skips (samples per channel)
  uint64_t trim_end_frames = 0;
  uint64_t trim_end_samples = 0;
  uint32_t trim_first_count = 0;         // pairs in trim_first (<= 4)
  std::array<uint32_t, 4> trim_first{};  // (start << 16) | end of the first non-zero frames

  // Frames whose input was passed without output.
  uint64_t lost() const {
    return frames_parsed > frames_emitted ? frames_parsed - frames_emitted : 0;
  }
};

struct SiblingGroupDeltas {
  uint64_t frame_delta = 0;  // max - min frames_emitted
  uint64_t lost_delta = 0;   // max - min lost frames
};

inline SiblingGroupDeltas ComputeGroupDeltas(std::span<const SiblingCtxSample> group) {
  std::vector<uint64_t> emitted;
  std::vector<uint64_t> lost;
  for (const SiblingCtxSample& s : group) {
    emitted.push_back(s.frames_emitted);
    lost.push_back(s.lost());
  }
  return {MaxPairwiseDelta(emitted), MaxPairwiseDelta(lost)};
}

namespace detail {
template <typename F>
std::string JoinSamples(std::span<const SiblingCtxSample> group, char sep, F&& field) {
  std::string out;
  for (size_t i = 0; i < group.size(); ++i) {
    if (i) {
      out += sep;
    }
    out += fmt::format("{}", field(group[i]));
  }
  return out;
}
}  // namespace detail

// The first non-zero (start:end) trim pairs of one context, ';'-separated,
// "-" when none.
inline std::string FormatTrimFirst(const SiblingCtxSample& s) {
  if (s.trim_first_count == 0) {
    return "-";
  }
  std::string out;
  for (uint32_t i = 0; i < std::min<uint32_t>(s.trim_first_count, 4); ++i) {
    out += fmt::format("{}{}:{}", i ? ";" : "", s.trim_first[i] >> 16, s.trim_first[i] & 0xFFFF);
  }
  return out;
}

// One group line. Lists are in ctxs order; drop lists are per context,
// separated by '/'.
inline std::string FormatSiblingGroupLine(uint32_t group_id,
                                          std::span<const SiblingCtxSample> group) {
  using detail::JoinSamples;
  const SiblingGroupDeltas d = ComputeGroupDeltas(group);
  return fmt::format(
      "[xma] group: id={} ctxs=[{}] frames_emitted=[{}] frames_parsed=[{}] consumed_blocks=[{}] "
      "drops=[split={},ff={},err4={},fail={}] start_skip=[{}] skipped_blocks=[{}] kick=[{}] "
      "frame_delta={} lost_delta={} start_kick=[{}] trim_start=[{}] trim_end=[{}] trim_first=[{}]",
      group_id, JoinSamples(group, ',', [](const auto& s) { return s.ctx; }),
      JoinSamples(group, ',', [](const auto& s) { return s.frames_emitted; }),
      JoinSamples(group, ',', [](const auto& s) { return s.frames_parsed; }),
      JoinSamples(group, ',', [](const auto& s) { return s.blocks_consumed; }),
      JoinSamples(group, '/', [](const auto& s) { return s.drops_split; }),
      JoinSamples(group, '/', [](const auto& s) { return s.drops_ff; }),
      JoinSamples(group, '/', [](const auto& s) { return s.err4; }),
      JoinSamples(group, '/', [](const auto& s) { return s.decode_fail; }),
      JoinSamples(group, ',', [](const auto& s) { return s.start_skip; }),
      JoinSamples(group, ',', [](const auto& s) { return s.blocks_skipped; }),
      JoinSamples(group, ',', [](const auto& s) { return s.kick_group; }), d.frame_delta,
      d.lost_delta, JoinSamples(group, ',', [](const auto& s) { return s.start_kick; }),
      JoinSamples(group, ',',
                  [](const auto& s) {
                    return fmt::format("{}:{}", s.trim_start_frames, s.trim_start_samples);
                  }),
      JoinSamples(group, ',',
                  [](const auto& s) {
                    return fmt::format("{}:{}", s.trim_end_frames, s.trim_end_samples);
                  }),
      JoinSamples(group, '/', [](const auto& s) { return FormatTrimFirst(s); }));
}

// Event totals over one minute (all contexts).
struct SiblingMinuteTotals {
  uint64_t drops_split = 0;
  uint64_t drops_ff = 0;
  uint64_t err4 = 0;
  uint64_t decode_fail = 0;
  uint64_t stalls = 0;          // D2 stalls taken (fix on)
  uint64_t stall_timeouts = 0;  // stalls that fell back to the upstream action
  uint64_t err4_cleared = 0;    // error_status 4 cleared after a decode (fix on)
  uint64_t swapbacks = 0;       // swap-back: start packet has no frame start
  uint64_t no_bits = 0;         // "no bits to copy" swap
  uint64_t start_skips = 0;     // first decodes with a non-zero start-skip field
  uint64_t start_skips_applied = 0;
  uint64_t codec_resets = 0;  // decoder re-created on Clear/Release (fix on)
  uint64_t first_decodes = 0;
  // KI-071 step 3: per-frame trim fields and the first-decode burst budget.
  uint64_t trim_frames = 0;           // frames with a non-zero start or end skip
  uint64_t trim_samples = 0;          // sum of start + end skips (samples per channel)
  uint64_t trim_ctxs = 0;             // distinct contexts with a non-zero trim this minute
  uint64_t trim_applied_samples = 0;  // samples dropped by xma_honor_frame_trim
  uint64_t trim_unapplied = 0;        // trim samples not applied (end overflow, unaligned)
  uint64_t trim_parse_errors = 0;     // frames whose header could not be parsed
  uint64_t burst_first_decodes = 0;   // first-decode records logged on the burst budget
};

struct SiblingMinuteSummary {
  uint64_t groups = 0;  // distinct groups reported this minute
  uint64_t max_frame_delta = 0;
  uint64_t max_lost_delta = 0;
  SiblingMinuteTotals totals;
  bool stall_fix = false;
  bool skip_fix = false;
  bool reset_fix = false;
  bool trim_fix = false;
  bool trim_block = false;
};

inline std::string FormatSiblingMinuteLine(const SiblingMinuteSummary& m) {
  const SiblingMinuteTotals& t = m.totals;
  return fmt::format(
      "MCLA_XMA_SIB v=2 kind=minute groups={} max_frame_delta={} drops_split={} drops_ff={} "
      "err4={} decode_fail={} max_lost_delta={} stalls={} stall_timeouts={} err4_cleared={} "
      "swapbacks={} no_bits={} start_skips={} start_skips_applied={} codec_resets={} "
      "first_decodes={} stall_fix={} skip_fix={} reset_fix={} trim_frames={} trim_samples={} "
      "trim_ctxs={} trim_applied_samples={} trim_unapplied={} trim_parse_errors={} "
      "burst_first_decodes={} trim_fix={} trim_block={}",
      m.groups, m.max_frame_delta, t.drops_split, t.drops_ff, t.err4, t.decode_fail,
      m.max_lost_delta, t.stalls, t.stall_timeouts, t.err4_cleared, t.swapbacks, t.no_bits,
      t.start_skips, t.start_skips_applied, t.codec_resets, t.first_decodes, int(m.stall_fix),
      int(m.skip_fix), int(m.reset_fix), t.trim_frames, t.trim_samples, t.trim_ctxs,
      t.trim_applied_samples, t.trim_unapplied, t.trim_parse_errors, t.burst_first_decodes,
      int(m.trim_fix), int(m.trim_block));
}

// ---------------------------------------------------------------------------
// KI-071 step 3: sticky sibling groups.
//
// l74 showed the contiguous-buffer rule alone splits the 7 cut-scene voice
// contexts into a pair and a five (two reads started in the same kick burst)
// and re-forms subsets once buffers swap. Rule:
//  1. two contexts link when their buffers are contiguous AND they started in
//     the same kick burst (start_kick, the burst id at the first decode since
//     Clear) at the same rate and channel count;
//  2. linked components of two or more that share (start_kick, rate, stereo)
//     with start_kick != 0 merge (one scene start, several reads);
//  3. membership is sticky: a context stays in its group while it keeps the
//     same start_kick (no Clear), even after its buffers stop being
//     contiguous; the group id is a serial number given when the group forms
//     (the lower id wins when two groups merge), so it is stable over a scene.

struct GroupMember {
  uint32_t ctx = 0;
  uint32_t start_kick = 0;
  uint32_t rate = 0;
  bool stereo = false;
};

struct SiblingGroup {
  uint32_t id = 0;
  std::vector<uint32_t> ctxs;  // sorted
};

class StickySiblingGrouper {
 public:
  // members: contexts that have decoded since their Clear; slices: their
  // input buffers. Returns groups of two or more, ordered by id.
  std::vector<SiblingGroup> Update(std::span<const GroupMember> members,
                                   std::vector<BufferSlice> slices) {
    std::map<uint32_t, GroupMember> info;
    for (const GroupMember& m : members) {
      info[m.ctx] = m;
    }
    // Drop members that are gone or restarted.
    for (auto it = assign_.begin(); it != assign_.end();) {
      auto m = info.find(it->first);
      if (m == info.end() || m->second.start_kick != it->second.start_kick) {
        it = assign_.erase(it);
      } else {
        ++it;
      }
    }
    std::erase_if(slices, [&info](const BufferSlice& s) { return !info.contains(s.ctx); });
    auto same_start = [&info](uint32_t a, uint32_t b) {
      const GroupMember& x = info.at(a);
      const GroupMember& y = info.at(b);
      return x.start_kick == y.start_kick && x.rate == y.rate && x.stereo == y.stereo;
    };
    std::vector<std::vector<uint32_t>> components =
        GroupContiguousSlices(std::move(slices), same_start);
    // Merge the components of one start burst.
    std::map<std::array<uint32_t, 3>, std::vector<uint32_t>> merged;
    std::vector<std::vector<uint32_t>> result;
    for (auto& c : components) {
      const GroupMember& m = info.at(c.front());
      if (m.start_kick == 0) {
        result.push_back(std::move(c));
        continue;
      }
      auto& dst = merged[{m.start_kick, m.rate, uint32_t(m.stereo)}];
      dst.insert(dst.end(), c.begin(), c.end());
    }
    for (auto& entry : merged) {
      result.push_back(std::move(entry.second));
    }
    // Sticky ids.
    for (const auto& c : result) {
      uint32_t id = 0;
      for (uint32_t ctx : c) {
        auto a = assign_.find(ctx);
        if (a != assign_.end() && (id == 0 || a->second.id < id)) {
          id = a->second.id;
        }
      }
      if (id == 0) {
        id = ++next_id_;
      } else {
        // Two existing groups joined: move every member of the other id.
        for (uint32_t ctx : c) {
          auto a = assign_.find(ctx);
          if (a != assign_.end() && a->second.id != id) {
            const uint32_t old = a->second.id;
            for (auto& entry : assign_) {
              if (entry.second.id == old) {
                entry.second.id = id;
              }
            }
          }
        }
      }
      for (uint32_t ctx : c) {
        assign_[ctx] = {id, info.at(ctx).start_kick};
      }
    }
    std::map<uint32_t, std::vector<uint32_t>> by_id;
    for (const auto& entry : assign_) {
      by_id[entry.second.id].push_back(entry.first);
    }
    std::vector<SiblingGroup> groups;
    for (auto& entry : by_id) {
      if (entry.second.size() >= 2) {
        groups.push_back({entry.first, std::move(entry.second)});
      }
    }
    return groups;
  }

 private:
  struct Assignment {
    uint32_t id = 0;
    uint32_t start_kick = 0;
  };
  std::map<uint32_t, Assignment> assign_;
  uint32_t next_id_ = 0;
};

}  // namespace rex::audio::xma
