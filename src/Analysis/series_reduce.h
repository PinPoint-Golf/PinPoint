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
// And, added in Phase 6 so a chart can DRAW what a card reports:
//
//   * windowedMeans — the Extremum's own per-sample centred mean, one entry per sample. THE DRAWN
//                 LINE IS THE CANDIDATE SET reduceExtremum RANKS: both call the one per-sample
//                 function (detail::windowMeanValueAt), so "PEAK is the largest value on the line
//                 inside the window" holds BY CONSTRUCTION, not by two arithmetics that agree.
//                 That is the same argument as the paragraph above about the card and the engine,
//                 one layer out — a line and a number computed two ways will differ one day, and
//                 the reader has no way to tell which of them is lying. There is no other smoothing
//                 anywhere for display; the raw samples stay drawn beside this line, and an INVALID
//                 sample's entry is its RAW value with ok = false, because a bridged run is a line
//                 the producer drew and not a measurement to average.
//
// ⚠ A LEAST-SQUARES SLOPE IS NOT A ROBUST ESTIMATOR, and the header says so where a reader will
// look. It removes the "noise ÷ 8 ms" failure completely — that is the failure the design is about,
// and on white noise the fitted slope is near zero where the adjacent-frame rate is an order of
// magnitude larger (pinned by series_reduce_test §3) — but a single LARGE outlier still moves it.
// For a window of n samples spanning T seconds an outlier of magnitude A displaces the slope by
// roughly 6A/(nT) per second, so at 8 ms spacing over 50 ms (n ≈ 8, T ≈ 0.056 s) a 99-unit spike on
// an 8-unit ramp still reads ≈ 100 per 100 ms against the ramp's 1.0 — 12× better than the
// adjacent-frame 1188, and nowhere near the truth. What saves the presentation there is `sigma`:
// the fit's own standard error comes back the same order as the slope (≈ 57), which is exactly the
// signal design §5.3 renders as "± σ" beside PK RATE. If a spike-PROOF rate is ever needed the
// answer is a robust regression (Theil–Sen over the window's pairwise slopes) or a residual gate,
// not a wider window; that is a design decision and is NOT taken here.
//
// Valid-aware throughout: a sample marked 0 in MetricSeries::valid was BRIDGED across a gated or
// absent run (metric_channel.h channelValidityMask) and is a line the producer drew, not a
// measurement. It may not pull a median, be an extremum, or sit in a fit. Same rule in all four,
// and a NON-FINITE sample is treated identically — see SeriesView::isValid.
//
// std-only: no Qt, no OpenCV, nothing but <algorithm>/<cmath>/<vector> and the frozen constants, so
// a tool, a probe or a test can point one of these at any pair of arrays. MetricSeries is one line
// away in series_reduce_metric.h, which is the only file here that knows the analysis types exist.
// Header-only, so no consumer needs a link-time addition.
//
// COMPLEXITY, stated because a reducer that runs per metric per span is easy to make quadratic by
// accident. At and Rate are O(n·w) in the samples n and the window occupancy w — each anchor walks
// only its own window, and the windows are bounded in TIME (±15 ms, 50 ms), so w is ≈5 in the dense
// pose zone and ≈3 in the sparse one. Extremum is O(n·m) in the series length and the CANDIDATE
// count m, because each anchor's support is a neighbourhood of the whole series (the support is
// deliberately not clamped to the query — see reduceExtremum) and the symmetric widening has to
// know how far away the next valid sample outside the window is. m is a span's own sample count for
// the engine (a handful) and the whole series for the chart's one full-window call, where 207²
// comparisons is nothing. Two degenerate shapes are handled rather than tolerated: the anchor loops
// start at the first sample at or after `fromUs` instead of at 0, so a late span does not rescan the
// prefix once per anchor; and every loop that is bounded by `toUs` tests it BEFORE validity and
// breaks, so a long all-bridged tail is not rescanned either (it was, when validity came first).
//
// Phase 6 split the per-sample mean into detail::windowMeanValueAt (the value, ranked at every
// candidate) and detail::windowMeanSigmaAt (the residual, wanted at exactly one), because the first
// cut computed both together and that is a real cost in the place it lands. On a 250-sample series
// reduceExtremum is ~64 k inner iterations and 2 vector allocations per call — the σ gather runs
// once, for the winner, after the loop — where computing σ per candidate made it ~125 k iterations
// and ~500 allocations, twice per summaryMasked per brush-drag frame across up to 25 facets.
// windowedMeans is the ~125 k one by nature (every sample gets a σ) and keeps its allocations at 2
// by reusing scratch vectors, and it runs ONCE PER DATA CHANGE rather than per frame. The value
// arithmetic is still written exactly once, which is the C16 guarantee; only the error bar is
// computed where it is asked for.

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
    int64_t atHalfWindowUs     = tuned::sampler::kWindowHalfUs;      // ±15 ms, the existing convention
    // How many valid samples an At window needs. 1 — a single measurement at the instant IS the
    // measurement. It is configurable only because PhaseGridConfig::minValidSamples already exists
    // and the engine has to be able to delegate WITHOUT changing its answer.
    int     minAtSamples       = tuned::sampler::kMinValidSamples;
    int64_t extremumWindowUs   = tuned::reduce::kExtremumWindowUs;   // 40 ms, CENTRED (±20 ms)
    // The floor that keeps the centred window from being a single sample where the grid is sparse.
    int     minExtremumSamples = tuned::reduce::kMinExtremumSamples; // 3
    int64_t rateWindowUs       = tuned::reduce::kRateWindowUs;       // 50 ms minimum time base
    int     minRateSamples     = tuned::reduce::kMinRateSamples;     // 3
};

// A borrowed view of one series: no ownership, no copy, no Qt.
//
// `valid` may be null, and null means EVERY SAMPLE IS VALID — the same statement MetricSeries's
// empty mask makes. THE CALLER APPLIES THE SHORT-MASK RULE: a mask shorter than the curve is a
// malformed document rather than a partial statement, and is treated as no mask at all (guessing
// which end it was truncated from would invent validity we were never told about). viewOf() in
// series_reduce_metric.h applies it; a caller assembling a view from QVariantLists must do the
// same, which is why chart_metrics.cpp and measure_sample.cpp each have exactly one place that
// decides it.
//
// ⚠ TIMESTAMPS MUST BE ASCENDING. Every reducer walks the window with that assumption and stops
// early on it. Coincident (equal) timestamps are TOLERATED but DOUBLE-WEIGHTED: two samples at one
// instant both enter a median, a mean and a fit, which is the right reading of two measurements
// that happen to share a clock tick and the wrong reading of an accidentally duplicated row. No
// producer emits one — the grid is a set of instants — so this is stated rather than defended
// against.
struct SeriesView {
    const int64_t *t     = nullptr;   // ASCENDING, absolute µs
    const double  *v     = nullptr;
    const uint8_t *valid = nullptr;   // null ⇒ all valid
    size_t         n     = 0;

    // A NON-FINITE VALUE IS NOT A MEASUREMENT EITHER. NaN and ±inf are folded in here rather than
    // guarded at four call sites: a NaN that reaches std::nth_element is undefined behaviour, and
    // one that reaches a mean or a least-squares fit poisons the whole window silently and comes
    // back as a confident `ok = true` with a NaN in it. Nothing in the pipeline should produce one
    // (interpChannel never returns NaN by contract), which is exactly why it must not be trusted
    // as an invariant here.
    bool isValid(size_t i) const
    {
        return (!valid || valid[i] != 0) && std::isfinite(v[i]);
    }
};

// One reduction's answer. `ok` false means THE CURVE HAS NOTHING TO SAY HERE — not zero. Every
// caller must branch on it rather than printing `value`, which is the whole of design §4
// principle 2 at this layer.
struct Reduced {
    double  value = 0.0;
    int64_t atUs  = 0;      // where the answer was taken (see each reducer for what it means)
    double  sigma = 0.0;    // 1σ on `value` where the reduction has one, else 0
    bool    ok    = false;
};

// One sample's windowed mean — the drawn line, and the Extremum's candidate at that sample.
//
// `ok` false means THIS SAMPLE IS NOT A MEASUREMENT (bridged, gated, or non-finite), and then
// `value` is the sample's RAW value: it is what the chart already draws dashed across a bridged run
// and it is the only thing the curve has to say there. `sigma` is 0 for such an entry — an invalid
// sample has no mean, so it cannot have a standard error on one, and printing a small number
// instead would be a claim. There is no `atUs`: the entry belongs to sample i, whose time the
// caller already has.
struct WindowedMean {
    double value = 0.0;
    double sigma = 0.0;
    bool   ok    = false;
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

// Does `cand` beat `best` by enough to be a different answer?
//
// ⚠ TIES GO TO THE EARLIEST WINDOW, AND THAT NEEDS A MARGIN RATHER THAN `>`. On a clean ramp every
// sliding window has the same slope to the last bit, and a bare `>` handed the reported instant to
// whichever anchor accumulated its sums 1e-16 higher — anchor 48 of 93 on the test's ramp, which
// then moves with the optimiser, the platform and the sample count. `atUs` is shown to the user
// (the PEAK dot, tRateUs) and is compared between runs, so it may not be decided by float noise.
// The margin is relative, floored at 1, so it is meaningful for both a 0.001-unit curve and a
// 100-unit one.
inline bool improves(double cand, double best, bool wantMax)
{
    const double margin = 1.0e-12 * std::max(1.0, std::fabs(best));
    return wantMax ? (cand > best + margin) : (cand < best - margin);
}

// The noise the WINDOW MEAN carries — NOT the spread of the window's samples.
//
// ⚠ THIS IS THE DISTINCTION THE FIRST VERSION GOT WRONG, and it reported a NOISELESS ramp as
// ±0.08: the sample standard deviation of a window sitting on a rising curve measures the CURVE'S
// OWN MOTION across 40 ms, which is signal, not error. Beside a peak, "± 0.08" then reads as
// measurement uncertainty when it is the slope. So the scatter is taken about a LOCAL STRAIGHT
// LINE through the window (any real excursion is locally linear over 40 ms), and what is returned
// is the standard error of the MEAN of k samples: sqrt(SSE/(k−2)) / sqrt(k). A clean ramp gives 0,
// a noisy still gives σ/√k, and a spike in the window gives something large — which is the honest
// answer for a mean that a spike is inside.
//
// Fewer than three samples ⇒ 0: a two-point window is fitted exactly by the line and has no
// residual to speak from, and pretending otherwise would print certainty.
inline double windowMeanSigma(const std::vector<double> &xs, const std::vector<double> &ys,
                              double ybar)
{
    const std::size_t k = xs.size();
    if (k < 3)
        return 0.0;

    double sx = 0.0;
    for (double x : xs) sx += x;
    const double xbar = sx / static_cast<double>(k);

    double sxx = 0.0, sxy = 0.0;
    for (std::size_t m = 0; m < k; ++m) {
        const double dx = xs[m] - xbar;
        sxx += dx * dx;
        sxy += dx * (ys[m] - ybar);
    }
    // sxx == 0 only when every sample in the window shares one timestamp (see the coincident-
    // timestamp note on SeriesView): the scatter is then taken about the mean itself.
    const double b = (sxx > 0.0) ? sxy / sxx : 0.0;

    double sse = 0.0;
    for (std::size_t m = 0; m < k; ++m) {
        const double res = (ys[m] - ybar) - b * (xs[m] - xbar);
        sse += res * res;
    }
    if (!(sse > 0.0))
        return 0.0;                    // an exact local fit — a clean ramp, a constant
    return std::sqrt(sse / static_cast<double>(k - 2)) / std::sqrt(static_cast<double>(k));
}

// ⚠ THE ONE PLACE THE WINDOWED MEAN IS COMPUTED. reduceExtremum ranks these and windowedMeans
// tabulates them, so there is exactly one arithmetic and the chart's line cannot drift from the
// card's PEAK. Do not inline a second copy of this loop anywhere, for any reason.
//
// The mean of the valid samples within ±extremumWindowUs/2 of t_i, WIDENED symmetrically (out to
// the next valid sample's distance, both sides) until it holds minExtremumSamples valid samples or
// the series has no sample left to give. The support is the anchor's own neighbourhood over the
// WHOLE series and is deliberately NOT clamped to any query — the full argument for that, and for
// the widening, is in reduceExtremum's comment below, which is where a reader looking for the
// definition of PEAK will be. Query-independence is exactly what lets one of these serve every
// span, every card and the drawn line at once.
//
// ⚠ THE VALUE AND ITS σ ARE TWO CALLS ON PURPOSE. reduceExtremum needs the value at EVERY candidate
// and the σ at exactly one of them (the winner), and it runs twice per summaryMasked per
// brush-drag frame across up to 25 facets; computing the residual here would put two vector
// allocations and a second full walk on every candidate for a number all but one of them throws
// away. So this returns the window it settled on (`h`, `cnt`) and windowMeanSigmaAt finishes the
// job for whoever actually wants the error bar. The VALUE arithmetic is still in one place, which
// is the guarantee that matters.
//
// An out-of-range or invalid `i` returns the raw value with ok = false; for a valid `i` cnt >= 1
// always, since sample i is inside its own window.
struct WindowValue {
    double      value = 0.0;    // the windowed mean, or the RAW sample when !ok
    int64_t     h     = 0;      // the half-width the widening settled on
    std::size_t cnt   = 0;      // valid samples inside it
    bool        ok    = false;
};

inline WindowValue windowMeanValueAt(const SeriesView &s, std::size_t i, const ReduceConfig &cfg)
{
    WindowValue w;
    if (i >= s.n)
        return w;                      // nothing to read: value stays 0, ok stays false
    w.value = s.v[i];                  // the raw value, which is all an invalid sample has
    if (!s.isValid(i))
        return w;

    const int64_t half  = cfg.extremumWindowUs / 2;
    const int     minEx = std::max(1, cfg.minExtremumSamples);

    // This anchor's half-width and its window mean, in one walk per widening step. `next` is the
    // smallest distance strictly outside the current width, i.e. where the window would have to
    // reach to admit one more sample; when there is none the series has nothing further to give and
    // the window is as wide as it will ever be.
    int64_t     h   = half;
    double      sum = 0.0;
    std::size_t cnt = 0;
    for (;;) {
        sum = 0.0;
        cnt = 0;
        int64_t next = -1;
        for (std::size_t j = 0; j < s.n; ++j) {
            if (!s.isValid(j))
                continue;
            const int64_t d = absUs(s.t[j] - s.t[i]);
            if (d <= h) {
                sum += s.v[j];
                ++cnt;
            } else if (next < 0 || d < next) {
                next = d;
            }
        }
        if (static_cast<int>(cnt) >= minEx || next < 0)
            break;
        h = next;
    }

    w.value = sum / static_cast<double>(cnt);
    w.h     = h;
    w.cnt   = cnt;
    w.ok    = true;
    return w;
}

// The standard error of THAT mean, about a local straight line (windowMeanSigma). Second half of
// the pair above: it re-gathers the window windowMeanValueAt settled on — same anchor, same `h`,
// same validity rule, so the same samples in the same order — and hands them to the one σ
// arithmetic. x in SECONDS from the anchor, never raw µs (see reduceRate for why).
//
// `xs` / `ys` are CALLER-OWNED SCRATCH, cleared here and never shrunk, so a caller that asks for
// every sample's σ (windowedMeans) allocates twice for the whole series instead of twice per
// sample. A caller that wants one σ can pass two empty locals and pay the two allocations once.
inline double windowMeanSigmaAt(const SeriesView &s, std::size_t i, const WindowValue &w,
                                std::vector<double> &xs, std::vector<double> &ys)
{
    if (!w.ok)
        return 0.0;                    // an invalid sample has no mean, so no error on one

    xs.clear();
    ys.clear();
    xs.reserve(w.cnt);
    ys.reserve(w.cnt);
    for (std::size_t j = 0; j < s.n; ++j) {
        if (!s.isValid(j))
            continue;
        if (absUs(s.t[j] - s.t[i]) > w.h)
            continue;
        xs.push_back(static_cast<double>(s.t[j] - s.t[i]) * 1.0e-6);
        ys.push_back(s.v[j]);
    }
    return windowMeanSigma(xs, ys, w.value);
}

} // namespace detail

// ── The drawn line ──────────────────────────────────────────────────────────
// One entry per sample: `out.size() == s.n` always, and an empty series gives an empty vector.
// Entry i is the windowed mean AT SAMPLE i — the very number reduceExtremum ranks when sample i is
// a candidate — so a caller that plots this vector is plotting the curve the PEAK tile reduces, and
// the tile's value is the extremum of the plotted points inside its window BIT-FOR-BIT (pinned in
// series_reduce_test §10). Invalid samples carry their raw value with ok = false; nothing here
// depends on a query, so ONE call per data change serves every window the caller will ever ask
// about.
//
// COST. This is the expensive one and it is meant to be: every sample pays BOTH walks (the value
// window and the σ gather), which on a 250-sample curve is ~125 k inner iterations — twice
// reduceExtremum's, which gathers σ for the winner alone. It buys that back by running ONCE PER
// DATA CHANGE, never per frame (PpMetricChart._plottable), and by reusing one pair of scratch
// vectors for the whole series, so the whole call allocates twice rather than twice per sample.
inline void windowedMeans(const SeriesView &s, std::vector<WindowedMean> &out,
                          const ReduceConfig &cfg = {})
{
    out.clear();
    out.resize(s.n);

    std::vector<double> xs, ys;        // reused across entries: clear() keeps the capacity
    for (std::size_t i = 0; i < s.n; ++i) {
        const detail::WindowValue w = detail::windowMeanValueAt(s, i, cfg);
        out[i].value = w.value;        // the mean, or the raw sample where !ok
        out[i].ok    = w.ok;
        out[i].sigma = detail::windowMeanSigmaAt(s, i, w, xs, ys);
    }
}

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
        if (s.t[i] - us > cfg.atHalfWindowUs)
            break;                     // ascending: nothing later is in the window either
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
// delta, it is no delta. atUs = toUs (the instant the change is reported AS OF), which is also
// what makes a BACKWARDS delta — to earlier than from — the negation of the forward one rather
// than an error: the caller chose the direction and the sign is the answer.
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
// of the excursion rather than one edge of it), `sigma` the standard error of that mean about a
// local straight line (detail::windowMeanSigma — the noise the mean carries, NOT the curve's
// spread). Ties go to the earliest window (detail::improves).
//
// THE CANDIDATES ARE windowedMeans()'s ENTRIES, one per valid anchor in range — the same
// detail::windowMeanValueAt call, so `value` is by construction the extremum of the curve a chart
// draws from that vector; and `sigma` is that entry's sigma, gathered once for the winner by
// detail::windowMeanSigmaAt out of the very window that entry used (series_reduce_test §10 pins
// both, bit-exact, on every fixture).
//
// ⚠ [from, to] BOUNDS THE CANDIDATES, NOT THE SUPPORT. Only the anchors — the instants that may
// WIN — are required to lie in the window; each anchor's ±half-window mean draws on every valid
// sample near it, inside the window or not. That is deliberate and it is the harder of the two
// choices to defend, so here is the argument.
//
// The diagnostics engine CACHES an extreme per (lo, hi] span and the review card reduces a whole
// window in one call. Those two can only ever agree if a sample's windowed mean is the same number
// whoever asked — i.e. if the support is QUERY-INDEPENDENT. Clamp the support and it is not: the
// same instant reduces to one value as the last anchor of span B and to another as an interior
// anchor of the union, and the card and the engine then disagree by construction. W2 measured it on
// rich_7iron: 20 of 514 authored extremum measures disagreed between the span cache and the
// whole-window reduction with the support clamped, 0 of 514 with it unclamped. Cache agreement is
// design §5.2's whole reason for existing, so the support stays unclamped and what the reducers
// guarantee instead is the invariant the cache actually needs — the extreme over a union of
// adjacent spans is the extreme OF their extremes (pinned in series_reduce_test §8).
//
// An earlier cut clamped it, for a real reason: on a rise-to-impact-then-reverse curve the mean at
// a span's first anchor averages in the steeper approach BEFORE the span, so a P6→P7 span whose
// samples run 18.4 → 20.0 reports a minimum of 17.92 — a number the curve never had inside the
// span. ⚠ THAT IS A DOMAIN LEAK AND IT IS CLOSED AT THE PRODUCER, WHICH IS THE ONLY PLACE IT CAN
// BE CLOSED ONCE: a sample PAST IMPACT on one of the ten Address→Impact channels is now marked
// INVALID by lower_body_metrics.cpp / upper_body_metrics.cpp, so it is not a measurement and no
// reducer at any query can draw on it. Clamping here would have hidden the leak for spans while
// leaving it wide open for every whole-window card — and cost the cache agreement. (The pre-Address
// head is deliberately NOT marked: the chart does not clip that side, so marking it made every
// clamped card read `partial`, and those samples are honest readings of a still address —
// applyPhaseDomainMask carries that argument.)
//
// ⚠ THE SAMPLE FLOOR IS WHY THE SUPPORT MATTERS AT ALL. At the 27 ms spacing outside the dense
// pose zone a ±20 ms window holds exactly ONE sample, so the "mean" was the sample and the reducer
// did nothing whatever in the address and backswing regions — the corpus probe printed peakSigma
// 0.000 on every still-address row, which is the tell. So the window WIDENS symmetrically (the same
// half-width both sides, out to the next valid sample's distance) until it holds
// minExtremumSamples valid samples, or until there is no valid sample left to add. Widening obeys
// the same rule as the window it grows: it takes valid samples wherever they are, which keeps a
// widened mean query-independent too. It is inert wherever the grid is dense — every 8 ms fixture
// answers identically with the floor at 1.
inline Reduced reduceExtremum(const SeriesView &s, int64_t fromUs, int64_t toUs, bool wantMax,
                              const ReduceConfig &cfg = {})
{
    Reduced r;
    if (s.n == 0)
        return r;

    detail::WindowValue best;      // the winning anchor's window, for its σ after the loop
    std::size_t         bestI = 0;

    // The CANDIDATE block starts here — the anchors, not their support (see the note above), so
    // this bounds the outer loop only and a late span does not walk the prefix once per anchor.
    std::size_t first = 0;
    while (first < s.n && s.t[first] < fromUs)
        ++first;

    for (std::size_t i = first; i < s.n; ++i) {
        if (s.t[i] > toUs)
            break;                     // ascending: no later anchor is in range either
        if (!s.isValid(i))
            continue;              // skipped before the window is built — w.ok would say the same

        // ⚠ THE CANDIDATE IS windowedMeans[i], NOT A SECOND COPY OF ITS ARITHMETIC. Phase 6 lifted
        // the mean into detail::windowMeanValueAt so the chart can draw the same numbers this loop
        // ranks; ranking anything else here — a raw sample, a differently-clamped mean — would put
        // the line and the card back into disagreement, which is the one failure this header
        // exists to make impossible.
        const detail::WindowValue w = detail::windowMeanValueAt(s, i, cfg);
        if (r.ok && !detail::improves(w.value, r.value, wantMax))
            continue;

        best    = w;               // its window, kept for the ONE σ gather below
        bestI   = i;
        r.value = w.value;
        r.atUs  = s.t[i];          // the winner's OWN time: the centre of its window
        r.ok    = true;
    }

    // σ for the WINNER ONLY, after the loop. Every candidate needs a value; exactly one needs an
    // error bar, and this reducer runs twice per summaryMasked per brush-drag frame across up to
    // 25 facets — so the residual walk is paid once per call and not once per sample. It is the
    // same window windowedMeans would gather at bestI, hence the same double
    // (series_reduce_test §10).
    if (r.ok) {
        std::vector<double> xs, ys;
        r.sigma = detail::windowMeanSigmaAt(s, bestI, best, xs, ys);
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
// the least misleading single instant to hang it on); ties go to the earliest window
// (detail::improves). sigma = that slope's standard error, sqrt(SSE/(n−2)) / sqrt(Sxx), also per
// 100 ms — 0 for an exact fit, where there is no residual to speak from.
inline Reduced reduceRate(const SeriesView &s, int64_t fromUs, int64_t toUs,
                          const ReduceConfig &cfg = {})
{
    Reduced r;
    // ⚠ THREE IS A FLOOR, NOT A DEFAULT. A two-point "fit" passes through both points, so its SSE
    // is 0 and its standard error is 0 — and a rate of 39 per 100 ms reported as "± 0" is a
    // confident reading of two samples of noise. The third point is what buys the residual degree
    // of freedom that makes `sigma` mean anything, so it is required whatever the config says.
    const int minN = std::max(3, cfg.minRateSamples);

    std::size_t first = 0;
    while (first < s.n && s.t[first] < fromUs)
        ++first;

    for (std::size_t i = first; i < s.n; ++i) {
        if (s.t[i] > toUs)
            break;
        if (!s.isValid(i))
            continue;

        // Pass 1 — the window's extent and its means. x is SECONDS FROM t_i, never raw µs: a
        // least-squares fit in absolute microseconds subtracts two ~1e11 quantities to recover a
        // 1e-2 one and loses most of the mantissa doing it.
        std::size_t cnt  = 0;
        std::size_t end  = i;
        double      sx   = 0.0;
        double      sy   = 0.0;
        for (std::size_t j = i; j < s.n; ++j) {
            if (s.t[j] > toUs)
                break;                 // range first, so an all-bridged tail is not rescanned
            if (!s.isValid(j))
                continue;
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
        // Unreachable, and kept: the span test above already guarantees two distinct timestamps in
        // the window, so Sxx > 0. It costs one compare and it is the difference between a future
        // caller with a degenerate view getting a 0 and getting an inf.
        if (!(sxx > 0.0))
            continue;

        const double slope = sxy / sxx;                     // units per SECOND

        // Pass 3 — the residual, for the standard error. sqrt(SSE/(n−2))/sqrt(Sxx): an exact fit
        // has no residual at all, and the honest answer there is 0 rather than a divide by zero.
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
        if (r.ok && !detail::improves(std::fabs(value), std::fabs(r.value), true))
            continue;
        r.value = value;
        r.sigma = se * 0.1;
        r.atUs  = s.t[i] + (s.t[end] - s.t[i]) / 2;
        r.ok    = true;
    }
    return r;
}

} // namespace pinpoint::analysis
