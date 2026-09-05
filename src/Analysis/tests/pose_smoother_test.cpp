// Standalone tests for the Phase-1 motion-overlay pose smoother
// (src/Analysis/pose_smoother — smoothPoseTrack). Pure synthetic: a noisy
// circular wrist arc + deterministic LCG noise; no frames, no OpenCV. Mirrors
// clubhead_temporal_test's style (own main(), check()-macros, no googletest).
//
// Sections 8–9 cover the phase-4.1 LEGS group (keypoints 11–16): that it ships
// dark, that it reaches those six keypoints and no others, that a jerk scale buys
// the window the derivation block's law predicts, and that a real hip excursion
// survives it (metric_presentation_honesty.md §5.4).
//
// Sections 10–19 cover the phase-5 MOTION-ADAPTIVE window (poseSmooth.adapt.*), PROMOTED
// 2026-09-05 (mode "accel" over the legs group, aRef 4000, expo 8, minScale 0.01,
// leadMs 20 — the C15 gate row is in 10a and in pp_tuned_constants.h): that the shipped
// defaults are that winning row, that mode "off" is still inert byte for byte and is
// therefore the parity switch, that the accel policy floors the window while the
// joint is quiet and returns it to today's while the joint accelerates without moving
// the excursion or the onset sample, that it responds in the right direction on a track
// with the real pose cadence (11c — which prints the |a| floor it presents to aRef but
// deliberately does NOT gate engagement; read its fixture note before changing any noise
// in this file), that the innov policy is wired to its knob (and
// what it cannot do — see the note in section 12), that the group selects 11–16 or
// 0–16 and never the tail, that every key round-trips and is range-guarded, that the lead
// is a DURATION and is local to an
// acceleration edge, that a segment break widens the window instead of inheriting the
// address one, and that aRefPxS2's per-frame-width scaling holds.
//
//   cmake --build build/analyzer-tests --target pose_smoother_test
//   ctest --test-dir build/analyzer-tests -R pose_smoother --output-on-failure

#include "../pose_smoother.h"

#include <QVariantMap>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static constexpr int    W = 1920, H = 1080;
static constexpr int    KP = 9;                 // COCO left_wrist — the tracked joint
static constexpr double kPi = 3.14159265358979323846;

// Deterministic uniform [0,1) — a 64-bit LCG (NO std::random). Box-Muller on top
// for a repeatable Gaussian-ish measurement noise.
struct Lcg {
    uint64_t s;
    explicit Lcg(uint64_t seed) : s(seed) {}
    double u01() { s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                   return double((s >> 11) & ((1ULL << 53) - 1)) / double(1ULL << 53); }
    double gauss() { double u1 = u01(); if (u1 < 1e-12) u1 = 1e-12;
                     return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u01()); }
};

// Ground-truth wrist arc in PIXELS at time t (s): a circle swept at ω rad/s.
struct Arc { double cx, cy, R, w, a0; };
static void truthPx(const Arc &a, double t, double &px, double &py)
{
    const double ang = a.a0 + a.w * t;
    px = a.cx + a.R * std::cos(ang);
    py = a.cy + a.R * std::sin(ang);
}

// Build a track that samples the arc at the given absolute times (s). KP carries
// the arc + noise at `conf`; a time inside [gapLo,gapHi) is dropped to gapConf.
// Every other keypoint is left at conf 0 (⇒ Off, ignored by these tests).
static std::vector<PoseFrame2D> buildTrack(const Arc &a, const std::vector<double> &times,
                                           double noisePx, float conf, Lcg &rng,
                                           double gapLo = 1e30, double gapHi = 1e30,
                                           float gapConf = 0.05f)
{
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double t = times[i];
        double px, py; truthPx(a, t, px, py);
        px += noisePx * rng.gauss();
        py += noisePx * rng.gauss();
        f[i].t_us = int64_t(std::llround(t * 1e6));
        f[i].kp[KP]   = QPointF(px / double(W), py / double(H));
        f[i].conf[KP] = (t >= gapLo && t < gapHi) ? gapConf : conf;
    }
    return f;
}

static std::vector<double> uniformTimes(double rate, double T)
{
    std::vector<double> t;
    for (int i = 0; i * (1.0 / rate) < T; ++i) t.push_back(i / rate);
    return t;
}

// ── legs-group (phase 4.1) fixtures ──────────────────────────────────────────
// A wholebody track carrying confident, noisy data on one keypoint of every group
// a per-group scale can select: a body joint ABOVE the hips (5) and the wrist (9)
// for the frozen 0–10 range, all six legs keypoints (11–16), a foot (17) and a
// hand (100) for the tail. Every keypoint the legs scale must not touch therefore
// has real filter state to be perturbed — an all-Off keypoint would make "byte
// identical" vacuous.
struct KpArc { int kp; Arc a; double noise; };
static const KpArc kLegsFixture[] = {
    {   5, {  900.0, 300.0, 120.0, 4.0, 0.2 }, 3.0 },  // shoulder — frozen body
    {   9, {  960.0, 540.0, 400.0, 5.0, 0.3 }, 4.0 },  // wrist — the joint σ_jerk was tuned on
    {  11, {  900.0, 600.0,  30.0, 1.5, 0.0 }, 3.0 },  // hips / knees / ankles: slow, small arcs
    {  12, {  980.0, 600.0,  30.0, 1.5, 3.1 }, 3.0 },
    {  13, {  890.0, 760.0,  20.0, 1.2, 0.5 }, 3.0 },
    {  14, {  990.0, 760.0,  20.0, 1.2, 2.6 }, 3.0 },
    {  15, {  880.0, 940.0,  10.0, 1.0, 1.0 }, 3.0 },
    {  16, { 1000.0, 940.0,  10.0, 1.0, 2.0 }, 3.0 },
    {  17, {  700.0, 990.0,  12.0, 1.0, 1.1 }, 3.0 },  // L bigtoe — feet group
    { 100, { 1100.0, 400.0, 350.0, 5.5, 0.7 }, 3.0 },  // left hand — hand group
};

static std::vector<PoseFrame2D> buildLegsTrack(const std::vector<double> &times, Lcg &rng)
{
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        f[i].t_us = int64_t(std::llround(times[i] * 1e6));
        for (const KpArc &tr : kLegsFixture) {
            double px, py; truthPx(tr.a, times[i], px, py);
            f[i].kp[tr.kp]   = QPointF((px + tr.noise * rng.gauss()) / double(W),
                                       (py + tr.noise * rng.gauss()) / double(H));
            f[i].conf[tr.kp] = 0.8f;
        }
    }
    return f;
}

// ⚠ QPointF::operator== is FUZZY (qFuzzyCompare, ≈1e-12 relative), so it would accept a
// keypoint that moved in the last few bits — which is exactly what a "byte-identical"
// claim has to reject. Every equality below goes through this instead: the two doubles
// compared with ==. (Bitwise, not near: the only NaN in this file would itself be a bug,
// and no fixture produces one.)
static bool exactEq(const QPointF &a, const QPointF &b)
{
    return a.x() == b.x() && a.y() == b.y();
}

// Byte-equality of two smoother outputs over the keypoints `keep` selects.
template <typename Pred>
static bool sameWhere(const PoseSmootherOutput &a, const PoseSmootherOutput &b, Pred keep)
{
    if (a.smoothed.size() != b.smoothed.size() || a.aux.size() != b.aux.size()) return false;
    for (std::size_t i = 0; i < a.smoothed.size(); ++i) {
        if (a.smoothed[i].t_us != b.smoothed[i].t_us) return false;
        for (int k = 0; k < kWholeBodyJoints; ++k) {
            if (!keep(k)) continue;
            if (!(exactEq(a.smoothed[i].kp[k], b.smoothed[i].kp[k])
               && a.smoothed[i].conf[k] == b.smoothed[i].conf[k]
               && a.aux[i].tier[k]      == b.aux[i].tier[k]
               && a.aux[i].sigma[k]     == b.aux[i].sigma[k])) return false;
        }
    }
    return true;
}

// A one-keypoint track: kp `k` at truth (tx(t), ty) px + Gaussian noise, conf 0.8
// (⇒ the filter's σ_meas = 2 + 0.2·6 = 3.2 px). Everything else stays Off.
template <typename Fn>
static std::vector<PoseFrame2D> buildOneKpTrack(int k, const std::vector<double> &times,
                                                Fn truthX, double ty, double noisePx, Lcg &rng)
{
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        f[i].t_us = int64_t(std::llround(times[i] * 1e6));
        f[i].kp[k]   = QPointF((truthX(times[i]) + noisePx * rng.gauss()) / double(W),
                               (ty             + noisePx * rng.gauss()) / double(H));
        f[i].conf[k] = 0.8f;
    }
    return f;
}

// Mean / max Euclidean residual (px) of the KP output vs the arc truth.
struct Resid { double mean, max; int n; };
static Resid residual(const Arc &a, const std::vector<PoseFrame2D> &out)
{
    double sum = 0.0, mx = 0.0; int n = 0;
    for (const auto &fr : out) {
        double px, py; truthPx(a, double(fr.t_us) * 1e-6, px, py);
        const double dx = fr.kp[KP].x() * W - px, dy = fr.kp[KP].y() * H - py;
        const double d = std::hypot(dx, dy);
        sum += d; mx = std::max(mx, d); ++n;
    }
    return { n ? sum / n : 0.0, mx, n };
}
static Resid residualRaw(const Arc &a, const std::vector<PoseFrame2D> &in)
{
    return residual(a, in);   // raw frames carry the same KP-vs-truth geometry
}

// ── phase-5 (motion-adaptive window) fixtures ────────────────────────────────
// The frame size is a PARAMETER here, unlike the sections above: adapt.aRefPxS2 is
// quoted in px/s² at a reference frame WIDTH (1280 px), so a fixture that hard-codes
// 1920 would silently be testing a 1.5× threshold. 1280×1024 is the corpus subset's
// format, i.e. the format the shipped aRef was measured on.
static constexpr double WA = 1280.0, HA = 1024.0;
static constexpr int    KPA = 11;                    // left hip — inside the legs group

template <typename Fn>
static std::vector<PoseFrame2D> buildKpTrackWH(int k, const std::vector<double> &times,
                                               Fn truthX, double ty, double noisePx, Lcg &rng,
                                               double w, double h,
                                               double holeLo = 1e30, double holeHi = 1e30)
{
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double t = times[i];
        const double px = truthX(t) + (noisePx > 0.0 ? noisePx * rng.gauss() : 0.0);
        const double py = ty        + (noisePx > 0.0 ? noisePx * rng.gauss() : 0.0);
        f[i].t_us    = int64_t(std::llround(t * 1e6));
        f[i].kp[k]   = QPointF(px / w, py / h);
        f[i].conf[k] = (t >= holeLo && t < holeHi) ? 0.05f : 0.8f;
    }
    return f;
}

// The adapt track: 1.0 s still, 1.0 s of a 40 px (half peak-to-peak) 4 Hz RAISED
// COSINE, 0.5 s still — 375 frames at 150 fps. Velocity is continuous at both ends
// (the raised cosine starts and ends at rest) and the motion is exactly four whole
// periods, so the amplitude can be read by projection and the boundary is a clean
// ACCELERATION step rather than a velocity step.
//
// ⚠ WHY 4 Hz AND NOT THE 0.5 Hz A P1→P4 SWAY LOOKS LIKE. The accel policy reads the
// smoothed ACCELERATION, and a 40 px 0.5 Hz excursion peaks at 40·(2π·0.5)² = 395
// px/s² — two orders below the corpus's measured hip accelerations (P6 11136, P7 13877
// px/s² at 1280 wide) and far below the smoother's own acceleration-estimate noise on a
// noisy stationary point. No setting of any policy could tell that motion from an
// address hold, so a 0.5 Hz fixture cannot test discrimination at all. 4 Hz puts the
// peak at 40·(2π·4)² = 25265 px/s², i.e. the P7 order, and is still deep inside the
// filter's passband: even pinned at the minScale floor (q ×0.05 ⇒ cutoff ×0.05^(1/6) =
// ×0.607) the predicted amplitude loss is (25.13/55.5)⁶ = 0.9 %, so ONE fixture can
// carry both "the window returns to 1.0 while it accelerates" and "the excursion
// survives" — which is the whole point of the policy.
static constexpr double kAdQuietS = 1.0, kAdMotionS = 1.0, kAdTailS = 0.5;
static constexpr double kAdAmpPx  = 40.0, kAdHz = 4.0;
static constexpr double kAdX0 = 900.0, kAdY = 600.0;
static constexpr int    kAdQuietLo = 40, kAdQuietHi = 120;   // still-interior frames (of 0..149)
static constexpr int    kAdMotionLo = 150, kAdMotionHi = 300;
static double adOmega() { return 2.0 * kPi * kAdHz; }
static double adPeakAccel() { return kAdAmpPx * adOmega() * adOmega(); }   // 25265 px/s²
static double adTruthX(double t)
{
    if (t < kAdQuietS) return kAdX0;
    const double tp = t - kAdQuietS;
    if (tp >= kAdMotionS) return kAdX0;                  // four whole periods ⇒ back at rest
    return kAdX0 + kAdAmpPx * (1.0 - std::cos(adOmega() * tp));
}
static double adTruthAccel(double t)
{
    if (t < kAdQuietS) return 0.0;
    const double tp = t - kAdQuietS;
    if (tp >= kAdMotionS) return 0.0;
    return adPeakAccel() * std::cos(adOmega() * tp);
}

// Excursion amplitude by projection onto the truth's own −cos shape over exactly four
// whole periods (unbiased in noise, which a peak-to-peak read is not).
static double adAmplitude(const std::vector<PoseFrame2D> &fr)
{
    double acc = 0.0; int n = 0;
    for (const auto &f : fr) {
        const double t = double(f.t_us) * 1e-6;
        if (t < kAdQuietS || t >= kAdQuietS + kAdMotionS) continue;
        const double tp = t - kAdQuietS;
        acc += (f.kp[KPA].x() * WA - (kAdX0 + kAdAmpPx)) * (-std::cos(adOmega() * tp));
        ++n;
    }
    return n ? 2.0 * acc / n : 0.0;
}

// Residual sd (px, both axes) of the still stretch — the noise-averaging measure the
// window law predicts.
static double adResidSd(const PoseSmootherOutput &o, int lo, int hi)
{
    double s = 0.0; int n = 0;
    for (int i = lo; i < hi && i < int(o.smoothed.size()); ++i) {
        const double dx = o.smoothed[std::size_t(i)].kp[KPA].x() * WA - kAdX0;
        const double dy = o.smoothed[std::size_t(i)].kp[KPA].y() * HA - kAdY;
        s += dx * dx + dy * dy; n += 2;
    }
    return n ? std::sqrt(s / n) : 0.0;
}

static double medianOf(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ── the REALISTIC still/motion fixture (section 11c) ─────────────────────────
// Two things about real pose data set the floor the accel policy has to clear, and a
// uniform 150 fps track with white noise gets both wrong.
//
// (1) CADENCE. PoseRunner does not pose the address hold densely: the address region
// sits on the coarse/sparse grid (the corpus's measured dt there is ≈27 ms) and the
// dense ≈6.7 ms zone only opens ≈500 ms before impact (pose_runner.h's stride set —
// addressStride 15 / coarseStride 12 for the padded address, the sparse stride for the
// rest). |a| is a second-derivative statistic, so the grid it is read on matters — and
// the lead is `leadMs`, a DURATION, precisely because of this grid: ±3 FRAMES would have
// been ±81 ms out at the address and ±20 ms in the dense zone — widest where the lead is
// least needed. The flip side is that at a 27 ms grid a 20 ms lead reaches NO neighbour,
// so out at the address the max filter is a no-op and the still scale is the raw
// per-frame reading; a densely posed address instead takes the max over several
// independent samples of the noise floor and therefore reads LOUDER.
//
// (2) NOISE COLOUR, which is the bigger lever by far. |a| in a still stretch is driven
// by the HIGH-FREQUENCY part of the keypoint error: for the filter's spectral
// equivalent, sigma_a = 0.447·omega_c²·sigma_p with omega_c = (q/(sigma_m²·dt))^(1/6),
// so error energy near omega_c (≈70–90 rad/s here, i.e. ≈11–14 ms) is what shows up as
// apparent acceleration, and energy well below it is tracked as real motion and shows up
// barely at all. Independent per-frame (WHITE) noise is the worst case: it puts full
// power at omega_c. Real detector error is mostly a slow drift with the pose/appearance,
// so the corpus's measured still-address hip |a| p95 is only 1652 px/s² at 1280 wide
// (docs/validation/data/hip_accel_reference_subset.csv) even though the smoothed track's
// positional residual is over a px. A uniform-150 fps white 3 px fixture reads high enough
// that the policy never engages on it at all (measured: still scale median 1.000).
//
// So this fixture carries the real cadence and 2 px of keypoint error split into two
// colours: 0.5 px white + 1.94 px AR(1) drift with tau = 60 ms (2.0 px total). MEASURED on
// it: still |a| median 3113, p95 7546 px/s² — better than white, still ≈4.6× the corpus's
// 1652, because an AR(1) drift has a 1/omega² tail and so is NOT quiet at omega_c (that
// 1.94 px drift carries ≈9× the 0.5 px white component's power there). A fixture that
// matched the corpus would need a spectrally smoother error model, which nothing in this
// repo measures — so this fixture pins the DIRECTION of the policy and the bake-off on real
// swings decides engagement. Read section 11c before changing any noise here.
static std::vector<double> realCadenceTimes(double coarseUntil, double total,
                                            double coarseDt = 0.027, double denseDt = 1.0 / 150.0)
{
    std::vector<double> t;
    for (int i = 0; i * coarseDt < coarseUntil; ++i) t.push_back(i * coarseDt);
    for (int j = 0; coarseUntil + j * denseDt < total; ++j) t.push_back(coarseUntil + j * denseDt);
    return t;
}

// White + AR(1) drift, the two colours of a real keypoint error, per axis. Started in
// its stationary distribution so the first frames are not special.
struct DriftNoise {
    double sdWhite, sdDrift, tauS;
    double dx = 0.0, dy = 0.0;
    void seed(Lcg &rng) { dx = sdDrift * rng.gauss(); dy = sdDrift * rng.gauss(); }
    void step(double dt, Lcg &rng)
    {
        const double phi = std::exp(-std::max(dt, 0.0) / tauS);
        const double sw  = sdDrift * std::sqrt(std::max(0.0, 1.0 - phi * phi));
        dx = phi * dx + sw * rng.gauss();
        dy = phi * dy + sw * rng.gauss();
    }
};

static std::vector<PoseFrame2D> buildRealisticTrack(const std::vector<double> &times, Lcg &rng)
{
    DriftNoise n{ 0.5, 1.94, 0.060 };
    n.seed(rng);
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        if (i) n.step(times[i] - times[i - 1], rng);
        const double px = adTruthX(times[i]) + n.dx + 0.5 * rng.gauss();
        const double py = kAdY               + n.dy + 0.5 * rng.gauss();
        f[i].t_us      = int64_t(std::llround(times[i] * 1e6));
        f[i].kp[KPA]   = QPointF(px / WA, py / HA);
        f[i].conf[KPA] = 0.8f;
    }
    return f;
}

// The same raised-cosine excursion carried on the OTHER axis (x constant), for the
// anisotropy case in section 17: a format change that is not a similarity transform
// scales the two axes differently, and one isotropic threshold cannot undo both.
static std::vector<PoseFrame2D> buildYMotionTrack(const std::vector<double> &times,
                                                  double w, double h)
{
    std::vector<PoseFrame2D> f(times.size());
    for (std::size_t i = 0; i < times.size(); ++i) {
        f[i].t_us      = int64_t(std::llround(times[i] * 1e6));
        // adTruthX carries the motion; here it drives y, and x is the constant.
        f[i].kp[KPA]   = QPointF(kAdX0 / w, (kAdY - kAdX0 + adTruthX(times[i])) / h);
        f[i].conf[KPA] = 0.8f;
    }
    return f;
}

static double percentileOf(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t idx = std::min(v.size() - 1,
                                     std::size_t(p * double(v.size() - 1) + 0.5));
    return v[idx];
}

// A single ACCELERATION PULSE (noiseless), for the lead-locality test: 0.40 s
// still, 0.15 s at +12000 px/s², 0.25 s at constant velocity, 0.15 s at −12000, then
// still. |a| is therefore two clean plateaus with four transitions, which is what makes
// "the lead filter changes nothing except near an acceleration edge" checkable — a
// sustained oscillation has an edge every few frames and the claim would be empty.
// 12000 px/s² is s = 1.5 at the shipped aRef, so each plateau clamps to 1.0 with margin.
static constexpr double kPulseA  = 12000.0;
static constexpr double kPulseE0 = 0.40, kPulseE1 = 0.55, kPulseE2 = 0.80, kPulseE3 = 0.95;
static double pulseTruthX(double t)
{
    const double t1 = kPulseE1 - kPulseE0, t2 = kPulseE2 - kPulseE1, t3 = kPulseE3 - kPulseE2;
    const double v  = kPulseA * t1;                      // 1800 px/s
    const double d1 = 0.5 * kPulseA * t1 * t1;           // 135 px
    const double d2 = v * t2;                            // 450 px
    const double d3 = v * t3 - 0.5 * kPulseA * t3 * t3;  // 135 px
    if (t <= kPulseE0) return 200.0;
    if (t <= kPulseE1) { const double u = t - kPulseE0; return 200.0 + 0.5 * kPulseA * u * u; }
    if (t <= kPulseE2) return 200.0 + d1 + v * (t - kPulseE1);
    if (t <= kPulseE3) { const double u = t - kPulseE2;
                         return 200.0 + d1 + d2 + v * u - 0.5 * kPulseA * u * u; }
    return 200.0 + d1 + d2 + d3;                         // 920 px, at rest
}

int main()
{
    const Arc arc{ 960.0, 540.0, 400.0, 5.0, 0.3 };   // R=400px, ω=5 rad/s ⇒ ~2000 px/s tip speed

    // ── 1. noisy arc: smoothed residual ≪ raw residual ───────────────────────
    std::printf("=== smoothPoseTrack: noise reduction on a swing arc ===\n");
    {
        Lcg rng(0x1234ABCDu);
        const auto times = uniformTimes(150.0, 0.6);
        const auto in    = buildTrack(arc, times, 4.0, 0.8f, rng);
        const auto res   = smoothPoseTrack(in, W, H);
        check(res.smoothed.size() == in.size() && res.aux.size() == in.size(),
              "one smoothed frame + aux per input frame");
        const Resid raw = residualRaw(arc, in);
        const Resid sm  = residual(arc, res.smoothed);
        std::printf("       raw mean=%.2fpx  smoothed mean=%.2fpx (max %.2fpx)\n",
                    raw.mean, sm.mean, sm.max);
        check(sm.mean < 0.6 * raw.mean, "smoothed mean residual < 0.6 x raw");
        check(sm.mean < 3.0, "smoothed mean residual is small in absolute px");
        int nMeas = 0; for (const auto &ax : res.aux) if (ax.tier[KP] == uint8_t(PoseTier::Meas)) ++nMeas;
        check(nMeas > int(in.size()) * 3 / 4, "a strong confident run is mostly meas-tier");
    }

    // ── 2. confidence dropout is bridged (no spike), tiers Pred, conf ≥ 0.5 ───
    std::printf("=== smoothPoseTrack: dropout bridged without a spike ===\n");
    {
        const Arc slow{ 960.0, 540.0, 400.0, 3.0, 0.3 };   // gentler so the bridge is honest
        Lcg rng(0x55AA1234u);
        const auto times = uniformTimes(150.0, 0.7);
        const double gLo = 0.30, gHi = 0.45;               // 150 ms occlusion mid-arc
        const auto in  = buildTrack(slow, times, 3.0, 0.8f, rng, gLo, gHi);
        const auto res = smoothPoseTrack(in, W, H);

        bool allPred = true, allConf = true; double gapMax = 0.0; int nGap = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double t = double(in[i].t_us) * 1e-6;
            if (t < gLo || t >= gHi) continue;
            ++nGap;
            if (res.aux[i].tier[KP] != uint8_t(PoseTier::Pred)) allPred = false;
            if (res.smoothed[i].conf[KP] < 0.5f) allConf = false;
            double px, py; truthPx(slow, t, px, py);
            gapMax = std::max(gapMax, std::hypot(res.smoothed[i].kp[KP].x() * W - px,
                                                 res.smoothed[i].kp[KP].y() * H - py));
        }
        std::printf("       gap frames=%d  max deviation across gap=%.2fpx\n", nGap, gapMax);
        check(nGap > 15, "the 150 ms gap spans several frames");
        check(allPred, "every bridged gap frame is Pred tier");
        check(allConf, "every bridged gap frame paints at conf >= 0.5");
        check(gapMax < 30.0, "bridge deviation is bounded (no spike across the gap)");
    }

    // ── 3. determinism: identical input ⇒ byte-identical output ───────────────
    std::printf("=== smoothPoseTrack: determinism ===\n");
    {
        Lcg rng(0xDEADBEEFu);
        const auto in = buildTrack(arc, uniformTimes(120.0, 0.5), 4.0, 0.75f, rng);
        const auto a = smoothPoseTrack(in, W, H);
        const auto b = smoothPoseTrack(in, W, H);
        bool same = a.smoothed.size() == b.smoothed.size() && a.aux.size() == b.aux.size();
        for (std::size_t i = 0; same && i < a.smoothed.size(); ++i) {
            for (int k = 0; k < kWholeBodyJoints && same; ++k) {
                same = exactEq(a.smoothed[i].kp[k], b.smoothed[i].kp[k])
                    && a.smoothed[i].conf[k] == b.smoothed[i].conf[k]
                    && a.aux[i].tier[k] == b.aux[i].tier[k]
                    && a.aux[i].sigma[k] == b.aux[i].sigma[k];
            }
            same = same && a.smoothed[i].t_us == b.smoothed[i].t_us;
        }
        check(same, "two runs are exactly equal on every value");
    }

    // ── 4. fps-independence: 150 fps and 30 fps recover the truth similarly ───
    std::printf("=== smoothPoseTrack: fps-independence ===\n");
    {
        const Arc a4{ 960.0, 540.0, 400.0, 4.0, 0.3 };
        Lcg r150(0xABC0001u), r30(0xABC0002u);
        const auto in150 = buildTrack(a4, uniformTimes(150.0, 0.6), 3.0, 0.85f, r150);
        const auto in30  = buildTrack(a4, uniformTimes(30.0,  0.6), 3.0, 0.85f, r30);
        const Resid s150 = residual(a4, smoothPoseTrack(in150, W, H).smoothed);
        const Resid s30  = residual(a4, smoothPoseTrack(in30,  W, H).smoothed);
        std::printf("       150fps mean=%.2fpx (n=%d)   30fps mean=%.2fpx (n=%d)\n",
                    s150.mean, s150.n, s30.mean, s30.n);
        check(s150.mean < 4.0 && s30.mean < 4.5, "both frame rates recover the arc to a few px");
        const double hi = std::max(s150.mean, s30.mean), lo = std::min(s150.mean, s30.mean);
        check(hi < 2.5 * std::max(lo, 1e-6), "residuals are within a similar band (fps-independent)");
    }

    // ── 5. non-uniform sampling: mixed dt doesn't break segmentation ──────────
    std::printf("=== smoothPoseTrack: non-uniform sampling ===\n");
    {
        // Coarse address (~90 ms), then a dense ~150 fps burst through 'impact',
        // then a ~40 fps sparse follow-through — the real PoseRunner cadence.
        std::vector<double> times; double t = 0.0;
        for (int i = 0; i < 4;  ++i) { times.push_back(t); t += 0.090; }   // coarse
        for (int i = 0; i < 40; ++i) { times.push_back(t); t += 1.0/150; } // dense
        for (int i = 0; i < 12; ++i) { times.push_back(t); t += 1.0/40;  } // sparse
        Lcg rng(0x0FF1CEu);
        const auto in  = buildTrack(arc, times, 3.5, 0.8f, rng);
        const auto res = smoothPoseTrack(in, W, H);

        int nOff = 0; double jump = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i)
            if (res.aux[i].tier[KP] == uint8_t(PoseTier::Off)) ++nOff;
        for (std::size_t i = 1; i < res.smoothed.size(); ++i)   // px jump between neighbours
            jump = std::max(jump, std::hypot(
                (res.smoothed[i].kp[KP].x() - res.smoothed[i-1].kp[KP].x()) * W,
                (res.smoothed[i].kp[KP].y() - res.smoothed[i-1].kp[KP].y()) * H));
        const Resid sm = residual(arc, res.smoothed);
        std::printf("       nOff=%d  max neighbour jump=%.1fpx  mean residual=%.2fpx\n",
                    nOff, jump, sm.mean);
        check(nOff == 0, "one confident segment spans the whole track (no spurious Off)");
        check(sm.mean < 5.0, "mixed-dt track still recovers the arc");
        check(jump < 260.0, "no discontinuity at a stride boundary (jump ~ true motion)");
    }

    // ── 6. tier honesty: a never-confident keypoint stays Off / passthrough ───
    std::printf("=== smoothPoseTrack: tier honesty (Off passthrough) ===\n");
    {
        std::vector<PoseFrame2D> in(30);
        for (int i = 0; i < 30; ++i) {
            in[i].t_us = int64_t(i) * 6667;                 // ~150 fps
            in[i].kp[KP]   = QPointF(0.2 + 0.01 * i, 0.6 - 0.005 * i);
            in[i].conf[KP] = 0.2f;                          // always below confMeasMin (0.35)
        }
        const auto res = smoothPoseTrack(in, W, H);
        bool off = true, passthrough = true, zeroConf = true, zeroSig = true;
        for (int i = 0; i < 30; ++i) {
            off         &= res.aux[i].tier[KP]   == uint8_t(PoseTier::Off);
            passthrough &= exactEq(res.smoothed[i].kp[KP], in[i].kp[KP]);
            zeroConf    &= res.smoothed[i].conf[KP]  == 0.0f;
            zeroSig     &= res.aux[i].sigma[KP]      == 0.0f;
        }
        check(off, "sub-threshold keypoint never leaves Off tier");
        check(passthrough, "Off output is the raw input kp, byte-identical");
        check(zeroConf, "Off output conf is 0 (paint-alpha contract)");
        check(zeroSig, "Off output sigma is 0 (no smoothed value)");
    }

    // ── 7. wholebody widen: body parity + smoothed tail (WB0) ─────────────────
    // (a) body keypoint output must be unaffected by confident wholebody-tail
    // keypoints in the same track (per-keypoint filters are independent, and
    // body always runs the frozen base constants) — the in-process analogue of
    // "identical to a 17-wide run". (b) feet/hand keypoints are first-class:
    // a confident tail arc gets Meas-tier smoothing like any body joint.
    std::printf("=== smoothPoseTrack: wholebody body-parity + tail smoothing ===\n");
    {
        constexpr int KPFOOT = 17;    // L bigtoe — first wholebody tail index
        constexpr int KPHAND = 100;   // inside the left-hand range (91–111)

        Lcg rngA(0x77AA55EEu);
        const auto times  = uniformTimes(150.0, 0.6);
        const auto bodyIn = buildTrack(arc, times, 4.0, 0.8f, rngA);   // KP=9 only

        // Same body samples + confident foot/hand arcs on the tail indices.
        const Arc footArc{ 700.0, 900.0, 150.0, 4.0, 1.1 };
        const Arc handArc{ 1100.0, 400.0, 350.0, 5.5, 0.7 };
        auto wholeIn = bodyIn;
        Lcg rngB(0x11223344u);
        for (std::size_t i = 0; i < wholeIn.size(); ++i) {
            const double t = double(wholeIn[i].t_us) * 1e-6;
            double px, py;
            truthPx(footArc, t, px, py);
            wholeIn[i].kp[KPFOOT]   = QPointF((px + 3.0 * rngB.gauss()) / double(W),
                                              (py + 3.0 * rngB.gauss()) / double(H));
            wholeIn[i].conf[KPFOOT] = 0.8f;
            truthPx(handArc, t, px, py);
            wholeIn[i].kp[KPHAND]   = QPointF((px + 3.0 * rngB.gauss()) / double(W),
                                              (py + 3.0 * rngB.gauss()) / double(H));
            wholeIn[i].conf[KPHAND] = 0.8f;
        }

        const auto resBody  = smoothPoseTrack(bodyIn,  W, H);
        const auto resWhole = smoothPoseTrack(wholeIn, W, H);

        bool bodySame = resBody.smoothed.size() == resWhole.smoothed.size();
        for (std::size_t i = 0; bodySame && i < resBody.smoothed.size(); ++i)
            bodySame = exactEq(resBody.smoothed[i].kp[KP], resWhole.smoothed[i].kp[KP])
                    && resBody.smoothed[i].conf[KP] == resWhole.smoothed[i].conf[KP]
                    && resBody.aux[i].tier[KP]      == resWhole.aux[i].tier[KP]
                    && resBody.aux[i].sigma[KP]     == resWhole.aux[i].sigma[KP];
        check(bodySame, "body keypoint output identical with/without a wholebody tail");

        int nFootMeas = 0, nHandMeas = 0;
        for (const auto &ax : resWhole.aux) {
            if (ax.tier[KPFOOT] == uint8_t(PoseTier::Meas)) ++nFootMeas;
            if (ax.tier[KPHAND] == uint8_t(PoseTier::Meas)) ++nHandMeas;
        }
        check(nFootMeas > int(wholeIn.size()) * 3 / 4, "confident foot keypoint is mostly meas-tier");
        check(nHandMeas > int(wholeIn.size()) * 3 / 4, "confident hand keypoint is mostly meas-tier");

        // Per-group scales are additive: a non-1.0 hand scale must change the
        // hand posterior yet leave body output untouched (frozen constants).
        PoseSmootherConfig scaled;
        scaled.handSigmaScale = 3.0;
        scaled.handJerkScale  = 0.5;
        const auto resScaled = smoothPoseTrack(wholeIn, W, H, scaled);
        bool bodyFrozen = true, handMoved = false;
        for (std::size_t i = 0; i < resWhole.smoothed.size(); ++i) {
            bodyFrozen = bodyFrozen
                      && exactEq(resWhole.smoothed[i].kp[KP], resScaled.smoothed[i].kp[KP])
                      && resWhole.aux[i].sigma[KP]    == resScaled.aux[i].sigma[KP];
            handMoved  = handMoved
                      || resWhole.aux[i].sigma[KPHAND] != resScaled.aux[i].sigma[KPHAND];
        }
        check(bodyFrozen, "hand-group scales never touch a body keypoint");
        check(handMoved, "hand-group scales change the hand posterior");
    }

    // ── 8. legs group 11–16 (phase 4.1, metric_presentation_honesty §5.4) ─────
    // A fourth per-group scale, over the COCO BODY keypoints 11–16 (hips, knees,
    // ankles), because σ_jerk was tuned on a wrist and a hip does nothing at that
    // 33 ms window. It ships DARK (1.0/1.0) and these cases pin all four halves of
    // that claim: the default is dark, ×1.0 is byte-identical, the scale reaches
    // 11–16 and NOTHING else, and the dark keys arrive from an override map.
    // NB every run in this section shares whatever the shipped phase-5 window does (it is
    // ON for kp 11–16 since 2026-09-05), so these equalities stay exact — they compare the
    // STATIC scale against itself. Section 9 switches the window off explicitly, because a
    // law measurement cannot have two multipliers on q at once.
    std::printf("=== smoothPoseTrack: legs group (11-16) scales ===\n");
    {
        // (a) the shipped default is the dark one, and an explicit 1.0/1.0 run is
        // byte-identical to it on every keypoint of every frame. If 4.3 ever moves
        // a default, THIS is the check that must be updated deliberately.
        PoseSmootherConfig def;
        check(def.legsSigmaScale == 1.0 && def.legsJerkScale == 1.0,
              "shipped legs scales are 1.0 (DARK — 4.3 flips them only behind a corpus gate)");

        Lcg rng(0x1E65CA1Eu);
        const auto times = uniformTimes(150.0, 1.0);
        const auto in    = buildLegsTrack(times, rng);
        const auto base  = smoothPoseTrack(in, W, H);
        PoseSmootherConfig ones; ones.legsSigmaScale = 1.0; ones.legsJerkScale = 1.0;
        check(sameWhere(base, smoothPoseTrack(in, W, H, ones), [](int) { return true; }),
              "legs scales at 1.0 are byte-identical on every kp/conf/tier/sigma of every frame");

        // The fixture has to actually exercise the keypoints it claims to protect.
        int nSmoothed = 0;
        for (const KpArc &tr : kLegsFixture)
            if (base.aux[base.aux.size() / 2].sigma[tr.kp] > 0.0f) ++nSmoothed;
        check(nSmoothed == int(std::size(kLegsFixture)),
              "every fixture keypoint (body, legs, foot, hand) really is smoothed");

        // (c) a non-1.0 legs scale moves 11–16 and leaves 0–10 and 17+ untouched.
        PoseSmootherConfig legs; legs.legsSigmaScale = 2.0; legs.legsJerkScale = 0.1;
        const auto scaled = smoothPoseTrack(in, W, H, legs);
        check(sameWhere(base, scaled, [](int k) { return !isLegKeypoint(k); }),
              "legs scales leave keypoints 0-10 and 17+ byte-identical");
        bool everyLegMoved = true;
        for (int k = kLegFirstKp; k <= kLegLastKp; ++k) {
            bool moved = false;
            for (std::size_t i = 0; i < base.aux.size() && !moved; ++i)
                moved = base.aux[i].sigma[k] != scaled.aux[i].sigma[k]
                     || !exactEq(base.smoothed[i].kp[k], scaled.smoothed[i].kp[k]);
            everyLegMoved = everyLegMoved && moved;
        }
        check(everyLegMoved, "every legs keypoint 11-16 responds to the scale");

        // (d) the dark keys round-trip through a QVariantMap into the config, and
        // an empty map is the frozen default.
        QVariantMap ov;
        ov[QStringLiteral("poseSmooth.legsSigmaScale")] = 2.0;
        ov[QStringLiteral("poseSmooth.legsJerkScale")]  = 0.1;
        const PoseSmootherConfig fromOv = PoseSmootherConfig::fromOverrides(ov);
        check(fromOv.legsSigmaScale == 2.0 && fromOv.legsJerkScale == 0.1,
              "poseSmooth.legs*Scale round-trip through a QVariantMap override");
        const PoseSmootherConfig noOv = PoseSmootherConfig::fromOverrides(QVariantMap{});
        check(noOv.legsSigmaScale == def.legsSigmaScale && noOv.legsJerkScale == def.legsJerkScale,
              "an empty override map leaves the frozen defaults");
        check(sameWhere(scaled, smoothPoseTrack(in, W, H, fromOv), [](int) { return true; }),
              "the override-built config produces the same output as the hand-built one");
    }

    // ── 9. legs window: noise averaging vs a real hip excursion ───────────────
    // The two halves of the 4.2 sweep's acceptance test, on synthetic hips.
    //
    // Effective-window arithmetic (the .cpp derivation block): the smoother's
    // cutoff is ω_c = (σ_jerk²/(σ_m²·dt))^(1/6), so the window T = 1/ω_c scales as
    // σ_jerk^(−1/3) and, on a STATIONARY point where the residual is pure noise
    // averaging (σ_out = σ_in/√(T/dt)), the residual σ scales as σ_jerk^(+1/6).
    // A jerk scale of 0.1 therefore predicts window ×0.1^(−1/3) = ×2.15 (33 ms →
    // ≈71 ms at 150 fps) and residual σ ×0.1^(1/6) = ×0.681. The band below is
    // that prediction ±25 % — the law is asymptotic and the track is finite.
    std::printf("=== smoothPoseTrack: legs window (stationary noise vs 0.5 Hz motion) ===\n");
    {
        constexpr int KPHIP = 11;                        // left hip
        const double predicted = std::pow(0.1, 1.0 / 6.0);   // 0.681
        // ⚠ BOTH configs switch the phase-5 window OFF. kp 11 is in the legs group, and
        // since 2026-09-05 the shipped default runs the per-frame adaptive window over it —
        // which is a SECOND multiplier on q and would make this section measure the two
        // features convolved. This section is the phase-4 STATIC scale's law; section 11(b)
        // is the phase-5 one.
        PoseSmootherConfig fast;                             // the base run, window off
        fast.adapt.mode = AdaptMode::Off;
        PoseSmootherConfig slow = fast;
        slow.legsJerkScale = 0.1;

        // (b1) STATIONARY hip, deterministic pseudo-noise sd 3 px at 150 fps.
        {
            const auto times = uniformTimes(150.0, 2.0);   // 300 frames
            Lcg r1(0x5747104Eu), r2(0x5747104Eu);          // same seed ⇒ same track
            const auto in    = buildOneKpTrack(KPHIP, times, [](double) { return 900.0; },
                                               600.0, 3.0, r1);
            const auto inChk = buildOneKpTrack(KPHIP, times, [](double) { return 900.0; },
                                               600.0, 3.0, r2);
            check(in.size() == inChk.size() && exactEq(in[7].kp[KPHIP], inChk[7].kp[KPHIP]),
                  "the stationary-hip fixture is deterministic");

            auto residSd = [&](const PoseSmootherOutput &o) {
                double s = 0.0; int n = 0;
                // Skip 40 frames (≈267 ms) at each end: the loose init priors make
                // the first/last few frames of an RTS pass genuinely less certain,
                // and at scale 0.1 the window itself is ≈11 frames wide.
                for (std::size_t i = 40; i + 40 < o.smoothed.size(); ++i) {
                    const double dx = o.smoothed[i].kp[KPHIP].x() * W - 900.0;
                    const double dy = o.smoothed[i].kp[KPHIP].y() * H - 600.0;
                    s += dx * dx + dy * dy; n += 2;
                }
                return n ? std::sqrt(s / n) : 0.0;
            };
            const double sdBase = residSd(smoothPoseTrack(in, W, H, fast));
            const double sdSlow = residSd(smoothPoseTrack(in, W, H, slow));
            const double ratio  = sdBase > 0.0 ? sdSlow / sdBase : 1.0;
            // ⚠ The band has to be tight enough to tell the two EXPONENT FAMILIES apart.
            // legsJerkScale multiplies sigma_jerk, so the residual law is s^(1/6) = 0.681;
            // the same number read as a q scale (phase 5's multiplier) would be s^(1/12) =
            // 0.825. A +-25% band admits both, which would let a future refactor move the
            // scale onto q without a single test noticing. +-10% admits only one, and the
            // second check states the discrimination directly.
            const double wrongFamily = std::pow(0.1, 1.0 / 12.0);   // 0.825, the q reading
            std::printf("       stationary hip: sd base=%.3fpx  scale0.1=%.3fpx  ratio=%.3f"
                        " (sigma_jerk law %.3f, q law %.3f; window %.0fms -> %.0fms)\n",
                        sdBase, sdSlow, ratio, predicted, wrongFamily,
                        legWindowMsForJerkScale(1.0), legWindowMsForJerkScale(0.1));
            check(ratio < 1.0, "legsJerkScale 0.1 reduces the stationary-hip residual sd");
            check(ratio > 0.90 * predicted && ratio < 1.10 * predicted,
                  "the reduction matches the sigma_jerk-family law s^(1/6) within 10%");
            check(std::abs(ratio - predicted) < std::abs(ratio - wrongFamily),
                  "and is nearer the sigma_jerk law than the q-scale law (right family)");
        }

        // (b2) a REAL hip excursion must survive it: 0.5 Hz, 40 px amplitude — the
        // order of a P1→P4 sway. Amplitude by projection onto the truth sinusoid
        // over exactly two periods (600 frames at 150 fps), which is unbiased in
        // the presence of the same 3 px noise that a peak-to-peak read is not.
        {
            const double f0 = 0.5, amp = 40.0, w0 = 2.0 * kPi * f0;
            const auto times = uniformTimes(150.0, 4.0);   // 600 frames == 2 periods exactly
            Lcg rng(0x51E7A1DEu);
            const auto in = buildOneKpTrack(KPHIP, times,
                                            [&](double t) { return 900.0 + amp * std::sin(w0 * t); },
                                            600.0, 3.0, rng);
            auto amplitude = [&](const std::vector<PoseFrame2D> &fr) {
                double acc = 0.0; int n = 0;
                for (const auto &f : fr) {
                    const double t = double(f.t_us) * 1e-6;
                    acc += (f.kp[KPHIP].x() * W - 900.0) * std::sin(w0 * t); ++n;
                }
                return n ? 2.0 * acc / n : 0.0;
            };
            const double aRaw  = amplitude(in);
            const double aBase = amplitude(smoothPoseTrack(in, W, H, fast).smoothed);
            const double aSlow = amplitude(smoothPoseTrack(in, W, H, slow).smoothed);
            std::printf("       0.5Hz 40px hip: raw=%.2fpx  base=%.2fpx  scale0.1=%.2fpx\n",
                        aRaw, aBase, aSlow);
            check(std::abs(aRaw / amp - 1.0) < 0.05, "the fixture really carries a 40 px amplitude");
            check(std::abs(aSlow / amp - 1.0) < 0.05,
                  "legsJerkScale 0.1 keeps a 0.5 Hz 40 px hip excursion within 5%");
            check(std::abs(aSlow / std::max(aBase, 1e-9) - 1.0) < 0.05,
                  "and within 5% of what the shipped window returns");
        }

        // The sweep helper the 4.2 scale search reads: 80–100 ms is the design's
        // hip target, and it is 0.05 that lands there — not 0.1.
        const double w05 = legWindowMsForJerkScale(0.05), w10 = legWindowMsForJerkScale(0.1);
        std::printf("       legWindowMsForJerkScale: 0.05 -> %.0fms   0.1 -> %.0fms\n", w05, w10);
        check(w05 > 80.0 && w05 < 100.0, "jerk scale 0.05 predicts the design's 80-100 ms window");
        check(w10 > 60.0 && w10 < 80.0,  "jerk scale 0.1 predicts ~71 ms (short of the target)");
        check(legWindowMsForJerkScale(1.0) == 33.0, "scale 1.0 is the measured 33 ms baseline");
    }

    // ── 10a. the shipped defaults ARE the swept winner (promoted 2026-09-05) ───
    // Phase 5 was promoted on the C15 gate — 17 settings × 11 swings against a control,
    // 6 passing every criterion, and this row winning on margin: ΔP4 0.12/0.34 σ,
    // ΔP7 0.10/0.38 σ (hipLineTilt P7 0.00), excursion 0.997–1.004, σ ratio 0.89–0.99,
    // still-address jitter ×0.71/0.71/0.52 (35.3 % gain), 0 lost samples, 0 fallbacks.
    // If a future change moves any of these numbers, THIS is the check that must be
    // updated deliberately, with a gate row beside it.
    std::printf("=== smoothPoseTrack: the shipped adapt defaults are the swept winner ===\n");
    {
        PoseSmootherConfig def;
        check(def.adapt.mode == AdaptMode::Accel, "shipped adapt.mode is accel (PROMOTED)");
        check(def.adapt.group == AdaptGroup::Legs, "shipped adapt.group is legs (kp 11-16)");
        check(def.adapt.minScale == 0.01 && def.adapt.aRefPxS2 == 4000.0
                  && def.adapt.expo == 8.0 && def.adapt.leadMs == 20.0
                  && def.adapt.innovRef == 4.0 && def.adapt.innovRun == 3,
              "shipped adapt numbers are the winning row (0.01 / 4000 / 8 / 20ms / 4 / 3)");
        check(!def.adapt.emitScalesForTest, "the C14 test hook still ships off");
        // The group is what the gate was run on: the six legs keypoints and nothing else.
        check(def.adapt.appliesTo(11) && def.adapt.appliesTo(16),
              "the shipped default reaches the legs group it was gated on");
        check(!def.adapt.appliesTo(10) && !def.adapt.appliesTo(0) && !def.adapt.appliesTo(17),
              "and reaches neither the upper body nor the wholebody tail");
        // expo 8 is a near-switch, and that is why the gate row moves P4/P7 so little: the
        // floor is reached only below ≈0.66·aRef and 1.0 is reached at aRef itself.
        const double aRef = def.adapt.aRefPxS2;
        check(std::pow(0.55, def.adapt.expo) < def.adapt.minScale
                  && std::pow(1.00, def.adapt.expo) >= 1.0,
              "expo 8 floors below ~0.56x aRef and clamps at aRef (a near-switch, not a ramp)");
        check(std::pow(2172.0 / aRef, def.adapt.expo) < def.adapt.minScale,
              "the corpus still-address p95 (2172) lands on the floor");
        check(std::pow(5232.0 / aRef, def.adapt.expo) >= 1.0,
              "and the corpus p05 of min(|a| P6, P7) (5232) is clamped to today's window");
    }

    // ── 10b. mode "off" is still exact — the parity switch ─────────────────────
    // The promise is BY CONSTRUCTION, not by an arithmetic identity: with mode off no
    // scale vector is built anywhere and predict() is never handed anything but its 1.0
    // default. The reference config is HAND-BUILT (mode = Off) because the shipped
    // default is no longer off, and this is the switch a parity run against phase 5 uses.
    std::printf("=== smoothPoseTrack: adapt mode off is inert (the parity switch) ===\n");
    {
        Lcg rng(0x5CA1E5EDu);
        const auto times = uniformTimes(150.0, 1.0);
        const auto in    = buildLegsTrack(times, rng);
        PoseSmootherConfig offRef;                  // the pre-phase-5 tree, by construction
        offRef.adapt.mode = AdaptMode::Off;
        const auto base = smoothPoseTrack(in, W, H, offRef);
        check(base.adaptScale.empty() && base.adaptAccel.empty(),
              "mode off builds no scale vector and no |a| vector");
        check(base.adaptFallbacks == 0, "mode off can have nothing to fall back (count 0)");
        // And it really is a different output from the shipped default — otherwise this
        // whole section would be vacuous and the promotion would have moved nothing.
        check(!sameWhere(base, smoothPoseTrack(in, W, H), [](int k) { return isLegKeypoint(k); }),
              "the shipped default really does move the legs keypoints (off is not the default)");
        check(sameWhere(base, smoothPoseTrack(in, W, H), [](int k) { return !isLegKeypoint(k); }),
              "and moves nothing outside the group");

        for (const AdaptGroup g : { AdaptGroup::Legs, AdaptGroup::Body }) {
            PoseSmootherConfig off;                 // every adapt field wild, mode still off
            off.adapt.mode = AdaptMode::Off;
            off.adapt.group = g;
            off.adapt.minScale = 0.5;  off.adapt.aRefPxS2 = 1.0;  off.adapt.expo = 2.0;
            off.adapt.leadMs = 90.0;   off.adapt.innovRef = 0.1;  off.adapt.innovRun = 1;
            off.adapt.emitScalesForTest = true;
            const auto res = smoothPoseTrack(in, W, H, off);
            check(sameWhere(base, res, [](int) { return true; }),
                  g == AdaptGroup::Legs
                      ? "mode off + wild adapt fields (legs): byte-identical on every kp/conf/tier/sigma"
                      : "mode off + wild adapt fields (body): byte-identical on every kp/conf/tier/sigma");
            check(res.adaptScale.empty() && res.adaptAccel.empty() && res.adaptFallbacks == 0,
                  "mode off emits no scale/|a| vector even with the hook on, and no fallbacks");
        }
    }

    // ── 11. accel policy: the window floors when quiet, returns when it moves ──
    // (a) on a NOISELESS track, where the policy's input is the truth. This is the
    // discrimination test and it runs on the SHIPPED (promoted) defaults — aRef 4000 at the
    // 1280×1024 reference, expo 8, minScale 0.01, leadMs 20: quiet ⇒ the clamp floor,
    // accelerating ⇒ 1.0. At expo 8 that transition is a near-switch, which is the property
    // the gate row's tiny ΔP4/ΔP7 rests on.
    std::printf("=== smoothPoseTrack: adapt accel, noiseless discrimination ===\n");
    {
        Lcg noRng(1);
        const auto times = uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS);
        const auto in    = buildKpTrackWH(KPA, times, adTruthX, kAdY, 0.0, noRng, WA, HA);
        PoseSmootherConfig cfg;
        cfg.adapt.mode = AdaptMode::Accel;
        cfg.adapt.emitScalesForTest = true;
        const auto res = smoothPoseTrack(in, int(WA), int(HA), cfg);
        PoseSmootherConfig offCfg;                  // the control is HAND-BUILT: the shipped
        offCfg.adapt.mode = AdaptMode::Off;         // default is the adaptive window now
        const auto off = smoothPoseTrack(in, int(WA), int(HA), offCfg);

        check(res.adaptScale.size() == std::size_t(kWholeBodyJoints)
                  && res.adaptScale[KPA].size() == in.size(),
              "the hook emits one scale per frame for the adapting keypoint");
        check(res.adaptScale[9].empty() && res.adaptScale[17].empty(),
              "a keypoint outside the group keeps an EMPTY scale row");
        const std::vector<double> &s = res.adaptScale[KPA];
        check(s[0] == 1.0, "a segment's init frame never predicts, so it carries 1.0");

        bool quietFloor = true, tailFloor = true;
        for (int i = kAdQuietLo; i < kAdQuietHi; ++i)
            quietFloor = quietFloor && s[std::size_t(i)] == cfg.adapt.minScale;
        for (int i = 340; i < 370; ++i)
            tailFloor = tailFloor && s[std::size_t(i)] == cfg.adapt.minScale;
        check(quietFloor, "the still stretch sits exactly on minScale (the long window)");
        check(tailFloor, "and returns to it after the motion");

        // Every high-acceleration frame is back at today's window. aRefEff = 4000 at the
        // 1280×1024 reference and the peak is 25265, so the clamp is reached from
        // |cos| ≥ 0.16; the assertion uses 0.4 to leave the estimator room at the crossings.
        bool motionOne = true; int nHigh = 0;
        for (int i = kAdMotionLo; i < kAdMotionHi; ++i) {
            const double t = double(in[std::size_t(i)].t_us) * 1e-6;
            if (std::abs(adTruthAccel(t)) < 0.4 * adPeakAccel()) continue;
            motionOne = motionOne && s[std::size_t(i)] == 1.0;
            ++nHigh;
        }
        check(nHigh > 60, "the fixture has plenty of high-acceleration frames");
        check(motionOne, "every high-acceleration frame runs at 1.0 (today's window at impact)");

        // The lead is symmetric, so the window is already short BEFORE the step. 20 ms is
        // 3 frames on this uniform 150 fps fixture (a DURATION — see leadMs).
        check(s[std::size_t(kAdMotionLo - 3)] > cfg.adapt.minScale,
              "the scale is already above the floor 20 ms (3 frames) before the acceleration");

        // F9's first-class |a| hook: one value per frame, the policy's actual input, and
        // NOTHING dropped — the init frame carries a real |a| too (it is inside the
        // segment's RTS), it is only its SCALE that is 1.0 because it never predicted.
        check(res.adaptAccel.size() == std::size_t(kWholeBodyJoints)
                  && res.adaptAccel[KPA].size() == in.size(),
              "the |a| hook emits one value per frame for the adapting keypoint");
        check(res.adaptAccel[9].empty(), "and an empty row for a keypoint outside the group");
        int nAccelPos = 0;
        for (const double v : res.adaptAccel[KPA]) if (v > 0.0) ++nAccelPos;
        check(nAccelPos > 100, "the motion frames carry a real |a| (no sentinel, no dropping)");
        // On a NOISELESS still stretch the RTS acceleration is ~0 — the posterior equals the
        // prediction there, so the backward pass has nothing to correct except what bleeds
        // back from the motion (a contraction per step, so it is denormal by here). Not
        // asserted as exactly 0.0 for that reason. It is also why 0 cannot double as "no
        // value": the vector is one entry per frame, and only a NON-ADAPTING keypoint's row
        // is empty.
        check(res.adaptAccel[KPA][80] < 1.0, "a noiseless still frame reads |a| ~ 0, not a sentinel");
        check(res.adaptAccel[KPA][std::size_t(kAdMotionLo + 4)] > 8000.0,
              "|a| through the motion is the P6/P7 order the policy is thresholded against");

        // And the onset sample must not have moved: this is the P7 failure of phase 4.2
        // stated as a test — the sample the corridors read must survive the policy.
        const double dOn = std::abs(res.smoothed[kAdMotionLo].kp[KPA].x()
                                    - off.smoothed[kAdMotionLo].kp[KPA].x()) * WA;
        const double sigOn = double(res.aux[kAdMotionLo].sigma[KPA]);
        std::printf("       onset frame: |delta| vs mode off = %.3fpx, reported sigma = %.3fpx\n",
                    dOn, sigOn);
        check(sigOn > 0.0 && dOn < sigOn,
              "the motion-onset sample moves less than its own reported sigma vs mode off");
    }

    // (b) on a NOISY track: the window law, the excursion, and an honest note about
    // what the policy can see in noise.
    std::printf("=== smoothPoseTrack: adapt accel, window law and excursion (noisy) ===\n");
    {
        Lcg rng(0xA5A5F00Du);
        const auto times = uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS);
        const auto in    = buildKpTrackWH(KPA, times, adTruthX, kAdY, 3.0, rng, WA, HA);

        // ⚠ EVERY RUN IN THIS SECTION WIDENS THE GATE TO 6σ, and that is load-bearing.
        // The law is about NOISE AVERAGING; the 3σ gate is tested in sections 1–8 and the
        // divergence guard in section 19. At minScale 0.05 the reduced q shrinks Pp, which
        // shrinks S, which tightens the 3σ radius by ≈10 % — enough that on a 3 px white
        // track one borderline sample of the ~750 flips its accept flag, the divergence
        // guard (correctly, by its own rule) rejects the whole keypoint, and the "adaptive"
        // run comes back as the control: sd ratio exactly 1.000, which is what this section
        // measured before the gate was widened. At 6σ nothing is rejected in either pass on
        // 3 px noise (the largest |innov| over 750 samples is ≈3.5σ_meas), so accepted[] is
        // all-ones in both and the guard cannot fire. The fallback counts are asserted 0
        // below so this can never silently become a test of the guard again.
        PoseSmootherConfig lawBase;
        lawBase.gateSig = 6.0;
        lawBase.adapt.mode = AdaptMode::Off;   // the control is the pre-phase-5 filter
        // The law fixture PINS its own minScale rather than inheriting the shipped one, so
        // the arithmetic below (and the 2 % excursion claim, which is derived for it) does
        // not move when a future sweep moves the default.
        const double lawMinScale = 0.05;
        const auto off = smoothPoseTrack(in, int(WA), int(HA), lawBase);   // the control

        // (i) THE WINDOW LAW, and its exponents. A q scale s is a sigma_jerk scale of
        // sqrt(s), so the phase-4 law (window ∝ sigma_jerk^(−1/3), stationary residual
        // sigma ∝ sigma_jerk^(+1/6)) reads window ∝ s^(−1/6) and residual sigma ∝
        // s^(+1/12) in q terms: minScale 0.05 ⇒ window x1.648 (33 → 54 ms at 150 fps)
        // and residual sigma x0.05^(1/12) = x0.779. Measured with an UNREACHABLE aRef so
        // every step is pinned at the floor — this checks the LAW, not the policy's
        // ability to see this fixture's noise (which (iii) reports instead).
        PoseSmootherConfig flrCfg = lawBase;
        flrCfg.adapt.mode = AdaptMode::Accel;
        flrCfg.adapt.aRefPxS2 = 1e9;
        flrCfg.adapt.minScale = lawMinScale;
        flrCfg.adapt.emitScalesForTest = true;
        const auto flr = smoothPoseTrack(in, int(WA), int(HA), flrCfg);
        bool allFloor = true;
        for (std::size_t i = 1; i < flr.adaptScale[KPA].size(); ++i)
            allFloor = allFloor && flr.adaptScale[KPA][i] == flrCfg.adapt.minScale;
        std::printf("       adaptFallbacks: floored run=%d\n", flr.adaptFallbacks);
        check(flr.adaptFallbacks == 0, "the widened gate keeps the law fixture non-divergent");
        check(allFloor, "an unreachable aRef pins every predicted step at minScale");

        const double predicted   = std::pow(lawMinScale, 1.0 / 12.0);   // 0.779 — the q family
        const double wrongFamily = std::pow(lawMinScale, 1.0 /  6.0);   // 0.607 — sigma_jerk
        const double sdOff = adResidSd(off, kAdQuietLo, kAdQuietHi);
        const double sdFlr = adResidSd(flr, kAdQuietLo, kAdQuietHi);
        const double ratio = (sdOff > 0.0) ? sdFlr / sdOff : 1.0;
        std::printf("       still-stretch sd: off=%.3fpx  minScale=%.3fpx  ratio=%.3f"
                    " (q law %.3f, sigma_jerk law %.3f)\n",
                    sdOff, sdFlr, ratio, predicted, wrongFamily);
        check(ratio < 1.0, "the adaptive floor reduces the still-stretch residual sd");
        // ±10%, not ±25%: the two exponent families for minScale 0.05 are 0.779 (q, which
        // is what predict() scales) and 0.607 (sigma_jerk). A wide band admits both, so it
        // would not notice a refactor that moved the multiplier onto sigma_jerk.
        check(ratio > 0.90 * predicted && ratio < 1.10 * predicted,
              "the reduction matches the q-family window law s^(1/12) within 10%");
        check(std::abs(ratio - predicted) < std::abs(ratio - wrongFamily),
              "and is nearer the q law than the sigma_jerk law (right family)");

        // (ii) the excursion survives — including in the worst case (pinned at the
        // floor for the whole track), which is the 0.9 % loss the fixture note derives.
        PoseSmootherConfig accCfg = lawBase;      // the shipped policy, gate widened only
        accCfg.adapt.mode = AdaptMode::Accel;
        accCfg.adapt.emitScalesForTest = true;
        const auto acc = smoothPoseTrack(in, int(WA), int(HA), accCfg);
        std::printf("       adaptFallbacks: shipped-defaults run=%d\n", acc.adaptFallbacks);
        check(acc.adaptFallbacks == 0, "and keeps the excursion fixture non-divergent too");
        const double aRaw = adAmplitude(in), aOff = adAmplitude(off.smoothed);
        const double aAcc = adAmplitude(acc.smoothed), aFlr = adAmplitude(flr.smoothed);
        std::printf("       4Hz 40px excursion: raw=%.2f  off=%.2f  accel=%.2f  minScale=%.2f\n",
                    aRaw, aOff, aAcc, aFlr);
        check(std::abs(aRaw / kAdAmpPx - 1.0) < 0.05, "the fixture really carries a 40 px amplitude");
        check(std::abs(aAcc / kAdAmpPx - 1.0) < 0.02, "accel keeps the excursion within 2% of 40 px");
        check(std::abs(aAcc / std::max(aOff, 1e-9) - 1.0) < 0.02,
              "and within 2% of the mode-off amplitude");
        check(std::abs(aFlr / kAdAmpPx - 1.0) < 0.02,
              "even pinned at minScale the excursion survives (0.9% predicted loss)");

        // (iii) Whether the policy ENGAGES is not asked of this fixture, and cannot be:
        // uniform 150 fps with WHITE 3 px noise is the worst case for a second-derivative
        // statistic (full noise power at the filter's own cutoff), so its still-stretch
        // |a| sits near or above aRef and the scale stays near 1.0 throughout. Measured
        // on this fixture: still scale median 1.000 (min 0.718). Section 11(c) asks that
        // question on a fixture with the real cadence and the real noise COLOUR, and
        // prints the |a| floor against aRef so the log shows why.
    }

    // ── 11c. the policy's DIRECTION on a realistic track, and the floor on record ─
    // The question phase 5 lives or dies on — at the shipped aRef (4000 px/s² at the
    // 1280×1024 reference, chosen from the P6/P7 side on 83 corpus swings) is a still
    // address below the knee? — is NOT settled here, and cannot be: see the fixture note
    // above and the ⚠ block at the assertions.
    // This section runs the real ≈27 ms address cadence with the dense zone opening 500 ms
    // before the motion, asserts the ORDERING (still smoothed harder than moving, moving
    // back at 1.0), and prints the pass-1 |a| median/p95 beside the scale medians so the
    // floor this fixture presents to aRef is always on the record next to the corpus's
    // measured 1652 px/s² p95. Engagement is the bake-off's call, on real swings.
    std::printf("=== smoothPoseTrack: adapt accel engagement on a realistic track ===\n");
    {
        const double total = kAdQuietS + kAdMotionS + kAdTailS;
        const auto times = realCadenceTimes(kAdQuietS - 0.5, total);   // dense from 500 ms before
        Lcg rng(0xC0FFEE11u);
        const auto in = buildRealisticTrack(times, rng);

        PoseSmootherConfig cfg;                    // the SHIPPED adapt defaults
        cfg.adapt.mode = AdaptMode::Accel;
        cfg.adapt.emitScalesForTest = true;
        const auto res = smoothPoseTrack(in, int(WA), int(HA), cfg);
        const std::vector<double> &s    = res.adaptScale[KPA];
        const std::vector<double> &aMag = res.adaptAccel[KPA];   // F9: first-class, not derived
        const double aRefEff = cfg.adapt.aRefPxS2 * std::sqrt((WA * HA) / (1280.0 * 1024.0));

        // The still stretch: everything at least 300 ms before the motion starts, minus
        // the first 250 ms (the loose init priors), so BOTH cadences are represented — the
        // coarse address grid and the dense zone that opens 500 ms before the motion.
        std::vector<double> stillScale, stillAccel, moveScale;
        int nCoarse = 0, nDense = 0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            const double t = double(in[i].t_us) * 1e-6;
            if (t > 0.25 && t < kAdQuietS - 0.30) {
                stillScale.push_back(s[i]);
                stillAccel.push_back(aMag[i]);
                if (t < kAdQuietS - 0.5) ++nCoarse; else ++nDense;
            } else if (t >= kAdQuietS && t < kAdQuietS + kAdMotionS
                       && std::abs(adTruthAccel(t)) >= 0.9 * adPeakAccel()) {
                moveScale.push_back(s[i]);
            }
        }
        check(stillAccel.size() == stillScale.size(),
              "the |a| hook drops nothing: one value per still frame (F9)");
        const double aMed = medianOf(stillAccel), aP95 = percentileOf(stillAccel, 0.95);
        const double sMed = medianOf(stillScale),  mMed = medianOf(moveScale);
        // The statistic that survives this fixture's floor being 2–4× the corpus's: the
        // FRACTION of frames the policy actually lengthens. A median saturates at 1.0 as
        // soon as more than half the still frames clear aRef, which on a fixture this
        // noisy says nothing about the policy.
        auto fracBelowOne = [](const std::vector<double> &v) {
            if (v.empty()) return 0.0;
            int n = 0; for (const double x : v) if (x < 1.0) ++n;
            return double(n) / double(v.size());
        };
        const double fStill = fracBelowOne(stillScale), fMove = fracBelowOne(moveScale);
        std::printf("       still |a|: median=%.0f p95=%.0f px/s2   aRefEff=%.0f"
                    "   (corpus 08-18 still-address p95 = 2172)\n", aMed, aP95, aRefEff);
        std::printf("       scale: still median=%.3f frac<1=%.2f (n=%zu: %d coarse + %d dense)"
                    "   moving median=%.3f frac<1=%.2f\n",
                    sMed, fStill, stillScale.size(), nCoarse, nDense, mMed, fMove);
        check(nCoarse > 5 && nDense > 15, "the fixture really carries both cadences");
        check(mMed == 1.0, "the moving stretch is back at today's window (scale 1.0)");
        // ⚠ THESE TWO ARE ORDERING CHECKS, NOT AN ENGAGEMENT GATE, AND THAT IS DELIBERATE.
        // Measured on this fixture: still |a| median 3113, p95 7546 px/s² — against the
        // shipped aRefEff of 4000 that reads as MOTION, where the corpus's own 08-18
        // still-address p95 is 2172 (s = 0.54). (At the earlier aRef 8000 and a ±3-FRAME
        // lead the same fixture read a still scale median of 0.585.) The gap is COLOUR, not
        // cadence: |a| is driven by error energy at the filter's cutoff (≈91 rad/s in the
        // dense zone), and an AR(1) drift has a 1/omega² tail, so even this fixture's 1.94 px
        // tau = 60 ms drift carries ≈9× the 0.5 px white component's power THERE. Matching
        // the corpus would need a spectrally smoother error model (integrated or
        // band-limited, not AR(1)), which nothing in this repo measures — so a synthetic
        // track cannot decide engagement and this test does not pretend to.
        // WHETHER THE POLICY ENGAGES AT THE SHIPPED aRef IS DECIDED BY THE BAKE-OFF ON REAL SWINGS
        // (tools/metrics/adapt_settings.jsonl + the C15 gate criteria). Asserted here: only
        // that the policy responds to motion in the right DIRECTION on a track with the real
        // cadence. The printed lines above put the floor on the record either way.
        check(fStill > 0.0, "the policy does something at all on a realistic still stretch");
        check(fStill > fMove, "and it lengthens the window in the still stretch, never in the moving one");
        check(sMed <= mMed, "the still stretch is never smoothed LESS than the moving one");
    }

    // ── 12. innov policy: wired to its knob, and honest about its limit ────────
    // ⚠ The tolerances here are WIDER than the accel ones and that is a finding, not a
    // convenience. innov²/S is the gate's own statistic, and for a CONSISTENT filter it
    // is chi²(1)-distributed with mean 1 WHATEVER the joint is doing; the part of a real
    // trajectory a constant-acceleration predictor cannot see over one dense step is
    // ~jerk·dt³/6 = 0.03 px for this 4 Hz 40 px fixture at 150 fps, against sigma_m =
    // 3.2 px. So at dense sampling the statistic is dominated by measurement noise, the
    // shipped innovRef of 4.0 sits at the 2-sigma point of that noise (~5 % of steps),
    // and the policy reads as a randomly modulated window rather than a motion-driven
    // one. What CAN be pinned exactly is that the statistic is wired to the knob, that
    // the run restarts per segment, and that the law holds when the knob pins the floor.
    std::printf("=== smoothPoseTrack: adapt innov ===\n");
    {
        Lcg rng(0xA5A5F00Du);                       // the same track as section 11(b)
        const auto times = uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS);
        const auto in    = buildKpTrackWH(KPA, times, adTruthX, kAdY, 3.0, rng, WA, HA);
        PoseSmootherConfig offCfg;                 // hand-built control (the default is accel)
        offCfg.adapt.mode = AdaptMode::Off;
        const auto off   = smoothPoseTrack(in, int(WA), int(HA), offCfg);

        PoseSmootherConfig iv;
        iv.adapt.mode = AdaptMode::Innov;
        iv.adapt.minScale = 0.05;                  // pinned: the law arithmetic below is for it
        iv.adapt.emitScalesForTest = true;
        const auto res = smoothPoseTrack(in, int(WA), int(HA), iv);
        const std::vector<double> &s = res.adaptScale[KPA];
        bool bounded = true, headOnes = true, someBelow = false, someOne = false;
        for (const double v : s) {
            bounded   = bounded && v >= iv.adapt.minScale && v <= 1.0;
            someBelow = someBelow || v < 1.0;
            someOne   = someOne   || v == 1.0;
        }
        for (int i = 0; i <= iv.adapt.innovRun; ++i)
            headOnes = headOnes && s[std::size_t(i)] == 1.0;
        check(bounded, "the innov scale never leaves [minScale, 1]");
        check(headOnes, "the init frame + the first innovRun accepted steps run at 1.0");
        check(someBelow && someOne, "the scale really tracks the statistic (it moves both ways)");
        check(std::abs(adAmplitude(res.smoothed) / kAdAmpPx - 1.0) < 0.02,
              "innov keeps the excursion within 2% of 40 px");

        // innovRef unreachable ⇒ pinned at the floor once the run fills ⇒ the same
        // window law as the accel floor run. This is the law check for this policy.
        PoseSmootherConfig pin = iv;
        pin.adapt.innovRef = 1e9;
        const auto pinned = smoothPoseTrack(in, int(WA), int(HA), pin);
        bool pinFloor = true;
        for (std::size_t i = std::size_t(pin.adapt.innovRun) + 1;
             i < pinned.adaptScale[KPA].size(); ++i)
            pinFloor = pinFloor && pinned.adaptScale[KPA][i] == pin.adapt.minScale;
        check(pinFloor, "an unreachable innovRef pins every later step at minScale");
        const double predicted = std::pow(0.05, 1.0 / 12.0);
        const double sdOff = adResidSd(off, kAdQuietLo, kAdQuietHi);
        const double sdPin = adResidSd(pinned, kAdQuietLo, kAdQuietHi);
        const double ratioPin = (sdOff > 0.0) ? sdPin / sdOff : 1.0;
        check(ratioPin > 0.75 * predicted && ratioPin < 1.25 * predicted,
              "pinned at the floor, innov reduces the still-stretch sd by the same law");

        // innovRef ~0 ⇒ saturated at 1.0 everywhere ⇒ byte-identical to mode off. This
        // is the byte-identity argument itself under test: a scale of exactly 1.0 leaves
        // m_q x 1.0 == m_q, so the whole filter arithmetic is untouched.
        PoseSmootherConfig sat = iv;
        sat.adapt.innovRef = 1e-9;
        const auto satRes = smoothPoseTrack(in, int(WA), int(HA), sat);
        bool allOne = true;
        for (const double v : satRes.adaptScale[KPA]) allOne = allOne && v == 1.0;
        check(allOne, "an innovRef at zero keeps every step at 1.0");
        check(sameWhere(off, satRes, [](int) { return true; }),
              "and a scale of exactly 1.0 reproduces the mode-off output byte for byte");

        // What the shipped innovRef 4.0 actually buys on this track — recorded, and
        // bounded only by what is physically guaranteed (scale >= minScale ⇒ the ratio
        // cannot beat the floor run's; scale <= 1 ⇒ it cannot be worse than the control).
        const double sdIv = adResidSd(res, kAdQuietLo, kAdQuietHi);
        const double ratio = (sdOff > 0.0) ? sdIv / sdOff : 1.0;
        int nOne = 0;
        for (const double v : s) if (v == 1.0) ++nOne;
        std::printf("       innovRef 4.0: still sd ratio=%.3f (floor %.3f)  frames at 1.0 = %d/%d\n",
                    ratio, predicted, nOne, int(s.size()));
        check(ratio > 0.75 * predicted && ratio <= 1.0 + 1e-9,
              "innovRef 4.0 lands between the floor run and the control (see the note above)");
    }

    // ── 13. the group: legs = 11-16, body = 0-16, and the tail NEVER adapts ────
    std::printf("=== smoothPoseTrack: adapt group selection ===\n");
    {
        Lcg rng(0x1E65CA1Fu);
        const auto times = uniformTimes(150.0, 1.0);
        const auto in    = buildLegsTrack(times, rng);
        // Two test-only settings, both so this section measures GROUP SELECTION and
        // nothing else: gateSig 6 (a reduced q tightens the 3σ gate, and a single flipped
        // accept flag would make the divergence guard reject the keypoint and hand back the
        // unadapted output — section 19 is where that is tested), and an unreachable aRef so
        // every fixture keypoint is genuinely scaled. At the SHIPPED aRef the fixture's
        // wrist (arc |a| = 400·5² = 10 kpx/s², i.e. above aRefEff) would clamp to 1.0 and
        // "the group reaches the wrist" would be untestable on this fixture rather than
        // false — which is exactly how it failed before.
        PoseSmootherConfig off_ = { };
        off_.gateSig = 6.0;
        off_.adapt.mode = AdaptMode::Off;   // hand-built control (the default is accel now)
        const auto off   = smoothPoseTrack(in, W, H, off_);
        PoseSmootherConfig legs = off_;
        legs.adapt.mode = AdaptMode::Accel;
        legs.adapt.group = AdaptGroup::Legs;
        legs.adapt.aRefPxS2 = 1e5;
        legs.adapt.emitScalesForTest = true;
        PoseSmootherConfig body = legs;
        body.adapt.group = AdaptGroup::Body;
        const auto rl = smoothPoseTrack(in, W, H, legs);
        const auto rb = smoothPoseTrack(in, W, H, body);
        std::printf("       adaptFallbacks: legs=%d body=%d\n", rl.adaptFallbacks, rb.adaptFallbacks);
        check(rl.adaptFallbacks == 0 && rb.adaptFallbacks == 0,
              "the widened gate keeps both group runs non-divergent");

        // The group decides which keypoints get a scale AT ALL — the rows are the direct
        // statement of that, independent of whether a given keypoint's |a| then moves it.
        bool legRows = true, bodyRows = true;
        for (int k = 0; k < kWholeBodyJoints; ++k) {
            const bool legWant  = isLegKeypoint(k);
            const bool bodyWant = (k < kFootFirstKp);
            legRows  = legRows  && (rl.adaptScale[k].empty() != legWant);
            bodyRows = bodyRows && (rb.adaptScale[k].empty() != bodyWant);
        }
        check(legRows, "group legs emits a scale row for 11-16 and for no other keypoint");
        check(bodyRows, "group body emits one for 0-16 and for no keypoint of the tail");
        check(sameWhere(off, rl, [](int k) { return !isLegKeypoint(k); }),
              "group legs leaves keypoints 0-10 and 17+ byte-identical");
        check(sameWhere(off, rb, [](int k) { return k >= kFootFirstKp; }),
              "group body leaves the wholebody tail 17+ byte-identical");

        auto moved = [&](const PoseSmootherOutput &r, int k) {
            for (std::size_t i = 0; i < off.aux.size(); ++i)
                if (off.aux[i].sigma[k] != r.aux[i].sigma[k]
                        || !exactEq(off.smoothed[i].kp[k], r.smoothed[i].kp[k])) return true;
            return false;
        };
        bool legsMoved = true, bodyMoved = true;
        for (int k = kLegFirstKp; k <= kLegLastKp; ++k) legsMoved = legsMoved && moved(rl, k);
        for (const KpArc &tr : kLegsFixture)
            if (tr.kp < kFootFirstKp) bodyMoved = bodyMoved && moved(rb, tr.kp);
        check(legsMoved, "every legs keypoint 11-16 responds to the adaptive window");
        check(bodyMoved, "group body reaches the fixture's shoulder (5) and wrist (9) too");
    }

    // ── 14. every poseSmooth.adapt.* key round-trips through fromOverrides ─────
    std::printf("=== smoothPoseTrack: adapt override keys ===\n");
    {
        QVariantMap ov;
        ov[QStringLiteral("poseSmooth.adapt.mode")]       = QStringLiteral("accel");
        ov[QStringLiteral("poseSmooth.adapt.group")]      = QStringLiteral("body");
        ov[QStringLiteral("poseSmooth.adapt.minScale")]   = 0.2;
        ov[QStringLiteral("poseSmooth.adapt.aRefPxS2")]   = 12345.0;
        ov[QStringLiteral("poseSmooth.adapt.expo")]       = 2.0;
        ov[QStringLiteral("poseSmooth.adapt.leadMs")]     = 35.0;
        ov[QStringLiteral("poseSmooth.adapt.innovRef")]   = 9.0;
        ov[QStringLiteral("poseSmooth.adapt.innovRun")]   = 7;
        ov[QStringLiteral("poseSmooth.adapt.emitScalesForTest")] = true;   // NOT a key
        const PoseSmootherConfig c = PoseSmootherConfig::fromOverrides(ov);
        check(c.adapt.mode == AdaptMode::Accel && c.adapt.group == AdaptGroup::Body
                  && c.adapt.minScale == 0.2 && c.adapt.aRefPxS2 == 12345.0
                  && c.adapt.expo == 2.0 && c.adapt.leadMs == 35.0
                  && c.adapt.innovRef == 9.0 && c.adapt.innovRun == 7,
              "all eight poseSmooth.adapt.* keys round-trip through a QVariantMap");
        check(!c.adapt.emitScalesForTest,
              "emitScalesForTest is a test hook, NOT a sweep key (not readable from the map)");

        QVariantMap m2;
        m2[QStringLiteral("poseSmooth.adapt.mode")] = QStringLiteral("  Innov ");
        check(PoseSmootherConfig::fromOverrides(m2).adapt.mode == AdaptMode::Innov,
              "mode parsing trims and case-folds");
        QVariantMap m3;
        m3[QStringLiteral("poseSmooth.adapt.mode")]  = QStringLiteral("adaptive");
        m3[QStringLiteral("poseSmooth.adapt.group")] = QStringLiteral("torso");
        const PoseSmootherConfig c3 = PoseSmootherConfig::fromOverrides(m3);
        check(c3.adapt.mode == AdaptMode::Off && c3.adapt.group == AdaptGroup::Legs,
              "an unrecognised mode/group reads as off/legs (a typo'd sweep line is a CONTROL run)");
        // Range guards (F7): the two keys where an out-of-range value would be silently
        // WRONG rather than merely odd. minScale > 1 would make the "adaptive" window
        // SHORTER than today's everywhere, < 0 would invert the clamp, and an unbounded
        // innovRun would let one sweep line carry a multi-second memory of one innovation.
        QVariantMap bad;
        bad[QStringLiteral("poseSmooth.adapt.minScale")] = 1.5;
        bad[QStringLiteral("poseSmooth.adapt.innovRun")] = 1000;
        const PoseSmootherConfig cb = PoseSmootherConfig::fromOverrides(bad);
        check(cb.adapt.minScale == 1.0, "minScale 1.5 is clamped to 1.0 (never a SHORTER window)");
        check(cb.adapt.innovRun == 32, "innovRun 1000 is capped at 32");
        QVariantMap neg;
        neg[QStringLiteral("poseSmooth.adapt.minScale")] = -0.5;
        neg[QStringLiteral("poseSmooth.adapt.innovRun")] = 0;
        const PoseSmootherConfig cn = PoseSmootherConfig::fromOverrides(neg);
        check(cn.adapt.minScale == 0.0, "minScale -0.5 is clamped to 0.0");
        check(cn.adapt.innovRun == 1, "innovRun 0 is raised to 1");

        const PoseSmootherConfig none = PoseSmootherConfig::fromOverrides(QVariantMap{});
        PoseSmootherConfig def;
        check(none.adapt.mode == def.adapt.mode && none.adapt.aRefPxS2 == def.adapt.aRefPxS2
                  && none.adapt.expo == def.adapt.expo && none.adapt.innovRun == def.adapt.innovRun,
              "an empty override map leaves the frozen adapt defaults");
    }

    // ── 15. the lead (leadMs) is LOCAL to an acceleration edge ─────────────────
    // The symmetric max filter exists so the window is short before the acceleration
    // arrives; the cost of that must be confined to the edges. Noiseless, on the
    // single-pulse fixture, so |a| is two clean plateaus rather than an oscillation.
    std::printf("=== smoothPoseTrack: adapt leadMs locality ===\n");
    {
        Lcg noRng(1);
        const auto times = uniformTimes(150.0, 1.35);
        const auto in    = buildKpTrackWH(KPA, times, pulseTruthX, kAdY, 0.0, noRng, WA, HA);
        PoseSmootherConfig c0;
        c0.adapt.mode = AdaptMode::Accel;
        c0.adapt.leadMs = 0.0;
        c0.adapt.emitScalesForTest = true;
        PoseSmootherConfig c3 = c0;
        c3.adapt.leadMs = 20.0;         // the shipped lead; 3 frames on this uniform track
        const auto r0 = smoothPoseTrack(in, int(WA), int(HA), c0);
        const auto r3 = smoothPoseTrack(in, int(WA), int(HA), c3);
        const std::vector<double> &s0 = r0.adaptScale[KPA];
        const std::vector<double> &s3 = r3.adaptScale[KPA];

        // Allowance = leadMs + 8 frames. The 8 frames (≈53 ms) are the ESTIMATOR's own
        // ramp, not the lead's: |a| here is an RTS quantity with a ≈33 ms window at
        // 150 fps, so it takes several frames to reach a plateau after the truth steps,
        // and the lead filter can only be asked to be local to within that.
        const double allowS = c3.adapt.leadMs * 1e-3 + 8.0 / 150.0;
        const double edges[] = { kPulseE0, kPulseE1, kPulseE2, kPulseE3 };
        bool monotone = true, localOnly = true, leadsEarly = false;
        int nFar = 0, nDiff = 0;
        for (std::size_t i = 0; i < s0.size(); ++i) {
            const double t = double(in[i].t_us) * 1e-6;
            double dEdge = 1e30;     // (not `near` - that is a legacy MSVC macro)
            for (const double e : edges) dEdge = std::min(dEdge, std::abs(t - e));
            monotone = monotone && s3[i] >= s0[i];
            if (s3[i] != s0[i]) ++nDiff;
            if (dEdge > allowS) { ++nFar; localOnly = localOnly && (s3[i] == s0[i]); }
            if (t < kPulseE0 && (kPulseE0 - t) <= 3.5 / 150.0 && s3[i] > s0[i]) leadsEarly = true;
        }
        std::printf("       lead 0 vs 20ms: %d frames differ, %d frames are far from an edge\n",
                    nDiff, nFar);
        check(monotone, "a symmetric max filter can only RAISE the scale");
        check(c3.adapt.leadMs == 20.0, "the lead is a DURATION (ms), not a frame count");
        check(nFar > 90, "most frames are far from an acceleration edge (the claim is not empty)");
        check(localOnly, "leadMs 0 vs 20 differ only near an acceleration edge");
        check(leadsEarly, "and the scale is already higher in the 3 frames BEFORE an edge");
        check(nDiff > 0, "the two lead settings really do differ somewhere");
    }

    // ── 16. a segment break: accel widens across it, innov's run restarts ──────
    std::printf("=== smoothPoseTrack: adapt across a segment break ===\n");
    {
        Lcg rng(0xB0B0CAFEu);
        const auto times = uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS);
        const double hLo = 1.20, hHi = 1.50;        // 300 ms hole > the 250 ms coast budget
        const auto in = buildKpTrackWH(KPA, times, adTruthX, kAdY, 3.0, rng, WA, HA, hLo, hHi);

        PoseSmootherConfig ac;
        ac.adapt.mode = AdaptMode::Accel;
        ac.adapt.emitScalesForTest = true;
        const auto ra = smoothPoseTrack(in, int(WA), int(HA), ac);
        // Selected by conf, not by a reconstructed time — see the ⚠ note in section 18 for
        // the boundary bug that pattern hides (the fixture stamps the hole from times[i],
        // a reader recomputing from t_us can land one frame either side of the literal).
        int nHole = 0, firstAfter = -1;
        bool holeOne = true, holeOff = true, seenHole = false;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (double(in[i].conf[KPA]) < ac.confMeasMin) {
                ++nHole;
                seenHole = true;
                holeOne = holeOne && ra.adaptScale[KPA][i] == 1.0;
                holeOff = holeOff && ra.aux[i].tier[KPA] == uint8_t(PoseTier::Off);
            } else if (seenHole && firstAfter < 0) {
                firstAfter = int(i);        // the frame that re-opens a segment
            }
        }
        check(nHole > 40, "the 300 ms hole spans ~45 frames");
        check(holeOff, "the hole leaves no smoothed value (trimmed coast, then no segment)");
        check(holeOne, "accel carries 1.0 across a break — it never inherits the still window");

        PoseSmootherConfig ivc;
        ivc.adapt.mode = AdaptMode::Innov;
        ivc.adapt.emitScalesForTest = true;
        const auto ri = smoothPoseTrack(in, int(WA), int(HA), ivc);
        bool headOnes = firstAfter > 0, refilled = false;
        for (int i = firstAfter; i >= 0 && i <= firstAfter + ivc.adapt.innovRun; ++i)
            headOnes = headOnes && ri.adaptScale[KPA][std::size_t(i)] == 1.0;
        for (int i = firstAfter + ivc.adapt.innovRun + 1; i > 0 && i < int(in.size()); ++i)
            if (ri.adaptScale[KPA][std::size_t(i)] < 1.0) refilled = true;
        check(headOnes, "innov restarts its accepted-step run in the segment after the break");
        check(refilled, "and the run refills afterwards (the scale leaves 1.0 again)");
    }

    // ── 17. aRefPxS2 is a per-FORMAT number: the sqrt(W·H) scaling ─────────────
    // |a| is a PIXEL quantity, so the same motion filmed larger reads more px/s² and a
    // fixed threshold would score the smaller capture as quiet. The run therefore scales
    // aRef by sqrt(frameW·frameH / (1280·1024)) — and these are the cases that fail if
    // that factor is dropped or reduced to the width alone.
    //   (a) a SIMILARITY change (both axes ×1.5) is undone EXACTLY: with every
    //       px-dimensioned filter constant scaled by 1.5 alongside the track, the filter
    //       is homogeneous of degree 1 in px (states ×1.5, covariances ×2.25, gate
    //       decisions unchanged), so |a| is exactly 1.5× and the scale vector must be
    //       UNCHANGED. (In production the σ constants are fixed px, so the invariance is
    //       approximate — what the factor removes is the first-order dependence.)
    //   (b) an ANISOTROPIC change cannot be undone by any single factor, and the second
    //       block below measures what the geometric mean actually does about it.
    // Both run with an unreachable aRef, minScale 0 and no lead filter, so the emitted
    // scale is |a|/aRefEff unclamped and the comparison is of the scaling, not the clamp.
    std::printf("=== smoothPoseTrack: adapt aRef scales with the frame FORMAT ===\n");
    {
        Lcg noRng(1);
        const auto times = uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS);
        const auto in    = buildKpTrackWH(KPA, times, adTruthX, kAdY, 0.0, noRng, WA, HA);
        PoseSmootherConfig base;
        base.adapt.mode = AdaptMode::Accel;
        base.adapt.aRefPxS2 = 1e5;       // nothing clamps, so the scale IS |a|/aRefEff
        base.adapt.minScale = 0.0;
        base.adapt.expo     = 1.0;       // LINEAR, so a threshold ratio reads as a scale
                                         // ratio: at the shipped expo 8 the same format
                                         // change would show as that ratio to the 8th
                                         // power (0.75^8 = 0.10), which measures pow(),
                                         // not the format rule.
        base.adapt.leadMs   = 0.0;       // per-frame, no max filter
        base.adapt.emitScalesForTest = true;
        const auto r1 = smoothPoseTrack(in, int(WA), int(HA), base);

        const double k = 1.5;
        PoseSmootherConfig big = base;
        big.measSigBasePx  *= k; big.measSigSlopePx *= k; big.sigmaJerk *= k;
        big.initSigPPx     *= k; big.initSigV       *= k; big.initSigA  *= k;
        const auto r2 = smoothPoseTrack(in, int(WA * k), int(HA * k), big);

        double worst = 0.0; int nMid = 0;
        for (std::size_t i = 0; i < r1.adaptScale[KPA].size(); ++i) {
            const double a = r1.adaptScale[KPA][i], b = r2.adaptScale[KPA][i];
            worst = std::max(worst, std::abs(a - b));
            if (a > 1e-6 && a < 0.999) ++nMid;   // strictly inside the clamp
        }
        std::printf("       1280x1024 vs 1920x1536: worst |delta scale| = %.3g over %zu frames"
                    " (%d strictly inside the clamp)\n", worst, r1.adaptScale[KPA].size(), nMid);
        check(nMid > 10, "frames sit strictly inside the clamp, so the check is not vacuous");
        check(worst < 1e-9, "the scale vector is invariant to a SIMILARITY format change");

        // ── the anisotropic case (F8): 1280×1024 → 720×1024, the corpus's own other
        // format, which changes the width by 0.5625 and the height NOT AT ALL. |a| is one
        // isotropic magnitude and aRef is one isotropic threshold, so no single factor can
        // undo that; the geometric mean sqrt(W·H) moves the threshold by sqrt(0.5625) =
        // 0.75, which SPLITS the error between the axes instead of putting it all on one:
        //     horizontal motion: |a| ×0.5625, aRef ×0.75  ⇒ s ×0.75
        //     vertical motion:   |a| ×1.0,    aRef ×0.75  ⇒ s ×1.333
        // A width-only rule (aRef ×0.5625) would read ×1.0 on the horizontal and ×1.778 on
        // the vertical — exact for a sway, 78 % wrong for a pelvis lift. This case measures
        // both so the residual is a number in the log, not a claim in a comment. The honest
        // fix is per-axis normalisation and it needs a design decision (see the constant).
        const PoseSmootherConfig &aniso = base;   // same unclamped, un-led config
        const auto xIn = in;                                        // motion in x
        const auto yIn = buildYMotionTrack(times, WA, HA);          // motion in y
        const auto x12 = smoothPoseTrack(xIn, int(WA), int(HA), aniso);
        const auto x07 = smoothPoseTrack(xIn, 720, int(HA), aniso);
        const auto y12 = smoothPoseTrack(yIn, int(WA), int(HA), aniso);
        const auto y07 = smoothPoseTrack(yIn, 720, int(HA), aniso);
        std::vector<double> rx, ry;
        for (int i = kAdMotionLo; i < kAdMotionHi; ++i) {
            const double t = double(xIn[std::size_t(i)].t_us) * 1e-6;
            if (std::abs(adTruthAccel(t)) < 0.9 * adPeakAccel()) continue;
            const double a12 = x12.adaptScale[KPA][std::size_t(i)];
            const double b12 = y12.adaptScale[KPA][std::size_t(i)];
            if (a12 > 1e-9) rx.push_back(x07.adaptScale[KPA][std::size_t(i)] / a12);
            if (b12 > 1e-9) ry.push_back(y07.adaptScale[KPA][std::size_t(i)] / b12);
        }
        const double mx = medianOf(rx), my = medianOf(ry);
        std::printf("       720x1024 vs 1280x1024: horizontal motion s ratio=%.4f (mean rule 0.75,"
                    " width rule 1.0)   vertical s ratio=%.4f (mean rule 1.3333, width rule 1.7778)\n",
                    mx, my);
        check(!rx.empty() && !ry.empty(), "both anisotropy fixtures produced readable ratios");
        check(std::abs(mx / 0.75 - 1.0) < 0.03,
              "a horizontal motion reads 0.75x at 720x1024 (the geometric mean's half of the error)");
        check(std::abs(my / (4.0 / 3.0) - 1.0) < 0.03,
              "and a vertical motion 1.333x — one isotropic threshold cannot undo a non-square change");
    }

    // ── 18. a step with NO MEASUREMENT never gets the reduced q ────────────────
    // A coasted step is the filter GUESSING, and its posterior σ is the only honest
    // statement of that. Shrinking q on such a step shrinks that σ without adding one bit
    // of information — a bridged sample would claim to be more certain than the measured
    // samples either side of it, which is precisely the dishonesty this design exists to
    // remove. So both policies force scale 1.0 wherever there is no measurement, and this
    // section pins it structurally (the emitted scale) and behaviourally (σ still grows
    // across the bridge).
    std::printf("=== smoothPoseTrack: a coasted step keeps today's q ===\n");
    {
        const auto times = uniformTimes(150.0, 2.0);
        const double hLo = 1.00, hHi = 1.10;        // 100 ms (15 frames) — well inside the 250 ms budget
        Lcg rng(0xFEEDFACEu);
        const auto in = buildKpTrackWH(KPA, times, [](double) { return kAdX0; },
                                       kAdY, 2.0, rng, WA, HA, hLo, hHi);
        // gateSig 6 for the same reason as sections 11(b) and 13: at minScale the tightened
        // 3σ radius can flip one borderline accept flag, and the divergence guard would then
        // hand back the unadapted output (with the scale row rewritten to 1.0), which would
        // make this section pass or fail for a reason that has nothing to do with F2.
        PoseSmootherConfig fl;                      // floored everywhere the policy can see
        fl.gateSig = 6.0;
        fl.adapt.mode = AdaptMode::Accel;
        fl.adapt.aRefPxS2 = 1e9;
        fl.adapt.emitScalesForTest = true;
        PoseSmootherConfig iv = fl;
        iv.adapt.mode = AdaptMode::Innov;
        iv.adapt.innovRef = 1e9;                    // same: pinned at the floor
        PoseSmootherConfig ofc;
        ofc.gateSig = 6.0;
        ofc.adapt.mode = AdaptMode::Off;    // hand-built control (the default is accel now)
        const auto acc = smoothPoseTrack(in, int(WA), int(HA), fl);
        const auto ivr = smoothPoseTrack(in, int(WA), int(HA), iv);
        const auto off = smoothPoseTrack(in, int(WA), int(HA), ofc);

        // ⚠ THE COASTED SET IS SELECTED BY conf, NOT BY A TIME COMPARISON, and that is the
        // whole fix for this section's earlier failure. The fixture stamps the hole using
        // `times[i]` (an i/150 quotient) while a reader recomputing the time from `t_us`
        // goes through llround + a 1e-6 multiply, and for the boundary frame those two paths
        // straddle the literal: t_us/1e6 came out just BELOW 1.10 where i/150.0 was just
        // above, so the reader's window held 16 frames where the fixture had stamped 15 —
        // one confident, measured frame inside the "hole", which is exactly what tripped
        // "every frame Pred" and "every step at 1.0" while the diagnostic line showed
        // 15 Pred + 1 Meas and scales of 1.0. conf is the same stored byte on both sides,
        // so this selection cannot drift; the same pattern is used in section 16.
        int nHole = 0, nOff = 0, nPred = 0, nMeas = 0;
        bool holeOnesA = true, holeOnesI = true, holePred = true, floorsElsewhere = false;
        bool seenHole = false;
        double sigEdgeA = 0.0, sigMidA = 0.0, sigEdgeO = 0.0, sigMidO = 0.0;
        double firstConf = -1.0, firstScaleA = -1.0, firstScaleI = -1.0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (double(in[i].conf[KPA]) < fl.confMeasMin) {   // no measurement offered
                ++nHole;
                seenHole = true;
                const uint8_t tier = acc.aux[i].tier[KPA];
                if (tier == uint8_t(PoseTier::Off)) ++nOff;
                else if (tier == uint8_t(PoseTier::Pred)) ++nPred;
                else ++nMeas;
                if (firstConf < 0.0) {              // one frame's raw facts, for the log
                    firstConf   = double(in[i].conf[KPA]);
                    firstScaleA = acc.adaptScale[KPA][i];
                    firstScaleI = ivr.adaptScale[KPA][i];
                }
                holeOnesA = holeOnesA && acc.adaptScale[KPA][i] == 1.0;
                holeOnesI = holeOnesI && ivr.adaptScale[KPA][i] == 1.0;
                holePred  = holePred  && tier == uint8_t(PoseTier::Pred);
                sigMidA = std::max(sigMidA, double(acc.aux[i].sigma[KPA]));
                sigMidO = std::max(sigMidO, double(off.aux[i].sigma[KPA]));
            } else if (!seenHole) {
                sigEdgeA = double(acc.aux[i].sigma[KPA]);   // the last measured frame before it
                sigEdgeO = double(off.aux[i].sigma[KPA]);
                if (i > 45 && acc.adaptScale[KPA][i] == fl.adapt.minScale) floorsElsewhere = true;
            }
        }
        // ⚠ THIS LINE IS THE DIAGNOSTIC, and it is here because this section failed once
        // with all three of its claims false at the same time. Each field discriminates a
        // different cause: conf says whether the hole reached the fixture at all (it must be
        // 0.05, i.e. below confMeasMin 0.35); the tier histogram says whether the segment
        // bridged (Pred), broke (Off) or never coasted (Meas); the two scales say whether the
        // no-measurement override fired (1.0) or the floor leaked into a coasted step
        // (minScale); and the fallback counts say whether the guard rewrote the rows.
        std::printf("       hole: n=%d tiers Off/Pred/Meas=%d/%d/%d  first conf=%.2f"
                    " scaleA=%.4f scaleI=%.4f  fallbacks A=%d I=%d\n",
                    nHole, nOff, nPred, nMeas, firstConf, firstScaleA, firstScaleI,
                    acc.adaptFallbacks, ivr.adaptFallbacks);
        std::printf("       bridge sigma: adaptive %.2f -> %.2f px   mode off %.2f -> %.2f px\n",
                    sigEdgeA, sigMidA, sigEdgeO, sigMidO);
        check(nHole >= 12, "the 100 ms hole spans several frames");
        check(acc.adaptFallbacks == 0 && ivr.adaptFallbacks == 0,
              "the widened gate keeps this fixture non-divergent (so the rows are pass 2's)");
        check(firstConf < 0.35, "the hole really is below confMeasMin (no measurement offered)");
        check(nOff == 0 && nMeas == 0 && holePred,
              "the hole bridges (every frame Pred) rather than breaking the segment");
        check(floorsElsewhere, "the run really IS floored on the measured frames (not a no-op)");
        check(holeOnesA, "accel: every step without a measurement runs at scale 1.0");
        check(holeOnesI, "innov: every step without a measurement runs at scale 1.0");
        // σ still grows across the bridge. NB the adaptive run's bridge σ is legitimately
        // SMALLER than mode-off's — the state entering the bridge is better determined, and
        // that is information, not a claim. What F2 fixes is the coasted STEP's own q.
        check(sigEdgeA > 0.0 && sigMidA > 1.3 * sigEdgeA,
              "the bridged samples still admit their uncertainty (sigma grows across the hole)");
    }

    // ── 19. the divergence guard: the adaptive window never removes a sample ───
    // Pass 2 re-decides segmentation from scratch, and a smaller q shrinks Pp, which
    // shrinks S, which TIGHTENS the 3σ gate. At the shipped minScale that is negligible;
    // at the C15 grid's 0.0025 (σ_jerk a full 10× below the collapse knee the .cpp's
    // derivation block measured) it is not. So the guard compares the two passes'
    // accepted[]/hasSmoothed[] and, on ANY difference, keeps pass 1 — the UNADAPTED
    // output — and counts it. The fixture engineers the divergence deterministically: a
    // still track at conf 1.0 (σ_meas 2 px ⇒ R = 4 ⇒ a 3σ gate radius of ≈6–8 px) carrying
    // a LADDER of planted single-frame outliers from 3.0 to 13.5 px, so several of them sit
    // between pass 1's gate radius and pass 2's tighter one wherever exactly those fall.
    std::printf("=== smoothPoseTrack: adapt divergence guard ===\n");
    {
        const auto times = uniformTimes(150.0, 2.7);
        Lcg rng(0x0D15C0DEu);
        auto in = buildKpTrackWH(KPA, times, [](double) { return kAdX0; },
                                 kAdY, 2.0, rng, WA, HA);
        for (auto &f : in) f.conf[KPA] = 1.0f;      // σ_meas = measSigBasePx = 2 px
        for (int j = 0; j < 31; ++j) {              // the ladder: 3.00, 3.35, … 13.50 px
            const int f = 40 + 12 * j;
            if (f >= int(in.size())) break;
            in[std::size_t(f)].kp[KPA].setX(in[std::size_t(f)].kp[KPA].x() + (3.0 + 0.35 * j) / WA);
        }
        PoseSmootherConfig offCfg;          // the UNADAPTED reference, hand-built
        offCfg.adapt.mode = AdaptMode::Off;
        const auto off = smoothPoseTrack(in, int(WA), int(HA), offCfg);

        PoseSmootherConfig div;
        div.adapt.mode = AdaptMode::Accel;
        div.adapt.aRefPxS2 = 1e9;        // floor the scale everywhere the policy can see
        div.adapt.minScale = 0.0025;     // the C15 grid's floor — the case F3 is about
        div.adapt.emitScalesForTest = true;
        const auto res = smoothPoseTrack(in, int(WA), int(HA), div);
        std::printf("       minScale 0.0025 on a gate-boundary ladder: adaptFallbacks=%d\n",
                    res.adaptFallbacks);
        check(res.adaptFallbacks == 1,
              "the guard fired for exactly the one keypoint whose segmentation would have moved");
        check(sameWhere(off, res, [](int) { return true; }),
              "and that keypoint kept its UNADAPTED output, byte for byte (no sample removed)");
        bool rowOnes = true;
        for (const double v : res.adaptScale[KPA]) rowOnes = rowOnes && v == 1.0;
        check(rowOnes, "the emitted scale row describes the output that was KEPT (1.0 = pass 1)");

        // Benign 1: a scale of 1.0 everywhere has nothing to diverge from.
        PoseSmootherConfig ben = div;
        ben.adapt.minScale = 1.0;
        const auto benr = smoothPoseTrack(in, int(WA), int(HA), ben);
        check(benr.adaptFallbacks == 0, "minScale 1.0 never diverges (count 0)");
        check(sameWhere(off, benr, [](int) { return true; }),
              "and is byte-identical to mode off without needing the guard");

        // Benign 2 — the one that matters: a well-behaved ENGAGED run at the shipped
        // defaults must not trip the guard, or the guard would quietly disable the feature.
        Lcg noRng(1);
        const auto clean = buildKpTrackWH(KPA, uniformTimes(150.0, kAdQuietS + kAdMotionS + kAdTailS),
                                          adTruthX, kAdY, 0.0, noRng, WA, HA);
        PoseSmootherConfig ship;
        ship.adapt.mode = AdaptMode::Accel;
        ship.adapt.emitScalesForTest = true;
        const auto shipRes = smoothPoseTrack(clean, int(WA), int(HA), ship);
        bool engaged = false;
        for (const double v : shipRes.adaptScale[KPA]) if (v < 1.0) engaged = true;
        check(engaged && shipRes.adaptFallbacks == 0,
              "the shipped defaults engage on a clean track WITHOUT tripping the guard");

        // Benign 3 — the case that decides whether the guard is usable at all: a QUIET
        // REAL-CADENCE track (section 11c's fixture, same seed) at the shipped defaults must
        // not trip it. A guard that fires on ordinary swings disables the feature SILENTLY —
        // the keypoint keeps its unadapted output and only this count ever says so. The
        // real-swing equivalent is being measured on the corpus in parallel; this is the
        // synthetic canary, and its printed count is the number to compare against.
        Lcg rc(0xC0FFEE11u);
        const auto rcIn = buildRealisticTrack(
            realCadenceTimes(kAdQuietS - 0.5, kAdQuietS + kAdMotionS + kAdTailS), rc);
        PoseSmootherConfig rcCfg;
        rcCfg.adapt.mode = AdaptMode::Accel;
        rcCfg.adapt.emitScalesForTest = true;
        const auto rcRes = smoothPoseTrack(rcIn, int(WA), int(HA), rcCfg);
        std::printf("       real-cadence track, shipped defaults: adaptFallbacks=%d\n",
                    rcRes.adaptFallbacks);
        check(rcRes.adaptFallbacks == 0,
              "a quiet real-cadence swing at the shipped defaults does not trip the guard");
    }

    // ── degenerate inputs ─────────────────────────────────────────────────────
    std::printf("=== smoothPoseTrack: degenerate ===\n");
    {
        check(smoothPoseTrack({}, W, H).smoothed.empty(), "empty input ⇒ empty output");
        std::vector<PoseFrame2D> one(1);
        one[0].kp[KP] = QPointF(0.5, 0.5); one[0].conf[KP] = 0.9f;
        const auto r = smoothPoseTrack(one, W, H);
        check(r.smoothed.size() == 1 && r.aux[0].tier[KP] == uint8_t(PoseTier::Off),
              "a lone frame (<2 steps) has no smoothed value ⇒ Off passthrough");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
