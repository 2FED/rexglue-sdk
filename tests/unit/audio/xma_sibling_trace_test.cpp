#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <rex/audio/xma/sibling_trace.h>

using rex::audio::xma::BoundaryAction;
using rex::audio::xma::BoundaryPath;
using rex::audio::xma::BufferSlice;
using rex::audio::xma::ComputeGroupDeltas;
using rex::audio::xma::DecideBoundary;
using rex::audio::xma::ErrorStatusAfterDecode;
using rex::audio::xma::FormatSiblingGroupLine;
using rex::audio::xma::FormatSiblingMinuteLine;
using rex::audio::xma::GroupContiguousSlices;
using rex::audio::xma::kMaxConsecutiveStalls;
using rex::audio::xma::MaxPairwiseDelta;
using rex::audio::xma::SiblingCtxSample;
using rex::audio::xma::SiblingMinuteSummary;
using rex::audio::xma::StartSkipBlocks;

TEST_CASE("XMA sibling boundary: upstream actions with the stall fix off", "[audio][xma][ki071]") {
  CHECK(DecideBoundary(BoundaryPath::kSplitHeader, true, false, 0) == BoundaryAction::kContinue);
  CHECK(DecideBoundary(BoundaryPath::kSpanningFrame, true, false, 0) == BoundaryAction::kContinue);
  CHECK(DecideBoundary(BoundaryPath::kFullSkip, true, false, 0) == BoundaryAction::kSwap);
  CHECK(DecideBoundary(BoundaryPath::kSplitHeader, false, false, 0) == BoundaryAction::kSwap);
  CHECK(DecideBoundary(BoundaryPath::kFullSkip, false, false, 0) == BoundaryAction::kSwap);
  CHECK(DecideBoundary(BoundaryPath::kSpanningFrame, false, false, 0) ==
        BoundaryAction::kSetError4);
}

TEST_CASE("XMA sibling boundary: stall while the next buffer is invalid, bounded",
          "[audio][xma][ki071]") {
  for (BoundaryPath path :
       {BoundaryPath::kSplitHeader, BoundaryPath::kFullSkip, BoundaryPath::kSpanningFrame}) {
    for (uint32_t stalls = 0; stalls < kMaxConsecutiveStalls; ++stalls) {
      CHECK(DecideBoundary(path, false, true, stalls) == BoundaryAction::kStall);
    }
  }
  // Valid next data is never stalled.
  CHECK(DecideBoundary(BoundaryPath::kSplitHeader, true, true, 0) == BoundaryAction::kContinue);
  CHECK(DecideBoundary(BoundaryPath::kFullSkip, true, true, 0) == BoundaryAction::kSwap);
  // After the limit the upstream action is taken (end of stream, late guest).
  CHECK(DecideBoundary(BoundaryPath::kSplitHeader, false, true, kMaxConsecutiveStalls) ==
        BoundaryAction::kSwap);
  CHECK(DecideBoundary(BoundaryPath::kSpanningFrame, false, true, kMaxConsecutiveStalls) ==
        BoundaryAction::kSetError4);
}

TEST_CASE("XMA sibling: error_status 4 cleared only by the fix after a decode",
          "[audio][xma][ki071]") {
  CHECK(ErrorStatusAfterDecode(4, true, false) == 4);
  CHECK(ErrorStatusAfterDecode(4, true, true) == 0);
  CHECK(ErrorStatusAfterDecode(4, false, true) == 4);
  CHECK(ErrorStatusAfterDecode(0, true, true) == 0);
  CHECK(ErrorStatusAfterDecode(7, true, true) == 7);
}

TEST_CASE("XMA sibling: start subframe skip accounting", "[audio][xma][ki071]") {
  // Mono frame = 4 blocks of 128 samples; field = (start_sample >> 7) & 3.
  CHECK(StartSkipBlocks(0, false, 4) == 0);
  CHECK(StartSkipBlocks(1, false, 4) == 1);
  CHECK(StartSkipBlocks(3, false, 4) == 3);
  // 4 and above = whole frame (warm-up), as the loop skip.
  CHECK(StartSkipBlocks(4, false, 4) == 4);
  CHECK(StartSkipBlocks(7, false, 4) == 4);
  // Stereo blocks interleave two channels.
  CHECK(StartSkipBlocks(2, true, 8) == 4);
  CHECK(StartSkipBlocks(4, true, 8) == 8);
  // Never more than the frame has left (loop skip already applied).
  CHECK(StartSkipBlocks(3, false, 2) == 2);
}

TEST_CASE("XMA sibling grouping: contiguous slices of one read", "[audio][xma][ki071]") {
  // The l73 cut-scene group: 67 (19 pk) 70 (10) 38 (3) 72 (11) 69 (19),
  // contiguous from 03F5F800; plus an unrelated context and a radio pair.
  std::vector<BufferSlice> slices = {
      {69, 0x03F6F800 + 0x5800, 19}, {67, 0x03F5F800, 19}, {70, 0x03F69000, 10},
      {38, 0x03F6E000, 3},           {72, 0x03F6F800, 11}, {5, 0x01000000, 4},
      {0, 0x03F18000, 31},           {1, 0x03F27800, 31},  {3, 0, 0},
  };
  const auto groups = GroupContiguousSlices(slices);
  REQUIRE(groups.size() == 2);
  CHECK(groups[0] == std::vector<uint32_t>{0, 1});
  CHECK(groups[1] == std::vector<uint32_t>{38, 67, 69, 70, 72});
}

TEST_CASE("XMA sibling grouping: both buffers link, self links ignored", "[audio][xma][ki071]") {
  std::vector<BufferSlice> slices = {
      // ctx 10 buffer 0 followed by its own buffer 1: not a sibling.
      {10, 0x1000000, 2},
      {10, 0x1001000, 2},
      // ctx 11 follows ctx 10's buffer 1 (second read).
      {11, 0x1002000, 2},
  };
  const auto groups = GroupContiguousSlices(slices);
  REQUIRE(groups.size() == 1);
  CHECK(groups[0] == std::vector<uint32_t>{10, 11});
  CHECK(GroupContiguousSlices({{10, 0x1000000, 2}, {10, 0x1001000, 2}}).empty());
}

TEST_CASE("XMA sibling: group frame deltas", "[audio][xma][ki071]") {
  CHECK(MaxPairwiseDelta(std::vector<uint64_t>{}) == 0);
  CHECK(MaxPairwiseDelta(std::vector<uint64_t>{5}) == 0);
  CHECK(MaxPairwiseDelta(std::vector<uint64_t>{100, 99, 101, 100}) == 2);

  std::vector<SiblingCtxSample> group(3);
  group[0].frames_parsed = 100;
  group[0].frames_emitted = 100;
  group[1].frames_parsed = 101;  // one split-header drop
  group[1].frames_emitted = 100;
  group[1].drops_split = 1;
  group[2].frames_parsed = 99;
  group[2].frames_emitted = 99;
  const auto d = ComputeGroupDeltas(group);
  CHECK(d.frame_delta == 1);
  CHECK(d.lost_delta == 1);
  CHECK(group[1].lost() == 1);
}

TEST_CASE("XMA sibling: line formatting", "[audio][xma][ki071]") {
  std::vector<SiblingCtxSample> group(2);
  group[0].ctx = 67;
  group[0].frames_parsed = 10;
  group[0].frames_emitted = 10;
  group[0].blocks_consumed = 36;
  group[0].kick_group = 7;
  group[1].ctx = 70;
  group[1].frames_parsed = 11;
  group[1].frames_emitted = 10;
  group[1].blocks_consumed = 36;
  group[1].drops_split = 1;
  group[1].start_skip = 2;
  group[1].blocks_skipped = 2;
  group[1].kick_group = 7;
  CHECK(FormatSiblingGroupLine(67, group) ==
        "[xma] group: id=67 ctxs=[67,70] frames_emitted=[10,10] frames_parsed=[10,11] "
        "consumed_blocks=[36,36] drops=[split=0/1,ff=0/0,err4=0/0,fail=0/0] start_skip=[0,2] "
        "skipped_blocks=[0,2] kick=[7,7] frame_delta=0 lost_delta=1");

  SiblingMinuteSummary m;
  m.groups = 3;
  m.max_frame_delta = 2;
  m.max_lost_delta = 1;
  m.totals.drops_split = 4;
  m.totals.drops_ff = 1;
  m.totals.err4 = 2;
  m.totals.decode_fail = 0;
  m.totals.stalls = 5;
  m.stall_fix = true;
  CHECK(FormatSiblingMinuteLine(m) ==
        "MCLA_XMA_SIB v=1 kind=minute groups=3 max_frame_delta=2 drops_split=4 drops_ff=1 "
        "err4=2 decode_fail=0 max_lost_delta=1 stalls=5 stall_timeouts=0 err4_cleared=0 "
        "swapbacks=0 no_bits=0 start_skips=0 start_skips_applied=0 codec_resets=0 "
        "first_decodes=0 stall_fix=1 skip_fix=0 reset_fix=0");
}
