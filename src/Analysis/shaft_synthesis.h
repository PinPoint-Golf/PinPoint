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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <QPointF>

#include "swing_analysis.h"   // ShaftSample2D / ShaftPosition / ShaftSynthesized flag

// Position-first shaft measurement — Layer C "synthesis between anchors"
// (docs/design/shaft_position_first_design.md §2 Layer C). Header-only, unit-tested
// per the detector-math convention (shaft_positions.h / impact_detector.h).
//
// Given the located coaching P-anchors (P1..P8, whatever subset exists) and a
// timestamp grid, emit ONE synthesized shaft state per grid tick STRICTLY between
// consecutive anchors — a smooth-scrub VISUALIZATION tier flagged
// ShaftSynthesized. The production caller drives the grid at SynthConfig::rateHz
// (default 240 Hz), NOT the source frame rate, so ¼× replay and the shaft fan read
// a dense series that fills the inter-frame gaps (the interpolation math is
// time-continuous — see synthSampleAt). It REPLACES nothing: samples[] keeps the
// real per-frame track and this series rides alongside.
//
// ⚠ METRICS DO READ IT, and the "display channel only" framing below is no longer the whole truth.
// Scoring, the estimands, the plane fit and the wrist channel all still filter this tier out by flag
// (§2 Layer C, same discipline as ShaftKinematicPredicted). But the metrics that measure the club's
// PATH take it on purpose:
//   * `clubheadSpeed`, `handSpeed`, `lagAngle` (kinematic_series.cpp) PREFER synth over samples and
//     fall back to samples — a C¹ curve differentiates better than a gappy one.
//   * `lowPointAhead` (club_delivery.h) is DEFINED on it, with no fallback: the clubhead detector
//     does not hold a measured lock through impact, so the interpolated arc is the best statement
//     about the club's path there that exists. It ships as an ESTIMATE with a published ±2 in.
// The consequence to know: `enabled=false` below no longer changes nothing — the speeds drop back
// to the measured samples, and `lowPointAhead` disappears entirely.
//
// SYNTHESIS MODEL (per bracket [a,b] of consecutive anchors, τ = (t−t_a)/(t_b−t_a)):
//   θ(t)   C¹ monotone-safe cubic Hermite through the two anchors' (θ, θ̇). The
//          target θ_b is UNWRAPPED onto θ_a along the track's direction of rotation
//          (expected Δθ = mean anchor slope × Δt) so a pair straddling a large
//          rotation (e.g. 200°) takes the track-consistent branch, not the shortest
//          wrap. Between P anchors the rotation is monotone (that is why the P-times
//          are located as monotone-elevation crossings), so the Fritsch–Carlson
//          slope limiter is a no-op in the common case and C¹ is exact at anchors;
//          it engages only to veto overshoot at a reversal anchor (θ̇≈0 at the top).
//   grip(t) plain cubic Hermite per axis through the anchor grips with endpoint
//          velocities from the pose-grip path (the grip traces an arc — NOT monotone,
//          so no limiter).
//   L(t)   linear interpolation of the anchor drawn-lengths (lenPx).
//   conf   min(anchor confs) · decay — 1.0 at anchors, midConfFrac at the midpoint
//          (the parabola 4τ(1−τ) peaks 1 in the middle), so a synthesized run reads
//          as least-trustworthy where it is furthest from a real measurement.
//   headPx grip + L·(cos θ, sin θ) — the same image-plane convention as samples[].
//   θ̇      linear interpolation of the anchor rates (legacy viz field), or with
//          SynthConfig::curveRate the ANALYTIC dθ/dt of the Hermite above — the rate
//          the emitted θ(t) actually has, which is what `clubheadSpeed` composes from.
//
// IMPACT IS A BOUNDARY, NOT A KNOT (Sept 2026, clubhead-speed timing). The club's
// angular rate is discontinuous at contact (an iron loses ~20–30 % of its speed in
// two frames), and a Hermite that shares ONE slope at the P7 anchor cannot say so:
// pinned to an under-estimated end slope but forced to cover the true Δθ, the cubic
// bulges and its rate peaks at the bracket MIDPOINT (−18 ms on the 08-18 pairs) then
// decays ~1 mph/ms into P7. The six-argument overload of synthesizeBetweenAnchors
// therefore takes an IN and an OUT rate per anchor: bracket [a,b] uses θ̇_out(a) and
// θ̇_in(b). The five-argument form (in == out) is the legacy behaviour, bit for bit.
//
// C⁰ is exact at anchors by construction (Hermite interpolates its endpoints
// regardless of the slopes), so a synthesized state evaluated AT an anchor equals
// that anchor's geometry. Nothing is emitted outside [first anchor, last anchor].

namespace pinpoint::analysis {

// "synth.*" tuning namespace (design §4). Nested in ShaftV3Config as `synth` (lives
// here beside the Layer-C code, mirroring PositionsConfig in shaft_positions.h).
// enabled defaults ON: the synthesized tier is a visualization channel PLUS the one
// input `lowPointAhead` is defined on (see the ⚠ above) — everything else is
// ShaftSynthesized-flagged and filtered out, so it moves no other number; the real
// per-frame track stays in samples[]. Set enabled=false to go dark again
// (ShaftTrack2D.synth stays empty ⇒ swing.json omits the club.synth block,
// byte-identical to the pre-synth baseline) — which now ALSO suppresses
// `lowPointAhead`, where before it changed nothing. Keys
// "synth.enabled" / "synth.midConfFrac" / "synth.rateHz" via ShaftV3Config::fromOverrides.
struct SynthConfig {
    bool   enabled     = true;    // master gate — VIZ tier is live; metrics never read synth
    double midConfFrac = 0.6;     // conf multiplier at a span midpoint (1.0 at the anchors)
    double rateHz      = 240.0;   // dense visualization cadence (Hz). The series is sampled on
                                  // this FIXED grid, not the source frame rate, so a low-fps or
                                  // gappy capture still yields a smooth ¼×-replay/fan trail.
                                  // <= 0 ⇒ fall back to the source per-frame timestamps.
    bool   curveRate   = true;    // synth.curveRate — thetaDotRadS = analytic dθ/dt of the emitted
                                  // Hermite (slope-limited, in/out-slope aware); what clubheadSpeed
                                  // composes from (ON 2026-09-06). false = the legacy linear
                                  // interpolation of the anchor rates (pre-Sept swing.json, bit for bit).
};

namespace synth_detail {

constexpr double kSynthPi = 3.14159265358979323846;

// Unwrap target angle b (rad) onto reference a (rad) along the track's rotation:
// pick the full-turn count so (wrap(b−a) + 2πk) is closest to `expectedDelta` (the
// signed rotation the anchor slopes predict). Returns a continuous θ_b = a + Δ that
// survives arbitrarily large inter-anchor rotations.
inline double unwrapTowards(double a, double b, double expectedDelta)
{
    const double dRaw  = std::remainder(b - a, 2.0 * kSynthPi);            // ∈ (−π, π]
    const double turns = std::round((expectedDelta - dRaw) / (2.0 * kSynthPi));
    return a + dRaw + turns * 2.0 * kSynthPi;
}

// Fritsch–Carlson limiting of the endpoint derivatives (θ path): derivatives opposite
// in sign to the secant are zeroed and the pair is scaled back inside the α²+β²≤9
// monotone region — so the curve never bulges past [p0,p1]. Split out so the value
// AND the derivative below are evaluated with the SAME limited slopes.
inline void limitMonotone(double p0, double p1, double& m0, double& m1)
{
    const double delta = p1 - p0;
    if (delta == 0.0) { m0 = 0.0; m1 = 0.0; return; }
    double a = m0 / delta, b = m1 / delta;
    if (a < 0.0) { m0 = 0.0; a = 0.0; }
    if (b < 0.0) { m1 = 0.0; b = 0.0; }
    const double s = a * a + b * b;
    if (s > 9.0) {
        const double tau = 3.0 / std::sqrt(s);
        m0 = tau * a * delta;
        m1 = tau * b * delta;
    }
}

// Cubic Hermite value at unit parameter τ∈[0,1] with the slopes AS GIVEN (already
// limited, or deliberately unlimited). The value at τ=0/1 is p0/p1 EXACTLY.
inline double hermiteValue(double p0, double p1, double m0, double m1, double t)
{
    const double t2 = t * t, t3 = t2 * t;
    return (2.0 * t3 - 3.0 * t2 + 1.0) * p0 + (t3 - 2.0 * t2 + t) * m0
         + (-2.0 * t3 + 3.0 * t2) * p1 + (t3 - t2) * m1;
}

// dP/dτ of the same cubic — equals m0 at τ=0 and m1 at τ=1 exactly.
inline double hermiteDeriv(double p0, double p1, double m0, double m1, double t)
{
    const double t2 = t * t;
    return (6.0 * t2 - 6.0 * t) * p0 + (3.0 * t2 - 4.0 * t + 1.0) * m0
         + (-6.0 * t2 + 6.0 * t) * p1 + (3.0 * t2 - 2.0 * t) * m1;
}

// Cubic Hermite value at unit parameter τ∈[0,1]. p0/p1 = endpoint values; m0/m1 =
// endpoint derivatives w.r.t. τ (i.e. dP/dτ). `monotone` applies the Fritsch–Carlson
// limiter above. The value at τ=0/1 is p0/p1 EXACTLY regardless of limiting.
inline double hermite(double p0, double p1, double m0, double m1, double t, bool monotone)
{
    if (monotone) limitMonotone(p0, p1, m0, m1);
    return hermiteValue(p0, p1, m0, m1, t);
}

} // namespace synth_detail

// The synthesized shaft state at time `t` within the bracket of consecutive anchors
// [a, b] given each anchor's θ̇ (rad/s) and grip velocity (px/s). `t` may equal an
// endpoint ⇒ that anchor's geometry EXACTLY (C⁰ contract; public so the continuity
// gate is directly unit-testable). Emits a ShaftSample2D flagged ShaftSynthesized.
inline ShaftSample2D synthSampleAt(const ShaftPosition& a, double thetaDotA, const QPointF& gripVelA,
                                   const ShaftPosition& b, double thetaDotB, const QPointF& gripVelB,
                                   int64_t t, const SynthConfig& cfg)
{
    using namespace synth_detail;
    ShaftSample2D s;

    const double dtUs = double(b.t_us - a.t_us);
    const double hSec = dtUs * 1e-6;                                  // bracket duration (s)
    const double tau  = dtUs > 0.0 ? double(t - a.t_us) / dtUs : 0.0; // ∈ [0,1] for in-bracket t

    // θ — unwrap b onto a along the track direction, then monotone-safe Hermite. The
    // limited slopes are kept so the analytic rate below is the rate of THIS curve.
    const double expected = 0.5 * (thetaDotA + thetaDotB) * hSec;     // signed expected Δθ (rad)
    const double thB      = unwrapTowards(a.thetaRad, b.thetaRad, expected);
    double mA = thetaDotA * hSec, mB = thetaDotB * hSec;
    limitMonotone(a.thetaRad, thB, mA, mB);
    const double theta    = hermiteValue(a.thetaRad, thB, mA, mB, tau);

    // grip — plain cubic Hermite per axis (grip path is an arc, not monotone).
    const double gxp = hermite(a.gripPx.x(), b.gripPx.x(), gripVelA.x() * hSec, gripVelB.x() * hSec, tau, false);
    const double gyp = hermite(a.gripPx.y(), b.gripPx.y(), gripVelA.y() * hSec, gripVelB.y() * hSec, tau, false);

    // L — linear interpolation of the anchor extents (fall back to whichever anchor
    // has a valid length when the other abstains, lenPx < 0).
    const double la = a.lenPx, lb = b.lenPx;
    double len;
    if (la > 0.0 && lb > 0.0)  len = la + (lb - la) * tau;
    else if (la > 0.0)         len = la;
    else if (lb > 0.0)         len = lb;
    else                       len = 0.0;

    // conf — min(anchor confs) decayed toward the midpoint.
    const double base  = std::min(double(a.conf), double(b.conf));
    const double decay = 1.0 - (1.0 - cfg.midConfFrac) * 4.0 * tau * (1.0 - tau);

    s.t_us         = t;
    s.gripPx       = QPointF{ gxp, gyp };
    s.thetaRad     = theta;
    // θ̇ — the analytic rate of the emitted curve (curveRate), or the legacy linear
    // interpolation of the anchor rates (a viz-tier field; the precision channel is
    // samples[], never this synthesized series).
    s.thetaDotRadS = (cfg.curveRate && hSec > 0.0)
                   ? hermiteDeriv(a.thetaRad, thB, mA, mB, tau) / hSec
                   : thetaDotA + (thetaDotB - thetaDotA) * tau;
    s.visibleLenPx = len;
    s.conf         = float(base * decay);
    s.flags        = ShaftSynthesized;
    s.headPx       = QPointF{ gxp + len * std::cos(theta), gyp + len * std::sin(theta) };
    return s;
}

// Emit synthesized samples between the located P-anchors. `anchors` MUST be sorted
// strictly ascending by t_us (the caller passes ShaftTrack2D.positions, which is
// ordered by p ⇒ by time); `thetaDotRadS` / `gripVelPxS` are the smoothed-track θ̇
// (rad/s) and grip velocity (px/s) sampled at each anchor instant (parallel to
// `anchors`). `frameTUs` are the per-frame camera timestamps (ascending). Produces
// one sample per frame STRICTLY between each consecutive-anchor pair — so the result
// is ascending in t_us, all flagged ShaftSynthesized, and empty outside
// [first anchor, last anchor] or when < 2 anchors are given.
// Two rates per anchor: bracket [a_k, a_k+1] leaves a_k at thetaDotOutRadS[k] and
// arrives at a_k+1 at thetaDotInRadS[k+1]. An anchor whose in and out rates differ
// (P7: the impact step) is a C⁰ knot with a rate discontinuity — deliberately.
inline std::vector<ShaftSample2D> synthesizeBetweenAnchors(
    const std::vector<ShaftPosition>& anchors,
    const std::vector<double>&        thetaDotInRadS,
    const std::vector<double>&        thetaDotOutRadS,
    const std::vector<QPointF>&       gripVelPxS,
    const std::vector<int64_t>&       frameTUs,
    const SynthConfig&                cfg)
{
    std::vector<ShaftSample2D> out;
    const size_t n = anchors.size();
    if (n < 2 || thetaDotInRadS.size() != n || thetaDotOutRadS.size() != n
        || gripVelPxS.size() != n) return out;

    for (size_t k = 0; k + 1 < n; ++k) {
        const ShaftPosition& a = anchors[k];
        const ShaftPosition& b = anchors[k + 1];
        if (b.t_us <= a.t_us) continue;                     // defensive: strictly increasing
        for (int64_t t : frameTUs) {
            if (t <= a.t_us || t >= b.t_us) continue;       // STRICTLY between the anchors
            out.push_back(synthSampleAt(a, thetaDotOutRadS[k],    gripVelPxS[k],
                                        b, thetaDotInRadS[k + 1], gripVelPxS[k + 1], t, cfg));
        }
    }
    return out;
}

// Legacy single-rate form: in == out at every anchor (C¹ everywhere).
inline std::vector<ShaftSample2D> synthesizeBetweenAnchors(
    const std::vector<ShaftPosition>& anchors,
    const std::vector<double>&        thetaDotRadS,
    const std::vector<QPointF>&       gripVelPxS,
    const std::vector<int64_t>&       frameTUs,
    const SynthConfig&                cfg)
{
    return synthesizeBetweenAnchors(anchors, thetaDotRadS, thetaDotRadS, gripVelPxS, frameTUs, cfg);
}

} // namespace pinpoint::analysis
