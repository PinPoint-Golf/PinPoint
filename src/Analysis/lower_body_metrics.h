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

// Lower-body FRONTAL-PLANE metrics — see docs/design/lower_body_face_on_metrics.md.
// Structure mirrors head_track.h and foot_metrics.h deliberately (a per-frame
// state pass, a robust Address reference, Address-referenced sparse channels, a
// MetricSeries builder) — pure, deterministic, Qt-only (no OpenCV/ORT);
// unit-tested standalone.
//
// THE RULE THAT DECIDES WHAT IS IN HERE: a face-on camera resolves the frontal
// plane (image x and y) and NOTHING in depth. Every channel below is a frontal-
// plane displacement or a frontal-plane angle. That is also why the neighbouring
// planned metrics stay planned and are not quietly added here — `pelvisThrust`
// is toward-and-away from the camera, and `leadKneeFlexion` is a sagittal angle
// the frontal projection foreshortens almost to nothing. Producing either from
// this view would put a confident number on a quantity the camera cannot see.
//
// It reads only COCO BODY keypoints 11–16 (hips, knees, ankles). Those exist in
// BOTH pose layouts, so unlike foot_metrics this module works on legacy 17-kp
// tracks as well as WholeBody ones — there is no keypoint here that a swing
// recorded before WB0 does not have.
//
// ── feetAlignment, and why it exists next to toeLineAngle ───────────────────
//
// `toeLineAngle` (foot_metrics) already reports a stance line, from the big toes
// at address. `feetAlignment` reports the same kind of reading from the ANKLES and
// through the whole swing, and it is not a duplicate for two reasons the content
// states: the ankle joints are far less affected by foot flare than the toes are,
// and the impact read — how the trail foot has rolled and the ankles re-oriented
// as the player pushes off — has no counterpart in an address-only scalar.
//
// It also fixes a sign defect by construction. `toeLineAngle` is a RAW atan2 of
// the lead→trail vector, which inverts for a left-handed golfer or a mirrored
// camera; `feetAlignment` uses the same absolute-denominator form as the hip line
// below, so it describes the same posture whichever way the camera was pointed.
//
// ── comOverLeadFoot is a PROXY, and says so ────────────────────────────────
//
// It is the pelvis centre's distance from the lead ankle along the stance line, not
// a centre of mass: without a segment-mass model or pressure data this reads
// geometry only, which is exactly what its descriptor tells the golfer. The reading
// that matters is at the finish, where a player who used the ground is stacked over
// the lead foot and one who did not is still falling. It is UNSIGNED — "further
// from the lead ankle" covers both still-back and fallen-through, and those are the
// same fault seen from either side.
//
// ── leadKneeDrift, and why it is a DIFFERENCE ───────────────────────────────
//
// The coaching observation is that the lead knee working in toward the trail
// knee at the top suggests the turn was not made in the hips. Measuring that
// directly — the lead knee's lateral travel in the image — does NOT work, and
// the reason is geometric rather than a matter of precision. Pelvic rotation
// carries the lead hip rearward AND toward the trail side, so in a face-on
// projection a DEEPER turn moves the lead knee toward the trail knee by exactly
// the signature the fault is supposed to have. A detector on raw knee travel
// fires on good turns and bad ones alike.
//
// What separates them is the HIP. Under a genuine turn the lead hip and the lead
// knee travel together, because the whole limb is being carried by the pelvis.
// Under the compensation the pelvis has barely rotated and the knee goes in on
// its own. So the channel is the DIFFERENCE of the two displacements:
//
//     leadKneeDrift(t) = Δx(leadKnee) − Δx(leadHip)     [both from address]
//
// Both points share the same rotation, so the first-order projection term
// cancels. It is not a proof — hip and knee sit at different radii from the
// rotation axis, so a residual survives — which is why the corridor over this is
// a hypothesis to be seated from a corpus and NOT a threshold read off a paper.
// No published figure exists for this quantity in any unit.
//
// ── Sign ────────────────────────────────────────────────────────────────────
//
// Lateral channels are LEAD-POSITIVE, per docs/design/pinpoint_sign_conventions.md
// rule 2. Which image direction that is gets resolved from the address geometry
// (which ankle is further along +x), not assumed — a camera may be mirrored, and
// a convention that depends on the operator not flipping the preview is not a
// convention. The fault direction for all three lateral channels is therefore
// NEGATIVE: a knee working toward the trail leg, a pelvis swaying off the ball.
//
// `hipLineTilt` is an ABSOLUTE angle, not an address-referenced one, because the
// figure it is read against is absolute: the trail hip sits somewhat above the
// lead hip at the top for every golfer, and the question is how much.
//
// ── Units ───────────────────────────────────────────────────────────────────
//
// Displacements are a PERCENTAGE OF THE ADDRESS ANKLE SPAN; angles are degrees.
// One unit each, for all time. The alternative — centimetres via a ruler — was
// rejected: the ruler needs a detected ball, and a metric that is present in cm
// on some swings and absent on others is worse than one that is always present
// and body-relative. It is also the better reading, for the reason stanceWidth
// gives: a norm in millimetres is a norm on the golfer's height.
//
// The span is measured ANKLE to ankle rather than heel to heel so that the
// denominator lives in the same keypoint set as the numerators and survives a
// legacy track. It is close to but not identical with `stanceWidth`'s heel-to-
// heel span, which is why these carry their own unit string rather than
// borrowing that one.

#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // PoseTrack2D, PoseFrame2D, MetricSeries, PhaseEvent
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::lowerBody::

namespace pinpoint::analysis {

// Lower-body knobs. Defaults track the frozen constants (pp_tuned_constants.h
// lowerBody::); SwingLab sweeps them via "lowerBody.*" dotted keys.
struct LowerBodyConfig {
    double  confMin        = tuned::lowerBody::kConfMin;         // lowerBody.confMin
    int     addrMinFrames  = tuned::lowerBody::kAddrMinFrames;   // lowerBody.addrMinFrames
    int64_t addrWindowUs   = tuned::lowerBody::kAddrWindowUs;    // lowerBody.addrWindowUs
    double  minStanceSpanPx = tuned::lowerBody::kMinStanceSpanPx; // lowerBody.minStanceSpanPx

    static LowerBodyConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        LowerBodyConfig c;
        apply(ov, "lowerBody.confMin",         c.confMin);
        apply(ov, "lowerBody.addrMinFrames",   c.addrMinFrames);
        apply(ov, "lowerBody.addrWindowUs",    c.addrWindowUs);
        apply(ov, "lowerBody.minStanceSpanPx", c.minStanceSpanPx);
        return c;
    }
};

// Per-frame lower-body state. Coordinates are PIXELS (isotropic — kp arrive
// normalized by width and height separately, so every geometric quantity here is
// computed after de-normalizing by both). Each PAIR carries its own validity:
// the hip line needs both hips, the knee channel needs the lead hip and the lead
// knee, and the span needs both ankles. A pair below confidence leaves its flag
// false and the channel skips that frame — the resample bridges it, never NaN.
struct LowerBodyState {
    int64_t t_us       = 0;
    bool    hipsValid  = false;   // both hips
    bool    leadLegValid = false; // lead hip AND lead knee — what leadKneeDrift needs
    bool    anklesValid  = false; // both ankles
    QPointF leadHipPx,  trailHipPx;
    QPointF leadKneePx, trailKneePx;
    QPointF leadAnklePx, trailAnklePx;
    float   conf       = 0.f;     // mean of the contributing confidences
};

// One sparse address-referenced channel (valid-subset t_us, ascending).
struct LowerBodyChannel {
    std::vector<int64_t> t_us;
    std::vector<double>  value;
};

struct LowerBodyResult {
    std::vector<LowerBodyState> states;   // one per input frame (time order)

    // All already in their final units — % of address ankle span for the
    // displacements, degrees for the angles. kneeDrift/pelvisSway are
    // lead-positive; pelvisLift is positive UP; hipLineTilt and feetAlignment are
    // positive when the TRAIL end sits above the lead end; comOverLead is an
    // unsigned distance.
    LowerBodyChannel kneeDrift, pelvisSway, pelvisLift, hipTilt;
    LowerBodyChannel feetAlign, comOverLead;

    QPointF addrLeadHipPx, addrTrailHipPx;
    QPointF addrLeadKneePx;
    double  addrSpanPx = 0.0;    // ankle-to-ankle at address, px — the denominator
    double  leadSign   = 1.0;    // +1 if the lead side is image +x at address, else −1
    int     frameW = 0, frameH = 0;
    bool    valid  = false;      // address reference resolved AND the span cleared its floor
};

// Per-frame lower-body state + the address-referenced channels, from the
// (smoothed, else raw) pose track. `leadIsLeft` selects which physical side
// (COCO L/R) is "lead" — the same handedness convention as the lead arm and the
// feet (the caller resolves it, e.g. `handedness != 2`). addressUs = the Address
// phase instant for the robust reference; < 0 ⇒ fall back to the first
// cfg.addrMinFrames frames with a usable lower body.
LowerBodyResult trackLowerBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs = -1, const LowerBodyConfig &cfg = {});

// Resample the sparse channels onto the full per-frame grid (linear interp, hold
// at ends, gaps bridged — NEVER NaN) and emit leadKneeDrift / pelvisSway /
// pelvisLift / hipLineTilt / feetAlignment / comOverLeadFoot with phase samples.
// The finish is sampled alongside Address/Top/Impact because comOverLeadFoot is
// read there and nothing sampled it before. UNSCORED here
// (the corridors live in the diagnostics norm set, not in the producer). Empty
// when the address reference is unresolved or the stance span is unusable.
std::vector<MetricSeries> buildLowerBodySeries(const LowerBodyResult &res,
                                               const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
