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

    static PoseWristAngleConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        PoseWristAngleConfig c;
        apply(ov, "pose.wristAngles.enabled",         c.enabled);
        apply(ov, "pose.wristAngles.confMin",         c.confMin);
        apply(ov, "pose.wristAngles.apparentPenalty", c.apparentPenalty);
        return c;
    }
};

// An IWristAngleSource whose LeadWristFlexExt / LeadWristRadUln series are the
// apparent camera-plane angles above, sampled once per pose frame (smoothed track
// preferred), with the P1–P8 timeline resolved from `phases` via the shared
// wristCheckpoints() map. Frames whose required endpoints fall below cfg.confMin
// yield a sample with `available == false` (a gap the sampler bridges — never a
// fabricated value). `handedness` is the repo int convention (1 right / 2 left).
// frameW/frameH de-normalize the kp so the image-plane angles are isotropic.
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
std::vector<MetricSeries> buildTrailWristSeries(const PoseTrack2D &pose,
                                                const std::vector<PhaseEvent> &phases,
                                                int handedness, int frameW, int frameH,
                                                const PoseWristAngleConfig &cfg = {});

} // namespace pinpoint::analysis
