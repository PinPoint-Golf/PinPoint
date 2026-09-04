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

// The four reductions every consumer of a metric curve needs, written ONCE.
//
// Design: docs/design/metric_presentation_honesty.md §5.2 (and §4 principle 4 — "reducers are
// robust by definition, not by tuning").
//
// ⚠ THE POINT IS THAT THE CARD AND THE ENGINE CANNOT DISAGREE. Before this header the review
// chart's summary (Gui/review/chart_metrics.cpp) and the diagnostics phase grid
// (Diagnostics/measure_sample.cpp) each carried their own arithmetic for the same words. The
// engine's `At` was already a ±15 ms windowed median; the chart's was a linear interpolation at
// one instant. The engine's `Extremum` was the raw min/max of the samples in a span; the chart's
// PEAK was the raw argmax of |value| over the window. Both raw extremes return the largest
// OUTLIER by definition, which on the 2026-08-18 corpus swing is how a summary card came to report
// a sway peak of 34 % (the curve settles at 12 %) and a PK RATE of 39 %/100 ms from half a percent
// of frame-to-frame jitter divided by 8 ms.
//
// So the definitions here are deliberately stated in TIME, not in samples:
//
//   * At        — the median of the valid samples within ±15 ms. Unchanged from the convention
//                 measure_sample.cpp has always used; the chart adopts it.
//   * Delta     — At(to) − At(from). Both ends robust, meaning unchanged.
//   * Extremum  — the extremum of the CENTRED-WINDOW MEAN. A one-sample outlier cannot be the peak
//                 because a peak has to be there for `extremumWindowUs`.
//   * Rate      — the largest-magnitude LEAST-SQUARES slope over any sliding window of at least
//                 `rateWindowUs` with at least `minRateSamples` valid samples. Never an
//                 adjacent-frame difference: at 8 ms spacing that divides the jitter by 8 ms and
//                 calls the result a rate.
//
// ⚠ A LEAST-SQUARES SLOPE IS NOT A ROBUST ESTIMATOR, and the header says so where a reader will
// look. It removes the "noise ÷ 8 ms" failure completely — that is the failure the design is about,
// and on white noise the fitted slope is near zero where the adjacent-frame rate is an order of
// magnitude larger (pinned by series_reduce_test §3) — but a single LARGE outlier still moves it.
// For a window of n samples spanning T seconds an outlier of magnitude A displaces the slope by
// roughly 6A/(nT) per second, so at 8 ms spacing over 50 ms (n ≈ 8, T ≈ 0.056 s) a 99-unit spike on
// an 8-unit ramp still reads ≈ 72 per 100 ms against the ramp's 1.0 — 16× better than the
// adjacent-frame 1188, and nowhere near the truth. What saves the presentation there is `sigma`:
// the fit's own standard error comes back the same order as the slope (≈ 56), which is exactly the
// signal design §5.3 renders as "± σ" beside PK RATE. If a spike-PROOF rate is ever needed the
// answer is a robust regression (Theil–Sen over the window's pairwise slopes) or a residual gate,
// not a wider window; that is a design decision and is NOT taken here.
//
// Valid-aware throughout: a sample marked 0 in MetricSeries::valid was BRIDGED across a gated or
// absent run (metric_channel.h channelValidityMask) and is a line the producer drew, not a
// measurement. It may not pull a median, be an extremum, or sit in a fit. Same rule in all four.
//
// Qt-only and Gui-free: the reductions themselves touch no Qt type at all — they run on borrowed
// C arrays, so a tool or a test can point one at anything — and only viewOf() needs MetricSeries
// (whose key/label are QString). Header-only, so no consumer needs a link-time addition.

#include "swing_analysis.h"                // MetricSeries
#include "../Core/pp_tuned_constants.h"    // tuned::sampler:: / tuned::reduce::

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pinpoint::analysis {

// The windows. Every consumer builds one of these from the frozen constants; there is no runtime
// override plumbing in this phase (docs/validation/tunable_parameters_reference.md §2.17 records
// the keys the sweep will use when there is).
struct ReduceConfig {
    int64_t atHalfWindowUs   = tuned::sampler::kWindowHalfUs;      // ±15 ms, the existing convention
    // How many valid samples an At window needs. 1 — a single measurement at the instant IS the
    // measurement. It is configurable only because PhaseGridConfig::minValidSamples already exists
    // and the engine has to be able to delegate WITHOUT changing its answer.
    int     minAtSamples     = tuned::sampler::kMinValidSamples;
    int64_t extremumWindowUs = tuned::reduce::kExtremumWindowUs;   // 40 ms, CENTRED (±20 ms)
    int64_t rateWindowUs     = tuned::reduce::kRateWindowUs;       // 50 ms minimum time base
    int     minRateSamples   = tuned::reduce::kMinRateSamples;     // 3
};

// A borrowed view of one series: no ownership, no copy, no Qt.
//
// `valid` may be null, and null means EVERY SAMPLE IS VALID — the same statement MetricSeries's
// empty mask makes. THE CALLER APPLIES THE SHORT-MASK RULE: a mask shorter than the curve is a
// malformed document rather than a partial statement, and is treated as no mask at all (guessing
// which end it was truncated from would invent validity we were never told about). viewOf() below
// applies it; a caller assembling a view from QVariantLists must do the same, which is why
// chart_metrics.cpp and measure_sample.cpp each have exactly one place that decides it.
struct SeriesView {
    const int64_t *t     = nullptr;   // ASCENDING, absolute µs
    const double  *v     = nullptr;
    const uint8_t *valid = nullptr;   // null ⇒ all valid
    size_t         n     = 0;

    bool isValid(size_t i) const { return !valid || valid[i] != 0; }
};

// The one view constructor that knows about MetricSeries — and the one place the short-mask rule is
// applied for it. `n` is the shorter of t_us / value, defensively: a curve whose two arrays
// disagree has only as many samples as both of them carry.
inline SeriesView viewOf(const MetricSeries &m)
{
    SeriesView s;
    s.n = std::min(m.t_us.size(), m.value.size());
    if (s.n == 0)
        return s;
    s.t = m.t_us.data();
    s.v = m.value.data();
    if (m.valid.size() >= s.n)
        s.valid = m.valid.data();      // empty (the common case) or short ⇒ stays null: all valid
    return s;
}

// One reduction's answer. `ok` false means THE CURVE HAS NOTHING TO SAY HERE — not zero. Every
// caller must branch on it rather than printing `value`, which is the whole of design §4
// principle 2 at this layer.
struct Reduced {
    double  value = 0.0;
    int64_t atUs  = 0;      // where the answer was taken (see each reducer for what it means)
    double  sigma = 0.0;    // 1σ on `value` where the reduction has one, else 0
    bool    ok    = false;
};

namespace detail {

// Median of a window, order-independent. Character-for-character the convention
// measure_sample.cpp's medianOf uses (nth_element, the two-element average on an even count), so
// the engine's phase values do not move by a rounding when it delegates.
inline double medianOfWindow(std::vector<double> w)
{
    const std::size_t n   = w.size();
    const std::size_t mid = n / 2;
    std::nth_element(w.begin(), w.begin() + mid, w.end());
    const double hi = w[mid];
    if (n % 2 == 1)
        return hi;
    std::nth_element(w.begin(), w.begin() + (mid - 1), w.end());
    return 0.5 * (w[mid - 1] + hi);
}

inline int64_t absUs(int64_t d) { return d < 0 ? -d : d; }

} // namespace detail

// ── At ──────────────────────────────────────────────────────────────────────
// Median of the VALID samples within ±atHalfWindowUs of `us`, INCLUSIVE at the edge (`> window`
// skips, exactly as measure_sample.cpp has always written it). atUs = us, because the answer is
// stated AT the instant asked for and not at whichever sample happened to be nearest. sigma = 0: a
// median carries none here, and inventing one from the window's spread would report the curve's
// motion as measurement error.
inline Reduced reduceAt(const SeriesView &s, int64_t us, const ReduceConfig &cfg = {})
{
    Reduced r;
    r.atUs = us;

    std::vector<double> win;
    for (std::size_t i = 0; i < s.n; ++i) {
        if (detail::absUs(s.t[i] - us) > cfg.atHalfWindowUs)
            continue;
        if (!s.isValid(i))
            continue;              // bridged, not measured — it may not pull the median
        win.push_back(s.v[i]);
    }
    if (static_cast<int>(win.size()) < std::max(1, cfg.minAtSamples))
        return r;                  // ok stays false: no value here, which is not the same as zero

    r.value = detail::medianOfWindow(std::move(win));
    r.ok    = true;
    return r;
}

// ── Delta ───────────────────────────────────────────────────────────────────
// At(to) − At(from), and ok ONLY when both ends are. A delta with one end missing is not a smaller
// delta, it is no delta. atUs = toUs (the instant the change is reported AS OF).
inline Reduced reduceDelta(const SeriesView &s, int64_t fromUs, int64_t toUs,
                           const ReduceConfig &cfg = {})
{
    const Reduced a = reduceAt(s, fromUs, cfg);
    const Reduced b = reduceAt(s, toUs, cfg);

    Reduced r;
    r.atUs = toUs;
    if (!a.ok || !b.ok)
        return r;
    r.value = b.value - a.value;
    r.ok    = true;
    return r;
}

// ── Extremum ────────────────────────────────────────────────────────────────
// The extremum (max if wantMax, else min) of the CENTRED-WINDOW MEAN over [fromUs, toUs].
//
// For every valid sample i whose t lies in the window, the mean of the valid samples within
// ±extremumWindowUs/2 of t_i — always at least one, itself. `value` is the winning mean, `atUs` the
// winner's OWN time (the centre of its window, which is what makes the reported instant the middle
// of the excursion rather than one edge of it), `sigma` the sample standard deviation of that
// window about the mean (0 with fewer than two samples).
//
// ⚠ THE NEIGHBOURS ARE NOT CLAMPED TO [from, to]. A sample 15 ms outside the window still enters
// the mean of a window-edge sample, deliberately: the 40 ms is that reading's own support, and
// truncating it at the edge would make the same instant reduce differently depending on where the
// caller cut its window. It does mean an Extremum taken right up to a phase domain's edge borrows
// up to 20 ms of samples from outside the domain; if that is ever shown to matter, clamp here and
// nowhere else.
inline Reduced reduceExtremum(const SeriesView &s, int64_t fromUs, int64_t toUs, bool wantMax,
                              const ReduceConfig &cfg = {})
{
    Reduced r;
    if (s.n == 0)
        return r;

    const int64_t half = cfg.extremumWindowUs / 2;
    std::size_t   lo   = 0;        // walks forward with i: t is ascending, so the window is too

    for (std::size_t i = 0; i < s.n; ++i) {
        if (!s.isValid(i) || s.t[i] < fromUs || s.t[i] > toUs)
            continue;
        while (lo < i && s.t[i] - s.t[lo] > half)
            ++lo;

        double      sum = 0.0;
        std::size_t cnt = 0;
        std::size_t end = lo;
        for (std::size_t j = lo; j < s.n && s.t[j] - s.t[i] <= half; ++j) {
            if (!s.isValid(j))
                continue;
            sum += s.v[j];
            ++cnt;
            end = j;
        }
        // cnt >= 1 always: sample i is valid and is inside its own window.
        const double mean = sum / static_cast<double>(cnt);
        if (r.ok && !(wantMax ? mean > r.value : mean < r.value))
            continue;

        double ss = 0.0;
        for (std::size_t j = lo; j <= end; ++j) {
            if (!s.isValid(j))
                continue;
            const double d = s.v[j] - mean;
            ss += d * d;
        }
        r.value = mean;
        r.atUs  = s.t[i];
        r.sigma = (cnt >= 2) ? std::sqrt(ss / static_cast<double>(cnt - 1)) : 0.0;
        r.ok    = true;
    }
    return r;
}

// ── Rate ────────────────────────────────────────────────────────────────────
// The signed slope of largest MAGNITUDE over sliding windows, reported PER 100 ms (the chart's
// existing unit, kept so the number on the card means what it always meant).
//
// The window anchored at valid sample i is the valid samples in [t_i, t_i + rateWindowUs],
// EXTENDED to the first valid sample at or beyond t_i + rateWindowUs when one exists at or before
// toUs. The extension is what makes a sparse region answerable at all: at the 27 ms spacing outside
// the dense pose zone the samples inside a 50 ms window span only 27 ms, and fitting a slope to
// that would report a 50 ms rate taken over half the time base. A window still needs
// minRateSamples valid samples AND a span of at least rateWindowUs, else it is skipped — which is
// why a 2-sample series, or an address posed at 100 ms strides, returns ok = false rather than a
// slope through two points.
//
// atUs = the winning window's CENTRE (a slope is a statement about an interval, and its midpoint is
// the least misleading single instant to hang it on). sigma = that slope's standard error,
// sqrt(SSE/(n−2)) / sqrt(Sxx), also per 100 ms — 0 for a two-point window or an exact fit, where
// the fit has no residual to speak from.
inline Reduced reduceRate(const SeriesView &s, int64_t fromUs, int64_t toUs,
                          const ReduceConfig &cfg = {})
{
    Reduced r;
    // A slope needs two points whatever the config says.
    const int minN = std::max(2, cfg.minRateSamples);

    for (std::size_t i = 0; i < s.n; ++i) {
        if (!s.isValid(i) || s.t[i] < fromUs || s.t[i] > toUs)
            continue;

        // Pass 1 — the window's extent and its means. x is SECONDS FROM t_i, never raw µs: a
        // least-squares fit in absolute microseconds subtracts two ~1e11 quantities to recover a
        // 1e-2 one and loses most of the mantissa doing it.
        std::size_t cnt  = 0;
        std::size_t end  = i;
        double      sx   = 0.0;
        double      sy   = 0.0;
        for (std::size_t j = i; j < s.n; ++j) {
            if (!s.isValid(j))
                continue;
            if (s.t[j] > toUs)
                break;
            sx += static_cast<double>(s.t[j] - s.t[i]) * 1.0e-6;
            sy += s.v[j];
            ++cnt;
            end = j;
            if (s.t[j] - s.t[i] >= cfg.rateWindowUs)
                break;             // the window is full (this sample IS the extension when needed)
        }
        if (static_cast<int>(cnt) < minN || s.t[end] - s.t[i] < cfg.rateWindowUs)
            continue;              // too few samples, or too short a time base to call it a rate

        const double xbar = sx / static_cast<double>(cnt);
        const double ybar = sy / static_cast<double>(cnt);

        // Pass 2 — the centred sums. Centred rather than Sxy = Σxy − ΣxΣy/n because a series that
        // sits at 100 ± 0.2 (a still address on an absolute-angle channel) loses four or five
        // digits to that cancellation and the whole quantity here IS the small difference.
        double sxx = 0.0, sxy = 0.0;
        for (std::size_t j = i; j <= end; ++j) {
            if (!s.isValid(j))
                continue;
            const double dx = static_cast<double>(s.t[j] - s.t[i]) * 1.0e-6 - xbar;
            sxx += dx * dx;
            sxy += dx * (s.v[j] - ybar);
        }
        if (!(sxx > 0.0))
            continue;              // coincident timestamps only (defensive: the span test passed)

        const double slope = sxy / sxx;                     // units per SECOND

        // Pass 3 — the residual, for the standard error. sqrt(SSE/(n−2))/sqrt(Sxx): with two points
        // there is no residual degree of freedom left and an exact fit has no residual at all, and
        // in both cases the honest answer is 0 rather than a divide by zero.
        double sse = 0.0;
        for (std::size_t j = i; j <= end; ++j) {
            if (!s.isValid(j))
                continue;
            const double dx  = static_cast<double>(s.t[j] - s.t[i]) * 1.0e-6 - xbar;
            const double res = (s.v[j] - ybar) - slope * dx;
            sse += res * res;
        }
        const double se = (cnt > 2 && sse > 0.0)
                              ? std::sqrt(sse / static_cast<double>(cnt - 2)) / std::sqrt(sxx)
                              : 0.0;

        // Per 100 ms LAST, so the fit and its error are scaled by exactly the same factor.
        const double value = slope * 0.1;
        if (r.ok && !(std::fabs(value) > std::fabs(r.value)))
            continue;
        r.value = value;
        r.sigma = se * 0.1;
        r.atUs  = s.t[i] + (s.t[end] - s.t[i]) / 2;
        r.ok    = true;
    }
    return r;
}

} // namespace pinpoint::analysis
