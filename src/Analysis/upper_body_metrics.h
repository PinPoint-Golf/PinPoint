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

// Upper-body FRONTAL-PLANE metrics — the chest, shoulders and arms counterpart to
// lower_body_metrics.{h,cpp}. See docs/design/upper_body_face_on_metrics.md.
//
// THE RULE THAT DECIDES WHAT IS IN HERE is the one lower_body_metrics states: a
// face-on camera resolves the frontal plane (image x and y) and NOTHING in depth.
// Every channel below is a frontal-plane displacement, a frontal-plane angle, or a
// frontal-plane distance between two resolved points. That is also why the
// neighbouring planned metrics stay planned rather than being quietly added here —
// `spineForwardBend` is a sagittal hinge the frontal projection foreshortens to
// nothing, and `thoracicFlexion` / `lumbarExtension` need a keypoint between the
// shoulders and the hips that NEITHER pose layout carries.
//
// ── Built on the anatomy vocabulary, not on keypoint indices ────────────────
//
// Unlike lower_body_metrics, which predates it and hardcodes `kLHip = 11`, every
// point and segment here resolves through `Diagnostics/anatomy_vocabulary.h`:
// `resolvePoint(AnatomyRole::ThoraxCentre, …)`, `resolveSegment(AnatomyRole::
// ShoulderLine, …)`. Three things follow, and all three are the reason:
//
//   * The DIAGNOSTICS CONTENT ALREADY NAMES THIS GEOMETRY. The composed measures in
//     core.json carry a `series` facet triple — `m_shoulderPlane` is
//     `shoulderLine · angle · ground`, `m_thoraxDrift` is `thoraxCentre · distance ·
//     trailAnkle`, `m_trailElbowRise` is `trailElbow · height · shoulderLine`. Those
//     ARE the producer specification, authored before the producer. Building to the
//     same vocabulary is what makes the join exact instead of coincidental.
//   * Handedness and layout resolve once, correctly, in one place. A role is lead or
//     trail, never left or right, and a role missing from a 17-keypoint track reports
//     WHY rather than silently reading a neighbouring index.
//   * The overlay and the measurement agree by construction — the vocabulary is
//     explicitly intended as the single source of truth for the skeleton overlay too.
//
// The resolver works in whatever units the caller's keypoints are in, so each frame
// is de-normalized to PIXELS once into a scratch array and the vocabulary is asked
// against that. Pixels are isotropic here: kp arrive normalized by width and height
// SEPARATELY, so every geometric quantity must be computed after de-normalizing by
// both or an angle is a lie about the aspect ratio.
//
// ── Sign, for every channel ─────────────────────────────────────────────────
//
// docs/design/pinpoint_sign_conventions.md rule 2: lateral channels are
// LEAD-POSITIVE, and which image direction that is gets RESOLVED from the address
// geometry (which ankle is further along +x) rather than assumed. A camera may be
// mirrored and an operator may flip the preview; a convention that depends on
// neither happening is not a convention.
//
// The two body-line tilts (`shoulderPlaneAngle`, and `hipLineTilt` in the lower-body
// module) share ONE convention: POSITIVE MEANS THE TRAIL END SITS ABOVE THE LEAD END,
// computed against the ABSOLUTE horizontal separation so the answer does not flip for
// a left-handed golfer or a mirrored camera. A raw atan2 of the lead→trail vector
// does flip, which is why `toeLineAngle` — which uses one — is NOT the model here and
// why `feetAlignment` was added as its sign-stable replacement.
//
// `secondaryAxisTilt` is POSITIVE AWAY FROM THE TARGET (trail-side lean), matching
// its descriptor and `m_axisTiltImpact`'s `highMeans`. It is therefore the one
// lateral channel that is deliberately TRAIL-positive: the quantity is named for the
// lean away from the target, and inverting it to satisfy rule 2 would leave every
// coach-facing sentence about it backwards.
//
// ── Units ───────────────────────────────────────────────────────────────────
//
// Angles are degrees. Distances are a PERCENTAGE OF AN ADDRESS BODY SPAN, never
// centimetres, for the reason lower_body_metrics gives at length: a ruler needs a
// detected ball, and a metric present in cm on some swings and absent on others is
// worse than one that is always present and body-relative. Which span is the
// denominator is chosen to match the content's authored unit string exactly:
//
//   % shoulder width  — trailElbowHeight, leadUpperArmToChest   (address shoulder span)
//   % stance width    — thoraxLateralDrift                      (address ankle span)
//   % arm length      — leadHandWidth                           (address lead arm, shoulder→elbow→wrist)
//
// A denominator is measured over the SAME address reference frames as its numerators,
// so the ratio shares its scale by construction rather than by luck — the reason
// foot_metrics captures shoulder width on the stance-width frames.
//
// ── ⚠ THE SHOULDER GATE IS NOT A RARE EVENT ─────────────────────────────────
//
// Read `upperBody.minShoulderSpanRatio` (0.40) with this number in front of you: on the
// 108-swing corpus the shoulder span ratio AT THE TOP has a median of 0.32, and 55 of 83
// measured swings sit below the gate there. So on most swings `shoulderPlaneAngle`,
// `spineSideBend` and `trailElbowHeight` have NO reading at P4 at all — not a worse one, none.
// That is the honest consequence of the geometry (a golfer who has turned 90° has no shoulder
// line in a face-on image), and it is a deliberate product decision rather than an accident:
// the alternative was continuing to grade +88° as a posture. Anyone tempted to treat the gate
// as an edge case, or to wonder why a Top corridor stopped firing, should start here.

#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // PoseTrack2D, PoseFrame2D, MetricSeries, PhaseEvent
#include "metric_channel.h"          // MetricChannel, buildChannelSeries
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::upperBody::

namespace pinpoint::analysis {

// Upper-body knobs. Defaults track the frozen constants (pp_tuned_constants.h
// upperBody::); SwingLab sweeps them via "upperBody.*" dotted keys.
struct UpperBodyConfig {
    double  confMin           = tuned::upperBody::kConfMin;           // upperBody.confMin
    int     addrMinFrames     = tuned::upperBody::kAddrMinFrames;     // upperBody.addrMinFrames
    int64_t addrWindowUs      = tuned::upperBody::kAddrWindowUs;      // upperBody.addrWindowUs
    double  minShoulderSpanPx = tuned::upperBody::kMinShoulderSpanPx; // upperBody.minShoulderSpanPx
    // The body LINES' foreshortening gates — |dx| now over |dx| at address. A different
    // failure from minShoulderSpanPx, which guards a percentage denominator: these guard
    // ANGLES whose divisor is the live horizontal separation, and which run to ±90° as the
    // turn collapses two joints into one image column (+88° of "shoulder plane" AT THE TOP).
    // Gates shoulderPlaneAngle, the shoulder half of spineSideBend, and trailElbowHeight —
    // which is a height rather than a tilt but interpolates the shoulder line's y at the
    // elbow's x, so it divides by the same dx and is UNBOUNDED where an angle at least
    // saturates at 90°. See the ⚠ note at the top of this file for how often it fires.
    double  minShoulderSpanRatio = tuned::upperBody::kMinShoulderSpanRatio; // upperBody.minShoulderSpanRatio
    // The ELBOW line's gate, and an ABSOLUTE pixel floor rather than a ratio — a ratio is
    // INERT here. The elbows are NARROWEST at address (the arms hang together and separate
    // through the swing), so |dx|/address|dx| is ≈1 at address and ≥1 after it: the gate
    // would never fire, and it would read exactly 1.0 at address, where a 20 px elbow
    // separation is pure keypoint noise and `elbowAlignment` is READ.
    double  minElbowSpanPx       = tuned::upperBody::kMinElbowSpanPx;       // upperBody.minElbowSpanPx
    // The HIP line's gate, for spineSideBend's other half. Deliberately the LOWER-BODY key:
    // it is the same geometric question about the same line, so it takes the same ratio.
    // ⚠ It does NOT make the two modules agree frame by frame — this module resolves the hip
    // line against its own address reference frames and its own denominator (see
    // LowerBodyState::hipLineValid). One ratio, two references.
    double  minHipSpanRatio      = tuned::lowerBody::kMinHipSpanRatio;      // lowerBody.minHipSpanRatio
    // Where a bridge stops being a measurement (metric_channel.h channelValidityMask).
    // Carried in the config, then on the result, so buildUpperBodySeries keeps its signature.
    // NEGATIVE maxBridgeUs = do not mask at all, the pre-mask behaviour.
    int64_t maxBridgeUs          = tuned::channel::kMaxBridgeUs;            // channel.maxBridgeUs
    double  bridgeSpacingFactor  = tuned::channel::kBridgeSpacingFactor;    // channel.bridgeSpacingFactor

    static UpperBodyConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        UpperBodyConfig c;
        apply(ov, "upperBody.confMin",              c.confMin);
        apply(ov, "upperBody.addrMinFrames",        c.addrMinFrames);
        apply(ov, "upperBody.addrWindowUs",         c.addrWindowUs);
        apply(ov, "upperBody.minShoulderSpanPx",    c.minShoulderSpanPx);
        apply(ov, "upperBody.minShoulderSpanRatio", c.minShoulderSpanRatio);
        apply(ov, "upperBody.minElbowSpanPx",       c.minElbowSpanPx);
        apply(ov, "lowerBody.minHipSpanRatio",      c.minHipSpanRatio);
        apply(ov, "channel.maxBridgeUs",            c.maxBridgeUs);
        apply(ov, "channel.bridgeSpacingFactor",    c.bridgeSpacingFactor);
        return c;
    }
};

// The address reference every channel is scaled or referenced against. Captured over a robust
// window rather than one frame, component-wise median, exactly like the lower-body and ball
// references — a single address frame is one pose estimate and inherits all of its jitter.
struct UpperBodyReference {
    double  shoulderSpanPx = 0.0;   // |lead shoulder − trail shoulder|, the % shoulder width denominator
    double  stanceSpanPx   = 0.0;   // |lead ankle − trail ankle|, the % stance width denominator
    double  leadArmLenPx   = 0.0;   // shoulder→elbow + elbow→wrist, the % arm length denominator
    double  leadSign       = 1.0;   // +1 if the lead side is image +x at address, else −1
    bool    valid          = false; // a shoulder span above the floor was resolved

    // The two body lines' HORIZONTAL address spans, px — the denominators of the
    // foreshortening ratios. There is no elbow entry: that line takes an absolute pixel
    // floor (cfg.minElbowSpanPx) because a ratio against its address value is inert, the
    // elbows being at their narrowest exactly there.
    //
    // ⚠ shoulderDxPx IS NOT shoulderSpanPx. That one is the EUCLIDEAN separation, because
    // that is what "% shoulder width" has always meant here and in foot_metrics; this one
    // is |Δx| in the image, because the degeneracy the gate exists for is horizontal
    // foreshortening and a Euclidean length stays comfortably large while Δx goes to zero.
    // Two different numbers for two different jobs; conflating them would silently disable
    // the gate on exactly the frames it is for.
    //
    // 0.0 means the line's address span was never measured (its joints were not confident
    // over the reference window), and the channel it gates is then ABSENT for the whole
    // swing rather than ungated — the same refusal `leadArmLenPx == 0` already makes for
    // leadHandWidth. A ratio with no denominator is not a measurement. For hipDxPx that has
    // a visible consequence worth knowing: an address whose hips were never confident leaves
    // spineSideBend absent for the whole swing even though the lower-body module, with its
    // own reference, still produces hipLineTilt.
    double  shoulderDxPx   = 0.0;
    double  hipDxPx        = 0.0;
};

// Every channel this module produces, sparse (valid frames only) and already in its final unit.
struct UpperBodyResult {
    std::vector<int64_t> grid;      // one entry per input frame, time order — the resample target

    MetricChannel axisTilt;         // secondaryAxisTilt        °   (+ = leaning away from target)
    MetricChannel sideBend;         // spineSideBend            °   (+ = side bend toward the trail side)
    MetricChannel thoraxDrift;      // thoraxLateralDrift       % stance width (+ = toward the lead side)
    MetricChannel shoulderPlane;    // shoulderPlaneAngle       °   (+ = trail shoulder above lead)
    MetricChannel elbowLine;        // elbowAlignment           °   (+ = trail elbow above lead)
    MetricChannel trailElbowHeight; // trailElbowHeight         % shoulder width (+ = above the shoulder line)
    MetricChannel leadHandWidth;    // leadHandWidth            % arm length
    MetricChannel leadArmGap;       // leadUpperArmToChest      % shoulder width
    MetricChannel leadArmToTorso;   // leadArmToTorso           °   (unsigned, 0–180)

    // The instants each gated channel's geometry was REFUSED at — the joints were confident and the
    // LINE was not usable. Ascending, one entry per such frame.
    //
    // ⚠ These are NOT "the instants the channel lacks", and the difference decides whether a value is
    // drawn as measured. A frame the detector dropped is a HOLD the resample may bridge; a frame the
    // geometry refused has nothing to hold, so channelValidityMask forces it to 0 whatever the bridge
    // budget. Without the distinction a 23-frame gated shoulder run came back with 30 of its frames
    // flagged valid, and a P4 shoulder-plane sample of 25.8° was emitted from the bridge (see the
    // note in metric_channel.h).
    std::vector<int64_t> gatedShoulderLine;   // shoulderPlaneAngle, trailElbowHeight
    std::vector<int64_t> gatedElbowLine;      // elbowAlignment
    std::vector<int64_t> gatedSideBend;       // spineSideBend — EITHER line refused

    UpperBodyReference ref;
    int64_t maxBridgeUs = tuned::channel::kMaxBridgeUs;  // carried from the config for the builder
    double  bridgeSpacingFactor = tuned::channel::kBridgeSpacingFactor;   // likewise
    int  frameW = 0, frameH = 0;
    bool valid  = false;            // the address reference resolved AND at least one channel has samples
};

// Per-frame upper-body channels from the (smoothed, else raw) pose track.
//
// `leadIsLeft` selects which physical side is "lead", the same handedness convention the lead arm,
// the feet and the lower body already use (the caller resolves it once, e.g. `handedness != 2`).
// `addressUs` is the Address phase instant; < 0 falls back to the first cfg.addrMinFrames frames
// with a usable upper body, so a swing with no confident ladder still gets a reference.
UpperBodyResult trackUpperBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs = -1, const UpperBodyConfig &cfg = {});

// Resample the sparse channels onto the full per-frame grid and emit the nine series. Empty when
// the address reference is unresolved or the shoulder span is below its floor — below that the
// percentages are noise divided by noise, and the honest answer is that nothing was measured.
//
// Each series carries MetricSeries::valid: 0 on every frame whose geometry this module GATED (always,
// whatever the bridge budget — a refused line has no value to hold) and 0 where the resample had to
// bridge more than res.maxBridgeUs across frames the DETECTOR lost; empty when neither happened. AN
// INVALID INSTANT EMITS NO PHASE SAMPLE, the same rule an unsegmented phase already obeys.
std::vector<MetricSeries> buildUpperBodySeries(const UpperBodyResult &res,
                                               const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
