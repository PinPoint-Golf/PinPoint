# Phase 6 (draw the windowed mean) contracts — pinned by the orchestrator.

Repo main at acb9a61. Tracker: metric_presentation_honesty_impl_plan.md Phase 6 (agreed with Mark
5 Sept: "the chart should draw the windowed mean"). Principle: ONE CURVE — the drawn line is the
same reduction the PEAK tile already uses (series_reduce.h reduceExtremum's per-sample centred
mean, ±extremumWindowUs/2 widened to ≥ minExtremumSamples valid samples, invalid samples excluded),
so PEAK == max of the drawn line inside the window BY CONSTRUCTION, and the raw samples stay
visible. Persisted values do not change. No other smoothing anywhere.

## C16. The reducer side (owner W1) — src/Analysis/series_reduce.h, series_reduce_test.cpp
    // One mean per sample, the exact candidate means reduceExtremum ranks. For an INVALID sample
    // the entry is the raw value (it is drawn dashed and is not a measurement), and `ok` is false
    // for it. Length == s.n.
    struct WindowedMean { double value; double sigma; bool ok; };
    inline void windowedMeans(const SeriesView &s, std::vector<WindowedMean> &out,
                              const ReduceConfig &cfg = {});
Refactor reduceExtremum to use the same per-sample function (one implementation), so the
identity holds by construction, and pin it: for any [from,to], reduceExtremum(max/min).value ==
max/min over the valid samples in [from,to] of windowedMeans[i].value, bit-exact, on the ramp,
spike, noise, sparse and masked fixtures. sigma = the same residual standard error the
extremum reports.

## C17. The chart side (owner W3) — chart_metrics.{h,cpp}, chart_metrics_test.cpp,
PpChartPlot.qml, PpMetricChart.qml, PpSegmentBrush.qml, PpChartSummary.qml (if the header
text needs a word), tools/probes/plumb_bob_chart.qml
- `Q_INVOKABLE QVariantMap ChartMetrics::windowedMean(const QVariantList &tUs, const QVariantList
  &value, const QVariantList &valid) const` → { mean: [..], sigma: [..] } (same length as value;
  short-mask rule as elsewhere; invalid entries carry the raw value). Called ONCE per data change
  in PpMetricChart._plottable (decorate each series entry with `mean` and `meanSigma`), never per
  frame.
- PpChartPlot: the trace polyline draws `mean`; raw samples drawn as dots (radius ~Theme.sp(1.2),
  the series colour at 0.35 opacity, only for valid samples, behind the trace) behind a
  `showRawDots` property default true; invalid runs still dashed (drawing the raw value there, as
  now); the σ ribbon follows `mean`; phase dots keep their persisted phaseSample values (they are
  the producers' readings) — do not move them onto the mean.
- Hover / crosshair / legend chip: the value text is the MEAN at the nearest measured sample;
  the tooltip row adds "raw <value>" in colorText3 after it.
- PpSegmentBrush sparkline draws `mean` (raw dots not needed there), y-range from summaryMasked
  as now.
- summaryMasked is unchanged (it already reduces on these means). Add a test: on every fixture,
  summaryMasked.peak == max/min of windowedMean over the window (bit-exact).
- Probe: for each series print `mean[i]` vs `value[i]` at the peak index beside csum.peak, and a
  line asserting csum.peak == that mean (exact).
- No display-only smoothing beyond this one shared reduction; comments say why the line and the
  numbers are one thing.

Rules as always: workers never build/run/commit; own only listed files; report files, tests
and what each proves, contract problems, anything left undone.
