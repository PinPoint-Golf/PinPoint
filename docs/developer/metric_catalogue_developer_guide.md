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
- **A metric has ONE `group`; a PRESET is how a reading spans groups.** The chart's preset combo is
  derived from `.group` (`ChartMetrics::seriesGroups`), so a group is also the unit a reader plots
  together — and a metric cannot be in two groups without being taken out of one. That is the right
  constraint for the directory, where a metric filed twice reads as two metrics, and the wrong one
  for a coaching read that deliberately spans groups. `MetricDescriptor::presets` is the additive
  answer: a list of named presets a metric ALSO appears in, offered after the groups and before
  "Other", and only when at least **two** of a preset's members are plottable on the swing in hand
  (one curve is a legend chip, not a preset). `"Plumb Bob"` is the first — `plumbBobDistance` read
  with `hipLineTilt` and `pelvisSway`, none of which leaves `Pelvis & lateral`. Reach for a preset
  only when the read genuinely crosses groups; a preset that duplicates a group is clutter.
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

## 1A. Sign conventions — the frames, and what ISB governs

> **The canonical statement is [`docs/design/pinpoint_sign_conventions.md`](../design/pinpoint_sign_conventions.md)**
> — rules 0/1/2, the per-metric tables, and the ISB compliance audit. Code and
> `axis_direction_test` cite it by rule number. This section is the working summary for somebody
> adding a metric; where the two ever disagree, the design doc wins and this one is the bug.

**THE RULE: A SIGN'S MEANING NEVER CHANGES WITH HANDEDNESS.** Whatever transform is needed to hold
that fixed is the producer's job, never the reader's. A left-handed golfer and a right-handed golfer
reading the same number must be told the same thing by it.

Two frames express that rule, and every metric belongs to exactly one.

### The world frame — target line, ground, horizon

Referenced to the world, not to the golfer. **Positive is to the RIGHT of the target line, or
UPWARD.** Nothing is mirrored, because the reference is the world and the world does not care which
way the golfer stands. `clubPath`, `launchDirection`, `shaftDirection`, `faceAngle`, `attackAngle`,
`offline`, every `lm.*` reading.

"Right of the target line" is read looking down the line toward the target, so it is the same
absolute direction for both golfers — which is exactly what makes it handedness-free.

**What flips is the GLOSS, never the sign.** *In-to-out*, *open*, *draw* are right-handed readings
of a world-frame number: positive `clubPath` is right of the target line for everybody, which is
in-to-out for a right-hander and **out-to-in for a left-hander**. State the frame-referenced meaning
first and the gloss second, or a left-handed reader is misled by prose that looks authoritative.

### The anatomical frame — the golfer's own lead side

Referenced to the body. **Positive is flexion, ulnar deviation and pronation.** Producers DO mirror
a left-handed golfer (`pose_wrist_angle_source.cpp`, `mirrorSign()` in `wrist_assessment_types.h`),
and that mirror is what holds the meaning fixed: a bowed lead wrist must read negative-of-cupped for
both golfers. Removing it would invert every left-handed swing's archetype.

### ISB compliance — what the standard governs, and what it does not

The anatomical frame implements **ISB / Wu et al. 2005** (`ref.wu2005`), the recommendation of the
Standardization and Terminology Committee of the International Society of Biomechanics — a society
standard, not one laboratory's preference. ISB fixes flexion, ulnar deviation and pronation as
positive, **and positive for both the left and the right arm**, so the standard itself delivers the
handedness invariance rather than us bolting it on.

**Four metrics are ISB joint angles, and all four comply:**

| Metric | Ours | ISB (Wu 2005) | |
|---|---|---|:--:|
| `leadWristFlexExt` | + flexion (bowed) | flexion + | ✓ |
| `leadWristRadUln` | + ulnar (hinge) | ulnar deviation + | ✓ |
| `forearmPronation` | + pronation | pronation + | ✓ |
| `leadArmFlexion` | + elbow flexion (magnitude) | flexion + | ✓ |

**Every other metric is outside ISB's scope, and saying so is what protects the claim.** The
credibility risk here runs the other way from the obvious one: declaring ISB conformance for a
two-dimensional apparent shoulder-line angle taken off one camera is what would fail review, because
ISB defines three-DOF rotations between segment triads built on palpable bony landmarks, and a
face-on silhouette supplies none of that. State the boundary precisely rather than implying blanket
conformance:

| Family | Why ISB does not govern it | Examples |
|---|---|---|
| **Club & ball** | ISB defines *human joint* motion. A clubhead is not a joint, and a ball is not a segment. These use the world frame. | every `lm.*`, `clubPath`, `ballSpeed`, `offline` |
| **Turn magnitudes** | Deliberately UNSIGNED magnitudes of turn away from address, not signed axial rotations about a defined axis. A face-on camera or a single IMU does not give the bony-landmark triad an ISB rotation needs. | `pelvisRotation`, `thoraxRotation`, `xFactor` |
| **Image-plane body lines** | 2D apparent angles between two keypoints as one camera sees them. Not joint rotations at all; they read the *apparent* line and a golfer stood at an angle to the camera reads differently. | `hipLineTilt`, `shoulderPlaneAngle`, `elbowAlignment`, `feetAlignment`, `toeLineAngle` |
| **Normalised displacements** | Not angles. Distances expressed as a fraction of stance or shoulder width, or in cm. | `pelvisSway`, `headSway`, `leadKneeDrift`, `ballPosition` |
| **Composites & timings** | Derived from other metrics, or durations. | `xFactorStretch`, `tempoRatio`, the scores |

None of that is a defect. It is the honest reach of a face-on camera and a wrist IMU, and each of
those metrics carries its own stated convention below. What must never happen is a descriptor
implying ISB conformance it does not have.

**A commercial sensor reports the inverse of us on bow/cup.** At least one widely used wrist device
defines extension (cupping) as positive and flexion (bowing) as negative — the exact inverse of ISB
and of us. **The published standard wins over the popular product**: a vendor can change their
convention, and a standard is the thing that lets two datasets be compared at all. The disagreement
is recorded in `ref.wu2005` and in `docs/reference/wristmetrics.md` so it stays a known difference
rather than a discovered one. Never compare a raw wrist sign across sources without checking the
frame.

### Our own lateral channels

Within the non-ISB families the conventions are consistent by family, with two deliberate exceptions
that each say so in their own `howToRead`:

- **Displacements** — positive is toward the **lead** side (`pelvisSway`, `headSway`,
  `leadKneeDrift`, `thoraxLateralDrift`).
- **Body lines** — positive means the **trail** joint sits above the lead joint (`hipLineTilt`,
  `shoulderPlaneAngle`, `elbowAlignment`, `feetAlignment`).
- **The two exceptions** — `spineSideBend` is positive toward the **trail** side, and
  `secondaryAxisTilt` is positive **away from the target**. Both are named for the thing they
  measure, and inverting either would leave every sentence written about it backwards. They are
  exceptions on purpose, not drift.

### Declaring it on a new metric

`MetricDescriptor::signPositive` / `signNegative` carry the pair. Both are lower-case fragments
completing *"positive means …"* / *"negative means …"*, so they read inside a sentence and inside a
table cell.

- **`signPositive` is required** for anything in a signable unit. Every such metric has a direction,
  even an unsigned one — its direction is what the magnitude counts.
- **`signNegative` may be empty**, and empty means *cannot go negative*: a carry, a spin rate, a
  duration. Writing prose there anyway invents a meaning the metric does not have.

`metric_catalogue_test` enforces three things, so none of this can be forgotten or quietly undone:
every direction-carrying metric declares `signPositive`; no world-frame metric is stated **only** as
a right-handed gloss (*"in-to-out"* flips for a left-hander where *"right of the target line"* does
not); and the four ISB joint angles keep ISB polarity — the assertion that stops somebody "fixing"
us to match a commercial sensor that reports the inverse.

### Every metric, and what its sign means

**GENERATED — do not hand-edit.** It restates `signPositive` / `signNegative` from
`metric_catalogue_manifest.cpp`; a second hand-maintained copy of 86 facts is exactly the drift the
manifest header warns about. Change the descriptor, then run:

```
python3 tools/metrics/gen_sign_table.py
```

*Positive means* is stated frame-first: **"right of the target line"** rather than *"in-to-out"*,
because the gloss flips for a left-handed golfer and the frame does not. *cannot go negative* marks
a magnitude — a carry, a spin rate, a duration.

<!-- BEGIN GENERATED SIGN TABLE -->

#### Score

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `wristScore` | — | live | *no direction* | *cannot go negative* |
| `wristResemblance` | — | live | *no direction* | *cannot go negative* |
| `swingScore` | — | planned | *no direction* | *cannot go negative* |

#### Wrist & forearm

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `leadWristFlexExt` | ° | live | flexion — the lead wrist bowed | extension — the lead wrist cupped |
| `leadWristRadUln` | ° | live | ulnar deviation — the wrist hinged or cocked | radial deviation |
| `forearmPronation` | ° | live | pronation — the lead forearm rolled toward face-down | supination |
| `leadArmFlexion` | ° | live | elbow flexion — a more bent lead arm; 0° is straight | *cannot go negative* |
| `trailWristFlexExt` | ° | live | extension — the trail wrist cupped, the opposite of the lead wrist's polarity because  the hands are mirror images | flexion — the trail wrist bowed |

#### Body rotation

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `pelvisRotation` | ° | live | turn away from address — a MAGNITUDE, so positive at the top and again at impact,  passing through zero as the body squares | *cannot go negative* |
| `thoraxRotation` | ° | live | turn away from address — a MAGNITUDE, so positive at the top and again at impact | *cannot go negative* |
| `xFactor` | ° | live | the chest turned further than the pelvis | the pelvis turned further than the chest |
| `xFactorStretch` | ° | live | separation still growing after the top — the stretch | separation already unwinding at the top |
| `hipInternalRotation` | ° | planned | internal rotation of that hip | external rotation |
| `shoulderPlaneAngle` | ° | live | the TRAIL shoulder sits above the lead shoulder | the lead shoulder sits above the trail shoulder |

#### Spine & tilt

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `spineForwardBend` | ° | planned | more forward bend from the hips | standing taller than upright, which a swing does not reach |
| `spineSideBend` | ° | live | side bend toward the TRAIL side | side bend toward the lead side |
| `secondaryAxisTilt` | ° | live | the upper body leaning AWAY from the target — trail-side lean | leaning toward the target |
| `thoracicFlexion` | ° | planned | a more rounded upper back | a flatter, more extended upper back |
| `lumbarExtension` | ° | planned | a more arched low back | a flattened low back |

#### Pelvis & lateral

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `pelvisSway` | % stance width | live | the pelvis moved toward the LEAD side | moved away from the lead side |
| `pelvisThrust` | cm | planned | the pelvis moved toward the ball | moved away from the ball |
| `pelvisLift` | % stance width | live | the pelvis rose | the pelvis dropped |
| `hipLineTilt` | ° | live | the TRAIL hip sits above the lead hip | the lead hip sits above the trail hip |
| `plumbBobDistance` | in | live | the hips sit AHEAD of the stance centre, toward the lead side | the hips sit behind centre, toward the trail side |
| `thoraxLateralDrift` | % stance width | live | the chest moved toward the LEAD side | moved away from the lead side |

#### Feet & stance

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `leadKneeDrift` | % stance width | live | the lead knee moved toward the LEAD side | moved toward the trail side — working inward |
| `stanceWidth` | % shoulder width | live | a wider stance | *cannot go negative* |
| `stanceWidthMm` | mm | live | a wider stance | *cannot go negative* |
| `ballPosition` | % stance width | live | the ball further BACK, toward the trail foot — 0% is the lead heel, 100% the trail  heel, the scale other golf software uses | forward of the lead heel, which is a real driver setup |
| `leadFootFlare` | ° | live | the lead toe turned out, away from the ball | turned in |
| `trailFootFlare` | ° | live | the trail toe turned out, away from the ball | turned in |
| `toeLineAngle` | ° | live | a closed stance line as the camera sees it | an open stance line — and this one line metric FLIPS for a mirrored camera |
| `leadHeelLift` | cm | live | the lead heel further off the ground | *cannot go negative* |
| `leadKneeFlexion` | ° | planned | more knee bend | *cannot go negative* |
| `ballBodyDistance` | % shoulder width | planned | standing further from the ball | standing closer to the ball |
| `trailKneeFlexion` | ° | planned | more knee bend | *cannot go negative* |
| `comOverLeadFoot` | % stance width | live | further FROM the lead ankle — UNSIGNED, because still back and fallen through are the  same fault seen from either side | *cannot go negative* |

#### Club & speed

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `clubheadSpeed` | mph | live | a faster clubhead | *cannot go negative* |
| `handSpeed` | mph | live | faster hands | *cannot go negative* |
| `lagAngle` | ° | live | more lag retained — a tighter forearm-to-shaft angle | *cannot go negative* |
| `impactShaftLean` | ° | live | the shaft leaning FORWARD, toward the target | leaning back, away from the target |
| `lm.clubheadSpeed` | mph | device | a faster clubhead | *cannot go negative* |

#### Club delivery

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `swingPlane` | ° | planned | a steeper plane | a flatter plane |
| `clubPath` | ° | planned | the head travelling RIGHT of the target line — in-to-out for a right-hander | travelling left — out-to-in for a right-hander |
| `attackAngle` | ° | live | an UPWARD strike | a descending strike |
| `lowPointAhead` | in | live (estimated, ±2 in) | the arc bottoming out AHEAD of the ball, on the target side | bottoming out behind the ball |
| `shaftDirection` | ° | planned | pointing RIGHT of the target line — across the line for a right-hander | pointing left — laid off, or dragged inside |
| `shaftAngleVsHorizontal` | ° | live | PAST parallel to the ground; zero IS parallel | short of parallel |
| `lm.attackAngle` | ° | device | an UPWARD strike | a descending strike |
| `lm.clubPath` | ° | device | the head travelling RIGHT of the target line — in-to-out for a right-hander | travelling left — out-to-in for a right-hander |
| `lm.faceAngle` | ° | device | the face pointing RIGHT of the target line — OPEN for a right-hander | pointing left — closed for a right-hander |
| `lm.dynamicLoft` | ° | device | more loft delivered to the ball | the face delofted past square |
| `lm.spinLoft` | ° | device | a larger angle between delivered loft and the direction of travel | the face delivered below the path direction |
| `lm.lieAngle` | ° | device | the clubhead sole toe UP relative to the ground at impact | the sole toe DOWN; zero is flat to the ground |
| `lm.closureRate` | °/s | device | the face rotating CLOSED through impact — a faster closing rate | the face rotating open; a low or stable value is a squarer, held-off release |

#### Tempo & sequence

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `tempoBackswing` | s | live | a longer backswing | *cannot go negative* |
| `tempoRatio` | :1 | live | a backswing slower relative to the downswing | *cannot go negative* |
| `kinematicSequence` | — | planned | *no direction* | *cannot go negative* |

#### Alignment

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `elbowAlignment` | ° | live | the TRAIL elbow sits above the lead elbow | the lead elbow sits above the trail elbow |
| `feetAlignment` | ° | live | the TRAIL ankle sits above the lead ankle — a closed stance | the lead ankle sits above the trail ankle — an open stance |

#### Head

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `headSway` | cm | live | the head moved toward the LEAD side | moved away from the lead side |
| `headLift` | cm | live | the head rose | the head dropped |
| `headTilt` | ° | live | the eye line tilted further than at address, trail-end high | tilted the other way from address |

#### Arms

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `leadArmToTorso` | ° | live | the arm further from the torso — UNSIGNED 0–180°, a frontal projection cannot say  which side it left on | *cannot go negative* |
| `trailElbowHeight` | % shoulder width | live | the trail elbow higher above the shoulder line | below the shoulder line |
| `leadHandWidth` | % arm length | live | the hands further from the chest — a wider arc | the hands closer to the chest |
| `leadUpperArmToChest` | % shoulder width | live | a larger gap — the arm further from the chest | *cannot go negative* |

#### Ball flight

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `launchDirection` | ° | planned | the ball starting RIGHT of the target line — a push for a right-hander | starting left — a pull |
| `launchAngle` | ° | planned | a higher launch | the ball leaving below horizontal |
| `ballSpeed` | mph | planned | a faster ball off the face | *cannot go negative* |
| `lm.ballSpeed` | mph | device | a faster ball off the face | *cannot go negative* |
| `lm.launchAngle` | ° | device | a higher launch | the ball leaving below horizontal |
| `lm.launchDirection` | ° | device | the ball starting RIGHT of the target line — a push for a right-hander | starting left — a pull |
| `lm.faceToPath` | ° | device | the face OPEN to the path — curvature to the right, a fade | closed to the path — curvature to the left, a draw |
| `lm.spinRate` | rpm | device | more total spin | *cannot go negative* |
| `lm.backSpin` | rpm | device | more backspin | *cannot go negative* |
| `lm.sideSpin` | rpm | device | spin curving the ball RIGHT | curving the ball left |
| `lm.spinAxis` | ° | device | the axis tilted RIGHT — a fade or a slice | tilted left — a draw or a hook |
| `lm.carryDistance` | yd | device | the ball carrying further | *cannot go negative* |
| `lm.totalDistance` | yd | device | the ball finishing further away | *cannot go negative* |
| `lm.offline` | yd | device | finishing RIGHT of the target line | finishing left of it |
| `lm.peakHeight` | ft | device | a higher flight | *cannot go negative* |
| `lm.descentAngle` | ° | device | a steeper descent — the ball stopping faster | *cannot go negative* |
| `lm.distanceToPin` | yd | device | finishing further from the pin | *cannot go negative* |

#### Strike

| Metric | Unit | Status | Positive means | Negative means |
|---|---|---|---|---|
| `lm.smashFactor` | ratio | device | a more efficient strike — more of the club's speed reaching the ball | *cannot go negative* |
| `lm.strikeLocation` | mm | device | struck toward the TOE | struck toward the heel |
| `lm.strikeHeight` | mm | device | struck ABOVE the centre of the face | struck below centre |

<!-- END GENERATED SIGN TABLE -->

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

**`.domain` — where the metric still means something.** A `PhaseDomain {first, last}`, inclusive,
defaulting to the whole swing, which is the right answer for almost every metric: leave it out.
Narrow it only where the geometry itself expires, not where the number merely gets noisy. The ten
frontal-plane metrics do — `pelvisSway`, `pelvisLift`, `leadKneeDrift`, `plumbBobDistance`,
`hipLineTilt`, `shoulderPlaneAngle`, `elbowAlignment`, `spineSideBend`, `secondaryAxisTilt`,
`thoraxLateralDrift` all take the file-local `P1toP7` constant — because past impact the body has
turned far enough that a face-on lateral reading is reporting the *rotation* in a translation's
units. `comOverLeadFoot` deliberately does not narrow: a distance along the stance line survives the
turn, and it is read at the finish on purpose.

The domain is read in **ladder order** (`phaseLadderIndex` / `phaseInDomain` in
`metric_descriptor.h`), never in enum order. `Phase` is append-only, so sorted by value it reads
`P1, takeaway, P4, transition, downswing, P7, release, P10, P3, P6, max speed, P9, P2, P5, P8` — a
numeric comparison against an Address→Impact domain would wrongly **exclude P2, P3, P5 and P6**
(enum values 12, 8, 13 and 9, all above Impact's 5) and wrongly **include the finish** (value 7).
P8 happens to come out right at 14; that is luck, not a rule.

**Two things read the domain at runtime**, and they are the two places a wrong number would reach a
person:

- the **pack validator** (`validateMeasureDomains`, called from `validatePack` and from
  `diagnostics_health`) refuses an `at` anchor, or a delta/rate/extremum window, that reaches
  outside it — reported as `measureOutsideDomain`;
- the **chart** dims and dashes the region beyond it, draws no phase dots there, and clamps the
  summary window to it.

**Phase samples are not suppressed by the domain, and do not need to be.** Producers sample the
descriptor's own `phases` list, and `metric_catalogue_test` asserts `phases ⊂ domain` — that
containment is what keeps every emitted phase sample inside the domain, and it is checked at build
time rather than filtered at run time. So a metric documenting itself at a phase it cannot be read
at fails the build, because a descriptor contradicting itself is the bug; there is no runtime pass
that would quietly paper over it. See design §5.1.

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

Metrics that were *examined and declined* are not here — they are in Appendix B, with the objection
recorded. Read that before proposing a shortcut to anything in this table.

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

That claim is now checked rather than trusted, because it stopped being true once: the launch
monitor rename left nine rows pointing at keys that no longer existed and twenty-five descriptors
with no row at all. `python3 tools/metrics/gen_sign_table.py` regenerates the sign table **and**
exits non-zero listing any stale or missing Appendix A row. Run it after touching the manifest.

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
| **device** | The *reading* comes from hardware we integrate rather than a producer we author. Requires the device, and **live** — the GC Quad connector reads FSX2020's `LastShot.CSV`, so these resolve Measured on any shot the monitor reported. Every one is keyed `lm.*`: where we also estimate the quantity ourselves the bare key keeps our estimate, because comparing the two is the point. There is no detection work, only the connector. |
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

`plumbBobDistance` is the one member in a real-world unit, and the exception is argued in
[`../design/lower_body_face_on_metrics.md`](../design/lower_body_face_on_metrics.md) §6a: the module
is body-relative because a norm in millimetres is usually a norm on the golfer's height, and the
plumb bob is the case where it is not — the figures it is graded against are absolute inches that do
not scale with the player, and they differ **by club**, including in sign.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `pelvisSway` | live | FaceCam + ground | `buildLowerBodySeries` lateral translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `pelvisLift` | live | FaceCam + ground | vertical translation ✓ | camCal, ground | `lower_body_metrics_test` · (mocap owed) |
| `hipLineTilt` | live | FaceCam | hip-line angle vs horizontal ✓ | camCal | `lower_body_metrics_test` · (corpus) |
| `plumbBobDistance` | live | FaceCam + Ball | hip centre − ankle-line centre, projected on the stance line, through the ball-diameter ruler ✓ | **px→in**. **ABSENT when the ruler does not resolve**, never re-expressed in another unit | `lower_body_metrics_test` · corpus distribution owed (the four club corridors are heuristics) |
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
| `lm.clubheadSpeed` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |

### Club delivery

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `shaftAngleVsHorizontal` | live | FaceCam + Club | `buildClubDeliverySeries` shaft vs horizontal ✓ | px→mm | `club_delivery_test` · (corpus) |
| `attackAngle` | live | FaceCam + Club | vertical velocity angle at Impact (`PointInTime`) ✓ | px→mm | `club_delivery_test` · (launch monitor) |
| `lowPointAhead` | live *(Bridged)* | FaceCam + Club + Ball | **synthesized-arc** low-point vs ball (`PointInTime`), σ = 2.0 in ✓ | px→mm | `club_delivery_test` · (launch monitor, n=6) |
| `lm.faceAngle` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.dynamicLoft` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.spinLoft` | device | **LM** | derived in the connector from the row ✓ | — | `gcquad_csv_parser_test` |
| `swingPlane` | planned | **DTL** + Club | SVD best-fit plane of head path — needs the path in 3D | stereo | new unit · (DTL cross-check) |
| `clubPath` | planned | **DTL** + Club | horizontal velocity angle — needs depth | stereo | new unit · (launch monitor) |
| `shaftDirection` | planned | **DTL** + Club | shaft pointing vs target line — needs depth | stereo | new unit · (DTL cross-check) |
| `lm.attackAngle` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.clubPath` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.lieAngle` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.closureRate` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |

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
| `lm.faceToPath` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.spinAxis` | device | **LM** | derived in the connector from the row ✓ | — | `gcquad_csv_parser_test` |
| `lm.spinRate` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.carryDistance` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `launchDirection` | planned | **DTL** + Ball (high rate) | initial ball vector, horizontal | camCal / stereo | new unit · (launch monitor) |
| `launchAngle` | planned | FaceCam + Ball (high rate) | initial ball vector, vertical | camCal / stereo | new unit · (launch monitor) |
| `ballSpeed` | planned | FaceCam + Ball (high rate) | post-impact ball speed | px→mm | new unit · (launch monitor) |
| `lm.ballSpeed` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.launchAngle` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.launchDirection` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.backSpin` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.sideSpin` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.totalDistance` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.offline` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.peakHeight` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.descentAngle` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.distanceToPin` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |

### Strike

Both read from the monitor; there is no detection work here, only the connector.

| Metric | Status | Capture | Detection | Calibration | Verification & validation |
|---|---|---|---|---|---|
| `lm.smashFactor` | device | **LM** | derived in the connector from the row ✓ | — | `gcquad_csv_parser_test` |
| `lm.strikeLocation` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |
| `lm.strikeHeight` | device | **LM** | GC Quad connector ✓ | — | `gcquad_csv_parser_test` |

---

## Appendix B — considered and not pursued

Appendix A0 ranks the metrics we **intend** to build. This one records the metrics we **do not**, and
it exists because the arguments against them are not obvious and keep being rediscovered. Every entry
below was reached the same way — by reading the diagnostic model for conditions that name a cause the
pipeline can never see, and asking what new metric would close the gap — and every one of them was
set aside on the merits, not on effort.

**Nothing here is a defect and nothing here is a roadmap item.** The model is authored ahead of the
producers on purpose (see the note opening §8.4 of the diagnostics guide); a condition with no live
signal is the library doing its job. What this appendix adds is the *reason* a particular closing
move was declined, so that the next reader who spots the same gap starts from the objection rather
than from the idea.

**Standing of the review.** Taken against `core` pack `1.0.0` (146 conditions, 110 measures, 306
edges) on 2026-08-03. At that reading **74 of 146 conditions had no live signal** — 31 with no signal
authored at all, 29 blocked on a `planned` metric, 10 on a launch monitor, 4 on the two `noProducer`
spine curves — and **49 of the 100 nodes that are the `from` end of a `causes` edge** could not be
detected. Those counts move as the pack does; regenerate them rather than quoting these.

```sh
# Causes the model can name but the pipeline cannot see, ranked by explanatory reach.
python3 -c "
import json, collections
d = json.load(open('src/Resources/diagnostics/core.json'))
M = {m['id']: m for m in d['measures']}; S = {s['id']: s for s in d['signals']}
C = {c['id']: c for c in d['conditions']}
def live(c):
    return any(all(M[m]['status'] == 'live' for m in S[s]['measures'])
               for s in c.get('detectedBy', []))
reach = collections.Counter(e['from'] for e in d['edges'] if e['type'] == 'causes')
for cid, n in reach.most_common():
    c = C.get(cid)
    if c and not live(c):
        print(n, cid, c['kind'], c['prominence'], c['confirmedBy'])"
```

### B.1 Video-measured physical screens

*Would close:* the 14 `capacity` conditions with no signal, which between them sit at the head of the
causal graph — `limited_thoracic_rotation` (15 effects), `poor_pelvic_disassociation` (13),
`limited_trail_hip_ir` (13), `poor_core_stability` (10), `limited_lead_hip_ir` (9),
`poor_single_leg_balance` (7): **67 causal edges**, more than any producer in Appendix A0.
`screens.json` already carries the protocols, and each is a 2D angle or distance from a fixed
camera — the thing pose estimation is best at.

*Why not.* The protocol is not the geometry. Every screen in that registry qualifies its measurement
with a constraint the camera cannot police — *keep the pelvis still*, *stop at the first resistance
rather than at end range* — and the registry says so in as many words: "letting it lift is what makes
this test read high". A camera would return a confident number for a quantity defined differently
from the one the pass criterion was drawn against, and it would return it *unsupervised*, which is
exactly the condition under which those constraints fail. A screen also asserts a **capacity**, and a
capacity is a property of the body rather than of a swing; adopting it would mean a second kind of
capture, a second estimand and a second calibration story before the first number appeared.

*What would change our mind.* A guided protocol in which the pose stack gates the reading on
compliance (pelvis-stable, limb-plane-in-view) and refuses rather than estimates when it cannot, plus
a cohort validated against goniometry. Absent the refusal path this stays out — the failure mode is
silent, and silent is the one we cannot ship.

### B.2 Optical ball flight for `launchAngle` / `ballSpeed`

*Would close:* `launch_low`, `launch_high`, `ball_speed_deficit`. The argument for trying is real and
worth restating so nobody has to reconstruct it: launch angle lives in the vertical plane containing
the target line, which **is** the face-on image plane, so it is a face-on reading by the same
reasoning `attackAngle` is (and by which DTL is the one view that cannot take it).

*Why not.* Frame rate, not projection — see the `launchMonitor` comment in `metric_descriptor.h:52`.
The ball leaves at tens of metres per second and the initial vector is defined over the first few
centimetres of flight; at our capture rates it is a blurred streak that has left the detector's
ROI within a frame or two. The Appendix A rows already say `Ball (high rate)`, which is the honest
statement: this is a **capture** gap wearing a producer's clothes, and building a producer against
the frames we have would give a number whose error is larger than the corridor it is read against.

*What would change our mind.* A high-rate capture path with a dedicated post-impact ROI and a known
exposure — at which point the existing `planned` rungs resolve with no catalogue change.

### B.3 `smashFactor` from `ballSpeed` ÷ face-on `clubheadSpeed`

*Would close:* `smash_deficit`, and it is tempting because it would move a `device` row to `live`
without a connector — `clubheadSpeed` already has a live `faceOnClub` rung.

*Why not.* Beyond depending on B.2, the arithmetic destroys the metric. The face-on clubhead rung is
`Projected`: it differentiates the head path in the image plane and is missing the axial term, which
is precisely why the ladder puts `clubSensorFused` *above* the second camera rather than below it. A
few per cent of error in the denominator moves smash factor by more than the whole band that
separates a centred strike from a poor one. A ratio cannot be more trustworthy than its worse input,
and here the worse input's error sits in the same direction as the effect being diagnosed.

*What would change our mind.* The `clubSensorFused` rung landing. With a shaft sensor supplying the
axial term the denominator becomes Direct, and then only B.2 stands between us and a monitor-free
smash factor.

### B.4 Acoustic strike signature

*Would close:* `strike_toe`, `strike_heel` (both `common`, both launch-monitor-only today), with
corroboration for `chunk` / `thin` / `top` / `sky` / `shank` — seven `outcome` conditions currently
`asserted`, i.e. the golfer has to tell us. The appeal: we already capture the audio and already
detect the onset, so the transient's spectral content is data on disk that nothing reads.

*Why not.* The timbre is dominated by variables we neither control nor calibrate — face material and
head construction, ball model, shaft, room, microphone and its distance. Toe/heel discrimination is
robust for one fixed combination of those and does not survive a corpus that varies all of them.
Worse, the discrimination is not signed: toe and heel misses both go duller, so the feature separates
*centred from not* far better than it separates the two conditions the model actually distinguishes.
`AcousticShotDetector` is deliberately a level-and-onset device for that reason — it uses audio for
the one thing audio is unambiguously best at, which is *when*.

*What would change our mind.* A per-club acoustic baseline captured in-session (so the reading is
relative to that club's own centred strike, not to an absolute), plus a corpus carrying
launch-monitor strike labels to validate against. Note this would only ever yield a *quality* scalar;
toe-versus-heel needs a different modality.

### B.5 Grip geometry from whole-body hand keypoints

*Would close:* `grip_strong` (3 effects) and `grip_weak` (`common`, 2 effects), both `intent` /
`asserted` today. The hands (COCO-WholeBody 91–132) are already decoded; their only consumer is
`hand_axis.h`, as a smoothed anchor for the shaft tracker.

*Why not.* Two reasons that compound. The hands at address are the most occluded landmarks in the
frame — they overlap each other on the grip and the trail hand is largely behind the lead — which is
why `pose.gripFromSmoothedHands` is still defaults-off and awaiting its own corpus evaluation rather
than promoted. And grip strength is conventionally defined by knuckle count or the direction of the
V's, both of which are the hand's **rotation about the shaft**: a depth reading, on the axis the
face-on camera does not have. A projected substitute would be a different quantity sharing a name
with the coaching term, which is the specific mistake the descriptor voice exists to prevent.

*What would change our mind.* Hand-keypoint confidence characterised at address on the corpus, and a
definition of grip strength that survives projection — most plausibly against the live
`forearmPronation` DOF at P1 rather than off the hand landmarks at all.

### B.6 `kinematicSequence` off the face-on rotation series

*Would close:* the sequence ordering, and it looks nearly free — the descriptor says what is missing
is "angular-SPEED series for the pelvis and thorax to order", and `pelvisRotation` / `thoraxRotation`
both carry **live** face-on rungs already. Differentiate what we have.

*Why not.* Those rungs are `Estimated`, and their summaries say how: *from the collapse of the hip /
shoulder span in the face-on image*. That is a cosine, and a cosine is flattest exactly where the
sequence needs the most resolution — near the top and through transition, where the spans are most
foreshortened. Differentiating a foreshortening estimate amplifies the error where it is already
worst, and sequence is a claim about the *timing of peaks* to within tens of milliseconds. The
metric would resolve, chart, and be wrong in a way no reader could see.

*What would change our mind.* Nothing about the maths; this one is waiting on the pelvis and thorax
IMUs its route already declares. The route ladder is right and the shortcut is not.

### B.7 A face-on rung for `leadKneeFlexion` / `trailKneeFlexion`

*Would close:* `late_buckle`, `excessive_knee_flex`, `insufficient_knee_flex`, `trail_knee_straighten`
— four conditions, and the rung already exists in the manifest.

*Why not.* It exists and is marked `PLANNED` with the reason attached: "a sagittal angle foreshortened
by the frontal projection — visible, but close enough to noise that no producer reads it there
today". Across the range that matters (roughly 20°–35° at address) the projected shin-against-thigh
difference is a handful of pixels, and the knee is one of the noisier keypoints. Building it would
put four conditions into `Bridged` on a reading that cannot separate the bands they are authored
against.

*What would change our mind.* Measure it before deciding — a corpus pass quantifying the projected
angle's spread against the DTL truth would settle this either way, and it is the cheapest experiment
in this appendix. Absent that, the `dtl` rung is the answer.

### B.8 A `Range` reducer, and cross-shot dispersion

*Would close:* `limited_wrist_mobility` off the live IMU DOF series; `tempo_habit` and `stance_habit`
off `tempoRatio`, `stanceWidth` and `ballPosition`, all live. `ReducerKind` is
`At | Delta | Rate | Extremum` and a peak-to-peak range is a small addition to it.

*Why not.* Two distinct objections, and only the second is about cost.

*Range* is cheap and would be measuring the wrong thing. Range **used** in a swing is not range
**available** in the joint: a golfer with full mobility who does not use it reads identically to one
who cannot, and `limited_wrist_mobility` is a `capacity` — the model classes capacities as `latent`
precisely so this substitution cannot be made quietly. Adding the reducer to serve that condition
would license the category error at the schema level.

*Dispersion* is not a reducer at all. Every measure in the model reduces one series from one shot;
consistency is a property of a shot **set**, which has no estimand here — no window, no cohort, no
statement of what varies legitimately between clubs or over a session. That is a model-shape question
(see `docs/implementation/score_estimand_alignment_plan.md`), not a catalogue one, and answering it
in the catalogue would be answering it in the wrong place.

*What would change our mind.* A `Range` reducer is defensible on its own merits for series where
range-used *is* the quantity of interest — author it for one of those and the wrist-mobility use stays
out. Dispersion waits on a session-level estimand.

### B.9 Face-on proxies for `early_extension` and `loss_of_posture`

*Would close:* two `ubiquitous` faults, currently blocked on `pelvisThrust` and `spineForwardBend`,
both DTL. Two proxies suggest themselves: the pelvis coming toward the camera changes its **apparent
hip span**, which is a depth cue a single view does have; and loss of posture might be composed from
the live `headLift` and `pelvisLift`.

*Why not.* The hip-span proxy is already spoken for. `pelvisRotation`'s Estimated rung reads the same
collapse of the same span, and turning is what the pelvis does through the downswing — one signal
cannot be depth and rotation at once, and the model draws `causes` edges between the two. We would be
measuring one quantity twice, then observing a relationship we had manufactured. The `headLift` +
`pelvisLift` composition fails differently: both rise in a perfectly good pivot, so the composite has
no discriminating power without the spine angle, which is the sagittal reading we do not have and the
reason `spineForwardBend` is planned in the first place.

*What would change our mind.* For early extension, a depth cue that is independent of turn — which in
practice means DTL or a trunk IMU. For loss of posture, the `trunkImus` rung `spineForwardBend`
already declares.

### B.10 Out of scope for this appendix — live metrics no condition reads

The review also surfaced the mirror-image gap: **12 metrics with a live rung that no characteristic
reads** — `wristScore`, `wristResemblance`, `trailWristFlexExt`, `xFactor`, `spineSideBend`,
`clubheadSpeed`, `tempoBackswing`, `stanceWidthMm`, `leadFootFlare`, `trailFootFlare`, `toeLineAngle`,
`elbowAlignment`. Some are chart-only rollups and correctly unreferenced; others (`xFactor` is the
sharpest example, since its `xFactorStretch` sibling *is* wired) look like model content that was
never authored.

That is not a metric gap and no work in this catalogue would close it. It belongs to the diagnostics
pack — a measure, a signal and a condition, no producer involved — and is recorded here only so the
finding is not lost between the two guides. Regenerate the list before acting on it:

```sh
# Live metrics that no condition's signal reaches.
python3 -c "
import json, re, collections
d = json.load(open('src/Resources/diagnostics/core.json'))
M = {m['id']: m for m in d['measures']}; S = {s['id']: s for s in d['signals']}
reached = {M[mid]['metricKey'] for c in d['conditions'] for s in c.get('detectedBy', [])
           for mid in S[s]['measures'] if mid in M}
src = open('src/Metrics/metric_catalogue_manifest.cpp').read()
for b in re.split(r'cat\.addDescriptor\(\{', src)[1:]:
    key = re.search(r'\.key = QStringLiteral\(\"([^\"]+)\"\)', b).group(1)
    r = b[b.find('.routes'):]
    if r.count('via(') > r.count('PLANNED') and key not in reached:
        print(key, '(measure authored)' if any(m.get('metricKey') == key for m in d['measures'])
                   else '(no measure)')"
```
