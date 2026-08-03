# Pinpoint SwingLab — Developer Guide

**Audience**: Developers working on the analysis pipeline or the offline evaluation tooling
**Location**: `tools/swinglab/` (runner + Python lab), built via `-DPINPOINT_BUILD_TOOLS=ON`
**Language**: C++20 (`swinglab_run`) / Python 3 (numpy, opencv, matplotlib)
**Status**: L1–L5 implemented and validated end-to-end on the synthetic corpus (100/100 baseline). The runner's disk→window reconstruction is now shared with the in-app re-analyse path (`SwingDiskLoader`), and `parity_diff.py` gates analyzer refactors byte-identically. Real-data missions await clean corpus v1 — pre-corpus-v1 studio recordings are unreliable (2026-06-11 decision).

---

## Contents

1. [What SwingLab Is](#1-what-swinglab-is)
2. [Where It Fits in Pinpoint](#2-where-it-fits-in-pinpoint)
3. [Core Concepts](#3-core-concepts)
4. [The C++ Runner, Stage by Stage](#4-the-c-runner-stage-by-stage)
5. [The Python Lab — `lab.py` Subcommands](#5-the-python-lab--labpy-subcommands)
6. [The Scorecard](#6-the-scorecard)
7. [Capture Provenance and Legacy Fallbacks](#7-capture-provenance-and-legacy-fallbacks)
8. [The Synthetic Corpus](#8-the-synthetic-corpus)
9. [The Tuning Workflow](#9-the-tuning-workflow)
10. [Multi-Host Operation](#10-multi-host-operation)
11. [Building and Running](#11-building-and-running)
12. [Extending SwingLab](#12-extending-swinglab)
13. [Common Mistakes](#13-common-mistakes)
14. [File Map](#14-file-map)

---

## 1. What SwingLab Is

SwingLab makes recorded swings a **development substrate**. As the analysis
goes deeper (shaft tracking, segmentation, cross-modal fusion), assessing
correctness by a human watching replays does not scale — and it locks an
assistant out of the loop entirely. SwingLab re-runs the **real production
analysis pipeline** on a saved swing dir offline, scores the output against
physics invariants and (optionally) hand-labelled ground truth, and renders
the evidence as plots a model can read.

Three design principles drive everything:

1. **Replay the production code, never a reimplementation.** The core is a
   C++ runner (`swinglab_run`) that reconstructs a `SwingWindow` from a recorded
   swing dir and executes the unmodified production stage pipeline
   (`makeShotAnalyzer`). A Python rewrite of the tracker would drift from the app
   within a week. **The reconstruction is now shared code**, not the runner's own:
   `SwingDiskLoader` (`src/Analysis/swing_reanalyzer.h`) backs both this runner and
   the in-app re-analyse path, so both exercise one tested loader.
2. **Make failure legible.** Every run produces a per-swing
   **scorecard.json** (named checks with values vs thresholds), a
   **contact_sheet.png** (the track overlaid on key frames — how a model
   "watches" a swing), and an aggregate **REPORT.md**.
3. **Parameters change without rebuilds.** All tuning knobs are injectable from a
   params JSON via `ShotAnalysisJob::tuningOverrides`, so a sweep iterates at binary
   speed with zero tokens. Namespaces by config owner:
   - `seg.*` (`SegmentationConfig`), `shaft.*` (`ShaftDetectConfig`),
     `assembly.*` (`AssemblyConfig`) — track/segmentation, always on.
   - `score.*` (`SwingScorer` bands + deadbands) — moves `r.score` directly; e.g.
     `score.leadWristFlexExt.mu`, `score.zOut`.
   - `sampler.*` / `rules.*` / `bands.*` (Tier-2 wrist assessment) — observable only
     when the offline analyzer runs assessment (`ShotAnalysisJob::runAssessment`, set by
     `swinglab_run`); they drive the `analysis.assessment.findings[]` block and the
     score.py `diag.*` known-groups checks. E.g. `rules.confidenceFloor`,
     `sampler.gimbalThresholdDeg`, `bands.flexExtMargin`.
   - `filter.*` (orientation re-fusion gain/schedule) — via the `--refuse-orientation`
     re-fuser; feeds the main wrist metric only once re-fusion-into-analysis is enabled.

   Frozen defaults all live in `src/Core/pp_tuned_constants.h` (`pinpoint::tuned::*`) —
   the single edit-point when validation locks a value (the override path does not consult
   it). Unknown keys are logged and ignored, so a typo silently no-ops — check `runner.log`.

SwingLab is **not** part of the shipping app. The production hooks it relies
on (`tuningOverrides`, `poseTrackPath`, trace out-params, the
`analysis.bindings` snapshot, the capture-provenance blocks) are all additive
and inert in production.

---

## 2. Where It Fits in Pinpoint

```
        THE APP (record time)                      SWINGLAB (any time later)
┌─────────────────────────────────┐    ┌──────────────────────────────────────────┐
│ ShotProcessor                   │    │  swinglab_run <swing_dir> --out <run>    │
│   SwingExporter ──► swing dir   │    │    SwingDiskLoader::load(swingDir)       │
│     <alias>.mp4 / .raw          │───►│      → disk-backed SwingWindow (streams  │
│     imu_<alias>.csv|.bin|inline │    │        one frame at a time) +            │
│     swing.json  (streams +      │    │        ShotAnalysisJob from swing.json   │
│       capture/setup/device +    │    │    apply --params / --pose / --ball      │
│       analysis.bindings)        │    │    makeShotAnalyzer → analyze()  ◄═ THE  │
│     thumb.jpg                   │    │      PRODUCTION PIPELINE, UNMODIFIED     │
└─────────────────────────────────┘    │    write result.json + runmeta.json      │
                                       │      (+ trace.jsonl with --trace)        │
                                       └───────────────────┬──────────────────────┘
                                                           │ files in, files out
                                       ┌───────────────────▼──────────────────────┐
                                       │  lab.py (Python)                          │
                                       │    score  → scorecard.json (Tier 1–3)     │
                                       │    plot   → contact_sheet.png             │
                                       │    run    → per-corpus batch + REPORT.md  │
                                       │    diff   → DIFF.md (regression gate)     │
                                       │    sweep  → mechanical param search       │
                                       │    label  → truth.json (click-UI)         │
                                       │    synth  → ground-truthed fixture        │
                                       ├──────────────────────────────────────────┤
                                       │  parity_diff.py  byte-identical soak gate │
                                       │  montage_positions.py  P1–P8 montages     │
                                       └──────────────────────────────────────────┘
```

The interface between every stage is **files** — no IPC, no shared state.
That is what makes multi-host operation (§10) and model-driven operation
(the `/swinglab` skill) work without machinery.

Related guides: the window being replayed is documented in
`docs/developer/event_buffer_developer_guide.md`; the pipeline being re-run in
`docs/developer/shot_analyzer_developer_guide.md`; the swing-dir artifacts in
`docs/developer/swing_export_developer_guide.md`.

---

## 3. Core Concepts

### Swing dir

The unit of input: one shot's saved folder (see the swing-export guide §5).
SwingLab needs `swing.json` + at least one video stream; it prefers the
`.raw` sidecar (bit-faithful — the exact bytes the live analyzer saw) and
falls back to decoding the MP4 to BGR24 (re-encoded pixels ≈ source;
scorecards carry a `frames: raw|mp4` flag so tuning conclusions can be
restricted to raw swings).

Two files are SwingLab-private and live *inside* the swing dir:
`pose.json` (an injected `PoseTrack2D` — used by the synthetic corpus, where
there is no human for ViTPose to find, and as a pose cache during shaft
tuning) and `truth.json` (hand labels from `lab.py label`). **Nothing else
may ever be written into a swing dir.**

### Corpus

A directory tree of swing dirs **outside the repo** (convention:
`/mnt/swingdata/corpus-v1` ≡ Windows `C:\Users\developer\Data\PinPointStudio\corpus-v1`).
`lab.py ingest` scans it recursively for `swing.json` files and writes
`corpus.json` — one entry per swing with quick facts (stream counts, raw
availability, impact present, binding count, truth present) plus the capture
provenance fields (§7).

**Blessing**: a corpus root must contain a `CORPUS.md` stating recording date
and calibration provenance; only then does `ingest` mark the manifest
`"blessed": true`. Unblessed real data must not drive tuning conclusions.

### Run dir

One invocation's output folder. A batch run is
`<runs_root>/<run_id>/<swing_name>/` containing `result.json`,
`runmeta.json`, `runner.log`, `trace.jsonl` (unless `--no-trace`),
`scorecard.json`, and `contact_sheet.png`; the run root gets `summary.json` +
`REPORT.md`. Runs also live on the shared SwingData drive so every host can
read the evidence.

### Scorecard tiers

- **Tier 1 — physics invariants** (no labels needed; the soak backbone):
  the "expected tracking shape" as named checks with margins.
- **Tier 2 — cross-modal consistency**: vision↔IMU θ̇ correlation,
  segmentation sanity.
- **Tier 3 — truth metrics**: only where `truth.json` exists.

### Attribution

Every scorecard and run summary records the git SHA (`git_sha()`), the params
file, and (via runmeta) host + platform — "regression in swing_0007" always
answers *what changed*. Cross-host runs are attributable but **not
comparable**: CPU and CUDA pose outputs differ subtly, so the diff gate
compares same-host runs only.

### The model contract

Operator/engineer tiering (which model may do what, when to escalate) is
encoded in the `/swinglab` skill — `.claude/skills/swinglab/SKILL.md`,
**local-only and gitignored** (Claude artefacts stay out of the repo; copy it
to a new operator host once, e.g. via the SwingData share).

---

## 4. The C++ Runner, Stage by Stage

`tools/swinglab/src/swinglab_run.cpp` — a single-file CLI built inside the
app build (it needs the same OpenCV/ORT/whisper machinery the app
configures).

```
swinglab_run <swing_dir> --out <run_dir> [--params p.json] [--trace]
             [--session-type N] [--face-on Str] [--impact-us N]
             [--pose f.json] [--ball f.json]
             [--refuse-orientation] [--refuse-beta X]
```

### Stage 1–2 — reconstruct, via `SwingDiskLoader`

**This used to be the runner's own code and is now shared.** One call —
`SwingDiskLoader::load(swingDir, opts)` (`src/Analysis/swing_reanalyzer.h`) —
returns a `LoadedSwing`: a disk-backed `SwingWindow` plus a `ShotAnalysisJob`
resolved from `swing.json`. The in-app re-analyse path
(`ReanalysisController`) calls the same loader, so a reconstruction bug cannot
exist in one and not the other.

The important behavioural change: it **streams**. The old runner rebuilt a full
`EventBuffer` and wrote every payload into it at its original timestamp, which
holds the whole window in RAM — multi-GB at high frame rates. The loader instead
reads one frame at a time into a single reusable buffer per camera (raw sidecar
offset reads where present, else `cv::VideoCapture`), keeping only the IMU
samples and the frame index resident. That is the `SwingPayloadSource` contract
(event buffer guide §9), and it is why re-analysing a corpus no longer needs a
workstation's worth of memory. Frames still come from the **raw sidecars when
present** (bit-faithful — the exact bytes the live analyzer saw), MP4 otherwise;
`LoadedSwing::usedRaw` reports which, and the scorecard's `frames: raw|mp4` flag
follows from it.

**Face-on selection**: `setup.perspective == 2` when the stream carries `setup`
and `--face-on` was not explicitly passed; otherwise the legacy alias-substring
match (default needle `"Face"`). An explicit `--face-on` always wins — the escape
hatch for mislabelled recordings.

### Stage 3 — the job, and what the runner adds to it

The loader fills the `ShotAnalysisJob` from the manifest, mirroring what
`ShotProcessor::buildAnalysisJob()` does from live state — but reading
**`swing.json`, never `AppSettings`**. That is the whole determinism argument: a
setting changed since capture must not alter a past swing's numbers.

| Field | Source | Override |
|---|---|---|
| `sessionType` | `capture.sessionType` when present | `--session-type` (also the legacy default, 1 = Wrist) |
| `impactUs` | the recorded Impact phase, else `capture.impactUs` (which is what an analysis-skipped corpus swing has) | `--impact-us` |
| `handedness` | `athlete.handedness` | — |
| `cameraSources` | face-on first (`faceOnCameraCount` set) | `--face-on` |
| `imuBindings` | `analysis.bindings[]` serial-matched to IMU streams — the exact A/M the app used, plus calibration status (§7). Falls back to each stream's `device` calibration block when `analysis.bindings` is absent (the corpus-capture case). | — |
| club geometry, length prior, quality tier, ball ROI/baseline | the recorded `capture.*` / `setup.*` blocks | — |
| `tuningOverrides` | `--params` JSON, flattened to dotted keys (`shaft.ridgeKernelPx`) | — |
| `poseTrackPath` | `--pose` (skip ViTPose, load a `PoseTrack2D` JSON) | — |
| `ballTrackPath` | `--ball` (skip the offline ball replay, load a ground-truth `BallTrack2D`) | — |

Bindings are **never fabricated**: if a swing has neither `analysis.bindings` nor
a stream-level calibration block, the runner does not synthesize identity A/M —
re-fusing without the session calibration would be fiction. No impact instant at
all is a hard error (`--impact-us` is the manual rescue).

`--refuse-orientation` re-fuses the recorded raw IMU through the orientation
filter before analysis (with `--refuse-beta` setting the gain), which is how the
`filter.*` namespace is exercised.

### Stage 4 — run the production pipeline

`makeShotAnalyzer(job.sessionType)->analyze(window, job)` — the same factory
and analyzer the app's worker thread runs. Wall-clock for build and analyze
phases is recorded.

### Stage 5 — outputs

- **`result.json`** — the re-run `SwingAnalysis` serialized by the production
  `SwingDocWriter::writeSwingJson()` (then renamed from `swing.json`), under
  a `pinpoint.swinglab/1` manifest that records the source swing dir and
  frame source (`raw|mp4`). Identical shape to the app's `analysis` block, so
  every downstream consumer (score, plots) reads one format.
- **`runmeta.json`** — provenance: ok/error/score, build/analyze wall ms,
  params echo, impact, binding count, host + platform, `sessionType`, a
  verbatim echo of the swing's `capture` block, and `calibrated`
  (`true`/`false`/`null` — null means the recording predates calibration
  provenance).
- **`trace.jsonl`** (with `--trace`) — the shaft stages re-run with trace
  sinks: one line per frame (grip anchor, `qHandValid`, every candidate with
  θ/σ/L/score/wedge/head, the association choice) and a final line with the
  ŝ_hand fit record (`ok/sign/offsetRad/residualRad/framesUsed/sHand`) +
  pose frame count + segmentation confidence. This is the deep-debugging
  channel: "why did the fit refuse?" is answered by reading the last line,
  not by speculation.

Exit codes: `0` analysis ok, `1` load/usage failure, `2` analysis failed
(runmeta still written). Failure to open runmeta/trace files warns to stderr
rather than silently producing empty artifacts.

---

## 5. The Python Lab — `lab.py` Subcommands

One entry point (`tools/swinglab/lab.py`), subcommands, all outputs
file-based. Run with the SwingLab venv: `~/.swinglab-venv/bin/python lab.py …`
(deps: `requirements.txt` — numpy, opencv-python, matplotlib).

| Command | What it does |
|---|---|
| `doctor` | Self-orientation on any host: binary present, python deps, conventions, DLL-path hint (Windows). Run it first in a fresh session. |
| `synth <out_dir> [--clutter] [--seed N]` | Ground-truthed synthetic swing dir (§8). |
| `ingest <corpus_root>` | Build `corpus.json`; warns and refuses to bless without `CORPUS.md`. |
| `run <corpus> <runs> [--id X] [--params f] [--no-trace]` | Batch: every corpus swing through runner + scorecard; writes `summary.json` + `REPORT.md` (mean score, per-swing table sorted worst-first). |
| `one <swing> <run> [--params f] [--no-trace]` | Single swing: run + score + contact sheet, prints the verdict JSON. Exit 2 when the runner failed. |
| `score <run> <swing>` | (Re)compute `scorecard.json` for an existing run. |
| `plot <run> <swing>` | (Re)render `contact_sheet.png`. |
| `report <run_root>` | Regenerate `REPORT.md` from `summary.json`. |
| `diff <run_a> <run_b>` | Per-swing regression diff (≥ 5 points down = regression). Writes `DIFF.md` into run_b; **exit 1 when any regression** — the soak-loop gate. |
| `sweep <corpus> <runs> <space.json> [--trials N] [--method random\|coordinate] [--baseline R] [--partition p.json] [--freeze] [--allow-frozen]` | Mechanical param search. `space.json` = `{"shaft.ridgeKernelPx": [5, 15, "int"], "assembly.coverageMin": [0.4, 0.8]}`; objective = mean scorecard score; keeps best params + full history in `sweep-result.json`. No model involved. See the flag notes below. |
| `label <swing> [--every N]` | OpenCV click-UI (needs a display): step frames, click grip→head, mark P-positions with keys 1–6; writes `truth.json`. **In-app alternative:** the PinPoint **Markup** panel (`src/Gui/session/PpMarkupPanel.qml`; enable via the session toolbar's View menu → "Markup") loads/edits/saves the same `truth.json` from inside the app on the focused swing (frame-accurate cv::VideoCapture view; P1–P10 + club; byte-compatible with the scorecard). |

### `sweep` flags worth understanding before you use them

- `--method coordinate` — coordinate descent, the default strategy for separable
  knobs. `random` remains for spaces where the knobs interact.
- `--baseline <run_dir>` — applies the **diff gate per trial**: any trial that
  regresses any swing by ≥5 points against that run is rejected outright. Without
  it a sweep will happily "improve" the mean while destroying two swings.
- `--partition partitions.json` (`{tune:[], validation:[], heldout:[]}`) — sweep
  on Tune, *select* on Validation. This is the guard against fitting the search
  to the corpus.
- `--freeze` — the one-time held-out evaluation. It is opt-in precisely because
  running it more than once turns a held-out set into another validation set.
- `--allow-frozen` — permits sweeping `score.*` / `rules.*` / `bands.*`, which
  are frozen until labels exist. A post-label pass only.

### Two standalone tools alongside `lab.py`

| Tool | Purpose |
|---|---|
| `parity_diff.py RUN_A RUN_B` | **Byte-identical soak gate for refactors.** Pairs every `result.json` under two run roots by relative path and diffs each pair. The *only* excluded field is `analysis.timings` — per-stage wall-clock ms, which legitimately differ between two runs of the same deterministic pipeline. Everything else, the whole document, must match exactly. Exit 0 iff every pair compared equal **and nothing was unpaired**. This is what gated the staged-vs-monolith analyzer migration. |
| `montage_positions.py` | P1–P8 coaching-position montages per swing: a `_pstrip.png` row of 8 cropped tiles with the shaft drawn (green = MilestoneFit, orange = TrackSample, thin white = a `truth.json` label within 40 ms, grey = missing), and a `_strobe.png` single-frame overlay of every P plus a low-alpha interpolated layer — measured vs synthesized at a glance. |

The contact sheet (`plots.py`) is a 16:9 PNG: five overlay frames at the
ladder's key instants (Address/Top/Impact/Release/Finish) with the recovered
track drawn over them, plus θ(t), L(t), and θ̇(t) panels with the
segmentation ladder — the single image that tells you whether a track is
sane.

Helper classes (`swinglab/__init__.py`): `Swing` (a swing dir —
`face_on()`, `impact_us()`, `capture()`, `bindings()`, `calibrated()`,
`calib_age_sec()`, `truth()`) and `RunResult` (a run dir — `analysis`,
`club_samples()`, `trace_lines()`). Use them rather than re-parsing JSON.

---

## 6. The Scorecard

`swinglab/score.py`. Every check is a named verdict
`{name, pass, value, threshold, severity}` so a model (or a human in a
hurry) can act without watching video. Severity `fail` counts as a failure;
`warn` is reported but does not fail the swing. The score is the blunt
roll-up `100 × passed / total` — the *named checks* are the actionable
output, the score is for trend lines.

### Tier 1 — physics invariants (no labels)

| Check | Threshold | Severity |
|---|---|---|
| `club.valid` | analyzer's all-or-nothing validity gate | fail |
| `club.coverage` | ≥ 0.6 of span frames Measured/ImuBridged | fail |
| `track.monotonic_t` | 0 timestamp inversions | fail |
| `track.theta_step` | < 25°/frame between *measured* neighbours (coasted spans may legitimately bridge more) | fail |
| `track.downswing_sweep` | total \|θ\| travel in the 400 ms before impact ∈ [86°, 458°] | fail |
| `track.peak_rate_near_impact` | θ̇ peak within 120 ms of impact | warn |
| `track.head_step` | head-point step < 0.25 frame/frame (normalized) | fail |
| `track.len_step` | visible-length step < 80 px/frame between measured neighbours | warn |

### Tier 2 — cross-modal and segmentation sanity

| Check | Threshold | Severity |
|---|---|---|
| `xmodal.imu_vision_corr` | ≥ 0.9 **when IMUs are bound** (vacuously true otherwise) | warn |
| `seg.monotone` | phase events in time order | fail |
| `seg.tempo_ratio` | backswing/downswing duration ∈ [1.2, 6.0] | warn |

### Tier 3 — truth metrics (only with `truth.json`)

| Check | Threshold | Severity |
|---|---|---|
| `truth.theta_rms_deg` | θ RMS vs labelled frames < 3° | fail |
| `truth.head_median_px` | median head-point error < 25 px | fail |
| `truth.line_dist` | median ⊥ distance, labelled shaft midpoint → drawn line, < 6 px (provisional pending the Phase A2 corpus gate; per-frame `distances_px` in the check detail) | warn |
| `truth.event_top_s` | Top timing error ≤ 0.03 s | fail |
| `truth.event_takeaway_s` / `truth.event_finish_s` | ≤ 0.08 s / ≤ 0.12 s | warn |

A swing whose runner crashed scores 0 with the single failure
`runner_crashed` so batch reports never silently drop it.

---

## 7. Capture Provenance and Legacy Fallbacks

Since 2026-06 the app records capture provenance into every saved shot (see
the swing-export guide §6 for the full schema). SwingLab is the primary
consumer; **every reader keeps a legacy fallback** so pre-provenance swings
behave exactly as before:

| Recorded field | SwingLab use | Legacy fallback |
|---|---|---|
| `capture.sessionType` | runner's `job.sessionType` | `--session-type` (default 1) |
| video `capture.fps_num/den` | buffer registration fps + interarrival | 150/1, 6700 µs |
| video `setup.perspective` | face-on selection (== 2) | alias-substring match |
| imu `device.outputRateHz` | `ImuFormat::sample_rate_hz` + interarrival | 200 Hz, 5000 µs |
| `analysis.bindings[].calibrated` (+ gate angles, `calibratedAt`, `calibAgeSec`) | stderr warning per uncalibrated binding; runmeta `calibrated: true\|false\|null`; corpus filtering | assume calibrated (old behaviour), runmeta `null` |
| video `setup.ballDetection` (`calibrated`, `margin`, `driftAtCapture`, `calibratedAt`) | `ballCalibrated` (any calibrated video stream) + `ballMargin` (min margin over calibrated streams) in corpus.json — corpus filtering | absent → `null` (pre-B5 swings) |
| `capture.host.*` | runmeta echo; `appVersion` in corpus.json | absent → `null` |

`lab.py ingest` surfaces `sessionType / shotSource / calibrated /
calibAgeSec / perspectives / appVersion / ballCalibrated / ballMargin` per
swing in `corpus.json` — filter out `calibrated: false` swings before drawing
tuning conclusions, and filter on `ballCalibrated` / a `ballMargin` floor when
a mission depends on trustworthy ball-presence data. The
`calibrated` flag is the app-side composite mount gate
(`ImuInstance::fullyCalibrated()`: anatomical transform valid AND mount
deviation ≤ 15° AND gravity error ≤ 25°).

---

## 8. The Synthetic Corpus

`swinglab/synth.py` generates a complete, **ground-truthed** swing dir with
no real data: the regression fixture that validated L1–L5 and the acceptance
test for any new host (`lab.py synth` + `lab.py one` → expect ~100).

The geometry is closed-form, so truth is exact by construction:

- A 640×640 @ **60 fps** (deliberately not the production 150 — it proves
  the fps-from-JSON path), 5 s clip: a rendered shaft line over a noisy
  floor, with an optional `--clutter` alignment stick (association torture
  case).
- φ(t): address still → waggle burst → backswing smoothstep to −120° →
  downswing u² to +20° at impact (3.5 s) → follow-through to +90° → still.
  Visible length dips near Top (foreshortening), the grip drifts slightly.
- Two inline IMU streams (200 Hz) whose quaternions project **exactly** onto
  the rendered shaft angle (`hand = Ry(φ)·Rx(−80°)`), with FD-consistent
  gyro and gravity-consistent accel — so the ŝ_hand fit must engage with
  `sign = −1`, δ = 90°, corr ≈ 1.0. Identity-calibration bindings.
- `pose.json` is injected (a plausible static figure with wrists at the
  grip) because there is no human for ViTPose to find; `truth.json` carries
  per-frame θ/grip/head plus P-position times.
- The manifest stamps the full capture-provenance shape (§7) — including a
  calibrated `setup.ballDetection` block — so synthetic swings exercise every
  metadata reader.

Validation evidence on this fixture: pipeline E2E score 100/100 (16 named
checks, `imuVisionCorr 0.986`, θ RMS vs truth 0.2°, head median 2.6 px, Top
within 7 ms); deliberately broken params drop the corpus to 33/100 and
`lab.py diff` flags 3/3 regressions with a non-zero exit.

---

## 9. The Tuning Workflow

The loop the `/swinglab` skill encodes (abridged — the skill is binding for
model sessions):

1. **Baseline**: `ingest` → `run --id baseline` → read `REPORT.md`.
2. **Triage** the worst swings from `scorecard.json` (named checks),
   `contact_sheet.png`, and `trace.jsonl` if needed. Classify each failure:
   **parametric** (a config threshold plausibly wrong), **data** (bad
   recording, missing bindings, uncalibrated — report, don't tune around
   it), or **algorithmic** (the method fails structurally — escalate).
3. **Parametric fixes**: a params JSON with dotted keys matching the config
   struct fields (`seg.* / shaft.* / assembly.* / score.* / sampler.* /
   rules.* / bands.* / filter.*` — see the namespace list above), then
   `run --id candidate --params p.json`, then **always** `diff baseline
   candidate`. Keep the change only if the mean improves and `regressions:
   0`. Prefer `sweep` over hand-iterating more than ~3 times; the sweep
   loop applies the diff gate per trial and respects the Tune/Validation/
   Held-out partition (`--baseline` / `--partition` / `--freeze`).
4. **Record** findings in `<runs>/TRIAGE.md`; escalate via
   `<runs>/ESCALATION.md` on the mechanical triggers (sweep plateau ×2, one
   invariant failing >30 % of the corpus, ≥2 identical algorithmic failures,
   any C++ change needed).

Unknown tuning keys are logged and ignored by the C++ side — a typo'd key
silently does nothing to the run but is visible in `runner.log`, so check it
when a param appears to have no effect.

---

## 10. Multi-Host Operation

The Windows studio PC (RTX 5080 — CUDA pose, corpus on local NVMe) is the
preferred engine for batch runs and sweeps; the Linux dev box reaches the
same data via the `/mnt/swingdata` SMB mount. Because the interface is files
in / files out, nothing structural is needed:

- **The SwingData share is the artifact exchange medium** — corpus and runs
  live on it, so scorecards/contact sheets/TRIAGE.md written on one host are
  immediately readable on the other. `ssh studio "… lab.py run …"` works
  as-is.
- **Bootstrap from the repo** (+ one local copy of the skill): configure
  with `-DPINPOINT_BUILD_TOOLS=ON`, build `swinglab_run`, create the venv,
  `lab.py doctor`, then `synth` + `one` as the acceptance test.
- **Windows specifics**: the runner's DLLs are not on the service PATH —
  `setx SWINGLAB_DLL_PATH "<Qt bin>;<OpenCV bin>"` once per host
  (`run_one()` prepends it for the child; exit `-1073741515` = DLL not
  found). `lab.py` reconfigures stdout/stderr to UTF-8 so report unicode
  never crashes cp1252 consoles.
- **Never diff across hosts** — pose output differs CPU vs CUDA; runmeta
  records host/platform precisely so this is checkable.

---

## 11. Building and Running

```bash
# one-time: configure the tool target, build it
cmake -S . -B build/Desktop_Qt_6_11_0-Debug -DPINPOINT_BUILD_TOOLS=ON
cmake --build build/Desktop_Qt_6_11_0-Debug --target swinglab_run --parallel 4

cd tools/swinglab
P=~/.swinglab-venv/bin/python          # numpy / opencv / matplotlib venv

$P lab.py doctor                                  # orient on this host
$P lab.py synth /tmp/corpus/synth_0001            # ground-truthed fixture
$P lab.py one   /tmp/corpus/synth_0001 /tmp/runs/x  # run+score+contact sheet
$P lab.py ingest /path/to/corpus && $P lab.py run /path/to/corpus /tmp/runs --id baseline
```

Binary resolution: `$SWINGLAB_BIN` if set, else
`build/Desktop_Qt_6_11_0-Debug/swinglab_run`. The runner resolves the
ViTPose model next to its own binary, same as the app (the CMake target
compiles with `HAVE_VITPOSE` only when the model file was found at
configure time — without it, inject poses with `--pose`).

The target builds the production analysis sources directly into the binary
(`shot_analyzer.cpp`, `shaft_tracker*.cpp`, `pose_runner.cpp`,
`swing_doc.cpp`, …) and links `pinpoint_buffer`, OpenCV, ORT, and whisper —
see the `PINPOINT_BUILD_TOOLS` block at the bottom of the root
`CMakeLists.txt`. **Rebuild `swinglab_run` after any analysis C++ change** —
it does not happen via the app target.

---

## 12. Extending SwingLab

### Adding a Tier-1/2 check

Append to `invariants()` in `score.py` using the `_check(name, ok, value,
threshold, severity)` helper. Rules: name it `area.check_name`; always
report the measured value and the threshold (a model triages from those
numbers); choose `warn` unless a failure reliably means a broken track
(over-eager `fail`s teach operators to ignore the score). New checks shift
the 100-point normalisation — re-baseline before diffing across the change.

### Adding a tuning knob

Add the field to the relevant config struct (`ShaftDetectConfig` etc.), then
map the dotted key in the tuning-override application in the analyzer code
path. Knobs must default to today's constants so production behaviour is
unchanged. Document the key in `tools/swinglab/configs/` presets if it is
sweep-worthy.

### Adding a new recorded input

Follow the additive-schema contract (swing-export guide): new stream fields
or blocks must be optional, with the runner falling back to current
behaviour when absent (§7 is the template). Stamp the same shape into
`synth.py` so the new reader path is exercised by the fixture, and surface
anything corpus-filterable in `ingest()`.

### Trace additions

`trace.jsonl` is the deep-debug channel; new per-frame fields go into the
`ShaftTrace` out-params (C++) and are serialized in the runner's trace loop.
Keep it one JSON object per line — downstream readers are
line-oriented (`RunResult.trace_lines()`).

---

## 13. Common Mistakes

- **Reimplementing analysis math in Python.** The scorecard *judges*
  outputs; it never recomputes the pipeline. If you need different analysis
  behaviour, change the C++ and rebuild — that is the whole point of the
  runner.
- **Forgetting to rebuild `swinglab_run` after C++ changes.** It compiles
  the analysis sources itself; building the app target does not refresh it.
- **Tuning on unblessed or uncalibrated data.** No `CORPUS.md` → no
  conclusions. `calibrated: false` swings (corpus.json / runmeta) are data
  failures, not parametric ones — exclude them, don't tune around them.
- **Keeping a params change without the diff gate.** `lab.py diff` exists
  because a +3 mean can hide a −20 on two swings. `regressions: 0` or it
  doesn't land.
- **Comparing runs across hosts.** CPU vs CUDA pose differs subtly;
  same-host diffs only.
- **Writing into swing dirs.** Only `truth.json` (via `label`) and the
  SwingLab-owned `pose.json` belong there. Runs land in the run dir you
  point at.
- **Trusting MP4 replays for pixel-level conclusions.** Re-encoded pixels
  are approximate; scorecards carry `frames: mp4` for a reason. Record with
  `saveRawFrames` ON for tuning corpora.
- **Changing reconstruction in the runner instead of the loader.**
  `SwingDiskLoader` is shared with the in-app re-analyse path. A "quick fix"
  inside `swinglab_run.cpp` that bypasses it means SwingLab and the app disagree
  about the same swing — which is precisely the class of bug the shared loader
  exists to make impossible.
- **Sweeping without `--baseline`.** A sweep optimises the *mean*; nothing stops
  it trading two swings' collapse for a broad small gain. The per-trial diff gate
  is the flag, not an afterthought.
- **Running `--freeze` more than once.** The second run makes the held-out set a
  validation set, and you no longer have an unbiased estimate of anything.
- **Synthesizing identity bindings for swings that lack them.** The runner
  deliberately refuses — re-fusing without the session calibration
  fabricates data.
- **Expecting a `--params` typo to fail loudly.** Unknown keys are logged
  and ignored (`runner.log`), not fatal — verify the key took effect before
  concluding "the knob does nothing".

---

## 14. File Map

```
tools/swinglab/
  lab.py                        # CLI entry point (subcommands → swinglab/*)
  parity_diff.py                # byte-identical refactor soak gate (§5)
  montage_positions.py          # P1–P8 position strips + strobe tiles (§5)
  requirements.txt              # numpy, opencv-python, matplotlib
  configs/                      # params presets + sweep spaces
  src/swinglab_run.cpp          # C++ offline runner (PINPOINT_BUILD_TOOLS=ON)
  swinglab/
    __init__.py                 # paths, json io, Swing / RunResult models
    core.py                     # doctor, ingest, run_one/run_corpus, report, diff, sweep
    score.py                    # Tier 1–3 scorecard (named checks)
    plots.py                    # contact sheets (track overlay + θ/L/θ̇ panels)
    label.py                    # truth.json click-UI (needs display)
    synth.py                    # ground-truthed synthetic swing generator

src/Analysis/swing_reanalyzer.h # SwingDiskLoader — the SHARED disk → SwingWindow
                                #   reconstruction (§4), also used by the in-app
                                #   re-analyse path
src/Analysis/shot_analyzer.h    # ShotAnalysisJob (tuningOverrides, poseTrackPath,
                                #   ballTrackPath)
src/Analysis/analysis_tuning.h  # tuning::apply — dotted key → config struct field
src/Core/pp_tuned_constants.h   # pinpoint::tuned::* — the frozen defaults
src/Analysis/swing_analysis.h   # ImuSegmentBinding / BindingRecord (calibration status)
src/Export/swing_doc.{h,cpp}    # result.json writer (shared with the app)

docs/implementation/swinglab_impl.md      # design + stage history (L0–L5)
docs/developer/swing_export_developer_guide.md  # the swing-dir artifacts replayed here
.claude/skills/swinglab/SKILL.md          # /swinglab operator contract (LOCAL-ONLY, gitignored)

<SwingData>/corpus-v1/          # corpora live OUTSIDE the repo (CORPUS.md required)
<SwingData>/runs/               # run outputs (scorecards, plots, traces, reports)
```
