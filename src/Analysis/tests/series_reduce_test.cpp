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
// ⚠ EVERY EXPECTED NUMBER IS DERIVED IN A COMMENT BESIDE IT, not observed from a
// run. If one of these fails, read the derivation before changing the number: the
// window arithmetic is the thing under test.

#include "../series_reduce.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

    const ReduceConfig cfg;                          // the frozen windows: ±15 / 40 / 50 ms, 3
    CHECK("frozen windows are the ones design §5.2 states",
          cfg.atHalfWindowUs == 15000 && cfg.extremumWindowUs == 40000
              && cfg.rateWindowUs == 50000 && cfg.minRateSamples == 3);

    // Kept from §1 so §4 can assert the masked spike gives the CLEAN answer exactly, rather than
    // re-deriving it and risking two different arithmetics.
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
        CHECK("rate: atUs is the winning window's centre, inside the window",
              rate.atUs >= 0 && rate.atUs <= 792000);

        // Extremum (max). The window is ±20 ms, so the LAST sample's window holds samples 97, 98
        // and 99 only (776000, 784000, 792000 — 772000 is not a sample): mean = (7.76+7.84+7.92)/3
        // = 7.84, which is above sample 98's 4-sample mean of 7.80 and every earlier one. This is
        // the whole design in one number: the largest windowed mean of a rising ramp is NOT its
        // largest sample.
        const Reduced mx = reduceExtremum(s, 0, 792000, true, cfg);
        std::printf("      max = %.12f at %lld (sigma %.6f)\n", mx.value,
                    static_cast<long long>(mx.atUs), mx.sigma);
        CHECK("extremum(max): the last sample's windowed mean, 7.84 (not its value 7.92)",
              mx.ok && near(mx.value, 7.84, 1e-9));
        // §h — the reported instant is the CENTRE sample's own time, and sigma is that window's
        // sample sd: deviations ±0.08 about 7.84 over 3 samples ⇒ sqrt((0.0064+0+0.0064)/2) = 0.08.
        CHECK("extremum(max): atUs is the centre sample's time", mx.atUs == 792000);
        CHECK("extremum(max): sigma is the window's sample sd (0.08)", near(mx.sigma, 0.08, 1e-9));

        // The mirror at the other end: sample 0's window holds 0, 8000, 16000 ⇒ (0+0.08+0.16)/3.
        const Reduced mn = reduceExtremum(s, 0, 792000, false, cfg);
        CHECK("extremum(min): the first sample's windowed mean, 0.08",
              mn.ok && near(mn.value, 0.08, 1e-9) && mn.atUs == 0);

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

        refMax   = mx.value;
        refMaxAt = mx.atUs;
        refRate  = rate.value;
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
        std::printf("      max = %.6f at %lld (bound %.6f, raw argmax 99)\n", mx.value,
                    static_cast<long long>(mx.atUs), bound);
        CHECK("extremum(max): a lone 99 cannot BE the peak — the 40 ms window divides it by the "
              "5 samples it holds at 8 ms spacing",
              mx.ok && mx.value <= bound + 1e-9);
        CHECK("extremum(max): and it is nowhere near the spike's own value", mx.value < 30.0);
        CHECK("extremum(max): the reported instant is within one half-window of the spike",
              std::llabs(static_cast<long long>(mx.atUs) - 400000LL) <= 20000LL);

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
        std::printf("      max = %.6f (mean %.6f, %.2f sd away); raw max = %.6f (%.2f sd)\n",
                    mx.value, mean, (mx.value - mean) / sd, rawMax, (rawMax - mean) / sd);
        // A ±20 ms window holds 5 samples, so each windowed mean has sd 0.447 sd, and the largest
        // of ≈ 16 independent ones sits about 1.0 sd above the mean. 1.5 sd is therefore the honest
        // bound for a MAX over a whole series; 1.0 sd would be a coin flip on the seed.
        CHECK("extremum(max): within 1.5 sd of the mean on pure noise",
              mx.ok && (mx.value - mean) < 1.5 * sd);
        CHECK("extremum(max): strictly inside the raw maximum — the window pulled the peak in",
              mx.value < rawMax);
    }

    // ── 4) the validity mask: an invalid sample enters NOTHING ──────────────
    {
        std::printf("=== 4) the 99 lands on an INVALID sample ===\n");
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
    }

    // ── 5) sparse spacing, and the two rate gates ──────────────────────────
    {
        std::printf("=== 5) sparse 27 ms spacing, and the sample/span gates ===\n");
        // 27 ms is PoseRunner's sparseStride at 150 fps — outside the dense zone this is the real
        // spacing, and a 50 ms window holds only two samples there. The span extension (to the
        // first sample at or beyond t_i + 50 ms) is what makes it answerable: 0, 27000, 54000 is
        // 3 samples over 54 ms.
        const Series r = makeRamp(27000, 20, 10.0);
        const Reduced rate = reduceRate(view(r), 0, r.t.back(), cfg);
        std::printf("      sparse rate = %.12f per 100 ms\n", rate.value);
        CHECK("rate: a 27 ms series still finds windows (span extension) and reports the slope",
              rate.ok && near(rate.value, 1.0, 1e-9) && rate.sigma <= 1e-6);

        // Two samples 60 ms apart: the SPAN is long enough, the sample count is not. A line through
        // two points has no residual and so no standard error to be honest with.
        Series two;
        two.t = { 0, 60000 };
        two.v = { 0.0, 0.6 };
        CHECK("rate: a 2-sample series is ok=false (minRateSamples), never a slope through 2 points",
              !reduceRate(view(two), 0, 60000, cfg).ok);

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

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
