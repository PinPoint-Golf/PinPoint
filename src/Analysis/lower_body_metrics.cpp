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

#include "lower_body_metrics.h"

#include "metric_channel.h"   // channelValidityMask + the shared interp / phase / nearest helpers

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>

namespace pinpoint::analysis {
namespace {

// The empty gated-instant list, at namespace scope because a default argument cannot name a
// local of the enclosing function (even a static one).
const std::vector<int64_t> kNoneGated;

// COCO-17 body indices, shared by both layouts (anatomy_vocabulary.h kp::).
constexpr int kLHip = 11, kRHip = 12, kLKnee = 13, kRKnee = 14, kLAnkle = 15, kRAnkle = 16;

constexpr double kRadToDeg = 57.29577951308232;
constexpr double kMmPerInch = 25.4;

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// ── σ helpers ───────────────────────────────────────────────────────────────────────────────
//
// One keypoint's smoother posterior σ (px), or 0 when there is none. 0 is PoseKpAux::sigma's own
// "no smoothed value" sentinel and it stays a sentinel all the way through: nothing below ever
// treats it as a measured zero.
double sigAt(const PoseKpAux *aux, int idx)
{
    if (!aux) return 0.0;
    const double v = double(aux->sigma[size_t(idx)]);
    return v > 0.0 ? v : 0.0;
}

// `quad2`, `lineTiltSigmaDeg` and `medianSigmaOverValid` all live in metric_channel.h, beside
// channelValidityMask whose output the last of them reads, and this file owns no copy of any of them.
// The line-tilt σ in particular is shared with upper_body_metrics on purpose: the two files measure
// four body lines between them under ONE sign convention (`lineTiltDeg`, spelled the same way in
// both), so they take one uncertainty convention too.

// Signed angle (deg) of a body line, POSITIVE when the trail end sits above the lead end.
// Image y grows downward, so "trail higher" is trailY < leadY and the numerator is leadY − trailY.
// The denominator is the ABSOLUTE horizontal separation, which is what makes the sign independent
// of which image side the lead is on — the alternative (a raw atan2 of the lead→trail vector)
// flips its answer for a left-handed golfer, or for a mirrored camera, while describing the same
// posture. `toeLineAngle` in foot_metrics IS that alternative, which is one of the two reasons
// `feetAlignment` exists beside it (see the header).
//
// Shared by the hip line and the ankle line here, and by the shoulder and elbow lines in
// upper_body_metrics.cpp — one convention for every body line in the product.
double lineTiltDeg(const QPointF &lead, const QPointF &trail)
{
    const double dx = std::abs(trail.x() - lead.x());
    if (dx <= 1e-9) return 0.0;                 // ends vertically stacked — no line to measure
    return std::atan2(lead.y() - trail.y(), dx) * kRadToDeg;
}

// Per-frame lower-body state from hips / knees / ankles. `aux` is the smoother's per-keypoint
// honesty record for THIS frame, or nullptr on a track that was never smoothed — in which case every
// σ below stays 0 and every channel's σ stays absent, which is the pre-smoother behaviour and the
// only honest one (we have no error budget, so we claim none).
LowerBodyState computeState(const PoseFrame2D &f, int frameW, int frameH, double confMin,
                            bool leadIsLeft, const PoseKpAux *aux)
{
    LowerBodyState s;
    s.t_us = f.t_us;

    // No layout guard is needed and none is written: PoseFrame2D's kp/conf are fixed 133-wide
    // arrays whatever produced them, and 11–16 are COCO BODY indices that both layouts fill. A
    // pre-WholeBody track has zeroed confidences only in the 17+ tail, which is why this module
    // answers on swings where foot_metrics cannot.
    const int leadHipIdx   = leadIsLeft ? kLHip   : kRHip;
    const int trailHipIdx  = leadIsLeft ? kRHip   : kLHip;
    const int leadKneeIdx  = leadIsLeft ? kLKnee  : kRKnee;
    const int trailKneeIdx = leadIsLeft ? kRKnee  : kLKnee;
    const int leadAnkIdx   = leadIsLeft ? kLAnkle : kRAnkle;
    const int trailAnkIdx  = leadIsLeft ? kRAnkle : kLAnkle;

    const auto pxOf = [&](int idx) {
        return QPointF(f.kp[idx].x() * frameW, f.kp[idx].y() * frameH);
    };
    const auto ok = [&](int idx) { return f.conf[idx] >= confMin; };

    s.leadHipPx    = pxOf(leadHipIdx);
    s.trailHipPx   = pxOf(trailHipIdx);
    s.leadKneePx   = pxOf(leadKneeIdx);
    s.trailKneePx  = pxOf(trailKneeIdx);
    s.leadAnklePx  = pxOf(leadAnkIdx);
    s.trailAnklePx = pxOf(trailAnkIdx);

    // The smoother's posterior σ for exactly the five joints the channels read. Indexed by the same
    // lead/trail resolution as the points, so a mirrored camera or a left-handed golfer cannot pair a
    // point with the other side's σ.
    s.leadHipSigPx   = sigAt(aux, leadHipIdx);
    s.trailHipSigPx  = sigAt(aux, trailHipIdx);
    s.leadKneeSigPx  = sigAt(aux, leadKneeIdx);
    s.leadAnkleSigPx = sigAt(aux, leadAnkIdx);
    s.trailAnkleSigPx = sigAt(aux, trailAnkIdx);

    s.hipsValid    = ok(leadHipIdx) && ok(trailHipIdx);
    s.leadLegValid = ok(leadHipIdx) && ok(leadKneeIdx);
    s.anklesValid  = ok(leadAnkIdx) && ok(trailAnkIdx);
    s.conf = 0.25f * (f.conf[leadHipIdx] + f.conf[trailHipIdx]
                      + f.conf[leadKneeIdx] + f.conf[leadAnkIdx]);
    return s;
}

// interpChannel / phaseTimeOpt / nearestIndex USED TO LIVE HERE, as private copies of the same
// three functions metric_channel.h holds. They are gone, and this note is the reason: the validity
// mask below has to call metric_channel.h's channelValidityMask, and the moment that header is
// included an unqualified call to a private copy of `interpChannel` is AMBIGUOUS between the
// anonymous namespace and pinpoint::analysis. The copies were character-for-character identical to
// the shared ones (nearestIndex there adds an empty-grid guard that cannot fire here — the caller
// checks res.states first), so deleting them changes no output. metric_channel.h's own note about
// the three unmigrated producers is updated to say so; head_track and foot_metrics still carry
// theirs, and still have no reason to be opened.

} // namespace

LowerBodyResult trackLowerBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs, const LowerBodyConfig &cfg, double mmPerPx)
{
    LowerBodyResult res;
    res.frameW = frameW;
    res.frameH = frameH;
    res.maxBridgeUs = cfg.maxBridgeUs;   // the builder reads these off the result, not off a config
    res.bridgeSpacingFactor = cfg.bridgeSpacingFactor;
    if (frameW <= 0 || frameH <= 0)
        return res;

    // The smoothed companion track (parallel t_us, same normalized kp) is preferred, exactly like
    // head_track and foot_metrics — falls back to raw frames on swings analysed before the
    // smoother existed.
    const std::vector<PoseFrame2D> &frames = pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frames.empty())
        return res;

    // THE SMOOTHER'S σ IS ATTACHED ONLY WHERE IT DESCRIBES THE CURVE WE ARE ABOUT TO MEASURE.
    // `smoothedAux` is documented as parallel to `pose.smoothed`, and smoothPoseTrack fills the two
    // arrays together, so `frames` IS the track the σ belongs to exactly when `smoothed` is
    // non-empty. Requiring both is not defensive noise: a hand-built track carrying an aux array and
    // no smoothed track would otherwise stamp a smoother's posterior onto raw passthrough values,
    // which is a confident statement about a curve nobody drew.
    const bool haveAux = !pose.smoothed.empty() && pose.smoothedAux.size() == frames.size();

    res.states.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); ++i)
        res.states.push_back(computeState(frames[i], frameW, frameH, cfg.confMin, leadIsLeft,
                                          haveAux ? &pose.smoothedAux[i] : nullptr));

    // Robust (median) address reference: prefer the confident frames inside the Address-event
    // window; if none (or no event), fall back to the first N usable frames. Same shape as the two
    // neighbouring modules resolve theirs.
    //
    // The admission test is `hipsValid && leadLegValid && anklesValid` — everything, not any one
    // piece. This reference sets the origin for THREE channels and the denominator for all four,
    // so a frame that supplied only part of it would leave the channels referenced to different
    // instants, which is a class of error that produces plausible numbers.
    const auto usable = [](const LowerBodyState &s) {
        return s.hipsValid && s.leadLegValid && s.anklesValid;
    };

    std::vector<const LowerBodyState *> ref;
    if (addressUs >= 0) {
        for (const LowerBodyState &s : res.states)
            if (usable(s) && std::llabs(s.t_us - addressUs) <= cfg.addrWindowUs)
                ref.push_back(&s);
    }
    if (ref.empty()) {
        for (const LowerBodyState &s : res.states) {
            if (!usable(s)) continue;
            ref.push_back(&s);
            if (int(ref.size()) >= cfg.addrMinFrames) break;
        }
    }
    if (ref.empty())
        return res;   // no usable lower body anywhere — leave valid == false

    std::vector<double> lhx, lhy, thx, thy, lkx, spans, hipSpans, leadAnkX, trailAnkX;
    for (const LowerBodyState *s : ref) {
        lhx.push_back(s->leadHipPx.x());   lhy.push_back(s->leadHipPx.y());
        thx.push_back(s->trailHipPx.x());  thy.push_back(s->trailHipPx.y());
        lkx.push_back(s->leadKneePx.x());
        spans.push_back(std::abs(s->trailAnklePx.x() - s->leadAnklePx.x()));
        // The hip line's own address span, |dx| in the IMAGE — the same quantity the live frames are
        // compared against, taken over the same reference frames and by the same median, so the
        // ratio cannot inherit a scale difference. Deliberately NOT the Euclidean hip separation:
        // the degeneracy the gate exists for is horizontal foreshortening, and a Euclidean length
        // stays comfortably large while dx goes to zero.
        //
        // NB the contract asked for |median(trailX) − median(leadX)|, i.e. the span between the two
        // reference points above; this is median(|dx_i|), the span measured per frame and then
        // reduced. They differ by well under a pixel on a still address and the per-frame form is
        // the more honest of the two: it is the same quantity the live frames are compared against,
        // measured the same way, and a median of a length cannot go negative or collapse when one
        // endpoint is noisier than the other.
        hipSpans.push_back(std::abs(s->trailHipPx.x() - s->leadHipPx.x()));
        leadAnkX.push_back(s->leadAnklePx.x());
        trailAnkX.push_back(s->trailAnklePx.x());
    }

    res.addrLeadHipPx  = QPointF(medianOf(lhx), medianOf(lhy));
    res.addrTrailHipPx = QPointF(medianOf(thx), medianOf(thy));
    res.addrLeadKneePx = QPointF(medianOf(lkx), 0.0);
    res.addrSpanPx     = medianOf(spans);
    res.addrHipSpanPx  = medianOf(hipSpans);

    // Which image direction the lead side is, resolved from the address geometry rather than
    // assumed. A camera can be mirrored and an operator can flip the preview, so a convention that
    // depends on neither happening is not a convention. Sign-conventions rule 2: positive is toward
    // the lead side.
    res.leadSign = (medianOf(leadAnkX) <= medianOf(trailAnkX)) ? -1.0 : 1.0;

    // The denominator floor. Below it every percentage would be noise divided by noise, and the
    // honest answer is that nothing was measured — not a large number.
    if (res.addrSpanPx < cfg.minStanceSpanPx)
        return res;

    const double toPct = 100.0 / res.addrSpanPx;
    const double addrLeadHipX  = res.addrLeadHipPx.x();
    const double addrLeadKneeX = res.addrLeadKneePx.x();
    const double addrMidX      = 0.5 * (res.addrLeadHipPx.x() + res.addrTrailHipPx.x());
    const double addrMidY      = 0.5 * (res.addrLeadHipPx.y() + res.addrTrailHipPx.y());

    // The hip LINE's validity gate. A tilt is atan2(dy, |dx|), so as the pelvis turns toward the
    // target the two hips foreshorten into the same image column and the angle runs to ±90° while
    // nothing about the golfer's posture changed — the −88° hip tilt a review chart shows just after
    // impact is exactly this, and it is a reading of the camera, not of the player. Below the ratio
    // the frame HAS no hip line and hipLineTilt is ABSENT for it (never a sentinel, never the 0.0
    // lineTiltDeg returns for a vertically stacked pair, which would read as "perfectly level").
    //
    // The other channels are deliberately NOT gated on this: sway, lift, knee drift, the plumb bob
    // and comOverLeadFoot are POSITIONS, differences of positions, or projections onto the Euclidean
    // stance line. The same turn distorts them, but it does not divide them by a vanishing span —
    // that is a domain question (design §5.1's Address→Impact domains) answered one layer up, not a
    // validity question answered here.
    //
    // feetAlignment IS a lineTiltDeg and so is the one exception in this file, ungated by judgement
    // rather than by construction: the feet stay planted, so unlike the pelvis the stance line does
    // not turn out of the image plane and its dx never collapses. If a corpus swing is ever found
    // where it does, it takes this same gate against the address ankle span.
    const bool hipRatioMeasurable = res.addrHipSpanPx > 1e-9;
    for (LowerBodyState &s : res.states) {
        if (s.hipsValid && hipRatioMeasurable) {
            const double dx = std::abs(s.trailHipPx.x() - s.leadHipPx.x());
            s.hipLineValid = (dx / res.addrHipSpanPx) >= cfg.minHipSpanRatio;
        }
        // leadKneeDrift — the knee's displacement MINUS its own hip's. See the header: this
        // difference is what survives the projection of a genuine pelvic turn, and the raw knee
        // travel is not.
        if (s.leadLegValid) {
            const double dKnee = s.leadKneePx.x() - addrLeadKneeX;
            const double dHip  = s.leadHipPx.x()  - addrLeadHipX;
            // σ: the channel is leadSign·((kneeX − addrKneeX) − (hipX − addrHipX))·100/addrSpanPx.
            // Two independent x-coordinates differenced, so
            //     σ = sqrt(σ_knee² + σ_hip²) · 100/addrSpanPx
            // and leadSign, being ±1, cannot change a magnitude.
            //
            // ⚠ THE ADDRESS REFERENCE'S OWN NOISE IS OMITTED, and "negligible by √N" OVERCLAIMS it.
            // The reference is a component-wise MEDIAN over the window's frames, σ = 1.253·σ_kp/√N_eff
            // — and N_eff is NOT the frame count, because the smoother's residuals are autocorrelated
            // over roughly its own window (≈40 ms at the base sigmaJerk, per the derivation block in
            // pose_smoother.cpp), so a 250 ms address window buys about 250/40 ≈ 6 independent looks
            // rather than the ~30 frames it may contain. The offset enters in quadrature, so the σ
            // above is LOW by
            //     sqrt(1 + 1.571/N_eff) − 1
            // ≈ 3.9 % at N_eff = 20, ≈ 12 % at N_eff ≈ 6, ≈ 33 % at N_eff ≈ 2.
            //
            // WHERE THE MARGIN GOES: there is none to spare. It is single-digit percent only while the
            // window stays wide, and it degrades as 1/√N_eff the moment `lowerBody.addrWindowUs` is
            // narrowed, or the address region is posed sparsely — `addressStride` 15 is ≈100 ms at
            // 150 fps (pose_runner.h), already past the residual correlation time, so a short window
            // there can hold two or three independent looks and this term becomes a third of the
            // answer. It is left out because the reference is shared by three channels and its
            // correlation with each frame's own estimate is not characterised; it is the next term to
            // add, and a real one, not a rounding.
            //
            // The DENOMINATOR's noise is the smaller cousin of the same thing, and that one genuinely
            // is small: a multiplicative 1.253·sqrt(σ_lAnk² + σ_tAnk²)/(√N_eff·addrSpanPx) ≈ 0.7 % at
            // N_eff = 6 on §8's fixture.
            res.kneeDrift.push(s.t_us, res.leadSign * (dKnee - dHip) * toPct,
                               quad2(s.leadKneeSigPx, s.leadHipSigPx) * toPct);
        }
        if (s.hipsValid) {
            const double midX = 0.5 * (s.leadHipPx.x() + s.trailHipPx.x());
            const double midY = 0.5 * (s.leadHipPx.y() + s.trailHipPx.y());
            // σ: both channels are ±(mid − addrMid)·100/addrSpanPx with mid = 0.5·(lead + trail), so
            // the 0.5 carries straight through the derivative:
            //     σ = 0.5·sqrt(σ_lead² + σ_trail²) · 100/addrSpanPx.
            // Averaging two independent estimates is why the pelvis CENTRE is quieter than either
            // hip on its own, and it is the same factor the anatomy vocabulary's midpoints get.
            // ONE number for both channels because one is the x of that midpoint and the other is
            // the y of it, and the smoother's σ is per-axis (LowerBodyState).
            const double sigMidPct = 0.5 * quad2(s.leadHipSigPx, s.trailHipSigPx) * toPct;
            res.pelvisSway.push(s.t_us, res.leadSign * (midX - addrMidX) * toPct, sigMidPct);
            // Positive UP, so the address height minus the current one (image y grows downward).
            res.pelvisLift.push(s.t_us, (addrMidY - midY) * toPct, sigMidPct);
        }
        // ABSOLUTE, not address-referenced — see the header. Gated on the hip LINE rather than on
        // hipsValid: both hips can be perfectly confident and still not describe a line.
        if (s.hipLineValid) {
            // σ: see lineTiltSigmaDeg — sqrt(σ_lead² + σ_trail²) over the line's Euclidean length,
            // which is the exact first-order answer for an atan2 of a difference of two keypoints.
            res.hipTilt.push(s.t_us, lineTiltDeg(s.leadHipPx, s.trailHipPx),
                             lineTiltSigmaDeg(s.leadHipPx, s.trailHipPx,
                                              s.leadHipSigPx, s.trailHipSigPx));
        } else if (s.hipsValid) {
            // Confident hips, refused geometry: record the instant as GATED, not merely missing. The
            // resample may bridge a detector dropout; it must never bridge this.
            res.gatedHipLine.push_back(s.t_us);
        }
        // feetAlignment — the ankle line, absolute, in the same convention as the hip line.
        if (s.anklesValid) {
            // σ: the same line-tilt rule, on the ankle line and its own length. The stance line is
            // the LONGEST line this module measures a tilt on, so this is also the quietest angle
            // here — the lever arm is the denominator.
            res.feetAlign.push(s.t_us, lineTiltDeg(s.leadAnklePx, s.trailAnklePx),
                               lineTiltSigmaDeg(s.leadAnklePx, s.trailAnklePx,
                                                s.leadAnkleSigPx, s.trailAnkleSigPx));
        }
        // comOverLeadFoot — how far the pelvis centre sits from the lead ankle ALONG the stance
        // line. Unsigned: "further from the lead ankle" is the fault whether the golfer is still
        // back over the trail side or has fallen through, and a signed reading would grade those
        // two as opposites when they are the same finish.
        if (s.hipsValid && s.anklesValid) {
            const double ux = s.leadAnklePx.x() - s.trailAnklePx.x();
            const double uy = s.leadAnklePx.y() - s.trailAnklePx.y();
            const double ul = std::sqrt(ux * ux + uy * uy);
            if (ul > 1e-9) {
                const double midX = 0.5 * (s.leadHipPx.x() + s.trailHipPx.x());
                const double midY = 0.5 * (s.leadHipPx.y() + s.trailHipPx.y());
                const double along = ((midX - s.leadAnklePx.x()) * ux
                                      + (midY - s.leadAnklePx.y()) * uy) / ul;
                // ── σ: THE FULL FIRST-ORDER PROPAGATION, and its DOMINANT term is a rotation ──
                //
                // `along` = (M − A)·û, with M the hip centre, A the LEAD ankle, B the trail ankle and
                // û = (A − B)/L the unit stance vector. Every input is a keypoint coordinate and the
                // expression is smooth in all of them, so the honest thing is to differentiate the
                // WHOLE expression rather than add term-by-term guesses. That also settles the
                // correlation question outright: a keypoint that enters twice enters ONCE in its own
                // gradient, so there is nothing left to correlate.
                //
                //   ∂/∂M = û                        |·|² = 1        (M is 0.5 per hip ⇒ 0.25 each)
                //   ∂/∂A = −û + (h/L)·n̂             |·|² = 1 + (h/L)²
                //   ∂/∂B =      −(h/L)·n̂            |·|² = (h/L)²
                //
                // n̂ is the unit normal and h = (M − A)·n̂ is the hip centre's PERPENDICULAR offset from
                // the ankle line. The lead ankle enters TWICE — it is the origin the distance is
                // measured from AND an endpoint of the line it is measured along — and the two act
                // along PERPENDICULAR directions (û and n̂), so they add in quadrature with no cross
                // term. Nothing is assumed away there; it falls out.
                //
                // With an isotropic per-axis σ per keypoint the squared gradients weight the variances
                // directly:
                //   var = 0.25σ_lHip² + 0.25σ_tHip² + (1 + (h/L)²)σ_lAnk² + (h/L)²σ_tAnk²
                //   σ   = sqrt(var) · 100/addrSpanPx
                //
                // WHY THE ROTATION TERM IS THE WHOLE STORY: keypoint noise in either ankle ROTATES the
                // stance line by dφ ≈ σ/L, and a point h px off that line then moves h·dφ ALONG it. The
                // hips sit about TWO stance widths above the ankles, so h/L ≈ 2 and the two rotation
                // terms carry ~4× the reference-ankle term and ~16× each hip's. It is not a correction.
                //
                // ⚠ WHAT C11 PINNED, AND WHY IT UNDERSTATED. The contract gives
                // `sqrt(σ_mid² + σ_lAnk²)·100/addrSpanPx`, σ_mid = 0.5·sqrt(σ_lHip² + σ_tHip²) — the
                // (h/L) = 0 case, i.e. a hip centre lying ON the ankle line. On §8's fixture that reads
                // 1.22 % where the full form reads 3.08 %, a factor of 2.5 LOW. Design principle 3 does
                // not permit shipping the smaller number, so the full form is what ships.
                //
                // h/L, from the 2D cross product: |r × u| / L² with r = M − A. Exact, and no normal
                // vector has to be constructed.
                const double lever = std::abs((midX - s.leadAnklePx.x()) * uy
                                              - (midY - s.leadAnklePx.y()) * ux) / (ul * ul);
                const double lev2  = lever * lever;
                double sigCom = 0.0;
                if (s.leadHipSigPx > 0.0 && s.trailHipSigPx > 0.0
                    && s.leadAnkleSigPx > 0.0 && s.trailAnkleSigPx > 0.0) {
                    sigCom = std::sqrt(0.25 * (s.leadHipSigPx * s.leadHipSigPx
                                               + s.trailHipSigPx * s.trailHipSigPx)
                                       + (1.0 + lev2) * s.leadAnkleSigPx * s.leadAnkleSigPx
                                       + lev2 * s.trailAnkleSigPx * s.trailAnkleSigPx) * toPct;
                }
                // The reported value is |along|, and |·| is not differentiable at 0 — but the hip
                // centre sits the better part of a stance width from the lead ankle on every real
                // swing, so away from that point the σ of |x| IS the σ of x.
                res.comOverLead.push(s.t_us, std::abs(along) * toPct, sigCom);

                // plumbBobDistance — the SIGNED twin of the reading above, taken about the stance
                // CENTRE rather than the lead ankle, and in inches rather than a fraction of the
                // stance. `u` runs trail ankle -> lead ankle, so the projection is lead-positive by
                // construction and needs no leadSign: a mirrored camera cannot invert it.
                //
                // Inches only, and only when the ball ruler resolved. See the header: the figures
                // this is read against are absolute inches a coach quotes out loud, and a metric
                // whose unit changes per swing cannot carry a norm.
                if (mmPerPx > 0.0) {
                    const double cx = 0.5 * (s.leadAnklePx.x() + s.trailAnklePx.x());
                    const double cy = 0.5 * (s.leadAnklePx.y() + s.trailAnklePx.y());
                    const double off = ((midX - cx) * ux + (midY - cy) * uy) / ul;
                    // ── σ: the same full propagation, with the stance CENTRE as the reference ──
                    //
                    // `off` = (M − C)·û with C = 0.5(A + B). C is a function of BOTH ankles, so each of
                    // them now carries a −0.5û reference term ALONGSIDE its ±(h/L)n̂ rotation term:
                    //
                    //   ∂/∂M = û                     |·|² = 1        (0.25 per hip, as above)
                    //   ∂/∂A = −0.5û + (h/L)·n̂        |·|² = 0.25 + (h/L)²
                    //   ∂/∂B = −0.5û − (h/L)·n̂        |·|² = 0.25 + (h/L)²
                    //   var  = 0.25σ_lHip² + 0.25σ_tHip² + (0.25 + (h/L)²)(σ_lAnk² + σ_tAnk²)
                    //   σ    = sqrt(var) · mmPerPx / 25.4
                    //
                    // h is the SAME perpendicular offset comOverLeadFoot uses: C lies on the ankle
                    // line, so subtracting it or the lead ankle leaves the same normal component. The
                    // reference and rotation terms again act along perpendicular directions and add in
                    // quadrature — the shared ankle σ needs no covariance term because each ankle was
                    // differentiated once, as a whole.
                    //
                    // ⚠ WHAT C11 PINNED, AND WHY IT UNDERSTATED. The contract gives
                    // `0.5·sqrt(σ_lHip² + σ_tHip²)·mmPerPx/25.4` — the hip centre alone, dropping the
                    // stance centre's own noise AND the rotation. On §8's fixture that reads 0.111 in
                    // where the full form reads 0.472 in, a factor of 4.2 LOW.
                    //
                    // Still NOT in here, and deliberately: the ball ruler's own scale uncertainty. It
                    // is calibrated at the ball's ground-plane depth and read at hip height (see the
                    // header), which is a BIAS, and a bias does not belong in a 1σ noise figure —
                    // widening σ would not make a systematically wrong inch reading right.
                    double sigPb = 0.0;
                    if (s.leadHipSigPx > 0.0 && s.trailHipSigPx > 0.0
                        && s.leadAnkleSigPx > 0.0 && s.trailAnkleSigPx > 0.0) {
                        sigPb = std::sqrt(0.25 * (s.leadHipSigPx * s.leadHipSigPx
                                                  + s.trailHipSigPx * s.trailHipSigPx)
                                          + (0.25 + lev2)
                                                * (s.leadAnkleSigPx * s.leadAnkleSigPx
                                                   + s.trailAnkleSigPx * s.trailAnkleSigPx))
                                * mmPerPx / kMmPerInch;
                    }
                    res.plumbBob.push(s.t_us, off * mmPerPx / kMmPerInch, sigPb);
                }
            }
        }
    }

    res.valid = !res.kneeDrift.t_us.empty() || !res.pelvisSway.t_us.empty();
    return res;
}

std::vector<MetricSeries> buildLowerBodySeries(const LowerBodyResult &res,
                                               const std::vector<PhaseEvent> &phases)
{
    std::vector<MetricSeries> out;
    if (!res.valid || res.states.empty())
        return out;

    // Full per-frame grid (every input frame, time order) — each channel resamples onto it so a
    // low-confidence gap coasts (bridged) rather than dropping out.
    std::vector<int64_t> grid;
    grid.reserve(res.states.size());
    for (const LowerBodyState &s : res.states)
        grid.push_back(s.t_us);

    // `gated` = the instants THIS channel's geometry was refused at (ascending, empty for a channel
    // with no gate). Distinct from the instants it merely lacks — see LowerBodyResult::gatedHipLine.
    // `p1toP7Domain` = this channel is an ADDRESS→IMPACT quantity (design §5.1's domain table), so
    // everything PAST IMPACT is marked invalid rather than reduced. The pre-Address head stays valid
    // — a still golfer referenced to address is a real reading of address posture, and it is the only
    // evidence the still-address gate window has. See applyPhaseDomainMask for the whole argument.
    const auto pushSeries = [&](const LowerBodyChannel &ch, const QString &key, const QString &label,
                                const QString &unit, bool p1toP7Domain = false,
                                const std::vector<Phase> &at = { Phase::Address, Phase::Top,
                                                                 Phase::Impact },
                                const std::vector<int64_t> &gated = kNoneGated) {
        if (ch.t_us.empty())
            return;
        std::vector<double> vals(grid.size());
        for (size_t i = 0; i < grid.size(); ++i)
            vals[i] = interpChannel(ch.t_us, ch.value, grid[i]);
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        m.t_us  = grid;
        m.value = vals;
        // WHICH OF THOSE VALUES WE ACTUALLY MEASURED. The resample still fills every grid sample —
        // the renderer wants a continuous curve and a hole invites "did it crash" — but a sample
        // bridged across more than res.maxBridgeUs of absence is a straight line we drew ourselves,
        // and the mask is what stops PEAK, PK RATE and the diagnostics corridors from grading it.
        // EMPTY when every sample is a measurement, which is the common case — and on such a swing no
        // `valid` array is emitted at all, so nothing about it changed.
        //
        // ⚠ THE >= 0 GUARD IS THE OFF-SWITCH, not defensive noise. channel.maxBridgeUs < 0 means "do
        // not mask" (buildChannelSeries documents the same sentinel), and calling the mask with a
        // negative budget would fail `d <= allowance` on every sample INCLUDING the measured ones —
        // an all-zeros mask, six metrics silently ungraded, from the knob that was supposed to
        // restore the old behaviour.
        //
        // A GATED instant is 0 whatever the budget says — the geometry was seen and refused, so there
        // is nothing to hold across. Only a CONFIDENCE hole gets the budget. Conflating the two is
        // what let a 10-frame gated run of hipLineTilt at 7 ms spacing come back flagged as measured.
        if (res.maxBridgeUs >= 0)
            m.valid = channelValidityMask(grid, ch.t_us, res.maxBridgeUs, res.bridgeSpacingFactor,
                                          gated);

        // THE PHASE DOMAIN'S TAIL, on the same mask and for the same reason: past impact the pelvis
        // has turned, so the frontal projection of a lateral quantity is rotation and not the
        // quantity. applyPhaseDomainMask carries the argument, the reason it cannot live in a reducer,
        // and why the HEAD is left open.
        //
        // Behind the SAME off-switch as the bridge mask deliberately: channel.maxBridgeUs < 0 means
        // "emit no validity mask at all" for a parity run, and a knob that restored half of it would
        // not restore the old bytes, which is the only thing it is for.
        if (res.maxBridgeUs >= 0 && p1toP7Domain)
            applyPhaseDomainMask(m, phases);

        // ── The series' σ, LAST, because it is a median over what survived the masks above ────
        //
        // Both masks have now run, so `m.valid` is final and medianSigmaOverValid can skip exactly
        // the frames no reducer will read. Set only when at least one frame contributed: an absent
        // MetricSeries::sigma means "not characterised" and a 0 would mean "measured perfectly",
        // which is never true — the same discipline body_rotation.cpp's IMU tier follows when it
        // leaves sigma unset rather than claiming a budget it never propagated.
        //
        // ⚠ σ NEVER TOUCHES value[] OR phaseSamples. It is read off a parallel track that the value
        // pass filled and it writes one optional field; a swing whose track has no `smoothedAux`
        // serialises exactly as it did before this existed.
        //
        // ⚠ BEHIND THE SAME OFF-SWITCH AS BOTH MASKS, and that is not tidiness. `channel.maxBridgeUs`
        // < 0 means "emit no validity mask at all", and its ONLY purpose is to restore the pre-mask
        // bytes for a parity run. A σ written with no mask would be a median over the gated and
        // post-Impact frames too — a DIFFERENT number from the masked one, on a NEW key, in the one run
        // whose whole job is to reproduce the old bytes. Half-restoring is not restoring, so σ goes
        // with them: negative budget ⇒ no mask, no domain marking, no σ.
        if (res.maxBridgeUs >= 0) {
            if (const std::optional<double> sg = medianSigmaOverValid(m, ch.t_us, &ch.sigma))
                m.sigma = *sg;
        }

        // Address / Top / Impact by default, the same three every other frontal-plane channel
        // samples. Top is P4 — the reading the knee-drift and hip-tilt corridors are both keyed on.
        // The caller may ask for more: comOverLeadFoot is read at the FINISH, which nothing sampled
        // before it existed, and hipLineTilt / plumbBobDistance are read across the whole P1–P7
        // ladder. The four channels that predate those keep their original list deliberately, so the
        // SET of phases they sample is unchanged.
        //
        // ⚠ That is no longer a byte-identical promise, and the sentence that used to claim one has
        // been deleted rather than softened. Since the validity mask landed, ANY channel — including
        // those four — loses a phaseSample whose instant the resample had to bridge, so a confidence
        // dropout wider than the bridge allowance can remove a P1 or P7 reading that used to be
        // emitted. Only the CORPUS GATE can say which swings that touches (plan §1.4: value[]
        // equality plus an accounting of every removed sample); no comment here can.
        //
        // AN UNSEGMENTED PHASE PRODUCES NO SAMPLE. It used to coast to the grid front, which was
        // invisible while the list was Address/Top/Impact and is a fabricated reading the moment
        // P2/P3/P5/P6 are asked for — those come off the P-position bridge and are genuinely
        // missing on plenty of swings.
        //
        // AN INVALID INSTANT PRODUCES NO SAMPLE EITHER, and for the same reason: measure_sample.cpp
        // falls back to the LABELLED sample where the curve has nothing, so a bridged value wearing
        // a P4 label would be graded against the P4 corridor. That is how a +88° shoulder plane at
        // the top became a graded reading, and the hip line has the identical degeneracy.
        for (const Phase p : at) {
            const std::optional<int64_t> t = phaseTimeOpt(phases, p);
            if (!t)
                continue;
            const int idx = nearestIndex(grid, *t);
            if (!m.valid.empty() && m.valid[size_t(idx)] == 0u)
                continue;
            m.phaseSamples.push_back({ p, grid[idx], vals[idx], QString() });
        }
        out.push_back(std::move(m));
    };

    // P1..P7 in ladder order — NOT enum order, which is append-only and unrelated (see the Phase
    // enum in swing_analysis.h). The two channels a coach reads as a progression rather than at one
    // instant take this list; the rest keep Address/Top/Impact so their serialized phaseSamples
    // stay byte-identical.
    static const std::vector<Phase> kP1toP7 = {
        Phase::Address,             // P1
        Phase::ShaftParallelBack,   // P2
        Phase::MidBackswing,        // P3
        Phase::Top,                 // P4
        Phase::ArmParallelDown,     // P5
        Phase::Delivery,            // P6
        Phase::Impact,              // P7
    };

    const QString pct = QStringLiteral("% stance width");
    pushSeries(res.kneeDrift,  QStringLiteral("leadKneeDrift"),
               QStringLiteral("Lead knee drift"), pct, /*p1toP7Domain=*/true);
    pushSeries(res.pelvisSway, QStringLiteral("pelvisSway"),
               QStringLiteral("Pelvis sway"), pct, /*p1toP7Domain=*/true);
    pushSeries(res.pelvisLift, QStringLiteral("pelvisLift"),
               QStringLiteral("Pelvis lift"), pct, /*p1toP7Domain=*/true);
    pushSeries(res.hipTilt,    QStringLiteral("hipLineTilt"),
               QStringLiteral("Hip line tilt"), QStringLiteral("°"), /*p1toP7Domain=*/true,
               kP1toP7, res.gatedHipLine);
    pushSeries(res.feetAlign,  QStringLiteral("feetAlignment"),
               QStringLiteral("Feet alignment"), QStringLiteral("°"));
    // Whole-swing, NOT narrowed: comOverLeadFoot is READ at the finish and is a distance along the
    // stance line, which survives the turn (design §5.1's table says so explicitly).
    pushSeries(res.comOverLead, QStringLiteral("comOverLeadFoot"),
               QStringLiteral("Balance over the lead foot"), pct, /*p1toP7Domain=*/false,
               { Phase::Address, Phase::Top, Phase::Impact, Phase::Finish });
    pushSeries(res.plumbBob,   QStringLiteral("plumbBobDistance"),
               QStringLiteral("Plumb bob"), QStringLiteral("in"), /*p1toP7Domain=*/true, kP1toP7);
    return out;
}

} // namespace pinpoint::analysis
