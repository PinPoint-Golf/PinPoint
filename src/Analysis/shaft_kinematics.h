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

// The knot type both tables share: an axis value (swing progress for v1,
// seconds-before-impact for v2), the signed wrist-cock midpoint, and its
// spread. Defined before kin_detail because the interpolator below reads its
// members.
struct WristCockKnot { double s, betaDeg, sigmaDeg; };

namespace kin_detail {
// Wrap a degree difference into (−180, 180].
inline double wrapDeg(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

// Piecewise-linear lookup over a knot table, clamped to its own domain (so a
// value off either end reads the end knot rather than extrapolating a line
// that was never fitted). v1 clamped to [0,1] explicitly; the table's own
// endpoints are 0 and 1, so this is the same arithmetic.
template <int N>
inline double interpKnot(double x, const WristCockKnot (&k)[N], bool sigma)
{
    x = std::clamp(x, k[0].s, k[N - 1].s);
    for (int i = 1; i < N; ++i) {
        const WristCockKnot& a = k[i - 1];
        const WristCockKnot& b = k[i];
        if (x <= b.s) {
            const double t = (b.s > a.s) ? (x - a.s) / (b.s - a.s) : 1.0;
            const double av = sigma ? a.sigmaDeg : a.betaDeg;
            const double bv = sigma ? b.sigmaDeg : b.betaDeg;
            return av + t * (bv - av);
        }
    }
    return sigma ? k[N - 1].sigmaDeg : k[N - 1].betaDeg;
}
} // namespace kin_detail

// The design §R6 9-knot wrist-cock curve: swing progress s / signed wrist-cock
// midpoint β̂ (deg, trail side positive) / spread σ_β (deg).
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

// ── v2: the same model, FITTED, on a different clock ────────────────────────
//
// The table above was hand-authored and indexed by swing progress. Graded
// against hand-placed shaft truth (23 swings, 463 labels, leave-one-swing-out)
// it carries a median bias of −9.1° and a p10–p90 residual of 74.3°, with 26%
// of samples worse than 30°. Two things are wrong with it, and only one of them
// is the numbers.
//
// The axis is the larger error. Wrist release is not a fixed fraction of the
// swing; it is an event at a roughly fixed TIME before impact, so indexing by
// progress smears every golfer's release across every other's. Re-indexing the
// same model on **seconds before impact** and fitting it to the corpus takes
// the residual to 20.9° with the bias gone (+0.1°) and gross errors to 6.7% —
// a better than three-fold improvement, and it improves every session in the
// corpus, not just the average.
//
// Knots are spaced in √(−t) rather than uniformly: the wrist holds its lag near
// 90° for most of the downswing and then releases nearly all of it in the last
// ~120 ms, which uniform spacing cannot represent (an 80° drop lands inside one
// linear segment).
//
// σ is the fitted spread inflated ×2.5, a factor that is measured rather than
// guessed: at ×2.5 the 3σ envelope covers 97.0% of true wrist cock against the
// shipped table's 96.8%, while searching a median half-width of 44° against its
// 62.5°. The envelope is therefore no less forgiving than the one it replaces,
// and the corpus behind it is ONE athlete — the inflation is what stands
// between a fitted centre and a golfer this model has never seen.
//
// Fitted by tools/swinglab/wrist_cock_fit.py; the derivation, the candidate
// forms that lost, and the negative results are in
// docs/research/wrist_cock_model.md.
inline constexpr WristCockKnot kWristCockKnotsV2[15] = {
    {-1.100,  -8.1, 15.7},
    {-0.948,   2.2, 13.3},
    {-0.808,  15.1,  9.1},
    {-0.679,  38.9, 12.3},
    {-0.561,  75.1, 14.1},
    {-0.455,  85.6, 12.0},
    {-0.359,  76.3, 18.4},
    {-0.275,  78.4, 16.8},
    {-0.202,  85.7, 16.5},
    {-0.140,  96.9, 19.1},
    {-0.090,  90.6, 12.2},
    {-0.051,  61.2, 13.2},
    {-0.022,  31.6, 14.3},
    {-0.006,  16.0, 14.4},
    { 0.000,  11.3, 14.9},
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
    return kin_detail::interpKnot(s, kWristCockKnots, false);
}

// Wrist-cock spread σ_β(s) (deg), piecewise-linear over the knot table.
inline double sigmaBetaDeg(double s)
{
    return kin_detail::interpKnot(s, kWristCockKnots, true);
}

// ── the v2 axis and table ───────────────────────────────────────────────────

// Seconds before impact — negative through the swing, 0 at impact. This is the
// v2 model's clock, and the reason it beats swing progress: release is an event
// at a roughly fixed time before impact, not at a fixed fraction of the swing.
inline double timeToImpactS(std::int64_t tUs, std::int64_t impactUs)
{
    return double(tUs - impactUs) * 1e-6;
}

inline double betaHatV2Deg(double tImpS)
{
    return kin_detail::interpKnot(tImpS, kWristCockKnotsV2, false);
}

inline double sigmaBetaV2Deg(double tImpS)
{
    return kin_detail::interpKnot(tImpS, kWristCockKnotsV2, true);
}

// Predicted club direction (deg, wrapped [0,360)). The branch — which side of
// the arm the club sits on — is chir·sign(β̂(s)): trail side while β̂ > 0
// (through the downswing), lead side after release.
inline double phiClubFromBetaDeg(double phiArmDeg, double betaDeg, int chir)
{
    double th = std::fmod(phiArmDeg + double(chir) * betaDeg, 360.0);
    if (th < 0) th += 360.0;
    return th;
}

inline double phiClubPredDeg(double phiArmDeg, double s, int chir)
{
    return phiClubFromBetaDeg(phiArmDeg, betaHatDeg(s), chir);
}

// v2: the same prediction from the fitted table on the time axis.
inline double phiClubPredV2Deg(double phiArmDeg, double tImpS, int chir)
{
    return phiClubFromBetaDeg(phiArmDeg, betaHatV2Deg(tImpS), chir);
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
inline KinEnvelope envelopeFromBeta(double betaDeg, double sigmaDeg, double phiArmDeg,
                                    int chir, double kSigma)
{
    KinEnvelope e;
    e.centerDeg = phiClubFromBetaDeg(phiArmDeg, betaDeg, chir);
    e.halfDeg   = std::min(kSigma * sigmaDeg, 175.0);
    return e;
}

inline KinEnvelope envelope(double s, double phiArmDeg, int chir, double kSigma)
{
    return envelopeFromBeta(betaHatDeg(s), sigmaBetaDeg(s), phiArmDeg, chir, kSigma);
}

// v2: the same envelope from the fitted table on the time axis.
inline KinEnvelope envelopeV2(double tImpS, double phiArmDeg, int chir, double kSigma)
{
    return envelopeFromBeta(betaHatV2Deg(tImpS), sigmaBetaV2Deg(tImpS), phiArmDeg, chir, kSigma);
}

} // namespace pinpoint::analysis
