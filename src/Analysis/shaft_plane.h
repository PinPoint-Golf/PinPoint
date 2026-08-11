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

// The face-on swing-plane transition delta (wrist_cock_model.md §9/§13;
// transition_plane_producer_brief.md). THE PLANE IS THE POINT: the shaft vector
// sweeps a circle of fixed radius on the swing plane, and a planar closed curve
// images as an ellipse whose axis ratio is the cosine of the plane's inclination
// to the image plane. So the plane falls straight out of the SHAPE the shaft
// vector traces — no foreshortening model, no per-frame length, no sign
// ambiguity, because a plane fixes which side of the node line every direction
// is on.
//
//     ι     = arccos(minor / major)      larger ι = flatter, smaller = steeper
//     delta = ι_back − ι_down            positive = the club STEEPENED in transition
//
// Two windows off the phase ladder: backswing = takeaway→top, downswing =
// top→impact, both bounds INCLUSIVE (the sample exactly at top is counted in
// both — reference behaviour, and changing it moves the sample counts).
//
// Note what is and is not assumed. head = grip + L·u(θ) algebraically, so the
// path carries no new MEASUREMENT beyond (grip, θ, radius) — but whether the
// resulting 3-D path is PLANAR is a genuine structural claim the algebra does
// not grant, and the conic fit is what tests it. Conflating those two is an
// error; it cost the research a session.
//
// TWO LOAD-BEARING CHOICES, each worth tens of degrees and both learned the
// hard way. They are not tuning knobs:
//
//   · Fit the SHAFT VECTOR (head − grip), never the absolute head path. The
//     absolute path is grip translation PLUS club rotation, so it is not a
//     planar closed curve about a fixed centre and a conic fitted to it is
//     badly conditioned — split-half repeatability degrades from 0.6–0.7° to
//     9.2°, worst case 47.6°.
//   · Normalise ISOTROPICALLY — one shared scale from the pooled radial
//     spread, never per-axis standard deviations. Per-axis scaling is an
//     anisotropic map: it changes an ellipse's axis ratio AND its orientation,
//     i.e. exactly the two quantities being measured. The earlier anisotropic
//     version reported inclinations wrong by up to 40°.
//
// SIGN INVARIANCE. The conic coefficient vector v is defined only up to sign
// (it is a null direction), and this header does not try to match any
// particular solver's choice, because nothing downstream can see it: b²−4ac is
// quadratic in v so the ellipse gate is invariant; M = [[a,b/2],[b/2,c]]
// negates, so its eigenVALUES negate but its eigenVECTOR directions do not; the
// centre solve negates on both sides; `const` negates in step with the
// eigenvalues so −const/ev, hence the axes, the ratio and ι, are invariant; the
// node's mod-180 fold absorbs the eigenvector's 180° flip; and median|D·v| is
// invariant. What is NOT invariant is SCALE — median|D·v| is linear in ‖v‖ — so
// v is unit-normalised before the residual is taken.
//
// Reference implementation: tools/shaftlab/plane_probe.py (`fit_ellipse`,
// `head_path_plane`, `corpus`), which produced the corpus golden file
// docs/research/data/wrist_cock_model/transition_plane_corpus.csv.
//
// Pure header: no Qt, no OpenCV, no tracker types — standalone-testable. The
// caller does the head−grip subtraction and the sample-tier selection, so this
// header never learns what a tracker is.

namespace pinpoint::analysis {

// ─────────────────────────────────────────────────────────────────────────────
// Inputs
// ─────────────────────────────────────────────────────────────────────────────

// One shaft-vector sample: the window clock plus headPx − gripPx in IMAGE px.
struct ShaftPlanePoint {
    std::int64_t tUs = 0;
    double       vx  = 0.0;
    double       vy  = 0.0;
};

// A located P-anchor. The synth channel's ONLY honest quality (see the split-half
// trap on PlaneWindowFit) — anchors are all the information that channel has.
struct PlaneAnchor {
    std::int64_t tUs  = 0;
    float        conf = -1.f;
};

struct ShaftPlaneInput {
    // The honest channel: measured heads only (headConf > 0, ShaftSynthesized
    // excluded) — the selection plane_probe.load_run makes.
    std::vector<ShaftPlanePoint> measured;
    // The C¹ Hermite through the P-anchors. Really "the plane implied by the
    // located P-positions", an inference rather than an observation; admitted
    // as a channel only because this measure is experimental and cannot fire.
    std::vector<ShaftPlanePoint> synth;
    // Spans the synth channel's quality. Ignored by the measured channel.
    std::vector<PlaneAnchor>     anchors;

    std::int64_t takeawayUs = 0;
    std::int64_t topUs      = 0;
    std::int64_t impactUs   = 0;
    // false ⇒ the ladder was incomplete and nothing is computed at all.
    bool         haveWindows = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// Rejections — exactly the five ways fit_ellipse returns None, plus two the
// caller can hit. Recorded per window even on the channel that lost, because a
// synth-tagged emission must be able to say WHICH window failed and HOW.
// ─────────────────────────────────────────────────────────────────────────────
enum class ConicReject : int {
    None = 0,
    NoWindow,          // haveWindows == false / the window was never attempted
    TooFewSamples,     // n < 12
    NonFinite,         // a NaN/inf reached the fit (no Python counterpart)
    NotAnEllipse,      // b² − 4ac >= 0 — hyperbola/parabola, not planar-closed
    DegenerateAxes,    // |eigenvalue| < 1e-12
    CentreOnConic,     // the conic evaluates to exactly 0 at its own centre
    NonPositiveMajor,  // major axis <= 0
    IllConditioned,    // minor/major < kMinAxisRatio — a needle, not a plane
};

// A NEEDLE IS NOT A PLANE. An arc too short or too sparse to constrain the second
// axis still admits a conic — an arbitrarily elongated one threaded through the
// points — and ι = arccos(minor/major) then runs toward 90° and takes the delta
// with it. This is NOT the yield-rescue §9 forbids; it is the opposite, a floor on
// conditioning, and it exists because the corpus showed the failure is real and
// silent: three of the reference's 33 fits are needles (ι 81.7°, 82.9°, 89.0° —
// the last an axis ratio of 0.017, a straight line to within 2%), all three in the
// sparser window of a 2026-07-04 swing, producing deltas of −58.5°, +46.6° and
// +56.7° that no golf swing performs.
//
// 0.26 is ι = 75°, chosen at the gap the corpus shows: of 66 window fits, 53 sit
// below ι 45°, six between 45° and 60°, four between 60° and 75°, and then the
// three needles. It is a conditioning floor, not a claim about golf — the absolute
// ι is uncalibrated either way (§9).
//
// DELIBERATE DIVERGENCE from tools/shaftlab/plane_probe.py, which has no such gate.
// shaft_plane_corpus_test pins the divergence by name rather than asserting blanket
// parity, so it reads as a decision and not as drift.
inline constexpr double kMinAxisRatio = 0.26;

inline const char *conicRejectName(ConicReject r)
{
    switch (r) {
    case ConicReject::None:             return "none";
    case ConicReject::NoWindow:         return "noWindow";
    case ConicReject::TooFewSamples:    return "tooFewSamples";
    case ConicReject::NonFinite:        return "nonFinite";
    case ConicReject::NotAnEllipse:     return "notAnEllipse";
    case ConicReject::DegenerateAxes:   return "degenerateAxes";
    case ConicReject::CentreOnConic:    return "centreOnConic";
    case ConicReject::NonPositiveMajor: return "nonPositiveMajor";
    case ConicReject::IllConditioned:   return "illConditioned";
    }
    return "unknown";
}

// One window's conic.
struct ConicFit {
    bool        ok               = false;
    ConicReject reject           = ConicReject::NoWindow;
    int         n                = 0;
    double      ratioMinorMajor  = 0.0;
    double      iotaDeg          = 0.0;   // arccos(ratio); larger = flatter
    double      nodeDeg          = 0.0;   // major-axis bearing, folded to (−90, +90]
    double      conicResid       = -1.0;  // median |D·v|, NORMALISED coords, ‖v‖₂ = 1
    double      coeff[6]         = { 0, 0, 0, 0, 0, 0 };  // a,b,c,d,e,f (normalised space)
};

// One window plus its CHANNEL-APPROPRIATE quality.
//
// THE SPLIT-HALF TRAP, made structural. Odd and even synth samples lie on the
// same Hermite, so a split-half refit on that channel reads near zero: it
// measures interpolation smoothness, not repeatability, and would fake
// excellent quality. `splitHalfDeg` is therefore written ONLY by the measured
// path and `anchors` ONLY by the synth path — there is no code path that can
// compute a split-half on interpolated samples, so the failure mode is
// unreachable rather than merely discouraged. Never compare quality across
// channels.
//
// WHAT THE SPLIT-HALF CANNOT SEE, even on the measured channel. It tests
// REPEATABILITY, not VALIDITY. When an arc is too short to constrain the second
// axis, both the odd and the even half collapse to the SAME elongated fit, so
// the disagreement between them is small — a bad number, confidently agreed on.
// Observed in the corpus: a window with ι = 89.03° (axis ratio 0.017, a straight
// line) returned a split-half of 0.00°, the best score obtainable. Conditioning
// is therefore gated on the axis ratio (kMinAxisRatio) BEFORE a fit is accepted;
// a low split-half is evidence about precision only, and never on its own
// evidence that a fit means anything.
struct PlaneWindowFit {
    ConicFit fit;
    double   splitHalfDeg = -1.0;  // MEASURED only; < 0 = not computed (n < 24, or a sub-fit failed)
    int      anchors      = 0;     // SYNTH only: P-anchors spanning this window
};

enum class PlaneChannel : int { None = -1, Measured = 0, Synth = 1 };

inline const char *planeChannelName(PlaneChannel c)
{
    switch (c) {
    case PlaneChannel::None:     return "none";
    case PlaneChannel::Measured: return "measured";
    case PlaneChannel::Synth:    return "synth";
    }
    return "unknown";
}

struct PlaneChannelFit {
    bool           fitted        = false;  // BOTH windows produced an ellipse
    PlaneWindowFit back, down;
    double         deltaDeg      = 0.0;    // ι_back − ι_down; meaningless unless fitted
    float          anchorConfMin = -1.f;   // SYNTH only: the weakest anchor over both windows
};

struct ShaftPlaneResult {
    bool            valid   = false;              // some channel fitted both windows
    PlaneChannel    channel = PlaneChannel::None; // WHICH channel is the emitted value
    // BOTH are always populated, including the loser's per-window reject codes.
    // Recording the pair on every swing IS the experiment (brief §8.3): the
    // question "does the synth plane track the measured plane?" then answers
    // itself in production data instead of needing another lab study.
    PlaneChannelFit measured, synth;

    // Headline convenience — mirrors the selected channel; 0 when !valid.
    double deltaDeg    = 0.0;
    double iotaBackDeg = 0.0;
    double iotaDownDeg = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
namespace plane_detail {

// Floored modulo, because Python's % floors and std::fmod truncates. The node
// line folds through this; a truncated fmod puts every negative node 180° out.
inline double floorMod(double a, double m)
{
    double r = std::fmod(a, m);
    if (r < 0) r += m;
    return r;
}

// Median with the even-n midpoint AVERAGED, matching np.median. Sorts in place.
inline double medianInPlace(std::vector<double> &v)
{
    if (v.empty()) return -1.0;
    const std::size_t n = v.size();
    std::sort(v.begin(), v.end());
    return (n % 2) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// One-sided Jacobi SVD of an n×6 matrix, for the right singular vector of the
// smallest singular value — the direct-linear conic fit's null direction.
//
// WHY NOT the eigendecomposition of DᵀD, which is the obvious shortcut. Forming
// DᵀD squares the condition number, and the accuracy that buys back is not
// free here: 25 of the corpus's 28 measured-channel misses are discriminant
// rejections, and the golden cross-check asserts the same misses for the same
// reasons, with the nearest ACCEPTED ellipse sitting at b²−4ac = −7.9e-4. The
// exact synthetic ellipses in the unit test are worse still — they have σ₆ = 0
// exactly, where a DᵀD eigenvalue is pure rounding noise. One-sided Jacobi is
// the same amount of code, keeps the error linear in σ₁/σ₆ rather than
// quadratic, and yields an orthonormal V by construction, which matters because
// the conic residual is the one output that is NOT scale-invariant.
//
// `a` is n×6 column-major and is DESTROYED (it leaves holding D·V). `v` is
// 6×6 column-major, filled with the accumulated rotations. Both A and V take
// the same right-rotations, so the invariant D·V = A_final holds exactly and
// column j of V is the right singular vector for ‖A[j]‖.
inline void jacobiSvdRight(double *a, int n, double *v)
{
    constexpr int kCols   = 6;
    constexpr int kSweeps = 30;      // never binds in practice; corpus converges in 6–9
    constexpr double kOrthTol = 1e-15;

    for (int i = 0; i < kCols * kCols; ++i) v[i] = 0.0;
    for (int i = 0; i < kCols; ++i) v[i * kCols + i] = 1.0;

    for (int sweep = 0; sweep < kSweeps; ++sweep) {
        double worst = 0.0;
        for (int p = 0; p < kCols - 1; ++p) {
            for (int q = p + 1; q < kCols; ++q) {
                double app = 0.0, aqq = 0.0, apq = 0.0;
                for (int i = 0; i < n; ++i) {
                    const double ap = a[p * n + i], aq = a[q * n + i];
                    app += ap * ap;
                    aqq += aq * aq;
                    apq += ap * aq;
                }
                if (!(app > 0.0) || !(aqq > 0.0)) continue;   // a zero column needs no rotation
                const double off = std::fabs(apq) / std::sqrt(app * aqq);
                worst = std::max(worst, off);
                if (off <= kOrthTol) continue;

                // Smaller root of t² + 2ζt − 1 = 0 — the rotation that
                // orthogonalises columns p and q of A.
                const double zeta = (aqq - app) / (2.0 * apq);
                const double sgn  = (zeta >= 0.0) ? 1.0 : -1.0;
                const double t    = sgn / (std::fabs(zeta) + std::sqrt(1.0 + zeta * zeta));
                const double c    = 1.0 / std::sqrt(1.0 + t * t);
                const double s    = c * t;

                for (int i = 0; i < n; ++i) {
                    const double ap = a[p * n + i], aq = a[q * n + i];
                    a[p * n + i] = c * ap - s * aq;
                    a[q * n + i] = s * ap + c * aq;
                }
                for (int i = 0; i < kCols; ++i) {
                    const double vp = v[p * kCols + i], vq = v[q * kCols + i];
                    v[p * kCols + i] = c * vp - s * vq;
                    v[q * kCols + i] = s * vp + c * vq;
                }
            }
        }
        // Quadratic convergence once off-diagonality drops below ~0.1. If the
        // cap ever bound, V is still exactly orthogonal (it is a product of
        // Givens rotations) — only A's columns would be imperfectly decoupled,
        // so the fit degrades continuously and there is no failure branch.
        if (worst <= kOrthTol) break;
    }
}

// Eigendecomposition of the symmetric 2×2 [[m00, m01], [m01, m11]], closed form.
// Eigenvalues come out ASCENDING to reproduce numpy.linalg.eigh's ordering, so
// the major/minor argmax below lands on the same index the reference picks.
struct Eig2 {
    double ev[2]      = { 0.0, 0.0 };
    double vec[2][2]  = { { 1.0, 0.0 }, { 0.0, 1.0 } };  // vec[k] = eigenvector for ev[k]
};
inline Eig2 eigSym2(double m00, double m01, double m11)
{
    Eig2 e;
    const double p = 0.5 * (m00 + m11);
    const double q = 0.5 * (m00 - m11);
    const double r = m01;
    const double h = std::hypot(q, r);
    e.ev[0] = p - h;
    e.ev[1] = p + h;

    // Eigenvector for the LARGER eigenvalue, taking whichever of the two
    // algebraically-equivalent spellings has the larger norm (r² = (h−q)(h+q),
    // so ‖(h+q, r)‖² = 2h(h+q) and ‖(r, h−q)‖² = 2h(h−q)).
    double ux, uy;
    if (h == 0.0) {                    // a multiple of I — a circle; eigh returns I
        ux = 1.0; uy = 0.0;
    } else if (q >= 0.0) {
        ux = h + q; uy = r;
    } else {
        ux = r; uy = h - q;
    }
    const double un = std::hypot(ux, uy);
    if (un > 0.0) { ux /= un; uy /= un; }
    else          { ux = 1.0; uy = 0.0; }

    e.vec[1][0] =  ux; e.vec[1][1] =  uy;
    e.vec[0][0] = -uy; e.vec[0][1] =  ux;   // orthogonal complement
    return e;
}

} // namespace plane_detail

// ─────────────────────────────────────────────────────────────────────────────
// The direct linear conic fit — the port of plane_probe.fit_ellipse. Every gate
// below is in the reference's order and with its exact comparison; the golden
// cross-check asserts the same 33 fits and the same 28 misses, so a relaxed or
// reordered gate is a behaviour change, not a cleanup.
// ─────────────────────────────────────────────────────────────────────────────
inline ConicFit fitConic(const double *x, const double *y, int n)
{
    ConicFit f;
    f.n = n;
    if (n < 12) { f.reject = ConicReject::TooFewSamples; return f; }
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(x[i]) || !std::isfinite(y[i])) {
            f.reject = ConicReject::NonFinite;
            return f;
        }

    // ISOTROPIC normalisation: mean-centre, then ONE shared scale from the
    // pooled radial spread. See the header preamble — per-axis σ is the 40°
    // mistake. `== 0.0` and not an epsilon, matching the reference's `or 1.0`.
    double mx = 0.0, my = 0.0;
    for (int i = 0; i < n; ++i) { mx += x[i]; my += y[i]; }
    mx /= n; my /= n;
    double s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double dx = x[i] - mx, dy = y[i] - my;
        s2 += dx * dx + dy * dy;
    }
    double sc = std::sqrt(s2 / n);
    if (sc == 0.0) sc = 1.0;

    // Design matrix D = [X², XY, Y², X, Y, 1], column-major.
    std::vector<double> d(std::size_t(6) * n), a(std::size_t(6) * n);
    for (int i = 0; i < n; ++i) {
        const double X = (x[i] - mx) / sc, Y = (y[i] - my) / sc;
        d[std::size_t(0) * n + i] = X * X;
        d[std::size_t(1) * n + i] = X * Y;
        d[std::size_t(2) * n + i] = Y * Y;
        d[std::size_t(3) * n + i] = X;
        d[std::size_t(4) * n + i] = Y;
        d[std::size_t(5) * n + i] = 1.0;
    }
    a = d;

    double vm[36];
    plane_detail::jacobiSvdRight(a.data(), n, vm);

    // The null direction: the column of V whose A-column has the smallest norm.
    // Strict `<` so the FIRST minimum wins, for determinism.
    int k = 0;
    double best = -1.0;
    for (int j = 0; j < 6; ++j) {
        double nrm = 0.0;
        for (int i = 0; i < n; ++i) { const double t = a[std::size_t(j) * n + i]; nrm += t * t; }
        if (best < 0.0 || nrm < best) { best = nrm; k = j; }
    }
    double v[6];
    double vn = 0.0;
    for (int r = 0; r < 6; ++r) { v[r] = vm[k * 6 + r]; vn += v[r] * v[r]; }
    vn = std::sqrt(vn);
    if (!(vn > 0.0) || !std::isfinite(vn)) { f.reject = ConicReject::NonFinite; return f; }
    for (int r = 0; r < 6; ++r) v[r] /= vn;   // ‖v‖₂ = 1: conicResid is scale-dependent

    const double ca = v[0], cb = v[1], cc = v[2], cd = v[3], ce = v[4], cf = v[5];
    for (int r = 0; r < 6; ++r) f.coeff[r] = v[r];

    if (cb * cb - 4.0 * ca * cc >= 0.0) {     // hyperbola/parabola: not planar-closed
        f.reject = ConicReject::NotAnEllipse;
        return f;
    }

    const plane_detail::Eig2 e = plane_detail::eigSym2(ca, 0.5 * cb, cc);
    if (std::fabs(e.ev[0]) < 1e-12 || std::fabs(e.ev[1]) < 1e-12) {
        f.reject = ConicReject::DegenerateAxes;
        return f;
    }

    // Centre: 2M·cen = [−d, −e]. det(2M) = 4ac − b² = −(b²−4ac) > 0 because the
    // discriminant gate above already passed, so Cramer's rule cannot divide by
    // zero and there is no throw path.
    const double det = 4.0 * ca * cc - cb * cb;
    const double cx  = (-2.0 * cc * cd + cb * ce) / det;
    const double cy  = (-2.0 * ca * ce + cb * cd) / det;

    const double konst = ca * cx * cx + cb * cx * cy + cc * cy * cy + cd * cx + ce * cy + cf;
    if (konst == 0.0) { f.reject = ConicReject::CentreOnConic; return f; }

    const double ax0 = std::sqrt(std::fabs(-konst / e.ev[0]));
    const double ax1 = std::sqrt(std::fabs(-konst / e.ev[1]));
    // np.argmax returns the FIRST maximum on ties, so `>=` and not `>`. The tie
    // is reachable — a circular arc gives an axis ratio of exactly 1 — but note
    // that when it ties there is no major axis, so the node below is arbitrary
    // in both implementations; matching numpy here buys parity, not meaning.
    const int    i   = (ax0 >= ax1) ? 0 : 1;
    const double maj = (i == 0) ? ax0 : ax1;
    const double min_ = (i == 0) ? ax1 : ax0;
    if (!(maj > 0.0)) { f.reject = ConicReject::NonPositiveMajor; return f; }

    f.ratioMinorMajor = min_ / maj;
    f.iotaDeg = std::acos(std::clamp(f.ratioMinorMajor, 0.0, 1.0)) * 180.0 / M_PI;

    // The conditioning floor. Recorded on the ratio BEFORE refusing, so the reject
    // carries its own diagnostic rather than just a name.
    if (f.ratioMinorMajor < kMinAxisRatio) {
        f.reject = ConicReject::IllConditioned;
        return f;
    }

    // The MAJOR axis is the node line — where the swing plane cuts the image
    // plane. Its angle says which way the plane leans, which the axis ratio
    // alone cannot: two planes tilted opposite ways share a ratio. Recorded,
    // not interpreted (brief §9).
    const double nodeRaw = std::atan2(e.vec[i][1], e.vec[i][0]) * 180.0 / M_PI;
    f.nodeDeg = plane_detail::floorMod(nodeRaw + 90.0, 180.0) - 90.0;

    std::vector<double> resid;
    resid.resize(std::size_t(n));
    for (int r = 0; r < n; ++r) {
        double acc = 0.0;
        for (int j = 0; j < 6; ++j) acc += d[std::size_t(j) * n + r] * v[j];
        resid[std::size_t(r)] = std::fabs(acc);
    }
    f.conicResid = plane_detail::medianInPlace(resid);

    f.ok     = true;
    f.reject = ConicReject::None;
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
namespace plane_detail {

// Collect a window's shaft vectors. Bounds INCLUSIVE at BOTH ends, matching
// head_path_plane — which means the sample exactly at top belongs to both
// windows. Preserved deliberately: changing it shifts every sample count.
inline void window(const std::vector<ShaftPlanePoint> &pts, std::int64_t lo, std::int64_t hi,
                   std::vector<double> &x, std::vector<double> &y)
{
    x.clear(); y.clear();
    for (const ShaftPlanePoint &p : pts)
        if (p.tUs >= lo && p.tUs <= hi) { x.push_back(p.vx); y.push_back(p.vy); }
}

// Odd/even-frame refit disagreement — the honest per-swing error bar, and the
// MEASURED channel's alone. Needs ≥ 24 samples so each half still clears the
// 12-sample conic gate. −1 = could not be computed; never 0, which would read
// as perfect agreement.
inline double splitHalfDeg(const std::vector<double> &x, const std::vector<double> &y)
{
    const int n = int(x.size());
    if (n < 24) return -1.0;
    std::vector<double> ax, ay, bx, by;
    for (int i = 0; i < n; ++i) {
        if (i % 2 == 0) { ax.push_back(x[std::size_t(i)]); ay.push_back(y[std::size_t(i)]); }
        else            { bx.push_back(x[std::size_t(i)]); by.push_back(y[std::size_t(i)]); }
    }
    const ConicFit fa = fitConic(ax.data(), ay.data(), int(ax.size()));
    const ConicFit fb = fitConic(bx.data(), by.data(), int(bx.size()));
    if (!fa.ok || !fb.ok) return -1.0;
    return std::fabs(fa.iotaDeg - fb.iotaDeg);
}

inline int countAnchors(const std::vector<PlaneAnchor> &an, std::int64_t lo, std::int64_t hi)
{
    int c = 0;
    for (const PlaneAnchor &a : an) if (a.tUs >= lo && a.tUs <= hi) ++c;
    return c;
}

} // namespace plane_detail

// ─────────────────────────────────────────────────────────────────────────────
// The producer. Fits BOTH channels, tags the emission, and never merges them.
//
// The channel policy lives here rather than in the calling stage on purpose:
// it is pure arithmetic over two fit results, it is the thing the unit test
// most needs to pin, and the corpus cross-check calls this function directly —
// so the rule that ships and the rule that is tested are the same code.
// ─────────────────────────────────────────────────────────────────────────────
inline ShaftPlaneResult fitShaftPlane(const ShaftPlaneInput &in)
{
    ShaftPlaneResult out;
    if (!in.haveWindows) return out;   // every window stays ConicReject::NoWindow

    std::vector<double> x, y;

    // ── measured: the honest channel, and the headline whenever it fits ──
    plane_detail::window(in.measured, in.takeawayUs, in.topUs, x, y);
    out.measured.back.fit          = fitConic(x.data(), y.data(), int(x.size()));
    out.measured.back.splitHalfDeg = plane_detail::splitHalfDeg(x, y);

    plane_detail::window(in.measured, in.topUs, in.impactUs, x, y);
    out.measured.down.fit          = fitConic(x.data(), y.data(), int(x.size()));
    out.measured.down.splitHalfDeg = plane_detail::splitHalfDeg(x, y);

    out.measured.fitted = out.measured.back.fit.ok && out.measured.down.fit.ok;
    if (out.measured.fitted)
        out.measured.deltaDeg = out.measured.back.fit.iotaDeg - out.measured.down.fit.iotaDeg;

    // ── synth: an inference, not an observation. No split-half, ever. ──
    plane_detail::window(in.synth, in.takeawayUs, in.topUs, x, y);
    out.synth.back.fit     = fitConic(x.data(), y.data(), int(x.size()));
    out.synth.back.anchors = plane_detail::countAnchors(in.anchors, in.takeawayUs, in.topUs);

    plane_detail::window(in.synth, in.topUs, in.impactUs, x, y);
    out.synth.down.fit     = fitConic(x.data(), y.data(), int(x.size()));
    out.synth.down.anchors = plane_detail::countAnchors(in.anchors, in.topUs, in.impactUs);

    out.synth.fitted = out.synth.back.fit.ok && out.synth.down.fit.ok;
    if (out.synth.fitted)
        out.synth.deltaDeg = out.synth.back.fit.iotaDeg - out.synth.down.fit.iotaDeg;

    // The weakest anchor over both windows — the synth channel's honest quality,
    // because anchors are all the information it has.
    for (const PlaneAnchor &a : in.anchors) {
        if (a.tUs < in.takeawayUs || a.tUs > in.impactUs) continue;
        if (out.synth.anchorConfMin < 0.f || a.conf < out.synth.anchorConfMin)
            out.synth.anchorConfMin = a.conf;
    }

    // Measured wins whenever both its windows fit; synth is the fallback, and
    // a synth-tagged emission IS the record that the measured channel could not
    // see that swing's downswing.
    if (out.measured.fitted)   out.channel = PlaneChannel::Measured;
    else if (out.synth.fitted) out.channel = PlaneChannel::Synth;
    else                       return out;

    const PlaneChannelFit &sel = (out.channel == PlaneChannel::Measured) ? out.measured : out.synth;
    out.valid       = true;
    out.deltaDeg    = sel.deltaDeg;
    out.iotaBackDeg = sel.back.fit.iotaDeg;
    out.iotaDownDeg = sel.down.fit.iotaDeg;
    return out;
}

} // namespace pinpoint::analysis
