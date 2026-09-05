# Honest metric presentation — implementation plan and tracker

**This is the tracker.** Maintain it here. A new session should read: this file, then the
design doc, then the Log at the bottom.

**Design:** `docs/design/metric_presentation_honesty.md`
**Repo:** `PinPointStudio` only. No PinPointCapture or libppcp involvement.

---

## Context, in four lines

The review chart's curves carry residual detector jitter, the body-line angles go degenerate
when the body turns out of the image plane (−88° hip tilt after impact, +88° shoulder plane at
the top), and the PEAK / PK RATE tiles are raw argmax and adjacent-frame slope, which reward a
single bad sample. The design separates those into validity (Phase 1), robust reducers
(Phase 2), σ propagation and σ-governed display (Phase 3), and a longer smoother window for
slow joints (Phase 4). Phases 1–3 change no persisted `value`; Phase 4 does and is gated on
its own.

**Order is deliberate.** Phase 1 removes every implausible number in the motivating screenshot
with the smallest change and is worth shipping alone. Phase 4 is last because a smoother curve
would hide Phases 1–3's problems without fixing them.

## How to use this file

Status: `[ ]` not started · `[~]` in progress · `[x]` done · `[!]` blocked · `[-]` dropped

Update the boxes and the **Log** as work lands. Commit per phase (Mark approves each commit;
never push without approval). Build with
`cmake --build build/Qt_6_11_1_for_macOS_Debug --target <target> -j 8` and run tests **only
through ctest** (`ctest --test-dir build/Qt_6_11_1_for_macOS_Debug -R <name>`); a bare test
binary lacks `PINPOINT_CORE_NORMS` and fake-fails. At most two or three builds per phase.

**Corpus gate, every phase.** The 108-swing corpus on `/mnt/swingdata` (pose2 cache), via
`tools/swinglab/reanalyze_corpus.py` on GOLFSIMPC for the bulk run, or a single-swing check on
the Mac. Always run a **control** (same code twice) beside the before/after: pose runs are
non-deterministic and ~20 metrics differ at 1e-14 between identical runs, so a diff without a
control cannot attribute anything. Judge a run by metric COUNT first, never the score.
`tools/swinglab/parity_diff.py` is the byte-identical gate for the swings a phase must not
touch.

---

## Phase 0 — Measure the baseline `[x]`

Nothing ships from this phase; it produces the numbers the definition of done is judged against.

- [x] **0.1** Script `tools/metrics/series_noise.py` (new): for every swing.json under a root,
      for the lower-body and body-line keys, report median / p95 frame-to-frame jitter, the
      current `summary()`-style PK RATE over (a) the whole swing and (b) a still address window
      (Address − 300 ms → Address), the raw range, and every phase sample. One CSV row per
      (swing, key). This is the one-off probe from 2026-09-04 made repeatable.
- [x] **0.2** Add a foreshortening column: per frame, `|dx_hips| / addrHipSpanPx` and the
      shoulder equivalent, read from `pose2d.smoothed` (fall back to `frames`). Report, per
      swing, the fraction of P1–P7 frames below 0.4 and whether any P1–P7 phase instant is
      below it. This is what decides the default in 1.1 and whether the gate ever fires inside
      the domain (it should be rare; if it is common, the ratio is wrong, not the swings).
- [x] **0.3** Run on the corpus; commit the CSV under `docs/validation/data/` with the run's
      commit hash in the filename. Record the headline numbers in the Log.

**Done when:** the CSV exists and the Log has the baseline table.

---

## Phase 1 — Validity: geometric gates and phase domains `[x]`

Changes no persisted `value`. Adds absence. The screenshot's −88°, +35 % and the 88° top
sample all disappear here.

### 1.1 Series validity mask

- [x] `swing_analysis.h`: `MetricSeries` gains `std::vector<uint8_t> valid;` (empty ⇒ all
      valid). Comment it beside `sigma` with the same absent-means-what discipline.
- [x] `metric_channel.h`: add `buildChannelSeriesMasked(grid, channel, …)` (or extend the
      existing builder with a `maxBridgeUs` argument): a grid sample farther than
      `maxBridgeUs` from the nearest real channel sample, or inside a gated run, is filled by
      `interpChannel` as now **and** marked invalid. Default `maxBridgeUs` = 60 ms so a
      one-frame conf dropout still bridges silently (today's behaviour) and only a real run is
      marked. Tuned constant `tuned::channel::kMaxBridgeUs`, dark key `channel.maxBridgeUs`.
- [x] `shot_processor.cpp` `toAnalysisDetail` and `disk_replay_source.cpp`: bridge `valid`
      as an int list, present only when any sample is 0 (mirror pair; a live shot and its
      reload must agree). Schema note in `docs/reference/swing_json_schema.md` §metrics.
- [x] Phase samples: in `buildLowerBodySeries` / `buildUpperBodySeries`, do not emit a
      phaseSample whose grid index is invalid (extend the existing "an unsegmented phase
      produces no sample" rule to "an invalid instant produces no sample").

### 1.2 Geometric gates

- [x] `pp_tuned_constants.h`: `lowerBody::kMinHipSpanRatio = 0.40`,
      `upperBody::kMinShoulderSpanRatio = 0.40`; dark keys `lowerBody.minHipSpanRatio`,
      `upperBody.minShoulderSpanRatio`; register both in
      `docs/validation/tunable_parameters_reference.md`. Revisit 0.40 against 0.2's numbers.
- [x] `lower_body_metrics.cpp`: the address hip span (`|addrTrailHipPx.x − addrLeadHipPx.x|`)
      joins the address reference; `hipTilt` is pushed only when the frame's `|dx| / addrHipSpan`
      clears the ratio. `LowerBodyState` gains `hipLineValid` so the upper-body module can ask
      the same question of the same frame.
- [x] `upper_body_metrics.cpp`: the same for the shoulder line (`shoulderPlane`), the elbow
      line (`elbowLine`, against the address elbow span) and `sideBend` (needs both lines valid).
- [x] Tests, `lower_body_metrics_test.cpp` / `upper_body_metrics_test.cpp`: a synthetic frame
      sequence that rotates the hips (or shoulders) from square to 80° about the vertical:
      the tilt channel must go absent, not to ±90°, once the ratio crosses the gate; the
      series' `valid` mask must be 0 there; no phase sample lands on an invalid index.

### 1.3 Phase domains

- [x] `metric_descriptor.h`: `struct PhaseDomain { Phase first = Address; Phase last = Finish; }`,
      `MetricDescriptor::domain`, default whole swing. `metric_catalogue_manifest.cpp`: author
      Address→Impact on the ten metrics in design §5.1's table.
- [x] `metric_catalogue_test.cpp`: every phase in `descriptor.phases` lies inside
      `descriptor.domain` (ladder order, not enum order — reuse the manifest's `kP1toP7`
      ordering helper, or add one to `swing_analysis.h` beside the Phase enum).
- [x] `measure_facets.cpp` `checkReducer`: a `delta`/`rate`/`extremum` window, or an `at`
      anchor, outside the metric's domain is refused with a reason naming the domain. Run
      `core_pack_test` / `diagnostics_catalogue_integrity_test`; fix any content the validator
      now refuses as a content bug (expected: none, since the pack reads P1–P7 only).
- [x] `chart_metrics.cpp`: `ChartMetrics::domainFor(key)` returning `{firstPhase, lastPhase}`
      from the catalogue; `summary()` gains a `valid` argument and skips invalid samples
      (min/max/peak/rate); returns `partial: true` when a window edge had to interpolate from
      non-adjacent valid samples.
- [x] `PpMetricChart.qml` / `PpChartSummary.qml`: the summary window is clamped to the
      metric's domain (resolved through the phase list to instants; a swing lacking the
      domain's last phase falls back to the window end). A "partial" chip on the card when
      `partial` is set.
- [x] `PpChartPlot.qml`: split each series into valid / invalid runs; invalid runs drawn
      with `strokeStyle: ShapePath.DashLine` at 0.35 opacity; the region outside the domain
      gets the same treatment plus no phase dots. Hover value reads "—" on an invalid sample.
      Probe with an offscreen `--probe` build rather than screenshots (see memory).
- [x] `chart_metrics_test.cpp`: summary skips invalid samples; window edges on invalid
      samples set `partial`; domain clamp.

### 1.4 Gate

- [x] Corpus before/after + control. `parity_diff.py` must be byte-identical for every swing
      whose series contain no invalid sample. For the rest, the only diffs are: `valid` arrays
      appearing, phase samples disappearing where the design says they must, and nothing in
      `value[]`. Re-run 0.1/0.2; the "phase sample below the ratio" count must be 0.

**Done when:** design §7 items 1 and 6 hold on the corpus, and the Plumb Bob preset on the
2026-08-18 swing shows no tilt below −30° anywhere and dashed runs past P7.

---

## Phase 2 — Robust reducers, one implementation `[x]`

Changes what is *derived*; `value[]` untouched.

- [x] **2.1** `src/Analysis/series_reduce.h` (+ `.cpp` if it will not stay header-only),
      Qt-only, no Gui: `reduceAt` (±window median, valid-aware), `reduceDelta`,
      `reduceExtremum` (extremum of the centred-window mean; returns value + centre `atUs`),
      `reduceRate` (max-|slope| least-squares over sliding windows ≥ `rateWindowUs`, ≥ 3 valid
      samples; returns slope per 100 ms and its standard error). Tuned constants
      `tuned::reduce::kExtremumWindowUs = 40000`, `kRateWindowUs = 50000`; dark keys
      `reduce.extremumWindowUs`, `reduce.rateWindowUs`.
- [x] **2.2** `series_reduce_test.cpp` (new, `src/Analysis/tests`): a clean ramp gives the
      ramp's slope and its endpoint as extremum; the same ramp with one 10σ spike gives the
      same answers within σ; a still series with white noise gives a rate near 0 and an
      extremum within one σ of the mean; invalid samples are ignored; sparse (27 ms) and dense
      (8 ms) spacing both work.
- [x] **2.3** `ChartMetrics::summary` delegates to 2.1 for start/end (windowed At),
      peak/tPeakUs (extremum), delta and rate. Keep `min`/`max`/`range` as the windowed-mean
      extremes for the same reason. `chart_metrics_test.cpp` updated: the old adjacent-frame
      rate assertions are replaced, not deleted; add the spike case.
- [x] **2.4** `measure_sample.cpp` `buildPhaseGrid`: `PhaseGridSpan.min/max` from
      `reduceExtremum` over the span (valid-aware). `kPhaseGridSchemaVersion` → 3 so the
      sidecar cache rebuilds. `measure_sample_test.cpp`: the spike case; the schema bump.
- [x] **2.5** Gate: corpus run. For every authored `extremum` measure, before/after of the
      reduced value per swing; expected: peaks move toward the mean by about one σ, none
      moves the other way beyond the control's spread. For `at`/`delta`: unchanged within
      the control (already windowed medians). Record the table in the Log. Design §7 items 2,
      3 and 5 are checked here.

**Done when:** PK RATE over a still address window is under 2 units per 100 ms on every
lower-body and body-line series across the corpus, and the card and the engine agree to
display precision on every authored measure.

---

## Phase 3 — σ propagation and σ-governed display `[x]`

- [x] **3.1** `lower_body_metrics.cpp`: `LowerBodyState` gains per-keypoint σ (px) read from
      `pose.smoothedAux[frame].sigma[k]`; `trackLowerBody` takes `const PoseTrack2D&` already,
      so no signature change. Propagate per design §5.3, median over valid frames, set
      `m.sigma` only where at least one frame had a smoothed σ (tier ≠ Off). Same in
      `upper_body_metrics.cpp` for the three lines and side bend.
- [x] **3.2** Tests: a synthetic track with a known constant keypoint σ gives the closed-form
      series σ within 5 %; a track with `smoothedAux` empty leaves `sigma` unset.
- [x] **3.3** `ChartMetrics::formatBare(v, unit, sigma = 0)`: step = the nicest of
      {1, 2, 5}×10ⁿ not below σ, floored at 1. `formatValue` likewise. Every QML caller
      passes the series' sigma (the summary card, the legend chip, the hover tooltip in
      `PpMetricChart.qml`). Test: σ = 2.5 → step 5; σ = 0.3 → step 1; σ = 0 → today's
      behaviour exactly.
- [x] **3.4** `PpChartSummary.qml`: "± σ" beside PEAK, "± σ_rate" beside PK RATE (from 2.1's
      standard error). Keep the existing unit-side chip and its tooltip.
- [x] **3.5** (dark, optional) `PpChartPlot.qml`: a ±σ ribbon as a filled `ShapePath` at
      0.06 opacity behind each curve, behind a `showSigmaBand` property defaulting to false.
- [x] **3.6** Gate: `parity_diff.py` shows only `sigma` keys appearing; design §7 item 4 holds
      (every lower-body and body-line series carries σ on every swing the smoother ran on).
      Look at the Plumb Bob preset with the step rule on and decide the open question in
      design §8 (step rule vs chip alone). Record the decision.

**Done when:** the σ chip renders on every card in the Plumb Bob preset and no displayed digit
is finer than the series' σ.

---

## Phase 4 — Smoother window for slow joints `[x]`

The only phase that moves `value[]`. Separate commit, separate gate.

- [x] **4.1** `pose_smoother.h/.cpp`: `legsSigmaScale` / `legsJerkScale` over keypoints
      11–16, default 1.0 (byte-identical). Dark keys `poseSmooth.legsJerkScale`,
      `poseSmooth.legsSigmaScale` through the existing tuning path in `PoseSmoothStage`.
      `pose_smoother_test.cpp`: scale 1.0 is byte-identical; scale 0.1 on a noisy stationary
      hip reduces residual σ and keeps a 0.5 Hz sinusoid's amplitude within 5 %.
- [x] **4.2** Sweep on the corpus: `legsJerkScale` ∈ {1, 0.3, 0.1, 0.05, 0.02}. Metrics:
      residual jitter (0.1's script) on the hip series, amplitude of the P1→P4 sway excursion
      (must not shrink beyond the control's spread; if it does the window is too long), and
      the phase-sample values at P4/P7 (must move by less than σ). Pick the largest window
      that preserves the excursion.
- [-] **4.3** (DECIDED 2026-09-05: NOT promoted, default stays 1.0 — see the Log) Promote the default only with a before/after + control table in the Log and
      Mark's approval; the corridor content was seeded on the current smoothing and a
      systematic shift at P4 would need the norms looked at (`docs/design/norm_shapes.md`).

**Done when:** median hip-series jitter is at least halved on the corpus with the P1→P4
excursion preserved, or the sweep shows it cannot be and the phase is closed as measured.

---


---

## Phase 5 — Motion-adaptive smoother window `[~]`

Follows from Phase 4's finding. Contract: `metric_presentation_honesty_phase5_contracts.md`.
Run unattended per Mark's instruction (5 Sept): the C15 gate decides promotion.

- [ ] **5.0** `tools/metrics/hip_accel_reference.py`: hip-centre acceleration on the smoothed track
      per swing window (still address / backswing / downswing / post-impact) → `aRefPxS2`.
- [ ] **5.1** The hook: `Kf3::predict(dt, qScale)`, per-frame qScale into `smoothKeypoint`, rts
      returns the smoothed acceleration. Byte-identical when off.
- [ ] **5.2** Two policies behind `poseSmooth.adapt.*` (mode off|accel|innov, group legs|body,
      minScale, aRefPxS2, expo, leadFrames, innovRef, innovRun), tests per C14.
- [ ] **5.3** Sweep tooling: `sweep_adapt.sh` + `sweep_summary.py --gate` printing C15.
- [ ] **5.4** Bake-off on the 11-swing subset with a control; parity at off.
- [ ] **5.5** Promote the winner for `legs` if the gate passes; else nothing promoted, reason
      recorded. Design §5.4 updated either way.


---

## Phase 6 — Draw the windowed mean `[ ]`

Agreed with Mark 5 Sept ("I think the chart should draw the windowed mean"), after Phase 5.
Rationale: Phases 1–3 deliberately never changed the drawn line ("one curve"), so the wobble
looks the same to the eye. Drawing the SAME 40 ms centred mean (with the ≥3-sample widening) that
the PEAK tile already reduces on keeps "one curve" honest: line and numbers come from one
reduction, and the raw samples stay visible as faint dots behind the line.

- [ ] **6.1** `ChartMetrics::windowedMean(t_us, value, valid)` in C++ (delegating to
      series_reduce's centred-mean at every valid sample; invalid samples excluded and kept
      invalid), exposed to QML once per data change.
- [ ] **6.2** `PpChartPlot.qml` draws the mean as the trace; raw samples as dots at low opacity
      (a `showRawDots` default on); invalid runs still dashed; hover reads the mean and shows the
      raw sample in the tooltip; the σ ribbon follows the mean.
- [ ] **6.3** `PpSegmentBrush.qml` sparkline uses the same mean.
- [ ] **6.4** Probe prints mean-vs-raw at the peak index; tests pin that the tiles equal the drawn
      line at the peak (PEAK == max of the drawn mean inside the window).
- [ ] **6.5** One windowed look by Mark.

## Side items (not gating any phase)

- [x] `PpChartSummary.qml` text overflow: the 2×2 grid's value texts overrun their cells at
      narrow widths (second screenshot of 2026-09-04). Give the value `Text`s
      `Layout.fillWidth` + `elide`, or reduce the unit token. Independent fix, own commit.

---

## What is shared, and must not be re-litigated per phase

- **One curve.** No display-only smoothing anywhere in QML, in any phase. If a curve needs to
  look different, the producer or the reducer changes and the swing.json changes with it.
- **Absent, never a sentinel.** A gated frame is absent from the channel; a bridged grid
  sample is marked invalid; neither is a 0, a NaN, or a clamp.
- **`sigma` absent means uncharacterised.** Never write 0.
- **Reducers live in `src/Analysis`.** The chart is one consumer of two.
- **Corpus runs need a control.** Non-deterministic pose; no exceptions.

---

## Log

- **2026-09-04** — Design doc and this plan written from the Plumb Bob screenshot and a probe of
  `2026-08-18_Mark-Liversedge_Wrist_01/swing_0001`. Baseline (that swing, whole-swing window):
  hipLineTilt PK RATE 291°/100 ms, range −85..+16°; shoulderPlaneAngle Top sample +88°;
  pelvisSway PK RATE 39 %/100 ms, range −19..+42 %; plumbBob PK RATE 11 in/100 ms. Nothing
  built.

- **2026-09-04 (Phase 0 done)** — `tools/metrics/series_noise.py`; CSVs in `docs/validation/data/`
  (`series_noise_baseline_corpus.csv`, run at 72bfd42; 108 files, 83 carry body metrics — the
  other 25 are the LM-only `2026-08-04_Wrist_05` session). Corpus medians (p95 jitter / PK RATE
  whole / still-address / P1–P7): hipLineTilt 8.1° / 322 / 4.8 / 30; shoulderPlaneAngle 10.2° /
  375 / 4.0 / 347; pelvisSway 3.5 % / 38 / 7.3 / 24; plumbBob 1.6 in / 13.8 / 3.0 / 8.0.
  **Gate evidence:** hip span ratio at P4 median 0.76 (min 0.64), at P7 median 1.00; only 2/83
  swings have any P1–P7 hip instant below 0.40, both at P7 and both with an absurd tilt (−58°,
  −23°) — 0.40 is right for the hips. **Shoulders are different:** P4 ratio median 0.32 (IQR
  0.22–0.44); 55/83 swings sit below 0.40 at the Top, ~1/3 of the P1–P7 curve is below it. At
  0.40 the graded Top `shoulderPlaneAngle` sample is absent on two thirds of the corpus. That is
  what a face-on camera can honestly say about a full turn; flagged for Mark at the Phase 1
  checkpoint (the alternative, 0.20, still cuts ~30 swings and admits ~7° of angle error).
  Also: the P1–P7 domain alone does NOT fix the body-line angles (their degeneracy is at P4,
  inside the domain) — only the gate does; still-address PK RATE fails §7 item 2 corpus-wide
  today (that is Phase 2's job; the 2-unit target will be re-judged there); five old swings
  show shoulder ratios of 1.5–3.6 (a collapsed address denominator — ratio gates have no upper
  bound; noted, out of scope); grid spacing corpus-wide has median dt_max 81 ms, so the 60 ms
  bridge floor will mark runs on most swings and the byte-identical parity set will be small —
  the Phase 1 gate compares `value[]` directly instead.

- **2026-09-04 (Phase 1 built, gate pending)** — Three Opus workstreams (Analysis / catalogue+
  diagnostics / Gui) plus three adversarial reviews; 14 suites green through ctest in
  `build/tests`. Decisions taken by the orchestrator, for Mark's review at the checkpoint:
  (1) **shoulder gate 0.40 kept** although Phase 0 shows the Top shoulder ratio at median 0.32:
  on most swings `shoulderPlaneAngle`, `spineSideBend` and `trailElbowHeight` (whose ONLY
  scored phase is Top) have no P4 reading — absence, per the design; the alternative worth
  considering is a foreshortening-corrected tilt `asin(dy / L_address)`, which is a definition
  change and Mark's call. (2) `trailElbowHeight` joined the shoulder gate (same 1/dx, and
  unbounded); the elbow-line gate became an ABSOLUTE floor `upperBody.minElbowSpanPx = 25`
  because a ratio against the address span (where the elbows are narrowest) can never fire.
  (3) Bridge budget is now `max(60 ms, 1.5 × local grid spacing)` so one dropped frame in the
  sparsely posed address is a hold, not a fabrication; two adjacent dropped frames still bridge,
  three mark the middle. (4) The one out-of-domain measure `m_pelvisSwayFinish` and its two
  signals were DELETED (not retired — `notCapturable` is a statement about the metric and the
  integrity test rightly refused it); `weight_back_at_finish` is now `confirmedBy: asserted`
  with no detector (its in-domain content is covered by `sig_hangingBackPelvisDown`), and
  `off_balance_finish` keeps its comOverLeadFoot detector. A coverage regression, the honest
  one; Mark to confirm. (5) The domain check is armed in `diagnosticsHealth()` as an Error row
  and in the integrity test's rung sweep; it is NOT enforced at pack load (would need catalogue
  plumbing into `merged_pack_provider` — tracked, not done). (6) Default (whole-swing) domains
  clip nothing: `domainFor` reports per-side narrowing and the chart clips only a side the
  manifest moved. (7) `bandAtNearest` lost its "good" default and gained a 20 ms tolerance;
  @IMPACT reads "—" when the impact sample is bridged. (8) `channel.maxBridgeUs < 0` is the
  off switch and is now guarded in both modules. Side item (summary-card overflow) fixed.

- **2026-09-04 (Phase 1 gate + probe; committed a290146 + follow-up)** — Gate on the 11-swing
  2026-08-18 subset, RelWithDebInfo `swinglab_run` before (clean 72bfd42 worktree) vs after,
  control byte-identical (`tools/metrics/compare_runs.py`, `subset_pass.sh`): `value[]`/`t_us[]`
  differ ONLY in the five gated channels (hipLineTilt, shoulderPlaneAngle, elbowAlignment,
  spineSideBend, trailElbowHeight), and only on gated frames, which now carry the bridge and a 0 in
  `valid`; nothing else in `analysis` moved. Masks appeared on 4 ungated channels from confidence
  holes with values unchanged. Phase samples removed: the out-of-domain Finish sample on all five
  narrowed upper-body metrics (11/11 — they had ALWAYS sampled Finish; producers now take a P1–P7
  list), the Top sample of shoulderPlaneAngle / spineSideBend / trailElbowHeight on 8/11 and
  elbowAlignment on 3/11 (the gate at the top, as Phase 0 predicted), hipLineTilt kept every
  P1–P7 sample on 11/11. One 1-ulp phase-sample change (trailElbowHeight P2, one swing) from a
  bridged neighbour. **Defect the gate caught:** the first cut marked gated frames inside the
  60 ms budget as valid and bridged them — a 10-frame post-impact hip-tilt run drawn as measured,
  a P4 shoulder plane of 86° emitted as a bridged 26°. Fixed: gated instants force 0 before any
  budget applies. **Probe** (`tools/probes/plumb_bob_chart.qml`, offscreen, re-analysed
  swing_0001): Plumb Bob preset resolves; sway card PEAK 41.5 → 21.8 % once clamped to P1–P7,
  hip tilt −29.5 → +16.0°, plumb bob 4.6 → −2.8 in; hipLineTilt carries a 9-frame invalid run
  after impact, shoulderPlaneAngle two runs (53 frames). Probe found the domain end resolving
  0.6 ms BEFORE the P7 sample (phase samples sit on the nearest frame): `_domainWindow` now
  snaps to the series' sample grid. Not exercised offscreen: the padded replay axis (no video
  decode), so the still-address window and dashed rendering want one windowed look.

- **2026-09-04 (Phase 2 WIP checkpoint — session limit)** — Committed UNBUILT and UNTESTED because
  the session limit arrived mid-phase. On disk: W1's header-only `src/Analysis/series_reduce.h`
  (reduceAt / reduceDelta / reduceExtremum / reduceRate, `SeriesView`, `viewOf`, `ReduceConfig`
  with an extra `minAtSamples`), `series_reduce_test.cpp` registered in
  `src/Analysis/tests/CMakeLists.txt`, `tuned::reduce` constants; W2's engine adoption (DONE per
  its report: reduceAt for phase values, reduceExtremum for spans over (lo, hi],
  `kPhaseGridSchemaVersion` 3, 80 test assertions, span constants re-derived, e.g. dip min −20 →
  −16.93); W3's chart adoption (report NOT received — chart_metrics.{h,cpp}, chart_metrics_test,
  PpChartSummary.qml, the probe may be mid-edit). Contract: scratchpad `phase2_contracts.md`,
  reproduced in the memory note. **Next session, in order:** build `build/tests` targets
  series_reduce_test, measure_sample_test, chart_metrics_test, diagnostics_catalogue_integrity_test,
  live_measure_source_test, model_browser_test; fix whatever W1/W3 left unfinished; ctest; two
  adversarial reviews (Analysis reducers; chart+engine adoption); gate 2.5 = probe STILL ADDRESS
  rate on the 08-18 subset (target < 2 units/100 ms) + card-vs-engine agreement test; commit.

- **2026-09-04 (Phase 2, W1 report received after the WIP commit)** — reducers are header-only
  (`series_reduce.h`, Qt-only via swing_analysis.h, no .cpp — do NOT add one to any CMake list;
  `ReduceConfig` gained `minAtSamples`). W1 verified every fixture with a Python replica; not
  compiled. **Two design points for Mark before gate 2.5:** (1) the least-squares `reduceRate` is
  NOT spike-proof — a single 99 in a ramp still yields ~100/100 ms (12× better than adjacent-frame,
  with σ≈57 flagging it); a spike-proof rate needs Theil–Sen or a residual gate, a design change
  not a tuning; §7 item 2 is about STILL windows, which pass comfortably. (2) the centred
  extremum window is not clamped to [from, to], so a window-edge mean can borrow 20 ms from
  outside a phase domain (where the degenerate readings live) — recommend clamping; W2's spans
  inherit it. Tracker boxes 2.1/2.2/2.4 ticked on the workers' word; 2.3 (W3 chart) report NOT
  received; 2.5 gate not run.

- **2026-09-04 (Phase 2, W3 report received; all three workers complete, NOTHING BUILT)** — chart
  adoption landed: summaryMasked lifts once, delegates every reduction, new keys peakSigma /
  rateSigma / rateOk / tRateUs; PK RATE shows "—" when no window qualifies and displays the
  MAGNITUDE of the signed rate (what the tile always answered); probe prints a STILL ADDR row
  with a §7-item-2 PASS/FAIL line. Fixtures rescaled to 8 ms / 27 ms (the old 1 ms fixture was
  shorter than every window). **W3's four flags for the next session:** (1) §7 item 2's
  "< 2 units/100 ms" is a property of the SERIES' residual σ (needs ≲ 0.5), not of the reducer —
  a σ≈0.9 still series yields |rate| ≈ 2.0 with rateSigma ≈ 2.1; (2) the fallback→partial rule
  fires with no mask when a window edge is > 15 ms outside the data extent, so a pre-validity
  swing can wear a PARTIAL chip — consider gating it on a mask or on the edge being inside the
  extent; (3) min/max no longer include the interpolated edges, and `PpSegmentBrush.qml`
  normalises the RAW curve to st.min/st.max, so unmasked spikes now overshoot the sparkline
  strip — needs raw extremes or a clip there; (4) the unclamped extremum window lets a P1–P7
  PEAK borrow 20 ms past Impact — clamp in reduceExtremum. Resume = build + ctest first.

- **2026-09-04 (Phase 2 resumed after the limit)** — all six suites built and green after one
  fixture constant moved (m_pelvisSwayBack −28.60 → −28.36, a one-frame trough pulled toward
  its neighbours). Two adversarial reviews: reducers (R1–R11) and chart+engine (11 items). The
  headline finding: the card and the engine still disagreed on 36/170 extremum cases on
  rich_7iron (up to 9.4°) because the engine seeded aggregates with ±15 ms endpoint medians and
  kept spans half-open — fixed (closed spans, no seed, zero-width spans skipped, schema 4,
  agreement asserted unconditionally over 12 cases). Then a structural conflict surfaced:
  clamping the extremum's support to [from,to] (R1) breaks the span cache's agreement
  (20/514). **Resolution recorded in design §5.2:** support unclamped; the domain becomes a
  MASK set by the ten narrowed producers (out-of-domain samples valid=0); window widens to ≥3
  samples in sparse regions; peak σ is a residual standard error. Also: sparkline strip clamps
  its y; `partial` fires only when a mask exists; new `edgeOk` gates PEAK/Δ on a series with no
  valid sample; sidecar guard records its windows. **Gate 2.5 (probe, 5 swings of 08-18
  Wrist_01, still-address window):** 15/20 series under 2 units/100 ms; the 5 above it have
  rate ≫ its standard error and a consistent trail-ward pelvis drift on all five swings — real
  pre-takeaway motion, not noise, so §7 item 2 must be re-judged against rateSigma (target
  proposal: |rate| < 3·rateSigma OR < 2 units) rather than a fixed number. Producer change means
  the subset after-pass must be re-run before commit.

- **2026-09-04 (Phase 2 CLOSED)** — After the resolution: 13 suites green (`series_reduce_test`,
  the two producer suites, `measure_sample_test` with the unconditional 12-case card-vs-engine
  agreement, `chart_metrics_test`, integrity, live-measure, model browser, packs). One test bug
  found on the way (a pointer into a temporary vector). Subset gate (11 swings, before = 72bfd42
  worktree, after = this tree): the producer domain mask (post-Impact tail only — the head stays
  open because the chart never clips a domain that starts at Address and the still-address
  window lives there) changes NO value and NO phase sample; it adds ~85 post-impact zeros on the
  ten narrowed channels. Probe on 5 re-analysed swings: no §5.1 violation, CLAMPED cards
  partial=false on 15/20 — the 5 partials are shoulderPlaneAngle, whose gate fires INSIDE the
  domain at the top (honest). Still-address gate: 16/20 under 2 units/100 ms; the 4 above are
  pelvisSway on three swings (2.4–3.0 %/100 ms) and hipLineTilt on one (4.4°/100 ms), each with
  rate ≫ rateSigma and a consistent trail-ward pelvis drift across all five swings — a real
  pre-takeaway move; §7 item 2 should be restated as |rate| < max(2 units, 3·rateSigma) or judged
  on a window the golfer is actually still in. Open design flags carried forward: the
  least-squares rate is not spike-proof (Theil–Sen / residual gate would be), and the
  reduce.* keys have no runtime override plumbing yet.

- **2026-09-04 (Phase 3 built and probed; review of the propagation pending)** — σ propagated
  first-order from the smoother's per-keypoint posterior (`PoseKpAux::sigma` = RMS of the x and y
  posterior variances, used unchanged for one-axis formulas) through each channel's own formula,
  median over VALID frames after the full mask; 14 of 15 body series carry σ (`leadArmToTorso`
  unset: an unsigned acos has no symmetric ±). The contract's forms for the three projected-
  distance channels understated by 2.5–4.2× (they dropped the ankle-line rotation term h·dφ);
  the full gradient is implemented and the tests re-derived (plumb bob 0.111 → 0.472 in on the
  fixture). Display: readings are QUANTISED to the step (nicest of 1/2/5×10ⁿ ≥ σ, floor 1
  unit), uncertainties are QUOTED (one decimal, "± <0.1" below 0.05, never "± 0.0") — the
  review showed a step-quantised ± inflated a 3.0 slope error to "± 5" and that PEAK's step
  branch was unreachable (peakSigma ≈ σ/√k). `seriesSigma` is the one σ-at-the-boundary rule;
  the ribbon is dark (`showSigmaBand`) and extends the axis by max σ when on. Subset pass:
  values untouched, only `sigma` keys appear (15 series). Probe on the re-analysed 08-18
  swing_0001: sway σ 0.88 % (step 1), hip tilt 1.87° (step 2), plumb bob 0.35 in (step 1),
  shoulder plane 1.35° (step 2); card text e.g. hip tilt `PEAK +16 ± 0.2`, `PK RATE 23 °/100ms
  ± 1.0`, chip `± 1.9°`; ribbon drew and closes inside the axis on all three facets.
  **§8 open question 1 answered by the data:** the step rule costs multiples of 2° on the
  body-line angles and nothing on the lateral metrics — recommend keeping it; Mark to confirm
  with one windowed look (the ribbon stays dark until seen).

- **2026-09-04 (Phase 3 CLOSED)** — Adversarial review re-derived all eleven propagations and
  found them exact; it established that the smoother's per-keypoint scalar IS σ_x = σ_y
  bit-for-bit (both axis filters share q, dt, R and the accept flag), now pinned as a comment in
  pose_smoother.cpp. Fixes landed: σ behind the `maxBridgeUs < 0` parity off-switch; tilted
  fixtures pin Euclidean L and the √(1+s²) factor (4.4 % discriminators); distinct per-joint σ
  run under both handednesses pin every asymmetric coefficient; the σ primitives (quad2,
  lineTiltSigmaDeg, medianSigmaOverValid) live once in metric_channel.h with an explicit
  "no σ track" pointer. Correction to my own note: dropping the address-reference σ costs
  ≈12 % at N_eff ≈ 6 (autocorrelated residuals), not 0.3 % — recorded in the code. 11 suites
  green. Uncertainties are quoted, readings quantised; `formatUncertainty` takes no step.
  Carry-forward: the ribbon stays dark until Mark sees it windowed; the step rule is
  recommended kept (costs multiples of 2° on body-line angles only).

- **2026-09-05 (Phase 4.1 built, 4.2 swept — 4.3 NOT promoted, Mark's call)** — `legsSigmaScale` /
  `legsJerkScale` over keypoints 11–16, default 1.0, dark keys `poseSmooth.legs*`; parity at 1.0
  byte-identical (11/11), and the params path is inert at 1.0 (11/11). Sweep on the 11-swing
  subset (`tools/metrics/sweep_legs_scale.sh`, data in `docs/validation/data/legs_scale_sweep/`),
  medians over swings:

  | scale | sway p95 jitter | sway σ | sway P1–P7 excursion | sway P4 | **sway P7** | hip tilt excursion | hip tilt P7 |
  |---|---|---|---|---|---|---|---|
  | 1.0 | 2.08 % | 0.87 | 38.7 | −22.6 | 12.7 | 23.2° | −7.4 |
  | 0.3 | 1.74 | 0.71 | 39.4 | −22.6 | 14.2 | 22.7 | −7.4 |
  | 0.1 | 1.39 | 0.60 | 41.2 | −22.6 | **16.2** | 21.5 | −6.2 |
  | 0.05 | 1.37 | 0.53 | 41.7 | −22.7 | 16.3 | 20.7 | −6.0 |
  | 0.02 | 1.31 | 0.46 | 42.6 | −22.8 | 16.7 | 19.9 | −5.8 |

  Jitter falls as the window law predicts (σ ∝ scale^(1/6)) and P4 is stable to 0.2 units at
  every scale, but the **P7 (impact) samples move by 3–4 σ at 0.1** (sway +3.5 %, knee drift
  −3.1 %, lift −1.7 %, plumb bob +0.6 in) and the hip-tilt excursion shrinks 7 %: the hips move
  fast through impact, a 70 ms window blends the post-impact rotation into the impact frame, and
  the corridors are seeded at P7. So a global legs scale cannot buy jitter without biasing the
  impact reading; the honest lever would be a motion-adaptive window, out of scope. 0.3 is the
  only defensible candidate (jitter −16 %, P7 moves ≤ 1.7 σ on sway, < 1 σ elsewhere).
  **Recommendation: keep 1.0 (no promotion) and close Phase 4 as measured.** Mark to decide.

- **2026-09-05 (Phase 4 CLOSED, plan complete)** — Mark accepted the recommendation: default stays
  1.0, the dark keys and the sweep tooling ship. Design §7 item 2 restated (rate judged against its
  own standard error). Carry-forward, none blocking: one windowed look at the dashed runs, the
  step rule and the dark ribbon; a spike-proof rate (Theil–Sen or residual gate) if a spike ever
  shows in a PK RATE tile; runtime override plumbing for `reduce.*`; a full-corpus confirmation of
  Phases 1–3 on GOLFSIMPC when a Windows build is next made; a motion-adaptive smoother window
  as the honest route to less hip jitter.

- **2026-09-05 (Phase 5 built; 3-cell probe on real swings)** — Hook + both policies built and
  reviewed (a coasted step never takes the reduced q; pass 2 falls back to pass 1 on any change
  to accepted[]/hasSmoothed[] and counts it as `pose2d.adaptFallbacks`; keys clamped; aRef
  scaled by √(W·H)); parity at mode off byte-identical (11/11). aRef re-measured from the IMPACT
  side on the whole corpus (`hip_accel_reference.py` per-P columns): min(|a| at P6, P7) p05 5232
  px/s², P4 median 4378, so **aRef = 4000** keeps today's window from the top through impact on
  80/83 swings; the July sessions' addresses read as loud as their downswings (real setup
  motion) and correctly do not engage. Probe (control + expo 2/3 at minScale 0.05, 11 swings):
  P4/P7 moved ≤ 0.21 σ, excursion 1.00, σ ratio 0.99, no lost samples, **0 fallbacks**;
  still-address jitter ratio 0.83 (expo 2) / 0.78 (expo 3) — plumb bob 0.65, sway 0.80, hip
  tilt 0.88, so C4 (each ≤ 0.80) fails on hip tilt. Cause: at expo 3 the address sits at s ≈
  0.16 (a ~45–50 ms window); a steeper expo (5, 8) is the lever. Full sweep cut 3 running.
