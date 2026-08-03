# Pinpoint Shot Analyzer — Developer Guide

**Audience**: Developers working on or integrating with the Pinpoint application  
**Location**: `src/Analysis/` (analyzers + math), `src/Gui/shot/shot_processor.{h,cpp}` (orchestration), `src/Export/swing_doc.{h,cpp}` (persistence)  
**Language**: C++17 (analysis value types and math) / C++20 (app integration)  
**Status**: Production. Two analyzers: `WristAnalyzer` (session type 1 — the full IMU + camera stage profile) and `CameraKinematicsAnalyzer` (types 0/2/3 and the unknown fallback). Offline re-analysis of an exported swing runs the same analyzers over a disk-backed window.

> **Scope.** This is the *outside* view: how `analyze()` is launched, how jobs are
> built, how results join with the export, and how everything persists. The
> *inside* of `analyze()` — the capability-gated stage pipeline and every current
> stage — is `docs/developer/analysis_pipeline_developer_guide.md`.

---

## Contents

1. [What the Shot Analyzer Is](#1-what-the-shot-analyzer-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [The ShotProcessor Pipeline, Stage by Stage](#4-the-shotprocessor-pipeline-stage-by-stage)
5. [Getting Started — Writing a New Analyzer](#5-getting-started--writing-a-new-analyzer)
6. [Wrist-Chain Facts Worth Knowing Here](#6-wrist-chain-facts-worth-knowing-here)
7. [The Scoring Model](#7-the-scoring-model)
8. [Output Shapes — From Worker to QML](#8-output-shapes--from-worker-to-qml)
9. [Persistence — the Unified swing.json](#9-persistence--the-unified-swingjson)
9a. [Offline Re-analysis](#9a-offline-re-analysis)
10. [Threading and Lifetime Rules](#10-threading-and-lifetime-rules)
11. [Internals — Design Decisions Explained](#11-internals--design-decisions-explained)
12. [Testing](#12-testing)
13. [Common Mistakes](#13-common-mistakes)
14. [File Map](#14-file-map)

---

## 1. What the Shot Analyzer Is

The shot analyzer turns a **frozen 4-second `SwingWindow`** — IMU samples,
camera frames, and the shot marker — into everything the user sees about a
shot: a 0–100 **score**, per-metric **values at impact** (the carousel chips),
a **trace** sparkline, and the rich **`SwingAnalysis` detail** (full metric
curves over a shared time grid, swing-phase events, score breakdown, ranked
faults) that drives the replay-synced metric graph.

It is a **per-session-type** abstraction: a Wrist session and a GRF session
analyse the same kind of window with entirely different pipelines. The
`ShotAnalyzer` interface plus a factory keyed on
`SessionController::Type` keep that polymorphism out of the orchestration code.

Two implementations exist today:

| Session type | Analyzer | What it does |
|---|---|---|
| 1 — Wrist | `WristAnalyzer` | The full stage profile: IMU fusion, segmentation, the offline pose pass, ball/shaft tracking, metrics, scoring |
| 0 / 2 / 3 and unknown | `CameraKinematicsAnalyzer` | Shared camera-kinematics analyzer — a placeholder score plus the real clubhead/hand-speed and lag series from the face-on camera once kinematics is enabled |

The same `analyze()` also runs **offline**, over a swing reconstructed from an
exported folder rather than from the live ring — see §9a.

Each assessment type — **Wrist, Swing, GRF** — produces its **own**
session-appropriate score (its own bands and weights, so the number reflects
what is being assessed); the **Coach** type is the AI-coach **feedback layer on
top**, not a fourth independent score. Every analyzer is **maximal and
degradable**: it extracts as many metrics as the present cameras/IMUs support —
degrading per-metric on confidence — and never fails the whole result for a
missing sensor (`ok=false` is reserved for *no* usable data).

The analyzer is **not** shot detection (deciding that/when a shot happened —
see `docs/developer/shot_detector_developer_guide.md`) and **not** the media export
(encoding MP4s + thumbnail — see `docs/developer/swing_export_developer_guide.md`). It
runs *ahead of* the export over the same frozen window — the two are **sequenced**,
not overlapped (§11) — and the second to finish triggers the join before anything
is published.

---

## 2. Where It Fits in Pinpoint

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  ShotController ──shotDetected(source, impactUs, sessionType)──┐             │
│                                                                ▼             │
│                                                        [ShotProcessor]       │
│                                            POSTROLL (1250 ms auto / 500 man) │
│                                                                │             │
│                                       pauseBuffer → captureSwingWindow(4 s)  │
│                                                                │             │
│                              PROCESSING (sequenced)                          │
│                       ▼ (QtConcurrent) segmentation pre-stage                │
│                       ▼ (QtConcurrent)                                       │
│              [ShotAnalyzer]  makeShotAnalyzer(type)                          │
│              runStages(profile, ctx)  → see the pipeline guide               │
│                       │ analysis done                                        │
│                       ▼ (QtConcurrent)                                       │
│              [SwingExporter]  MP4s + thumb.jpg + raw manifest                │
│                       │                                                      │
│                       ▼                                                      │
│                      join   writeSwingJson (unified doc)                     │
│                       │     ShotListModel::addShot (ALWAYS)                  │
│                       ▼                                                      │
│                  REPLAYING (¼×, iff both OK)                                 │
│                       │                                                      │
│            finish: window destroyed → applyCaptureIntent() → Idle           │
│                    (trigger re-arms)                                         │
└──────────────────────────────────────────────────────────────────────────────┘
```

Consumers of the result:

- **`ShotListModel`** — every shot lands on the carousel, no matter what failed
  (failures degrade to `hasVideo=false` / score 0).
- **`ShotProcessor.replayAnalysisDetail`** — the in-replay metric graph
  (`ScreenWrist`) binds to the detail of the shot currently replaying, synced
  to the replay playhead (`replayPositionUs`, same µs domain as the series).
- **`swing.json`** — the analysis is folded into the one per-shot document so
  the shot reloads after a restart (`SwingDocReader` →
  `ShotListModel::addPersistedShot`) and feeds the session-review drawer.

---

## 3. Core Concepts

### `ShotAnalysisJob` — the value-type job

Everything the worker needs, **resolved on the UI thread before launch** (the
same rule as `SwingExportJob`). The worker must never touch `AppSettings`,
controllers, or live `ImuInstance` objects. The job has grown well past its
original handful of fields; the groups are:

| Group | Fields | Notes |
|---|---|---|
| **Identity** | `sessionType`, `shotSource`, `impactUs`, `swingDir` | `impactUs` is the marker anchor — the single source of truth (§11) |
| **Sources** | `cameraSources` (face-on first), `faceOnCameraCount`, `imuSources`, `markerSourceId` | Discovered from the window's own `formatOf()` descriptors |
| **Athlete / club** | `handedness`, `clubLengthM`, `clubName`, `bandCentersMm`, `shaftType`, `hoselFromButtMm` | Band geometry is empty for an untaped club — the shaft tracker then runs ray evidence only, no band tier |
| **Club-length prior** | `priorClubLenPx`, `priorClubLenVarPx`, `priorClubLenN` | A persisted `LengthPriorState` so the length fuse can join an E-prior. `N=0` ⇒ no prior joins |
| **Calibration** | `imuBindings` | `{SourceId, SegmentRole, alignA, mountM}` snapshots — see below |
| **Model selection** | `motionCaptureQuality` | `"High"` runs ViTPose++-L when downloaded; anything else runs ViTPose-B |
| **Ball** | `ballTrack`, `ballSearchRoi`, `ballBaseline` | The live face-on ball track, the hitting-area ROI, and the live empty-mat baseline. All optional; each has a defined fallback |
| **Progress** | `progress` | `std::function<void(float)>`, 0..1, **called from the worker thread**. May be null — check before calling |
| **SwingLab injection** | `poseTrackPath`, `ballTrackPath`, tuning overrides | Load a recorded pose/ball track instead of running the detector. Empty in production |

Two rules about the job that are easy to get wrong:

- **Every field has exactly two fillers**, and they must stay in step:
  `ShotProcessor::buildAnalysisJob()` on the **live** path (reading
  `AppSettings`), and `SwingDiskLoader` on the **re-analysis** path (reading the
  recorded `swing.json`, *never* `AppSettings` — that is what makes re-analysis
  deterministic). Adding a field means filling it in both.
- **`progress` is a worker-thread callback.** The `ShotProcessor` lambda posts a
  queued invoke; anything else you install must marshal to its own thread itself.

### `ImuSegmentBinding` — calibration snapshot

`{SourceId, SegmentRole, alignA, mountM}`. The anatomical calibration
quaternions A and M live on the `ImuInstance` (session-lifetime, GUI thread),
so they are *copied into the job* at build time. `SegmentRole` comes from the
user's placement slots (`AppSettings::imuPlacement`): for Wrist sessions,
slot A = LeadForearm, B = LeadHand, C = LeadUpperArm
(`segmentRoleForSlot`, shot_processor.cpp). Unknown roles are skipped by the
fuser but their A/M still travel with the job.

### The TimeGrid and `FusedStreams`

Raw IMU sources tick at their own rates and phases. `ImuVisionFuser::fuse()`
builds one fixed-rate (200 Hz) **master TimeGrid** over the bound IMUs' common
in-window coverage and resamples every segment onto it as **anatomical
quaternions** `q_anat(t) = A·q_raw(t)·M` (slerp via
`SwingWindow::interpolateImu`). Everything downstream — phases, metric curves,
the replay graph — indexes this one grid. Timestamps stay **absolute µs in the
`EventBuffer::nowMicros()` domain**, so the replay playhead needs no
conversion.

### Phases

`PhaseSegmenter` emits the swing-phase timeline (`Address / Top / Impact /
Finish` in M1). **Impact is never detected here** — it is the hard
`ShotMarker` anchor passed in via `job.impactUs`, clamped to the grid. Phases
carry confidences; low-confidence ticks fade in the UI rather than disappear.

### `MetricSeries`

One named metric's full story: `{key, label, unit}`, the continuous curve
(`t_us[]` + `value[]` over the TimeGrid), sparse `PhaseSample`s (the value at
Address/Top/Impact, with a score band at scored phases), and an optional
ideal band. Values are degrees, **Address-referenced** (zero at setup).
`flexPositive` records stored-sign polarity — flip only at the UI label, never
in storage.

### `ShotAnalysisResult` vs `SwingAnalysis`

The result has two tiers. The flat tier (`score`, `metrics` map,
`tracePoints`) mirrors the `ShotListModel` roles exactly, so the join hands
them to `addShot()` unmodified. The rich tier (`detail`, a
`shared_ptr<SwingAnalysis>`) carries the series/phases/score-breakdown/faults
— null from stub analyzers, folded into `swing.json` and the replay graph when
present.

### Degradation, not failure

`ok=false` (with `error`) is a *normal* outcome — no usable IMU data, missing
hand sensor, etc. The shot still lands on the carousel with score 0. The
analyzer must never crash on a sparse window; it degrades.

---

## 4. The ShotProcessor Pipeline, Stage by Stage

`ShotProcessor` (QML context property `shotProcessor`) owns the post-shot
pipeline and the SwingWindow lifecycle. States: `Idle → PostRoll → Processing
→ Replaying → Idle`; `busy` (any non-Idle state) disarms `ShotController` for
the duration.

### POSTROLL — `onShotDetected`

Captures the trigger tuple (source, impact µs, session type, wallclock label)
and starts a single-shot timer (`postRollMsFor(source)` — **1250 ms** for every
auto source, 500 ms for the manual button, which is pressed after the swing is
already over). The buffer **keeps capturing** so the follow-through lands in
the ring before it freezes.

### Freeze — `captureWindowAndLaunch`

`pauseBuffer()` → `captureSwingWindow(4 s)`. If the user pressed Stop during
the post-roll the rings froze early — still a valid (truncated) shot; only
buffer teardown aborts. One `ReplayTrack` is built per live camera with frames
in the window.

#### The skip-analysis corpus-capture escape hatch

Before anything heavy starts, the processor checks
`saveRawFrames() && skipAnalysisForRawCapture()`. When both are set it marks
analysis `Skipped`, exports the frames only, and suppresses the replay — so each
shot captures **instantly** and the swings are re-analysed in bulk later. The
raw-only `swing.json` still carries `capture.impactUs`, so offline re-analysis
(§9a) has its impact reference. This is how the blessed corpus is built.

#### The segmentation pre-stage

Otherwise a **milliseconds-cheap** fuse + inertial ladder runs first, on its own
`QtConcurrent` worker, gating both heavy workers. Its swing bounds trim the
export encode span and the replay to the swing rather than the whole window.
Failure, or no IMU, yields a confidence-0 result and everything below degrades to
full-window behaviour — it is an optimisation, never a gate on correctness.

After it resolves (`onSegmentationFinished`), the two heavy workers run
**sequenced, not overlapped** — analysis first, the x264 export only once
analysis finishes:

```cpp
// onSegmentationFinished(): launch analysis alone.
startAnalysis();    // QtConcurrent: makeShotAnalyzer(type)->analyze(*win, job)

// onAnalysisFinished(): pose pass done — now launch the export, which has the
// cores to itself.
startSwingSave();   // QtConcurrent: SwingExporter::run(*win, exportJob)
```

Both read the SAME frozen window — const, zero-copy reads over stable memory
(producers stopped while Paused). They no longer run at the same time: the
offline ViTPose pose pass inside `analyze()` is the wall-time bottleneck and is
far faster multi-threaded, so it gets the machine to itself; overlapping it with
the export's encode threads inflated per-frame inference ~5× (§11).

### Job building — `startAnalysis`

`buildAnalysisJob()` is where every UI-thread read happens: camera sources
ordered **face-on first** (so analyzers can prefer it without re-sorting), IMU +
marker sources discovered from the window's own `formatOf()` descriptors, athlete
handedness and club record, the motion-capture quality tier, the live ball track
and ROI, the persisted club-length prior, and the `ImuSegmentBinding` snapshots
(§3). The job then moves into the lambda by value.

The progress callback is installed here too: the analyzer calls it from the
worker thread, and the lambda posts a queued invoke that drives
`analysisProgress` (throttled to ~1 % steps) for the toolbar's ANALYSING bar.

The re-analysis path builds the same job from `swing.json` instead — see §9a.

### Join — `maybeJoin`

Runs when **both** outcomes are non-Pending (each watcher calls it; so does
the synchronous export-skip path):

1. **The one unified `swing.json`** is written here, on the GUI thread, after
   both workers returned — so neither worker ever writes the file itself. Export
   OK → exporter manifest + inline `"analysis"` block. Export
   failed/skipped but analysis OK → a **synthesised minimal manifest**
   (`buildSynthManifest`) so an analysis-only shot still survives a restart.
2. **`ShotListModel::addShot()` always runs** — with whatever the pipeline
   produced (trace/score/metrics empty or 0 on analysis failure;
   `hasVideo=false` on export failure).
3. **Replay gating**: the ¼× replay starts only when analysis AND export both
   succeeded and camera tracks exist; otherwise straight to `finishShot()`.

| analysis | export | swing.json | carousel row | replay |
|---|---|---|---|---|
| OK | OK | manifest + analysis | full | yes |
| OK | failed/skipped | synth manifest + analysis | no video, real score | no |
| failed | OK | manifest only (raw) | video, score 0 | no |
| failed | failed | none (in-memory shot only) | degraded | no |
| **skipped** (corpus capture) | OK | manifest only (raw, with `capture.impactUs`) | video, score 0 | **suppressed** |

### Finish

`finishShot()` (or the teardown stop-barrier `finishNowBlocking()` — camera
deselect and both destructors call it *before* any deregister) destroys the
window, then restores the user capture intent; `bufferStateChanged` re-arms
the trigger.

---

## 5. Getting Started — Writing a New Analyzer

**Do not write a new monolithic `analyze()`.** Analysis is now a
capability-gated **stage pipeline** over a shared typed context: every block —
IMU fusion, segmentation, the pose pass, ball/shaft tracking, metrics, scoring —
is one `AnalysisStage` with a `canRun` gate, and device presence is *data, not
control flow*. A new session type is a new **stage profile**, not a new
hand-rolled chain.

That mechanism, the current stage list, the ordering invariants, and the recipe
for adding a stage all live in
**`docs/developer/analysis_pipeline_developer_guide.md`** — read it before
writing anything. The skeleton is:

```cpp
// ── 1. The analyzer is thin: build a context, run a profile, project a result.
ShotAnalysisResult GrfAnalyzer::analyze(const pinpoint::SwingWindow &window,
                                        const ShotAnalysisJob &job)
{
    AnalysisContext ctx{ CaptureCapabilities::fromJob(job), job, &window };
    ctx.detail = std::make_shared<SwingAnalysis>();
    ctx.wall.start();
    runStages(grfProfile(), ctx);     // authored order, canRun gates, halt short-circuit
    return projectResult(ctx);        // flatten context → ShotAnalysisResult
}

// ── 2. Register in the factory (shot_analyzer.cpp).
if (sessionType == 2) return std::make_unique<GrfAnalyzer>();

// ── 3. Root CMakeLists.txt: add the sources to target_sources.
//      If your session type binds IMUs, extend segmentRoleForSlot()
//      (shot_processor.cpp) so placement slots map to SegmentRoles.

// ── 4. Fill your new job fields in BOTH builders — ShotProcessor::buildAnalysisJob()
//      (live) and SwingDiskLoader (re-analysis). A field filled in only one of
//      them makes re-analysis silently disagree with live capture.

// ── 5. Tests: a stage test per stage, plus scorer goldens for any new band
//      table. See the pipeline guide §8.
```

The contract in one sentence, unchanged and still load-bearing: **read only
`window` (const) and `job` (values), return in bounded time, and degrade to
`ok=false` instead of throwing** — the join, persistence, carousel, and replay
all behave correctly around any outcome you return.

---

## 6. Wrist-Chain Facts Worth Knowing Here

The Wrist profile's 18 stages are documented in the pipeline guide. What follows
is the handful of facts an *orchestration* reader keeps tripping over.

The shape of the chain, in one glance:

```
IMU fusion        200 Hz TimeGrid over common coverage; q_anat = A·q_raw·M per
                  segment; hold-last for momentary gaps; empty grid ⇒ degrade
      ▼
Segmentation      Impact = the marker anchor (clamped to grid). Address = settle
                  just before sustained lead-hand motion onset. Top = lead-hand
                  orientation FURTHEST from Address in (addr, impact].
      ▼
Pose / ball /     The offline ViTPose pass and the camera-derived tracking
shaft tracking    stages — the wall-clock bulk of an analysis (§11).
      ▼
Metrics           Lead-arm joint angles from RELATIVE quaternions between
                  adjacent segments, Address-referenced, via the swing-twist
                  decomposition in wrist_angles.h.
      ▼
Scoring           per-metric sub-scores vs reference bands → weighted geometric mean
```

Two hardware-locked facts to respect (full story in `wrist_angles.h` and
`docs/design/imu_frame_contract.md` §4–5):

- **Joint DOFs are read on the relative-rotation axes, not the segment-axis
  names**: in the forearm→hand decomposition, flexion/extension is about **Z**
  and radial/ulnar about **X** — deliberately the opposite of the segment
  +X=flexion naming. An earlier "flexion about X" form was wrong; do not
  "restore" it.
- **Relative quaternions cancel heading drift**: the shared (drifting) 6-axis
  yaw appears in both segments' `q_anat` and drops out of `qFore⁻¹·qHand`, so
  no per-shot re-zero is needed. The residual ~10–15° FE↔RUD cross-talk is the
  unobservable *relative* heading between two sensors — a known limitation,
  not a bug to fix in the extractor.

`job.handedness` (1 right / 2 left / 0 unknown) selects lead-arm sign
mirroring; the right-lead (left-handed golfer) case is not yet hardware-
verified.

---

## 7. The Scoring Model

**What the score estimates (estimand).** Scores are **per session type and
never aggregated** (a wrist score and a swing score are different
measurements). Every score is **criterion-referenced** — compared to defined
references, **not** a between-golfer percentile and **not** a predicted
ball-flight outcome — and carries an **uncertainty interval** (band σ is
coaching tolerance; sensor/timing error is propagated separately, and low
confidence widens the interval, never raises the score). Two flavours:
**Swing / GRF** have a defined-good reference, so their score is *adherence*
(closeness to an efficient, well-sequenced action); the **Wrist** score has no
defined-good — bowed and cupped both work — so it is a **per-archetype
resemblance diagnostic** that surfaces the closest pattern + strength
("bowed · 86"). The wrist resemblance is the `WristAssessmentEngine` archetype
machinery; its **faults are coach-layer feedback**, and the impact-only
`SwingScorer` below (with its cupping penalty) is **superseded for Wrist**.
Canonical definition: `shot_analyzer_design.md` §B.0.

`SwingScorer` (design: `shot_analyzer_design.md` §B) is deliberately
transparent and **non-compensatory**:

1. Each metric in the session's band table is read **at its scoring phase**
   (Impact for all current wrist bands) from its `PhaseSample`s.
2. `z = (value − mu) / sigma`, with one-sided bands clamping the *good*
   direction to no-penalty (e.g. extra lead-wrist bow is never penalised;
   cupping is).
3. **Deadband + bounded falloff**: |z| ≤ 1 → 100 (green); ramps down to ~1 at
   |z| = 3 (yellow in between, red beyond). No cliff edges, no negative scores.
4. Sub-scores aggregate by a **weighted geometric mean** into per-region,
   per-phase, and overall scores. Geometric, so it is weakest-link: one severe
   fault cannot be averaged away by three good metrics — the coaching premise.
5. Faults are ranked by `pointsLost = weight × (100 − subScore)`; the full
   `ScoredMetric` audit trail ships in the `ScoreBreakdown`.

The wrist band table (`kWristBands`, swing_scorer.cpp) is **provisional**
pending the sign-lock session; flex/ext carries the highest weight (0.45)
because it drives clubhead speed most (Sweeney), radial/ulnar the lowest
(0.15) because it is the weakest IMU axis. Bands are a versioned table per
session type — adding a session type means adding a `bandsFor()` entry, not
touching the math.

---

## 8. Output Shapes — From Worker to QML

Three views of one result, produced at the join:

| Consumer | Shape | Producer |
|---|---|---|
| Carousel chips | `metrics`: key → `{label, value}` (display string at Impact, e.g. via `wristMetricLabel`) | analyzer (flat tier) |
| Carousel sparkline | `tracePoints`: ~24 normalised `QPointF` (Wrist: lead-wrist FE from Address→Impact, y-up = more flexion) | analyzer (flat tier) |
| Replay graph + review | `analysisDetail`: `{tier, overall, series[], phases[]}` `QVariantMap` | `toAnalysisDetail(*detail)` (shot_processor.cpp) |

`toAnalysisDetail` flattens the `SwingAnalysis` for QML: each series becomes
`{key, label, unit, t_us[], value[], phaseSamples[]}` with **absolute µs**
timestamps — the same domain as `shotProcessor.replayPositionUs`, so the
replay graph scrubs with zero conversion. The identical map shape is stored as
the `ShotListModel` `analysisDetail` role and reconstructed from `swing.json`
on reload — live and persisted shots are indistinguishable to the UI.

---

## 9. Persistence — the Unified swing.json

One document per shot — **raw capture manifest and derived analysis in one
`swing.json`**, no separate analysis file:

- **Writer** (`SwingDocWriter::writeSwingJson`): composes the exporter's
  returned manifest tree with the analyzer's additive `"analysis"` object,
  stamps schema `pinpoint.swing/2`, writes atomically. Called exactly once,
  on the GUI thread, at the join. `analysis == nullptr` → raw-only document.
- **Degraded path**: export failed/skipped but analysis succeeded →
  `buildSynthManifest()` synthesises the header (athlete/session/clock/window,
  empty streams) from the cached export job + live window, so the shot
  reloads as analysis-only (`hasVideo=false`).
- **Review write-through** (`SwingDocWriter::updateReview`): the user's star
  rating and note are merged into an additive `"review"` block via atomic
  rewrite (`QSaveFile`), called from the shot model's setters. A shot whose
  swing.json was never written fails this harmlessly.
- **Reader** (`SwingDocReader::readSwingJson` → `PersistedShot`): rebuilds the
  exact `addShot` shapes — flat metrics, trace, score, `analysisDetail` —
  from disk for app-restart reload and the session-review drawer.

`savedSwingDir` is set only when a swing.json was actually written, so a
carousel row only ever links to a real file; an unwritten shot stays
in-memory-only by design.

---

## 9a. Offline Re-analysis

The same analyzers run over a swing **reconstructed from disk**, with no live
buffer anywhere in the picture. This is what makes corpus capture (§4) useful:
capture fast and analyse later, and re-analyse the whole corpus whenever the
pipeline changes.

### `SwingDiskLoader` — swing.json → SwingWindow

`SwingDiskLoader::load(swingDir)` returns a `LoadedSwing`: a **disk-backed**
`SwingWindow` plus a `ShotAnalysisJob` resolved entirely from `swing.json`
(session type, face-on-first camera sources, IMU sources, serial-matched A/M
bindings, impact, handedness, club record, quality tier, ball baseline).

It **streams**. A live window is multi-GB at high frame rates, so rebuilding a
whole `EventBuffer` in RAM is not an option: frames are read one at a time into a
single reusable buffer per camera (raw sidecar offset reads where available, else
`cv::VideoCapture`), while IMU samples and the frame index — both tiny — live in
RAM. That is exactly the contract `SwingPayloadSource` defines (see the event
buffer guide §9): the bytes for a source need only stay valid until the next read
of that same source, and analysis stages read frames strictly one at a time.
Performance is traded for bounded memory, deliberately — re-analysis is a rare
offline operation.

The `ReanalyzeOptions` knobs: `tuningOverrides` and `poseTrackPath` (SwingLab),
`sessionTypeOverride`, and `fullWindow` — which disables the swing-span bound on
the heavy stages so the whole captured window is scanned. The in-app path sets
`fullWindow` on every explicit re-analyse; SwingLab leaves it false so sweeps stay
comparable to production's live bound.

### `ReanalysisController` — the in-app funnel

`reanalysisController` (QML context property) serves both carousel paths: the
focused shot's action bar, and "re-analyse all shown". It is **model-agnostic** —
callers pass already-resolved swing *dirs* (the carousel resolves them from
whichever model is active: the live shot model, or a loaded session's review
model), and it emits `reanalysed(swingDir)` per success so the caller refreshes
the right row. Resolving ids against the wrong model when a past session is under
review is the trap that shape avoids.

Two concurrency rules, both about ViTPose:

- **One swing at a time.** Each re-analysis runs the pose pass at physical-core
  thread count; running two would double CPU and risk OOM. The queue drains
  sequentially.
- **It yields to live capture.** `setLiveBusy(true)` (wired from
  `ShotProcessor::busyChanged`) holds the queue *between* swings, so a re-analysis
  never starts a second ViTPose alongside a live one. An in-flight swing finishes
  — only the next is deferred.

Beyond that it is independent of `ShotProcessor`: its window is disk-backed and it
never touches the live ring, so it may run alongside live capture. Fresh analysis
is written back into `swing.json`, preserving the `capture` / `streams` / `review`
blocks.

`reanalyzeSwingDir(dir, opts)` is the convenience wrapper — load, run
`makeShotAnalyzer(...)->analyze()`, and wrap the call so a thrown analyzer
degrades to `{ok=false, error}` rather than propagating. The `swinglab_run` tool
uses the same loader, so both paths exercise one tested reconstruction.

---

## 10. Threading and Lifetime Rules

The pipeline's safety reduces to four rules:

1. **Jobs are values, resolved on the UI thread.** Nothing reachable from the
   worker may touch `AppSettings`, controllers, `ImuInstance` (its A/M
   calibration is why `ImuSegmentBinding` exists), `DeviceEnumerator`, or any
   QObject. If a new analyzer needs a new piece of live state, add a field to
   `ShotAnalysisJob` and fill it in `startAnalysis()`.
2. **The window is frozen and shared read-only.** While the buffer is Paused,
   producers cannot write (`slot.valid == false`) and the merger is quiesced —
   so the analyzer and exporter both reading the same rings is safe *because
   both are const*. (They are sequenced rather than overlapped today, but the
   safety rests on const-ness, not on the ordering.) Never mutate anything
   reachable from the window.
3. **The window outlives both workers, and only just.** The
   `std::optional<SwingWindow>` storage is stable; the window is destroyed
   only in `finishShot()` (after replay) or `finishNowBlocking()` (which
   **blocks** on both futures first). The processor is declared **after**
   `cameraManager` in main.cpp so it is destroyed first — workers join before
   sources deregister and ring memory frees. This is the same producer/
   stop-barrier discipline as the EventBuffer contract, extended to readers.
4. **One writer per file.** The exporter writes media only and *returns* its
   manifest; the analysis returns values; the single `swing.json` write
   happens at the join on the GUI thread. Workers must not write into
   `swingDir` themselves (the exporter's media files are its own namespace).

Also inherited from hard-won experience: the thumbnail is saved via
`QImage::save()`, **never `cv::imwrite`** — OpenCV's imgcodecs can resolve
libjpeg symbols against the wrong libjpeg generation in this multi-FFmpeg
process and SIGSEGV through libjpeg's error-handler longjmp (observed).

---

## 11. Internals — Design Decisions Explained

### Why analysis and export run sequentially rather than concurrently

They *used* to run concurrently — both are pure readers of the same frozen
memory, and overlapping the analyzer with the FFmpeg encode looked like it made
analysis "free". On the **offline analysis path that wall-clock assumption is
backwards**: the analyzer's heavy stage is the per-frame ViTPose pose pass
(`PoseRunner`), and on a CPU-only host (no GPU EP) it dominates — a 65-frame
pass is ~100× longer than the few-second x264 encode. ViTPose is also strongly
multi-threaded-friendly (measured ~337 ms/frame at 1 intra-op thread vs ~83 ms
at the physical-core count); running it *concurrently* with the multi-threaded
encoder starved its threads and inflated per-frame inference roughly **5×**.

So the post-shot pipeline now **sequences** them: analysis runs alone with the
cores to itself (and `PoseEstimatorViTPose::load()` sizes its intra-op pool to
the physical-core count, no longer pinned to 1), then `onAnalysisFinished()`
launches the export. Total wall-clock dropped from ~125 s to ~10 s for a typical
shot. The join is unchanged — still two `QFutureWatcher`s and an outcome-pair
check; the export simply starts later. Rule 4 above (neither worker writes the
shared file) remains the load-bearing invariant, independent of the ordering.

### Why Impact comes from the marker, not the segmenter

The shot detector already spent three modalities pinpointing the impact
instant, and wrote it into the ring as `shot_marker_v1` *on the same
timeline as every sample*. Re-deriving impact from IMU curves inside the
analyzer would be strictly worse (±5 ms sampling at best) and could disagree
with the replay alignment. The segmenter's job is only the phases that have
no marker: Address and Top.

### Why Top is "orientation furthest from Address"

Angular-velocity valley hunting (the classic approach) is fragile on real
swings — short pauses, double-pumps and noise create false valleys. The
backswing apex *defined as* maximum orientation distance from Address is
parameter-free, monotone-robust, and self-confidence-rating (the distance
itself, clamped, is the confidence).

### Why a fixed 200 Hz TimeGrid instead of native sample times

Joint angles need *pairs* of segments at the *same* instant; native timelines
never align. One grid makes every downstream consumer (extractor, scorer,
replay graph, QML) index-parallel, and 200 Hz matches the sensors' top rate so
nothing is invented. Slerp via `interpolateImu` is exact for orientation;
hold-last covers momentary BLE gaps without poisoning the grid.

### Why the geometric mean

An arithmetic mean lets a 100/100/100/10 swing score 78 — "pretty good" with a
catastrophic fault. The weighted geometric mean scores it ~46: faults are
things to fix, not average away. This is a coaching product decision encoded
in math; see shot_analyzer_design.md §B for the derivation.

### Why `detail` is a `shared_ptr`

The same `SwingAnalysis` is referenced by the result (worker → watcher), the
join (swing.json write), the model row (`analysisDetail` role) and the replay
binding — all on the GUI thread after the join, but with different lifetimes.
A `shared_ptr` in a registered metatype crosses the future boundary cheaply
and removes any copy/ownership question.

---

## 12. Testing

The standalone suite (own `main()`, CHECK macros, not in the root build):

```bash
# Standalone (Qt prefix auto-resolved — see testing_developer_guide.md)
cmake -S src/Analysis/tests -B build/analysis-tests
cmake --build build/analysis-tests -j6
ctest --test-dir build/analysis-tests --output-on-failure

# Or under the umbrella, with every other suite
cmake -S tests -B build/tests && cmake --build build/tests -j6
ctest --test-dir build/tests --output-on-failure
```

| Test | Covers |
|---|---|
| `wrist_angles_test` | swing-twist decomposition: axial isolation, magnitude, the 180° singularity, hardware-locked axis assignments |
| `imu_calibration_test` | the A·q·M anatomical solve + end-to-end keystone golden |
| `live_wrist_angles_test` | the live wrist-angle math contract (mirrors `live_wrist_angles.cpp`) |
| `pipeline_test` | **PhaseSegmenter + MetricExtractor over a synthetic swing** — the analyzer-chain test to mirror for new session types |
| `swing_scorer_test` | banded weighted-geometric-mean scorer goldens |
| `imu_sample_test` | the stored IMU sample frame (`makeImuSample`) the fuser reads back |
| `swing_doc_test` | unified swing.json writer/reader round-trip |
| `orientation_filter_test`, `imu_driver_frame_test` | upstream provisioning: fusion filters and the real driver parse path |
| `analysis_stage_test` | the stage mechanism itself: `canRun` gating, authored order, halt short-circuit |
| `swing_window_parity_test` | a ring-backed and a disk-backed `SwingWindow` present the same data — the guarantee re-analysis (§9a) rests on |
| `tuned_constants_parity_test`, `tuning_overrides_test` | the SwingLab override plumbing agrees with the compiled-in constants |
| `session_summary_test`, `viz_frame_test`, `arbiter_test` | neighbours sharing the suite (review drawer, viz math, shot-detection arbiter) |

The suite is large — 80+ targets and growing, because every stage carries its
own test. `ctest -R <pattern>` is the way to work in it. The per-stage tests are
catalogued in the pipeline guide.

What is deliberately *not* unit-tested here: `ShotProcessor` orchestration
(threading, joins, lifecycle). That is exercised headlessly —
`QT_QPA_PLATFORM=offscreen`, trigger a shot, assert the swing.json and
carousel row — and was the migration target of the window-lifetime guards
(`finishNowBlocking`, `swingWindowLive`). Treat changes there with
correspondingly more care.

---

## 13. Common Mistakes

### Reading live objects from the worker

The compiler will not stop you capturing `m_appSettings` or an `ImuInstance*`
in the analyze lambda — the data race at runtime will. Everything crosses the
boundary inside `ShotAnalysisJob` (values) or the const window. If the job
lacks something you need, extend the job.

### Adding a job field and filling it in only one place

`ShotAnalysisJob` has **two** fillers: `ShotProcessor::buildAnalysisJob()` reads
`AppSettings` on the live path, and `SwingDiskLoader` reads `swing.json` on the
re-analysis path. Fill only the first and re-analysis silently disagrees with
live capture on that field — the same swing scores differently depending on how
it was analysed, with nothing failing to point at the cause. (Note the
direction of the rule: re-analysis must *never* read `AppSettings`. That is what
makes it deterministic; a setting changed since capture must not alter a past
swing's numbers.)

### Calling `job.progress` without checking it

It is a `std::function` that may be null, and it is called **from the worker
thread**. Check before calling, and never touch anything UI-owned from inside
it — the `ShotProcessor` installer posts a queued invoke for exactly that
reason.

### Writing files from an analyzer

The join owns persistence. An analyzer that writes into `swingDir` collides with
the exporter (which writes the same directory namespace afterward) and breaks the
one-writer-per-file invariant that makes the unified swing.json safe. Return data;
let `maybeJoin` write it.

### Throwing (or crashing) instead of degrading

A window with no IMU data, one sample, or no hand sensor is a *normal Tuesday*
— the user forgot to strap a sensor. `ok=false` + `error` is the contract;
the shot still lands on the carousel. `QtConcurrent::run` will happily
propagate an exception into `result()` and take the app down with it.

### Inventing a second impact estimate

`job.impactUs` is the marker anchor — the single source of truth that the
replay, the export window and the metric phases all share. An analyzer that
"refines" impact privately will visibly desynchronise the replay graph from
the video.

### Relative-µs timestamps in MetricSeries

`t_us` is absolute (`nowMicros()` domain). The replay playhead, the phase
events and the swing.json clock block all assume it. If you want
window-relative time for a UI, convert in QML against `replayStartUs`.

### Re-deriving anatomical frames in an analyzer

`q_anat = A·q_raw·M` has exactly one implementation
(`imu_calibration::toAnatomical`, used by the fuser) and the axis/sign
assignments are hardware-locked. Analyzers consume `FusedStreams`; they do not
touch raw quaternions unless they are doing something genuinely new — and then
the new math belongs in a tested header, not inline.

### Skipping the factory

Constructing a concrete analyzer directly bypasses `makeShotAnalyzer`'s
guarantee (never nullptr, correct fallback for unknown/-1 session types) and
the single registration point the processor relies on.

### Blocking the join on replay state (or vice versa)

The shot is on the carousel and saved *before* the replay runs — replay is a
pure presentation tail. `cancelReplay()` (ESC) is just the normal
end-of-replay path taken early; do not attach completion semantics to it.

---

## 14. File Map

Only the orchestration-facing files are listed. The stage headers (there are
~60 in `src/Analysis/` now) are catalogued in the pipeline guide's file map.

```
src/Analysis/
├── shot_analyzer.h             ShotAnalyzer interface, ShotAnalysisJob/Result
├── shot_analyzer.cpp           makeShotAnalyzer factory: WristAnalyzer (type 1),
│                                 CameraKinematicsAnalyzer (everything else)
├── swing_analysis.h            All value shapes: SegmentRole, ImuSegmentBinding,
│                                 Phase(+Event/Sample), MetricSeries, ScoredMetric,
│                                 Fault, ScoreBreakdown, SwingAnalysis
├── analysis_stage.h            The stage mechanism — AnalysisStage, AnalysisContext,
│                                 CaptureCapabilities, runStages (pipeline guide)
├── wrist_analyzer.{h,cpp}      The Wrist profile: the stage list + projectResult
├── swing_reanalyzer.{h,cpp}    SwingDiskLoader / SwingDiskSource / reanalyzeSwingDir —
│                                 the offline disk-backed path (§9a)
├── swing_scorer.{h,cpp}        Band tables + weighted-geometric-mean scoring
├── score_uncertainty.h         Uncertainty-interval propagation (§7)
├── wrist_angles.h              Swing-twist decomposition (header-only, hardware-locked)
├── analysis_tuning.h           SwingLab override plumbing onto the config structs
└── tests/                      80+ targets; per-stage tests + the goldens

src/Gui/shot/
├── shot_processor.{h,cpp}      Pipeline orchestration: post-roll, segmentation
│                                 pre-stage, window, jobs, join, unified write,
│                                 replay, stop-barriers
├── reanalysis_controller.{h,cpp}  The in-app re-analyse funnel (§9a)
├── shot_list_model.*           Carousel rows: addShot (live) / addPersistedShot (reload)
└── shot_replay_controller.*    The ¼× replay tail

src/Export/
├── swing_exporter.{h,cpp}      The media-export worker, run after analysis (separate guide)
├── swing_doc.{h,cpp}           SwingDocWriter/Reader — the unified swing.json
└── swing_paths.{h,cpp}         Session/swing directory allocation
```

---

*Inside `analyze()`: `docs/developer/analysis_pipeline_developer_guide.md`.
Design rationale and the metric/scoring evidence base:
`docs/design/shot_analyzer_design.md` (architecture + scoring model),
`docs/design/analysis_pipeline_fusion_architecture_proposal.md` (the stage
architecture), `docs/implementation/shot_analyzer_m1_wrist.md` (the M1 wrist
chain), `docs/design/shot_analyzer_viz.md` (replay graph),
`docs/reference/wristmetrics.md` (bands),
`docs/design/imu_frame_contract.md` (frames and joint-DOF axes). Upstream:
`docs/developer/shot_detector_developer_guide.md`; sideways:
`docs/developer/swing_export_developer_guide.md`; underneath:
`docs/developer/event_buffer_developer_guide.md`.*
