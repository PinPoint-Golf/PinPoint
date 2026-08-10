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

// Shaft v3.0-r1 deciding half — faithful C++ port of tools/shaftlab/
// club_track_v3.py (the non-evidence stages: phase model, φ smoothing, C2
// geometry, per-frame DP emission, banded Viterbi, ψ-isotonic reconcile).
// scipy.ndimage.median_filter / gaussian_filter1d are reproduced with their
// default mode='reflect' so the smoothed φ / grip-speed / joints match numpy.

#include "shaft_track_assembly.h"

#include "analysis_tuning.h"       // pinpoint::analysis::tuning::apply
#include "ball_anchor.h"           // medianGripBallLenPx (A1 — L_px before head placement)
#include "shaft_kinematics.h"      // R6 predictor (swingProgress/phiClubPred/envelope, S2 wedge)
#include "shaft_position_fit.h"    // sampleClamp/med4/ridgeLineIntegral (shared) + fitPosition (Layer B B-fit)

#include <QElapsedTimer>           // Stage-2 head-pass wall-clock (trace->headMs)

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace pinpoint::analysis {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = 1e9;
// Anthropometric shoulder-mid→ankle-mid height as a fraction of stature — the
// denominator of the pose-scale rung's px/m estimate (A2). Not sweepable: it is
// a population constant, unlike lenStatureM/lenGripDownM which absorb the club.
constexpr double kShoulderAnkleFrac = 0.83;

inline double circWrap(double a) { return std::fmod(std::fmod(a + 180.0, 360.0) + 360.0, 360.0) - 180.0; }

// ── club-length fusion helpers (A2b) ─────────────────────────────────────────
// E-band px = band scale × grip-corrected club length; ≤0 (returns -1) when no
// band lock. Shared by the pre-pass, post-pass, and out.lengths so all three agree.
inline double bandLengthPx(double sTypical, double r0Med, double clubLenMm)
{
    return (sTypical > 0.0) ? sTypical * std::max(0.0, clubLenMm - r0Med) : -1.0;
}

// Build the INSTANTANEOUS candidate set (E-ball + E-band). Head is appended by
// the caller (post-pass only — Stage-2 heads don't exist at the pre-pass).
inline std::vector<LengthCandidate> instantLengthCandidates(float measuredClubLenPx,
                                                            double sTypical, double r0Med, double clubLenMm)
{
    std::vector<LengthCandidate> cands;
    if (measuredClubLenPx > 0.f)
        cands.push_back({LengthSource::Ball, double(measuredClubLenPx), -1.0});
    const double bl = bandLengthPx(sTypical, r0Med, clubLenMm);
    if (bl > 0.0)
        cands.push_back({LengthSource::Band, bl, -1.0});
    return cands;
}

// Append E-prior when it has matured (n ≥ 2, positive EMA); returns the prior's
// support count for confSupport (0 when not appended). σ = sqrt(varPx) (floored
// inside fuseClubLength).
inline int appendPriorCandidate(std::vector<LengthCandidate>& cands, const LengthPriorState* prior)
{
    if (!prior || prior->n < 2 || !(prior->emaPx > 0.0)) return 0;
    cands.push_back({LengthSource::Prior, prior->emaPx, std::sqrt(std::max(prior->varPx, 0.0))});
    return prior->n;
}

// Linear-interpolation percentile of a copy (type-7). Empty ⇒ NaN. Deterministic.
inline double percentileOf(std::vector<double> v, double p)
{
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    if (v.size() == 1) return v[0];
    const double pos = std::clamp(p, 0.0, 1.0) * double(v.size() - 1);
    const size_t lo = size_t(std::floor(pos));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (pos - double(lo)) * (v[hi] - v[lo]);
}

// Median of the finite entries of v (px search ceilings may hold NaN). ≤0 / empty
// ⇒ NaN (the caller treats a non-positive ceiling as "no pin guard").
inline double medianFinite(const std::vector<double>& v)
{
    std::vector<double> f;
    f.reserve(v.size());
    for (double x : v) if (std::isfinite(x)) f.push_back(x);
    if (f.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(f.begin(), f.end());
    const size_t m = f.size();
    return (m % 2) ? f[m / 2] : 0.5 * (f[m / 2 - 1] + f[m / 2]);
}

// scipy 'reflect' boundary (…d c b a | a b c d | d c b a…): edge sample dup'd.
inline int reflectIdx(int i, int n)
{
    if (n == 1) return 0;
    while (i < 0 || i >= n) {
        if (i < 0) i = -1 - i;
        if (i >= n) i = 2 * n - 1 - i;
    }
    return i;
}

// scipy.ndimage.median_filter(x, size) — odd size, mode='reflect', origin 0.
std::vector<double> medianFilter1d(const std::vector<double>& x, int size)
{
    const int n = int(x.size());
    const int r = size / 2;
    std::vector<double> out(n);
    std::vector<double> win(size);
    for (int i = 0; i < n; ++i) {
        for (int k = -r; k <= r; ++k) win[k + r] = x[reflectIdx(i + k, n)];
        std::nth_element(win.begin(), win.begin() + r, win.end());
        out[i] = win[r];   // odd size ⇒ middle element is the median
    }
    return out;
}

// scipy.ndimage.gaussian_filter1d(x, sigma, truncate=4.0), mode='reflect'.
std::vector<double> gaussianFilter1d(const std::vector<double>& x, double sigma)
{
    const int n = int(x.size());
    const int r = int(4.0 * sigma + 0.5);   // radius = int(truncate*sigma + 0.5)
    std::vector<double> w(2 * r + 1);
    double sum = 0;
    const double inv2s2 = -0.5 / (sigma * sigma);
    for (int k = -r; k <= r; ++k) { w[k + r] = std::exp(inv2s2 * k * k); sum += w[k + r]; }
    for (double& v : w) v /= sum;
    std::vector<double> out(n, 0.0);
    for (int i = 0; i < n; ++i) {
        double acc = 0;
        for (int k = -r; k <= r; ++k) acc += w[k + r] * x[reflectIdx(i + k, n)];
        out[i] = acc;
    }
    return out;
}

// np.unwrap over a radian sequence.
std::vector<double> unwrap(const std::vector<double>& p)
{
    std::vector<double> out = p;
    double corr = 0;
    for (size_t i = 1; i < out.size(); ++i) {
        double d = (p[i] - p[i - 1]);
        double dd = std::fmod(d + kPi, 2 * kPi);
        if (dd < 0) dd += 2 * kPi;
        dd -= kPi;
        if (dd == -kPi && d > 0) dd = kPi;
        corr += dd - d;
        out[i] = p[i] + corr;
    }
    return out;
}

inline int wmaxFor(SwingPhase p, const ShaftV3Config& c)
{
    double w = c.wmaxDownswing;
    switch (p) {
        case SwingPhase::Addr:      w = c.wmaxAddr; break;
        case SwingPhase::Backswing: w = c.wmaxBackswing; break;
        case SwingPhase::Top:       w = c.wmaxTop; break;
        case SwingPhase::Impact:    w = c.wmaxImpact; break;
        case SwingPhase::Downswing: w = c.wmaxDownswing; break;
        case SwingPhase::Thru:      w = c.wmaxThru; break;
        case SwingPhase::Finish:    w = c.wmaxFinish; break;
    }
    return int(std::ceil(w / c.grid));
}

inline int phaseSign(SwingPhase p)
{
    switch (p) {
        case SwingPhase::Backswing: return +1;
        case SwingPhase::Downswing:
        case SwingPhase::Impact:
        case SwingPhase::Thru:
        case SwingPhase::Finish:    return -1;
        default:                    return 0;   // addr, top
    }
}

inline bool isMidswing(SwingPhase p)
{
    return p == SwingPhase::Backswing || p == SwingPhase::Top || p == SwingPhase::Downswing
        || p == SwingPhase::Impact || p == SwingPhase::Thru;
}

// ── Layer A snap: local ridge line re-registration (shaft_position_first §2A) ──
// sampleClamp / med4 / ridgeLineIntegral are shared with the Layer B milestone
// fit and now live in shaft_position_fit.h (promoted verbatim from here so the two
// passes share one definition; snap math unchanged).

// One sample's snap search over (⊥ offset d, Δθ). Grid: 1 px offset × 0.5° angle.
struct SnapResult {
    double offsetPx      = 0.0;   // best perpendicular offset from the original anchor (px)
    double dThetaDeg     = 0.0;   // best angular delta from the original θ (deg)
    float  bestLineConf  = 0.f;   // support under the winning line
    float  originLineConf = 0.f;  // support under the original (d=0,Δθ=0) line — recorded on reject
};
SnapResult snapSearch(const cv::Mat& g32, double gx, double gy, double theta0Rad,
                      double drawnLenPx, const SnapConfig& sc, const RidgeConfig& rc)
{
    const double rLo   = double(rc.rLo);
    const double rStep = double(rc.rStep);
    const double rEnd  = std::clamp(drawnLenPx, double(rc.minLenPx) + rStep, double(rc.rHi));
    const double nx = -std::sin(theta0Rad), ny = std::cos(theta0Rad);   // offset axis (⊥ to θ0)
    SnapResult best;
    double originSup = 0.0;
    ridgeLineIntegral(g32, gx, gy, theta0Rad, rLo, rEnd, rStep, sc.corridorHalfPx, rc, originSup);
    best.originLineConf = float(std::clamp(originSup, 0.0, 1.0));

    // Evaluate the whole (d, Δθ) grid, then pick the CENTRE of the maximal
    // plateau rather than the first cell that reaches it — a thick/saturating
    // ridge scores equally across a band of offsets, so the plateau centroid is
    // the on-ridge centre (registers the anchor to a couple of px, not an arbitrary
    // plateau edge).
    struct Cell { double d, dd, obj, sup; };
    std::vector<Cell> cells;
    double bestObj = -std::numeric_limits<double>::infinity();
    for (double d = -sc.maxOffsetPx; d <= sc.maxOffsetPx + 1e-9; d += 1.0) {
        const double ax = gx + d * nx, ay = gy + d * ny;
        for (double dd = -sc.maxDeltaDeg; dd <= sc.maxDeltaDeg + 1e-9; dd += 0.5) {
            double sup = 0.0;
            const double obj = ridgeLineIntegral(g32, ax, ay, theta0Rad + dd * kPi / 180.0,
                                                 rLo, rEnd, rStep, sc.corridorHalfPx, rc, sup);
            cells.push_back({d, dd, obj, sup});
            bestObj = std::max(bestObj, obj);
        }
    }
    const double eps = 0.5;                       // evidence units — plateau membership
    double sumD = 0.0, sumDd = 0.0; int cnt = 0;
    for (const Cell& c : cells)
        if (c.obj >= bestObj - eps) { sumD += c.d; sumDd += c.dd; ++cnt; }
    const double cDbar = (cnt > 0) ? sumD / cnt : 0.0, cDDbar = (cnt > 0) ? sumDd / cnt : 0.0;
    double bestDist = std::numeric_limits<double>::infinity();
    for (const Cell& c : cells) {                 // the real plateau cell nearest the centroid
        if (c.obj < bestObj - eps) continue;
        const double dist = (c.d - cDbar) * (c.d - cDbar) + (c.dd - cDDbar) * (c.dd - cDDbar);
        if (dist < bestDist) {
            bestDist = dist;
            best.offsetPx = c.d; best.dThetaDeg = c.dd;
            best.bestLineConf = float(std::clamp(c.sup, 0.0, 1.0));
        }
    }
    return best;
}

} // namespace

// ── config ───────────────────────────────────────────────────────────────────
ShaftV3Config ShaftV3Config::fromOverrides(const QVariantMap& ov)
{
    using namespace tuning;
    ShaftV3Config c;
    apply(ov, "shaft.grid", c.grid);
    apply(ov, "shaft.wmaxBackswing", c.wmaxBackswing);
    apply(ov, "shaft.wmaxDownswing", c.wmaxDownswing);
    apply(ov, "shaft.wmaxImpact", c.wmaxImpact);
    apply(ov, "shaft.wmaxThru", c.wmaxThru);
    apply(ov, "shaft.wmaxFinish", c.wmaxFinish);
    apply(ov, "shaft.wE2", c.wE2);
    apply(ov, "shaft.wBand", c.wBand);
    apply(ov, "shaft.wArm", c.wArm);
    apply(ov, "shaft.wC1", c.wC1);
    apply(ov, "shaft.wC2", c.wC2);
    apply(ov, "shaft.wCone", c.wCone);
    apply(ov, "shaft.kSmooth", c.kSmooth);
    apply(ov, "shaft.coneHalf", c.coneHalf);
    apply(ov, "shaft.c1Tol", c.c1Tol);
    apply(ov, "shaft.rayEvMin", c.rayEvMin);
    apply(ov, "shaft.evAbsFloor", c.evAbsFloor);
    apply(ov, "shaft.evAbsFloorDif", c.evAbsFloorDif);
    apply(ov, "shaft.raySupportMin", c.raySupportMin);
    apply(ov, "shaft.bandTol", c.bandTol);
    apply(ov, "shaft.armVetoDeg", c.armVetoDeg);
    apply(ov, "shaft.stillSpeed", c.stillSpeed);
    apply(ov, "shaft.stillMin", c.stillMin);
    apply(ov, "shaft.bandNear", c.bandNear);
    apply(ov, "shaft.spanCollarUs", c.spanCollarUs);
    apply(ov, "shaft.addressCollarUs", c.addressCollarUs);
    apply(ov, "shaft.swLow", c.swLow);
    apply(ov, "shaft.phiOnsetDegPerFrame", c.phiOnsetDegPerFrame);
    apply(ov, "shaft.bsMinBeforeImpactUs", c.bsMinBeforeImpactUs);
    apply(ov, "shaft.bsMaxBeforeImpactUs", c.bsMaxBeforeImpactUs);
    apply(ov, "shaft.onsetReturnBoxPx", c.onsetReturnBoxPx);
    apply(ov, "shaft.onsetReturnGapFrames", c.onsetReturnGapFrames);
    apply(ov, "shaft.onsetRunBridgeFrames", c.onsetRunBridgeFrames);
    apply(ov, "shaft.onsetBridgeMinNetFrac", c.onsetBridgeMinNetFrac);
    apply(ov, "shaft.emitTakeaway", c.emitTakeaway);
    apply(ov, "shaft.runMaxStartAfterImpactUs", c.runMaxStartAfterImpactUs);
    apply(ov, "shaft.spanBound", c.spanBound);
    apply(ov, "shaft.bodyMargin", c.bodyMargin);
    apply(ov, "shaft.rasterC2", c.rasterC2);
    apply(ov, "shaft.psiRail", c.psiRail);
    apply(ov, "shaft.armOutlierDeg", c.armOutlierDeg);
    apply(ov, "shaft.wIsoBand", c.wIsoBand);
    apply(ov, "shaft.wIsoRay", c.wIsoRay);
    apply(ov, "shaft.wIsoPred", c.wIsoPred);
    apply(ov, "shaft.isoHuber", c.isoHuber);
    apply(ov, "shaft.isoIters", c.isoIters);
    apply(ov, "shaft.reconTol", c.reconTol);
    apply(ov, "shaft.psiWinBack", c.psiWinBack);
    apply(ov, "shaft.psiWinFwd", c.psiWinFwd);
    apply(ov, "shaft.swSpd", c.swSpd);
    apply(ov, "shaft.impHalf", c.impHalf);
    apply(ov, "shaft.coverageMin", c.coverageMin);
    apply(ov, "shaft.lenStatureM", c.lenStatureM);
    apply(ov, "shaft.lenGripDownM", c.lenGripDownM);
    // evidence engines
    apply(ov, "shaft.rStep", c.ridge.rStep);
    apply(ov, "shaft.rHi", c.ridge.rHi);
    apply(ov, "shaft.bgHi", c.ridge.bgHi);
    apply(ov, "shaft.minLenPx", c.ridge.minLenPx);
    apply(ov, "shaft.satT", c.band.satT);
    apply(ov, "shaft.gripGate", c.band.gripGate);
    // Stage-2 measured-clubhead (Phase B): "shaft.head.*" keys. Kept a separate
    // sub-parse (clubhead_track.cpp) so the head parameter set travels with its
    // module; still default enabled=false (dark at merge).
    c.head = ClubheadConfig::fromOverrides(ov);
    // Multi-estimator club-length fusion: "fusion.*" keys (club_length_fusion.h).
    c.fusion = LengthFusionConfig::fromOverrides(ov);
    // R8-T1 blur-wedge (S2): "shaft.wedge.*" keys (shaft_wedge.h).
    apply(ov, "shaft.wedge.enabled", c.wedge.enabled);
    apply(ov, "shaft.wedge.omegaMinDegS", c.wedge.omegaMinDegS);
    apply(ov, "shaft.wedge.tExpBootstrapS", c.wedge.tExpBootstrapS);
    apply(ov, "shaft.wedge.calOmegaLoDegS", c.wedge.calOmegaLoDegS);
    apply(ov, "shaft.wedge.kSigma", c.wedge.kSigma);
    apply(ov, "shaft.wedge.threshScale", c.wedge.threshScale);
    apply(ov, "shaft.wedge.minSpanDeg", c.wedge.minSpanDeg);
    apply(ov, "shaft.wedge.proximalRHiFrac", c.wedge.proximalRHiFrac);
    apply(ov, "shaft.wedge.proximalMinLenPx", c.wedge.proximalMinLenPx);
    apply(ov, "shaft.wedge.wWell", c.wedge.wWell);
    apply(ov, "shaft.wedge.conf", c.wedge.conf);
    apply(ov, "shaft.wedge.dpTolDeg", c.wedge.dpTolDeg);
    apply(ov, "shaft.wedge.kinCone", c.wedge.kinCone);
    apply(ov, "shaft.wedge.wKinCone", c.wedge.wKinCone);
    // Layer A line re-registration («snap»): "shaft.snap.*" keys.
    apply(ov, "shaft.snap.enabled", c.snap.enabled);
    apply(ov, "shaft.snap.maxOffsetPx", c.snap.maxOffsetPx);
    apply(ov, "shaft.snap.maxDeltaDeg", c.snap.maxDeltaDeg);
    apply(ov, "shaft.snap.minLineConf", c.snap.minLineConf);
    apply(ov, "shaft.snap.corridorHalfPx", c.snap.corridorHalfPx);
    // Layer B P-position extraction: "positions.*" keys.
    apply(ov, "positions.enabled", c.positions.enabled);
    apply(ov, "positions.hysteresisDeg", c.positions.hysteresisDeg);
    apply(ov, "positions.p1StillNetPx", c.positions.p1StillNetPx);
    apply(ov, "positions.p1StillSpeedPx", c.positions.p1StillSpeedPx);
    apply(ov, "positions.p1StillWindow", c.positions.p1StillWindow);
    apply(ov, "positions.p1ClubQuietSigma", c.positions.p1ClubQuietSigma);   // W3 club-quiet gate
    apply(ov, "positions.p6LastCrossing", c.positions.p6LastCrossing);       // P6 = last transit pre-impact
    // Layer B milestone fit (B2): "positions.*" keys → PositionsConfig::fit.
    apply(ov, "positions.fitEnabled", c.positions.fit.fitEnabled);
    apply(ov, "positions.skipMeasuredConf", c.positions.fit.skipMeasuredConf);
    apply(ov, "positions.useFusedLen", c.positions.fit.useFusedLen);
    apply(ov, "positions.halfWindowFrames", c.positions.fit.halfWindowFrames);
    apply(ov, "positions.minFitConf", c.positions.fit.minFitConf);
    apply(ov, "positions.wideSectorDeg", c.positions.fit.wideSectorDeg);
    apply(ov, "positions.narrowSectorDeg", c.positions.fit.narrowSectorDeg);
    apply(ov, "positions.gripSearchPx", c.positions.fit.gripSearchPx);
    apply(ov, "positions.thetaStepDeg", c.positions.fit.thetaStepDeg);
    apply(ov, "positions.lenStepPx", c.positions.fit.lenStepPx);
    apply(ov, "positions.lenMinFracH", c.positions.fit.lenMinFracH);
    apply(ov, "positions.lenMaxFracH", c.positions.fit.lenMaxFracH);
    apply(ov, "positions.gripStepPx", c.positions.fit.gripStepPx);
    apply(ov, "positions.corridorHalfPx", c.positions.fit.corridorHalfPx);
    apply(ov, "positions.ballBonus", c.positions.fit.ballBonus);
    apply(ov, "positions.ballSigmaPx", c.positions.fit.ballSigmaPx);
    apply(ov, "positions.plateauFrac", c.positions.fit.plateauFrac);
    // Layer C synthesis between anchors: "synth.*" keys.
    apply(ov, "synth.enabled", c.synth.enabled);
    apply(ov, "synth.midConfFrac", c.synth.midConfFrac);
    apply(ov, "synth.rateHz", c.synth.rateHz);
    // Hand-axis θ prior (WB4): "shaft.handAxisPrior.*" keys.
    apply(ov, "shaft.handAxisPrior.enabled", c.handAxisPrior.enabled);
    apply(ov, "shaft.handAxisPrior.weight",  c.handAxisPrior.weight);
    apply(ov, "shaft.handAxisPrior.confMin", c.handAxisPrior.confMin);
    apply(ov, "shaft.handAxisPrior.maxDeg",  c.handAxisPrior.maxDeg);
    return c;
}

// ── φ smoothing (club_track_v3.smooth_phi) ───────────────────────────────────
std::vector<double> smoothPhi(const std::vector<double>& phiDeg, const ShaftV3Config& cfg)
{
    const int n = int(phiDeg.size());
    constexpr int win = 9;
    std::vector<double> c(n), s(n);
    for (int i = 0; i < n; ++i) { const double r = phiDeg[i] * kPi / 180.0; c[i] = std::cos(r); s[i] = std::sin(r); }
    const std::vector<double> cm = medianFilter1d(c, win);
    const std::vector<double> sm = medianFilter1d(s, win);
    for (int i = 0; i < n; ++i) {
        const double dev = std::abs(std::atan2(s[i] * cm[i] - c[i] * sm[i], c[i] * cm[i] + s[i] * sm[i]) * 180.0 / kPi);
        if (dev > cfg.armOutlierDeg) { c[i] = cm[i]; s[i] = sm[i]; }
    }
    const std::vector<double> cx = gaussianFilter1d(medianFilter1d(c, win), 3.0);
    const std::vector<double> cy = gaussianFilter1d(medianFilter1d(s, win), 3.0);
    std::vector<double> out(n);
    for (int i = 0; i < n; ++i) out[i] = std::atan2(cy[i], cx[i]) * 180.0 / kPi;
    return out;
}

// ── hands-only phase model (club_track_v3.segment_phases) ────────────────────
PhaseModel segmentPhases(const std::vector<double>& gx, const std::vector<double>& gy,
                         int nf, double fps, int impactFrame, const ShaftV3Config& cfg,
                         const std::vector<double>* phiSmoothed)
{
    PhaseModel m;
    std::vector<double> spd(nf, 0.0);
    for (int f = 1; f < nf; ++f) spd[f] = std::hypot(gx[f] - gx[f - 1], gy[f] - gy[f - 1]);
    m.spdSmoothed = gaussianFilter1d(medianFilter1d(spd, 5), 2.0);
    const auto& spdS = m.spdSmoothed;

    std::vector<char> mo(nf);
    for (int f = 0; f < nf; ++f) mo[f] = spdS[f] > cfg.swSpd;

    std::vector<std::pair<int, int>> runs;
    for (int f = 0; f < nf;) {
        if (mo[f]) {
            int g = f;
            while (g + 1 < nf && mo[g + 1]) ++g;
            if (g - f >= 6) runs.emplace_back(f, g);
            f = g + 1;
        } else ++f;
    }
    // Run bridging (dark: 0 = off): merge min-length-QUALIFIED runs separated by
    // fewer than onsetRunBridgeFrames quiet frames, BEFORE the two-longest
    // ranking. A slow real backswing fragments into short bursts on the
    // lerped-pose speed profile and loses the two-longest race to a
    // follow-through fragment — bs0 then lands at the top/downswing (w2s4-class
    // mis-pick). Deliberately AFTER the >=7-frame filter: letting sub-7 waggle
    // bursts participate chains a fidget waggle cluster into a false "run" that
    // wins the race and parks bs0 mid-fidget, disabling the no-return veto
    // (observed on the w2s6 dump — veto-only +13 ms, sub-7-chained bridging
    // regressed it to the unfixed -214 ms).
    // chain[i] = how many RAW runs were folded into runs[i] by bridging (1 = an
    // unmerged run). Carried through the clamp so the m3gate below can tell a
    // long oscillation chain from an ordinary run or a 2-fragment merge.
    std::vector<int> chain(runs.size(), 1);
    if (cfg.onsetRunBridgeFrames > 0 && runs.size() > 1) {
        std::vector<std::pair<int, int>> merged{ runs.front() };
        std::vector<int> mChain{ 1 };
        for (size_t i = 1; i < runs.size(); ++i) {
            if (runs[i].first - merged.back().second - 1 < cfg.onsetRunBridgeFrames) {
                merged.back().second = runs[i].second;
                ++mChain.back();
            } else {
                merged.push_back(runs[i]);
                mChain.push_back(1);
            }
        }
        runs = std::move(merged);
        chain = std::move(mChain);
    }
    // Run-candidacy clamp: with a supplied impact, a run that STARTS more than
    // runMaxStartAfterImpactUs after it cannot be backswing or downswing — drop
    // it so post-finish motion at the window tail never wins a two-longest slot
    // against a fragmented backswing. Keep the unfiltered list if the clamp
    // would empty it (degenerate window; preserves the legacy fallback below).
    if (impactFrame >= 0 && cfg.runMaxStartAfterImpactUs > 0) {
        const int maxStart = impactFrame
                             + int(std::lround(double(cfg.runMaxStartAfterImpactUs) * 1e-6 * fps));
        std::vector<std::pair<int, int>> kept;
        std::vector<int> keptChain;
        for (size_t i = 0; i < runs.size(); ++i)
            if (runs[i].first <= maxStart) { kept.push_back(runs[i]); keptChain.push_back(chain[i]); }
        if (!kept.empty()) { runs = std::move(kept); chain = std::move(keptChain); }
    }
    if (runs.empty()) {
        m.phase.assign(nf, SwingPhase::Addr);
        m.bs0 = 0; m.top = nf / 2; m.impact = nf / 2; m.fin0 = nf - 1;
        return m;
    }
    // m3gate (FROZEN ON 2026-07-18 at 0.2; 0 = off) — chain-qualified
    // net-displacement gate on the two-longest RANKING. A grip-anchor
    // oscillation cluster (s0002's
    // presentation-move pose flapping: seven 7–8-frame >swSpd runs, gaps 4–6)
    // bridges into a run long enough to win the ranking while going NOWHERE
    // (net 11 px over an 824 px path, net/path 0.013) — bs0 then lands in the
    // flap and the A3 far edge pins Takeaway at impact − bsMax. A bridged run
    // assembled from >= 3 raw runs enters the ranking only if its smoothed net
    // displacement >= onsetBridgeMinNetFrac × its path length. The >= 3-chain
    // qualifier is a FIXED structural rule, not a knob: m = 2 merges are the
    // frozen w2s4 evidence (fragmented backswing rescue, and the downswing +
    // follow-through merge whose reversal legitimately nets low, ratio 0.08) —
    // permanently exempt. Degenerate fallback: if the gate would empty the
    // candidate list, rank the ungated list (never fewer candidates than
    // ungated reach the ranking floor of the legacy behaviour).
    if (cfg.onsetBridgeMinNetFrac > 0.0) {
        bool anyLongChain = false;
        for (int c : chain) anyLongChain = anyLongChain || c >= 3;
        if (anyLongChain) {
            // Smoothed grip endpoints (gauss(median5), the spd/joint convention);
            // path = raw per-frame travel over the run.
            const std::vector<double> gxS = gaussianFilter1d(medianFilter1d(gx, 5), 2.0);
            const std::vector<double> gyS = gaussianFilter1d(medianFilter1d(gy, 5), 2.0);
            std::vector<std::pair<int, int>> kept;
            for (size_t i = 0; i < runs.size(); ++i) {
                bool ok = chain[i] < 3;
                if (!ok) {
                    const double net = std::hypot(gxS[runs[i].second] - gxS[runs[i].first],
                                                  gyS[runs[i].second] - gyS[runs[i].first]);
                    double path = 0.0;
                    for (int f = runs[i].first + 1; f <= runs[i].second; ++f) path += spd[f];
                    ok = net >= cfg.onsetBridgeMinNetFrac * path;
                }
                if (ok) kept.push_back(runs[i]);
            }
            if (!kept.empty()) runs = std::move(kept);
        }
    }
    std::stable_sort(runs.begin(), runs.end(),
                     [](const std::pair<int, int>& a, const std::pair<int, int>& b) {
                         return (a.second - a.first) > (b.second - b.first);
                     });
    std::vector<std::pair<int, int>> big(runs.begin(), runs.begin() + std::min<size_t>(2, runs.size()));
    std::stable_sort(big.begin(), big.end(),
                     [](const std::pair<int, int>& a, const std::pair<int, int>& b) { return a.first < b.first; });
    const int bs0 = big[0].first;
    int top, dsEnd;
    if (big.size() >= 2) {
        const int gapLo = big[0].second, gapHi = big[1].first;
        if (gapHi > gapLo) {
            int amin = gapLo; for (int f = gapLo; f <= gapHi; ++f) if (spdS[f] < spdS[amin]) amin = f;
            top = amin;
        } else top = big[0].second;
        dsEnd = big[1].second;
    } else {
        int amax = bs0; for (int f = bs0; f <= big[0].second; ++f) if (-gy[f] > -gy[amax]) amax = f;
        top = amax;
        dsEnd = big[0].second;
    }
    // address grip height = median(gy[:max(bs0,1)])
    std::vector<double> head(gy.begin(), gy.begin() + std::max(bs0, 1));
    std::nth_element(head.begin(), head.begin() + head.size() / 2, head.end());
    double addrGy = head[head.size() / 2];
    if (head.size() % 2 == 0) {   // even count ⇒ mean of the two middle (np.median)
        double hi = head[head.size() / 2];
        double lo = *std::max_element(head.begin(), head.begin() + head.size() / 2);
        addrGy = 0.5 * (lo + hi);
    }
    int impf = impactFrame;
    if (impf < 0) {
        impf = (top + dsEnd) / 2;
        for (int f = top; f <= dsEnd; ++f) if (gy[f] >= addrGy - 20.0) { impf = f; break; }
    }
    impf = std::clamp(impf, top + 1, nf - 1);
    const int fin0 = std::min(dsEnd, nf - 1);
    const int IMP = cfg.impHalf;

    // Stage A true-onset walk-back (swing_span_bounding_plan.md §4). The
    // high-swSpd run picked bs0, which lags the real takeaway (grip still
    // rotating slowly about the wrist). swLow <= 0 disables the whole block —
    // onset stays at bs0, reproducing the pre-Stage-A boundary bit-for-bit.
    int onset = bs0;
    if (cfg.swLow > 0.0) {
        // A1: walk bs0 back to the first smoothed-speed frame below swLow.
        int onsetSpd = bs0;
        while (onsetSpd > 0 && spdS[onsetSpd] >= cfg.swLow) --onsetSpd;
        onset = onsetSpd;
        // A2: φ-onset witness — the lead forearm rotates before the grip moves.
        // Smooth wrap-aware |Δφ| the same way as speed (median5 + gauss2) and
        // walk back from the ORIGINAL bs0 while it stays above threshold.
        if (phiSmoothed && cfg.phiOnsetDegPerFrame > 0.0 && int(phiSmoothed->size()) == nf) {
            const std::vector<double>& phi = *phiSmoothed;
            std::vector<double> dphi(nf, 0.0);
            for (int f = 1; f < nf; ++f) dphi[f] = std::abs(circWrap(phi[f] - phi[f - 1]));
            const std::vector<double> dphiS = gaussianFilter1d(medianFilter1d(dphi, 5), 2.0);
            int onsetPhi = bs0;
            while (onsetPhi > 0 && dphiS[onsetPhi] > cfg.phiOnsetDegPerFrame) --onsetPhi;
            onset = std::min(onset, onsetPhi);
        }
        // No-return veto (fidget-proofing). A1/A2 walk back through ANY
        // sub-threshold motion — and on real capture the lerped-pose grip keeps
        // a 2–4 px/f smoothed-speed floor through every fidget settle, so the
        // walk-back runs 0.5–1.5 s past the true takeaway to the deep pre-fidget
        // stillness. This pass can only move the onset LATER, via a DEPARTURE-
        // REFERENCED revisit scan: a frame the track later comes back to (within
        // onsetReturnBoxPx, at least onsetReturnGapFrames ahead — excluding the
        // immediate dwell) is pre-departure fidget; the LAST such frame is the
        // no-return boundary, after which the grip departs for good (the
        // takeaway). No address anchor and no absolute-rest gate — the golfer
        // settles into an address DISPLACED from the pre-fidget stance, and true
        // grip rest never happens on the lerped signal (both premises of the
        // retired anchor-box veto were unsatisfiable on the 17-swing truth set).
        // The scan horizon is bs0 — the selected (post-bridging) run start, the
        // takeaway-run candidate — so the revisit test never sees the top dwell
        // (which revisits itself); onsetRunBridgeFrames is what makes bs0 the
        // true takeaway run on fragmented backswings, and the A3 clamp below is
        // the backstop for an unbridged mis-pick. onsetReturnBoxPx <= 0 disables
        // the whole block (the swLow<=0 dark idiom) — byte-identical onset.
        int noReturnBoundary = -1;   // → m.onsetFloor (the last settle) when the scan fires
        if (cfg.onsetReturnBoxPx > 0.0) {
            const int gap = std::max(1, cfg.onsetReturnGapFrames);
            if (bs0 - gap > onset) {
                // Smoothed grip position (gauss(median5), the spd/joint convention).
                const std::vector<double> gxS = gaussianFilter1d(medianFilter1d(gx, 5), 2.0);
                const std::vector<double> gyS = gaussianFilter1d(medianFilter1d(gy, 5), 2.0);
                int boundary = -1;
                for (int r = onset; r <= bs0 - gap; ++r) {
                    double mn = std::numeric_limits<double>::infinity();
                    for (int f = r + gap; f <= bs0; ++f)
                        mn = std::min(mn, std::hypot(gxS[f] - gxS[r], gyS[f] - gyS[r]));
                    if (mn < cfg.onsetReturnBoxPx) boundary = r;
                }
                noReturnBoundary = boundary;
                if (boundary > onset) onset = boundary;
            }
        }
        // A3: impact-anchored clamp (the safety rail). Only when a real impact
        // frame was supplied — a hands-derived impf must not clamp its own
        // onset. Violations pin to the violated edge.
        if (impactFrame >= 0) {
            const int framesMin = int(std::lround(double(cfg.bsMinBeforeImpactUs) * 1e-6 * fps));
            const int framesMax = int(std::lround(double(cfg.bsMaxBeforeImpactUs) * 1e-6 * fps));
            const int loEdge = std::max(0, impf - framesMax);
            const int hiEdge = std::max(0, impf - framesMin);
            onset = std::clamp(onset, loEdge, hiEdge);
        }
        // Publish the no-return boundary as the address-walk-back floor
        // (PhaseModel::onsetFloor contract). min(boundary, onset): when A3
        // pushed the onset EARLIER than the boundary the floor follows the
        // onset (a floor above the walk-back start would be unreachable);
        // when A3 pushed it later, the boundary — the last settle — stands.
        if (noReturnBoundary >= 0) m.onsetFloor = std::min(noReturnBoundary, onset);
    }

    m.phase.resize(nf);
    for (int f = 0; f < nf; ++f) {
        if (f < onset) m.phase[f] = SwingPhase::Addr;
        else if (f < top - 2) m.phase[f] = SwingPhase::Backswing;
        else if (f <= top + 2) m.phase[f] = SwingPhase::Top;
        else if (std::abs(f - impf) <= IMP) m.phase[f] = SwingPhase::Impact;
        else if (f < impf) m.phase[f] = SwingPhase::Downswing;
        else if (f <= fin0) m.phase[f] = SwingPhase::Thru;
        else m.phase[f] = SwingPhase::Finish;
    }
    m.bs0 = onset; m.top = top; m.impact = impf; m.fin0 = fin0;
    return m;
}

// ── joint smoothing + body geometry ─────────────────────────────────────────
std::vector<std::vector<cv::Point2d>>
smoothJoints(const std::vector<std::vector<cv::Point2d>>& raw)
{
    const int nf = int(raw.size());
    if (nf == 0) return {};
    const int nj = int(raw[0].size());
    std::vector<std::vector<cv::Point2d>> out(nf, std::vector<cv::Point2d>(nj));
    for (int j = 0; j < nj; ++j) {
        std::vector<double> xs(nf), ys(nf);
        for (int f = 0; f < nf; ++f) { xs[f] = raw[f][j].x; ys[f] = raw[f][j].y; }
        xs = gaussianFilter1d(medianFilter1d(xs, 5), 2.0);
        ys = gaussianFilter1d(medianFilter1d(ys, 5), 2.0);
        for (int f = 0; f < nf; ++f) out[f][j] = {xs[f], ys[f]};
    }
    return out;
}

std::vector<BodyPoly> bodyPolys(const std::vector<std::vector<cv::Point2d>>& joints)
{
    std::vector<BodyPoly> polys;
    if (joints.empty() || joints[0].empty()) return polys;
    polys.resize(joints.size());
    for (size_t f = 0; f < joints.size(); ++f) {
        std::vector<cv::Point2f> pts;
        pts.reserve(joints[f].size());
        for (const auto& p : joints[f]) pts.emplace_back(float(p.x), float(p.y));
        std::vector<cv::Point2f> hullf;
        cv::convexHull(pts, hullf);
        const int E = int(hullf.size());
        std::vector<cv::Point2d> hull(E);
        cv::Point2d c(0, 0);
        for (int i = 0; i < E; ++i) { hull[i] = {double(hullf[i].x), double(hullf[i].y)}; c += hull[i]; }
        c *= 1.0 / E;
        BodyPoly bp; bp.n.resize(E); bp.d.resize(E);
        for (int i = 0; i < E; ++i) {
            const cv::Point2d e = hull[(i + 1) % E] - hull[i];     // edge vector
            cv::Vec2d nrm(e.y, -e.x);
            nrm /= (std::hypot(nrm[0], nrm[1]) + 1e-9);
            const cv::Point2d mid = hull[i] + 0.5 * e - c;
            if (nrm[0] * mid.x + nrm[1] * mid.y < 0) nrm *= -1.0;  // orient outward
            bp.n[i] = nrm;
            bp.d[i] = nrm[0] * hull[i].x + nrm[1] * hull[i].y;
        }
        polys[f] = std::move(bp);
    }
    return polys;
}

std::vector<cv::Mat> bodyMasks(const std::vector<std::vector<cv::Point2d>>& joints,
                               int W, int H, const ShaftV3Config& cfg)
{
    std::vector<cv::Mat> masks;
    if (joints.empty() || joints[0].empty()) return masks;
    const int k = int(cfg.bodyMargin);
    const cv::Mat se = cv::getStructuringElement(cv::MORPH_ELLIPSE, {2 * k + 1, 2 * k + 1});
    masks.resize(joints.size());
    for (size_t f = 0; f < joints.size(); ++f) {
        std::vector<cv::Point2f> pts;
        for (const auto& p : joints[f]) pts.emplace_back(float(p.x), float(p.y));
        std::vector<cv::Point2f> hullf;
        cv::convexHull(pts, hullf);
        std::vector<cv::Point> hull;
        for (const auto& p : hullf) hull.emplace_back(int(p.x), int(p.y));
        cv::Mat m = cv::Mat::zeros(H, W, CV_8UC1);
        cv::fillConvexPoly(m, hull, cv::Scalar(255));
        cv::dilate(m, m, se);
        masks[f] = m;
    }
    return masks;
}

std::vector<char> staticRuns(const std::vector<double>& spdS, const ShaftV3Config& cfg)
{
    const int nf = int(spdS.size());
    std::vector<char> stat(nf, 0);
    for (int f = 0; f < nf;) {
        if (spdS[f] < cfg.stillSpeed) {
            int g = f;
            while (g + 1 < nf && spdS[g + 1] < cfg.stillSpeed) ++g;
            if (g - f + 1 >= cfg.stillMin) for (int k = f; k <= g; ++k) stat[k] = 1;
            f = g + 1;
        } else ++f;
    }
    return stat;
}

// ── per-frame DP emission (club_track_v3 main loop, one frame) ───────────────
void frameEmission(std::vector<float>& emOut, std::vector<float>& insideOut,
                   const std::vector<float>& evMax, const std::vector<float>& rawNorm,
                   const BandMatch& band, double phiSDeg, SwingPhase phase, int chir,
                   double gx, double gy, const BodyPoly* poly, const cv::Mat& mask,
                   const std::vector<float>& gridRad, const std::vector<float>& gridDeg,
                   const ShaftV3Config& cfg, double handAxisDeg, double handAxisConf)
{
    const int NS = int(evMax.size());
    emOut.assign(NS, 0.f);
    insideOut.assign(NS, 0.f);

    // ev copy with the band bin raised (E1 reward) — EV[] stored elsewhere is
    // the pre-raise evMax, so this raise is local to the emission.
    std::vector<float> ev = evMax;
    int bi = -1;
    if (band.ok) {
        bi = int(std::lround(band.thetaDeg / cfg.grid)) % NS;
        if (bi < 0) bi += NS;
        ev[bi] = std::max(ev[bi], 1.0f);
    }
    for (int k = 0; k < NS; ++k) emOut[k] = float(cfg.wE2 * (1.0 - ev[k]));

    // C4 arm-veto: shaft never points INTO the lead forearm (φ+180)
    const double arm = std::fmod(phiSDeg + 180.0, 360.0);
    for (int k = 0; k < NS; ++k)
        if (std::abs(circWrap(gridDeg[k] - arm)) < cfg.armVetoDeg) emOut[k] += float(cfg.wArm);

    // C4 wide reachable cone (chirality-centred), off addr/finish/top
    if (phase != SwingPhase::Addr && phase != SwingPhase::Finish && phase != SwingPhase::Top) {
        const double cen = phiSDeg + chir * (cfg.coneHalf - 40.0);
        for (int k = 0; k < NS; ++k)
            if (std::abs(circWrap(gridDeg[k] - cen)) > cfg.coneHalf) emOut[k] += float(cfg.wCone);
    }

    // C1 (weak form): reverse ridge OFF the forearm = scene line
    const double phiMod = std::fmod(std::fmod(phiSDeg, 360.0) + 360.0, 360.0);
    for (int k = 0; k < NS; ++k) {
        const double rev = rawNorm[(k + NS / 2) % NS];
        const double revArm = std::abs(circWrap(gridDeg[k] - phiMod));
        if (rev > cfg.c1Tol && revArm > cfg.armVetoDeg) emOut[k] += float(cfg.wC1);
    }

    // C2 body-overlap veto (mid-swing only)
    const bool haveC2 = (poly != nullptr) || !mask.empty();
    if (isMidswing(phase) && haveC2) {
        std::vector<double> BR;
        for (double r = cfg.bodyRLo; r < cfg.bodyRHi; r += cfg.bodyRStep) BR.push_back(r);
        const int nR = int(BR.size());
        const int W = mask.empty() ? 0 : mask.cols;
        const int H = mask.empty() ? 0 : mask.rows;
        for (int k = 0; k < NS; ++k) {
            const double ux = std::cos(double(gridRad[k])), uy = std::sin(double(gridRad[k]));
            int insideCount = 0;
            for (int r = 0; r < nR; ++r) {
                const double px = gx + ux * BR[r], py = gy + uy * BR[r];
                bool in;
                if (poly) {
                    double sdmax = -1e30;
                    for (size_t e = 0; e < poly->n.size(); ++e) {
                        const double sd = poly->n[e][0] * px + poly->n[e][1] * py - poly->d[e];
                        if (sd > sdmax) sdmax = sd;
                    }
                    in = (sdmax <= cfg.bodyMargin);
                } else {
                    int xi = int(px); if (xi < 0) xi = 0; else if (xi >= W) xi = W - 1;
                    int yi = int(py); if (yi < 0) yi = 0; else if (yi >= H) yi = H - 1;
                    in = (mask.at<uchar>(yi, xi) > 0);
                }
                if (in) ++insideCount;
            }
            const double frac = double(insideCount) / nR;
            insideOut[k] = float(frac);
            if (frac > 0.5) emOut[k] += float(cfg.wC2);
        }
    }

    // WB4 hand-axis θ prior — penalise states far from the grip hand axis. Before
    // the band well so the well still dominates; skipped entirely when disabled.
    addHandAxisPrior(emOut, gridDeg, handAxisDeg, handAxisConf, cfg.handAxisPrior);

    // band negative well LAST — dominates the gates, forces the global path
    if (band.ok) emOut[bi] = float(-cfg.wBand);
}

// ── WB4 hand-axis θ prior ────────────────────────────────────────────────────
void addHandAxisPrior(std::vector<float>& emOut, const std::vector<float>& gridDeg,
                      double handAxisDeg, double handAxisConf, const HandAxisPriorConfig& cfg)
{
    if (!cfg.enabled || handAxisConf < cfg.confMin || std::isnan(handAxisDeg))
        return;   // guarded ENTIRELY — emOut is bit-identical in every off case
    const float pen = float(cfg.weight * handAxisConf);
    const int NS = int(emOut.size());
    for (int k = 0; k < NS; ++k)
        if (std::abs(circWrap(double(gridDeg[k]) - handAxisDeg)) > cfg.maxDeg)
            emOut[k] += pen;
}

// ── global banded Viterbi DP (club_track_v3 C3) ──────────────────────────────
DPResult viterbiDP(const std::vector<std::vector<float>>& emis,
                   const std::vector<SwingPhase>& phase, const ShaftV3Config& cfg)
{
    const int nf = int(emis.size());
    DPResult out;
    if (nf == 0) return out;
    const int NS = int(emis[0].size());

    std::vector<double> cost(emis[0].begin(), emis[0].end());
    std::vector<std::vector<int>> back(nf, std::vector<int>(NS, 0));

    std::vector<double> best(NS), tmp(NS);
    std::vector<int> barg(NS);
    for (int f = 1; f < nf; ++f) {
        const int wmax = wmaxFor(phase[f], cfg);
        const int sgn = phaseSign(phase[f]);
        const int dLo = (sgn > 0) ? 0 : -wmax;
        const int dHi = (sgn < 0) ? 0 : wmax;
        std::fill(best.begin(), best.end(), kInf);
        std::fill(barg.begin(), barg.end(), 0);
        for (int d = dLo; d <= dHi; ++d) {
            const double t = cfg.kSmooth * (d * cfg.grid) * (d * cfg.grid);
            for (int k = 0; k < NS; ++k) {
                const int src = ((k - d) % NS + NS) % NS;
                const double cand = cost[src] + t;
                if (cand < best[k]) { best[k] = cand; barg[k] = src; }
            }
        }
        for (int k = 0; k < NS; ++k) { cost[k] = best[k] + emis[f][k]; back[f][k] = barg[k]; }
    }

    out.thstar.assign(nf, 0);
    int last = 0; for (int k = 1; k < NS; ++k) if (cost[k] < cost[last]) last = k;
    out.thstar[nf - 1] = last;
    for (int f = nf - 1; f > 0; --f) out.thstar[f - 1] = back[f][out.thstar[f]];
    out.thetaDeg.resize(nf);
    for (int f = 0; f < nf; ++f) out.thetaDeg[f] = out.thstar[f] * cfg.grid;
    return out;
}

// ── ψ-isotonic reconciliation ────────────────────────────────────────────────
std::vector<double> pava(const std::vector<double>& y, const std::vector<double>& w, bool increasing)
{
    const double sgn = increasing ? 1.0 : -1.0;
    const int n = int(y.size());
    std::vector<double> means(n), wts(w);
    std::vector<std::vector<int>> idx(n);
    for (int i = 0; i < n; ++i) { means[i] = sgn * y[i]; idx[i] = {i}; }
    int i = 0;
    while (i < int(means.size()) - 1) {
        if (means[i] > means[i + 1] + 1e-12) {
            const double nw = wts[i] + wts[i + 1];
            means[i] = (means[i] * wts[i] + means[i + 1] * wts[i + 1]) / nw;
            wts[i] = nw;
            idx[i].insert(idx[i].end(), idx[i + 1].begin(), idx[i + 1].end());
            means.erase(means.begin() + i + 1);
            wts.erase(wts.begin() + i + 1);
            idx.erase(idx.begin() + i + 1);
            if (i > 0) --i;
        } else ++i;
    }
    std::vector<double> out(n, 0.0);
    for (size_t b = 0; b < means.size(); ++b)
        for (int k : idx[b]) out[k] = sgn * means[b];
    return out;
}

std::vector<double> robustIsotonic(const std::vector<double>& y, const std::vector<double>& w,
                                   bool increasing, const ShaftV3Config& cfg)
{
    std::vector<double> x = pava(y, w, increasing);
    for (int it = 0; it < cfg.isoIters; ++it) {
        std::vector<double> hw(y.size());
        for (size_t i = 0; i < y.size(); ++i) {
            const double r = std::abs(y[i] - x[i]);
            hw[i] = w[i] * (r <= cfg.isoHuber ? 1.0 : cfg.isoHuber / (r + 1e-9));
        }
        x = pava(y, hw, increasing);
    }
    return x;
}

ReconResult reconcilePsi(const std::vector<double>& thetaDeg, const std::vector<double>& phiS,
                         const std::vector<SwingPhase>& phase, const std::vector<char>& bandOk,
                         const std::vector<double>& evAt, int top, int nf, const ShaftV3Config& cfg)
{
    ReconResult rr;
    rr.thetaOut = thetaDeg;
    rr.psiResid.assign(nf, std::numeric_limits<double>::quiet_NaN());
    rr.recon.assign(nf, 0);
    const int lo = top - cfg.psiWinBack, hi = top + cfg.psiWinFwd;

    struct Block { std::vector<SwingPhase> phs; bool inc; };
    const Block blocks[2] = {
        {{SwingPhase::Backswing}, true},
        {{SwingPhase::Downswing, SwingPhase::Impact, SwingPhase::Thru}, false},
    };
    auto inBlock = [](SwingPhase p, const std::vector<SwingPhase>& set) {
        for (SwingPhase q : set) if (q == p) return true;
        return false;
    };
    auto weight = [&](int f) {
        if (!bandOk[f] && phase[f] == SwingPhase::Impact) return cfg.wIsoPred;   // RECON_PHASES=(impact,)
        if (bandOk[f]) return cfg.wIsoBand;
        return evAt[f] >= cfg.rayEvMin ? cfg.wIsoRay : cfg.wIsoPred;
    };

    for (const Block& blk : blocks) {
        std::vector<int> fs;
        for (int f = 0; f < nf; ++f)
            if (inBlock(phase[f], blk.phs) && !(lo <= f && f <= hi) && !std::isnan(thetaDeg[f]))
                fs.push_back(f);
        if (int(fs.size()) < 4) continue;
        std::vector<double> psiRad(fs.size()), ph(fs.size()), w(fs.size());
        for (size_t i = 0; i < fs.size(); ++i) {
            ph[i] = phiS[fs[i]];
            psiRad[i] = (thetaDeg[fs[i]] - ph[i]) * kPi / 180.0;
            w[i] = weight(fs[i]);
        }
        const std::vector<double> psiU = unwrap(psiRad);
        std::vector<double> psi(fs.size());
        for (size_t i = 0; i < fs.size(); ++i) psi[i] = psiU[i] * 180.0 / kPi;   // deg, continuous
        const std::vector<double> iso = robustIsotonic(psi, w, blk.inc, cfg);
        for (size_t i = 0; i < fs.size(); ++i) {
            const int f = fs[i];
            rr.psiResid[f] = std::abs(psi[i] - iso[i]);
            if (phase[f] == SwingPhase::Impact && !bandOk[f]) {    // blur: arm is the witness
                rr.thetaOut[f] = std::fmod(std::fmod(iso[i] + ph[i], 360.0) + 360.0, 360.0);
                rr.recon[f] = 1;
            }
        }
    }
    return rr;
}

// ── SwingWindow-free decide core ─────────────────────────────────────────────
namespace {

// np.percentile with linear interpolation.
float percentile(std::vector<float> v, double p)
{
    std::sort(v.begin(), v.end());
    const double idx = p / 100.0 * (v.size() - 1);
    const int lo = int(std::floor(idx)), hi = int(std::ceil(idx));
    return float(v[lo] + (v[hi] - v[lo]) * (idx - lo));
}

// Python norm(): clip((s - p50)/(p97 - p50 + 1e-6), 0, 1).
std::vector<float> normScores(const std::vector<float>& s)
{
    const float lo = percentile(s, 50.0), hi = percentile(s, 97.0);
    std::vector<float> out(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out[i] = std::clamp((s[i] - lo) / (hi - lo + 1e-6f), 0.f, 1.f);
    return out;
}

// np.interp fill of NaN entries using the frame index as the abscissa.
void interpFillNan(std::vector<double>& v)
{
    const int n = int(v.size());
    std::vector<int> good;
    for (int i = 0; i < n; ++i) if (!std::isnan(v[i])) good.push_back(i);
    if (good.empty()) { std::fill(v.begin(), v.end(), 0.0); return; }
    for (int i = 0; i < n; ++i) {
        if (!std::isnan(v[i])) continue;
        if (i <= good.front()) { v[i] = v[good.front()]; continue; }
        if (i >= good.back())  { v[i] = v[good.back()];  continue; }
        int a = good.front(), b = good.back();
        for (size_t k = 1; k < good.size(); ++k) if (good[k] >= i) { b = good[k]; a = good[k - 1]; break; }
        v[i] = v[a] + (v[b] - v[a]) * (double(i - a) / double(b - a));
    }
}

} // namespace

Segmentation phasesToSegmentation(const PhaseModel& pm, const std::vector<int64_t>& tUs, float conf,
                                  int addressFrame, bool emitTakeaway)
{
    Segmentation seg;
    const int nf = int(tUs.size());
    if (nf < 2) return seg;
    auto tAt = [&](int f) { return tUs[std::clamp(f, 0, nf - 1)]; };
    auto add = [&](Phase p, int f) { PhaseEvent e; e.phase = p; e.t_us = tAt(f); e.conf = conf; seg.events.push_back(e); };
    // Address = the hold end when the caller located one (addressHoldEndFrame,
    // camera-first P1 fix); bs0 — the TAKEAWAY-start frame — is the legacy value
    // and the fallback. swingStartUs stays bs0-derived either way: the heavy-stage
    // span bound wants motion onset, not the hold.
    add(Phase::Address, addressFrame >= 0 ? addressFrame : pm.bs0);
    // W2: additive vision Takeaway event at bs0 (motion onset). Dark by default;
    // Address (hold end) ≤ bs0 structurally, so the stable_sort below keeps
    // Address before Takeaway even when they share a timestamp (addressFrame < 0).
    if (emitTakeaway) add(Phase::Takeaway, pm.bs0);
    add(Phase::Top,     pm.top);
    add(Phase::Impact,  pm.impact);
    add(Phase::Finish,  pm.fin0);
    std::stable_sort(seg.events.begin(), seg.events.end(),
                     [](const PhaseEvent& a, const PhaseEvent& b) { return a.t_us < b.t_us; });
    const int64_t pad = 100000;   // 100 ms, clamped to coverage
    seg.swingStartUs = std::max(tUs.front(), tAt(pm.bs0) - pad);
    seg.swingEndUs   = std::min(tUs.back(),  tAt(pm.fin0) + pad);
    seg.conf = conf;
    return seg;
}

// Stage B two-pass pose bound helper (swing_span_bounding_plan.md §5). Grip-only
// (no φ witness — the coarse pass has no pose yet), so it leans on the A1 speed
// walk-back + A3 impact clamp inside segmentPhases.
SwingSpanEstimate estimateSwingSpanUs(const std::vector<double>& gx, const std::vector<double>& gy,
                                      const std::vector<int64_t>& tUs, double fps,
                                      int64_t impactUs, const ShaftV3Config& cfg)
{
    SwingSpanEstimate est;
    const int nf = int(gx.size());
    if (nf < 2 || int(gy.size()) != nf || int(tUs.size()) != nf) return est;   // ok = false

    // impactUs → nearest coverage frame (−1 when no impact time is known).
    int impactFrame = -1;
    if (impactUs >= 0) {
        int64_t bestD = std::numeric_limits<int64_t>::max();
        for (int f = 0; f < nf; ++f) {
            const int64_t d = tUs[f] >= impactUs ? tUs[f] - impactUs : impactUs - tUs[f];
            if (d < bestD) { bestD = d; impactFrame = f; }
        }
    }

    const PhaseModel pm = segmentPhases(gx, gy, nf, fps, impactFrame, cfg, nullptr);
    // Degenerate whole-clip-address (no run): the span is the full window —
    // signal the caller to fall back rather than trust a phantom [onset, fin0].
    if (pm.bs0 == 0 && pm.fin0 == nf - 1) return est;   // ok = false

    est.startUs = tUs[std::clamp(pm.bs0, 0, nf - 1)];
    est.endUs   = tUs[std::clamp(pm.fin0, 0, nf - 1)];
    est.ok      = true;
    return est;
}

double projectedClubLenPx(double measuredClubLenPx, double sTypical, double r0Med,
                          double poseExtentPx, double armFloorPx,
                          double clubLenMm, double frameH, const ShaftV3Config& cfg,
                          int& rung)
{
    double L;
    if (measuredClubLenPx > 0.0) {
        L = measuredClubLenPx; rung = 1;                            // ball anchor's address measurement
    } else if (sTypical > 0.0) {
        L = sTypical * std::max(0.0, clubLenMm - r0Med); rung = 2;  // band scale, grip-corrected (grip→head, not butt→head)
    } else if (poseExtentPx > 0.0) {
        const double pxPerM = poseExtentPx / (kShoulderAnkleFrac * cfg.lenStatureM);
        L = pxPerM * std::max(0.0, clubLenMm * 1e-3 - cfg.lenGripDownM); rung = 3;
    } else {
        L = 0.45 * frameH; rung = 4;                                // last-resort frame-height guess
    }
    // Floor then ceiling — a club is always longer than the lead arm, and a
    // measured length caps its own projection; the ceiling is authoritative on
    // conflict (trust the direct measurement / keep the drawn line on-frame).
    if (armFloorPx > 0.0) L = std::max(L, armFloorPx);
    const double ceil = (rung == 1) ? 1.1 * measuredClubLenPx : 0.62 * frameH;
    return std::min(L, ceil);
}

ShaftTrack2D decideTrack(const FrameSource& frameAt, const std::vector<int64_t>& tUs,
                         const std::vector<double>& gxIn, const std::vector<double>& gyIn,
                         const std::vector<double>& phiRawIn,
                         const std::vector<std::vector<cv::Point2d>>& rawJoints,
                         int frameW, int frameH, double fps,
                         const std::vector<double>& bandsMm, double clubLenMm,
                         int impactFrame, const ShaftV3Config& cfg, ShaftDecideTrace* trace,
                         const BallTrack2D* ball, const LengthPriorState* lengthPrior,
                         const std::vector<double>& handAxisDeg,
                         const std::vector<double>& handAxisConf)
{
    ShaftTrack2D out;
    out.frameWidth = frameW; out.frameHeight = frameH;
    const int nf = int(gxIn.size());
    if (nf < 2) return out;
    const std::vector<double>& gx = gxIn;
    const std::vector<double>& gy = gyIn;

    // Decode-once span cache. frameAt is called STRICTLY serially here — the
    // payload contract (bytes valid only to the next fetch) + one-resident-frame
    // disk backing forbid concurrent decode. In production the upstream tracker
    // has already parallel-decoded into owned Mats, so this is a cheap header
    // copy; a synthetic/render frameAt is simply decoded once. The owned Mats let
    // the evidence loop + scene median below run under cv::parallel_for_ race-
    // free. Over the byte cap the cache is skipped and every pass falls back to
    // the serial frameAt (byte-identical to the pre-cache tracker).
    // Must match the ShaftTracker cap so both layers make the same decision.
    constexpr size_t kFrameCacheCapBytes = 1200ull * 1024 * 1024;
    const size_t cacheBytes = size_t(nf) * size_t(std::max(0, frameW)) * size_t(std::max(0, frameH));
    std::vector<cv::Mat> frameCache;
    if (cacheBytes > 0 && cacheBytes <= kFrameCacheCapBytes) {
        frameCache.assign(size_t(nf), cv::Mat());
        for (int i = 0; i < nf; ++i) frameCache[size_t(i)] = frameAt(i);
    }
    const bool parFrames = !frameCache.empty();
    const FrameSource frameSrc = [&](int i) -> cv::Mat {
        if (parFrames && i >= 0 && i < nf) return frameCache[size_t(i)];
        return frameAt(i);
    };

    std::vector<double> phiRaw = phiRawIn;
    interpFillNan(phiRaw);
    const std::vector<double> phiS = smoothPhi(phiRaw, cfg);

    const PhaseModel pm = segmentPhases(gx, gy, nf, fps, impactFrame, cfg, &phiS);

    // chirality from unwrapped φ over [bs0, top]
    int chir = 1;
    {
        std::vector<double> phu(nf);
        double corr = 0; phu[0] = phiS[0] * kPi / 180.0;
        for (int i = 1; i < nf; ++i) {
            const double pr = phiS[i] * kPi / 180.0, pp = phiS[i - 1] * kPi / 180.0;
            double d = pr - pp, dd = std::fmod(d + kPi, 2 * kPi); if (dd < 0) dd += 2 * kPi; dd -= kPi;
            corr += dd - d; phu[i] = pr + corr;
        }
        const int t = std::min(pm.top, nf - 1);
        chir = (phu[t] - phu[pm.bs0]) >= 0 ? 1 : -1;
    }

    const std::vector<std::vector<cv::Point2d>> smoothed = smoothJoints(rawJoints);
    const std::vector<BodyPoly> polys = cfg.rasterC2 ? std::vector<BodyPoly>{} : bodyPolys(smoothed);
    const std::vector<cv::Mat>  masks = cfg.rasterC2 ? bodyMasks(smoothed, frameW, frameH, cfg) : std::vector<cv::Mat>{};
    const std::vector<char> stat = staticRuns(pm.spdSmoothed, cfg);

    const int NS = int(std::lround(360.0 / cfg.grid));
    std::vector<float> gridRad(NS), gridDeg(NS);
    for (int k = 0; k < NS; ++k) { gridDeg[k] = float(k * cfg.grid); gridRad[k] = float(k * cfg.grid * kPi / 180.0); }

    // scene background: median of every-8th frame (float32)
    cv::Mat sceneMed;
    {
        std::vector<cv::Mat> bg;
        for (int i = 0; i < nf; i += 8) { cv::Mat g = frameSrc(i); if (!g.empty()) bg.push_back(g); }
        if (!bg.empty()) {
            const int H = bg[0].rows, W = bg[0].cols; const size_t n = bg.size();
            sceneMed.create(H, W, CV_32F);
            // Per-row nth_element median; `vals` is per-thread (the only shared
            // mutable in the old serial loop). The k-th order statistic is unique,
            // so the parallel result is byte-identical to the serial scan.
            const auto medianRows = [&](const cv::Range& rng) {
                std::vector<uchar> vals(n);
                for (int r = rng.start; r < rng.end; ++r) {
                    float* orow = sceneMed.ptr<float>(r);
                    for (int c = 0; c < W; ++c) {
                        for (size_t k = 0; k < n; ++k) vals[k] = bg[k].ptr<uchar>(r)[c];
                        std::nth_element(vals.begin(), vals.begin() + n / 2, vals.end());
                        orow[c] = float(vals[n / 2]);
                    }
                }
            };
            if (parFrames) cv::parallel_for_(cv::Range(0, H), medianRows);
            else           medianRows(cv::Range(0, H));
        }
    }

    const int addressCollar = int(std::lround(double(cfg.addressCollarUs) * 1e-6 * fps));
    const int finishCollar  = int(std::lround(double(cfg.spanCollarUs) * 1e-6 * fps));
    const int spanLo = cfg.spanBound ? std::max(0, pm.bs0 - addressCollar) : 0;
    const int spanHi = cfg.spanBound ? std::min(nf - 1, pm.fin0 + finishCollar) : nf - 1;
    const double rmax = 0.62 * frameH;

    // A1 (clubhead_length plan) — measure club length from the ball HERE, before
    // head placement, so it can seed rung 1 of the length ladder below. The
    // window is the address HOLD (last still run at bs0, derived inside from
    // stat + bs0 + collar) — NOT the whole pre-takeaway period, whose teeing/
    // setup still frames measured hands-at-ball and ran 107–178 px short on the
    // 2026-07-04 corpus. Leaves -1 when too few admissible ball samples exist
    // or the lock fails the golf-prior gate (ankle line / between-the-feet:
    // gateA-0704's consistent driver-head lock passed the cluster gate and
    // shorted rung 1; per-frame ankles from the smoothed body joints,
    // kBodyJoints order 6/7 = L/R ankles). Read-only wrt the θ path
    // (applyBallAnchor no longer recomputes this — it only reads
    // out.measuredClubLenPx now).
    if (ball && !ball->frames.empty()) {
        std::vector<AnklePx> ankles(static_cast<size_t>(nf));
        for (int i = 0; i < nf; ++i) {
            if (smoothed[i].size() < 8) continue;
            ankles[size_t(i)] = {smoothed[i][6].x, smoothed[i][6].y,
                                 smoothed[i][7].x, smoothed[i][7].y, true};
        }
        int lpxReject = 0;
        const double lpx = medianGripBallLenPx(*ball, gx, gy, tUs, frameW, frameH,
                                               pm.bs0, addressCollar, &stat, &ankles, &lpxReject);
        if (lpx > 0.0) out.measuredClubLenPx = float(lpx);
        if (trace) trace->lPxRejected = lpxReject;
    }

    std::vector<std::vector<float>> emis(nf, std::vector<float>(NS, float(cfg.wE2)));
    std::vector<std::vector<float>> EV(nf, std::vector<float>(NS, 0.f));
    // Per-θ ridge support (max over the surviving evidence channels), the
    // companion of EV. ridgeSweep has always returned it and the tracker has
    // always thrown it away; the RAY tier now asks it whether the DP's winning
    // direction is a continuous line rather than a percentile artefact.
    std::vector<std::vector<float>> SUP(nf, std::vector<float>(NS, 0.f));
    std::vector<BandMatch> band(nf);
    std::vector<char> bandOk(nf, 0);
    // Per-frame evidence: each frame writes ONLY its own indexed slots (emis[i],
    // EV[i], SUP[i], band[i], bandOk[i], trace->rawP97[i]/difP97[i]) and everything
    // ridgeSweep/frameBandMatch/frameEmission touches is a per-call local — so the
    // loop is embarrassingly parallel over frames. The lone serial-loop accumulator
    // (`heavy`) is replaced by a per-frame processed flag summed afterwards, keeping
    // the count identical regardless of thread order. sceneMed / gridRad / gridDeg /
    // polys / masks / phiS / pm are read-only. Runs parallel only when the owned
    // frame cache is live (frameSrc never touches payloadOf under threads);
    // else serial.
    std::vector<char> heavyMark(size_t(nf), 0);
    // S1 calibration channels: sized here so the parallel body only ever writes
    // its own index (−1 = channel/frame never ran). supAtDp is filled later, in
    // the tier loop, where the DP's θ exists.
    if (trace) {
        trace->rawP97.assign(size_t(nf), -1.0);
        trace->difP97.assign(size_t(nf), -1.0);
        trace->supAtDp.assign(size_t(nf), -1.0);
    }
    // ── R8 wedge pre-pass (S2): R6 predictor → per-frame trigger + envelope ──
    // ω̂ and the envelope come from the MEASURED arm + the stereotyped
    // wrist-cock table only, never from the DP: the trigger must not chase the
    // tracker's own (possibly wrong) path, and t_exp calibration needs a rate
    // that exists before the DP runs. Data-gated on |ω̂| (never sessionType,
    // never phase-only). All of this is inert — and every downstream byte
    // identical — when cfg.wedge.enabled is false.
    std::vector<char>           wedgeTrig(size_t(nf), 0);
    std::vector<double>         omegaPred(size_t(nf), 0.0);
    std::vector<double>         envCenter(size_t(nf), 0.0), envHalf(size_t(nf), 0.0);
    std::vector<WedgeCandidate> wedgeCand(static_cast<size_t>(nf));
    if (cfg.wedge.enabled) {
        std::vector<double> phiPred(size_t(nf), 0.0);
        for (int i = 0; i < nf; ++i) {
            const double s = swingProgress(i, pm.bs0, pm.top, pm.impact, pm.fin0);
            phiPred[size_t(i)] = phiClubPredDeg(phiS[i], s, chir);
            const KinEnvelope e = envelope(s, phiS[i], chir, cfg.wedge.kSigma);
            envCenter[size_t(i)] = e.centerDeg;
            envHalf[size_t(i)]   = e.halfDeg;
        }
        for (int i = 0; i < nf; ++i) {
            const int a = std::max(0, i - 1), b = std::min(nf - 1, i + 1);
            const double dt = double(tUs[b] - tUs[a]) * 1e-6;
            omegaPred[size_t(i)] = omegaPredDegPerS(phiPred[size_t(a)], phiPred[size_t(b)], dt);
            // Confined to the evidence span: outside it no sweep runs, so no
            // well can exist and the kinCone must not shape the coast path.
            wedgeTrig[size_t(i)] = spanLo <= i && i <= spanHi && !std::isnan(gx[i])
                                   && std::abs(omegaPred[size_t(i)]) >= cfg.wedge.omegaMinDegS;
        }
    }
    const auto evidenceBody = [&](int i) {
        if (std::isnan(gx[i])) return;
        if (!(spanLo <= i && i <= spanHi)) return;
        cv::Mat g8 = frameSrc(i);
        if (g8.empty()) return;
        heavyMark[size_t(i)] = 1;
        cv::Mat g32; g8.convertTo(g32, CV_32F);
        const RidgeResult sRaw = ridgeSweep(g32, gx[i], gy[i], gridRad, cfg.ridge, false);
        // Raw (pre-normalisation) p97 of the channel: the ONLY absolute
        // statement available about whether this frame contains a line at all —
        // normScores() below rescales any row, noise included, to a full-strength
        // [0,1] winner. A channel that misses the floor is drowned: zero scores
        // AND zero support, so it can neither win the DP nor bless a RAY tier.
        // The other channel still speaks for the frame.
        const float rawP97 = percentile(sRaw.score, 97.0);
        if (trace) trace->rawP97[size_t(i)] = double(rawP97);
        const bool rawDrown = cfg.evAbsFloor > 0.0 && rawP97 < float(cfg.evAbsFloor);
        std::vector<float> normRaw = rawDrown ? std::vector<float>(NS, 0.f) : normScores(sRaw.score);
        std::vector<float> supMax  = rawDrown ? std::vector<float>(NS, 0.f) : sRaw.support;
        std::vector<float> evMax = normRaw;
        cv::Mat diff;   // scene-median residual, shared with the wedge sweep below
        if (!sceneMed.empty() && sceneMed.size() == g32.size()) {
            cv::absdiff(g32, sceneMed, diff);
            const RidgeResult sDif = ridgeSweep(diff, gx[i], gy[i], gridRad, cfg.ridge, true);
            // The dif channel is a different score population (scene-median
            // residual, brightOnly), so it carries its own floor; < 0 means
            // "no separate opinion", i.e. share evAbsFloor.
            const float difP97 = percentile(sDif.score, 97.0);
            if (trace) trace->difP97[size_t(i)] = double(difP97);
            const double difFloor = cfg.evAbsFloorDif >= 0.0 ? cfg.evAbsFloorDif : cfg.evAbsFloor;
            const bool difDrown = difFloor > 0.0 && difP97 < float(difFloor);
            const std::vector<float> normDif = difDrown ? std::vector<float>(NS, 0.f) : normScores(sDif.score);
            for (int k = 0; k < NS; ++k) evMax[k] = std::max(normRaw[k], normDif[k]);
            if (!difDrown)
                for (int k = 0; k < NS; ++k) supMax[k] = std::max(supMax[k], sDif.support[k]);
        }
        EV[i] = evMax;
        SUP[i] = std::move(supMax);
        // R8 wedge: a second, PROXIMAL ridge sweep (raw + dif) restricted to
        // the R6 envelope's θ bins. Image velocity ∝ ρ, so the innermost shaft
        // barely blurs — the R5 relaxation (short rHi, short minLenPx) lives in
        // a DERIVED RidgeConfig; the main sweep above is untouched (frozen).
        // The fan plateau is measured against the ABSOLUTE evAbsFloor anchor —
        // both channels drown ⇒ no candidate (never fabricate). Writes only
        // this frame's wedgeCand slot (parallel-safe).
        if (wedgeTrig[size_t(i)]) {
            const int cBin     = int(std::lround(envCenter[size_t(i)] / cfg.grid));
            const int halfBins = std::min(NS / 2 - 1,
                                          int(std::lround(envHalf[size_t(i)] / cfg.grid)));
            std::vector<float> proxRad, proxDeg;
            proxRad.reserve(size_t(2 * halfBins + 1));
            proxDeg.reserve(size_t(2 * halfBins + 1));
            for (int d = -halfBins; d <= halfBins; ++d) {
                const int k = ((cBin + d) % NS + NS) % NS;
                proxRad.push_back(gridRad[k]);
                proxDeg.push_back(gridDeg[k]);
            }
            RidgeConfig prox = cfg.ridge;
            prox.rHi      = float(cfg.wedge.proximalRHiFrac * rmax);
            prox.minLenPx = float(cfg.wedge.proximalMinLenPx);
            const RidgeResult pRaw = ridgeSweep(g32, gx[i], gy[i], proxRad, prox, false);
            std::vector<float> pDif;
            if (!diff.empty()) {
                const RidgeResult pd = ridgeSweep(diff, gx[i], gy[i], proxRad, prox, true);
                pDif = pd.score;
            }
            wedgeCand[size_t(i)] = measureWedge(pRaw.score, pDif, proxDeg, phiS[i],
                                                cfg.armVetoDeg, cfg.evAbsFloor, cfg.wedge);
        }
        BandMatch bm = frameBandMatch(g8, gx[i], gy[i], rmax, bandsMm, cfg.band);
        if (bm.ok && bm.r0 > 0.0f && bm.r0 <= 260.0f) { band[i] = bm; bandOk[i] = 1; }
        std::vector<float> em, inside;
        const double haDeg  = (i < int(handAxisDeg.size()))  ? handAxisDeg[size_t(i)]
                                                             : std::numeric_limits<double>::quiet_NaN();
        const double haConf = (i < int(handAxisConf.size())) ? handAxisConf[size_t(i)] : 0.0;
        frameEmission(em, inside, evMax, normRaw, band[i], phiS[i], pm.phase[i], chir,
                      gx[i], gy[i], polys.empty() ? nullptr : &polys[i],
                      masks.empty() ? cv::Mat() : masks[i], gridRad, gridDeg, cfg, haDeg, haConf);
        emis[i] = std::move(em);
    };
    if (parFrames)
        cv::parallel_for_(cv::Range(0, nf), [&](const cv::Range& rng) {
            for (int i = rng.start; i < rng.end; ++i) evidenceBody(i);
        });
    else
        for (int i = 0; i < nf; ++i) evidenceBody(i);
    int heavy = 0;
    for (int i = 0; i < nf; ++i) heavy += heavyMark[size_t(i)];

    // ── R8 wedge injection (serial, pre-DP): t_exp calibration + centroid well
    //    + kinCone. The frozen frameEmission/viterbiDP/normScores bodies are
    //    untouched — the wedge only EDITS the already-built emission rows of
    //    triggered frames, exactly as the band well edits its bin. ───────────
    double              wedgeTExp = -1.0;
    std::vector<double> wedgeSigmaDeg(size_t(nf), 0.0);
    if (cfg.wedge.enabled) {
        // Exposure from the fan widths themselves: width ≈ ω̂·t_exp, so the
        // median of width/|ω̂| over confidently-rotating candidate frames is
        // t_exp — clamped to plausibility, bootstrapped when nothing qualifies.
        std::vector<double> texps;
        for (int i = 0; i < nf; ++i)
            if (wedgeTrig[size_t(i)] && wedgeCand[size_t(i)].ok
                && std::abs(omegaPred[size_t(i)]) >= cfg.wedge.calOmegaLoDegS)
                texps.push_back(wedgeCand[size_t(i)].widthDeg / std::abs(omegaPred[size_t(i)]));
        if (!texps.empty()) {
            std::nth_element(texps.begin(), texps.begin() + texps.size() / 2, texps.end());
            wedgeTExp = std::clamp(texps[texps.size() / 2], kTExpLoS, kTExpHiS);
        } else {
            wedgeTExp = std::clamp(cfg.wedge.tExpBootstrapS, kTExpLoS, kTExpHiS);
        }
        for (int i = 0; i < nf; ++i) {
            if (!wedgeTrig[size_t(i)]) continue;
            if (wedgeCand[size_t(i)].ok) {
                // σ_θ: at least the measured half-width, at least the expected
                // half-sweep — a narrow plateau under a fast prediction is not
                // allowed to claim delta-function precision.
                const double sig = std::max(wedgeCand[size_t(i)].widthDeg * 0.5,
                                            std::abs(omegaPred[size_t(i)]) * wedgeTExp * 0.5);
                wedgeSigmaDeg[size_t(i)] = sig;
                const double cen = wedgeCand[size_t(i)].centroidDeg;
                for (int k = 0; k < NS; ++k) {
                    const double d = std::abs(circWrap(gridDeg[k] - cen));
                    if (d > 3.0 * sig) continue;
                    const double well = cfg.wedge.wWell * std::exp(-0.5 * (d / sig) * (d / sig));
                    // Clamped at −wBand: a band lock always outranks a wedge.
                    emis[i][k] = std::max(float(emis[i][k] - well), float(-cfg.wBand));
                }
            }
            // kinCone (expected arm, S1 finding): the wrong downswing path sits
            // on genuinely strong OFF-shaft structure that evidence-level
            // honesty cannot demote — so on triggered frames the states outside
            // the kinematic envelope pay a soft prior penalty (the C4-cone
            // idiom), defunding structure-backed short paths. NEVER a tier.
            if (cfg.wedge.kinCone) {
                for (int k = 0; k < NS; ++k)
                    if (std::abs(circWrap(gridDeg[k] - envCenter[size_t(i)])) > envHalf[size_t(i)])
                        emis[i][k] += float(cfg.wedge.wKinCone);
                // Re-assert the band well after the cone (band always outranks).
                if (bandOk[i]) {
                    int bi = int(std::lround(band[i].thetaDeg / cfg.grid)) % NS;
                    if (bi < 0) bi += NS;
                    emis[i][bi] = float(-cfg.wBand);
                }
            }
        }
    }

    const DPResult dp = viterbiDP(emis, pm.phase, cfg);
    std::vector<double> evAt(nf, 0.0);
    for (int i = 0; i < nf; ++i) evAt[i] = EV[i][dp.thstar[i]];
    ReconResult rec;
    if (cfg.psiRail)
        rec = reconcilePsi(dp.thetaDeg, phiS, pm.phase, bandOk, evAt, pm.top, nf, cfg);
    else {
        rec.thetaOut = dp.thetaDeg;
        rec.psiResid.assign(nf, std::numeric_limits<double>::quiet_NaN());
        rec.recon.assign(nf, 0);
    }

    // A2 length ladder — the projected grip→head length for coasted/predicted
    // heads. Band scale (rung 2) + the pose stature surrogate (rung 3) are
    // medians over the still/banded frames; rung 1 is the ball's A1 measurement.
    // Feeds ONLY the projected-head fallback below — the corpus-validated θ path
    // (segmentPhases/emission/DP/reconcile above) is entirely upstream of this.
    double sTypical = 0, r0Med = 0;
    { std::vector<double> ss, rr;
      for (int i = 0; i < nf; ++i) if (bandOk[i]) { ss.push_back(band[i].s); rr.push_back(band[i].r0); }
      if (!ss.empty()) { std::nth_element(ss.begin(), ss.begin() + ss.size() / 2, ss.end()); sTypical = ss[ss.size() / 2]; }
      if (!rr.empty()) { std::nth_element(rr.begin(), rr.begin() + rr.size() / 2, rr.end()); r0Med = rr[rr.size() / 2]; } }
    // Pose-scale surrogate + arm floor over the still frames: shoulder-mid→ankle-
    // mid extent (stature) and shoulder-mid→grip reach (arm). smoothed[i] joints
    // are kBodyJoints order — 0/1 = L/R shoulders, 6/7 = L/R ankles.
    // NB `armFloorMedPx` (not `armFloorPx`) — the clubhead module exports a free
    // function armFloorPx(); the local must not shadow it (the head pass below
    // calls the per-frame free function).
    double poseExtentPx = 0, armFloorMedPx = 0;
    { std::vector<double> ext, arm;
      for (int i = 0; i < nf; ++i) {
          if (!stat[i] || std::isnan(gx[i]) || smoothed[i].size() < 8) continue;
          const cv::Point2d sh = 0.5 * (smoothed[i][0] + smoothed[i][1]);
          const cv::Point2d an = 0.5 * (smoothed[i][6] + smoothed[i][7]);
          const double e = std::hypot(sh.x - an.x, sh.y - an.y);
          if (e > 1.0) ext.push_back(e);
          arm.push_back(std::hypot(sh.x - gx[i], sh.y - gy[i]));
      }
      if (!ext.empty()) { std::nth_element(ext.begin(), ext.begin() + ext.size() / 2, ext.end()); poseExtentPx = ext[ext.size() / 2]; }
      if (!arm.empty()) { std::nth_element(arm.begin(), arm.begin() + arm.size() / 2, arm.end()); armFloorMedPx = 1.05 * arm[arm.size() / 2]; } }
    // E-pose sanity surrogate (rung-3 math, RECOMPUTED so projectedClubLenPx stays
    // byte-untouched). fuseClubLength uses it ONLY as a [poseLo, poseHi]× sanity
    // band, never as a fused value — E-pose reads ~33% short. 0 ⇒ no pose scale.
    double poseBoundPx = 0.0;
    if (poseExtentPx > 0.0)
        poseBoundPx = (poseExtentPx / (kShoulderAnkleFrac * cfg.lenStatureM))
                      * std::max(0.0, clubLenMm * 1e-3 - cfg.lenGripDownM);

    // ── A2b PRE-PASS length fusion (club_length_fusion): fuse ball+band+prior at
    // the ladder. conf ≥ ladderConfMin ⇒ rung 0 with the fused px, which then
    // feeds headBounds' fallback ceiling AND hin.lPx below. ABSTAIN / fusion
    // disabled ⇒ the EXACT existing projectedClubLenPx call runs (and hin.lPx
    // keeps its existing expression) — byte-identical to the pre-fusion tracker.
    int    projLenRung = 0;
    double projLenPx   = 0.0;
    LengthFused preFuse;
    bool   fusedLadder = false;
    if (cfg.fusion.enabled) {
        std::vector<LengthCandidate> cands =
            instantLengthCandidates(out.measuredClubLenPx, sTypical, r0Med, clubLenMm);
        const int priorNarg = appendPriorCandidate(cands, lengthPrior);
        preFuse = fuseClubLength(cands, poseBoundPx, armFloorMedPx, double(frameH), priorNarg, cfg.fusion);
        if (!preFuse.abstained && preFuse.fusedPx > 0.0 && preFuse.conf >= cfg.fusion.ladderConfMin) {
            // min(max(fused, armFloor), 1.1·fused) — floor to the lead arm, cap at
            // 1.1× the fused estimate (matches rung 1's own-measurement ceiling).
            projLenPx   = std::min(std::max(preFuse.fusedPx, armFloorMedPx), 1.1 * preFuse.fusedPx);
            projLenRung = 0;
            fusedLadder = true;
        }
    }
    if (!fusedLadder)
        projLenPx = projectedClubLenPx(out.measuredClubLenPx, sTypical, r0Med, poseExtentPx,
                                       armFloorMedPx, clubLenMm, frameH, cfg, projLenRung);
    if (trace) { trace->projLenRung = projLenRung; trace->projLenPx = projLenPx; }

    enum Tier { PRED = 0, RAY = 1, BAND = 2, RECON = 3, WEDGE = 4 };

    // ── PASS 1: tier decision, HOISTED out of the placement loop ─────────────
    // Precompute the per-frame tier + confidence (Phase B needs s1IsMeas[i] = the
    // stage-1 tier is a real vision measurement, before it can bless a head as
    // meas). This is a PURE REFACTOR of the old fused loop: identical tier/conf
    // for every frame — the placement loop below consumes the result. The head
    // geometry that the old BAND branch computed inline is placement, so it moves
    // to pass 2 (guarded on tierOf[i]==BAND). RECON's head-clearing is likewise a
    // placement effect (pass 2 simply does not band-place a RECON frame).
    std::vector<uint8_t> tierOf(static_cast<size_t>(nf), static_cast<uint8_t>(PRED));
    std::vector<float>   confOf(static_cast<size_t>(nf), 0.f);
    for (int i = 0; i < nf; ++i) {
        if (std::isnan(gx[i])) continue;
        const int thi = dp.thstar[i];
        const double thDp = dp.thetaDeg[i];
        const double th = rec.thetaOut[i];
        if (trace) trace->supAtDp[size_t(i)] = double(SUP[i][thi]);
        int tier = PRED; float conf = 0.30f;
        if (bandOk[i] && std::abs(circWrap(thDp - band[i].thetaDeg)) <= cfg.bandTol) {
            tier = BAND;
            conf = float(std::min(0.9, 0.75 + 0.05 * (band[i].n - 4)));
        // Addr-labelled frames are normally excluded from ray publication (a
        // static hold is the classic counterfeit trap) — EXCEPT inside the
        // widened address collar (i >= spanLo), where bs0's grip-speed lag is
        // known to have already mislabelled real early-takeaway motion as
        // address (Mark, 2026-07-08). The `verifiable` check below still
        // requires motion (!stat[i]) or band corroboration, so a genuinely
        // still frame in that collar is still refused — only real, moving,
        // evidenced frames newly qualify.
        } else if (pm.phase[i] != SwingPhase::Addr || i >= spanLo) {
            const double evs = EV[i][thi], evrev = EV[i][(thi + NS / 2) % NS];
            bool bandNear = false;
            for (int j = std::max(0, i - cfg.bandNear); j <= std::min(nf - 1, i + cfg.bandNear); ++j) if (bandOk[j]) { bandNear = true; break; }
            const bool verifiable = (pm.phase[i] == SwingPhase::Finish) ? bandNear : (!stat[i] || bandNear);
            // The support gate is absolute, unlike evs: a blur frame can win the
            // normalised comparison in both directions and still have no
            // continuous line under the ray. Demoted frames fall to PRED (the
            // honest label) — BAND is untouched, it has its own witness.
            if (evs >= cfg.rayEvMin && evs > 1.15 * evrev && verifiable
                && (cfg.raySupportMin <= 0.0 || SUP[i][thi] >= float(cfg.raySupportMin))) { tier = RAY; conf = 0.55f; }
        }
        // WEDGE tier (chain BAND > RAY > WEDGE): the DP settled inside the
        // measured fan — the frame is evidenced by integration, not a thin
        // line, so it earns the wedge blessing (and spanMeas coverage) at
        // wedge.conf, deliberately below RAY's 0.55.
        if (tier == PRED && wedgeTrig[size_t(i)] && wedgeCand[size_t(i)].ok
            && std::abs(circWrap(thDp - wedgeCand[size_t(i)].centroidDeg))
                   <= wedgeSigmaDeg[size_t(i)] + cfg.wedge.dpTolDeg) {
            tier = WEDGE; conf = float(cfg.wedge.conf);
        }
        if (rec.recon[i] && std::abs(circWrap(th - thDp)) > cfg.reconTol) { tier = RECON; conf = 0.40f; }
        tierOf[size_t(i)] = uint8_t(tier);
        confOf[size_t(i)] = conf;
    }

    // ── Stage-2 measured clubhead (Phase B, dark behind cfg.head.enabled) ─────
    // Runs AFTER reconcilePsi + the length ladder, BEFORE placement — everything
    // it needs (decided θ/grip/tier, sceneMed, pm.top/impact, stat[], smoothed
    // joints, L_px) is in scope here. It consumes decided results only and never
    // feeds back into the DP/reconcile, so the stage-1 θ path is bit-identical
    // with the head pass on or off (only headPx/flags/headConf differ). Empty
    // headResults ⇒ head pass off ⇒ placement keeps the Phase-A projected path.
    std::vector<HeadFrameResult> headResults;
    // Per-frame Stage-2 search ceiling rHi, captured for the A2b post-pass E-head
    // pinned-at-bound guard (E-head must not ratify its own search bound). NaN
    // where no head measurement was attempted; empty when the head pass is off.
    std::vector<double> headRHi;
    if (cfg.head.enabled && !sceneMed.empty()) {
        QElapsedTimer headTimer; headTimer.start();
        const double kHNaN = std::numeric_limits<double>::quiet_NaN();
        headRHi.assign(size_t(nf), kHNaN);
        const int W = frameW, H = frameH;
        const HeadSceneCtx hctx = makeHeadSceneCtx(sceneMed, W, H, cfg.head);
        // 40 px annulus-bbox inflation covers the 3×3 Sobel kernel + the widest
        // edge-pair lateral shift (blade width/2) + any lateral offset band.
        const int roiInflate = 40 + int(std::ceil(cfg.head.latMaxPx));

        HeadTemporalInput hin;
        hin.z.assign(size_t(nf), kHNaN);   hin.zconf.assign(size_t(nf), 0.0);
        hin.thetaDeg.assign(size_t(nf), kHNaN);
        hin.s1IsMeas.assign(size_t(nf), 0); hin.flipSuspect.assign(size_t(nf), 0);
        hin.rEdge.assign(size_t(nf), 0.0);
        // Pre-pass fused length (rung 0) overrides the ball-only L_px so the
        // Stage-2 annulus ceiling/floor use the fused estimate; ABSTAIN keeps the
        // exact existing expression (byte-identical to the pre-fusion path).
        hin.lPx = fusedLadder ? preFuse.fusedPx
                              : ((out.measuredClubLenPx > 0.f) ? double(out.measuredClubLenPx) : -1.0);
        hin.dt  = (fps > 0.0) ? 1.0 / fps : 0.0;
        hin.cfg = cfg.head;

        // Second decode over the swing span (full-frame caching ~2 MB × N frames
        // is not viable). prev32 = previous decoded frame; bg32 = running EMA
        // background (deviation from the Python's fixed sample set — corpus-
        // validate). Warm-start bg at spanLo so the first frame's change==0.
        cv::Mat prev32, bg32;
        for (int i = spanLo; i <= spanHi; ++i) {
            cv::Mat g8 = frameSrc(i);
            if (g8.empty()) continue;                 // undecodable — prev/bg unchanged
            cv::Mat g32; g8.convertTo(g32, CV_32F);
            if (bg32.empty()) g32.copyTo(bg32);
            else cv::addWeighted(g32, cfg.head.bgAlpha, bg32, 1.0 - cfg.head.bgAlpha, 0.0, bg32);
            const cv::Mat prevUse = prev32.empty() ? g32 : prev32;

            const double th = rec.thetaOut[i];
            if (!std::isnan(gx[i]) && !std::isnan(th)) {
                hin.thetaDeg[size_t(i)]  = th;
                hin.s1IsMeas[size_t(i)]  = char(tierOf[size_t(i)] == RAY || tierOf[size_t(i)] == BAND);
                const double rEdge = rayEdgeRadius(gx[i], gy[i], th, W, H);
                hin.rEdge[size_t(i)] = rEdge;

                // quasi-still (a pre-top hold) and the impact window drive the
                // ball-length floors + Gaussian prior (B2).
                const bool quasiStill   = stat[i] && i <= pm.top;
                const bool floorApplies = quasiStill || std::abs(i - pm.impact) <= cfg.impHalf;
                double armFloor = -1.0;
                if (smoothed[i].size() >= 2)            // 0/1 = L/R shoulders
                    armFloor = armFloorPx(smoothed[i][0], smoothed[i][1], gx[i], gy[i], quasiStill, cfg.head);
                // Phase-ramped acceptance floor (gateB iter-2): face-on the club
                // is near-FULL projected length at takeaway and at impact —
                // foreshortening develops toward the top — so the floor fraction
                // ramps floorRampHi → measFloorFrac across [bs0, top] and mirrors
                // back up over (top, impact]. Elsewhere −1 ⇒ the module's constant
                // measFloorFrac. Kills the early-backswing streak short-locks
                // (26/28 iter-2 confident-bad labels at bs0+20..bs0+45, terminus
                // 0.5–0.96·L̂ vs near-full truth). The ramp arithmetic is wiring-
                // level and covered by the corpus honesty gate (headBounds'
                // floorFracOverride contract is unit-tested in the module tests).
                double floorFrac = -1.0;
                if (pm.top > pm.bs0 && i >= pm.bs0 && i <= pm.top) {
                    const double t = double(i - pm.bs0) / double(pm.top - pm.bs0);
                    floorFrac = cfg.head.floorRampHi + t * (cfg.head.measFloorFrac - cfg.head.floorRampHi);
                } else if (pm.impact > pm.top && i > pm.top && i <= pm.impact) {
                    const double t = double(i - pm.top) / double(pm.impact - pm.top);
                    floorFrac = cfg.head.measFloorFrac + t * (cfg.head.floorRampHi - cfg.head.measFloorFrac);
                }
                const HeadBounds b = headBounds(rEdge, hin.lPx, projLenPx, armFloor, floorApplies, hctx,
                                                floorFrac);
                headRHi[size_t(i)] = b.rHi;   // A2b E-head pinned-at-bound guard
                const double lPrior = headPrior(hin.lPx, quasiStill);

                // ROI-bounded Sobel of the annulus grip+[rLo,rHi]·dir(θ), written
                // into full-size zero Mats so measureHeadRadius's ray samples are
                // valid inside the annulus (BORDER_CONSTANT=0 elsewhere is harm-
                // less — the ray only reads inside the ROI). The main perf lever.
                cv::Mat gxs(H, W, CV_32F, cv::Scalar(0)), gys(H, W, CV_32F, cv::Scalar(0));
                const double cth = std::cos(th * kPi / 180.0), sth = std::sin(th * kPi / 180.0);
                const double ax0 = gx[i] + b.rLo * cth, ay0 = gy[i] + b.rLo * sth;
                const double ax1 = gx[i] + b.rHi * cth, ay1 = gy[i] + b.rHi * sth;
                int rx0 = std::max(0,     int(std::floor(std::min(ax0, ax1))) - roiInflate);
                int ry0 = std::max(0,     int(std::floor(std::min(ay0, ay1))) - roiInflate);
                int rx1 = std::min(W - 1, int(std::ceil (std::max(ax0, ax1))) + roiInflate);
                int ry1 = std::min(H - 1, int(std::ceil (std::max(ay0, ay1))) + roiInflate);
                if (rx1 > rx0 && ry1 > ry0) {
                    const cv::Rect roi(rx0, ry0, rx1 - rx0 + 1, ry1 - ry0 + 1);
                    cv::Mat sgx, sgy;
                    cv::Sobel(g32(roi), sgx, CV_32F, 1, 0, 3);
                    cv::Sobel(g32(roi), sgy, CV_32F, 0, 1, 3);
                    sgx.copyTo(gxs(roi)); sgy.copyTo(gys(roi));
                }

                const HeadMeasurement fwd = measureHeadRadius(g32, prevUse, bg32, gxs, gys, hctx,
                                                              gx[i], gy[i], th, b.rLo, b.rHi, b.rFloor, lPrior);
                if (std::isfinite(fwd.rPx)) {
                    // 180°-flip suspicion: does the opposite ray decisively out-
                    // support? Never CORRECT the ray (decoupling) — only refuse
                    // the frame meas-tier blessing (fed as flipSuspect).
                    const HeadMeasurement opp = measureHeadRadius(g32, prevUse, bg32, gxs, gys, hctx,
                                                                  gx[i], gy[i], th + 180.0, b.rLo, b.rHi, b.rFloor, kHNaN);
                    hin.flipSuspect[size_t(i)] = char(isFlipSuspect(fwd.conf, opp.conf, cfg.head));
                }
                hin.z[size_t(i)]     = fwd.rPx;
                hin.zconf[size_t(i)] = fwd.conf;
            }
            g32.copyTo(prev32);
        }

        headResults = runHeadTemporal(hin);
        if (trace) {
            trace->headMs = double(headTimer.nsecsElapsed()) * 1e-6;
            trace->headTier.assign(size_t(nf), int(HeadTier::Pred));
            trace->headR.assign(size_t(nf), kHNaN);
            trace->headZ.assign(size_t(nf), kHNaN);
            for (int i = 0; i < nf && i < int(headResults.size()); ++i) {
                trace->headTier[size_t(i)] = int(headResults[size_t(i)].tier);
                trace->headR[size_t(i)]    = headResults[size_t(i)].rOut;
                trace->headZ[size_t(i)]    = hin.z[size_t(i)];
            }
        }
    }

    // ── A2b POST-PASS length fusion: add the measured-head estimator ──────────
    // Re-fuse ball+band+prior+head into the RECORDED length + confidence
    // (out.lengths.fused*) AND the PRIOR-FREE instant variant (fusedInstant*,
    // which alone updates the persistent prior — no self-reinforcement). Pure
    // head/length product: θ and the placement below are untouched (the fused
    // length already refined projLenPx/hin.lPx in the pre-pass). E-head = p95 of
    // post-top HeadTier::Meas rOut with UNCAPPED headConf ≥ fusion.headConfMin,
    // ≥ headMinMeas frames, EXCLUDED when p95 sits within headPinFrac of those
    // frames' search ceiling rHi (must not ratify its own bound). NB the uncapped
    // hr.headConf, not the streak-capped sample field placed below.
    if (cfg.fusion.enabled) {
        double headLenPx = -1.0;
        int    headMeasN = 0;
        if (!headResults.empty()) {
            std::vector<double> rmeas, rhis;
            for (int i = pm.top + 1; i < nf && i < int(headResults.size()); ++i) {
                const HeadFrameResult& hr = headResults[size_t(i)];
                if (hr.tier != HeadTier::Meas || !std::isfinite(hr.rOut)) continue;
                if (hr.headConf < float(cfg.fusion.headConfMin)) continue;
                rmeas.push_back(hr.rOut);
                rhis.push_back(headRHi.empty() ? std::numeric_limits<double>::quiet_NaN()
                                               : headRHi[size_t(i)]);
            }
            headMeasN = int(rmeas.size());
            if (headMeasN >= cfg.fusion.headMinMeas) {
                const double p95     = percentileOf(rmeas, cfg.fusion.headPctile);
                const double ceilRep = medianFinite(rhis);   // representative search ceiling
                // Accept E-head only when NOT pinned against its own bound.
                if (!(ceilRep > 0.0) || p95 < (1.0 - cfg.fusion.headPinFrac) * ceilRep)
                    headLenPx = p95;
            }
        }

        std::vector<LengthCandidate> instCands =
            instantLengthCandidates(out.measuredClubLenPx, sTypical, r0Med, clubLenMm);
        if (headLenPx > 0.0) instCands.push_back({LengthSource::Head, headLenPx, -1.0});

        std::vector<LengthCandidate> withPrior = instCands;
        const int priorNarg = appendPriorCandidate(withPrior, lengthPrior);
        const LengthFused fRec  = fuseClubLength(withPrior, poseBoundPx, armFloorMedPx, double(frameH),
                                                 priorNarg, cfg.fusion);
        const LengthFused fInst = fuseClubLength(instCands, poseBoundPx, armFloorMedPx, double(frameH),
                                                 0, cfg.fusion);

        ClubLengthEstimate& L = out.lengths;
        L.ballPx           = (out.measuredClubLenPx > 0.f) ? double(out.measuredClubLenPx) : -1.0;
        L.bandPx           = bandLengthPx(sTypical, r0Med, clubLenMm);
        L.headPx           = headLenPx;
        L.posePx           = (poseBoundPx > 0.0) ? poseBoundPx : -1.0;
        L.priorPx          = (priorNarg >= 2 && lengthPrior) ? lengthPrior->emaPx : -1.0;
        L.fusedPx          = fRec.fusedPx;
        L.fusedSigmaPx     = fRec.sigmaPx;
        L.fusedConf        = fRec.conf;
        L.fusedInstantPx   = fInst.fusedPx;
        L.fusedInstantConf = fInst.conf;
        L.ladderRung       = projLenRung;    // rung the tracker actually used (0 = pre-pass fused)
        L.ladderLenPx      = projLenPx;      // px the tracker actually used
        L.nEstimators      = fRec.nUsed;
        L.priorN           = priorNarg;
        L.headMeasN        = headMeasN;

        if (trace) {
            trace->fuseFusedPx     = fRec.fusedPx;
            trace->fuseFusedConf   = fRec.conf;
            trace->fuseInstantPx   = fInst.fusedPx;
            trace->fuseInstantConf = fInst.conf;
            trace->fuseHeadPx      = headLenPx;
            trace->fuseNEst        = fRec.nUsed;
            trace->fusePriorN      = priorNarg;
            trace->fuseSpread      = fRec.spread;
        }
    }

    // ── PASS 2: placement — build the samples from tierOf/confOf + headResults ─
    int spanFrames = 0, spanMeas = 0;
    std::vector<int> sampleFrame;   // frame index per emitted sample (Layer A snap map)
    for (int i = 0; i < nf; ++i) {
        if (std::isnan(gx[i])) continue;
        const int tier = int(tierOf[size_t(i)]);
        const double th = rec.thetaOut[i];
        const float conf = confOf[size_t(i)];
        const double ux = std::cos(th * kPi / 180.0), uy = std::sin(th * kPi / 180.0);

        ShaftSample2D s;
        s.t_us = tUs[i];
        s.gripPx = QPointF(gx[i], gy[i]);
        s.thetaRad = th * kPi / 180.0;
        s.conf = conf;

        // Stage-1 vision flag for the projected-head paths below: RAY ⇒
        // Measured, WEDGE ⇒ the blur-wedge flag (deliberately NOT Measured —
        // its consumers, snap + the B2 fit skip-guard, already accept 0x08),
        // else Coasted.
        const uint16_t s1Flag = (tier == RAY)   ? ShaftMeasured
                              : (tier == WEDGE) ? ShaftWedge
                                                : ShaftCoasted;

        bool placed = false;
        // BAND geometry is a DIRECT measurement of the head (butt-anchored via the
        // retro-band centres) — the Stage-2 head pass must NOT overwrite it. We
        // adjudicate Meas-vs-BAND by leaving BAND frames untouched below.
        if (tier == BAND) {
            const double sB = band[i].s, r0 = band[i].r0;
            const double bx = gx[i] - sB * r0 * ux, by = gy[i] - sB * r0 * uy;
            const double headX = bx + sB * clubLenMm * ux, headY = by + sB * clubLenMm * uy;
            s.headPx = QPointF(headX, headY);
            s.flags = ShaftMeasured;
            s.visibleLenPx = std::hypot(headX - gx[i], headY - gy[i]);
            placed = true;
        }

        // Stage-2 head result (empty headResults ⇒ Phase-A path; BAND already
        // placed ⇒ skipped). rOut is on-axis so headPx = grip + rOut·dir(θ).
        if (!placed && !headResults.empty()) {
            const HeadFrameResult &hr = headResults[size_t(i)];
            const float hSig = float(std::isfinite(hr.sigmaR) ? hr.sigmaR : -1.0);
            // Backswing streak confidence cap (gateB iter-2): corpus-proven
            // systematic short-lock on motion-blur streaks in [bs0, top] —
            // confident (≥0.5) head claims are reserved for the delivery phase
            // where the metrics consume them. Cap the EMITTED sample field only:
            // tier/KF behaviour and the raw trace (filled above from headResults)
            // are untouched, so SwingLab can still see the uncapped conf.
            float hConf = hr.headConf;
            if (i >= pm.bs0 && i <= pm.top)
                hConf = std::min(hConf, float(cfg.head.streakConfCap));
            if (hr.tier == HeadTier::Meas && std::isfinite(hr.rOut)) {
                s.headPx = QPointF(gx[i] + hr.rOut * ux, gy[i] + hr.rOut * uy);
                s.flags = ShaftMeasured;                       // measured head — NOT projected
                s.visibleLenPx = hr.rOut;
                s.headConf = hConf; s.headSigmaPx = hSig;
                placed = true;
            } else if (hr.tier == HeadTier::Off) {
                // head expected outside the frame — clamp to the ray/frame-edge
                // point. This is NOT a head position: flag it so QML never dots it
                // (0x80 rides on 0x10, so the projected-dim style already applies).
                const double re = rayEdgeRadius(gx[i], gy[i], th, frameW, frameH);
                s.headPx = QPointF(gx[i] + re * ux, gy[i] + re * uy);
                s.flags = uint16_t(s1Flag
                                   | ShaftHeadProjected | ShaftHeadOffFrame);
                s.visibleLenPx = re;
                s.headConf = hConf; s.headSigmaPx = hSig;
                placed = true;
            } else if (hr.tier == HeadTier::Pred && std::isfinite(hr.rOut)) {
                // a radial estimate exists but stage-1 is pred (the ray is a
                // kinematic guess the emitted head can't beat) — place at the
                // smoothed rOut but KEEP ShaftHeadProjected (headConf ≤ reinitCap).
                s.headPx = QPointF(gx[i] + hr.rOut * ux, gy[i] + hr.rOut * uy);
                s.flags = uint16_t(s1Flag | ShaftHeadProjected);
                s.visibleLenPx = hr.rOut;
                s.headConf = hConf; s.headSigmaPx = hSig;
                placed = true;
            }
            // Pred with NaN rOut falls through to the Phase-A projection below.
        }

        if (!placed) {
            // Phase-A projected head (bit-identical to the pre-Phase-B path when
            // the head pass is off).
            s.headPx = QPointF(gx[i] + projLenPx * ux, gy[i] + projLenPx * uy);
            s.flags = uint16_t(s1Flag | ShaftHeadProjected);
        }

        out.samples.push_back(s);
        sampleFrame.push_back(i);
        if (trace) { trace->frameIdx.push_back(i); trace->tier.push_back(tier);
                     trace->thetaDeg.push_back(th); trace->conf.push_back(conf); }
        if (i >= pm.bs0 && i <= pm.fin0) {
            ++spanFrames;
            // WEDGE counts as measured coverage: it is a vision measurement
            // (integrated, not per-pixel) and restores the spanMeas the S1
            // demotions took away (the coverage-coupling trap).
            if (tier == BAND || tier == RAY || tier == WEDGE) ++spanMeas;
        }
    }

    for (size_t i = 0; i < out.samples.size(); ++i) {
        if (out.samples.size() < 2) break;
        const size_t a = (i == 0) ? 0 : i - 1;
        const size_t b = (i + 1 < out.samples.size()) ? i + 1 : i;
        const double dth = std::remainder(out.samples[b].thetaRad - out.samples[a].thetaRad, 2 * kPi);
        const double dt = double(out.samples[b].t_us - out.samples[a].t_us) * 1e-6;
        out.samples[i].thetaDotRadS = (dt > 0) ? dth / dt : 0.0;
    }

    // ── Layer A: line re-registration («snap»), shaft_position_first §2A ───────
    // Dark by default. For each vision-tier sample (Measured|Wedge — never a
    // coasted/predicted frame), search (⊥ offset, Δθ) for the line that maximises
    // the local ridge integral, and record lineConf. Accept the snap iff its
    // lineConf clears minLineConf AND the snapped θ still admits the arm sector
    // (the same C4 arm-veto the DP emission uses: shaft never points into φ+180);
    // otherwise keep the original geometry but still record the drawn line's own
    // lineConf. Runs AFTER the thetaDot derivation so thetaDotRadS stays the
    // smoothed-track velocity — snap is a spatial registration, not a kinematic
    // edit. tiers/DP/ψ/coverage/length are all upstream and untouched. With
    // enabled=false nothing here runs ⇒ byte-identical to the pre-snap tracker.
    if (cfg.snap.enabled) {
        std::vector<double> appliedOffsets;   // |⊥ offset| over accepted snaps
        std::vector<double> measuredConfs;    // lineConf over every measured sample
        int snapN = 0;
        for (size_t k = 0; k < out.samples.size(); ++k) {
            ShaftSample2D& s = out.samples[k];
            const bool visionTier = (s.flags & ShaftMeasured) || (s.flags & ShaftWedge);
            if (!visionTier) continue;                          // coasted/pred keep lineConf = -1
            const int i = sampleFrame[k];
            cv::Mat g8 = frameSrc(i);
            if (g8.empty()) continue;                           // undecodable — no measurement
            cv::Mat g32; g8.convertTo(g32, CV_32F);

            const double gx0 = s.gripPx.x(), gy0 = s.gripPx.y();
            const double theta0 = s.thetaRad;
            const double drawnLen = std::hypot(s.headPx.x() - gx0, s.headPx.y() - gy0);
            const SnapResult sr = snapSearch(g32, gx0, gy0, theta0, drawnLen, cfg.snap, cfg.ridge);

            // Arm-plausibility sector: mirror frameEmission's C4 arm-veto — the
            // snapped shaft must not point within armVetoDeg of the lead forearm
            // (φ+180). φ NaN ⇒ no witness ⇒ admit.
            const double snapThetaDeg = theta0 * 180.0 / kPi + sr.dThetaDeg;
            bool armOk = true;
            if (i < int(phiS.size()) && !std::isnan(phiS[i])) {
                const double arm = std::fmod(phiS[i] + 180.0, 360.0);
                armOk = std::abs(circWrap(snapThetaDeg - arm)) >= cfg.armVetoDeg;
            }
            const bool moved  = std::abs(sr.offsetPx) > 1e-9 || std::abs(sr.dThetaDeg) > 1e-9;
            const bool accept = sr.bestLineConf >= float(cfg.snap.minLineConf) && armOk;

            if (accept && moved) {
                // Move the anchor to the perpendicular foot of the original grip on
                // the snapped line, then rotate θ; keep the drawn head consistent.
                const double thNew = theta0 + sr.dThetaDeg * kPi / 180.0;
                const double nx = -std::sin(theta0), ny = std::cos(theta0);
                const double ax = gx0 + sr.offsetPx * nx, ay = gy0 + sr.offsetPx * ny;
                const double ux = std::cos(thNew), uy = std::sin(thNew);
                const double t  = (gx0 - ax) * ux + (gy0 - ay) * uy;   // foot of g0 on the line
                const double gxN = ax + t * ux, gyN = ay + t * uy;
                // Projected heads follow grip+θ at the same drawn length; a measured
                // head (BAND / Stage-2 blob — no ShaftHeadProjected) stays put.
                if (s.flags & ShaftHeadProjected)
                    s.headPx = QPointF(gxN + drawnLen * ux, gyN + drawnLen * uy);
                s.gripPx  = QPointF(gxN, gyN);
                s.thetaRad = thNew;
                s.lineConf = sr.bestLineConf;
                appliedOffsets.push_back(std::abs(sr.offsetPx));
                ++snapN;
            } else {
                s.lineConf = sr.originLineConf;   // keep the drawn line, record its own support
            }
            measuredConfs.push_back(double(s.lineConf));
        }
        if (trace) {
            trace->snapAppliedN = snapN;
            auto medianOf = [](std::vector<double> v) -> double {
                if (v.empty()) return -1.0;
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                return v[v.size() / 2];
            };
            trace->medianSnapOffsetPx = medianOf(appliedOffsets);
            trace->medianLineConf     = medianOf(measuredConfs);
        }
    }

    // ── Layer B: P-position extraction (shaft_position_first §2 Layer B) ────────
    // Dark by default. Locate P1–P8 from the reconciled θ(t) (deg) + smoothed
    // lead-arm φ(t) (deg) and the tracker's own address/top/impact landmarks
    // (shaft_positions.h — image-plane parallel crossings with hysteresis), then
    // populate out.positions by SAMPLING the final emitted track at each P-time.
    // Report-only in B1: source = TrackSample, no milestone fit (that is B2), so
    // samples[]/coverage/length/θ are all untouched (positions ride alongside).
    // NB PhaseEvent emission for P2/P3/P5/P6/P8 belongs to the analyzer layer —
    // PositionsLadderStage (positions_ladder.h, refine.positionsLadder) promotes
    // these positions into ladder events there; nothing emits them here (keeps
    // B1's blast radius to the club block only).
    // Set to the located address-hold end while positions run; the trace
    // segmentation's Address event follows it (gated here so a positions-off run
    // keeps the legacy bs0 event byte-for-byte — the V soak contract).
    int addressEventFrame = -1;
    if (cfg.positions.enabled && !out.samples.empty()) {
        // Sample the emitted track at t: linear-interpolate grip/θ between the two
        // straddling samples; conf/length come from the NEARER sample; the drawn
        // head follows grip + len·dir(θ).
        auto sampleTrackAt = [&](int64_t t, ShaftPosition& pos) {
            const std::vector<ShaftSample2D>& S = out.samples;
            size_t b = 0;
            while (b < S.size() && S[b].t_us < t) ++b;
            if (b == 0 || b >= S.size()) {   // clamp to the boundary sample
                const ShaftSample2D& s = (b == 0) ? S.front() : S.back();
                pos.gripPx   = s.gripPx;
                pos.thetaRad = s.thetaRad;
                pos.conf     = s.conf;
                pos.lenPx    = std::hypot(s.headPx.x() - s.gripPx.x(), s.headPx.y() - s.gripPx.y());
            } else {
                const ShaftSample2D& a = S[b - 1];
                const ShaftSample2D& c = S[b];
                const double denom = double(c.t_us - a.t_us);
                const double frac  = denom > 0.0 ? double(t - a.t_us) / denom : 0.0;
                const double gxp = a.gripPx.x() + (c.gripPx.x() - a.gripPx.x()) * frac;
                const double gyp = a.gripPx.y() + (c.gripPx.y() - a.gripPx.y()) * frac;
                const double dth = std::remainder(c.thetaRad - a.thetaRad, 2 * kPi);
                const ShaftSample2D& near = (frac < 0.5) ? a : c;
                pos.gripPx   = QPointF(gxp, gyp);
                pos.thetaRad = a.thetaRad + dth * frac;
                pos.conf     = near.conf;
                pos.lenPx    = std::hypot(near.headPx.x() - near.gripPx.x(),
                                          near.headPx.y() - near.gripPx.y());
            }
            const double ux = std::cos(pos.thetaRad), uy = std::sin(pos.thetaRad);
            pos.headPx = QPointF(pos.gripPx.x() + pos.lenPx * ux, pos.gripPx.y() + pos.lenPx * uy);
        };

        // P1 = end of the ball-anchored address hold, NOT bs0 (takeaway start) —
        // camera-first: grip stillness on the face-on track, corroborated by the
        // BallAnchored flag span when present (shaft_positions.h addressHoldEndFrame).
        std::vector<char> baByFrame(size_t(nf), 0);
        for (const ShaftSample2D& s : out.samples) {
            if (!(s.flags & ShaftBallAnchored)) continue;
            int best = 0; int64_t bd = std::numeric_limits<int64_t>::max();
            for (int i = 0; i < nf; ++i) {
                const int64_t d = std::llabs(tUs[i] - s.t_us);
                if (d < bd) { bd = d; best = i; }
            }
            baByFrame[size_t(best)] = 1;
        }

        // W3 club-quiet mask: for each frame, the nearest ball sample's club-corridor
        // activity (BallSample2D.clubActivity) discriminates a still-grip club BOB
        // (activity high, near the ball) from a genuine quiet address (activity low).
        // quiet = activity present (>= 0) AND < positions.p1ClubQuietSigma. The mask
        // is passed only when a MAJORITY (>= 50%) of the pre-bs0 frames actually carry
        // activity — otherwise (live tracks, dark ball.clubActivity, or the ball never
        // found) it is nullptr and addressHoldEndFrame keeps its legacy grip-only
        // behaviour. The 50% floor is a judgement call: the mask's per-window "every
        // frame quiet" test needs dense coverage over the hold to be trustworthy;
        // below it, sparse -1 gaps would spuriously break otherwise-quiet holds (and
        // the two-tier fallback would just return the legacy answer anyway).
        std::vector<char> clubQuietByFrame;
        if (ball && !ball->frames.empty()) {
            std::vector<char> quiet(size_t(nf), 0);
            const int  qLast    = std::clamp(pm.bs0, 0, nf - 1);
            const int  qSpan    = qLast + 1;               // frames in [0, bs0]
            int        nPresent = 0;                       // of those, how many carry activity
            for (int i = 0; i < nf; ++i) {
                const BallSample2D* best = nullptr; int64_t bd = std::numeric_limits<int64_t>::max();
                for (const BallSample2D& f : ball->frames) {
                    const int64_t d = std::llabs(f.t_us - tUs[i]);
                    if (d < bd) { bd = d; best = &f; }
                }
                if (!best || best->clubActivity < 0.f) continue;   // absent ⇒ not quiet
                if (i <= qLast) ++nPresent;
                if (best->clubActivity < cfg.positions.p1ClubQuietSigma) quiet[size_t(i)] = 1;
            }
            if (qSpan > 0 && nPresent * 2 >= qSpan) clubQuietByFrame = std::move(quiet);
        }
        const std::vector<char>* clubQuietPtr = clubQuietByFrame.empty() ? nullptr : &clubQuietByFrame;

        // pm.onsetFloor (the veto's no-return boundary — the LAST settle) floors
        // the walk-back: without it the absolute stillAt thresholds pass nowhere
        // near a real 2–4 px/f settle and the hold-end walks through the fidget
        // to the deep pre-fidget hold even from a corrected bs0. -1 (veto dark /
        // not fired) keeps the legacy walk byte-identical.
        const int p1Frame = addressHoldEndFrame(gx, gy, baByFrame, pm.bs0, cfg.positions,
                                                clubQuietPtr, pm.onsetFloor);
        addressEventFrame = p1Frame;

        const std::vector<PTime> pts =
            locatePTimes(tUs, rec.thetaOut, phiS, p1Frame, pm.top, pm.impact, pm.fin0, cfg.positions);
        out.positions.reserve(pts.size());
        for (const PTime& pt : pts) {
            ShaftPosition pos;
            pos.p      = pt.p;
            pos.t_us   = pt.tUs;
            pos.source = uint8_t(PositionSource::TrackSample);
            pos.stackN = 0;
            pos.sigmaThetaDeg = -1.f;
            pos.sigmaLenPx    = -1.f;
            sampleTrackAt(pt.tUs, pos);
            out.positions.push_back(pos);
        }

        // ── B2 milestone fit (shaft_position_first §2 Layer B "B-fit") ──────────
        // Dark until positions.fitEnabled. Re-measure each located P by a ±k-frame
        // shift-and-stack joint (grip, θ, L) fit (shaft_position_fit.h): on ACCEPT it
        // OVERWRITES the B1 TrackSample with the fitted geometry + source=MilestoneFit
        // + honest σ from the near-max plateau; on REJECT the B1 sample is KEPT (never
        // degrade). fitEnabled=false ⇒ the loop never runs ⇒ positions are byte-
        // identical to B1. A frame-source-less swing (fused / undecodable) leaves the
        // fit a no-op (empty stack ⇒ reject ⇒ B1 kept). samples[]/θ/coverage/length
        // above are untouched — the fit upgrades the report-only positions[] only.
        if (cfg.positions.fit.fitEnabled) {
            // Per-frame locally-smoothed ω(t) (deg/s) for the stack registration:
            // central difference of the reconciled θ over the frame timebase, then
            // median(5)+Gaussian(2) — the φ/joint smoothing convention. A flipped θ
            // is a constant offset (d/dt unchanged), so ω survives the impact-blur
            // flip that corrupts the absolute θ.
            std::vector<double> omega(size_t(nf), 0.0);
            for (int i = 0; i < nf; ++i) {
                const int a = std::max(0, i - 1), b = std::min(nf - 1, i + 1);
                const double dth = circWrap(rec.thetaOut[b] - rec.thetaOut[a]);
                const double dt  = double(tUs[b] - tUs[a]) * 1e-6;
                omega[size_t(i)] = (dt > 0.0) ? dth / dt : 0.0;   // deg/s
            }
            omega = gaussianFilter1d(medianFilter1d(omega, 5), 2.0);

            const double fusedLen = (out.lengths.fusedPx > 0.0) ? out.lengths.fusedPx : projLenPx;
            auto nearestFrame = [&](int64_t t) {
                int best = 0; int64_t bd = std::numeric_limits<int64_t>::max();
                for (int i = 0; i < nf; ++i) {
                    const int64_t d = std::llabs(tUs[i] - t);
                    if (d < bd) { bd = d; best = i; }
                }
                return best;
            };
            for (ShaftPosition& pos : out.positions) {
                const int cf = nearestFrame(pos.t_us);
                // Never-degrade guard (B2 corpus gate): a position whose underlying
                // track instant is already a confident vision measurement is NOT
                // re-fit — the fit only rescues positions the DP lost (pred/coast
                // tier, weak conf). Nearest emitted sample stands in for the instant.
                if (cfg.positions.fit.skipMeasuredConf > 0.0 && !out.samples.empty()) {
                    const ShaftSample2D* ns = nullptr;
                    int64_t bd = std::numeric_limits<int64_t>::max();
                    for (const ShaftSample2D& s : out.samples) {
                        const int64_t d = std::llabs(s.t_us - pos.t_us);
                        if (d < bd) { bd = d; ns = &s; }
                    }
                    if (ns && (ns->flags & (ShaftMeasured | ShaftWedge))
                           && !(ns->flags & ShaftCoasted)
                           && ns->conf >= float(cfg.positions.fit.skipMeasuredConf))
                        continue;
                }
                // Ball evidence only where the ball is physically on the tee: address
                // (P1) and impact (P7). Nearest pre-launch found sample to the P-time.
                const BallSample2D* ballNear = nullptr;
                BallSample2D bs;
                if (ball && !ball->frames.empty() && (pos.p == 1 || pos.p == 7)) {
                    int64_t bd = std::numeric_limits<int64_t>::max();
                    for (const BallSample2D& f : ball->frames) {
                        if (!f.found) continue;
                        if (ball->launchTUs >= 0 && f.t_us > ball->launchTUs) continue;   // pre-launch
                        const int64_t d = std::llabs(f.t_us - pos.t_us);
                        if (d < bd) { bd = d; bs = f; }
                    }
                    if (bd != std::numeric_limits<int64_t>::max()) ballNear = &bs;
                }
                const PositionFitResult fr =
                    fitPosition(frameSrc, pos.p, cf, pos.t_us, tUs, gx, gy, rec.thetaOut, omega,
                                phiS, armFloorMedPx, fusedLen, ballNear, frameW, frameH,
                                cfg.ridge, cfg.positions.fit, cfg.armVetoDeg);
                if (fr.accepted) {
                    pos.gripPx        = fr.gripPx;
                    pos.headPx        = fr.headPx;
                    pos.thetaRad      = fr.thetaRad;
                    pos.lenPx         = fr.lenPx;
                    pos.conf          = fr.conf;
                    pos.sigmaThetaDeg = fr.sigmaThetaDeg;
                    pos.sigmaLenPx    = fr.sigmaLenPx;
                    // Drawn extent from the fused club length (B2 corpus gate: the
                    // ridge-terminus L was weak everywhere — blur + band gaps); the
                    // fit's θ/grip stand, the head is re-projected at the fused L
                    // with the fusion posterior as σL.
                    if (cfg.positions.fit.useFusedLen && out.lengths.fusedPx > 0.0) {
                        pos.lenPx  = out.lengths.fusedPx;
                        pos.headPx = QPointF{ pos.gripPx.x() + pos.lenPx * std::cos(pos.thetaRad),
                                              pos.gripPx.y() + pos.lenPx * std::sin(pos.thetaRad) };
                        if (out.lengths.fusedSigmaPx > 0.0)
                            pos.sigmaLenPx = float(out.lengths.fusedSigmaPx);
                    }
                    pos.stackN        = fr.stackN;
                    pos.source        = uint8_t(PositionSource::MilestoneFit);
                }
            }
        }
    }

    // ── Layer C: synthesis between anchors (shaft_position_first §2 Layer C) ────
    // Dark by default. With ≥2 located P-anchors, fill a VISUALIZATION-TIER series
    // (out.synth) — C¹ Hermite-interpolated samples on a dense fixed cadence
    // (cfg.synth.rateHz, default 240 Hz) STRICTLY between consecutive anchors, so
    // ¼× replay / the fan fill the inter-frame gaps. Flagged ShaftSynthesized so
    // metrics/scoring/
    // estimands EXCLUDE it (shaft_synthesis.h). samples[]/positions[]/θ/coverage/
    // length above are untouched — synth rides alongside. cfg.synth.enabled==false
    // ⇒ out.synth stays empty ⇒ swing.json byte-identical (soak contract).
    if (cfg.synth.enabled && out.positions.size() >= 2) {
        // Per-anchor slopes from the smoothed track: θ̇ (deg/s) = central-difference
        // ω of the reconciled θ(t), median(5)+Gaussian(2) (the ω(t) convention shared
        // with the B2 fit registration), sampled at the anchor instants → rad/s.
        std::vector<double> omega(size_t(nf), 0.0);   // deg/s
        for (int i = 0; i < nf; ++i) {
            const int a = std::max(0, i - 1), b = std::min(nf - 1, i + 1);
            const double dth = circWrap(rec.thetaOut[b] - rec.thetaOut[a]);
            const double dt  = double(tUs[b] - tUs[a]) * 1e-6;
            omega[size_t(i)] = (dt > 0.0) ? dth / dt : 0.0;
        }
        omega = gaussianFilter1d(medianFilter1d(omega, 5), 2.0);
        // Grip velocity per frame (px/s), central difference of the pose-grip path.
        std::vector<double> gvx(size_t(nf), 0.0), gvy(size_t(nf), 0.0);
        for (int i = 0; i < nf; ++i) {
            const int a = std::max(0, i - 1), b = std::min(nf - 1, i + 1);
            const double dt = double(tUs[b] - tUs[a]) * 1e-6;
            if (dt > 0.0) { gvx[size_t(i)] = (gx[b] - gx[a]) / dt; gvy[size_t(i)] = (gy[b] - gy[a]) / dt; }
        }
        // Linear-interpolate a per-frame signal at time t over the tUs timebase.
        auto sampleAt = [&](const std::vector<double>& sig, int64_t t) {
            int b = 0; while (b < nf && tUs[b] < t) ++b;
            if (b <= 0)      return sig.front();
            if (b >= nf)     return sig.back();
            const double denom = double(tUs[b] - tUs[b - 1]);
            const double frac  = denom > 0.0 ? double(t - tUs[b - 1]) / denom : 0.0;
            return sig[size_t(b - 1)] + (sig[size_t(b)] - sig[size_t(b - 1)]) * frac;
        };
        std::vector<double>  thetaDot; thetaDot.reserve(out.positions.size());
        std::vector<QPointF> gripVel;  gripVel.reserve(out.positions.size());
        for (const ShaftPosition& p : out.positions) {
            thetaDot.push_back(sampleAt(omega, p.t_us) * kPi / 180.0);   // deg/s → rad/s
            gripVel.push_back(QPointF{ sampleAt(gvx, p.t_us), sampleAt(gvy, p.t_us) });
        }
        // Dense visualization grid: sample the synthesized tier at cfg.synth.rateHz
        // (default 240 Hz) rather than the source frame cadence, so ¼× replay and the
        // shaft fan read a smooth series that fills the inter-frame gaps. The grid
        // spans only [first anchor, last anchor] (synth emits strictly between
        // anchors) and synthesizeBetweenAnchors still drops any tick landing on an
        // interior anchor. rateHz <= 0 ⇒ fall back to the source frame timestamps.
        std::vector<int64_t> synthGrid;
        if (cfg.synth.rateHz > 0.0) {
            const int64_t t0     = out.positions.front().t_us;
            const int64_t t1     = out.positions.back().t_us;
            const double  stepUs = 1.0e6 / cfg.synth.rateHz;
            if (t1 > t0 && stepUs >= 1.0) {
                synthGrid.reserve(size_t(double(t1 - t0) / stepUs) + 2);
                for (double t = double(t0); t <= double(t1) + 0.5; t += stepUs)
                    synthGrid.push_back(int64_t(t + 0.5));
            }
        }
        const std::vector<int64_t> &synthTimes = synthGrid.empty() ? tUs : synthGrid;
        out.synth = synthesizeBetweenAnchors(out.positions, thetaDot, gripVel, synthTimes, cfg.synth);
    }

    out.coverage = spanFrames > 0 ? float(spanMeas) / float(spanFrames) : 0.f;
    out.valid = out.coverage >= float(cfg.coverageMin);
    // Always exposed (not gated on trace!=nullptr — trace is null for fused/
    // hasImu swings on the production path) so the ball-anchor post-hoc pass
    // can find the current address boundary regardless of presence mode.
    out.addressPhaseFrame = pm.bs0;
    // Same unconditional exposure for the onset veto's no-return floor — the
    // EventRefine slot re-floors its Address walk-back on it (swing_analysis.h
    // ShaftTrack2D::onsetFloorFrame). -1 when the veto is dark / never fired.
    out.onsetFloorFrame = pm.onsetFloor;

    if (trace) {
        trace->phases = pm; trace->phiSmoothed = phiS; trace->chir = chir;
        trace->spanLo = spanLo; trace->spanHi = spanHi; trace->heavyFrames = heavy;
        trace->dp = dp; trace->recon = rec;
        // S2 wedge diagnostics — the predicted rate, and the measured fan where
        // one was found (NaN elsewhere). Empty when the wedge is dark.
        if (cfg.wedge.enabled) {
            const double kWNaN = std::numeric_limits<double>::quiet_NaN();
            trace->wedgeTExpS = wedgeTExp;
            trace->wedgeOmegaDegS = omegaPred;
            trace->wedgeCentroidDeg.assign(size_t(nf), kWNaN);
            trace->wedgeWidthDeg.assign(size_t(nf), kWNaN);
            for (int i = 0; i < nf; ++i)
                if (wedgeCand[size_t(i)].ok) {
                    trace->wedgeCentroidDeg[size_t(i)] = wedgeCand[size_t(i)].centroidDeg;
                    trace->wedgeWidthDeg[size_t(i)]    = wedgeCand[size_t(i)].widthDeg;
                }
        }
        // Vision-only phase landmarks: real swing ⇒ vision-grade conf, else 0.
        const bool swingDetected = std::any_of(pm.phase.begin(), pm.phase.end(),
                                               [](SwingPhase p) { return p != SwingPhase::Addr; });
        trace->segmentation = phasesToSegmentation(pm, tUs, swingDetected ? 0.5f : 0.0f,
                                                   addressEventFrame, cfg.emitTakeaway);
    }
    return out;
}

} // namespace pinpoint::analysis
