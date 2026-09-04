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

// Per-frame lower-body state from hips / knees / ankles.
LowerBodyState computeState(const PoseFrame2D &f, int frameW, int frameH, double confMin,
                            bool leadIsLeft)
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

    res.states.reserve(frames.size());
    for (const PoseFrame2D &f : frames)
        res.states.push_back(computeState(f, frameW, frameH, cfg.confMin, leadIsLeft));

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
            res.kneeDrift.t_us.push_back(s.t_us);
            res.kneeDrift.value.push_back(res.leadSign * (dKnee - dHip) * toPct);
        }
        if (s.hipsValid) {
            const double midX = 0.5 * (s.leadHipPx.x() + s.trailHipPx.x());
            const double midY = 0.5 * (s.leadHipPx.y() + s.trailHipPx.y());
            res.pelvisSway.t_us.push_back(s.t_us);
            res.pelvisSway.value.push_back(res.leadSign * (midX - addrMidX) * toPct);
            // Positive UP, so the address height minus the current one (image y grows downward).
            res.pelvisLift.t_us.push_back(s.t_us);
            res.pelvisLift.value.push_back((addrMidY - midY) * toPct);
        }
        // ABSOLUTE, not address-referenced — see the header. Gated on the hip LINE rather than on
        // hipsValid: both hips can be perfectly confident and still not describe a line.
        if (s.hipLineValid) {
            res.hipTilt.t_us.push_back(s.t_us);
            res.hipTilt.value.push_back(lineTiltDeg(s.leadHipPx, s.trailHipPx));
        } else if (s.hipsValid) {
            // Confident hips, refused geometry: record the instant as GATED, not merely missing. The
            // resample may bridge a detector dropout; it must never bridge this.
            res.gatedHipLine.push_back(s.t_us);
        }
        // feetAlignment — the ankle line, absolute, in the same convention as the hip line.
        if (s.anklesValid) {
            res.feetAlign.t_us.push_back(s.t_us);
            res.feetAlign.value.push_back(lineTiltDeg(s.leadAnklePx, s.trailAnklePx));
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
                res.comOverLead.t_us.push_back(s.t_us);
                res.comOverLead.value.push_back(std::abs(along) * toPct);

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
                    res.plumbBob.t_us.push_back(s.t_us);
                    res.plumbBob.value.push_back(off * mmPerPx / kMmPerInch);
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
