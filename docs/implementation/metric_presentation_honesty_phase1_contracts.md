# Phase 1 contracts — pinned by the orchestrator. Do not change these shapes; if one is wrong, say so in your report and STOP on that item rather than inventing an alternative.

Repo: /Users/markliversedge/Projects/PinPointStudio (branch main)
Design: docs/design/metric_presentation_honesty.md  (read §3, §4, §5.1 in full before coding)
Tracker: docs/implementation/metric_presentation_honesty_impl_plan.md (Phase 1 tasks 1.1–1.3)

## C1. Series validity mask (owner W1, consumed by W2 + W3)
In `src/Analysis/swing_analysis.h`, `struct MetricSeries` gains, directly after `sigma`:
    // Per-sample validity, parallel to t_us. EMPTY means every sample is valid — every
    // series that predates this field, and every series with nothing to mark, leaves it
    // empty and serialises byte-identically. A 0 marks a sample whose value was BRIDGED
    // across a gated or absent run (see metric_channel.h channelValidityMask): drawn
    // dashed, skipped by every reducer, never the site of a phase sample.
    std::vector<uint8_t> valid;
Invariant: `valid.empty() || valid.size() == t_us.size()`.

## C2. Validity helper (owner W1) in `src/Analysis/metric_channel.h`
    // 1 where grid[i] lies within maxBridgeUs of a real channel sample (and inside the
    // channel's time extent), else 0. Returns an EMPTY vector when every sample is valid.
    inline std::vector<uint8_t> channelValidityMask(const std::vector<int64_t> &grid,
                                                    const std::vector<int64_t> &channelT,
                                                    int64_t maxBridgeUs);
Tuned constant `tuned::channel::kMaxBridgeUs = 60000` (dark key `channel.maxBridgeUs`) in
`src/Core/pp_tuned_constants.h`. Lower- and upper-body series builders call it and set
`m.valid`; a phaseSample is NOT emitted when its nearest grid index is invalid (extends the
existing "an unsegmented phase produces no sample" rule).

## C3. Geometric gates (owner W1)
`tuned::lowerBody::kMinHipSpanRatio = 0.40` (key `lowerBody.minHipSpanRatio`),
`tuned::upperBody::kMinShoulderSpanRatio = 0.40` (key `upperBody.minShoulderSpanRatio`).
Address hip span = |addrTrailHipPx.x − addrLeadHipPx.x| (median-of-address like the rest);
a frame's hip line is valid only when |dx| / addrHipSpanPx ≥ ratio. `LowerBodyState` gains
`bool hipLineValid`. `hipLineTilt` is pushed only on valid frames. Upper body: same for the
shoulder line (`shoulderPlane`), the elbow line (`elbowLine`, address elbow span) and
`sideBend` (needs BOTH the hip line and the shoulder line valid). The other channels
(sway, lift, knee drift, plumb bob, comOverLeadFoot, axis tilt, drift…) are NOT gated here.

## C4. JSON / QVariant key (owner W3 for the bridge; W2 reads it in the grid builder)
Key `"valid"`: an int list of 0/1 parallel to `t_us`, present ONLY when at least one entry
is 0 (mirrors the `sigma` discipline: omitted, never an all-ones array). Bridged in
`src/Gui/shot/shot_processor.cpp toAnalysisDetail()` and read back in
`src/Gui/shot/disk_replay_source.cpp` (mirror pair; a live shot and its reload must agree).
Whoever writes swing.json from that QVariantMap gets it for free — W3 verifies that and
documents the key in `docs/reference/swing_json_schema.md` next to `sigma`.

## C5. Phase domain (owner W2) in `src/Metrics/metric_descriptor.h`
    struct PhaseDomain { Phase first = Phase::Address; Phase last = Phase::Finish; };
    // in MetricDescriptor, after `phases`:
    PhaseDomain domain;   // where the metric's geometry means something; default = whole swing
Plus, in the same header (NOT swing_analysis.h — that file is W1's):
    // P-position ladder order for every Phase value (Address=P1 … Finish=P10; the
    // non-P events slot where they occur in time). The enum order is append-only and unrelated.
    inline int phaseLadderIndex(Phase p);
    inline bool phaseInDomain(const PhaseDomain &d, Phase p);
Manifest authoring (`metric_catalogue_manifest.cpp`): Address→Impact on pelvisSway,
pelvisLift, leadKneeDrift, plumbBobDistance, hipLineTilt, shoulderPlaneAngle,
elbowAlignment, spineSideBend, secondaryAxisTilt, thoraxLateralDrift. Everything else
default. `metric_catalogue_test`: every `phases` entry lies inside `domain`.
`measure_facets.cpp checkReducer` (or wherever the pack validator sees the metric key):
an `at` anchor, or a delta/rate/extremum window, outside the metric's domain is refused with
a reason that names the domain.

## C6. Diagnostics grid (owner W2) `src/Diagnostics/measure_sample.cpp`
`buildPhaseGrid` reads `"valid"` (C4) from the metric JSON object; samples with 0 enter
neither a phase's windowed median nor a span's min/max. A phase whose window has no valid
sample takes the existing "no entry" path. Do NOT bump kPhaseGridSchemaVersion in phase 1
unless the output for an existing (mask-free) swing changes — it must not.

## C7. Chart API (owner W3) `src/Gui/review/chart_metrics.{h,cpp}`
    Q_INVOKABLE QVariantMap summaryMasked(const QVariantList &tUs, const QVariantList &value,
                                          const QVariantList &valid,
                                          qint64 startUs, qint64 endUs) const;
Existing `summary(tUs, value, startUs, endUs)` stays and delegates with an empty mask.
Result map = today's keys + `partial` (bool): true when a window edge had to be read from
non-adjacent valid samples or the window contains invalid samples. Invalid samples are
skipped for min/max/peak/range/rate.
    Q_INVOKABLE QVariantMap domainFor(const QString &key) const;   // {firstPhase:int, lastPhase:int}
from the catalogue descriptor (whole swing when the key is unknown).
QML (`PpMetricChart.qml`, `PpChartSummary.qml`, `PpChartPlot.qml`): the summary window is
clamped to the domain resolved through the phase list (fall back to the window end when the
swing lacks the domain's last phase); a "PARTIAL" chip on a card when `partial`; invalid
runs AND the out-of-domain region are drawn with `ShapePath` dashed at ~0.35 opacity, no
phase dots there; the hover/crosshair value reads "—" on an invalid sample.
NO display-only smoothing of any kind.

## Rules for every worker
- Do NOT build, run tests, commit, or push. The orchestrator builds; you get the output back.
- Own only your files (listed in your prompt). If you need a change in another worker's file,
  write the exact change you need in your report instead of making it.
- Absent means absent: never a sentinel, NaN, 0 or clamp for a sample that was not measured.
- Match the surrounding comment style: explain WHY, name the contract, say what absent means.
- Tests follow the file's existing style (Analysis tests are hand-rolled `g_fail` counters;
  look before writing).
- Report: files changed; the tests you added and what each one PROVES; anything in these
  contracts you believe is wrong, with evidence; anything you deliberately left undone.
