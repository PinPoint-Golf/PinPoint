# Phase 2 contracts — pinned by the orchestrator. Code against these; if one is wrong, say so in your report and STOP on that item rather than inventing an alternative.

Repo: /Users/markliversedge/Projects/PinPointStudio (main, at ea38fd0 + Phase 2 work)
Design: docs/design/metric_presentation_honesty.md §5.2 (read it), §4 principles, §7 items 2/3/5
Tracker: docs/implementation/metric_presentation_honesty_impl_plan.md Phase 2 (2.1–2.5)
Phase 1 is committed: MetricSeries::valid (empty = all valid; a mask SHORTER than t_us is treated
as NO mask — the "short-mask rule", shared by chart_metrics.cpp and measure_sample.cpp).

## C8. The shared reducers (owner W1) — `src/Analysis/series_reduce.h` + `series_reduce.cpp`
namespace pinpoint::analysis. std-only (no Qt) so both consumers and any tool can use it.

    struct ReduceConfig {
        int64_t atHalfWindowUs    = tuned::sampler::kWindowHalfUs;    // ±15 ms, the existing convention
        int64_t extremumWindowUs  = tuned::reduce::kExtremumWindowUs; // 40 ms, centred
        int64_t rateWindowUs      = tuned::reduce::kRateWindowUs;     // 50 ms minimum span
        int     minRateSamples    = tuned::reduce::kMinRateSamples;   // 3
    };
    // A borrowed view of one series. `valid` may be null (every sample valid). The CALLER applies
    // the short-mask rule (pass null when the mask is shorter than n).
    struct SeriesView { const int64_t *t = nullptr; const double *v = nullptr;
                        const uint8_t *valid = nullptr; size_t n = 0;
                        bool isValid(size_t i) const { return !valid || valid[i] != 0; } };
    SeriesView viewOf(const MetricSeries &m);   // applies the short-mask rule itself
    struct Reduced { double value = 0.0; int64_t atUs = 0; double sigma = 0.0; bool ok = false; };

    // Median of the VALID samples within ±atHalfWindowUs of `us`. ok=false when none.
    // atUs = us. sigma = 0 (a median carries none here).
    Reduced reduceAt(const SeriesView &s, int64_t us, const ReduceConfig &cfg = {});
    // at(to) − at(from); ok only if both ok. atUs = to.
    Reduced reduceDelta(const SeriesView &s, int64_t fromUs, int64_t toUs, const ReduceConfig &cfg = {});
    // Extremum (max if wantMax else min) of the CENTRED-WINDOW MEAN: for every valid sample i with
    // t_i in [from, to], mean of the valid samples within ±extremumWindowUs/2 of t_i (always ≥ 1,
    // itself). value = the winning mean, atUs = t_i of the winner, sigma = sample sd of that window's
    // samples about the mean (0 when < 2). ok=false when no valid sample lies in [from, to].
    Reduced reduceExtremum(const SeriesView &s, int64_t fromUs, int64_t toUs, bool wantMax,
                           const ReduceConfig &cfg = {});
    // The slope of largest MAGNITUDE (signed) over sliding windows: for every valid sample i with
    // t_i in [from, to], the window is the valid samples in [t_i, t_i + rateWindowUs], extended to
    // the first valid sample at or beyond t_i + rateWindowUs if one exists within [from, to]
    // (so a sparse region still spans ≥ rateWindowUs); a window needs ≥ minRateSamples valid
    // samples AND a span ≥ rateWindowUs, else it is skipped. Least-squares slope, reported PER
    // 100 ms (unit/100ms — the chart's existing convention). atUs = the winning window's centre.
    // sigma = the standard error of that slope, per 100 ms (0 when the fit is exact / n == 2).
    // ok=false when no window qualifies.
    Reduced reduceRate(const SeriesView &s, int64_t fromUs, int64_t toUs, const ReduceConfig &cfg = {});

Tuned constants (W1, `src/Core/pp_tuned_constants.h`, new `namespace reduce`):
kExtremumWindowUs = 40000, kRateWindowUs = 50000, kMinRateSamples = 3, each with a why-comment
(design §5.2), registered in docs/validation/tunable_parameters_reference.md (keys
`reduce.extremumWindowUs`, `reduce.rateWindowUs`, `reduce.minRateSamples`; no runtime override
plumbing in this phase — the consumers construct a ReduceConfig from the constants).
Test: `src/Analysis/tests/series_reduce_test.cpp`, registered with `pp_add_test` in
`src/Analysis/tests/CMakeLists.txt` exactly like lower_body_metrics_test (SOURCES the test +
series_reduce.cpp). Hand-rolled g_fail style.

## C9. Chart adoption (owner W3) — `src/Gui/review/chart_metrics.{h,cpp}`, `chart_metrics_test.cpp`
summaryMasked() delegates: start/end = reduceAt at the window edges (if reduceAt is not ok —
no valid sample within ±15 ms — fall back to today's interpolation from the nearest valid samples
and set `partial`); min/max = reduceExtremum(min) / reduceExtremum(max) over [start, end];
peak/tPeakUs = whichever of those has the larger |value| (today's rule); range = max − min;
delta = end − start; rate = reduceRate.value (signed, per 100 ms; 0 and a new key `rateOk:false`
when no window qualifies — never a fabricated number). NEW KEYS: `peakSigma`, `rateSigma`,
`rateOk`, `tRateUs`. Every existing key survives. summary() keeps delegating with an empty mask.
QVariantList → std::vector once per call (already done), then SeriesView (apply the short-mask
rule by passing null). ChartMetrics gets NO reduction arithmetic of its own any more.
PpChartSummary.qml: PK RATE shows "—" when rateOk is false (the per-100ms unit hides too).
Also extend `tools/probes/plumb_bob_chart.qml` (yours now): for each series print a third summary
"STILL ADDRESS" over [Address − 300 ms, Address] with peak/rate/rateOk/rateSigma, and print
`peakSigma`/`rateSigma` on the CLAMPED summary.
Tests: rewrite, do not delete — the Phase 1 spike fixture (one 99 among ~4s at 8 ms) must now give
a peak well below 99 (state the expected windowed-mean bound arithmetically) and a rate near the
ramp's slope, not 9700; a still white-noise series gives rate ≈ 0 (bound it) and extremum within
one σ of the mean; ramp gives its slope and endpoints; sparse 27 ms spacing works; rateOk false
on a 2-sample series.

## C10. Engine adoption (owner W2) — `src/Diagnostics/measure_sample.{h,cpp}`, its test
PhaseGridConfig gains `extremumWindowUs` (default from tuned::reduce). buildPhaseGrid:
the per-phase value = reduceAt (same ±15 ms median, same valid rule — must be byte-identical to
today's for a mask-free swing; prove it with the existing all-ones/absent fixtures);
PhaseGridSpan.min/max = reduceExtremum(min/max) over (from, to] (half-open at the start exactly
as today: pass fromUs+1); the "no samples strictly inside ⇒ endpoint medians" fallback stays.
kPhaseGridSchemaVersion → 3 (spans change for every swing). The engine's own Rate reducer stays
Delta / elapsed between two phase medians — an AVERAGE rate, a different quantity from the chart's
peak rate, and already robust; document that in measure_reducer.h beside ReducerKind::Rate.
Tests: the Phase 1 spike case now shows the span extreme below the raw spike; the schema bump;
no change in `values` for any existing fixture; reduceOverGrid Extremum equals reduceExtremum
called directly on the same fixture (card-vs-engine agreement by construction).

## Rules (unchanged from Phase 1)
Workers never build/run/commit; own only listed files; absent means absent; no display-only
smoothing; comments explain WHY in the surrounding style; report files, tests and what each
proves, contract problems with evidence, anything left undone.
