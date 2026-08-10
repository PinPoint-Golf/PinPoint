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

// Minimal R6 double-pendulum club predictor (shaft_detection_skeleton_design.md
// §R6). The club direction is the measured lead-arm direction plus a
// stereotyped, swing-progress-indexed wrist-cock offset:
//
//     φ_club_pred(f) = φ_arm(f) + chir·β̂(s(f)),   envelope ± kσ·σ_β(s(f))
//
// where s ∈ [0,1] is swing progress anchored to the tracker's own phase
// landmarks (bs0→0, top→0.5, impact→0.9, fin0→1.0) and β̂/σ_β come from a
// 9-knot wrist-cock table. The predicted rate ω̂ is the finite difference of
// the prediction — deliberately NO DP dependency, so the wedge trigger and
// the t_exp calibration never chase the tracker's own (possibly wrong) path.
//
// Sign convention: β̂ is SIGNED, positive on the trail side (club lags the arm
// in the direction of rotation); the sign flips to the lead side at release
// (between the s=0.90 and s=0.95 knots, the club overtaking the arm through
// impact). chir is the tracker's chirality (sign of unwrapped φ over
// [bs0, top]) — consistent with the C4 reachable cone, whose mid-swing centre
// φ + chir·110° this table's s=0.60 peak (100°) reproduces.
//
// Pure header: no Qt, no OpenCV, no tracker types — standalone-testable.

namespace pinpoint::analysis {

namespace kin_detail {
// Wrap a degree difference into (−180, 180].
inline double wrapDeg(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}
} // namespace kin_detail

// The design §R6 9-knot wrist-cock curve: swing progress s / signed wrist-cock
// midpoint β̂ (deg, trail side positive) / spread σ_β (deg).
struct WristCockKnot { double s, betaDeg, sigmaDeg; };
inline constexpr WristCockKnot kWristCockKnots[9] = {
    {0.00,   8.0,  8.0},
    {0.15,  27.0, 15.0},
    {0.35,  70.0, 20.0},
    {0.50,  92.0, 22.0},
    {0.60, 100.0, 30.0},
    {0.80,  47.0, 25.0},
    {0.90,   7.0, 10.0},
    {0.95, -27.0, 20.0},   // sign flips to the lead side through release
    {1.00, -95.0, 30.0},
};

// Piecewise-linear swing progress from the frame index against the phase-model
// anchors: bs0→0, top→0.5, impact→0.9, fin0→1.0. Clamped to [0,1]; degenerate
// (non-increasing) anchor pairs collapse to the segment's far value so a
// pathological phase model still yields a monotone, bounded s.
inline double swingProgress(int f, int bs0, int top, int impact, int fin0)
{
    if (f <= bs0) return 0.0;
    if (f <= top)
        return top > bs0 ? 0.5 * double(f - bs0) / double(top - bs0) : 0.5;
    if (f <= impact)
        return impact > top ? 0.5 + 0.4 * double(f - top) / double(impact - top) : 0.9;
    if (f <= fin0)
        return fin0 > impact ? 0.9 + 0.1 * double(f - impact) / double(fin0 - impact) : 1.0;
    return 1.0;
}

// Signed wrist-cock midpoint β̂(s) (deg), piecewise-linear over the knot table.
inline double betaHatDeg(double s)
{
    s = std::clamp(s, 0.0, 1.0);
    constexpr int N = int(sizeof(kWristCockKnots) / sizeof(kWristCockKnots[0]));
    for (int i = 1; i < N; ++i) {
        const WristCockKnot& a = kWristCockKnots[i - 1];
        const WristCockKnot& b = kWristCockKnots[i];
        if (s <= b.s) {
            const double t = (b.s > a.s) ? (s - a.s) / (b.s - a.s) : 1.0;
            return a.betaDeg + t * (b.betaDeg - a.betaDeg);
        }
    }
    return kWristCockKnots[N - 1].betaDeg;
}

// Wrist-cock spread σ_β(s) (deg), piecewise-linear over the knot table.
inline double sigmaBetaDeg(double s)
{
    s = std::clamp(s, 0.0, 1.0);
    constexpr int N = int(sizeof(kWristCockKnots) / sizeof(kWristCockKnots[0]));
    for (int i = 1; i < N; ++i) {
        const WristCockKnot& a = kWristCockKnots[i - 1];
        const WristCockKnot& b = kWristCockKnots[i];
        if (s <= b.s) {
            const double t = (b.s > a.s) ? (s - a.s) / (b.s - a.s) : 1.0;
            return a.sigmaDeg + t * (b.sigmaDeg - a.sigmaDeg);
        }
    }
    return kWristCockKnots[N - 1].sigmaDeg;
}

// Predicted club direction (deg, wrapped [0,360)). The branch — which side of
// the arm the club sits on — is chir·sign(β̂(s)): trail side while β̂ > 0
// (through the downswing), lead side after release.
inline double phiClubPredDeg(double phiArmDeg, double s, int chir)
{
    double th = std::fmod(phiArmDeg + double(chir) * betaHatDeg(s), 360.0);
    if (th < 0) th += 360.0;
    return th;
}

// Predicted club angular rate (deg/s) from two prediction samples one frame
// apart on each side (central difference), wrap-aware. dtS = the timestamp
// difference between the two samples (s).
inline double omegaPredDegPerS(double phiPredPrevDeg, double phiPredNextDeg, double dtS)
{
    if (!(dtS > 0.0)) return 0.0;
    return kin_detail::wrapDeg(phiPredNextDeg - phiPredPrevDeg) / dtS;
}

// The wedge search envelope: centre = the predicted club direction, half-width
// = kSigma·σ_β(s), capped just short of the half-circle so the arc never
// degenerates into "everywhere".
struct KinEnvelope {
    double centerDeg = 0.0;
    double halfDeg   = 0.0;
};
inline KinEnvelope envelope(double s, double phiArmDeg, int chir, double kSigma)
{
    KinEnvelope e;
    e.centerDeg = phiClubPredDeg(phiArmDeg, s, chir);
    e.halfDeg   = std::min(kSigma * sigmaBetaDeg(s), 175.0);
    return e;
}

} // namespace pinpoint::analysis
