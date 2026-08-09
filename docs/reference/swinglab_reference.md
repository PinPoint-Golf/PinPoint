# SwingLab reference

SwingLab is the offline validation harness for the analysis pipeline: it replays recorded
swing folders through the **unmodified production analyzer** and grades, compares, and
reports on what came out. It exists so that pipeline changes are judged against real
captured swings — reproducibly, on any host — rather than by watching the app.

One C++ binary does the work; a Python CLI orchestrates it.

```
tools/swinglab/
├── lab.py                  the CLI (subcommands below)
├── src/swinglab_run.cpp    the runner binary (CMake target `swinglab_run`)
├── swinglab/               core.py · score.py · plots.py · coverage.py · synth.py · label.py
├── reanalyze_corpus.py     in-place corpus re-analysis driver (write-back sweeps)
├── parity_diff.py          byte-identical result.json diff between two run roots
├── montage_positions.py    P1–P8 position strips / strobe montages from club.positions[]
└── configs/                sweep spaces, partition template, A/B flag presets
```

`docs/validation/` holds the collection protocols and the tuning methodology this harness
serves; `docs/developer/analysis_pipeline_developer_guide.md` covers the pipeline itself.

---

## The runner: `swinglab_run`

Built with `-DPINPOINT_BUILD_TOOLS=ON`; compiles the production Analysis / Diagnostics /
Metrics / Export sources directly, so a run IS the production pipeline. Reconstructs the
swing from `<swing_dir>/swing.json` + media sidecars via the streaming `SwingDiskLoader`
(never the live EventBuffer), runs `makeShotAnalyzer(sessionType)->analyze()`, and writes
the results into a run directory.

```
swinglab_run <swing_dir> --out <run_dir> [options]
swinglab_run <swing_dir> --write-back [--session-type N]
```

| Option | Meaning |
|---|---|
| `<swing_dir>` | Positional: the swing folder (contains `swing.json`, `Face-On.mp4`/`.raw`, …). |
| `-o, --out <dir>` | Run directory to write `result.json` / `runmeta.json` / `runner.log` (+ `trace.jsonl` with `--trace`). Required except in `--write-back` mode. |
| `--params <file>` | Tuning-override JSON. Accepts nested `{"shaft": {"ridgeKernelPx": 11}}` and/or flat `{"shaft.ridgeKernelPx": 11}` spellings. **Unknown keys are logged and ignored** — check `runner.log` when a knob seems to do nothing. Since the 2026-07-18 freeze, `refine.enabled` / `ball.clubActivity` are on by default, so an empty params file is the production configuration. |
| `--trace` | Re-runs the shaft stages with trace sinks and writes `trace.jsonl` (one line per frame: phase, tier, θ, conf, ψ error, head tier…, plus a final summary line). |
| `--session-type <int>` | Analyzer session type. Explicit option wins; else the recorded `capture.sessionType`; else 1. Type 1 (Wrist) is the only analyzer with the shaft tracker; 0/2/3 run the shared camera-kinematics analyzer. |
| `--face-on <substr>` | Which camera counts as face-on: default matches the recorded per-stream `setup.perspective` (`"Face"`); the flag forces a substring match — the escape hatch for mislabelled recordings. |
| `--impact-us <us>` | Impact-instant override. Without it the recorded impact is used; a doc with no impact fails (`pass --impact-us`). |
| `--pose <file>` | Inject a `PoseTrack2D` JSON and skip ViTPose. **Timestamps must be window-relative** — see *The timebase contract* below. |
| `--ball <file>` | Inject a `BallTrack2D` JSON and skip the offline ball replay. |
| `--refuse-orientation` | Orientation re-fusion parity (corpus-1 gate E1): re-run Madgwick from the recorded raw accel+gyro, report disagreement vs the stored quaternion, write `refusion.json`, and exit — no pose/shaft pipeline. Run on pilot swings before bulk capture to prove the corpus is post-hoc-tunable. |
| `--refuse-beta <f>` | Madgwick β override for the re-fusion parity run. |
| `--write-back` | **Re-analyse in place**: the headless twin of the in-app ReanalysisController. Runs `reanalyzeSwingDir()` with production defaults, then rewrites the *source* `swing.json`, replacing only the `analysis` block — capture/streams/review ride through untouched, with an empty-manifest guard so an unreadable doc is never clobbered. Exclusive mode: only the positional dir and `--session-type` (for recordings that predate `capture.sessionType`) are honoured. **Back up the corpus `swing.json` set before a sweep.** |

Environment:

| Variable | Meaning |
|---|---|
| `SWINGLAB_BIN` | Path to the binary; the Python layer's default is the dev-tree Debug build. Point it at the app build tree (e.g. `build\Release-Installer\swinglab_run.exe` on the studio PC). |
| `SWINGLAB_DLL_PATH` | (Windows) Prepended to the child `PATH` by `run_one()`. Direct invocations of the exe outside the Python layer need it prepended manually (`set PATH=%SWINGLAB_DLL_PATH%;%PATH%`) or the exe dies with `0xC0000135` before printing anything. |

**Colocation (Windows):** the exe must live in the same directory as the app's DLL set and
`models/` (both are copied POST_BUILD next to `PinPointStudio.exe`). An exe built into a
fresh directory finds no ViTPose model and **pose silently produces nothing**.

---

## The CLI: `lab.py`

Run with the SwingLab venv (`~/.swinglab-venv/bin/python`, Windows
`%USERPROFILE%\.swinglab-venv\Scripts\python.exe`; deps in `requirements.txt`).

### `doctor`
Self-orientation for a session on any host: verifies the binary, python deps and
conventions; prints fixes. Run it first on an unfamiliar machine.

### `synth <out_dir>` — ground-truthed synthetic swing
Generates a fixture with known geometry (and therefore a perfect `truth.json`).

| Flag | Meaning |
|---|---|
| `--clutter` | Add background clutter. |
| `--seed N` | RNG seed (default 7). |
| `--impact-spike` | ±16 g saturation+ringing burst at impact (exercises blanking/saturation, D4). |
| `--archetype bowed\|neutral\|cupped` | Script a lead-wrist archetype (forearm FE bias) and stamp `truth.meta.archetype`. |
| `--fault <label>` | Known-groups label stamped in `truth.meta.knownGroup` (default `cast` for a plain synth; none for archetype/no-imu variants). |
| `--no-imu` | Camera-only fixture: omit IMU streams + bindings; every IMU/wrist check then skips. |

### `ingest <corpus_root>` — build `corpus.json`
Walks the root for dirs containing `swing.json` and records quick facts per swing:
videos, raw sidecar presence, IMU count, impact presence, **bindings count**, truth
presence, markup conditions (club/contact/scope/tempo), known-group label, session type,
calibration provenance. **Paths are absolute** — ingest on the host that will run the
batch. Names are the bare dir names — see *Gotchas* for the collision this causes.

### `run <corpus_root> <runs_root> [--params f] [--id NAME] [--no-trace] [--session-type N]`
Batch the whole corpus through the runner into `<runs_root>/<id>/<swing_name>/`, then
write `summary.json` + `REPORT.md` (markdown table, worst-first, mean score). Does **not**
render contact sheets (use `plot`/`one`).

### `one <swing_dir> <run_dir> [--params f] [--no-trace] [--session-type N]`
Run + score + plot for a single swing; prints the scorecard summary; exit 2 on failure.

### `score <run_dir> <swing_dir>` / `plot <run_dir> <swing_dir>`
Regenerate `scorecard.json` / `contact_sheet.png` for an existing run.

### `report <run_root>`
Regenerate `REPORT.md` + `summary.json` from the scorecards already on disk.

### `diff <run_a> <run_b>`
Regression diff between two run roots; writes `DIFF.md` into run_b; exit 1 on any
regression. Pairs swings **by bare name** — see *Gotchas*.

### `coverage <run_root> [--pack core.json] [--out dir]`
Diagnostics-pack coverage over a run root: which phase events each swing emitted, and per
pack measure whether it is RESOLVED / BLOCKED_PHASE (metric present, a reducer phase
absent) / BLOCKED_METRIC (metric absent) / NO_LM (an `lm.*` key with no launch-monitor
data), with corpus roll-ups and top-blocker rankings. Writes `COVERAGE.md` (to `--out`
when the run root is read-only). Default pack: `src/Resources/diagnostics/core.json`.
This is the instrument that makes a silently-dead stage visible — a healthy-looking run
with `RESOLVED=0` means the ladder never got written.

### `sweep <corpus_root> <runs_root> <space.json> [flags]`
Parameter search over a tuning space (`configs/shaft_space.json` is the model).

| Flag | Meaning |
|---|---|
| `--trials N` | Trial count (default 20). |
| `--seed N` | RNG seed (default 1). |
| `--method random\|coordinate` | Search strategy; coordinate descent is the §7.1 default for separable knobs. |
| `--baseline <run_dir>` | Reject any trial that regresses any swing vs this prior run (per-swing 5 pt). |
| `--partition <partitions.json>` | `{tune:[], validation:[], heldout:[]}` — sweep Tune, select on Validation. Template: `configs/partitions.template.json`. |
| `--freeze` | Permit running the held-out set — the one-time freeze evaluation. Refused otherwise. |
| `--allow-frozen` | Permit sweeping `score.*` / `rules.*` / `bands.*` (frozen until labels; post-label pass only). Refused otherwise. |

Outputs `sweep-result.json` + per-trial `sweep-params-NNN.json`.

### `label <swing_dir> [--every N]`
Click-UI for hand-labelling `truth.json` (every Nth frame, default 20).

---

## Companion scripts

**`parity_run.py CORPUS_ROOT RUNS_ROOT --repo REPO [--params f] [--pose-dir d]`**
(NAS-side, `stagegate/`.) Corpus driver that fixes two `lab.py run` limitations: run dirs
are session-disambiguated (`<session>__<swing>`), and manifest paths recorded on another
host are rebased onto this host's corpus root. `--pose-dir` injects per-swing pose files
named `<session>__<swing>.json`.

**`extract_pose.py RUN_ROOT CORPUS_ROOT OUT_DIR`**
(NAS-side, `stagegate/`.) Extracts `analysis.pose2d.frames` from each swing's
`result.json` into injectable pose files, **t_us copied verbatim** (see the timebase
contract). Purpose: CUDA ViTPose is nondeterministic run-to-run (~1e-9 keypoint jitter),
which breaks byte-identical parity gates — freeze one canonical pose pass and inject it
on both sides of every diff. Also ~25 % faster per swing.

**`reanalyze_corpus.py CORPUS_ROOT [--bin exe] [--only substr] [--session-type N]`**
(In-repo.) Sequential `--write-back` sweep over every swing dir under the root — the
headless "re-analyse all shown". Failures (LM-only, no media, no impact) are reported and
skipped. **Back up first**: `tar czf backup.tgz <root>/*/swing_*/swing.json`.

**`parity_diff.py RUN_A RUN_B`**
Byte-identical `result.json` comparison (excludes only `analysis.timings`) — the strict
gate for "this flag changes nothing".

**`montage_positions.py`**
Renders `_pstrip.png` / `_strobe.png` montages from `analysis.club.positions[]`
(green = MilestoneFit, orange = TrackSample).

---

## What a run produces

Per swing under `<runs_root>/<id>/<swing>/`:

| File | Content |
|---|---|
| `result.json` | The full `analysis` block (`pinpoint.analysis/3`): `ball`, `club`, `metrics`, `phases`, `pose2d`, `score`, `segmentation`, `tier`, `timings` (+`assessment`/`filter` when produced). ~10 MB. |
| `runmeta.json` | `ok/error/score`, per-stage timings (`poseMs/ballMs/shaftMs`; **-1 = stage did not run, 0 = ran and bailed instantly** — treat a 0 with suspicion), params echo, impact, bindings, host/platform, frames source (`raw`\|`mp4`), capture echo. |
| `runner.log` | The exe's stdout+stderr. Sparse by design — two lines on a clean run. |
| `trace.jsonl` | Per-frame shaft internals (with `--trace`). |
| `scorecard.json` | The graded checks (below). |
| `contact_sheet.png` | Via `one`/`plot` only. |

Run root: `summary.json`, `REPORT.md`, `DIFF.md` (after `diff`), `COVERAGE.md` (after
`coverage`).

## The scorecard

Three tiers; score = `100 × passed / total checks`, so **adding or removing a check
re-normalises the score — re-baseline before diffing across a check change**. Swings with
`scope` pitch/chip/putt skip the full-swing-only checks.

- **Tier 1 — track sanity** (no truth needed): `club.valid`, coverage ≥ 0.6, monotone t,
  θ step < 25°/frame, downswing sweep in [86°, 458°], peak rate near impact, head/length
  step bounds.
- **Tier 2 — cross-modal / segmentation**: IMU-vision correlation ≥ 0.9 (vacuous with no
  bindings), ladder monotone, tempo ratio in [1.2, 6.0].
- **Tier 3 — vs truth.json**: θ RMS < 3°, head median < 25 px, line distance, the
  `truth.event_*` timing set (P1/P3/P4/P5/P6/P7/P8/P9/P10 where the analyzer emits an
  event — p2 has none and gets a shaft-horizontality sanity check instead), and
  `diag.*`/`score.*` known-groups checks when `truth.meta` carries labels.

## The timebase contract

The one rule that has bitten three times, twice silently:

> **Everything serialized, and everything the offline replay window uses, is
> window-relative (0-based µs).** Only the *live* capture window runs on the absolute
> EventBuffer clock; `serializeAnalysis` (swing_doc.cpp) subtracts `clock.t0_us` on
> write, exactly once.

Consequences: injected pose/ball files carry window-relative `t_us`; anything that
"helpfully" rebases to absolute (`+ clock.t0_us`) produces timestamps that intersect no
window frame, and `ShaftTracker` refuses in 0 ms — `shaftMs: 0`, no club block, **no
phase ladder**, score 0, while `runner.log` still says "analysis ok". Every pre-2026-08-09
pose-injected run root is broken exactly this way. If a run root looks healthy but
`coverage` says `RESOLVED=0`, check this first.

## Gotchas

- **Name collisions**: `lab.py run`/`diff` key swings by bare dir name (`swing_0004`
  exists in every session). Either prefix the swing dir names in the corpus (the
  `wb1-corpus-win` convention) or use `parity_run.py`, which disambiguates.
- **Never diff across hosts or configs** — CPU vs CUDA pose differs; MSVC Debug is 5–6×
  slower and not comparable.
- **`corpus.json` is host-bound** (absolute paths): re-ingest on the machine that runs,
  or rely on `parity_run.py`'s rebasing.
- **`bindings: 0`** in the ingest record means no IMU-vision bindings were persisted: no
  offline re-fusion, no wrist metrics from IMU, no cross-modal check, and the IMU phase
  ladder (P3/P5/P6/P8/P9, MaxSpeed, Transition) can never be produced — the vision model
  supplies only P1/(takeaway)/P4/P7/finish. This is a capture-time gap; no replay flag
  recovers it.
- **Unknown `--params` keys don't error.** Verify in `runner.log`.
- **`run_corpus` renders no contact sheets**; call `plot` per swing.
- **Windows detach**: a long run started over ssh dies with the session — launch via
  `Invoke-CimMethod Win32_Process Create` on a `.cmd` that writes a done-sentinel, and
  note PowerShell `*>` redirects write UTF-16LE (prefer `cmd /c ... > log 2>&1`).

## Typical sessions

Studio corpus gate (Windows, Release+CUDA):

```powershell
cd C:\Users\developer\Projects\PinPointStudio ; git pull
cmake --build build\Release-Installer --target swinglab_run --parallel 4
set SWINGLAB_BIN=...\build\Release-Installer\swinglab_run.exe
%USERPROFILE%\.swinglab-venv\Scripts\python.exe tools\swinglab\lab.py doctor
# baseline at the pre-change commit, candidate at HEAD, both via parity_run.py, then:
lab.py diff <runs>\baseline <runs>\candidate
lab.py coverage <runs>\candidate
```

In-place corpus refresh after a pipeline change:

```powershell
tar czf backup.tgz Mark-Liversedge/*/swing_*/swing.json     # first, always
python tools\swinglab\reanalyze_corpus.py C:\PinPointStudio\Mark-Liversedge
```
