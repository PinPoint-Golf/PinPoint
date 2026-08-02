# Metric Catalogue — developer guide (adding a metric end-to-end)

The **Metric Catalogue** owns metric *identity + metadata* (label, unit, meaning, requirements) and
abstracts *which producer computes each metric*. It does **not** own corridors — those are norm-set
content, resolved through `Diagnostics/metric_corridor.h`; see Step D. It is additive metadata
over the existing `MetricSeries` keys — no `swing.json` schema change. This guide is the recipe for
adding a new metric so it appears (correctly) in the directory and resolves per shot.

Companion process doc: [`analysis_pipeline_developer_guide.md`](analysis_pipeline_developer_guide.md)
(stage orchestration + §6.4 "three CMake touchpoints"). Read it first if you are also adding a new
*producer stage*.

## 1. The layer at a glance

```
src/Metrics/                          (pure, Qt-only value types — no Qt-GUI)
  metric_type.h                   MetricType { Summary, PointInTime, TimeSeries, Sequence }
  metric_descriptor.h             MetricDescriptor, MetricRequirement  (NO normative values — Step D)
  metric_provider.h               IMetricProvider seam, ShotContext, MetricAvailability
  metric_reducer.h                the reducer vocabulary a measure names alongside a metricKey
  metric_catalogue.{h,cpp}        MetricCatalogue value object + makeMetricCatalogue() factory
  metric_resolver.{h,cpp}         provider fusion + describeRequirement() reason renderer
  metric_catalogue_manifest.cpp   installMetricManifest() — the ONE list of every descriptor
  metric_providers.{h,cpp}        WristMetricProvider / KinematicSeriesProvider / FootMetricProvider
                                  / LowerBodyMetricProvider / UpperBodyMetricProvider
                                  / TrailWristProvider / BodyRotationProvider / ClubDeliveryProvider
                                  / TempoProvider / HeadMetricProvider / ShaftLeanProvider
                                  / ScoreProvider / PlannedMetricProvider / LaunchMonitorProvider
src/Analysis/                         (the PRODUCERS — what emits a MetricSeries)
  metric_extractor.{h,cpp}        the IMU wrist metrics; the rest are per-region stage files
  metric_channel.h                MetricChannel, the shared curve type the producers fill
src/Gui/review/
  metric_catalog.{h,cpp}          MetricCatalog : QObject (QML_ELEMENT) — QVariant façade
  chart_metrics.{h,cpp}           ChartMetrics::seriesGroups() — presets from MetricDescriptor.group
  MetricLibrary/Detail/Row.qml    the directory + detail screens
```

**The catalogue lives in `src/Metrics/`, not `src/Analysis/`** — it moved, and paths through this
guide follow. `src/Analysis/` keeps the producers, which is the split the layer is built on:
identity here, production there, and a descriptor that never names a producer.

Design invariants (do not break):
- **Identity is decoupled from production.** A descriptor never names a producer. A provider declares
  which descriptor keys it can satisfy and, per shot, at what quality.
- **No startup singleton / self-registration.** The catalogue is assembled on demand by
  `makeMetricCatalogue()` (mirrors `makeReferenceBandProvider(Kind)`), which installs the manifest
  and a fixed provider set. This matches the Analysis module's ban on stage/provider registration
  (`analysis_stage.h` anti-goals).
- **The full design catalogue — live + planned.** The manifest declares every metric in
  `shot_analyzer_design.md §A`, each either **live** (a producer emits it) or a **planned**
  placeholder (`.planned = true`, no producer yet). **70 descriptors; 54 live, 16 planned.**
  Live today: metric_extractor ×4, kinematic_series ×3 + shaft-lean, foot_metrics ×5 +
  ball_position ×1, head_track ×3, lower_body_metrics ×6, upper_body_metrics ×9,
  body_rotation ×4, club_delivery ×3, the trail-wrist series ×1, tempo_metrics ×2, the two
  wrist `Summary` scores (sourced from a `ScoreBreakdown`, not a `MetricSeries`), and the nine
  launch-monitor readings (a device, not a producer we write).
  Planned: the depth-axis metrics, the sagittal spine pair, the keypoints no pose layout carries,
  the sensors we do not place, `kinematicSequence`, and `swingScore`.

  **This bullet and Appendix A both mirror the manifest's own header comment — keep all three in
  step.** They drift the moment a producer lands without the roadmap being re-read, and a status
  table that says "planned" about a metric already on the chart is worse than no table: it sends a
  reader off to build something that exists. Audited 2026-08-02, against a table that had been
  carrying **43 rows for a 70-metric catalogue**: 29 metrics were missing (three whole groups —
  Arms, Ball flight, Strike — among them), 13 rows said "planned" about metrics that had shipped,
  2 named metrics the manifest has never declared, and four rows described units their producers had
  already stopped emitting. Appendix A is now checked key-for-key and status-for-status against the
  manifest; re-run that check rather than trusting a spot read.
- **ONE UNIT PER KEY, FOR ALL TIME.** A producer may not switch a key's unit per swing depending on
  whether a ruler resolved. Where the unit depends on one, emit the metric only when it resolves and
  leave it ABSENT otherwise — unavailable is a fact the app already renders honestly, a silently
  different scale is a confident wrong answer. This is not a style rule: a corridor declares one unit
  and grading compares the numbers without ever consulting it, so a per-swing unit is ungradeable in
  a way nothing reports. `leadHeelLift`, `headSway` and `headLift` each shipped this way and each
  failed silently — one could never fire, two fired on everything. `measureUnitMismatch` in the
  diagnostics health list now compares a measure's unit against its descriptor's (`Rate` reducers
  exempt) and is gated at zero over the shipped library.
- **Placeholders resolve "planned", not "missing sensors".** The `PlannedMetricProvider` claims every
  planned key and always returns `Unavailable` with reason `"planned — not yet produced in this
  build"`, regardless of the shot's capability — a planned metric's `.requirement` is documentation
  of *what it will need*, surfaced as "will need …" on the detail page and a **Planned** badge in the
  directory. **Promoting a placeholder to live:** add the producer, drop `.planned`, move the key out
  of `PlannedMetricProvider::provides()` into a real provider that returns `Measured` when capable,
  flip the matching `core.json` measures from `planned` to `live`, and update the
  metric_catalogue_test counts. Then **re-run `core_pack_test`**: a condition that could not fire
  before may now fire with nothing behind it, which is a defect in what ships rather than a backlog
  item, and that test is the thing that catches it.
- **`PlannedMetricProvider::provides()` is grouped by WHY, and keep it that way.** Depth, sagittal,
  no-keypoint-in-any-layout, sensors-we-do-not-place, and work-we-have-not-done are five different
  statements, and a flat list of keys loses all of them. A reader deciding what to build next needs
  to know which group a key is in before anything else.

Two easy traps:
- The descriptor member is `requirement`, **not** `requires` — `requires` is a C++20 keyword and
  cannot be an identifier.
- The producer `Phase` enum (Address…Finish, `swing_analysis.h`) is **distinct** from
  `PpSwingPosition` (P1…P8, `wrist_assessment_types.h`). Where you need to cross between them, the
  one canonical table is `wristCheckpoints()` (`wrist_analysis_adapter.h`) — reuse it, never
  duplicate the mapping. (The diagnostics pack speaks `Phase` throughout, so a corridor lookup never
  crosses; the wrist grid's cell measures encode the position in their id.)

## 2. Recipe — add a new metric

### Step A — make sure something produces it
A metric must have a live producer emitting a `MetricSeries` with your `key`. Producers are
`AnalysisStage`s pushed into `wristProfile()` / `cameraKinematicsProfile()` in
`src/Analysis/wrist_analyzer.cpp`; each writes into `ctx.series` (scored) or `ctx.detail->series`
(unscored). If you are adding a brand-new producer, follow the analysis-pipeline guide §6 first, then
come back here. Confirm the key, unit, `MetricType` shape, and which phases matter.

Shapes: `Summary` (one value), `PointInTime` (empty curve + one `PhaseSample`, e.g. foot address
scalars), `TimeSeries` (a curve), `Sequence` (ordered peak events — no v1 producer yet).

### Step B — declare the descriptor in the manifest
Add one `cat.addDescriptor({...})` block to `installMetricManifest()` in
`src/Metrics/metric_catalogue_manifest.cpp`. Fill every field; source `description`/`howToRead` from
`docs/` (the wrist/foot/speed prose lives in `docs/design/shot_analyzer_design.md`,
`docs/reference/wristmetrics.md`, `docs/reference/swing_json_schema.md`) — the manifest is where that
scattered prose becomes structured. `usedBy` is static, hand-authored (e.g.
`{"chart:review","score:wrist"}`). Keep `requirement.minTier` at `Angles2D` unless the metric
genuinely needs a higher *camera reconstruction* tier — IMU metrics do not.

```cpp
cat.addDescriptor({
    .key = QStringLiteral("myMetric"),
    .type = MetricType::TimeSeries,
    .label = QStringLiteral("My metric — long name"),
    .shortLabel = QStringLiteral("Short"),
    .unit = QStringLiteral("°"),
    .group = QStringLiteral("Wrist & forearm"),      // reuse an existing group where possible
    .description = QStringLiteral("What it means (from docs/)."),
    .howToRead   = QStringLiteral("Sign, when to read, what good looks like; what it needs."),
    .flexPositive = true,
    .phases = { Phase::Top, Phase::Impact },
    .scored = false,
    // No normative block — a descriptor describes a metric, it does not judge it. See Step D.
    .requirement = { .imuRoles = { SegmentRole::LeadForearm, SegmentRole::LeadHand } },
    .usedBy = { QStringLiteral("chart:review") },
});
```

### Step C — declare the provider capability
A metric is `Unavailable` until a provider claims its key. Either extend an existing provider or add a
new one in `src/Metrics/metric_providers.{h,cpp}`:
- Add the key to that provider's `provides()`.
- Handle it in `availability(key, ctx)`. Build the sensor/camera/club verdict from a
  `MetricRequirement` via the shared `fromRequirement()` helper so reasons read identically to the
  resolver's `describeRequirement()`.

> ### ⛔ NEVER READ `ShotContext::sessionType`
>
> There used to be a `wristSessionOk()` gate here, and this guide used to tell you to add one. It is
> gone and must not come back. Availability answers one question — *can this shot's data and devices
> support this metric* — and a session type is not evidence about that: it is what the operator
> meant to capture. The gate made half the catalogue answer *"produced in Wrist Motion sessions
> only"*, which a golfer reads as a statement about their equipment, and it silently produced
> nothing on swings that were recorded perfectly well under the wrong session.
>
> The field survives on `ShotContext` because `swing.json` carries it and callers pass it through.
> Reading it in a provider is a defect. The production side matches:
> `appendBodyMetricStages()` in `wrist_analyzer.cpp` lists the body-metric stages ONCE and every
> profile runs them, with each stage gating in its own `canRun()` on the data it actually needs.

**Returning `Bridged`.** Three states, and the middle one is not decoration. Use it when the metric
IS produced but by a weaker route than the ideal sensor — `BodyRotationProvider` is the worked
example: a bound `SegmentRole::Pelvis` stream measures axial turn directly (`Measured`), and with
only a face-on camera the same producer estimates it from the collapse of the hip span in the image
(`Bridged`). Two rules follow. The reason must name the **method**, not the missing device
("estimated from the face-on camera — a pelvis / thorax IMU would measure it directly"), because
"needs a pelvis IMU" reads as a refusal when a value is in fact produced. And the producer must
carry the cost: `body_rotation.cpp` propagates its span noise into `MetricSeries::sigma` so a reader
can see how much to trust a small turn.
- If you add a **new** provider class, register a process-lifetime instance of it in
  `makeMetricCatalogue()` (`metric_catalogue.cpp`) with `cat.addProvider(&yourProvider)`.

**Hardware the user may not own is a REQUIREMENT, not a `planned` flag.** `MetricRequirement` gained
`launchMonitor` alongside `faceOnCamera` / `clubTrack` / `ballTrack`, matched by
`ShotContext::hasLaunchMonitor`. The distinction matters in both directions and both mistakes are
lies: marking a launch-monitor metric `planned` promises work we are not doing, while claiming it
`Measured` unconditionally reports numbers nobody supplied. Declaring the requirement instead makes
the absence graceful through the path every other missing input already uses — "needs a launch
monitor" today, `Measured` the moment a connector sets the flag, with no catalogue change at that
point. `LaunchMonitorProvider` is that connector's insertion point rather than a stub to delete, and
the diagnostics pack's matching statement is `MeasureStatus::ExternalDevice`.

Resolution rule (in `metric_resolver.cpp`): over all providers that list the key, the best state wins
(`Measured` > `Bridged` > `Unavailable`), ties broken by `priority()`. If no provider claims the key,
it is `Unavailable` with the descriptor requirement rendered as the reason.

### Step D — the corridor (a norm, not a descriptor field)

**A metric descriptor carries no normative values.** `MetricNormative` — the `dof` delegation, the
`inlineCorridors`, the `contextNote`, the `heuristic` flag — was deleted at stage 9 of the
diagnostics-norms work, along with `MetricCatalogue::corridor()`. A corridor is now **content**:

1. **The pack needs a measure that reads your metric.** A measure names your `metricKey` plus a
   **reducer**, and the reducer is where the phase lives (`at p7`, `delta p1→p4`, `extremum p5..p6`).
   Author it in `src/Resources/diagnostics/core.json`, or through Settings → Diagnostics → Measures,
   which mints one and refuses to do so without a `highMeans` sentence.
2. **The norm set needs a row for that measure**, at a context: `mu`, `sigmaLo`/`sigmaHi`, optional
   absolute `monitorLo`/`monitorHi`, `unit` (which must match the measure's — the loader refuses a
   mismatch), `source` and a `citation` where the figure is provisional. Author it in
   `src/Resources/diagnostics/norms.json` or seat it from real swings in the corridor editor.
3. **Nothing else is needed.** Every metric surface — `MetricDetail`, `PpBandRail`, the dashboard
   Motion / Setup / Verdict zones — resolves through `corridorForMetricAtPhase()`
   (`src/Diagnostics/metric_corridor.h`) via `MetricCatalog::descriptor()`, in the shot's own
   context. With no measure or no norm it renders "no norm for this metric yet" rather than a fake
   band.

Two rules worth knowing before you author the measure, because both decide what a surface draws:

- **The corridor is found through the REDUCER, not through the metric's `phases` list.** A measure
  whose reducer names a phase the metric does not declare resolves nothing and the corridor silently
  disappears — that was ledger C20, live for two stages on `m_tempoRatio`. Make the reducer name the
  phase the PRODUCER labels.
- **`at` beats `delta` where both name one phase**, because a corridor keyed on a phase means the
  absolute reading there. `leadWristFlexExt` has both at P7 and the absolute one wins.

Club-dependence is a **context**, not a note: put per-club rows under `driver` / `iron` / `wedge` in
the tree and they resolve automatically, inheriting `full_swing` where you author nothing.

### Step E — CMake (only if you added files)
Manifest/provider edits need no CMake change. New `.cpp` files reach three targets (analysis guide
§6.4): (1) the app — `target_sources(PinPointStudio PRIVATE …)` in the root `CMakeLists.txt`;
(2) the offline stack — `_pinpoint_offline_sources` (or `swinglab_run`/parity link-diverges);
(3) the unit test — `pp_add_test(...)` in `src/Analysis/tests/CMakeLists.txt`. The catalogue's own
sources are already wired.

### Step F — extend the unit test
Add cases to `src/Metrics/tests/metric_catalogue_test.cpp` (the source lives beside the catalogue;
the `pp_add_test` that registers it is in `src/Analysis/tests/CMakeLists.txt`):
- **completeness**: your key resolves via `descriptor()`; bump the expected count and the per-type
  counts.
- **a claimant**: some provider lists your key. There is no blanket assertion that every non-planned
  descriptor is claimed by one, and `stanceWidthMm` has been produced-but-unclaimed — and therefore
  permanently `Unavailable` — for exactly that reason. Adding the blanket case is worth more than
  adding your own.
- **resolve()**: a `ShotContext` where it is `Measured`, and one where it is `Unavailable` with the
  right reason (**missing sensor or camera — never a session type**; see the ⛔ box in Step C).
- **corridors**: nothing goes in this test — the catalogue no longer resolves them. If your metric
  gains a norm, add a case to `manifest_migration_test` (`src/Diagnostics/tests/`) asserting it
  resolves a corridor at every phase it declares. That is the gate that catches a reducer naming a
  phase the metric does not.

Build + run (targeted, `--parallel 4` per the box's memory cap):
```bash
cmake --build build/analyzer-tests --target metric_catalogue_test --parallel 4
ctest --test-dir build/analyzer-tests -R metric_catalogue --output-on-failure
```

### Step G — verify in the app
Build the app once, open **Settings → Metrics**, confirm the metric appears in its group with the
right unit/short-label and source glyph, and that its detail page renders the meaning, how-to-read,
normative bar (for any metric with a norm), requirement, and usedBy. Run the full 7-suite gate
before any release.

## 3. QML façade shapes (for UI work)

`MetricCatalog` (`src/Gui/review/metric_catalog.h`, `QML_ELEMENT`) marshals registry types to
QVariant. Row shape from `query(filters, shotCtx={})`:
`{ key, label, shortLabel, unit, type, group, scored, sources:[…], availability:{state,reason,tier} }`.
Detail from `descriptor(key, shotCtx={})` adds `description, howToRead, flexPositive, phases:[int…],
normative:{…}, requires:{…}, usedBy:[…]`. **Phases are Phase ints** — render with `TimelineLabels`.

The `normative` map is resolved out of the NORM SET, not the descriptor:
`{ corridors:[{phase,greenLo,greenHi,amberLo,amberHi,deltaFromAddress,measureId,contextId,inherited,overridden}],
measureId, contextId, contextLabel, inherited, overridden, source, sourceLabel, n, citation, weak,
weakReason }`. A metric with no measure or no norm gets an empty `corridors` list — draw the
"no norm yet" state, never a zeroed band. The Watch edge (`amberLo/amberHi`) is policy-dependent for
any norm that states no monitor band, which is why the host must bind
`gradePolicy: appSettings.diagnosticsGradePolicy` on the `MetricCatalog` it declares. `shotCtx` (all optional): `{ tier, sessionType, imuRoles:[roleName…],
hasFaceOn, hasClubTrack, hasBallTrack, archetype, club, shape }`; empty `{}` = the context-free
directory view.

## 4. Deferred (not in v1)

The new-dashboard rewrite (query-driven zones); the kinematic **Sequence** producer; wiring a live
swing adherence scorer so `swingScore` becomes Measured; retiring `ChartMetrics::shortLabel` once the catalogue is
the single source of short names.

*(2026-07-21: `tempoBackswing` / `tempoRatio` / `ballPosition` left this list — all three now have
producers.)*

*(2026-07-27: a "non-DOF band provider" also left it, because the question dissolved. Corridors are
norm-set content for every metric, DOF or not — `m_tempoRatio` and `m_stanceWidth` are ordinary norm
rows beside the 39 migrated wrist cells. If you need a band for a speed, author a measure and a
norm.)*

## Appendix A — per-metric work plan (capture · detection · calibration · V&V)

Every metric in the manifest, one row each, in manifest order. For **live** metrics the cells
describe what is already in place (the producer + its test); for **planned** metrics they describe
the outstanding work; **device** rows have no detection work at all. This is a roadmap, not a
contract — effort estimates live in the per-feature plans, and every "validate" step is corpus-scale
(a single labelled swing is development data only). Promote a planned metric only when all four
columns are satisfied.

The table is **exhaustive by construction** — every descriptor appears exactly once, with a status
matching its flags. If you add a metric, add its row; if you promote one, flip its status here too.

**Legend.** Capture: `F/H/U` = lead forearm/hand/upper-arm IMU · `Plv/Thx/Thg` = pelvis/thorax/thigh
IMU · `FaceCam` = face-on whole-body camera (pose) · `DTL` = down-the-line camera (depth axis) ·
`Club` = shaft/club track · `Ball` = ball track · `LM` = connected launch monitor · `Phases` =
segmentation phase events.
Calibration: `anat+mount` = IMU anatomical zero + mount check ([[calibration-state-signals]]) ·
`camCal` = camera intrinsic/extrinsic (`cameraFixedInPlace`) · `ground` = ground-plane · `px→mm` =
ball-scale (`setup.ballDetection`) · `stereo` = DTL/stereo extrinsics · `clubDev` = club-device mount.
V&V: unit = header-only standalone test (`src/Analysis/tests`); validation source in parentheses.

**Status** is read off the descriptor — `.planned` for the badge the directory shows, and
`.requirement.launchMonitor` for the third row, which is a requirement rather than a roadmap item:

| Status | Means |
|---|---|
| **live** | A producer we write emits it. Resolves Measured on a shot that meets the requirement. |
| **device** | Real and claimed by `LaunchMonitorProvider`, but the *reading* comes from hardware we integrate rather than a producer we author. Not on the roadmap below — there is no detection work to do, only a connector. |
| **planned** | `.planned = true`. `PlannedMetricProvider` claims it and always answers `Unavailable — "planned — not yet produced in this build"`, whatever the shot can do. |

Groups below are in manifest order, which is the order the Metric Library and the chart's metric
presets list them in.

### Score

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `wristScore` | live | F+H (+U) | WristAssessmentEngine rollup ✓ | anat+mount | `composite_score_v2_test` · (corpus: score stability) |
| `wristResemblance` | live | F+H | WristResemblanceScorer ✓ | anat+mount | `wrist_resemblance_test` · (corpus: per-archetype) |
| `swingScore` | planned | Plv+Thx + FaceCam | wire a live adherence scorer (SwingScorer is dark) | anat+mount, camCal | `swing_scorer_test` (exists) · (corpus: adherence vs coach) |

### Wrist & forearm

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `leadWristFlexExt` | live | F+H | MetricExtractor Cardan-1 ✓ | anat+mount | `wrist_angles_test` · per-rig sign ("check your sensors") · (corpus) |
| `leadWristRadUln` | live | F+H | MetricExtractor Cardan-2 ✓ | anat+mount | `wrist_angles_test` · (corpus; weakest IMU axis ~5°) |
| `forearmPronation` | live | F+H+U | MetricExtractor twist ✓ | anat+mount | `wrist_angles_test` · (corpus) |
| `leadArmFlexion` | live | F+H+U | MetricExtractor elbow angle ✓ | anat+mount | `wrist_angles_test` · (corpus) |
| `trailWristFlexExt` | live | FaceCam (WholeBody hand keypoints) | `buildTrailWristSeries` apparent camera-plane angle ✓ | camCal | `pose_wrist_angle_source_test` · (corpus) |

### Body rotation

Landed camera-first: `BodyRotationProvider` returns **Bridged** off a face-on camera and reserves
Measured for the pelvis / thorax IMUs, so these read as real-but-estimated rather than absent.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `pelvisRotation` | live | FaceCam (Plv IMU upgrades it) | `buildBodyRotationSeries` axial turn ✓ | camCal / anat+mount | `body_rotation_test` · (mocap ground truth owed) |
| `thoraxRotation` | live | FaceCam (Thx IMU upgrades it) | axial-turn channel ✓ | camCal / anat+mount | `body_rotation_test` · (mocap owed) |
| `xFactor` | live | FaceCam (Plv+Thx upgrade it) | thorax−pelvis separation ✓ | camCal / anat+mount | `body_rotation_test` · (mocap owed) |
| `xFactorStretch` | live | as `xFactor`, + a segmented Top | separation minus its value at Top ✓ | camCal / anat+mount | `body_rotation_test` · (corpus: speed correlation owed) |
| `shoulderPlaneAngle` | live | FaceCam | `buildUpperBodySeries` shoulder-line vs horizontal ✓ | camCal | `upper_body_metrics_test` · (corpus) |
| `hipInternalRotation` | planned | Plv+Thg | thigh-vs-pelvis twist | anat+mount (pelvis+thigh) | new unit · (mocap) |

### Spine & tilt

Trunk ANGLES. Split from the old "Spine & pelvis" alongside `Pelvis & lateral` below: `.group` is
what the chart's metric presets are derived from (`ChartMetrics::seriesGroups`), so a group is also
the set a reader plots together, and ten members made it unreadable.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `spineSideBend` | live | FaceCam | `buildUpperBodySeries` lateral flexion ✓ | camCal | `upper_body_metrics_test` · (mocap owed) |
| `secondaryAxisTilt` | live | FaceCam | frontal spine vector vs vertical ✓ | camCal, ground | `upper_body_metrics_test` · (mocap owed) |
| `spineForwardBend` | planned | Plv+Thx (or 3D cam) | thorax-rel-pelvis flex — SAGITTAL, so face-on cannot see it | anat+mount / camCal | new unit · (mocap) |
| `thoracicFlexion` | planned | Plv+Thx (or 3D cam) | thoracic flex at Address — sagittal | anat+mount / camCal | new unit · (mocap) |
| `lumbarExtension` | planned | Plv+Thx (or 3D cam) | lumbar extension at Address — sagittal | anat+mount / camCal | new unit · (mocap) |

### Pelvis & lateral

Centre TRANSLATIONS. `hipLineTilt` is named for an angle but belongs here: it is a pelvis reading in
the same frontal plane as sway and lift, and it is read alongside them.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `pelvisSway` | live | FaceCam + ground | `buildLowerBodySeries` lateral translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `pelvisLift` | live | FaceCam + ground | vertical translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `hipLineTilt` | live | FaceCam | hip-line angle vs horizontal ✓ | camCal | `lower_body_metrics_test` · (corpus) |
| `thoraxLateralDrift` | live | FaceCam + ground | `buildUpperBodySeries` thorax lateral translation ✓ | camCal, ground | `upper_body_metrics_test` · (corpus) |
| `pelvisThrust` | planned | **DTL** (optical axis) | toward-ball translation | stereo | new unit · (mocap; needs depth) |

### Feet & stance

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `stanceWidth` | live | FaceCam | `buildFootSeries` heel-to-heel ÷ shoulder width ✓ | none — a body-relative ratio. **ABSENT when the shoulders do not resolve**, never re-expressed in another unit | `foot_metrics_test` · distribution owed (no corridor yet) |
| `stanceWidthMm` | live | FaceCam + Ball | same span through the ball-diameter ruler ✓ | **px→mm** | `foot_metrics_test` · **see the defect note below** |
| `ballPosition` | live | FaceCam + Ball | ball projected on the heel line ÷ stance width (`ball_position.cpp`) | none — a same-plane ratio, scale-free | `ball_position_test` · per-club distribution owed |
| `leadFootFlare` | live | FaceCam | foot heel→bigtoe angle ✓ | none | `foot_metrics_test` · (corpus) |
| `trailFootFlare` | live | FaceCam | foot heel→bigtoe angle ✓ | none | `foot_metrics_test` · (corpus) |
| `toeLineAngle` | live | FaceCam | bigtoe→bigtoe line angle ✓ | none | `foot_metrics_test` · (corpus) |
| `leadHeelLift` | live | FaceCam + Ball | heel-vs-toe elevation curve, in **cm** ✓ | **px→mm**; absent without the ruler | `foot_metrics_test` · (corpus) |
| `leadKneeDrift` | live | FaceCam + ground | `buildLowerBodySeries` lead-knee lateral travel ✓ | camCal, ground | `lower_body_metrics_test` · (corpus) |
| `comOverLeadFoot` | live | FaceCam + ground | balance point vs lead foot, sampled to Finish ✓ | camCal, ground | `lower_body_metrics_test` · (corpus) |
| `leadKneeFlexion` | planned | FaceCam (DTL better) | knee angle — sagittal, face-on cannot see it | camCal / stereo | new unit · (mocap) |
| `trailKneeFlexion` | planned | FaceCam (DTL better) | knee angle — sagittal | camCal / stereo | new unit · (mocap) |
| `ballBodyDistance` | planned | **DTL** + Ball | ball standoff from the body — depth axis | stereo | new unit · (corpus) |

> **Known defect (2026-08-02).** `stanceWidthMm` is declared in the manifest and emitted by
> `foot_metrics.cpp`, but **no provider claims it** — it is absent from `FootMetricProvider::provides()`
> and from `PlannedMetricProvider`. `MetricCatalogue::resolve()` therefore answers `Unavailable` for
> every shot, so a metric that is genuinely produced never appears as Measured in the directory. The
> fix is one entry in `FootMetricProvider::provides()` (plus a `ballTrack` capability check, since the
> mm reading needs the ruler). Nothing catches this today: `metric_catalogue_test` asserts descriptor
> counts, not that every non-planned descriptor has a claimant — a gap worth closing at the same time.

### Club & speed

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `clubheadSpeed` | live | Club | `buildKinematicSeries` head-path speed ✓ | px→mm / ground | `kinematic_series_test` · (launch monitor) |
| `handSpeed` | live | Club (grip) | grip-path speed ✓ | px→mm | `kinematic_series_test` · (launch monitor) |
| `lagAngle` | live | Club + FaceCam pose | forearm-vs-shaft angle ✓ | px→mm, pose | `kinematic_series_test` · (strobe/montage review) |
| `impactShaftLean` | live | Club | shaft-lean stage ✓ | px→mm | `shaft_*` tests · (corpus) |

### Club delivery

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `shaftAngleVsHorizontal` | live | FaceCam + Club | `buildClubDeliverySeries` shaft vs horizontal ✓ | px→mm | `club_delivery_test` · (corpus) |
| `attackAngle` | live | FaceCam + Club | vertical velocity angle at Impact (`PointInTime`) ✓ | px→mm | `club_delivery_test` · (launch monitor) |
| `lowPointAhead` | live | FaceCam + Club + Ball | arc low-point vs ball (`PointInTime`) ✓ | px→mm | `club_delivery_test` · (corpus) |
| `faceAngle` | device | **LM** | read from the monitor | — | (launch monitor) |
| `dynamicLoft` | device | **LM** | read from the monitor | — | (launch monitor) |
| `spinLoft` | device | **LM** | read from the monitor | — | (launch monitor) |
| `swingPlane` | planned | Club (DTL best) | SVD best-fit plane of head path | camCal | new unit · (DTL cross-check) |
| `clubPath` | planned | **DTL** + Club | horizontal velocity angle — needs depth | stereo | new unit · (launch monitor) |
| `shaftDirection` | planned | **DTL** + Club | shaft pointing vs target line — needs depth | stereo | new unit · (DTL cross-check) |

### Tempo & sequence

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `tempoBackswing` | **live** | Phases | Address→Top duration (`tempo_metrics.cpp`; refuses an unconfident ladder) | none | `tempo_metrics_test` · corpus distribution owed |
| `tempoRatio` | **live** | Phases | backswing ÷ downswing time, + propagated 1σ | none | `tempo_metrics_test` · **`truth.event_top_s` still unmeasured — Top error is doubly leveraged here**; corridor provisional pending the Address→Takeaway gap distribution |
| `kinematicSequence` | planned | Plv+Thx+F + Club | per-segment peak-ω order/timing stage (Sequence shape) | anat+mount | new unit · (mocap sequence) |

### Alignment

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `elbowAlignment` | live | FaceCam | `buildUpperBodySeries` elbow-line angle ✓ | camCal | `upper_body_metrics_test` · (corpus) |
| `feetAlignment` | live | FaceCam | `buildLowerBodySeries` ankle-line angle ✓ | camCal | `lower_body_metrics_test` · (corpus) |

*`shoulderAlignment` and `hipAlignment` were listed here for a long time and have never existed in
the manifest. The readings they described are covered by `shoulderPlaneAngle` (Body rotation) and
`hipLineTilt` (Pelvis & lateral); a true target-line alignment needs DTL and is not declared.*

### Head

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `headSway` | live | FaceCam + Ball | `buildHeadSeries` lateral disp, in **cm** ✓ | **px→mm**; absent without the ruler | `head_track_test` · (corpus) |
| `headLift` | live | FaceCam + Ball | vertical disp, in **cm** ✓ | **px→mm**; absent without the ruler | `head_track_test` · (corpus) |
| `headTilt` | live | FaceCam | eye-line angle ✓ | none — an angle needs no scale | `head_track_test` · (corpus) |

### Arms

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `leadArmToTorso` | live | FaceCam | `buildUpperBodySeries` lead arm vs torso angle ✓ | camCal | `upper_body_metrics_test` · (corpus) |
| `trailElbowHeight` | live | FaceCam | trail elbow height ÷ shoulder width ✓ | camCal | `upper_body_metrics_test` · (corpus) |
| `leadHandWidth` | live | FaceCam | hand distance from the body ÷ arm length ✓ | camCal | `upper_body_metrics_test` · (corpus) |
| `leadUpperArmToChest` | live | FaceCam | lead-arm connection gap ÷ shoulder width ✓ | camCal | `upper_body_metrics_test` · (corpus) |

### Ball flight

Optical ball flight is not resolvable at our frame rates — see the `launchMonitor` requirement in
`metric_descriptor.h` for why that is stated as a requirement rather than tracked as a gap. The three
**planned** rows are the ones we could in principle see off a high-rate ball track and have not built.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `faceToPath` | device | **LM** | face angle − club path, from the monitor | — | (launch monitor) |
| `spinAxis` | device | **LM** | from the monitor | — | (launch monitor) |
| `spinRate` | device | **LM** | from the monitor | — | (launch monitor) |
| `carryDistance` | device | **LM** | from the monitor | — | (launch monitor) |
| `launchDirection` | planned | Ball (high rate) | initial ball vector, horizontal | camCal / stereo | new unit · (launch monitor) |
| `launchAngle` | planned | Ball (high rate) | initial ball vector, vertical | camCal / stereo | new unit · (launch monitor) |
| `ballSpeed` | planned | Ball (high rate) | post-impact ball speed | px→mm | new unit · (launch monitor) |

### Strike

Both read from the monitor; there is no detection work here, only the connector.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `smashFactor` | device | **LM** | ball speed ÷ clubhead speed, from the monitor | — | (launch monitor) |
| `strikeLocation` | device | **LM** | face-impact position, from the monitor | — | (launch monitor) |
