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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace pinpoint::analysis {
namespace {

// COCO-17 body indices, shared by both layouts (anatomy_vocabulary.h kp::).
constexpr int kLHip = 11, kRHip = 12, kLKnee = 13, kRKnee = 14, kLAnkle = 15, kRAnkle = 16;

constexpr double kRadToDeg = 57.29577951308232;

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

// Linear interp of an ascending sparse channel at t (hold at ends, bridge gaps). Same contract as
// head_track / foot_metrics: never NaN.
double interpChannel(const std::vector<int64_t> &xs, const std::vector<double> &ys, int64_t x)
{
    if (xs.empty()) return 0.0;                 // guarded upstream (channel non-empty)
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back())  return ys.back();
    const auto it = std::lower_bound(xs.begin(), xs.end(), x);
    const size_t hi = size_t(it - xs.begin());
    const size_t lo = hi - 1;
    const int64_t span = xs[hi] - xs[lo];
    if (span <= 0) return ys[lo];               // coincident samples (defensive)
    const double f = double(x - xs[lo]) / double(span);
    return ys[lo] + (ys[hi] - ys[lo]) * f;
}

int64_t phaseTime(const std::vector<PhaseEvent> &phases, Phase p, int64_t fallback)
{
    for (const PhaseEvent &e : phases)
        if (e.phase == p) return e.t_us;
    return fallback;
}

int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (t <= grid.front()) return 0;
    if (t >= grid.back())  return int(grid.size()) - 1;
    const auto it = std::lower_bound(grid.begin(), grid.end(), t);
    const int hi = int(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[lo] <= grid[hi] - t) ? lo : hi;
}

} // namespace

LowerBodyResult trackLowerBody(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                               int64_t addressUs, const LowerBodyConfig &cfg)
{
    LowerBodyResult res;
    res.frameW = frameW;
    res.frameH = frameH;
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

    std::vector<double> lhx, lhy, thx, thy, lkx, spans, leadAnkX, trailAnkX;
    for (const LowerBodyState *s : ref) {
        lhx.push_back(s->leadHipPx.x());   lhy.push_back(s->leadHipPx.y());
        thx.push_back(s->trailHipPx.x());  thy.push_back(s->trailHipPx.y());
        lkx.push_back(s->leadKneePx.x());
        spans.push_back(std::abs(s->trailAnklePx.x() - s->leadAnklePx.x()));
        leadAnkX.push_back(s->leadAnklePx.x());
        trailAnkX.push_back(s->trailAnklePx.x());
    }

    res.addrLeadHipPx  = QPointF(medianOf(lhx), medianOf(lhy));
    res.addrTrailHipPx = QPointF(medianOf(thx), medianOf(thy));
    res.addrLeadKneePx = QPointF(medianOf(lkx), 0.0);
    res.addrSpanPx     = medianOf(spans);

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

    for (const LowerBodyState &s : res.states) {
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
            // ABSOLUTE, not address-referenced — see the header.
            res.hipTilt.t_us.push_back(s.t_us);
            res.hipTilt.value.push_back(lineTiltDeg(s.leadHipPx, s.trailHipPx));
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

    const auto pushSeries = [&](const LowerBodyChannel &ch, const QString &key, const QString &label,
                                const QString &unit,
                                std::initializer_list<Phase> at = { Phase::Address, Phase::Top,
                                                                    Phase::Impact }) {
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
        // Address / Top / Impact by default, the same three every other frontal-plane channel
        // samples. Top is P4 — the reading the knee-drift and hip-tilt corridors are both keyed on.
        // The caller may ask for more: comOverLeadFoot is read at the FINISH, which nothing sampled
        // before it existed. The four original channels keep the original list deliberately, so
        // their serialized phaseSamples stay byte-identical and no corpus gate has to be re-run to
        // prove this change was additive.
        for (const Phase p : at) {
            const int idx = nearestIndex(grid, phaseTime(phases, p, grid.front()));
            m.phaseSamples.push_back({ p, grid[idx], vals[idx], QString() });
        }
        out.push_back(std::move(m));
    };

    const QString pct = QStringLiteral("% stance width");
    pushSeries(res.kneeDrift,  QStringLiteral("leadKneeDrift"),
               QStringLiteral("Lead knee drift"), pct);
    pushSeries(res.pelvisSway, QStringLiteral("pelvisSway"),
               QStringLiteral("Pelvis sway"), pct);
    pushSeries(res.pelvisLift, QStringLiteral("pelvisLift"),
               QStringLiteral("Pelvis lift"), pct);
    pushSeries(res.hipTilt,    QStringLiteral("hipLineTilt"),
               QStringLiteral("Hip line tilt"), QStringLiteral("°"));
    pushSeries(res.feetAlign,  QStringLiteral("feetAlignment"),
               QStringLiteral("Feet alignment"), QStringLiteral("°"));
    pushSeries(res.comOverLead, QStringLiteral("comOverLeadFoot"),
               QStringLiteral("Balance over the lead foot"), pct,
               { Phase::Address, Phase::Top, Phase::Impact, Phase::Finish });
    return out;
}

} // namespace pinpoint::analysis
