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
//   cmake --build build/analyzer-tests --target pose_smoother_test
//   ctest --test-dir build/analyzer-tests -R pose_smoother --output-on-failure

#include "../pose_smoother.h"

#include <QVariantMap>

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

// Byte-equality of two smoother outputs over the keypoints `keep` selects.
template <typename Pred>
static bool sameWhere(const PoseSmootherOutput &a, const PoseSmootherOutput &b, Pred keep)
{
    if (a.smoothed.size() != b.smoothed.size() || a.aux.size() != b.aux.size()) return false;
    for (std::size_t i = 0; i < a.smoothed.size(); ++i) {
        if (a.smoothed[i].t_us != b.smoothed[i].t_us) return false;
        for (int k = 0; k < kWholeBodyJoints; ++k) {
            if (!keep(k)) continue;
            if (!(a.smoothed[i].kp[k]   == b.smoothed[i].kp[k]
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
                same = a.smoothed[i].kp[k] == b.smoothed[i].kp[k]
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
            passthrough &= res.smoothed[i].kp[KP] == in[i].kp[KP];
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
            bodySame = resBody.smoothed[i].kp[KP]   == resWhole.smoothed[i].kp[KP]
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
                      && resWhole.smoothed[i].kp[KP]  == resScaled.smoothed[i].kp[KP]
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
                     || base.smoothed[i].kp[k] != scaled.smoothed[i].kp[k];
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
        PoseSmootherConfig slow; slow.legsJerkScale = 0.1;

        // (b1) STATIONARY hip, deterministic pseudo-noise sd 3 px at 150 fps.
        {
            const auto times = uniformTimes(150.0, 2.0);   // 300 frames
            Lcg r1(0x5747104Eu), r2(0x5747104Eu);          // same seed ⇒ same track
            const auto in    = buildOneKpTrack(KPHIP, times, [](double) { return 900.0; },
                                               600.0, 3.0, r1);
            const auto inChk = buildOneKpTrack(KPHIP, times, [](double) { return 900.0; },
                                               600.0, 3.0, r2);
            check(in.size() == inChk.size() && in[7].kp[KPHIP] == inChk[7].kp[KPHIP],
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
            const double sdBase = residSd(smoothPoseTrack(in, W, H));
            const double sdSlow = residSd(smoothPoseTrack(in, W, H, slow));
            const double ratio  = sdBase > 0.0 ? sdSlow / sdBase : 1.0;
            std::printf("       stationary hip: sd base=%.3fpx  scale0.1=%.3fpx  ratio=%.3f"
                        " (predicted %.3f; window %.0fms -> %.0fms)\n",
                        sdBase, sdSlow, ratio, predicted,
                        legWindowMsForJerkScale(1.0), legWindowMsForJerkScale(0.1));
            check(ratio < 1.0, "legsJerkScale 0.1 reduces the stationary-hip residual sd");
            check(ratio > 0.75 * predicted && ratio < 1.25 * predicted,
                  "the reduction matches the s^(1/6) window law within 25%");
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
            const double aBase = amplitude(smoothPoseTrack(in, W, H).smoothed);
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
