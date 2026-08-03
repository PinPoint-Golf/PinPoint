/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "metric_provider.h"

// Capability declarations for the live MetricSeries producers (design §6). None of these compute
// anything — production stays in the analysis stages.
//
// EACH OF THESE IS NOW A CLAIM LIST AND NOTHING ELSE. A provider says "I produce these keys"; HOW
// well, on a given shot, is read off each metric's route ladder in the manifest by the seam's
// default `availability()` (metric_provider.h). Before the ladder existed, every class here also
// rebuilt its metrics' requirements in C++ — a second copy of what the descriptor already said,
// which drifted: `stanceWidthMm` was produced on every ruler-resolved swing and claimed by nobody,
// and `leadHeelLift` was claimed while understating what it needed, so each lied in the opposite
// direction. The requirement now has exactly one home.
//
// Adding an `availability()` override below is therefore a design decision, not a routine step:
// it means the ladder cannot express something, and the first question is whether a route is
// missing from the manifest.
//
// NO PROVIDER LOOKS AT `ShotContext::sessionType`, and none may. Availability answers one question —
// can this shot's data and devices support this metric — and a session type is neither: it is what
// the operator meant to record, not evidence about what was recorded. The gates here therefore
// mirror the stages' own canRun() conditions exactly (a face-on camera, a club track, a ball track,
// a bound IMU role) and nothing else. The field survives on ShotContext because swing.json carries
// it and the resolver's callers pass it through, but reading it here is a defect: an earlier
// `wristSessionOk()` gate made half the catalogue answer "produced in Wrist Motion sessions only",
// which the golfer reads as a statement about their equipment. See appendBodyMetricStages() in
// wrist_analyzer.cpp, which removed the matching gate on the production side.

namespace pinpoint::analysis {

// metric_extractor.cpp. leadWristFlexExt / leadWristRadUln need LeadForearm + LeadHand;
// forearmPronation / leadArmFlexion additionally need LeadUpperArm.
class WristMetricProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// kinematic_series.cpp — runs in both the wrist and camera-kinematics profiles (any session).
// clubheadSpeed / handSpeed need the club track; lagAngle additionally needs face-on pose.
class KinematicSeriesProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// foot_metrics.cpp + ball_position.cpp. All need face-on whole-body pose (foot keypoints);
// ballPosition additionally needs the ball track, so this provider is NOT key-agnostic.
class FootMetricProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// lower_body_metrics.cpp — leadKneeDrift / pelvisSway / pelvisLift / hipLineTilt / feetAlignment /
// comOverLeadFoot are frontal-plane readings off the COCO body hips, knees and ankles. Deliberately
// a separate provider from the feet: these keypoints exist in BOTH pose layouts, so this answers on
// tracks where FootMetricProvider cannot.
class LowerBodyMetricProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// upper_body_metrics.cpp — the chest, shoulder and arm frontal-plane channels. Same input as the
// lower body (face-on pose, COCO body only), and separate for the same reason: different keypoints,
// different failure.
class UpperBodyMetricProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// pose_wrist_angle_source.cpp buildTrailWristSeries — trailWristFlexExt from the face-on pose.
//
// A REQUIREMENT THIS SEAM CANNOT EXPRESS: it needs the WholeBody hand keypoints, and MetricRequirement
// has no pose-layout field. Declaring one for a single metric would put a field on every descriptor
// to serve one; the producer refuses a 17-keypoint track outright instead, so the failure is honest
// at analysis time even though the capability answer here is optimistic. Stated rather than hidden.
class TrailWristProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// body_rotation.cpp — the producer that made Bridged necessary, and the worked example of a ladder.
//
// A bound Pelvis / Thorax IMU measures axial turn directly; with only a face-on camera the SAME
// producer estimates it from the collapse of the hip or shoulder span in the image — a real reading
// by a weaker method. That used to be an if/else here. It is now two routes on each of the four
// descriptors, which is what lets the directory show the ladder instead of only its outcome, and
// what makes xFactor's "resolves at the weaker of the two segments" fall out of its routes needing
// both IMUs rather than out of a hand-written per-key condition.
class BodyRotationProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// club_delivery.cpp — shaftAngleVsHorizontal / attackAngle from the measured clubhead;
// lowPointAhead additionally needs the ball (the reference it is stated against) and its diameter
// ruler (the unit it is stated in).
class ClubDeliveryProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// head_track.cpp — headSway / headLift / headTilt need face-on pose.
class HeadMetricProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// wrist_analyzer.cpp ShaftLeanStage — impactShaftLean needs the club track.
class ShaftLeanProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// tempo_metrics.cpp — needs NO devices beyond whatever produced a confident phase ladder: an
// IMU-only swing and a camera-only swing both qualify, which no single requirement can express. Both
// metrics therefore carry one `Derived` route with an empty requirement, and the claim it makes is
// "the pipeline can produce this", not "this shot's ladder was good enough" — the producer refuses
// an unreliable ladder at analysis time (see tempo_metrics.h).
class TempoProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// Summary scores (produced as ScoreBreakdown, not MetricSeries — hence keyed synthetically):
//   wristScore / wristResemblance — LIVE (WristResemblanceScorer + WristAssessmentEngine,
//     wrist_analyzer.cpp); need the lead forearm + hand IMUs.
//   swingScore — the Swing/GRF/Coach adherence score. SwingScorer is dead code and
//     CameraKinematicsAnalyzer returns a stub, so its only route is planned and it always resolves
//     Unavailable. That used to be an `if (key == "swingScore")` here, saying so in a sentence the
//     directory could not see; it is now the route's own summary.
class ScoreProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

// There is no PlannedMetricProvider any more, and reinstating one would be a step backwards. It
// claimed every `.planned` descriptor and answered one fixed sentence, which made it a second list
// of what is unbuilt that had to be kept in step with the first — and drifted, silently, because a
// new placeholder that nobody added to it fell through to "no producer available" (the reason an
// UNKNOWN key gets) rather than to "planned". A metric whose every route is planned now says so
// from its own ladder, in that route's words about why.

// The launch-monitor metrics: face angle and everything downstream of it, spin, strike location and
// carry. Deliberately NOT a planned placeholder — a planned metric has no producer and is a promise;
// these have a producer that the golfer may or may not own, so the honest answer is the ordinary
// requirement one ("needs a launch monitor"), which flips to Measured the moment a connector sets
// `ShotContext::hasLaunchMonitor`. That makes this class the integration's insertion point rather
// than a stub somebody will have to delete.
class LaunchMonitorProvider : public IMetricProvider {
public:
    std::vector<QString> provides() const override;
};

} // namespace pinpoint::analysis
