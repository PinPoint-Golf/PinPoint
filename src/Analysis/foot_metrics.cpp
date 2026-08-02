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

#include "foot_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pinpoint::analysis {
namespace {

// COCO-WholeBody foot keypoints (wholebody_pose_design.md §1.1): 17 L-bigtoe,
// 18 L-smalltoe, 19 L-heel, 20 R-bigtoe, 21 R-smalltoe, 22 R-heel. Only
// bigtoe + heel are used here (smalltoe is not needed for these quantities).
constexpr int kLBigToe = 17, kLHeel = 19;
constexpr int kRBigToe = 20, kRHeel = 22;

// COCO-17 body: 5 L-shoulder, 6 R-shoulder. Handedness-invariant — the distance between them is
// the same measurement whichever side leads.
constexpr int kLShoulder = 5, kRShoulder = 6;

// Euclidean pixel distance between two already-de-normalized px points.
double distPx(const QPointF &a, const QPointF &b)
{
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    return std::sqrt(dx * dx + dy * dy);
}

// Angle (deg) of the image-plane vector a→b vs the horizontal (y-down convention).
double angleDeg(const QPointF &a, const QPointF &b)
{
    return std::atan2(b.y() - a.y(), b.x() - a.x()) * 57.29577951308232;
}

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Shoulder-to-shoulder distance (px) on the pose frame nearest `t_us`, or 0 when either shoulder
// is below confidence. Measured in the same px space as the feet, so the two are directly
// comparable; 0 rather than a guess, because a fabricated denominator would silently rescale every
// stance reading rather than making the gap visible.
double shoulderWidthPxAt(const PoseTrack2D &pose, int64_t t_us, int frameW, int frameH,
                         double confMin)
{
    const PoseFrame2D *best = nullptr;
    int64_t bestDt = 0;
    for (const PoseFrame2D &f : pose.frames) {
        const int64_t dt = f.t_us > t_us ? f.t_us - t_us : t_us - f.t_us;
        if (best == nullptr || dt < bestDt) { best = &f; bestDt = dt; }
    }
    if (best == nullptr
        || int(best->kp.size()) <= kRShoulder || int(best->conf.size()) <= kRShoulder)
        return 0.0;
    if (best->conf[kLShoulder] < confMin || best->conf[kRShoulder] < confMin)
        return 0.0;

    const QPointF l(best->kp[kLShoulder].x() * frameW, best->kp[kLShoulder].y() * frameH);
    const QPointF r(best->kp[kRShoulder].x() * frameW, best->kp[kRShoulder].y() * frameH);
    return distPx(l, r);
}

// Per-frame foot state from the heel + bigtoe keypoints of each foot.
FootState computeState(const PoseFrame2D &f, int frameW, int frameH, double confMin, bool leadIsLeft)
{
    FootState s;
    s.t_us = f.t_us;

    const int leadHeelIdx  = leadIsLeft ? kLHeel  : kRHeel;
    const int leadToeIdx   = leadIsLeft ? kLBigToe : kRBigToe;
    const int trailHeelIdx = leadIsLeft ? kRHeel  : kLHeel;
    const int trailToeIdx  = leadIsLeft ? kRBigToe : kLBigToe;

    const auto pxOf = [&](int idx) {
        return QPointF(f.kp[idx].x() * frameW, f.kp[idx].y() * frameH);
    };

    if (f.conf[leadHeelIdx] >= confMin && f.conf[leadToeIdx] >= confMin) {
        s.leadValid  = true;
        s.leadHeelPx = pxOf(leadHeelIdx);
        s.leadToePx  = pxOf(leadToeIdx);
        s.leadConf   = 0.5f * (f.conf[leadHeelIdx] + f.conf[leadToeIdx]);
    }
    if (f.conf[trailHeelIdx] >= confMin && f.conf[trailToeIdx] >= confMin) {
        s.trailValid  = true;
        s.trailHeelPx = pxOf(trailHeelIdx);
        s.trailToePx  = pxOf(trailToeIdx);
        s.trailConf   = 0.5f * (f.conf[trailHeelIdx] + f.conf[trailToeIdx]);
    }
    return s;
}

// Linear interp of an ascending sparse channel at t (hold at ends, bridge gaps).
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

FootMetricsResult trackFeet(const PoseTrack2D &pose, int frameW, int frameH, bool leadIsLeft,
                            int64_t addressUs, const FootMetricsConfig &cfg)
{
    FootMetricsResult res;
    res.frameW = frameW;
    res.frameH = frameH;
    if (frameW <= 0 || frameH <= 0)
        return res;

    // The smoothed companion track (parallel t_us, same normalized kp) is
    // preferred, exactly like head_track — falls back to raw frames on swings
    // analysed before the smoother existed.
    const std::vector<PoseFrame2D> &frames =
        pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frames.empty())
        return res;

    res.states.reserve(frames.size());
    for (const PoseFrame2D &f : frames)
        res.states.push_back(computeState(f, frameW, frameH, cfg.confMin, leadIsLeft));

    // Robust (median) address reference over the lead-valid frames: prefer the
    // confident frames inside the Address-event window; if none (or no
    // event), fall back to the first N lead-valid frames — same shape as
    // head_track's reference resolution.
    std::vector<const FootState *> ref;
    if (addressUs >= 0) {
        for (const FootState &s : res.states)
            if (s.leadValid && std::llabs(s.t_us - addressUs) <= cfg.addrWindowUs)
                ref.push_back(&s);
    }
    if (ref.empty()) {
        for (const FootState &s : res.states) {
            if (!s.leadValid) continue;
            ref.push_back(&s);
            if (int(ref.size()) >= cfg.addrMinFrames) break;
        }
    }
    if (ref.empty())
        return res;   // no lead foot anywhere (e.g. a legacy 17-kp track) — leave valid == false

    std::vector<double> widths, leadFlares, trailFlares, toeLines, addrElev;
    std::vector<double> leadHeelX, leadHeelY, trailHeelX, trailHeelY;
    std::vector<double> shoulderWidths;
    for (const FootState *s : ref) {
        // Shoulder width over the SAME reference frames the stance is measured on, so the ratio
        // shares its denominator by construction rather than by luck — the same reason the heel
        // pair is captured here. It is what makes stance width a body-relative reading instead of
        // a reading about how tall the golfer is: a 190 cm player and a 160 cm player take
        // genuinely different stances and neither is wrong, so millimetres cannot carry a
        // population norm. It is also how the stance is actually described out loud.
        if (const double sw = shoulderWidthPxAt(pose, s->t_us, frameW, frameH, cfg.confMin);
            sw > 0.0)
            shoulderWidths.push_back(sw);

        // stanceWidth / toeLine need BOTH feet valid on this reference frame.
        if (s->leadValid && s->trailValid) {
            widths.push_back(distPx(s->leadHeelPx, s->trailHeelPx));
            toeLines.push_back(angleDeg(s->leadToePx, s->trailToePx));
            // The heel pair itself, from the SAME frames as `widths` — so
            // anything projected onto this line shares stanceWidth's denominator.
            leadHeelX.push_back(s->leadHeelPx.x());
            leadHeelY.push_back(s->leadHeelPx.y());
            trailHeelX.push_back(s->trailHeelPx.x());
            trailHeelY.push_back(s->trailHeelPx.y());
        }
        leadFlares.push_back(angleDeg(s->leadHeelPx, s->leadToePx));   // s->leadValid guaranteed by ref
        if (s->trailValid)
            trailFlares.push_back(angleDeg(s->trailHeelPx, s->trailToePx));
        addrElev.push_back(s->leadToePx.y() - s->leadHeelPx.y());
    }

    res.setup.stanceWidthValid = !widths.empty();
    if (res.setup.stanceWidthValid)
        res.setup.stanceWidthXFrame = medianOf(widths) / frameW;   // isotropic ×frame (single ref dim)

    res.setup.shoulderWidthValid = !shoulderWidths.empty();
    if (res.setup.shoulderWidthValid)
        res.setup.shoulderWidthPx = medianOf(shoulderWidths);
    // Component-wise median, matching every other robust reference here — it is
    // order-independent, so one bad reference frame cannot drag the heel line.
    res.setup.heelsValid = !leadHeelX.empty();
    if (res.setup.heelsValid) {
        res.setup.leadHeelPxAddr  = QPointF(medianOf(leadHeelX),  medianOf(leadHeelY));
        res.setup.trailHeelPxAddr = QPointF(medianOf(trailHeelX), medianOf(trailHeelY));
    }
    res.setup.leadFlareValid = !leadFlares.empty();
    if (res.setup.leadFlareValid)
        res.setup.leadFlareDeg = medianOf(leadFlares);
    res.setup.trailFlareValid = !trailFlares.empty();
    if (res.setup.trailFlareValid)
        res.setup.trailFlareDeg = medianOf(trailFlares);
    res.setup.toeLineValid = !toeLines.empty();
    if (res.setup.toeLineValid)
        res.setup.toeLineDeg = medianOf(toeLines);

    const double addrElevPx = medianOf(addrElev);
    res.valid = true;

    // Address-referenced lead-heel-lift channel: elevDiff(t) − elevDiff(addr),
    // ×frame units (single reference dimension, matches head_track's sway/lift).
    for (const FootState &s : res.states) {
        if (!s.leadValid) continue;
        const double elevPx = s.leadToePx.y() - s.leadHeelPx.y();
        res.liftTUs.push_back(s.t_us);
        res.liftValue.push_back((elevPx - addrElevPx) / frameW);
    }
    return res;
}

std::vector<MetricSeries> buildFootSeries(const FootMetricsResult &res,
                                          const std::vector<PhaseEvent> &phases,
                                          double mmPerPx)
{
    std::vector<MetricSeries> out;
    if (!res.valid || res.states.empty())
        return out;

    // Full per-frame grid — the lead-heel-lift curve resamples onto it (gaps
    // bridged, never NaN); the setup scalars' single phaseSample uses its front
    // as the ultimate fallback "address" instant.
    std::vector<int64_t> grid;
    grid.reserve(res.states.size());
    for (const FootState &s : res.states) grid.push_back(s.t_us);

    const int64_t addrT = phaseTime(phases, Phase::Address, grid.front());

    // Setup scalars — see foot_metrics.h's header note on this representation:
    // an empty t_us/value curve + one Address phaseSample. swing_doc.cpp's
    // generic writer and every reader already loop over both arrays
    // independently with no non-empty-curve assumption, so this needs no
    // reader/writer change (docs/reference/swing_json_schema.md 2026-07-13).
    const auto pushScalar = [&](bool ok, const QString &key, const QString &label,
                                const QString &unit, double value) {
        if (!ok) return;
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        m.phaseSamples.push_back({ Phase::Address, addrT, value, QString() });
        out.push_back(std::move(m));
    };
    // Stance width as a PERCENTAGE OF SHOULDER WIDTH.
    //
    // The unit is invariant, which is the point. It used to switch between "mm" and "×frame" at
    // runtime depending on whether the ball-diameter ruler resolved, and a metric whose unit
    // changes per swing cannot carry a norm — the norm declares one unit and the loader rejects a
    // mismatch. So when the shoulders are not resolvable the metric is ABSENT rather than present
    // in some other unit: unavailable is a fact worth reporting, a silently different scale is not.
    //
    // It is also the better reading. Millimetres are a norm on the golfer's height, whereas
    // "a shoulder-width stance" is both body-relative and exactly how the stance is described.
    // stanceWidthXFrame is px/frameW, so × frameW recovers px.
    const bool   swPctOk = res.setup.stanceWidthValid && res.setup.shoulderWidthValid
                           && res.setup.shoulderWidthPx > 0.0;
    const double swPct   = swPctOk ? (res.setup.stanceWidthXFrame * res.frameW)
                                         / res.setup.shoulderWidthPx * 100.0
                                   : 0.0;
    pushScalar(swPctOk, QStringLiteral("stanceWidth"),
               QStringLiteral("Stance width"), QStringLiteral("% shoulder width"), swPct);

    // The millimetre reading is kept as its own metric rather than dropped: it is real, it comes
    // from the ball-diameter ruler, and a coach asking "how wide, in the room?" is asking a
    // different question from "how wide, for this golfer?". Invariant unit here too — emitted only
    // when the ruler actually resolved.
    pushScalar(res.setup.stanceWidthValid && mmPerPx > 0.0, QStringLiteral("stanceWidthMm"),
               QStringLiteral("Stance width (absolute)"), QStringLiteral("mm"),
               res.setup.stanceWidthXFrame * res.frameW * mmPerPx);
    pushScalar(res.setup.leadFlareValid, QStringLiteral("leadFootFlare"),
              QStringLiteral("Lead foot flare"), QStringLiteral("°"), res.setup.leadFlareDeg);
    pushScalar(res.setup.trailFlareValid, QStringLiteral("trailFootFlare"),
              QStringLiteral("Trail foot flare"), QStringLiteral("°"), res.setup.trailFlareDeg);
    pushScalar(res.setup.toeLineValid, QStringLiteral("toeLineAngle"),
              QStringLiteral("Toe-line angle"), QStringLiteral("°"), res.setup.toeLineDeg);

    // leadHeelLift — full per-frame curve, resampled + gap-bridged exactly like
    // head_track's sway/lift (never NaN).
    //
    // CENTIMETRES, and emitted ONLY when the ball-diameter ruler resolved. This is the same
    // invariant-unit rule stanceWidth states above, and it is not a style choice: a metric whose
    // unit changes per swing cannot carry a norm, because the norm declares one unit and grading
    // compares the numbers without ever looking at it. This channel shipped as "×frame" against a
    // measure and a corridor that both said "cm", so a heel lift of ~0.05 was graded against a
    // 2 cm ceiling and `sig_excessiveHeelLift` could not fire on any swing ever recorded. Nothing
    // caught it: `normUnitMismatch` compares the norm against the MEASURE, and those two agreed.
    //
    // The ruler is the right one for this quantity — it is taken at the ball's ground-plane depth,
    // which face-on is essentially the feet's depth, so it is the same plane the heel moves in.
    // Without it the reading is ABSENT rather than present in some other scale: unavailable is a
    // fact the app already renders honestly, a silently different scale is a wrong answer wearing
    // a right answer's clothes.
    if (!res.liftTUs.empty() && mmPerPx > 0.0) {
        const double gain = double(res.frameW) * mmPerPx / 10.0;   // ×frame → px → mm → cm
        std::vector<double> vals(grid.size());
        for (size_t i = 0; i < grid.size(); ++i)
            vals[i] = interpChannel(res.liftTUs, res.liftValue, grid[i]) * gain;

        MetricSeries m;
        m.key   = QStringLiteral("leadHeelLift");
        m.label = QStringLiteral("Lead heel lift");
        m.unit  = QStringLiteral("cm");
        m.t_us  = grid;
        m.value = vals;
        for (const Phase p : { Phase::Address, Phase::Top, Phase::Impact }) {
            const int idx = nearestIndex(grid, phaseTime(phases, p, grid.front()));
            m.phaseSamples.push_back({ p, grid[idx], vals[idx], QString() });
        }
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace pinpoint::analysis
