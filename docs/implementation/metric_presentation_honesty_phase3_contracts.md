# Phase 3 contracts — pinned by the orchestrator. Code against these; if one is wrong, say so and STOP on that item.

Repo: /Users/markliversedge/Projects/PinPointStudio (main at 8ed15ea)
Design: docs/design/metric_presentation_honesty.md §5.3 (read it), §4 principle 3, §8 open question 1
Tracker: docs/implementation/metric_presentation_honesty_impl_plan.md Phase 3 (3.1–3.6)
Already in place: `MetricSeries::sigma` (std::optional<double>, 1σ MEASUREMENT noise in the metric's
unit; ABSENT means "not characterised", never write 0), persisted as `"sigma"` by swing_doc.cpp and
bridged by shot_processor / disk_replay_source; `PpChartSummary.qml` renders a "± σ" chip beside the
unit when present. `summaryMasked` exports `peakSigma` (standard error of the winning window mean about
a local line) and `rateSigma` (standard error of the fitted slope, per 100 ms) — both are NOISE of the
reduction, not the series' measurement σ.

## C11. σ propagation (owner W1) — lower_body_metrics.{h,cpp}, upper_body_metrics.{h,cpp}, their tests
Source: `PoseTrack2D::smoothedAux[frame].sigma[k]` (posterior σ in PIXELS per keypoint, 0 = no
smoothed value — see swing_analysis.h PoseKpAux and pose_smoother.cpp for how one scalar covers x and
y; state what you found and how you use it). Only frames where the smoother produced a value
(sigma > 0) contribute. If `smoothedAux` is empty or no frame qualifies, the series' σ stays UNSET.
Per-frame propagation, first order, in the metric's own unit, the same geometry the value uses:
- line tilt (hip, shoulder, elbow, feet): sqrt(σ_a² + σ_b²) / L_px · (180/π), L = that frame's line length
- pelvisSway, pelvisLift: 0.5·sqrt(σ_lead² + σ_trail²) / addrSpanPx · 100
- plumbBobDistance: 0.5·sqrt(σ_lead² + σ_trail²) · mmPerPx / 25.4
- leadKneeDrift: sqrt(σ_knee² + σ_hip²) / addrSpanPx · 100
- comOverLeadFoot: sqrt(σ_mid² + σ_ankle²) / addrSpanPx · 100, σ_mid = 0.5·sqrt(σ_l² + σ_t²)
- spineSideBend: sqrt(σ_hipTilt² + σ_shoulderTilt²)
- the other upper-body channels: derive by the same first-order rule from their own formula (state each);
  if a derivation is not clean, leave that series' σ UNSET and say why — never guess.
Series σ = MEDIAN over the frames that are VALID in the series' mask (skip gated/bridged/out-of-domain
frames) of the per-frame σ; set `m.sigma` only when at least one frame contributed. Pattern:
body_rotation.cpp (`ch.sigmaDeg`, median, set only when computed). Behind no new switch.
Tests: a synthetic track with `smoothedAux` carrying a constant keypoint σ gives each series the
closed-form σ within 5 % (state the arithmetic per series); `smoothedAux` empty ⇒ every `sigma`
unset; frames with sigma 0 excluded; the mask is honoured (an invalid frame's σ does not enter the
median); serialisation unchanged when unset (byte-identical swing.json promise for pre-smoother tracks).

## C12. σ-governed display (owner W3) — chart_metrics.{h,cpp}, chart_metrics_test.cpp, PpChartSummary.qml,
PpMetricChart.qml, PpChartPlot.qml (ribbon), tools/probes/plumb_bob_chart.qml
Display step: `ChartMetrics::displayStep(double sigma, const QString &unit)` = the nicest of
{1, 2, 5}×10ⁿ that is NOT below σ, floored at 1 unit (today's rounding), 1 when σ ≤ 0 or absent.
`formatBare(v, unit, sigma = 0.0)` and `formatValue(v, unit, sigma = 0.0)` round to that step (a
value rounded to a step of 5 prints "+10", "+15"). Q_INVOKABLE overloads by arity are fragile from
QML — add `formatBareSigma` / `formatValueSigma` (3 args) and keep the 2-arg names delegating with 0,
unless you verify overloads resolve in Qt 6.11 QML; say which. Every QML caller that prints a series
value (card tiles, legend chip, hover row, split gutter "@end") passes the series' `sigma`.
Card: PEAK shows "± <peakSigma>" and PK RATE "± <rateSigma>" (formatted with the same step rule,
one decimal at most), only when the value is present; the existing unit-side chip stays for the
series σ; @IMPACT and Δ SEGMENT show no ±. All ± text in Theme.colorText3 at micro size.
Ribbon (dark): `PpChartPlot.showSigmaBand` default false; when on, a filled ShapePath ±series σ
around the valid runs at 0.06 opacity (skip when σ absent). Wire a probe switch only, no UI toggle.
Probe: per series print `sigma`, `displayStep`, and the formatted @impact / peak / rate strings.
Tests: step rule (σ 0 → 1; 0.3 → 1; 1.4 → 2; 2.5 → 5; 6 → 10; 12 → 20; 30 → 50); formatBare with
step 5 rounds 12.4 → "+10", 12.6 → "+15", −7.4 → "-5"; 2-arg forms unchanged; peakSigma/rateSigma
formatting.

## Rules (unchanged)
Workers never build/run/commit; own only listed files; absent means absent (never sigma = 0);
comments say WHY; report files, tests and what each proves, contract problems with evidence,
anything left undone.
