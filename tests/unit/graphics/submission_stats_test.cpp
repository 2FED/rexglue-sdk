#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include <rex/graphics/util/submission_stats.h>

using rex::graphics::CpLeadGauge;
using rex::graphics::FormatSubmissionStatsFields;
using rex::graphics::FrameEndSubmissionRing;
using rex::graphics::SubmissionEndReason;
using rex::graphics::SubmissionsInFlight;
using rex::graphics::SubmissionStats;

TEST_CASE("Submission stats: counts by end reason", "[graphics][submission_stats]") {
  SubmissionStats s;
  s.NoteEnd(SubmissionEndReason::kFrameEnd);
  s.NoteEnd(SubmissionEndReason::kFrameEnd);
  s.NoteEnd(SubmissionEndReason::kDrawFull);
  s.NoteEnd(SubmissionEndReason::kIdle);
  s.NoteEnd(SubmissionEndReason::kFenceEarly);
  s.NoteEnd(SubmissionEndReason::kForcedWait);
  s.NoteEnd(SubmissionEndReason::kPrimaryEnd);
  s.NoteEnd(SubmissionEndReason::kPrimaryEnd);
  s.NoteEnd(SubmissionEndReason::kOther);
  // Out of range folds into other.
  s.NoteEnd(SubmissionEndReason::kCount);
  CHECK(s.total() == 10);
  CHECK(s.count(SubmissionEndReason::kFrameEnd) == 2);
  CHECK(s.count(SubmissionEndReason::kPrimaryEnd) == 2);
  CHECK(s.count(SubmissionEndReason::kOther) == 2);
}

TEST_CASE("Submission stats: submissions and frames in flight", "[graphics][submission_stats]") {
  // submission_current is the next to be signalled.
  CHECK(SubmissionsInFlight(1, 0) == 0);
  CHECK(SubmissionsInFlight(5, 2) == 2);
  CHECK(SubmissionsInFlight(5, 4) == 0);
  CHECK(SubmissionsInFlight(5, 9) == 0);
  CHECK(SubmissionsInFlight(0, 0) == 0);

  FrameEndSubmissionRing<4> ring;
  CHECK(ring.InFlight(0) == 0);
  ring.Push(3);
  ring.Push(7);
  ring.Push(12);
  CHECK(ring.InFlight(2) == 3);
  CHECK(ring.InFlight(7) == 1);
  CHECK(ring.InFlight(12) == 0);
  ring.Push(15);
  ring.Push(18);  // overwrites 3
  CHECK(ring.InFlight(2) == 4);
  CHECK(ring.InFlight(14) == 2);
}

TEST_CASE("Submission stats: CP lead gauge and line fields", "[graphics][submission_stats]") {
  SubmissionStats s;
  CHECK(FormatSubmissionStatsFields(s) ==
        " subm=0 subm_frame_end=0 subm_draw_full=0 subm_idle=0 subm_fence_early=0 "
        "subm_forced_wait=0 subm_primary_end=0 subm_other=0 cp_lead_samples=0 "
        "cp_lead_subs_min=0 cp_lead_subs_avg=0.00 cp_lead_subs_max=0 cp_lead_frames_min=0 "
        "cp_lead_frames_avg=0.00 cp_lead_frames_max=0 cp_lead_subs_now=0 cp_lead_frames_now=0");
  s.lead.Sample(4, 2);
  s.lead.Sample(9, 3);
  s.lead.Sample(2, 1);
  s.NoteEnd(SubmissionEndReason::kFrameEnd);
  s.NoteEnd(SubmissionEndReason::kIdle);
  const CpLeadGauge& g = s.lead;
  CHECK(g.samples == 3);
  CHECK(g.subs_min == 2);
  CHECK(g.subs_max == 9);
  CHECK(g.frames_min == 1);
  CHECK(g.frames_max == 3);
  CHECK(g.subs_now == 2);
  CHECK(g.frames_now == 1);
  CHECK(FormatSubmissionStatsFields(s) ==
        " subm=2 subm_frame_end=1 subm_draw_full=0 subm_idle=1 subm_fence_early=0 "
        "subm_forced_wait=0 subm_primary_end=0 subm_other=0 cp_lead_samples=3 "
        "cp_lead_subs_min=2 cp_lead_subs_avg=5.00 cp_lead_subs_max=9 cp_lead_frames_min=1 "
        "cp_lead_frames_avg=2.00 cp_lead_frames_max=3 cp_lead_subs_now=2 cp_lead_frames_now=1");
}
