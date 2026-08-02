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

#include "head_track.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace pinpoint::analysis {
namespace {

// COCO body head keypoints (0–4 stay valid verbatim under the WholeBody widen).
constexpr int kNose = 0, kLEye = 1, kREye = 2, kLEar = 3, kREar = 4;
constexpr int kChin = kFaceFirstKp + 8;   // 68-pt face contour tip (23+8 = 31)

// Euclidean pixel distance between two keypoints of a frame.
double distPx(const PoseFrame2D &f, int a, int b, int frameW, int frameH)
{
    const double dx = (f.kp[b].x() - f.kp[a].x()) * frameW;
    const double dy = (f.kp[b].y() - f.kp[a].y()) * frameH;
    return std::sqrt(dx * dx + dy * dy);
}

// Angle (deg) of the image-plane vector a→b vs the horizontal (y-down convention).
double lineAngleDeg(const PoseFrame2D &f, int a, int b, int frameW, int frameH)
{
    const double dx = (f.kp[b].x() - f.kp[a].x()) * frameW;
    const double dy = (f.kp[b].y() - f.kp[a].y()) * frameH;
    return std::atan2(dy, dx) * 57.29577951308232;
}

double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Per-frame head state from the body head keypoints (+ optional chin).
HeadState computeState(const PoseFrame2D &f, int frameW, int frameH,
                       const HeadTrackConfig &cfg)
{
    HeadState s;
    s.t_us = f.t_us;
    const double gate = cfg.confMin;

    double sx = 0.0, sy = 0.0, sw = 0.0, sc = 0.0;
    int n = 0;
    const auto addPt = [&](int idx, double wmul) {
        const float c = f.conf[idx];
        if (c < gate) return;
        const double w = double(c) * wmul;
        sx += f.kp[idx].x() * frameW * w;
        sy += f.kp[idx].y() * frameH * w;
        sw += w;
        sc += c;
        ++n;
    };
    for (const int idx : { kNose, kLEye, kREye, kLEar, kREar }) addPt(idx, 1.0);
    if (cfg.chinConfWeight > 0.0) addPt(kChin, cfg.chinConfWeight);

    if (n >= cfg.minContribPts && sw > 0.0) {
        s.valid    = true;
        s.centerPx = QPointF(sx / sw, sy / sw);
        s.conf     = float(sc / n);
    }

    // Head scale: inter-ear when both ears are confident, else inter-eye scaled to
    // an ear-equivalent (bi-tragion ≈ earIpdFactor × inter-pupillary). 0 = unknown.
    const bool earsOk = f.conf[kLEar] >= gate && f.conf[kREar] >= gate;
    const bool eyesOk = f.conf[kLEye] >= gate && f.conf[kREye] >= gate;
    if (earsOk)      s.scalePx = distPx(f, kLEar, kREar, frameW, frameH);
    else if (eyesOk) s.scalePx = distPx(f, kLEye, kREye, frameW, frameH) * cfg.earIpdFactor;

    // Tilt: eye-line preferred (fallback ear-line). Delta-from-address cancels any
    // constant left/right endpoint-order offset, so only consistency matters.
    if (eyesOk)      { s.tiltDeg = lineAngleDeg(f, kLEye, kREye, frameW, frameH); s.tiltValid = true; }
    else if (earsOk) { s.tiltDeg = lineAngleDeg(f, kLEar, kREar, frameW, frameH); s.tiltValid = true; }

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

HeadTrackResult trackHead(const PoseTrack2D &pose, int frameW, int frameH,
                          int64_t addressUs, const HeadTrackConfig &cfg)
{
    HeadTrackResult res;
    res.frameW = frameW;
    res.frameH = frameH;
    if (frameW <= 0 || frameH <= 0)
        return res;

    // The smoothed companion track (parallel t_us, same normalized kp) is
    // preferred — it bridges the occluded-head gaps at the top of the swing; fall
    // back to raw frames on swings analysed before the smoother existed.
    const std::vector<PoseFrame2D> &frames =
        pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frames.empty())
        return res;

    res.states.reserve(frames.size());
    for (const PoseFrame2D &f : frames)
        res.states.push_back(computeState(f, frameW, frameH, cfg));

    // Robust (median) address reference. Prefer the confident frames inside the
    // Address-event window; if none (or no event), fall back to the first N
    // centre-valid frames.
    std::vector<const HeadState *> ref;
    if (addressUs >= 0) {
        for (const HeadState &s : res.states)
            if (s.valid && std::llabs(s.t_us - addressUs) <= cfg.addrWindowUs)
                ref.push_back(&s);
    }
    if (ref.empty()) {
        for (const HeadState &s : res.states) {
            if (!s.valid) continue;
            ref.push_back(&s);
            if (int(ref.size()) >= cfg.addrMinFrames) break;
        }
    }
    if (ref.empty())
        return res;   // no head anywhere — leave res.valid == false

    std::vector<double> cx, cy, tl, sc;
    cx.reserve(ref.size()); cy.reserve(ref.size());
    for (const HeadState *s : ref) {
        cx.push_back(s->centerPx.x());
        cy.push_back(s->centerPx.y());
        if (s->tiltValid)   tl.push_back(s->tiltDeg);
        if (s->scalePx > 0) sc.push_back(s->scalePx);
    }
    res.addrCenterPx = QPointF(medianOf(cx), medianOf(cy));
    res.addrTiltDeg  = medianOf(tl);   // 0 when no tilt-valid ref frame
    res.addrScalePx  = medianOf(sc);
    res.valid        = true;

    // Address-referenced sparse channels. Both sway/lift divide by frameW (a single
    // reference dimension) so the two channels are isotropic ×frame-width units.
    for (const HeadState &s : res.states) {
        if (!s.valid) continue;
        res.sway.t_us.push_back(s.t_us);
        res.sway.value.push_back((s.centerPx.x() - res.addrCenterPx.x()) / frameW);
        res.lift.t_us.push_back(s.t_us);
        res.lift.value.push_back((res.addrCenterPx.y() - s.centerPx.y()) / frameW);
        if (s.tiltValid) {
            res.tilt.t_us.push_back(s.t_us);
            res.tilt.value.push_back(std::remainder(s.tiltDeg - res.addrTiltDeg, 360.0));
        }
    }
    return res;
}

std::vector<MetricSeries> buildHeadSeries(const HeadTrackResult &res,
                                          const std::vector<PhaseEvent> &phases,
                                          double pxPerMm)
{
    std::vector<MetricSeries> out;
    if (!res.valid || res.states.empty())
        return out;

    // Full per-frame grid (every input frame, time order) — the trace is resampled
    // onto it so a low-confidence gap coasts (bridged) rather than dropping out.
    std::vector<int64_t> grid;
    grid.reserve(res.states.size());
    for (const HeadState &s : res.states)
        grid.push_back(s.t_us);

    // CENTIMETRES, and the linear channels are emitted ONLY when the inter-ear ruler resolved.
    //
    // These used to switch between "mm" and "×frame" at runtime. A metric whose unit changes per
    // swing cannot carry a norm — the norm declares one unit and grading compares the numbers
    // without ever consulting it — and both of these carry one, in cm. So headSway arrived as 40
    // (mm) against a corridor of 4 ± 3 cm and read Action on every swing ever recorded, while the
    // ×frame fallback (~0.05) read Ideal on every swing. Same defect in both directions, and
    // `normUnitMismatch` was structurally unable to see it: it compares the norm against the
    // MEASURE, and those two agreed with each other.
    //
    // Absent rather than rescaled when the ruler fails, for the reason foot_metrics gives for
    // stanceWidth: unavailable is a fact the app renders honestly, a silently different scale is
    // not. `headTilt` is unaffected — degrees need no ruler and were never at risk.
    const bool   cm      = pxPerMm > 0.0;
    const double linGain = cm ? (double(res.frameW) / pxPerMm / 10.0) : 0.0;  // ×frame → px → mm → cm

    // NB: not named `emit` — that is a Qt keyword macro (expands to nothing).
    const auto pushSeries = [&](const HeadChannel &ch, const QString &key, const QString &label,
                                const QString &unit, double gain) {
        if (ch.t_us.empty())
            return;
        std::vector<double> vals(grid.size());
        for (size_t i = 0; i < grid.size(); ++i)
            vals[i] = interpChannel(ch.t_us, ch.value, grid[i]) * gain;
        MetricSeries m;
        m.key   = key;
        m.label = label;
        m.unit  = unit;
        m.t_us  = grid;
        m.value = vals;
        for (const Phase p : { Phase::Address, Phase::Top, Phase::Impact }) {
            const int idx = nearestIndex(grid, phaseTime(phases, p, grid.front()));
            m.phaseSamples.push_back({ p, grid[idx], vals[idx], QString() });
        }
        out.push_back(std::move(m));
    };

    if (cm) {
        pushSeries(res.sway, QStringLiteral("headSway"), QStringLiteral("Head sway"),
                   QStringLiteral("cm"), linGain);
        pushSeries(res.lift, QStringLiteral("headLift"), QStringLiteral("Head lift"),
                   QStringLiteral("cm"), linGain);
    }
    pushSeries(res.tilt, QStringLiteral("headTilt"), QStringLiteral("Head tilt"),
               QStringLiteral("°"), 1.0);
    return out;
}

} // namespace pinpoint::analysis
