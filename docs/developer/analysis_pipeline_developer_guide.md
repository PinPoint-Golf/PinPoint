# Pinpoint Analysis Pipeline — Developer Guide

**Audience**: Developers adding or modifying analysis stages (metrics, tracking passes, fusion proposals)  
**Location**: `src/Analysis/analysis_stage.h` (mechanism), `src/Analysis/wrist_analyzer.cpp` (the Wrist profile + all current stages)  
**Language**: C++17 (analysis value types and math) / C++20 (app integration)  
**Status**: Production. The Wrist profile (24 stages) is the only real profile; the original 17 passed the byte-identical staged-vs-monolith gate over the 61-swing blessed corpus before the monolith was deleted, and EventRefine (stage 11) landed afterwards through its own dark-then-freeze gate (2026-07-18). Swing / GRF profiles land as stage lists when their placement UX arrives.

---

## Contents

1. [What the Analysis Pipeline Is](#1-what-the-analysis-pipeline-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [The Wrist Profile, Stage by Stage](#4-the-wrist-profile-stage-by-stage)
5. [Ordering Invariants — What Must Not Move](#5-ordering-invariants--what-must-not-move)
6. [Adding a New Analysis Stage](#6-adding-a-new-analysis-stage)
7. [Managing Performance](#7-managing-performance)
8. [Testing](#8-testing)
9. [Common Mistakes](#9-common-mistakes)
10. [File Map](#10-file-map)

---

## 1. What the Analysis Pipeline Is

The analysis pipeline is the orchestration *inside* an analyzer's `analyze()` —
a **capability-gated stage pipeline over a shared typed context** (a
constrained blackboard). It is the target architecture of
`docs/design/analysis_pipeline_fusion_architecture_proposal.md` §10, shipped
as-built in `WristAnalyzer`:

```cpp
ShotAnalysisResult WristAnalyzer::analyze(const SwingWindow &window,
                                          const ShotAnalysisJob &job)
{
    AnalysisContext ctx{ CaptureCapabilities::fromJob(job), job, &window };
    ctx.detail = std::make_shared<SwingAnalysis>();
    ctx.wall.start();
    runStages(wristProfile(), ctx);      // authored order, canRun gates, halt short-circuit
    return projectResult(ctx);           // flatten context → ShotAnalysisResult
}
```

Every analysis block — IMU fusion, segmentation, the pose pass, ball/shaft
tracking, scoring — is one `AnalysisStage` with a `canRun` gate. Device
presence is **data, not control flow**: a webcam-only capture skips the IMU
stages, an IMU-only capture skips the camera stages, and there is exactly one
code path for every permutation. Every future fusion proposal (§8 P1–P11)
lands as one more stage with an off-switch, never another branch inside a
monolithic function.

This guide is the *inside* view. The *outside* view — how `analyze()` is
launched, jobs are built, results join with the export, and everything
persists — is `docs/developer/shot_analyzer_developer_guide.md`; its rules
(jobs are values, the window is frozen and const, degrade instead of throw)
all still apply and are not repeated here.

---

## 2. Where It Fits in Pinpoint

```
ShotProcessor::startAnalysis (live shot)            swinglab_run / in-app re-analyse (offline)
        │  QtConcurrent worker                              │  swing_reanalyzer (disk-backed window)
        └───────────────┬────────────────────────────────────┘
                        ▼
        makeShotAnalyzer(sessionType)->analyze(window, job)
                        │
        CaptureCapabilities::fromJob(job)      ← device inventory as data
                        │
        AnalysisContext ctx { caps, job, &window }
                        │
        runStages(wristProfile(), ctx)         ← 24 stages, authored order
                        │     each: canRun? → run(ctx) → trace entry (name, ns, skip reason)
                        ▼
        projectResult(ctx)                     ← metrics map, trace, score, detail
```

Two properties make the pipeline trustworthy enough to refactor against:

- **Determinism.** Over a frozen window with a pinned pose track, two runs are
  byte-identical (`swing_window_parity_test` Test 4 asserts this on every
  build). This is what makes corpus-scale parity gates a plain diff.
- **One path for live and offline.** `swinglab_run` and in-app re-analyse
  execute the same stage list over the same sources (`_pinpoint_offline_sources`
  in the root CMakeLists) — an offline corpus result is evidence about the live
  product, not about a parallel implementation.

---

## 3. Core Concepts

### `CaptureCapabilities` — device presence as data

Resolved once from the job before any stage runs (`fromJob`): which camera
placements exist (`FaceOn` today; `DownTheLine` reserved for stereo/DTL
fusion) and one `BoundImu{role, calibValid, calibAgeSec}` per IMU binding.
Stages gate on it — `caps.hasCamera(CameraPlacement::FaceOn)`,
`caps.hasRoles({SegmentRole::LeadForearm, SegmentRole::LeadHand})` — never on
job internals or live device objects.

Note the deliberate subtlety: a present-but-unfusable binding still *counts*
as a capability. The fuser drops Unknown-role / under-sampled bindings
downstream, so `ImuResampleStage` runs and produces empty streams, and the
follow-on stages gate on the *product* (`ctx.hasImuStreams()`), not the
capability. Capability = "the device was there"; product = "it yielded data".

### `AnalysisContext` — the typed blackboard

All inter-stage state lives in one struct (`analysis_stage.h`). Typed slots,
not a bag of variants — a stage's inputs and outputs are the compiler's
business:

| Slot | Written by | Read by |
|---|---|---|
| `streams` (`FusedStreams`) | ImuResample | everything IMU-derived; `hasImuStreams()` is the product gate |
| `segImu` / `segVision` (optional) | ImuSegmentation / Shaft | pre-adoption readers (WristMetrics, Pose, Shaft) |
| `seg` (resolved) | SegResolve | every post-adoption reader (BindDetail, HeadTrack, FootMetrics, Resemblance) |
| `series` (**local** `MetricSeries`) | WristMetrics (first writer), ShaftLean (append) | scorer / metrics map / trace — the *scored* series |
| `runnerOpt` | Pose | Ball, Shaft (resolved pose/ball runner knobs) |
| `ball` (optional) | Ball | Shaft |
| `detail` (`shared_ptr<SwingAnalysis>`) | many stages, in place | `projectResult`, then swing.json / QML |
| `halted` + `haltError` | RequireProducts | the orchestrator (skips the rest), `projectResult` (ok=false) |
| `wall`, `trace` | orchestrator | telemetry/log |

`window` is a **pointer** so the orchestrator is unit-testable with no window
(`analysis_stage_test` runs stages against `nullptr`); any stage that
dereferences it must gate on a capability that implies a window.

### `AnalysisStage` — the unit of work

```cpp
class AnalysisStage {
public:
    virtual QString name() const = 0;
    virtual bool    canRun(const AnalysisContext &) const { return true; }
    virtual QString skipReason(const AnalysisContext &) const { return QString(); }
    virtual void    run(AnalysisContext &) = 0;
};
```

Stages own **no state** — everything lives in the context, so a profile is
reusable and *order is the only contract*. `canRun` must be cheap and pure
(it may run twice: once as the gate, once re-deriving config inside `run`).

### `SessionProfile` + `runStages` — the deliberately dumb orchestrator

A profile is a named `std::vector<unique_ptr<AnalysisStage>>` in authored
order. The orchestrator is a loop you can read in one screen: halted ⇒ record
skip `"halted"`; `!canRun` ⇒ record the stage's `skipReason`; else time
`run()` into the trace. **Anti-goals are explicit** (§10.6): no dynamic
registration, no dependency sorting, no priorities, no parallel stage
execution (the QtConcurrent job is already the concurrency boundary), no
inter-stage messaging. If a stage list ever seems to need runtime reordering,
that is a design smell to investigate, not a feature to build.

### The optional-absence contract

A stage given none of its optional inputs must produce **byte-identical
output to the pipeline without it**. This is the soak-contract discipline
(snap, head, ball, refuse — all shipped dark) promoted to an architectural
invariant, and it is what makes "land dark, gate, then enable" mechanical:
run the corpus with the stage off, diff, expect zero.

### `StageTraceEntry` vs `AnalysisTimings`

Two telemetry layers — do not conflate them:

- **`ctx.trace`** — one entry per stage (name, ran/skip reason, `elapsedNs`).
  Rich, in-memory only, **never serialized**. Today its consumers are the
  orchestrator tests; wire it into a log line if you need it during
  development.
- **`AnalysisTimings`** (`detail->timings`: `poseMs/ballMs/shaftMs/totalMs`)
  — the four persisted wall times (swing.json `analysis.timings` + runmeta),
  self-reported by every shot so the live latency budget is measured, not
  anecdotal. -1 = not measured. Parity gates strip this block before diffing
  (it is the one legitimately nondeterministic output).

---

## 4. The Wrist Profile, Stage by Stage

`wristProfile()` is file-local in `wrist_analyzer.cpp`, but its **body-metric block is not**: stages
13–14 below and the four that joined them live in `appendBodyMetricStages()`, which BOTH
`wristProfile()` and `cameraKinematicsProfile()` call.

> **Analysis is agnostic of session type.** The body-metric stages used to be listed only in the
> Wrist profile, which put a second, invisible gate in front of conditions each stage already states
> for itself — a face-on camera and a pose track, a bound trunk IMU, a valid shaft track, a
> confident phase ladder. A swing recorded perfectly well under the Swing session produced no head,
> foot, lower-body or tempo metrics, and the metric catalogue then reported "produced in Wrist
> Motion sessions only", which reads to the golfer as a statement about their equipment. A session
> type is a capture INTENT; it is not evidence about what was captured. **Put the gate in
> `canRun()`, never in the choice of profile.**
>
> One consequence to know when adding to the block: `cameraKinematicsProfile()` has **no
> EventRefine**, so these stages read the SegResolve ladder rather than the refined one. Each of
> them already tolerates that (they read `ctx.seg.events` and fall back per phase) — a coarser
> Address is a slightly noisier reference, not a wrong one.

| # | Stage | Gate (`canRun`) | Writes |
|---|-------|-----------------|--------|
| 1 | ImuResample | ≥1 bound IMU | `streams` (200 Hz fused grid; optional `filter.refuse` offline re-fusion) |
| 2 | ImuSegmentation | `hasImuStreams()` | `segImu` |
| 3 | WristMetrics | `segImu` present | `series` (first writer; extractor runs over swing-span-trimmed streams) |
| 4 | Pose | FaceOn camera | `detail->pose2d`, `runnerOpt`, `timings.poseMs` |
| 5 | PoseSmooth | `runnerOpt` && pose frames | `pose2d.smoothed/-Aux/-Synth` (+ dark WB4 grip re-anchor) |
| 6 | Ball | `runnerOpt` && pose frames | `ball`, `timings.ballMs` (injection path wins, else job track, else offline replay) |
| 7 | Shaft | `ball` present | `detail->shaft`, `detail->ball`, `segVision` (only when no IMU seg), `timings.shaftMs` |
| 8 | SegResolve | always | `seg` = segImu, else confident segVision, else default |
| 9 | ShaftLean | `shaft.valid` | appends to **local** `series` |
| 10 | RequireProducts | always | `halted` when `series` empty **and** no pose frames |
| 11 | EventRefine | `refine.enabled` && **no** `segImu` && FaceOn && `shaft.valid` && `seg.conf > 0` | retimed Address/Takeaway `t_us` + conf + `provenance = Club`, `Segmentation.version = 3`, `swingStartUs` clamped ≤ refined Address (never Impact; abstains below `minConf` / beyond `maxShiftS`) |
| 12 | BindDetail | always | `detail->series/phases/segmentation` ← local products |
| 13 | HeadTrack | FaceOn && pose frames | appends to `detail->series` only (unscored) |
| 14 | FootMetrics | FaceOn && pose frames | appends to `detail->series` only (unscored) |
| 14a | LowerBodyMetrics | FaceOn && pose frames | 6 frontal-plane series (unscored) |
| 14b | UpperBodyMetrics | FaceOn && pose frames | 9 upper-body series + `trailWristFlexExt` (unscored) |
| 14c | BodyRotation | pose frames **OR** a bound Pelvis/Thorax stream | pelvis/thorax turn, X-factor + stretch (unscored) |
| 14d | ClubDelivery | `shaft.valid` && samples | shaft-vs-horizontal + attack angle from the MEASURED head; low point from the SYNTHESIZED arc (`shaft.synth`) — the two channels fail independently. All unscored; low point carries σ = 2.0 in |
| 14e | Tempo | confident Address/Top/Impact ladder | tempo backswing + ratio (unscored) |
| 14f | Kinematics | `kinematics.enabled` (ON 2026-07-18), `kinematics.composed` (ON 2026-09-06: clubhead speed composed from grip velocity + fused length × the synth tier's rate, masked past P7) | clubhead/hand speed + lag series from the shaft track and pose (unscored). Shared with `CameraKinematicsAnalyzer`, which is the whole of that profile. |
| 15 | Bindings | always | `detail->bindings` (calibration snapshot per device) |
| 16 | Resemblance | always | `detail->score` + §B.7 interval + tier |
| 17 | Assessment | `runAssessment` && IMU streams && local series | findings; **overrides** headline score, clears interval |
| 18 | PoseAssessment | dark flag && **no** IMU streams && pose frames | the IMU-less assessment fallback |

Reading the table top-down gives the degradation story for free: IMU-only ⇒
stages 4–7, 9, 11, 13–14, 18 skip and the wrist metrics + resemblance/assessment
still produce a scored shot; camera-only ⇒ stages 2–3, 17 skip, `segVision`
is adopted at SegResolve, EventRefine fine-tunes its events from the finished
shaft/ball products, and pose/shaft products carry the shot; neither ⇒
RequireProducts halts and `projectResult` degrades to `ok=false` (the shot
still lands on the carousel, score 0 — see the shot analyzer guide).

Two things the table's row granularity hides:

- **The vision segmentation now carries a Takeaway event.** `phasesToSegmentation`
  emits `{Address, Takeaway, Top, Impact, Finish}` (`shaft.emitTakeaway`, frozen
  ON 2026-07-17) — a camera-only `segVision` ladder is five events, not four.
- **The shaft-side onset machinery is stage internals, not stages.** The
  fidget-proofing that places bs0/Address on camera-only swings — the
  departure-referenced revisit ("no-return") veto, the m3gate bridged-run
  net-displacement gate, and the `addressHoldEndFrame` walk-back with its
  onset-veto floor — all lives INSIDE ShaftTracker (stage 7:
  `shaft_track_assembly.cpp` `segmentPhases`/`decideTrack`,
  `shaft_positions.h`). EventRefine (stage 11) then re-walks the same
  `addressHoldEndFrame` machinery from its refined takeaway; it reuses, never
  re-derives.

---

## 5. Ordering Invariants — What Must Not Move

These are enforced structurally (distinct context slots) but the *order*
itself is only protected by this list and the parity tests. The first five
were carried deliberately through the byte-identical gate (§10.5 as-built
note); 6–7 arrived with EventRefine:

1. **Local `series` vs `detail->series` are different products.** The scorer,
   carousel metrics map, and trace read the **local** `ctx.series` (wrist
   metrics + shaft lean only). Head and foot metrics go to `detail->series`
   *only* — they are unscored replay-graph lanes. Appending an unscored
   series to the local list changes the score; binding detail before
   ShaftLean drops a lane.
2. **The segmentation triple keeps readers honest.** Stages 2–7 read
   `segImu`/`segVision` (pre-adoption); everything after SegResolve reads
   `ctx.seg`. A stage that reads `ctx.seg` before stage 8 reads a
   default-constructed value.
3. **RequireProducts halts via `ctx.halted`**, and the orchestrator
   short-circuits stages 11–18 — the fail case must produce a *null* detail
   projection, not a half-bound one (halted contexts skip EventRefine free).
4. **Uncertainty stays fused into Resemblance** (not a separate stage): the
   §B.7 interval brackets the *resemblance* value and must be computed before
   Assessment may override the headline score and clear it. Splitting these
   reorders writes that the monolith sequenced deliberately.
5. **`projectResult()` is the profile-runner tail, not a stage** — a Project
   stage would need special halted-handling in the orchestrator (it must run
   *especially* when halted), which is exactly the machinery §10.6 forbids.
6. **EventRefine sits after SegResolve and before BindDetail — exactly there.**
   It mutates `ctx.seg` in place, so it must run after the resolve (there is
   no seg to refine earlier) and before BindDetail so the refined ladder binds
   with zero extra plumbing — every downstream `ctx.seg` reader (HeadTrack /
   FootMetrics addressUs, assessment P1, buildTrace, swing.json, timeline)
   picks the refined times up automatically. Only ShaftLean and
   RequireProducts sit between SegResolve and EventRefine, and neither reads
   `seg.events`, so the refine slot is the last safe point. A stage inserted
   between EventRefine and BindDetail that reads `seg.events` would see
   refined times — usually what you want, but say so in its design note.
7. **EventRefine NEVER touches Impact.** Impact is the acoustic-anchored
   marker contract (`ShotController` back-dated true-impact estimate; every
   truth swing is acoustic-anchored) — the refine pass retimes existing
   Address/Takeaway events only, never inserts events, and carries
   `refine.impactResidual` as log-only launch−impact telemetry. Any future
   stage that wants to move Impact is a marker-contract change, not a refine
   tweak.

Also load-bearing: Assessment reads `detail->series`/`detail->phases`
(including head/foot — deliberately, the AI-coach sees everything) while its
*gate* is the local series. Do not "fix" this asymmetry.

---

## 6. Adding a New Analysis Stage

The whole point of the architecture: a new analysis module is **one stage
struct + one profile line**, plus the supporting setup below. Work through
this as a checklist — every item exists because a shipped stage needed it.

**Worked example shipped:** EventRefine (2026-07-18) is the newest stage to
run this exact checklist end-to-end and is the best current model to crib
from: pure engine in `event_refine.{h,cpp}` (`refineEvents` over plain
`ShaftTrack2D`/`BallTrack2D`/`Segmentation` — no window, no context) + a
~30-line file-local `EventRefineStage` glue in `wrist_analyzer.cpp`; dark
`refine.*` keys seeded from `pinpoint::tuned::refine::` with
`EventRefineConfig::fromOverrides`; conf-gated abstention as the safety
posture (apply only above `minConf`, within `maxShiftS`, monotone ladder —
else leave the event untouched, which IS the byte-identical outcome); then
the freeze-edit procedure — evidence table in hand, flip the constants in
`pp_tuned_constants.h` with one-line evidence comments and invert the guard
tests to the dark-out direction.

### 6.1 Decide the shape before writing code

- **Which products do you need?** Those are your `canRun` gate. Gate on
  *products* (`ctx.hasImuStreams()`, `!ctx.detail->pose2d.frames.empty()`)
  when you consume data, on *capabilities* (`caps.hasCamera(...)`,
  `caps.hasRoles({...})`) when you consume the window directly.
- **Which slot do you produce into?** Scored metric → append to the local
  `ctx.series` (and face the scorer-band question now, not later). Unscored
  replay lane → `detail->series` after BindDetail. New product kind (a track,
  a fused estimate) → a **new typed slot** on `AnalysisContext` — adding a
  slot is a deliberate act, that's the design.
- **Where in the order?** After every producer you read, before every
  consumer of what you write. The §10.3 table's Requires/Optionals columns
  are the vocabulary; when in doubt, later is safer (fewer readers below
  you).
- **Coordination or fusion?** Consuming another modality's product as an
  *optional* (efficiency, output unchanged when absent) is coordination;
  requiring both modalities to produce something new is fusion. Fusion
  stages get their own design-doc section and their own gate.

### 6.2 Write the math in its own tested file, the stage as glue

House rule (see HeadTrack/FootMetrics as the model): the algorithm lives in
its own header + cpp in `src/Analysis/` (flat directory — no subfolders;
tests in `src/Analysis/tests/`), pure over plain data, with a config struct
that has a `fromOverrides(const QVariantMap &)` factory. The stage struct in
`wrist_analyzer.cpp` is ~20 lines of glue:

```cpp
// src/Analysis/knee_flex.h  — pure math, own unit test, no window access
struct KneeFlexConfig {
    bool   enabled  = pinpoint::tuned::kneeflex::kEnabled;   // frozen constant, dark
    double minConf  = pinpoint::tuned::kneeflex::kMinConf;
    static KneeFlexConfig fromOverrides(const QVariantMap &ov);  // tuning::apply per field
};
KneeFlexResult trackKneeFlex(const PoseTrack2D &pose, int w, int h,
                             int64_t addressUs, const KneeFlexConfig &cfg);

// wrist_analyzer.cpp — the stage, file-local in the anon namespace
struct KneeFlexStage : AnalysisStage {
    QString name() const override { return QStringLiteral("KneeFlex"); }
    bool canRun(const AnalysisContext &ctx) const override
    {
        return KneeFlexConfig::fromOverrides(ctx.job.tuningOverrides).enabled
            && ctx.caps.hasCamera(CameraPlacement::FaceOn)
            && !ctx.detail->pose2d.frames.empty();
    }
    QString skipReason(const AnalysisContext &) const override
    {
        return QStringLiteral("disabled or no face-on pose");
    }
    void run(AnalysisContext &ctx) override
    {
        const auto cfg = KneeFlexConfig::fromOverrides(ctx.job.tuningOverrides);
        // read ctx.seg (post-adoption!) for addressUs, frame dims from
        // ctx.window->formatOf(...) — the HeadTrackStage pattern verbatim
        for (const MetricSeries &m : buildKneeSeries(...))
            ctx.detail->series.push_back(m);      // unscored ⇒ detail only
    }
};
```

Stage rules, non-negotiable:

- **No stage state.** Members on the stage struct break profile reuse and
  hide dataflow — everything goes through the context.
- **Degrade, never throw.** An empty result is a valid product; `ok=false`
  is reserved for RequireProducts. Missing frame dims, low confidence, a
  malformed stream — produce nothing and move on (HeadTrack silently skips
  when the camera format is unusable; the swing.json block is simply
  omitted).
- **Timestamps are absolute µs** on the shared grid domain — same rule as
  everywhere (shot analyzer guide §3).
- **Quaternions, never Euler** for anything rotational (CLAUDE.md hard rule).

### 6.3 Tuning: land dark behind a frozen constant + dotted key

Every new stage ships **dark** (off by default, byte-identical when off) and
becomes tunable without a rebuild:

1. Add the default to `src/Core/pp_tuned_constants.h` (the frozen-constants
   header — e.g. `pinpoint::tuned::kneeflex::kEnabled = false`).
2. Map dotted override keys onto the config in `fromOverrides` using
   `pinpoint::analysis::tuning::apply(ov, "kneeflex.enabled", c.enabled)`
   (`analysis_tuning.h` — float/double/int/int64/bool overloads). SwingLab
   params files (`--params foo.json`) and the in-app tuning path both feed
   `ShotAnalysisJob::tuningOverrides`; production is an empty map, so the
   frozen constant is the production value.
3. Register the key in `docs/validation/tunable_parameters_reference.md`.
4. Extend `src/Analysis/tests/tuning_overrides_test.cpp` — it asserts each
   dotted key actually reaches its config field.

The enable flag flips to on-by-default in `pp_tuned_constants.h` only after
the corpus gate (6.6) — that's one changed constant, reviewable in isolation.

### 6.4 Register the sources — three CMake touchpoints

There is a single root `CMakeLists.txt` (no subdirectory build files), but a
new `.cpp` must reach **three** compile targets:

1. **The app** — add `src/Analysis/knee_flex.{h,cpp}` to the main
   `target_sources` list (the `src/Analysis/` block around the other
   analysis files, ~line 2170).
2. **The offline stack** — add the `.cpp` to `_pinpoint_offline_sources`
   (the `PINPOINT_BUILD_TOOLS` block). This is what makes swinglab_run,
   in-app re-analyse, and the parity fixture use your stage; forget it and
   the offline build breaks (or silently diverges) at link time.
3. **The unit test** — `src/Analysis/tests/CMakeLists.txt`:
   `pp_add_test(knee_flex_test SOURCES knee_flex_test.cpp ${ANALYSIS}/knee_flex.cpp)`.

Header-only math (like `wrist_angles.h`, `hand_axis.h`) skips 1–2's `.cpp`
entries but still gets its own `pp_add_test`.

### 6.5 Persist the product (only if it needs to survive a restart)

If the stage produces something the replay/review UI reads back, extend the
unified swing.json **additively** in `src/Export/swing_doc.cpp`: writer emits
the block only when the product is non-empty, reader tolerates its absence.
"Absent block ⇒ omitted ⇒ byte-identical to before your change" is the same
optional-absence contract at the serialization layer — and it's what keeps
old swing.json files loading forever. Round-trip it in `swing_doc_test`.

### 6.6 Insert into the profile and run the gates

Add the one line to `wristProfile()` at the position 6.1 chose, then gate in
this order (cheapest first):

1. **Unit test** — the math over synthetic data
   (`ctest --test-dir build/analyzer-tests`).
2. **Orchestrator behaviour** — if your gating is novel, extend
   `analysis_stage_test.cpp` (order, skip reasons, halt short-circuit).
3. **Local determinism** — `swing_window_parity_test` (Test 4 runs
   `analyze()` twice and diffs the serialized results; your stage must not
   introduce run-to-run nondeterminism — no wall-clock-dependent logic, no
   unordered-container iteration into outputs).
4. **OFF-parity on the corpus** — the stage dark must be byte-identical over
   the blessed corpus. Use the stagegate harness (`parity_run.py` two
   passes + `tools/swinglab/parity_diff.py`, timings stripped) with
   **injected pose** so CUDA jitter doesn't drown the diff (§7.4). ⚠ If your
   stage touches segmentation or event times, this gate is blind to it — see
   the §8 caveat and gate through the live-pose A/B + truth harness instead.
5. **ON-evaluation on the corpus** — accuracy/coverage judged at corpus
   scale on the studio PC (Release + CUDA). **A single swing never judges
   model accuracy** — one labeled swing is development data only.

### 6.7 Documentation

Amend the fusion proposal's §10.5 as-built note (or your feature's own
design doc) with what actually shipped and any deliberate deviations. The
as-built note is the contract the next refactor gates against.

---

## 7. Managing Performance

### 7.1 Know where the time goes before touching anything

The pipeline's cost is dominated by one stage, and the ratios are extreme
enough that intuition misleads:

- **Pose (ViTPose per frame) is 70–95 % of wall time.** Everything IMU-side
  (resample, segmentation, wrist metrics, scoring) is milliseconds. The
  corpus numbers make the point: 61 swings ≈ 417 s live-pose vs ≈ 147 s with
  pose injected — the entire rest of the pipeline is ~2.4 s/swing.
- Ball and shaft are second-order (the shaft C++ port runs ~3 s/swing);
  smoothing, head/foot, assessment are noise.

So: measure first (`analysis.timings` per shot; `ctx.trace` per stage during
development), and treat any optimisation that doesn't address the pose pass
— or the decode feeding it — with suspicion. The Python-era shaft profiling
taught the same lesson twice: the "obvious" hot spot (the DP) was <2 % and
the real levers were background sampling and decode.

### 7.2 The instrumentation you already have

| Tool | Granularity | Where |
|---|---|---|
| `AnalysisTimings` | pose/ball/shaft/total ms, every shot | swing.json `analysis.timings`, runmeta — trend it, budget against it |
| `ctx.trace` → profiler | every stage, ns + skip provenance | fed live to the resource profiler by `analysis_profiling.cpp` (`recordAnalysisRun`): aggregate `Analysis.Stage.*` scopes (avg/peak across the session, in the monitor's ANALYSIS STAGES table + the 60 s STATS HISTORY dumps + stats Export) **and** a per-run drill-down in the ANALYSIS RUNS tab (own Export). Also still in-memory on `ctx.trace` for a temporary ppInfo when hunting a regression |
| SwingLab runmeta | per-swing totals across a corpus | `swinglab_run` output dirs |

The profiler feed is pure instrumentation — it only reads `ctx.trace`/`ctx.wall`
and changes no analysis output, so it is byte-identical to omitting it (nothing
new lands in swing.json). Because it reads the trace, a **new stage is profiled
automatically** — the §6 checklist needs no profiler step. See the resource
profiler developer guide §4–§5 for the surfaced views and the honest-CPU note
(per-stage CPU is deliberately not attributed — the pose stage parallelises
inside ORT).

The live-shot budget is **< 20 s** end-to-end on studio hardware (analysis +
export; the swing-span-bounding plan's telemetry exists to hold that line).
A new stage that adds seconds to every shot needs a corpus-measured
justification.

Two levers/examples worth knowing by name:

- **`pose.intraOpThreads`** — the offline ViTPose ORT intra-op pool size
  (`pose_runner.cpp` seam, frozen default in `pp_tuned_constants.h`
  `pose::kIntraOpThreads`). Three-way: `0` (default) = the legacy heuristic
  `clamp(hardware_concurrency()/2, 1, 8)`, kept so the default path stays
  thread-count-identical to history; `-1` = topology auto
  (`physicalCoreCount()` from `src/Core/cpu_topology.h`, clamped [1,16]) —
  opt-in until its determinism A/B on no-SMT/hybrid hardware is done; `> 0`
  = pin exactly that many threads.
- **`ball.clubActivity` as the worked cost-measurement example** — the
  club-corridor activity producer was frozen ON (2026-07-18, the EventRefine
  Tier-B input) with its cost measured the way every enable should be: the
  corpus A/B put the delta at **+~207 ms median on the ball stage**
  (`timings.ballMs`), recorded in the freeze evidence. That is the pattern —
  a flag flip ships with its measured `AnalysisTimings` delta, not a guess.

### 7.3 The three structural levers (already built — reuse, don't reinvent)

1. **Capability/product gating order.** Cheap IMU stages run first and their
   products *bound* the heavy stages. Keep new cheap stages ahead of
   expensive consumers so their outputs can shrink the work.
2. **Swing-span bounding (G3).** The pose scan covers the detected swing
   span + 150 ms pad, not the raw 5 s ring — plus a sparse 4 s address
   reach-back (`shaft.addressScanPadUs` / `shaft.addressStride`) so the
   ball-anchor pass still sees address-hold frames. `job.fullWindow` opts
   out (correctness over speed on explicit re-analysis). A new heavy
   per-frame stage should consume the same bounds from `ctx.segImu`/`ctx.seg`
   — never rescan the full window by default.
3. **Two-pass pose for the camera-only case.** With no IMU span (`segImu`
   conf 0) the pose/span chicken-and-egg is broken by a coarse-stride pass
   then a dense pass around impact (`pose.coarseStride`, `pose.densePreMs/
   -PostMs`, `pose.denseStride`). The same pattern (coarse locate → dense
   refine) is the first thing to reach for in any new per-frame pass.

### 7.4 Injection paths — skip the expensive stage entirely

`ShotAnalysisJob::poseTrackPath` / `ballTrackPath` (and the in-memory
`job.ballTrack`) replace a stage's compute with a loaded track. Two uses:

- **Deterministic gates.** Live-CUDA ViTPose is run-to-run nondeterministic
  at ~1e-9 — enough to fail a byte-identical diff on every swing. Freeze one
  canonical pose pass per swing (`stagegate/extract_pose.py` pulls
  `analysis.pose2d` out of a prior run) and inject it into both sides of the
  diff. Always run an OFF-vs-OFF determinism baseline first so you know the
  residual is the change under test, not the environment.
- **Fast iteration.** A 61-swing corpus drops ~417 s → ~147 s with pose
  injected. When your change is downstream of pose (shaft, metrics,
  scoring), inject and iterate at that speed.

If you add a new expensive stage, add its injection path at the same time —
it will pay for itself at the first parity gate.

### 7.5 Build/host discipline (hard-won, in the memory bank)

- **Corpus runs happen on the studio PC, Release + CUDA** (`ssh studio`,
  `SWINGLAB_BIN` → Release-Installer `swinglab_run.exe`). Never compare
  timings across hosts or configs; MSVC Debug alone is a ~5–6× tax.
- **Sequencing beats overlap on the worker.** The pose pass sizes its ORT
  intra-op pool for the whole machine (`pose.intraOpThreads`, §7.2) and is
  starved badly (~5× measured) by a concurrent encoder — which is why
  ShotProcessor sequences analysis before export. Do not add threads inside
  a stage; the QtConcurrent job is the concurrency boundary and ORT already
  owns the cores during pose.
- **Local dev box limits:** build with `--parallel 4` (15 GiB box OOMs
  above ~4 jobs); one heavy analysis job at a time; if everything is
  mysteriously ~4× slow, check the BD_PROCHOT 800 MHz clamp (MSR `0x1FC`
  bit 0) before profiling anything.

### 7.6 Progress reporting for long stages

`job.progress` is a 0–1 callback budgeted across the heavy stages: pose owns
0.10–0.70, shaft 0.70–0.98. A stage that takes user-visible time rescales
into its own slice with a capturing lambda over `ctx.job` (see PoseStage /
ShaftStage — capture `ctx.job`, a stack-local of `analyze()`, not `ctx`).
If you insert a new heavy stage, carve its slice out of the neighbours and
keep the total monotone — the UI treats regressions in the progress value
as a bug.

---

## 8. Testing

The analyzer suite is standalone (own `main()`, CHECK macros — see
`docs/developer/testing_developer_guide.md` and the analyzer-tests
provisioning notes):

```bash
cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
cmake --build build/analyzer-tests --parallel 4
ctest --test-dir build/analyzer-tests --output-on-failure
```

Pipeline-specific coverage:

| Test | Covers |
|---|---|
| `analysis_stage_test` | the orchestrator: authored order, `canRun` gating + skip reasons, halt short-circuit, one-trace-entry-per-stage — over a windowless context |
| `tuning_overrides_test` | every dotted tuning key reaches its config field |
| `event_refine_test` | the EventRefine engine: three-tier at-ball evidence, last-departure/no-return takeaway, flicker debounce, abstention gates, enabled=false byte-identity, swingStartUs clamp |
| `swing_window_parity_test` (root build, Test 4a/4b) | double-run determinism of the full `analyze()` — IMU-only and camera paths, serialized diff with timings stripped |
| `pipeline_test`, `swing_scorer_test`, per-module tests | the math inside individual stages |

The suite is 52 tests as of the EventRefine landing — run it whole; it is
fast (~7 s).

Corpus-scale gating lives outside ctest: the stagegate harness
(`parity_run.py` + `extract_pose.py` on the studio share,
`tools/swinglab/parity_diff.py` locally) runs the blessed corpus through
`swinglab_run` and diffs serialized results. The discipline that shipped the
refactor — determinism baseline first, pose injected, timings stripped, zero
diffs required — is the template for every future stage gate. SwingLab
itself (`/swinglab`, `docs/developer/swinglab_developer_guide.md`) is the
tool for accuracy/coverage evaluation once parity is established.

**CRITICAL caveat — the pose-injected byte-gate is segmentation-blind.** The
pose-injection path produces **empty segmentation on every corpus swing** (a
harness gap, observed on the stagegate corpora and unfixed): the serialized
event ladder is empty on both sides of any pose-injected diff. A
pose-injected zero-diff therefore proves plumbing inertness for everything
EXCEPT segmentation/event-time changes — it can never catch an event
regression.
Segmentation-affecting work gates through two live-pose instruments instead:

- **Live-pose same-binary metric A/Bs** — run the corpus twice on ONE binary
  (same host, same config), flag OFF vs ON, and compare event times/metrics
  rather than bytes. Run the OFF-vs-OFF pair first: its event-movement noise
  floor is **zero** (CUDA pose jitter does not move event times), so any
  OFF-vs-ON movement is the change under test.
- **`fidget_eval.py` (stagegate share) — the 17-swing truth harness.** Runs
  `swinglab_run` over every swing carrying a `truth.json` `events.p1_s`
  annotation and reports per-swing Address (and Takeaway, when present)
  error vs truth, resumable, with an optional params file for the A/B side.
  This is what produced the EventRefine freeze evidence (median held
  0.052 s, max 0.577 → 0.145 s, within-100ms 12 → 14).

---

## 9. Common Mistakes

### Appending an unscored series to the local `ctx.series`

The scorer and the carousel metrics map read the local list. Your new
diagnostic lane goes to `detail->series` (after BindDetail) unless you have
a validated reference band and *mean* to change the score.

### Reading `ctx.seg` before SegResolve

Stages 1–7 must read `segImu`/`segVision`. `ctx.seg` is default-constructed
(conf 0, no events) until stage 8 — your address lookup will silently return
"no address" on every camera-only swing.

### Gating on a capability when you consume a product

`caps.imus.empty()` being false does **not** mean fused streams exist
(unfusable bindings produce empty streams by design). If you read
`ctx.streams`, gate on `ctx.hasImuStreams()`; if you read pose, gate on
`!ctx.detail->pose2d.frames.empty()`.

### Dereferencing `ctx.window` in `canRun` (or unguarded in `run`)

The orchestrator tests run stages with `window == nullptr`. Gate on a
capability that implies a window and keep `canRun` pure; touch the window
only inside `run()`.

### Keeping state on the stage struct

`wristProfile()` constructs fresh stages per analyze() today, but the
architecture promises stateless stages (profiles are reusable, order is the
only contract). Anything you're tempted to cache belongs in a context slot,
visibly.

### Landing enabled, or "just a small always-on tweak"

Every behaviour change ships dark behind a `pp_tuned_constants.h` flag and
flips on only after the corpus gate. The OFF state must be byte-identical —
that includes swing.json (absent block, not empty block) and iteration
order (no unordered containers into outputs).

### Forgetting the offline sources list

A stage whose `.cpp` is in the app target but not
`_pinpoint_offline_sources` makes swinglab_run/re-analyse diverge from live
— the exact split-brain the shared-path design exists to prevent. All three
CMake touchpoints (§6.4), every time.

### Serializing the stage trace

`ctx.trace` is in-memory telemetry. `AnalysisTimings` keeps its four fields;
adding per-stage timing to swing.json breaks every byte-identical gate and
bloats the doc. Log it, don't persist it.

### "Fixing" a carried deviation

The five §10.5 as-built deviations (in-place detail, projectResult-as-tail,
fused Uncertainty, extra stages, no placeholders) and oddities like
Assessment's local-gate/detail-read asymmetry are load-bearing parity
decisions. Changing them is a real refactor with a corpus gate, not a
cleanup.

---

## 10. File Map

```
src/Analysis/
├── analysis_stage.h            THE mechanism: CaptureCapabilities, AnalysisContext,
│                                 AnalysisStage, SessionProfile, runStages, StageTraceEntry
├── analysis_tuning.h           tuning::apply — dotted-key overrides onto config fields
├── wrist_analyzer.{h,cpp}      The Wrist profile: all 18 stage structs (file-local),
│                                 wristProfile(), projectResult(), analyze()
├── swing_analysis.h            Product shapes incl. AnalysisTimings, SwingAnalysis
├── imu_vision_fuser.*          Stage 1's engine (FusedStreams)
├── phase_segmenter.*           Stage 2's engine
├── metric_extractor.*          Stage 3's engine
├── pose_runner.* / pose_smoother.* / pose_synthesis.h      Stages 4–5
├── ball_runner.* / shaft_tracker.* / shaft_track_assembly.*  Stages 6–7
│                                 (shaft_track_assembly also owns the onset
│                                 machinery: revisit veto, m3gate, onsetFloor)
├── ball_activity.h             Club-corridor activity math (annulus + temporal
│                                 median) — the BallSample2D.clubActivity producer
│                                 helper, unit-tested standalone
├── shaft_positions.h           addressHoldEndFrame + P1–P8 locators (header-only)
│                                 — REUSED by both ShaftTracker and EventRefine
├── ball_anchor.*               v3.4 ball-anchor pass + buildThetaBallSeries, the
│                                 SHARED θ_ball builder (one implementation for
│                                 applyBallAnchor and EventRefine Tier A)
├── event_refine.{h,cpp}        Stage 11's engine (refineEvents — pure over
│                                 ShaftTrack2D/BallTrack2D/Segmentation)
├── head_track.* / foot_metrics.*   Stages 13–14 — the model for new pose-derived stages
├── wrist_resemblance.* / score_uncertainty.*               Stage 16
├── wrist_assessment_engine.* / pose_wrist_angle_source.*   Stages 17–18
└── tests/                      Standalone suite (build/analyzer-tests, 52 tests):
                                  analysis_stage_test, tuning_overrides_test,
                                  event_refine_test, per-module tests

src/Core/pp_tuned_constants.h   Frozen defaults for every dark flag / tunable
src/Core/cpu_topology.h         physicalCoreCount() — the pose.intraOpThreads=-1
                                  topology-auto source
tools/swinglab/parity_diff.py   Corpus result differ (timings-stripped byte compare)
CMakeLists.txt                  Root, single file: app target_sources +
                                  _pinpoint_offline_sources (PINPOINT_BUILD_TOOLS)
```

---

*Architecture and rationale:
`docs/design/analysis_pipeline_fusion_architecture_proposal.md` §10 (the
pattern, the stage inventory, the as-built note) and §§6–8 (the fusion
proposals that will land as stages). Around this pipeline:
`docs/developer/shot_analyzer_developer_guide.md` (job building, join,
persistence, threading). Evaluation tooling:
`docs/developer/swinglab_developer_guide.md` and
`docs/validation/tunable_parameters_reference.md`. Test conventions:
`docs/developer/testing_developer_guide.md`.*
