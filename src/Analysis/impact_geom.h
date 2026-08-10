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

// P7 impact from club-at-ball geometry. The acoustic/marker anchor
// (job.impactUs) is the ladder's only impact source, and the 2026-08-10 corpus
// pre-flight measured both of its failure modes on the 14 truth swings:
//   - recorded capture.impactUs runs a uniform 13-22 ms EARLY vs video truth
//     (trigger-to-contact bias in the capture chain, faithfully reproduced by
//     the analyzer within ~2 ms), and
//   - the impactUs -> nearest-coverage-frame mapping lands +234..+362 ms late
//     on 3 swings (a coverage gap around impact: the nearest surviving frame
//     sits deep in the follow-through), which also truncates/poisons the P6
//     search window (locatePTimes bounds P6 at the impact frame) - those 3
//     are exactly the no-P6 truth swings.
// The ball's launchTUs is NOT usable here (median ~965 ms early on the same
// corpus - launch detection fires on flicker).
//
// The geometric observation: the pre-swing ball position is stationary and
// reliably measured (A1's gated address-ball cluster), and at impact the
// wedge-corrected shaft direction theta(t) sweeps through the grip->ball
// direction theta_ball(t). That crossing interpolates sub-frame BETWEEN
// coverage frames, so it is immune to the nearest-frame gap failure by
// construction.
//
// Pure header: no Qt, no OpenCV. The crossing detector is a deliberate
// self-contained sibling of shaft_positions.h's findHorizontalCrossing (that
// body is frozen under its unit pins). It can NOT be reused here because its
// elevationDeg fold identifies theta and theta+180 - correct for "shaft
// parallel to ground", wrong for "shaft AT the ball": theta_ball+180 is the
// club pointing away from the ball and must never fire.

namespace pinpoint::analysis {

// "shaft.impactGeom.*" keys via ShaftV3Config::fromOverrides. enabled=false =>
// decideTrack never computes or consults the geometry - byte- AND
// code-path-identical output.
struct ImpactGeomConfig {
    bool    enabled    = false;    // master gate: locate the crossing, override implausible anchors (dark at merge)
    bool    retime     = false;    // sub-frame P7/Impact retime when the geometry corroborates the anchor (dark at merge)
    double  hystDeg    = 8.0;      // hysteresis deadband each side of theta_ball (mirrors positions.hysteresisDeg)
    double  maxStepDeg = 120.0;    // adjacent-valid |d(theta-theta_ball)| beyond this = the ±180 seam passing, not a transit
    int64_t overrideUs = 100000;   // |t_geo - t_anchor| beyond this => anchor implausible => override
    int64_t windowUs   = 600000;   // anchor-centred search half-window; generous vs the observed
                                   // +362 ms worst clamp corruption, short of the address hold
};

struct ImpactGeomResult {
    bool    found = false;   // a hysteresis-confirmed theta==theta_ball transit exists in (top, fin]
    int64_t tUs   = 0;       // sub-frame-interpolated crossing instant
    int     frame = -1;      // last coverage frame at-or-before tUs
};

namespace impact_geom_detail {

inline double wrap180(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

} // namespace impact_geom_detail

// FIRST hysteresis-confirmed crossing of theta(t) through theta_ball(t) over
// the inclusive frame window [searchLo, searchHi] (caller-computed: anchor
// ± windowUs when an anchor exists - the phase model's own top/fin0 collapse
// on exactly the swings whose impact needs rescuing, so the window must NOT
// lean on them there). theta_ball(f) = atan2(ballY - gy[f], ballX - gx[f]) -
// the FIXED pre-swing ball against the MOVING per-frame grip (the ball is
// stationary until launch, so this works even when the detector never finds
// the ball near impact). e(f) = wrap180(theta[f] - theta_ball[f]) makes the
// detector representation-independent (a long-path DP ending -270 deg == +90
// deg reads identically), and theta_ball+180 sits at e = ±180, outside the
// hysteresis band - it can never fire. NaN frames are skipped; crossings
// interpolate between consecutive VALID frames, so a NaN or coverage gap
// never quantizes the instant to a far frame. theta sweeps ~300 deg
// top->finish, so the ±180 seam of e transits exactly once: a seam transit
// sign-flips e between two LARGE values (+170 -> -170), so its RAW step
// |ea - eb| is huge (~340) where a genuine 0-transit's is small (the armed
// frame sits just outside the hysteresis band) - a would-be crossing whose
// raw step exceeds cfg.maxStepDeg is the seam, re-arms to the new side, and
// never fires. (The WRAPPED step is small for both - useless here.) First
// crossing = impact (the clubhead passes the ball once; later transits are
// follow-through geometry).
inline ImpactGeomResult locateImpactGeom(const std::vector<int64_t>& tUs,
                                         const std::vector<double>& thetaDeg,
                                         const std::vector<double>& gx,
                                         const std::vector<double>& gy,
                                         double ballXPx, double ballYPx,
                                         int searchLo, int searchHi,
                                         const ImpactGeomConfig& cfg)
{
    using impact_geom_detail::wrap180;
    constexpr double kPi = 3.14159265358979323846;

    ImpactGeomResult out;
    const int n = int(thetaDeg.size());
    if (n == 0 || tUs.size() != size_t(n) || gx.size() != size_t(n) || gy.size() != size_t(n))
        return out;
    const int i0 = std::max(0, searchLo);
    const int i1 = std::min(n - 1, searchHi);
    if (i1 <= i0) return out;

    // Valid frames + the at-ball error series e(f).
    std::vector<int>    vi;
    std::vector<double> ve;
    vi.reserve(size_t(i1 - i0 + 1));
    ve.reserve(size_t(i1 - i0 + 1));
    for (int i = i0; i <= i1; ++i) {
        if (std::isnan(thetaDeg[i])) continue;
        const double dx = ballXPx - gx[size_t(i)], dy = ballYPx - gy[size_t(i)];
        if (!(std::hypot(dx, dy) > 1.0)) continue;   // grip on the ball - direction undefined
        const double tb = std::atan2(dy, dx) * 180.0 / kPi;
        vi.push_back(i);
        ve.push_back(wrap180(thetaDeg[size_t(i)] - tb));
    }
    if (vi.size() < 2) return out;

    // Hysteresis state machine (the findHorizontalCrossing shape) with the
    // seam guard: a would-be crossing whose raw step |e(a) - e(armA)| exceeds
    // maxStepDeg is the ±180 seam passing by - re-arm to the new side, never
    // fire. armA advances on every armed frame (the sibling's semantics), so
    // for a genuine transit it sits just outside the band and the raw step is
    // small.
    int armedSign = 0, armA = -1, fromA = -1, toA = -1;
    for (int a = 0; a < int(vi.size()); ++a) {
        const double e = ve[size_t(a)];
        if (e > cfg.hystDeg) {
            if (armedSign < 0) {
                if (std::abs(e - ve[size_t(armA)]) > cfg.maxStepDeg) { armedSign = 1; armA = a; continue; }
                fromA = armA; toA = a; break;
            }
            armedSign = 1; armA = a;
        } else if (e < -cfg.hystDeg) {
            if (armedSign > 0) {
                if (std::abs(e - ve[size_t(armA)]) > cfg.maxStepDeg) { armedSign = -1; armA = a; continue; }
                fromA = armA; toA = a; break;
            }
            armedSign = -1; armA = a;
        }
    }
    if (fromA < 0) return out;   // never armed both sides => no genuine transit

    // Steepest zero-straddling consecutive-valid pair within [fromA, toA],
    // interpolated sub-frame (the sibling's refinement, on e instead of
    // elevation). Seam pairs are excluded by the same raw-step test.
    double  bestSlope = -1.0;
    int64_t bestT     = 0;
    bool    have      = false;
    for (int a = fromA; a < toA; ++a) {
        const double ea = ve[size_t(a)], eb = ve[size_t(a + 1)];
        if (std::abs(eb - ea) > cfg.maxStepDeg) continue;   // seam pair
        const bool straddle = (ea >= 0.0 && eb < 0.0) || (ea <= 0.0 && eb > 0.0);
        if (!straddle) continue;
        const double slope = std::abs(ea - eb);
        if (slope <= bestSlope) continue;
        bestSlope = slope;
        const double frac = (ea == eb) ? 0.0 : ea / (ea - eb);   // in [0,1]
        const int64_t ta = tUs[size_t(vi[size_t(a)])], tb = tUs[size_t(vi[size_t(a + 1)])];
        bestT = ta + int64_t(std::llround(frac * double(tb - ta)));
        have = true;
    }
    if (!have) bestT = tUs[size_t(vi[size_t(toA)])];   // numerical fallback

    out.found = true;
    out.tUs   = bestT;
    out.frame = vi[size_t(fromA)];
    for (int i = vi[size_t(fromA)]; i <= vi[size_t(toA)]; ++i)
        if (tUs[size_t(i)] <= bestT) out.frame = i;
    return out;
}

// What decideImpactFrame did (trace telemetry + the caller's retime gate).
enum : int {
    kImpactGeomKept     = 0,   // anchor kept: geometry absent, corroborating (retime off), or dark
    kImpactGeomOverride = 1,   // anchor implausible (> overrideUs) - geometry replaces it
    kImpactGeomAdopted  = 2,   // no anchor - geometry is the only impact (the "legitimate refine path")
    kImpactGeomRetimed  = 3,   // anchor corroborated - frame kept, P7/Impact retimed sub-frame
};

struct ImpactDecision {
    int     frame   = -1;   // the frame bound for the P5/P6/P8 windows (locatePTimes)
    int64_t tUs     = 0;    // the P7/Impact instant (sub-frame when geometry decided)
    int     applied = kImpactGeomKept;
};

// Pure decision: the geometry arbitrates the EMISSION. emitFrame is the
// phase model's pm.impact - the value every downstream consumer receives.
// The raw job.impactUs anchor does NOT appear here: its role is to CENTRE
// the caller's search window (anchor ± windowUs), which already constrains
// any found crossing to be anchor-plausible. On the phase-model-collapse
// swings segmentPhases' clamp(impact, top+1, ·) drags the EMISSION
// +234..+362 ms past a ~250 ms-late top landmark while the raw anchor (and
// the geometry, measured -10 ms) sit at truth - so a decision that compares
// t_geo to the raw anchor "corroborates" and misses the corruption;
// comparing to the emission catches it. Policy:
//   - geometry absent            => emission unchanged (abstain - never degrade);
//   - anchor absent              => adopt the geometry (live no-trigger path);
//   - |t_geo - t_emit| beyond
//     cfg.overrideUs             => override: frame = at-or-before t_geo,
//                                   time = t_geo (nearest-to-t_geo could land
//                                   across a coverage gap; at-or-before is
//                                   also the conservative right bound for the
//                                   P6 last-crossing window);
//   - within cfg.overrideUs      => the emission is corroborated: with
//     cfg.retime the P7/Impact TIME moves to the sub-frame t_geo while the
//     window FRAME stays the emission's (the corroborated swings' P5/P6/P8
//     windows stay byte-identical); without it, unchanged.
// Frames clamp to [0, n-1] only - re-imposing a top-derived clamp would
// reproduce the failure this module exists to catch. The CALLER decides
// whether the decided frame is usable as a positions window bound (it must
// still satisfy locatePTimes' top < impact ordering on sane models).
inline ImpactDecision decideImpactFrame(bool anchorPresent, int emitFrame,
                                        const ImpactGeomResult& geo,
                                        const std::vector<int64_t>& tUs,
                                        const ImpactGeomConfig& cfg)
{
    const int n = int(tUs.size());
    ImpactDecision d;
    const auto clampFrame = [&](int f) { return std::clamp(f, 0, n - 1); };

    d.frame = clampFrame(emitFrame);
    d.tUs   = n > 0 ? tUs[size_t(d.frame)] : 0;
    if (!geo.found) return d;   // abstain - the emission stands

    if (!anchorPresent) {
        d.frame   = clampFrame(geo.frame);
        d.tUs     = geo.tUs;
        d.applied = kImpactGeomAdopted;
        return d;
    }
    const int64_t tEmit = tUs[size_t(clampFrame(emitFrame))];
    if (std::llabs(geo.tUs - tEmit) > cfg.overrideUs) {
        d.frame   = clampFrame(geo.frame);
        d.tUs     = geo.tUs;
        d.applied = kImpactGeomOverride;
        return d;
    }
    if (cfg.retime) {
        d.tUs     = geo.tUs;   // frame (the window bound) stays the emission's
        d.applied = kImpactGeomRetimed;
    }
    return d;
}

} // namespace pinpoint::analysis
