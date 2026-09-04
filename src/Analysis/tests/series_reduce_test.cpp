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

// Standalone test for the shared robust reducers (src/Analysis/series_reduce.h).
// Synthetic series only — no pose, no fixture, no video. Mirrors
// lower_body_metrics_test.cpp in structure and style (own main, hand-rolled CHECK).
//
// The case that carries the design is §2: ONE sample of 99 in a ramp of 0..8 (the
// synthetic form of the +42 % sway and the −88° hip tilt in design §2's table).
// Every assertion there is stated against a NEGATIVE CONTROL computed inline from
// the same data by the arithmetic this header replaces — the adjacent-frame
// difference — because "the reducer returned a small number" only means something
// beside what the old one returned on the same samples.
//
// §5 and §8 carry the two corrections the adversarial review found: the centred
// window is CLAMPED to [from, to] (an unclamped span minimum came out below every
// sample in the span), and it WIDENS to three samples where the grid is sparse
// (at 27 ms spacing a ±20 ms window held one sample, so the reducer did nothing at
// all across the address and the backswing).
//
// ⚠ EVERY EXPECTED NUMBER IS DERIVED IN A COMMENT BESIDE IT, not observed from a
// run. If one of these fails, read the derivation before changing the number: the
// window arithmetic is the thing under test.

#include "../series_reduce_metric.h"   // series_reduce.h + viewOf(MetricSeries)

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;

#define CHECK(label, cond)                                        \
    do {                                                          \
        const bool ok = (cond);                                   \
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);  \
        if (!ok) ++g_fail;                                        \
    } while (0)

static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ── Synthetic series ────────────────────────────────────────────────────────

struct Series {
    std::vector<int64_t> t;
    std::vector<double>  v;
    std::vector<uint8_t> valid;      // empty ⇒ every sample valid, as MetricSeries means it
};

static SeriesView view(const Series &s)
{
    return SeriesView{ s.t.data(), s.v.data(), s.valid.empty() ? nullptr : s.valid.data(),
                       s.t.size() };
}

// A perfectly linear ramp: value = slopePerSec × t. At 8000 µs spacing and 10.0 per second the
// slope is exactly 1.0 per 100 ms and sample i is 0.08 i, so every expectation below is exact.
static Series makeRamp(int64_t spacingUs, int count, double slopePerSec)
{
    Series s;
    for (int i = 0; i < count; ++i) {
        const int64_t t = spacingUs * i;
        s.t.push_back(t);
        s.v.push_back(slopePerSec * (double(t) * 1.0e-6));
    }
    return s;
}

// THE reducer this header replaces, so the spike and noise cases can be stated as a ratio against
// it: the largest adjacent-VALID-sample |Δv/Δt|, scaled to 100 ms. Copied in spirit from
// chart_metrics.cpp's pre-Phase-2 summaryMasked loop.
static double rawAdjacentRate(const Series &s)
{
    double  rate = 0.0;
    bool    have = false;
    double  pv   = 0.0;
    int64_t pt   = 0;
    for (size_t i = 0; i < s.t.size(); ++i) {
        if (!s.valid.empty() && s.valid[i] == 0u) continue;
        if (have && s.t[i] != pt) {
            const double d = std::fabs(s.v[i] - pv) / (double(s.t[i] - pt) / 1.0e5);
            if (d > rate) rate = d;
        }
        pv = s.v[i]; pt = s.t[i]; have = true;
    }
    return rate;
}

// A deterministic pseudo-noise generator: a 32-bit LCG (the numerical-recipes constants) summed
// Irwin–Hall over 12 draws, which has mean 0 and standard deviation EXACTLY 1 — no <random>, no
// platform-dependent distribution implementation, the same numbers on every machine and every run.
struct Lcg {
    uint32_t s = 12345u;
    double uniform()
    {
        s = s * 1664525u + 1013904223u;
        return double(s >> 8) / 16777216.0;          // [0,1)
    }
    double gauss()                                    // mean 0, sd 1
    {
        double acc = 0.0;
        for (int k = 0; k < 12; ++k) acc += uniform();
        return acc - 6.0;
    }
};

int main()
{
    std::printf("=== series_reduce_test ===\n");

    const ReduceConfig cfg;                          // the frozen windows: ±15 / 40 / 50 ms, 3, 3
    CHECK("frozen windows are the ones design §5.2 states",
          cfg.atHalfWindowUs == 15000 && cfg.extremumWindowUs == 40000
              && cfg.minExtremumSamples == 3 && cfg.rateWindowUs == 50000
              && cfg.minRateSamples == 3);

    // Kept from §1 so §4 can assert the masked / non-finite spike gives the CLEAN answer exactly,
    // rather than re-deriving it and risking two different arithmetics.
    double  refMax = 0.0, refRate = 0.0;
    int64_t refMaxAt = 0;

    // ── 1) a clean ramp, 8 ms spacing — every reducer's exact answer ─────────
    {
        std::printf("=== 1) clean ramp: 100 samples, 8 ms, 1.0 per 100 ms ===\n");
        // t = 0..792000 µs, v = 0.08 i (so 0.00 .. 7.92).
        const Series r = makeRamp(8000, 100, 10.0);
        const SeriesView s = view(r);

        const Reduced rate = reduceRate(s, 0, 792000, cfg);
        std::printf("      rate = %.12f per 100 ms (sigma %.3g), atUs %lld\n",
                    rate.value, rate.sigma, static_cast<long long>(rate.atUs));
        CHECK("rate: the ramp's own slope, 1.0 per 100 ms", rate.ok && near(rate.value, 1.0, 1e-9));
        // An exact fit has no residual, so the standard error is 0 — not "small", 0 by definition.
        CHECK("rate: sigma is 0 on an exact fit", rate.sigma <= 1e-6);
        // TIES GO TO THE EARLIEST WINDOW. Every window here has the same slope to the last bit, so
        // without the relative margin in detail::improves the winner was decided by float noise
        // (anchor 48 of 93 in the first cut). The first qualifying window is samples 0..7 — 8
        // samples spanning 56 ms, since a 50 ms window at 8 ms spacing has to take the 7th sample
        // to reach its time base — and its centre is 28000.
        CHECK("rate: atUs is the FIRST qualifying window's centre, 28000", rate.atUs == 28000);

        // Extremum (max). The window is ±20 ms, so the LAST sample's window holds samples 97, 98
        // and 99 only (776000, 784000, 792000 — 772000 is not a sample): mean = (7.76+7.84+7.92)/3
        // = 7.84, which is above sample 98's 4-sample mean of 7.80 and every earlier one. This is
        // the whole design in one number: the largest windowed mean of a rising ramp is NOT its
        // largest sample.
        const Reduced mx = reduceExtremum(s, 0, 792000, true, cfg);
        std::printf("      max = %.12f at %lld (sigma %.3g)\n", mx.value,
                    static_cast<long long>(mx.atUs), mx.sigma);
        CHECK("extremum(max): the last sample's windowed mean, 7.84 (not its value 7.92)",
              mx.ok && near(mx.value, 7.84, 1e-9));
        CHECK("extremum(max): atUs is the centre sample's time", mx.atUs == 792000);
        // ⚠ SIGMA IS THE NOISE THE MEAN CARRIES, NOT THE WINDOW'S SPREAD. This series is NOISELESS,
        // so the honest answer is 0. The first cut reported the sample sd — 0.08 here — which is
        // the curve's own motion across 40 ms printed as if it were measurement error. Taking the
        // scatter about a LOCAL STRAIGHT LINE is what makes a clean ramp read 0.
        CHECK("extremum(max): sigma is 0 on a noiseless ramp (the curve's slope is not error)",
              mx.sigma <= 1e-9);

        // The mirror at the other end: sample 0's window holds 0, 8000, 16000 ⇒ (0+0.08+0.16)/3.
        const Reduced mn = reduceExtremum(s, 0, 792000, false, cfg);
        CHECK("extremum(min): the first sample's windowed mean, 0.08",
              mn.ok && near(mn.value, 0.08, 1e-9) && mn.atUs == 0 && mn.sigma <= 1e-9);

        // The sparse-region sample floor is INERT wherever the grid is dense: a ±20 ms window
        // already holds 5 samples at 8 ms (3 at the very ends), so widening never fires and every
        // 8 ms expectation in this file is the same with the floor at 1 as at 3.
        ReduceConfig one = cfg;
        one.minExtremumSamples = 1;
        const Reduced mxOne = reduceExtremum(s, 0, 792000, true, one);
        CHECK("extremum: minExtremumSamples does not change an 8 ms answer",
              mxOne.ok && mxOne.value == mx.value && mxOne.atUs == mx.atUs);

        // At. ±15 ms about 400000 admits 392000, 400000 and 408000 (384000 and 416000 are 16 ms
        // away), and the median of 3.92 / 4.00 / 4.08 is 4.00 — the ramp's value at the instant.
        const Reduced at = reduceAt(s, 400000, cfg);
        CHECK("at: median of the ±15 ms samples = 4.00", at.ok && near(at.value, 4.0, 1e-12));
        CHECK("at: atUs is the instant ASKED FOR, not the nearest sample", at.atUs == 400000);
        CHECK("at: a median carries no sigma here", at.sigma == 0.0);

        // Delta. Both ends are medians of three ramp samples ⇒ 6.00 − 2.00.
        const Reduced d = reduceDelta(s, 200000, 600000, cfg);
        CHECK("delta: At(600 ms) − At(200 ms) = 4.00",
              d.ok && near(d.value, 4.0, 1e-12) && d.atUs == 600000);
        // BACKWARDS is not an error: the caller chose the direction, so the sign is the answer and
        // atUs is still the instant it is reported AS OF.
        const Reduced back = reduceDelta(s, 600000, 200000, cfg);
        CHECK("delta: a backwards window is the negation, reported as of the earlier instant",
              back.ok && near(back.value, -4.0, 1e-12) && back.atUs == 200000);

        refMax   = mx.value;
        refMaxAt = mx.atUs;
        refRate  = rate.value;
    }

    // ── 1b) the same ramp descending — the sign survives every reducer ───────
    {
        std::printf("=== 1b) descending ramp ===\n");
        const Series r = makeRamp(8000, 100, -10.0);
        const SeriesView s = view(r);

        const Reduced rate = reduceRate(s, 0, 792000, cfg);
        // SIGNED, exactly: a rate reducer that returned magnitudes would leave no consumer able to
        // recover the direction, and "the hips moved 20 per 100 ms" is a different statement from
        // "the hips moved BACK 20 per 100 ms".
        CHECK("rate: a falling ramp is exactly −1.0 per 100 ms, not +1.0",
              rate.ok && near(rate.value, -1.0, 1e-9) && rate.atUs == 28000);
        const Reduced mn = reduceExtremum(s, 0, 792000, false, cfg);
        CHECK("extremum(min): the last sample's windowed mean, −7.84",
              mn.ok && near(mn.value, -7.84, 1e-9) && mn.atUs == 792000);
        const Reduced d = reduceDelta(s, 200000, 600000, cfg);
        CHECK("delta: −6.00 − (−2.00) = −4.00", d.ok && near(d.value, -4.0, 1e-12));
    }

    // ── 2) the same ramp with ONE spike of 99 ───────────────────────────────
    {
        std::printf("=== 2) ramp + one 99 at t=400000 (the design §2 failure) ===\n");
        Series r = makeRamp(8000, 100, 10.0);
        r.v[50] = 99.0;                              // sample 50 is t = 400000, ramp value 4.00
        const SeriesView s = view(r);

        // PEAK. Only 5 samples sit inside any ±20 ms window in the dense zone, and four of them
        // are ramp values ≤ 7.92, so NO windowed mean containing the spike can exceed
        // (4 × 7.92 + 99)/5 = 7.92 + 99/5 = 27.72. (It is in fact 23.16, the window centred on
        // sample 52 — the spike sits 16 ms from that centre and the four ramp samples around it are
        // the largest that can accompany it.) The raw argmax the card used to report was 99.
        const double bound = 7.92 + 99.0 / 5.0;
        const Reduced mx = reduceExtremum(s, 0, 792000, true, cfg);
        std::printf("      max = %.6f at %lld sigma %.4f (bound %.6f, raw argmax 99)\n", mx.value,
                    static_cast<long long>(mx.atUs), mx.sigma, bound);
        CHECK("extremum(max): a lone 99 cannot BE the peak — the 40 ms window divides it by the "
              "5 samples it holds at 8 ms spacing",
              mx.ok && mx.value <= bound + 1e-9);
        CHECK("extremum(max): and it is nowhere near the spike's own value", mx.value < 30.0);
        CHECK("extremum(max): the reported instant is within one half-window of the spike",
              std::llabs(static_cast<long long>(mx.atUs) - 400000LL) <= 20000LL);
        // The mean's sigma is now doing real work: the window's residuals about a local line are
        // enormous (one of five samples is 95 off it), so ≈ 15.5 comes back beside a value of 23 —
        // a peak the reader is TOLD not to trust, where the first cut printed the sample sd.
        CHECK("extremum(max): sigma says the mean is standing on a spike", mx.sigma > 1.0);

        // RATE, against the negative control. The adjacent-frame difference this header replaces
        // sees (99 − 3.92) over 8 ms and calls it 1188 per 100 ms.
        const double control = rawAdjacentRate(r);
        const Reduced rate = reduceRate(s, 0, 792000, cfg);
        std::printf("      rate = %.4f per 100 ms (sigma %.4f); adjacent-frame control = %.1f\n",
                    rate.value, rate.sigma, control);
        CHECK("control: the adjacent-frame rate on the SAME data is over 1000 per 100 ms — the "
              "spike is the thing being tamed",
              control > 1000.0);
        // The winning window is samples 43..50 (8 samples spanning 56 ms), where the spike sits at
        // the far end: dx = 0.028 s from the window's mean x, Sxx = 0.008² × 42 = 2.688e-3, so the
        // spike displaces the slope by 95.0 × 0.028 / 2.688e-3 = 989.6 per second = 98.96 per
        // 100 ms, on top of the ramp's 1.0. ⇒ 99.96, i.e. 11.9× smaller than the control.
        CHECK("rate: the 50 ms least-squares fit is an order of magnitude below the control",
              rate.ok && std::fabs(rate.value) < control / 8.0);
        CHECK("rate: and it is the derived 99.96 per 100 ms", near(rate.value, 99.96, 2.0));
        // ⚠ AND IT IS STILL NOT THE RAMP'S 1.0, WHICH IS THE HONEST LIMIT OF THIS REDUCER.
        // A least-squares slope is not robust: an outlier of magnitude A in a window of n samples
        // spanning T seconds moves the slope by about 6A/(nT), here 6 × 95 / (8 × 0.056) = 1272 per
        // second ≈ the 990 above. What makes the PRESENTATION honest is that the fit's own standard
        // error comes back the same order as the slope, which design §5.3 renders as "± σ" beside
        // PK RATE. sqrt(SSE/6)/sqrt(Sxx) works out at ≈ 57 per 100 ms against a value of ≈ 100.
        CHECK("rate: sigma flags the fit as untrustworthy (same order as the value)",
              rate.sigma > 0.25 * std::fabs(rate.value));
    }

    // ── 3) a still series with deterministic pseudo-noise ───────────────────
    {
        std::printf("=== 3) still series, 80 samples, 8 ms, sd 0.2 about 100 ===\n");
        // The design's definition of done for this phase: PK RATE over a still address window is
        // under 2 units per 100 ms. sd 0.2 is the corpus's own scale — 0.5° median frame-to-frame
        // jitter on hipLineTilt is a per-sample sd of about 0.35°.
        const double sd = 0.2;
        Lcg    rng;
        Series r;
        for (int i = 0; i < 80; ++i) {
            r.t.push_back(8000 * int64_t(i));
            r.v.push_back(100.0 + sd * rng.gauss());
        }
        const SeriesView s = view(r);

        const double  control = rawAdjacentRate(r);
        const Reduced rate    = reduceRate(s, 0, r.t.back(), cfg);
        std::printf("      rate = %.4f per 100 ms (sigma %.4f); adjacent-frame control = %.4f\n",
                    rate.value, rate.sigma, control);
        // sd(slope) = sd / sqrt(Sxx) = 0.2 / 0.0518 = 3.86 per second = 0.386 per 100 ms, and there
        // are ≈ 11 independent 56 ms windows in 632 ms, so the largest |slope| lands near 1 — under
        // the 2-per-100 ms gate with room. The control divides the same noise by 8 ms.
        CHECK("rate: a still series fits near zero — under 2 per 100 ms (the phase's DoD)",
              rate.ok && std::fabs(rate.value) < 2.0);
        CHECK("rate: and the adjacent-frame control on the same noise is several times worse",
              control > 5.0 * std::fabs(rate.value));

        double mean = 0.0, rawMax = 0.0;
        for (size_t i = 0; i < r.v.size(); ++i) {
            mean += r.v[i];
            if (i == 0 || r.v[i] > rawMax) rawMax = r.v[i];
        }
        mean /= double(r.v.size());
        const Reduced mx = reduceExtremum(s, 0, r.t.back(), true, cfg);
        std::printf("      max = %.6f (mean %.6f, %.2f sd away, sigma %.4f = %.2f sd); "
                    "raw max = %.6f (%.2f sd)\n",
                    mx.value, mean, (mx.value - mean) / sd, mx.sigma, mx.sigma / sd, rawMax,
                    (rawMax - mean) / sd);
        // A ±20 ms window holds 5 samples, so each windowed mean has sd 0.447 sd, and the largest
        // of ≈ 16 independent ones sits about 1.0 sd above the mean (measured: 1.07). 1.5 sd is
        // therefore the honest bound for a MAX over a whole series; 1.0 sd would be a coin flip.
        CHECK("extremum(max): within 1.5 sd of the mean on pure noise",
              mx.ok && (mx.value - mean) < 1.5 * sd);
        CHECK("extremum(max): strictly inside the raw maximum — the window pulled the peak in",
              mx.value < rawMax);
        // And HERE sigma is not 0: the residual scatter about the local line recovers the noise and
        // divides it by √k, so the reported uncertainty on a 5-sample mean is ≈ sd/√5 = 0.45 sd.
        CHECK("extremum(max): sigma recovers the noise on the mean (≈ sd/√5)",
              mx.sigma > 0.2 * sd && mx.sigma < 1.0 * sd);
    }

    // ── 4) the validity mask, and non-finite samples ────────────────────────
    {
        std::printf("=== 4) the 99 lands on an INVALID sample; then NaN / inf ===\n");
        Series r = makeRamp(8000, 100, 10.0);
        r.v[50] = 99.0;
        r.valid.assign(r.t.size(), 1u);
        r.valid[50] = 0u;                            // bridged, not measured
        const SeriesView s = view(r);

        // Not "reduced", IGNORED: the answers are §1's clean-ramp answers exactly.
        const Reduced mx = reduceExtremum(s, 0, 792000, true, cfg);
        CHECK("extremum(max): identical to the clean ramp's 7.84 — the spike never entered a mean",
              mx.ok && near(mx.value, refMax, 1e-12) && mx.atUs == refMaxAt);
        const Reduced rate = reduceRate(s, 0, 792000, cfg);
        CHECK("rate: identical to the clean ramp's 1.0 — the spike never entered a fit",
              rate.ok && near(rate.value, refRate, 1e-9) && near(rate.value, 1.0, 1e-9));
        // At the spike's OWN instant: ±15 ms admits 392000, 400000, 408000, but 400000 is invalid,
        // so the median is of 3.92 and 4.08 ⇒ 4.00. The bridged value is not consulted even where
        // it is the nearest sample there is.
        const Reduced at = reduceAt(s, 400000, cfg);
        CHECK("at: the median skips the invalid sample at its own instant (4.00, not 99)",
              at.ok && near(at.value, 4.0, 1e-12));
        // And a window with nothing valid in it has no value at all.
        Series q;
        q.t = { 0, 100000, 200000 };
        q.v = { 1.0, 2.0, 3.0 };
        q.valid = { 1u, 0u, 1u };
        CHECK("at: a window whose only sample is invalid is ok=false, not zero",
              !reduceAt(view(q), 100000, cfg).ok);

        // NON-FINITE IS THE SAME STATEMENT AS INVALID, and it has to be, for a reason that is not
        // tidiness: a NaN reaching std::nth_element is undefined behaviour, and one reaching a mean
        // or a fit poisons the whole window and comes back as a confident `ok = true` holding NaN.
        // Nothing upstream should produce one; that is exactly why it is not assumed.
        Series nf = makeRamp(8000, 100, 10.0);
        nf.v[50] = std::numeric_limits<double>::quiet_NaN();
        nf.v[60] = std::numeric_limits<double>::infinity();
        const SeriesView ns = view(nf);
        const Reduced nmx  = reduceExtremum(ns, 0, 792000, true, cfg);
        const Reduced nrat = reduceRate(ns, 0, 792000, cfg);
        const Reduced nat  = reduceAt(ns, 400000, cfg);
        CHECK("non-finite: a NaN and an inf are ignored exactly as invalid samples are",
              nmx.ok && near(nmx.value, refMax, 1e-12) && nmx.atUs == refMaxAt
                  && nrat.ok && near(nrat.value, refRate, 1e-9)
                  && nat.ok && near(nat.value, 4.0, 1e-12));
        CHECK("non-finite: and no reduction returns a NaN of its own",
              std::isfinite(nmx.value) && std::isfinite(nmx.sigma) && std::isfinite(nrat.value)
                  && std::isfinite(nrat.sigma) && std::isfinite(nat.value));
    }

    // ── 5) sparse spacing: the widening, the tie rule, and the two rate gates ─
    {
        std::printf("=== 5) sparse 27 ms spacing ===\n");
        // 27 ms is PoseRunner's sparseStride at 150 fps — outside the dense zone this is the real
        // spacing, and a ±20 ms centred window holds exactly ONE sample there.
        const Series r = makeRamp(27000, 20, 10.0);      // v = 0.27 i, 0.00 .. 5.13
        const SeriesView s = view(r);

        const Reduced rate = reduceRate(s, 0, r.t.back(), cfg);
        std::printf("      sparse rate = %.12f per 100 ms, atUs %lld\n", rate.value,
                    static_cast<long long>(rate.atUs));
        // The span extension is what makes this answerable: the samples INSIDE 50 ms are 0 and
        // 27000, spanning 27 ms, so the window takes 54000 as well — 3 samples over 54 ms.
        CHECK("rate: a 27 ms series still finds windows (span extension) and reports the slope",
              rate.ok && near(rate.value, 1.0, 1e-9) && rate.sigma <= 1e-6);
        CHECK("rate: ties go to the earliest window — centre of {0, 27000, 54000} is 27000",
              rate.atUs == 27000);

        // ⚠ THE FAILURE THE CORPUS PROBE FOUND. With a single-sample window the "windowed mean" is
        // the sample, so the extremum was the raw argmax again — 5.13 at the last sample, sigma
        // 0.000 — across the whole address and backswing. Widening to 3 samples makes the last
        // anchor's window {17, 18, 19} = (4.59 + 4.86 + 5.13)/3 = 4.86, and anchor 18's window is
        // the SAME three samples, so the two tie and the earliest wins: 486000, not 513000.
        const Reduced mx = reduceExtremum(s, 0, r.t.back(), true, cfg);
        ReduceConfig one = cfg;
        one.minExtremumSamples = 1;
        const Reduced mxOne = reduceExtremum(s, 0, r.t.back(), true, one);
        std::printf("      max widened = %.6f at %lld; single-sample window = %.6f at %lld\n",
                    mx.value, static_cast<long long>(mx.atUs), mxOne.value,
                    static_cast<long long>(mxOne.atUs));
        CHECK("extremum(max): the window widens to 3 samples and returns 4.86, not the sample 5.13",
              mx.ok && near(mx.value, 4.86, 1e-9) && mx.atUs == 486000);
        CHECK("extremum(max): with the floor at 1 it degenerates to the raw argmax (the old bug)",
              mxOne.ok && near(mxOne.value, 5.13, 1e-9) && mxOne.atUs == 513000);

        // The same thing said in sigma, which is how the probe saw it: a one-sample window cannot
        // have a residual, so peakSigma printed 0.000 on every still-address row.
        Lcg rng;
        Series still;
        for (int i = 0; i < 20; ++i) {
            still.t.push_back(27000 * int64_t(i));
            still.v.push_back(100.0 + 0.2 * rng.gauss());
        }
        const Reduced w3 = reduceExtremum(view(still), 0, still.t.back(), true, cfg);
        const Reduced w1 = reduceExtremum(view(still), 0, still.t.back(), true, one);
        std::printf("      still sparse: sigma widened %.6f, single-sample %.6f\n",
                    w3.sigma, w1.sigma);
        CHECK("extremum: a widened window carries a sigma on a noisy sparse still",
              w3.ok && w3.sigma > 0.0);
        CHECK("extremum: a single-sample window cannot, which is the probe's 0.000 tell",
              w1.ok && w1.sigma == 0.0);

        // Mixed spacing, which is what a real grid IS: sparse address, dense through impact.
        Series mixed;
        for (int i = 0; i < 10; ++i) mixed.t.push_back(27000 * int64_t(i));        // 0..243000
        for (int k = 1; k <= 20; ++k) mixed.t.push_back(243000 + 8000 * int64_t(k)); // ..403000
        for (int64_t t : mixed.t) mixed.v.push_back(10.0 * (double(t) * 1.0e-6));
        const Reduced mMax = reduceExtremum(view(mixed), 0, mixed.t.back(), true, cfg);
        const Reduced mMin = reduceExtremum(view(mixed), 0, mixed.t.back(), false, cfg);
        // Dense end: the last three samples (387/395/403 ms) ⇒ (3.87+3.95+4.03)/3 = 3.95.
        // Sparse end: the first three (0/27/54 ms) ⇒ (0.00+0.27+0.54)/3 = 0.27.
        CHECK("extremum: mixed 8→27 ms spacing works at both ends",
              mMax.ok && near(mMax.value, 3.95, 1e-9) && mMax.atUs == 403000
                  && mMin.ok && near(mMin.value, 0.27, 1e-9) && mMin.atUs == 0);

        // Two samples 60 ms apart: the SPAN is long enough, the sample count is not. A line through
        // two points has no residual and so no standard error to be honest with.
        Series two;
        two.t = { 0, 60000 };
        two.v = { 0.0, 0.6 };
        CHECK("rate: a 2-sample series is ok=false (minRateSamples), never a slope through 2 points",
              !reduceRate(view(two), 0, 60000, cfg).ok);
        // And the floor is 3 IN CODE, not just by default: a config asking for two still gets none.
        ReduceConfig two2 = cfg;
        two2.minRateSamples = 2;
        CHECK("rate: minRateSamples cannot be configured below 3 (a 2-point fit's sigma is 0)",
              !reduceRate(view(two), 0, 60000, two2).ok);

        // Three samples over 16 ms: the count is enough, the TIME BASE is not — this is the
        // adjacent-frame failure the window exists to refuse.
        Series brief;
        brief.t = { 0, 8000, 16000 };
        brief.v = { 0.0, 0.08, 0.16 };
        CHECK("rate: a 16 ms series is ok=false (span gate), whatever its sample count",
              !reduceRate(view(brief), 0, 16000, cfg).ok);
    }

    // ── 6) absence: nothing in ±15 ms, and a delta with one end missing ─────
    {
        std::printf("=== 6) no sample near the instant ===\n");
        Series r;
        r.t = { 0, 100000, 200000 };
        r.v = { 1.0, 2.0, 3.0 };
        const SeriesView s = view(r);

        CHECK("at: no valid sample within ±15 ms ⇒ ok=false", !reduceAt(s, 50000, cfg).ok);
        CHECK("at: a sample AT the instant ⇒ ok, value 2.0",
              reduceAt(s, 100000, cfg).ok && near(reduceAt(s, 100000, cfg).value, 2.0, 1e-12));
        CHECK("delta: one end missing ⇒ ok=false (not a smaller delta)",
              !reduceDelta(s, 0, 50000, cfg).ok && !reduceDelta(s, 50000, 200000, cfg).ok);
        const Reduced d = reduceDelta(s, 0, 100000, cfg);
        CHECK("delta: both ends present ⇒ 2.0 − 1.0, reported as of the LATER instant",
              d.ok && near(d.value, 1.0, 1e-12) && d.atUs == 100000);
        CHECK("extremum: an empty window (no sample in range) ⇒ ok=false",
              !reduceExtremum(s, 400000, 500000, true, cfg).ok);
    }

    // ── 7) viewOf and the short-mask rule ──────────────────────────────────
    {
        std::printf("=== 7) viewOf ===\n");
        MetricSeries m;
        m.key = QStringLiteral("test");
        for (int i = 0; i < 10; ++i) {
            m.t_us.push_back(8000 * int64_t(i));
            m.value.push_back(double(i));
        }

        CHECK("viewOf: an EMPTY mask is no mask — every sample valid",
              viewOf(m).valid == nullptr && viewOf(m).n == 10);

        m.valid.assign(4, 1u);                       // shorter than the curve: malformed
        const SeriesView shortMask = viewOf(m);
        CHECK("viewOf: a mask SHORTER than t_us is discarded wholesale (the short-mask rule), "
              "never applied to the first 4 samples",
              shortMask.valid == nullptr && shortMask.n == 10 && shortMask.isValid(9));

        m.valid.assign(10, 1u);
        m.valid[3] = 0u;
        const SeriesView full = viewOf(m);
        CHECK("viewOf: a full-length mask is honoured",
              full.valid != nullptr && !full.isValid(3) && full.isValid(4));

        MetricSeries ragged;
        ragged.t_us = { 0, 1, 2, 3 };
        ragged.value = { 0.0, 1.0 };
        CHECK("viewOf: n is the shorter of t_us / value", viewOf(ragged).n == 2);

        MetricSeries empty;
        CHECK("viewOf: an empty series has n=0 and reduces to nothing",
              viewOf(empty).n == 0 && !reduceAt(viewOf(empty), 0, cfg).ok
                  && !reduceExtremum(viewOf(empty), 0, 1000, true, cfg).ok
                  && !reduceRate(viewOf(empty), 0, 1000, cfg).ok);
    }

    // ── 8) [from, to] bounds the CANDIDATES, and the union invariant ────────
    {
        std::printf("=== 8) strict subintervals: the support is query-independent ===\n");
        // A rise-to-impact-then-reverse curve, 8 ms spacing: a steep approach (1.2 per sample),
        // then the P6→P7 stretch at 0.4 per sample (18.4 → 20.0 over 48000..80000), then the
        // post-impact reversal at −0.8.
        Series r;
        for (int i = 0; i < 17; ++i) {
            r.t.push_back(8000 * int64_t(i));
            if (i <= 6)       r.v.push_back(11.2 + 1.2 * i);        // .. 18.4 at i = 6
            else if (i <= 10) r.v.push_back(18.4 + 0.4 * (i - 6));  // 18.8 19.2 19.6 20.0
            else              r.v.push_back(20.0 - 0.8 * (i - 10)); // the reversal
        }
        const SeriesView s = view(r);

        // ⚠ THE CONTRACT UNDER TEST IS CACHE AGREEMENT, NOT "the extreme is a sample in the span".
        // The engine caches an extreme per (lo, hi] span and the card reduces a whole window, so a
        // sample's windowed mean has to be the same number whoever asked — the support is the
        // anchor's own ±20 ms neighbourhood and is NOT clipped to the query. W2 measured the
        // alternative on rich_7iron: clipping the support made the span cache and the whole-window
        // reduction disagree on 20 of 514 authored extremum measures, against 0 of 514 unclipped.
        //
        // The price, stated so nobody rediscovers it as a bug: over [48000, 80000] the minimum is
        // 17.92 — the mean at anchor 48000 of samples 32000..64000, which reaches back into the
        // steeper approach BEFORE the span, and is a value the curve never had inside it. That is a
        // phase-DOMAIN leak, and it is closed where a domain lives: the producers mark every sample
        // outside a metric's domain INVALID (lower_body_metrics.cpp / upper_body_metrics.cpp), so
        // there is nothing out there for a support to draw on. Clipping here would have hidden the
        // leak for spans and left it open for every whole-window card.
        const Reduced mn = reduceExtremum(s, 48000, 80000, false, cfg);
        const Reduced mx = reduceExtremum(s, 48000, 80000, true, cfg);
        std::printf("      [48000,80000] min %.6f at %lld, max %.6f at %lld\n",
                    mn.value, static_cast<long long>(mn.atUs), mx.value,
                    static_cast<long long>(mx.atUs));
        // min: anchor 48000, support 32000..64000 ⇒ (16.0+17.2+18.4+18.8+19.2)/5 = 17.92.
        // max: anchor 72000, support 56000..88000 — which reaches PAST the span and picks up the
        //      reversal's first sample (88000 ⇒ 19.2) ⇒ (18.8+19.2+19.6+20.0+19.2)/5 = 19.36.
        CHECK("extremum: the candidates are clamped to the query — the winner is inside it",
              mn.ok && mx.ok && mn.atUs >= 48000 && mn.atUs <= 80000
                  && mx.atUs >= 48000 && mx.atUs <= 80000);
        CHECK("extremum: min = 17.92 at 48000, max = 19.36 at 72000 (support unclipped)",
              near(mn.value, 17.92, 1e-9) && mn.atUs == 48000
                  && near(mx.value, 19.36, 1e-9) && mx.atUs == 72000);

        // ⚠ THE INVARIANT THE SPAN CACHE ACTUALLY NEEDS: the extreme over a UNION of adjacent
        // half-open spans is the extreme OF their extremes. Aggregate two cached spans or reduce
        // their union in one call and you get the same number and the same instant — which is what
        // makes a card and a corridor comparable at all.
        const Reduced aMin = reduceExtremum(s, 24001, 48000, false, cfg);
        const Reduced aMax = reduceExtremum(s, 24001, 48000, true,  cfg);
        const Reduced bMin = reduceExtremum(s, 48001, 80000, false, cfg);
        const Reduced bMax = reduceExtremum(s, 48001, 80000, true,  cfg);
        const Reduced uMin = reduceExtremum(s, 24001, 80000, false, cfg);
        const Reduced uMax = reduceExtremum(s, 24001, 80000, true,  cfg);
        std::printf("      A max %.6f@%lld  B max %.6f@%lld  union max %.6f@%lld\n",
                    aMax.value, static_cast<long long>(aMax.atUs), bMax.value,
                    static_cast<long long>(bMax.atUs), uMax.value,
                    static_cast<long long>(uMax.atUs));
        // A = (24001,48000] ⇒ min 16.00 @32000, max 17.92 @48000.
        // B = (48001,80000] ⇒ min 18.64 @56000, max 19.36 @72000.
        // Union                ⇒ min 16.00 @32000, max 19.36 @72000 — B's max, A's min, BIT-EQUAL.
        CHECK("extremum: max over the union is exactly the larger of the two spans' maxima",
              uMax.ok && aMax.ok && bMax.ok && uMax.value == bMax.value
                  && uMax.atUs == bMax.atUs && bMax.value > aMax.value);
        CHECK("extremum: min over the union is exactly the smaller of the two spans' minima",
              uMin.ok && aMin.ok && bMin.ok && uMin.value == aMin.value
                  && uMin.atUs == aMin.atUs && aMin.value < bMin.value);
        CHECK("extremum: and those spans' own numbers are the derived ones",
              near(aMax.value, 17.92, 1e-9) && near(aMin.value, 16.0, 1e-9)
                  && near(bMax.value, 19.36, 1e-9) && near(bMin.value, 18.64, 1e-9));
    }

    // ── 9) coincident timestamps: tolerated, and double-weighted ───────────
    {
        std::printf("=== 9) two samples at one instant ===\n");
        // No producer emits these — the grid is a set of instants — but a reducer that walks a
        // window has to be defined on them. They are DOUBLE-WEIGHTED: both copies enter the median,
        // the mean and the fit, which is the right reading of two measurements sharing a clock tick
        // and the wrong reading of a duplicated row. Stated, not defended against.
        Series dup;
        dup.t = { 0, 8000, 8000 };
        dup.v = { 1.0, 5.0, 5.0 };
        // The median of {1, 5, 5} is 5.0; without the duplicate it would be the even-count average
        // of {1, 5} = 3.0. That difference IS the double weight.
        const Reduced at = reduceAt(view(dup), 8000, cfg);
        CHECK("at: a coincident pair is weighted twice (5.0, where one copy gives 3.0)",
              at.ok && near(at.value, 5.0, 1e-12));

        // And a fit over a window containing the pair is finite and weights it twice: the same
        // three distinct instants without the duplicate fit 8.401 per 100 ms, with it 7.764.
        Series dr;
        dr.t = { 0, 8000, 8000, 56000 };
        dr.v = { 0.0, 3.0, 3.0, 5.6 };
        const Reduced rate = reduceRate(view(dr), 0, 56000, cfg);
        std::printf("      dup-weighted rate = %.9f (single copy would be 8.401162790698)\n",
                    rate.value);
        CHECK("rate: coincident timestamps do not divide by zero, and the pair pulls the fit",
              rate.ok && near(rate.value, 7.764227642276, 1e-9));
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
