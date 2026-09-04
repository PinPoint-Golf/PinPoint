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
// ── plumbBobDistance, and why it is the one metric here in INCHES ───────────
//
// The plumb-bob reading a coach takes at address: where the centre of the hips
// sits relative to the centre of the stance, along the stance line. It is the
// SIGNED twin of comOverLeadFoot about the stance CENTRE rather than the lead
// ankle, and it uses the same projection — the component along the live ankle
// line — so a tilted stance line or slightly uneven ground does not leak into
// the number.
//
// Its sign needs no leadSign term: the unit vector runs from the TRAIL ankle to
// the LEAD ankle, so the dot product is lead-positive by construction and a
// mirrored camera cannot invert it. Positive means the hips sit AHEAD of the
// stance centre, toward the lead side.
//
// It is the ONE channel here stated in a real-world unit, and the exception is
// deliberate rather than an oversight of the rule below. The figures it is read
// against are absolute inches a coach quotes out loud — a wedge about 1–2 in
// ahead, a mid-iron 0–1, a driver about 1 behind — and unlike stance width they
// are not a statement about the golfer's height. So it takes the ball-diameter
// ruler, and follows leadHeelLift's rule exactly: emitted in inches when that
// ruler resolves and ABSENT when it does not, never rescaled into a fallback
// unit. A metric whose unit changes per swing cannot carry a norm, because the
// norm declares one unit and grading compares the numbers without consulting it.
//
// TWO LIMITS, both real, both stated in the descriptor rather than hidden here:
// the ruler is calibrated at the ball's ground-plane depth while the hips are
// about a metre above it, so a camera not at hip height carries a small scale
// bias; and pelvic rotation moves the APPARENT hip centre sideways with no
// actual shift, so the address and impact readings are the trustworthy ends of
// the ladder and the mid-swing ones carry a turn artefact.
//
// ── Units ───────────────────────────────────────────────────────────────────
//
// Displacements are a PERCENTAGE OF THE ADDRESS ANKLE SPAN; angles are degrees.
// plumbBobDistance is the documented exception, in inches — see above.
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
    // The hip LINE's foreshortening gate — |dx_hips| / address |dx_hips|. A separate
    // failure from minStanceSpanPx, which guards the percentage denominator: this one
    // guards an ANGLE whose divisor is the live horizontal separation, and which runs
    // to ±90° as the turning pelvis collapses the two hips into one image column.
    double  minHipSpanRatio = tuned::lowerBody::kMinHipSpanRatio; // lowerBody.minHipSpanRatio
    // Where a bridge stops being a measurement (metric_channel.h channelValidityMask).
    // Carried here rather than passed to buildLowerBodySeries so the builder's signature
    // and every caller stay as they are; SwingLab sweeps one key for every producer.
    // NEGATIVE = do not mask at all, the pre-mask behaviour.
    int64_t maxBridgeUs     = tuned::channel::kMaxBridgeUs;       // channel.maxBridgeUs
    // …and the local-spacing multiplier that keeps the sparsely posed address region from
    // being marked for one dropped frame. See channelValidityMask.
    double  bridgeSpacingFactor = tuned::channel::kBridgeSpacingFactor; // channel.bridgeSpacingFactor

    static LowerBodyConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        LowerBodyConfig c;
        apply(ov, "lowerBody.confMin",         c.confMin);
        apply(ov, "lowerBody.addrMinFrames",   c.addrMinFrames);
        apply(ov, "lowerBody.addrWindowUs",    c.addrWindowUs);
        apply(ov, "lowerBody.minStanceSpanPx", c.minStanceSpanPx);
        apply(ov, "lowerBody.minHipSpanRatio", c.minHipSpanRatio);
        apply(ov, "channel.maxBridgeUs",       c.maxBridgeUs);
        apply(ov, "channel.bridgeSpacingFactor", c.bridgeSpacingFactor);
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
    // Both hips confident AND the hip line still spans enough of the image to HAVE a
    // tilt: |dx| / addrHipSpanPx >= cfg.minHipSpanRatio. Resolved against the address
    // reference, so it can only be answered after that reference exists — it is set in
    // trackLowerBody's channel pass, not in the per-frame state pass, and stays false
    // on every frame when the reference never resolved.
    //
    // Published on the state for TESTS AND DIAGNOSTICS, and for nothing else today.
    //
    // ⚠ It is NOT how upper_body_metrics learns about the hip line, and it cannot be: the two
    // modules are separate analysis stages with no shared result, so the upper module resolves
    // its own hip line against its OWN address reference frames and its own denominator. Same
    // ratio (lowerBody.minHipSpanRatio), two references — so on a marginal frame the two can
    // disagree, and if the upper module's address hips were never confident it has no
    // denominator at all and spineSideBend is absent for the whole swing while hipLineTilt is
    // produced here. That is stated rather than hidden, and pinned by a test.
    bool    hipLineValid = false;
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
    // INCHES, and empty when the ball ruler did not resolve. Lead-positive.
    LowerBodyChannel plumbBob;

    // The instants the HIP LINE was REFUSED on geometry — both hips confident, the line
    // foreshortened below cfg.minHipSpanRatio. Ascending, one entry per such frame.
    //
    // ⚠ NOT the same set as "instants hipTilt lacks", and the difference is the whole point:
    // a frame the detector dropped is a HOLD the resample may bridge, while a frame the
    // geometry refused has no value to hold — the quantity did not exist at that instant.
    // channelValidityMask forces those to 0 whatever the bridge budget, which is what stops a
    // 10-frame gated run at 7 ms spacing from being flagged as measured (it was: see the note
    // in metric_channel.h).
    std::vector<int64_t> gatedHipLine;

    QPointF addrLeadHipPx, addrTrailHipPx;
    QPointF addrLeadKneePx;
    double  addrSpanPx = 0.0;    // ankle-to-ankle at address, px — the denominator
    // |trail hip x − lead hip x| at address, px — the HIP LINE's reference span, and a
    // median over the same reference frames as everything else here so the ratio shares
    // its scale by construction rather than by luck. NOT the same quantity as
    // addrSpanPx: this one is the numerator's own address value, not a unit denominator.
    double  addrHipSpanPx = 0.0;
    double  leadSign   = 1.0;    // +1 if the lead side is image +x at address, else −1
    int64_t maxBridgeUs = tuned::channel::kMaxBridgeUs;  // carried from the config for the builder
    double  bridgeSpacingFactor = tuned::channel::kBridgeSpacingFactor;   // likewise
    int     frameW = 0, frameH = 0;
    bool    valid  = false;      // address reference resolved AND the span cleared its floor
};

// Per-frame lower-body state + the address-referenced channels, from the
// (smoothed, else raw) pose track. `leadIsLeft` selects which physical side
// (COCO L/R) is "lead" — the same handedness convention as the lead arm and the
// feet (the caller resolves it, e.g. `handedness != 2`). addressUs = the Address
// phase instant for the robust reference; < 0 ⇒ fall back to the first
// cfg.addrMinFrames frames with a usable lower body.
// `mmPerPx` is the ball-diameter ruler (ball_position.h) at the ball's ground-plane
// depth — face-on, essentially the feet's depth. Pass <= 0 (the default) when it did
// not resolve and `plumbBob` stays empty, which is what makes that metric absent
// rather than present in a second unit.
LowerBodyResult trackLowerBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs = -1, const LowerBodyConfig &cfg = {},
                               double mmPerPx = -1.0);

// Resample the sparse channels onto the full per-frame grid (linear interp, hold
// at ends, gaps bridged — NEVER NaN) and emit leadKneeDrift / pelvisSway /
// pelvisLift / hipLineTilt / feetAlignment / comOverLeadFoot / plumbBobDistance
// with phase samples. The finish is sampled alongside Address/Top/Impact because
// comOverLeadFoot is read there and nothing sampled it before; hipLineTilt and
// plumbBobDistance are sampled across the whole P1–P7 ladder because both are
// read as a progression rather than at one instant. AN UNSEGMENTED PHASE EMITS
// NO SAMPLE, and neither does an INVALID INSTANT. A grid sample carries a 0 in
// MetricSeries::valid when the producer GATED that frame's geometry (always — no
// bridge budget covers a quantity that did not exist), or when the resample had to
// bridge more than cfg.maxBridgeUs across frames the DETECTOR lost; neither is a
// place a phase reading can be taken from.
// UNSCORED here (the corridors live in the diagnostics norm set, not
// in the producer). Empty when the address reference is unresolved or the stance
// span is unusable.
std::vector<MetricSeries> buildLowerBodySeries(const LowerBodyResult &res,
                                               const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
