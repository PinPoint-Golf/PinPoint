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
  metric_descriptor.h             MetricDescriptor, MetricRoute, MetricRequirement, CaptureDevice
                                  (NO normative values — Step D)
  metric_provider.h               IMetricProvider seam, ShotContext, MetricAvailability,
                                  describeRequirement() + resolveRoutes() — the route walk
  metric_reducer.h                the reducer vocabulary a measure names alongside a metricKey
  metric_catalogue.{h,cpp}        MetricCatalogue value object + makeMetricCatalogue() factory
  metric_resolver.{h,cpp}         provider fusion (resolveAvailability) — nothing else
  metric_catalogue_manifest.cpp   installMetricManifest() — the ONE list of every descriptor
  metric_providers.{h,cpp}        CLAIM LISTS ONLY: WristMetricProvider / KinematicSeriesProvider
                                  / FootMetricProvider / LowerBodyMetricProvider
                                  / UpperBodyMetricProvider / TrailWristProvider
                                  / BodyRotationProvider / ClubDeliveryProvider / TempoProvider
                                  / HeadMetricProvider / ShaftLeanProvider / ScoreProvider
                                  / LaunchMonitorProvider
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
  which descriptor keys it can satisfy.
- **A metric declares a ROUTE LADDER, and it is the only statement of what the metric needs.**
  `MetricDescriptor::routes` is `std::vector<MetricRoute>`, **ordered best first**, and its **last
  rung is the cheapest** — both ends are read, so both orderings are load-bearing. Each rung carries
  its own `requirement`, a `RouteMethod` (Projected / Triangulated / Inertial / Fused / Device /
  Derived), a `RouteQuality` (`Direct` → Measured, `Estimated` → Bridged), a `summary` naming the
  method, and its own `planned` flag.

  There is **no `.requirement` field and no `.planned` field**. A flat requirement could state only
  the floor, so everything above it lived in provider C++ (`BodyRotationProvider`'s IMU-vs-camera
  if/else), in a metric's own `description`, and in Appendix A's Capture column — three copies, no
  gate, and they drifted. Everything now derives from the one ladder:

  | Reading | Method | Used by |
  |---|---|---|
  | is it built at all | `planned()` — every rung planned | the Planned badge, roadmap counts |
  | what it needs | `baselineRequirement()` — the last live rung | "Needs", the row's source glyphs |
  | what better kit adds | `upgradeDevices()` — devices above the floor, **planned rungs included** | the "Improves with" filter |
  | this shot's answer | `resolveRoutes()` — first satisfied live rung | Measured/Bridged, the reason, `upgrade` |

  The two device readings differ deliberately: `upgradeDevices()` answers a *catalogue* question
  ("where could this metric go") and counts routes nobody has built, while
  `MetricAvailability::upgrade` is shown against a *real swing* and may only ever name kit that
  would actually work today. Telling a golfer to buy a DTL camera for a pipeline we have not
  written is a lie with a price on it.
- **The upgrade hint names the BEST rung, not the nearest.** `resolveRoutes()` walks best-first and
  stops at the first unsatisfied live rung, so a metric with two rungs above the one that fired
  points at the top one. `pelvisRotation` on a camera-only shot has both a stereo rung and a pelvis
  IMU above it, and the IMU is what the hint must say: cheaper, measures the turn outright rather
  than triangulating two points, and works on the camera the owner already has. A nearest-rung walk
  would recommend a second camera to every owner of the weakest setup — the purchase
  `shot_analyzer_design.md` argues hardest against ("the UI prompts the *right* upgrade").
- **`stereoGain()` grades the second camera in four steps, and the weakest one is DERIVED.**
  `Unlocks` (the floor needs DTL) · `Improves` (an authored DTL rung above the floor) · `Refines`
  (**derived**) · `None`. The first three are read off routes. `Refines` is computed: a `Projected`
  floor rung whose metric is read at any phase past Address.

  That is projective geometry, not a fact about a metric. A face-on camera measures
  `atan2(Δz, Δx_projected)` where the truth is `atan2(Δz, √(Δx² + Δy²))`, and those agree only while
  the segment lies in the frontal plane; a swing rotates the body out of it. Translations have the
  same problem wearing a different hat — turning the pelvis moves the *apparent* hip centre sideways
  with no sway at all. Two families escape, and both fall out of the rule: readings taken while the
  golfer is still square (the six Address-only setup and foot metrics), and anything not `Projected`
  at all. **27 metrics** are `Refines` today, and authoring that 27 times would be 27 copies of one
  sentence that a 28th metric would then silently miss.

  It surfaces as its **own column and facet** ("2nd camera"), not as another entry under "Improves
  with" — that one is a device list where this is a graded verdict, and the weakest grade is not a
  route anybody authored. It shipped for one build as a facet with no column, which is the worst of
  both: the rail offered "Refine it (27)" while the table showed "—" beside every one of them, so
  scanning the list and filtering it gave opposite answers. **A facet must count what a column
  shows**; `model_browser_test` now asserts that for every metrics facet, by name and row-for-row.

  **Known limit:** the error is *phase*-dependent and a route is per-metric. `shoulderPlaneAngle`
  declares Address, Top and Impact — exact at the first, badly foreshortened at the second, one rung
  for both. Modelling it properly means per-phase routes, which is a lot of machinery for a caveat
  that belongs in prose. `Refines` is graded weakest partly for this reason.

  **`attackAngle` is `Refines`, and that does not contradict the design's correction** that DTL is
  the one view which cannot measure it. Both hold: a DTL camera *alone* puts the target-line
  direction on its own optical axis, while a *calibrated pair* recovers the 3D velocity vector and
  removes the depth-component bias the projected reading carries. Replacing the view and
  triangulating from both views are different operations. Do not "fix" this.
- **A club sensor is not club tracking.** `CaptureDevice::ClubSensor` (a shaft IMU) and
  `CaptureDevice::ClubTrack` (the club found in the image) are separate tags because
  `ClubInstrumented` is an upgrade axis *orthogonal* to the second camera — "a one-camera owner
  reaches club metrics by adding a sensor, not a camera". `clubheadSpeed` has both above its
  projected rung, sensor first, which is what makes its hint recommend the right hardware.
- **`planned` is a fact about a ROUTE.** `clubPath` is not work we owe — it needs a camera pointing
  down the target line, and the DTL producer on top of that. Both are now stated at once; the single
  flag could say only one, and **nine metrics were mis-filed as roadmap items** when what they
  actually needed was hardware. Hardware is a requirement, never a `planned` flag (see the launch
  monitor note in Step C) — but a route needing hardware we cannot *read* yet is honestly both.
- **Depth is a DEVICE (`MetricRequirement::dtlCamera`), not `minTier = Stereo3D`.** Three metrics
  used the tier as a proxy, which rendered as "needs a higher reconstruction tier" — true,
  unactionable, and invisible to any filter. `ShotContext::hasDtl` is false on every shot this build
  can record, exactly as `hasLaunchMonitor` is: the day a capture path lands, those metrics resolve
  with no catalogue change.
- **No startup singleton / self-registration.** The catalogue is assembled on demand by
  `makeMetricCatalogue()` (mirrors `makeReferenceBandProvider(Kind)`), which installs the manifest
  and a fixed provider set. This matches the Analysis module's ban on stage/provider registration
  (`analysis_stage.h` anti-goals).
- **The full design catalogue — live + planned.** The manifest declares every metric in
  `shot_analyzer_design.md §A`, each either **live** (some rung has a producer) or **planned**
  (every rung planned). **70 descriptors; 45 live, 25 planned.**
  Live today: metric_extractor ×4, kinematic_series ×3 + shaft-lean, foot_metrics ×5 +
  ball_position ×1, head_track ×3, lower_body_metrics ×6, upper_body_metrics ×9,
  body_rotation ×4, club_delivery ×3, the trail-wrist series ×1, tempo_metrics ×2, and the two
  wrist `Summary` scores (sourced from a `ScoreBreakdown`, not a `MetricSeries`).
  Planned: the depth-axis metrics, the sagittal spine pair, the keypoints no pose layout carries,
  the sensors we do not place, `kinematicSequence`, `swingScore`, and the nine launch-monitor
  readings — which **also** require the device, and are the worked case for a rung being both.

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
- **Planned metrics resolve "planned", not "missing sensors" — from the ladder, with no placeholder
  provider.** A descriptor whose every rung is planned answers `Unavailable` with
  `"planned — <that rung's summary>"`, whatever the shot can do; its `baselineRequirement()` is
  documentation of *what it will need*, surfaced as "will need …" on the detail page and a
  **Planned** badge in the directory.

  **`PlannedMetricProvider` is gone and must not come back.** It claimed every `.planned` key and
  repeated one fixed sentence, which made it a second list of what is unbuilt that had to be kept in
  step with the first — and it drifted: ten planned descriptors were missing from it and fell
  through to `"no producer available"`, the reason an **unknown** key gets. Those two statements are
  not interchangeable. `resolveAvailability()` now falls back to the descriptor's own ladder when
  nothing claims a key, so the planned answer comes from the same place the requirement does.

  **Promoting a route to live:** add the producer, drop `PLANNED` from that rung, add the key to a
  real provider's `provides()` if nothing claims it yet, flip the matching `core.json` measures from
  `planned` to `live`, and update the metric_catalogue_test counts. Then **re-run `core_pack_test`**:
  a condition that could not fire before may now fire with nothing behind it, which is a defect in
  what ships rather than a backlog item, and that test is the thing that catches it.
- **Each planned rung says WHY in its own `summary`, and that is where the grouping now lives.**
  Depth, sagittal, no-keypoint-in-any-layout, sensors-we-do-not-place and work-we-have-not-done are
  five different statements; they used to be comment headings over a flat key list in
  `PlannedMetricProvider`, a file away from the requirements they explained. The reason now sits on
  the route beside its own requirement. The manifest header still groups them for a reader deciding
  what to build next — keep that list and the routes in step.

Two easy traps:
- The route member is `requirement`, **not** `requires` — `requires` is a C++20 keyword and cannot be
  an identifier. (The QML façade calls it `requires`, which is fine: JavaScript has no such keyword.)
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
`{"chart:review","score:wrist"}`). Keep `minTier` at `Angles2D` on every rung; a rung that needs the
second camera says `.dtlCamera = true`, which is the device, where the tier is what a calibrated pair
of them yields.

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
    .routes = {
        via("wristImus", RM::Inertial, Direct,
            { .imuRoles = { SegmentRole::LeadForearm, SegmentRole::LeadHand } },
            QStringLiteral("the Cardan axis between the two IMU orientations")) },
    .usedBy = { QStringLiteral("chart:review") },
});
```

`via(...)` is a local helper at the top of `installMetricManifest()`, with `RM` /
`Direct` / `Estimated` / `PLANNED` aliases beside it — six designated initialisers per rung buried
the thing the ladder exists to show.

**Declare a second rung when a metric genuinely has one**, and only then. Reference the docs, not
your intuition: an upgrade route is a claim that some kit produces a better number, and one invented
here becomes a filter chip that sends a reader shopping.

The source is the **"2nd camera (DTL) adds"** column in `shot_analyzer_design.md` §"Single-camera
(face-on) viability", read together with the corrections block above it. Two traps in reading it:

- **Its verdicts assume an IMU.** The column header is `single-cam(face-on) + IMU`, so "nothing" for
  `xFactor` means *given a bound pelvis+thorax pair, a second camera adds nothing* — which is true.
  It is **not** a statement about the camera-only shot, which is what most swings are and what
  `body_rotation.cpp` actually serves with `acos(w/w₀)` and `σ ∝ 1/sin θ` (unbounded near square).
  Stereo genuinely beats that. Both facts are now rungs, in order.
- **Three of its rows are wrong on the merits** and the corrections block says so — most sharply
  `attackAngle`, where "DTL makes it fully in-plane" is backwards: the angle lives in the vertical
  plane containing the target line, which IS the face-on image plane. Do not add a DTL rung there.

Ordered best first, cheapest last:

```cpp
    .routes = {
        via("pelvisImu", RM::Inertial, Direct, { .imuRoles = { SegmentRole::Pelvis } },
            QStringLiteral("measured directly from the pelvis IMU")),
        via("faceOn", RM::Projected, Estimated, { .faceOnCamera = true },
            QStringLiteral("estimated from the collapse of the hip span in the face-on image")) },
```

A rung's `summary` must name the **method**, not the missing device. It is shown verbatim as the
availability reason for an `Estimated` rung, against a number that IS on screen — "needs a pelvis
IMU" there reads as a refusal. The missing-kit sentence is generated separately, from the
requirement.

### Step C — claim the key
**One line.** Add the key to a provider's `provides()` in `src/Metrics/metric_providers.{h,cpp}`, or
add a new provider class if none fits. That is the whole step: every provider we ship is a claim list
and nothing more, because the seam's default `availability()` walks the metric's ladder
(`resolveRoutes()` in `metric_provider.h`).

Each class used to rebuild its metrics' requirements in C++ as well — a second copy of what the
descriptor already said, and it drifted in both directions at once: `stanceWidthMm` was produced on
every ruler-resolved swing and claimed by nobody, while `leadHeelLift` was claimed but understated
what it needed, so a ball-less shot was told it was `Measured` while the producer had declined to
emit it.

> **Overriding `availability()` is a design decision, not a routine step.** It means the ladder
> cannot express something, and the first question is whether a rung is missing from the manifest.
> Nothing we ship overrides it today; two classes used to, and both were saying things the directory
> could not see (body rotation's IMU-vs-camera split, the scores' missing scorer).

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

**Getting `Bridged`.** Three states, and the middle one is not decoration. It is what an
`Estimated` rung yields: the metric IS produced, by a weaker method than the best rung. Body rotation
is the worked example — a bound `SegmentRole::Pelvis` stream measures axial turn directly, and with
only a face-on camera the same producer estimates it from the collapse of the hip span in the image.
You get this by **declaring the two rungs in Step B**, not by writing a condition here. The producer
still has to carry the cost: `body_rotation.cpp` propagates its span noise into `MetricSeries::sigma`
so a reader can see how much to trust a small turn.
- If you add a **new** provider class, register a process-lifetime instance of it in
  `makeMetricCatalogue()` (`metric_catalogue.cpp`) with `cat.addProvider(&yourProvider)`.

**Hardware the user may not own is a REQUIREMENT. Whether we can READ it is a separate field, and a
rung is often both.** `MetricRequirement` carries `launchMonitor` and `dtlCamera` alongside
`faceOnCamera` / `clubTrack` / `ballTrack`, matched by `ShotContext::hasLaunchMonitor` / `hasDtl`.
Declaring the requirement is what keeps the metric under the right chip and makes the absence
graceful through the path every other missing input uses.

But a requirement is not a promise that the device would work. **Nothing in this build sets
`hasLaunchMonitor`** — no code outside a test even mentions it — so all nine launch-monitor rungs are
`PLANNED` as well as requiring the device. "Needs a launch monitor" on its own would send a golfer to
buy hardware that changes nothing.

> This is not a reversal of the old rule, it is what the route made possible. The rule was written
> when `planned` was a **metric-level** flag with nowhere to put the hardware, so marking these
> planned really did erase the requirement and promise work instead. On a route the two are separate
> fields and both are visible: the **Needs** facet still says *Launch monitor*, the badge says
> **Planned**, and the reason says why. `clubPath` is the same shape — a camera we have no capture
> path for AND a producer we have not written.

`LaunchMonitorProvider` remains that connector's insertion point rather than a stub to delete, and
the diagnostics pack's matching statement is `MeasureStatus::ExternalDevice`. When a connector lands,
**dropping `PLANNED` from those nine rungs is the whole change** — and `metric_catalogue_test` §3e
flips back with it, because it asserts the connector's absence rather than assuming it.

**Resolution, in two layers.** `resolveRoutes()` (`metric_provider.h`) walks ONE metric's ladder:
first live rung the shot satisfies wins, `Direct` → `Measured` and `Estimated` → `Bridged`; the
nearest better live rung becomes `MetricAvailability::upgrade`; no live rung satisfied gives
`"needs X, or Y"` over every live rung; no live rung at all gives `"planned — …"`.
`resolveAvailability()` (`metric_resolver.cpp`) then fuses PROVIDERS: over all that list the key, the
best state wins (`Measured` > `Bridged` > `Unavailable`), ties broken by `priority()`. If none claims
the key it falls back to the ladder — which is the right answer for a planned metric and a defect for
anything else, gated by `metric_catalogue_test` §3d-bis.

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
- **a claimant**: nothing to add — section 3d-bis already sweeps every descriptor **with a live
  route** for one, so forgetting to list your key in a provider's `provides()` fails the suite by
  name. Do not weaken that sweep to make a new metric pass; an unclaimed key is `Unavailable` on
  every shot forever, which is what it was written to catch. Metrics whose every rung is planned are
  exempt: nothing produces them, so there is no producer to claim them.
- **the ladder**: nothing to add — 3d-ter checks every descriptor has at least one route and that
  none puts a `Direct` rung below an `Estimated` one, which would hand back `Bridged` on a shot that
  could have been `Measured`.
- **resolve()**: a `ShotContext` where it is `Measured`, and one where it is `Unavailable` with the
  right reason (**missing sensor or camera — never a session type**; see the ⛔ box in Step C). If
  you declared a second rung, add the `Bridged` case and the `upgrade` sentence too.
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
normative bar (for any metric with a norm), the **How it's measured** ladder, and usedBy. Then open
**Settings → Diagnostics → Metrics** and check it lands under the right **Needs**, **Improves with**
and **A 2nd camera would** chips — the last is derived, so a projected metric you read past Address
should appear under *Refine it* without you having authored anything. Run the full 7-suite gate before any release.

## 3. QML façade shapes (for UI work)

`MetricCatalog` (`src/Gui/review/metric_catalog.h`, `QML_ELEMENT`) marshals registry types to
QVariant. Row shape from `query(filters, shotCtx={})`:
`{ key, label, shortLabel, unit, type, group, scored, planned, sources:[…], needsDevices:[…],
improvesDevices:[…], availability:{state,reason,tier,routeId,upgrade} }`.
Detail from `descriptor(key, shotCtx={})` adds `description, howToRead, flexPositive, phases:[int…],
normative:{…}, routes:[…], requires:{…}, usedBy:[…]`. **Phases are Phase ints** — render with
`TimelineLabels`.

`routes` is the ladder, best first — one entry per rung:
`{ id, method, estimated, summary, planned, requires:{faceOnCamera,dtlCamera,imuRoles:[…],clubTrack,
ballTrack,launchMonitor,minTier}, devices:[deviceId…] }`. `requires` at the top level is the FLOOR
(the cheapest rung), kept under that name because every surface asking "what does this need" wants
exactly that. `needsDevices` / `improvesDevices` are stable slugs (`faceOn`, `dtl`, `wristImus`,
`bodyImus`, `clubTrack`, `clubSensor`, `ballTrack`, `launchMonitor`), not display text.
`stereoGain` is one of `unlocks` / `improves` / `refines` / `none` — what a second camera would do,
with `refines` derived rather than authored (see the invariant above).

**`CaptureDevice`'s declaration order is display order**, everywhere a device list is rendered:
IMUs (wrist · body · club sensor), then cameras and what they find in them (face-on · DTL · club
track · ball track), then the launch monitor. `allCaptureDevices()` is that sequence, and
`captureDevicesFor()` builds by walking it rather than by walking the requirement's fields — so
`imuRoles`, which is an unordered set the manifest authors in whatever sequence reads best, cannot
make `{Pelvis, LeadHand}` and `{LeadHand, Pelvis}` produce two different lists for the same kit.

`availability.upgrade` is a ready-made sentence ("Pelvis IMU would measure it directly") and is empty
when the best rung already fired. **Bind it; never synthesise one from `routes`** — that list
includes rungs no producer implements, and a shot-level hint may only name kit that would work today.

The `normative` map is resolved out of the NORM SET, not the descriptor:
`{ corridors:[{phase,greenLo,greenHi,amberLo,amberHi,deltaFromAddress,measureId,contextId,inherited,overridden}],
measureId, contextId, contextLabel, inherited, overridden, source, sourceLabel, n, citation, weak,
weakReason }`. A metric with no measure or no norm gets an empty `corridors` list — draw the
"no norm yet" state, never a zeroed band. The Watch edge (`amberLo/amberHi`) is policy-dependent for
any norm that states no monitor band, which is why the host must bind
`gradePolicy: appSettings.diagnosticsGradePolicy` on the `MetricCatalog` it declares. `shotCtx` (all optional): `{ tier, sessionType, imuRoles:[roleName…],
hasFaceOn, hasDtl, hasClubTrack, hasBallTrack, hasLaunchMonitor, archetype, club, shape }`; empty
`{}` = the context-free directory view.

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

## Appendix A0 — which producer to build next

Appendix A is exhaustive and unordered: it says what is outstanding for every metric, and nothing
about which is worth doing first. This answers that, and it is answerable **only because the
diagnostics library is authored ahead of the producers**. Every unbuilt metric already has
characteristics written against it, and every characteristic carries a `prominence` — how often a
coach expects to see it. Join the two and the roadmap sorts itself: *build the producer that would
light up the most common faults.*

That is the model earning its keep as a reference rather than as a mirror of the pipeline. Nothing in
this table is a defect, and none of it belongs in a health list — see the note opening §8.4 of the
diagnostics guide.

**Read the prominence column as this library's editorial judgement, not measured prevalence** — no
study counts named swing faults across a population. It is a prior, and it is re-seatable.

| Metric | Status | Faults it would unlock | Top rung | Which |
|---|---|---:|---|---|
| `swingPlane` | planned | **6** | ubiquitous | *Over the top*, *Steep through delivery*, *Stuck under the plane*, *Shallowing in transition*, *Steep backswing plane*, *Flat backswing plane* |
| `spineForwardBend` | planned | **4** | ubiquitous | *Loss of posture*, *Standing too upright*, *Bent over too much at address*, *Diving into the ball* |
| `clubPath` | planned | **2** | ubiquitous | *Path too far out-to-in*, *Path too far in-to-out* |
| `faceToPath` | device | **2** | ubiquitous | *Face open to the path*, *Face closed to the path* |
| `pelvisThrust` | planned | **2** | ubiquitous | *Early extension*, *Backing away from the ball* |
| `spinAxis` | device | **2** | ubiquitous | *Slice*, *Hook* |
| `shaftDirection` | planned | **4** | common | *Club taken inside*, *Club taken outside*, *Across the line at the top*, *Laid off at the top* |
| `launchDirection` | planned | **2** | common | *Pull*, *Push* |
| `lumbarExtension` | noProducer | **2** | common | *S-posture*, *Flat lower back at address* |
| `spinRate` | device | **2** | common | *Too much spin*, *Too little spin* |
| `strikeLocation` | device | **2** | common | *Heel strike*, *Toe strike* |
| `thoracicFlexion` | noProducer | **2** | common | *C-posture*, *Flat upper back at address* |
| `ballSpeed` | planned | **1** | common | *Lost ball speed* |
| `carryDistance` | device | **1** | common | *Short carry* |
| `smashFactor` | device | **1** | common | *Poor strike efficiency* |
| `trailKneeFlexion` | planned | **1** | common | *Trail knee straightens* |
| `leadKneeFlexion` | planned | **3** | occasional | *Sitting too deep*, *Legs too straight*, *Late lead-knee buckle* |
| `ballBodyDistance` | planned | **2** | occasional | *Ball too close to the body*, *Ball too far from the body* |
| `launchAngle` | planned | **2** | occasional | *Low ball flight*, *Ballooning* |

**`swingPlane` tops the list by a distance**, and it is worth seeing why the count is six rather than
the two you might expect. One metric read at three different phases carries the whole plane story —
the backswing pair off `m_shaftPlaneBackswing`, the delivery pair off `m_shaftPlaneDelivery`, and
over-the-top with its shallowing counterpart off the P4→P5 delta. That is `metric_reducer.h` doing
its job: *where* a reading is taken is a property of the measure, not of the metric, so one producer
serves six characteristics including the single commonest fault in amateur golf.

**The `device` rows are a different kind of work and are ranked here anyway.** They need a connector,
not a producer written from our own pixels — but a reader deciding what to build next should see all
of it in one order, and `faceToPath` sitting third says something real about what a launch-monitor
integration would be worth.

**This table is a JOIN, not a hand-kept list — regenerate it, do not edit it.** The lesson of the
Capture column below applies with more force here, because this one spans two registries:

```sh
python3 -c "
import json, collections
d = json.load(open('src/Resources/diagnostics/core.json'))
M = {m['id']: m for m in d['measures']}; S = {s['id']: s for s in d['signals']}
C = {c['id']: c for c in d['conditions']}
RANK = {'rare':0,'uncommon':1,'occasional':2,'common':3,'ubiquitous':4}
by = collections.defaultdict(set); status = {}
for m in M.values():
    k = m.get('metricKey')
    if k: status[k] = 'live' if (status.get(k)=='live' or m['status']=='live') else m['status']
for c in d['conditions']:
    for sid in c.get('detectedBy', []):
        for mid in S.get(sid, {}).get('measures', []):
            k = M.get(mid, {}).get('metricKey')
            if k: by[k].add(c['id'])
rows = [(max(RANK[C[x]['prominence']] for x in v), len(v), k, status[k],
         sorted(v, key=lambda x: -RANK[C[x]['prominence']]))
        for k, v in by.items() if status.get(k) != 'live']
for top, n, k, st, ids in sorted(rows, key=lambda r: (-r[0], -r[1], r[2])):
    print(k, st, n, [C[i]['label'] for i in ids])"
```

Two reading notes. Rows tie-break on the metric key, so the order above is exactly what the snippet
prints. And the Status column uses **this guide's** vocabulary — the pack spells `device` as
`externalDevice`, which is what the snippet emits.

Metrics with a live producer are excluded — for those the question is already answered. A metric
serving no characteristic at all (`wristScore`, the chart-only rollups) never appears, which is
correct: it is not waiting on anything the fault library can rank.

---

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

**Status** is read off the route ladder — `planned()` for the badge the directory shows, and
`baselineRequirement().launchMonitor` for the third row, which is a requirement rather than a roadmap
item:

| Status | Means |
|---|---|
| **live** | Some rung has a producer. Resolves Measured (or Bridged) on a shot that satisfies it. |
| **device** | The *reading* comes from hardware we integrate rather than a producer we author. Still requires the device, and **also planned** until a connector exists — nothing sets `hasLaunchMonitor` today. There is no detection work below, only the connector. |
| **planned** | Every rung `PLANNED`. Answers `Unavailable — "planned — <that rung's summary>"`, whatever the shot can do. |

**The Capture column is now derivable, so check it rather than remember it.** It duplicates
`.routes[].requirement`, and duplication is what put 43 rows against a 70-metric catalogue the last
time this table was audited. A row's Capture cell should read as its ladder does; where a metric has
two rungs the cell says so in parentheses (`FaceCam (Plv IMU upgrades it)`), which is exactly what
`baselineRequirement()` + `upgradeDevices()` return. The **Needs** and **Improves with** chips in
Settings → Diagnostics → Metrics are the same two readings, generated — read them off the running app
and correct this table, not the other way round.

Groups below are in manifest order, which is the order the Metric Library and the chart's metric
presets list them in.

### Score

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `wristScore` | live | F+H (+U) | WristAssessmentEngine rollup ✓ | anat+mount | `composite_score_v2_test` · (corpus: score stability) |
| `wristResemblance` | live | F+H | WristResemblanceScorer ✓ | anat+mount | `wrist_resemblance_test` · (corpus: per-archetype) |
| `swingScore` | planned | FaceCam | wire a live adherence scorer (SwingScorer is dark) | anat+mount, camCal | `swing_scorer_test` (exists) · (corpus: adherence vs coach) |

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
| `pelvisRotation` | live | FaceCam (Plv IMU upgrades it; DTL triangulates it) | `buildBodyRotationSeries` axial turn ✓ | camCal / anat+mount | `body_rotation_test` · (mocap ground truth owed) |
| `thoraxRotation` | live | FaceCam (Thx IMU upgrades it; DTL a weaker cross-check) | axial-turn channel ✓ | camCal / anat+mount | `body_rotation_test` · (mocap owed) |
| `xFactor` | live | FaceCam (Plv+Thx upgrade it; DTL triangulates both bearings) | thorax−pelvis separation ✓ | camCal / anat+mount | `body_rotation_test` · (mocap owed) |
| `xFactorStretch` | live | as `xFactor` (incl. the DTL rung), + a segmented Top | separation minus its value at Top ✓ | camCal / anat+mount | `body_rotation_test` · (corpus: speed correlation owed) |
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
| `thoracicFlexion` | planned | **DTL** | upper-back CONTOUR at Address — no keypoint exists between shoulders and hips in either layout | stereo | new unit · (mocap) |
| `lumbarExtension` | planned | **DTL** | low-back CONTOUR at Address — no keypoint exists between shoulders and hips in either layout | stereo | new unit · (mocap) |

### Pelvis & lateral

Centre TRANSLATIONS. `hipLineTilt` is named for an angle but belongs here: it is a pelvis reading in
the same frontal plane as sway and lift, and it is read alongside them.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `pelvisSway` | live | FaceCam + ground | `buildLowerBodySeries` lateral translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `pelvisLift` | live | FaceCam + ground | vertical translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `hipLineTilt` | live | FaceCam | hip-line angle vs horizontal ✓ | camCal | `lower_body_metrics_test` · (corpus) |
| `thoraxLateralDrift` | live | FaceCam + ground | `buildUpperBodySeries` thorax lateral translation ✓ | camCal, ground | `upper_body_metrics_test` · (corpus) |
| `pelvisThrust` | planned | FaceCam + **DTL** (optical axis) | toward-ball translation | stereo | new unit · (mocap; needs depth) |

### Feet & stance

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `stanceWidth` | live | FaceCam | `buildFootSeries` heel-to-heel ÷ shoulder width ✓ | none — a body-relative ratio. **ABSENT when the shoulders do not resolve**, never re-expressed in another unit | `foot_metrics_test` · distribution owed (no corridor yet) |
| `stanceWidthMm` | live | FaceCam + Ball | same span through the ball-diameter ruler ✓ | **px→mm** | `foot_metrics_test` · (corpus) |
| `ballPosition` | live | FaceCam + Ball | ball projected on the heel line ÷ stance width (`ball_position.cpp`) | none — a same-plane ratio, scale-free | `ball_position_test` · per-club distribution owed |
| `leadFootFlare` | live | FaceCam | foot heel→bigtoe angle ✓ | none | `foot_metrics_test` · (corpus) |
| `trailFootFlare` | live | FaceCam | foot heel→bigtoe angle ✓ | none | `foot_metrics_test` · (corpus) |
| `toeLineAngle` | live | FaceCam | bigtoe→bigtoe line angle ✓ | none | `foot_metrics_test` · (corpus) |
| `leadHeelLift` | live | FaceCam + **Ball** | heel-vs-toe elevation curve, in **cm** ✓ | **px→mm**; absent without the ruler | `foot_metrics_test` · (corpus) |
| `leadKneeDrift` | live | FaceCam + ground | `buildLowerBodySeries` lead-knee lateral travel ✓ | camCal, ground | `lower_body_metrics_test` · (corpus) |
| `comOverLeadFoot` | live | FaceCam + ground | balance point vs lead foot, sampled to Finish ✓ | camCal, ground | `lower_body_metrics_test` · (corpus) |
| `leadKneeFlexion` | planned | FaceCam (DTL better) | knee angle — sagittal, face-on cannot see it | camCal / stereo | new unit · (mocap) |
| `trailKneeFlexion` | planned | FaceCam (DTL better) | knee angle — sagittal | camCal / stereo | new unit · (mocap) |
| `ballBodyDistance` | planned | **DTL** + Ball | ball standoff from the body — depth axis | stereo | new unit · (corpus) |

> **Fixed 2026-08-02 — worth knowing about, because the failure was silent.** `stanceWidthMm` was
> declared and produced but claimed by **no provider**, so `MetricCatalogue::resolve()` fell back to
> rendering the descriptor's own requirement and answered `Unavailable` on every shot ever taken. It
> reads as a plausible "needs a face-on camera" on a shot that has one, so nothing about it looks
> wrong from the directory. `leadHeelLift` had the mirror-image bug: claimed, but understating its
> requirement, so a ball-less shot was told it was `Measured` while `foot_metrics.cpp` had declined
> to emit it (the reading is centimetres and the ball diameter is the only ruler at the ground plane
> — its own `howToRead` had said so all along).
>
> **An availability answer has to match what the producer actually does**, in both directions.
> `metric_catalogue_test` now sweeps every descriptor for a claimant, which is the check that would
> have caught the first one on the day it landed; a per-metric case would not have.

### Club & speed

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `clubheadSpeed` | live | FaceCam + Club (shaft sensor best, then DTL — the projected speed loses the depth component) | `buildKinematicSeries` head-path speed ✓ | px→mm / ground | `kinematic_series_test` · (launch monitor) |
| `handSpeed` | live | FaceCam + Club (grip); DTL restores the depth component | grip-path speed ✓ | px→mm | `kinematic_series_test` · (launch monitor) |
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
| `swingPlane` | planned | **DTL** + Club | SVD best-fit plane of head path — needs the path in 3D | stereo | new unit · (DTL cross-check) |
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
| `headSway` | live | FaceCam | `buildHeadSeries` lateral disp, in **cm** ✓ | **inter-ear px→mm** (NOT the ball — `wrist_analyzer.cpp` builds it from `head.addrScalePx / earWidthMm`); absent without it | `head_track_test` · (corpus) |
| `headLift` | live | FaceCam | vertical disp, in **cm** ✓ | **inter-ear px→mm**, as `headSway`; absent without it | `head_track_test` · (corpus) |
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
| `launchDirection` | planned | **DTL** + Ball (high rate) | initial ball vector, horizontal | camCal / stereo | new unit · (launch monitor) |
| `launchAngle` | planned | FaceCam + Ball (high rate) | initial ball vector, vertical | camCal / stereo | new unit · (launch monitor) |
| `ballSpeed` | planned | FaceCam + Ball (high rate) | post-impact ball speed | px→mm | new unit · (launch monitor) |

### Strike

Both read from the monitor; there is no detection work here, only the connector.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `smashFactor` | device | **LM** | ball speed ÷ clubhead speed, from the monitor | — | (launch monitor) |
| `strikeLocation` | device | **LM** | face-impact position, from the monitor | — | (launch monitor) |
