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

// Pose-derived wrist-angle source (WB4, wholebody_pose_design.md §2.2). An
// IWristAngleSource (wrist_assessment_contract.h) that feeds the Wrist assessment
// engine from the SMOOTHED (else raw) COCO-WholeBody pose track when NO IMU-derived
// source is available — the IMU-less lesson mode. It computes APPARENT camera-plane
// wrist angles for the LEAD wrist, NOT anatomical 3D DOFs (§2.2 honesty caveat):
//
//   apparentFlexExt = signed image-plane angle between the forearm vector
//                     (lead elbow → lead wrist) and the hand axis (wrist-root →
//                     middle-MCP).
//   apparentRadUln  = signed image-plane angle between the knuckle line
//                     (index-MCP → pinky-MCP) and the forearm normal.
//
// Because these are PROJECTED, not anatomical, each sample carries reduced
// confidence: min(endpoint conf) × a fixed 0.5 apparent-angle penalty factor
// (PoseWristAngleConfig::apparentPenalty). A left-handed golfer's swing is the
// left↔right image mirror of a right-handed one, so BOTH apparent angles are
// negated for handedness==Left (a whole-image mirror — distinct from the
// engine's per-DOF anatomical mirrorSign, which does NOT apply to camera-plane
// measurements) and the source then reports the canonical right-handed
// convention, exactly as InMemoryWristAngleSource / buildWristAngleSource do.
//
// SHIPS DARK: constructed ONLY when pose.wristAngles.enabled AND the swing has no
// IMU wrist source (wrist_analyzer.cpp). Off ⇒ the object is never built and the
// pipeline output is byte-identical. Qt-only (QPointF via swing_analysis.h),
// cv-free; unit-tested standalone.

#include <QVariantMap>

#include "swing_analysis.h"                // PoseTrack2D, PhaseEvent
#include "wrist_assessment_contract.h"     // InMemoryWristAngleSource / IWristAngleSource
#include "analysis_tuning.h"               // tuning::apply
#include "../Core/pp_tuned_constants.h"    // tuned::pose::wristAngles::

namespace pinpoint::analysis {

// Pose-wrist-angle knobs. Defaults track the frozen constants
// (pp_tuned_constants.h pose::wristAngles::); SwingLab sweeps "pose.wristAngles.*".
struct PoseWristAngleConfig {
    bool   enabled         = tuned::pose::wristAngles::kEnabled;         // pose.wristAngles.enabled
    double confMin         = tuned::pose::wristAngles::kConfMin;         // per-endpoint conf gate
    double apparentPenalty = tuned::pose::wristAngles::kApparentPenalty; // × min endpoint conf (0.5)
    // |apparent FE| above this is a detector failure, not a wrist — the frame is
    // REFUSED rather than clamped (see the constant). ≤ 0 disables the gate.
    double feLimitDeg      = tuned::pose::wristAngles::kFeLimitDeg;      // pose.wristAngles.feLimitDeg
    // Zero-phase low-pass cutoff for the σ estimate. ≤ 0 ⇒ no filter and NO σ.
    double fcHz            = tuned::pose::wristAngles::kFcHz;            // pose.wristAngles.fcHz
    // Emit the FILTERED curve rather than the raw one. Off; see the constant for what
    // evidence would justify turning it on, and why the corpus cannot supply it.
    bool   filterCurve     = tuned::pose::wristAngles::kFilterCurve;     // pose.wristAngles.filterCurve

    static PoseWristAngleConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        PoseWristAngleConfig c;
        apply(ov, "pose.wristAngles.enabled",         c.enabled);
        apply(ov, "pose.wristAngles.confMin",         c.confMin);
        apply(ov, "pose.wristAngles.apparentPenalty", c.apparentPenalty);
        apply(ov, "pose.wristAngles.feLimitDeg",      c.feLimitDeg);
        apply(ov, "pose.wristAngles.fcHz",            c.fcHz);
        apply(ov, "pose.wristAngles.filterCurve",     c.filterCurve);
        return c;
    }
};

// An IWristAngleSource whose LeadWristFlexExt / LeadWristRadUln series are the
// apparent camera-plane angles above, sampled once per pose frame (smoothed track
// preferred), with the P1–P8 timeline resolved from `phases` via the shared
// wristCheckpoints() map. Frames whose required endpoints fall below cfg.confMin —
// or whose FE exceeds cfg.feLimitDeg, which is the gate that actually fires — yield a
// sample with `available == false` (a gap the sampler bridges — never a fabricated
// value). `handedness` is the repo int convention (1 right / 2 left).
// frameW/frameH de-normalize the kp so the image-plane angles are isotropic.
//
// The FE limit is NOT applied to apparentRadUln. That angle is a different geometry
// (knuckle line against the forearm normal) whose corpus distribution has never been
// measured, and a limit chosen by analogy would be a guess wearing a number's clothes.
class PoseWristAngleSource : public InMemoryWristAngleSource {
public:
    PoseWristAngleSource(const PoseTrack2D &pose,
                         const std::vector<PhaseEvent> &phases,
                         int handedness, int frameW, int frameH,
                         const PoseWristAngleConfig &cfg = {});
};

// ── trailWristFlexExt ──────────────────────────────────────────────────────────────────────────
//
// The trail wrist's apparent bow / cup, as a plain MetricSeries. Same geometry as the lead-side
// `apparentFlexExt` above, same apparent-angle caveat, same confidence gate — and deliberately NOT
// routed through PoseWristAngleSource, because `PpJointDof` has no trail-side member and adding one
// would pull the assessment engine, the DOF metadata table and the reference bands into a change
// that produces exactly one curve. A metric is not a DOF.
//
// SIGN: POSITIVE IS EXTENSION (CUP), which is the OPPOSITE of the lead wrist's "+ = bowed". That is
// not an inconsistency, it is the two hands being mirror images. Face-on, both wrists are seen from
// the same side, so one signed image-plane angle means flexion on the lead hand and extension on
// the trail hand. It is also what the shipped corridors ask for: `m_trailWristFlexExt_p4` is seated
// at +45°, and 45° at the top is the trail wrist CUPPING, which every source describes as the
// normal backswing shape. The catalogue's howToRead was corrected to match rather than the other
// way round — the corridor is seated content, the sentence was boilerplate copied from the lead
// wrist. See docs/design/pinpoint_sign_conventions.md.
//
// NOT address-referenced. Every measure over it but the first is a `delta` anchored at P1, which
// does the referencing; a pre-subtracted series would make those deltas differences of differences.
//
// REQUIRES THE WHOLEBODY HAND KEYPOINTS. A legacy 17-keypoint track has no knuckles, so this
// returns empty rather than substituting the wrist joint for the hand axis — a hand axis measured
// from a point that is not on the hand is a confident number about nothing.
//
// CARRIES A σ, and the σ is a NOISE FLOOR ONLY. It is the out-of-band content — the robust scale of
// what a cfg.fcHz zero-phase low-pass would remove — so it answers "how much of this curve is the
// hand keypoints jittering" and nothing else. The larger error on this measure is the PROJECTION:
// graded against HackMotion on the lead hand (the only wrist a criterion instrument is ever worn
// on), the same geometry explains 31% of the criterion's variance under a fixed correction and 47%
// under one refitted per swing, with the fitted scale ranging −0.09 to −0.32 across swings of one
// golfer in one session. No σ can carry that, so a consumer must not read this field as a full
// error budget. MetricSeries::sigma is absent when cfg.fcHz ≤ 0 or the channel is too short to
// filter — absent means "not characterised", never "zero error".
//
// THE CURVE ITSELF IS NOT FILTERED. The low-pass exists here only to separate the noise for σ.
// Filtering the emitted curve moves 25% of the graded m_trailWristFlexExt_p1..p7 readings between
// bands while barely changing the grade distribution, and those corridors have a seating problem of
// their own (p6 fires Action on 66% of readings, p7 on 56%), so the curve must not move until that
// is resolved. Emitting the filtered vector instead is then a one-line change here.
std::vector<MetricSeries> buildTrailWristSeries(const PoseTrack2D &pose,
                                                const std::vector<PhaseEvent> &phases,
                                                int handedness, int frameW, int frameH,
                                                const PoseWristAngleConfig &cfg = {});

} // namespace pinpoint::analysis
