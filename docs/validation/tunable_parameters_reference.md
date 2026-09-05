# PinPoint Studio — Tunable Parameters: Validation, Optimisation & Developer Guide

**Status:** Reference (current). Companion to
[`pipeline_validation_and_tuning.md`](pipeline_validation_and_tuning.md) — that document is the
*methodological backbone* (validity ladder, the three-corpus progression, sample-size/power per
statistic); **this** document is the *parameter-centric* view: every knob that can be tuned, what it
moves, how we decide its value statistically, and exactly how the injection + sweep machinery works in
code.
**Audience:** Part I (Reference) for analysts/reviewers; Part II (Developer Guide) for anyone wiring a
new parameter or running a sweep.
**Harness:** SwingLab (`tools/swinglab/`, `swinglab_run` + `lab.py`).

> **One-line model.** A parameter is *injected* at run time via a dotted key
> (`<area>.<field>` → number) into `ShotAnalysisJob::tuningOverrides`; each stage applies the keys it
> owns onto its config struct, so a sweep iterates at binary speed with no rebuild. Its frozen default
> lives in `src/Core/pp_tuned_constants.h`. It is *optimised* by `lab.py sweep` against a scorecard
> objective, under a per-swing regression gate and a Tune/Validation/Held-out partition. When validation
> locks a value, you edit the one literal in the header.

---

# Part I — Reference

## 1. The injection contract (what "tunable" means)

"Tunable" has a precise operational meaning here: **the parameter is injectable via the dotted-key
`tuningOverrides` mechanism, so SwingLab can sweep and diff-gate it without a rebuild.** A parameter that
is only a compiled-in constant is *not* tunable in this sense until it is wired to a dotted key.

Three statuses appear in the §2.4 tuning ledger of the backbone doc, and they are the vocabulary here:

- **dotted-key** — injectable today; a `space.json` can sweep it.
- **code** — a real knob, but rebuild-only (e.g. live-thread seed tolerances, runtime filter choice).
- **n/a** — not a tuning surface (a capture gate / census).

A fourth, historical status — **escalation** (a knob that existed but had no injection path) — has been
retired for the wrist/scoring/diagnosis/filter layers: they are now dotted-key (this work).

## 2. The parameter catalog

Every namespace, its owning config, where its default is frozen, and **what observable it moves**. The
last column is the crucial one for optimisation: a sweep can only optimise a parameter whose change is
visible in a scorecard check (§4).

| Namespace | Owner config (file) | Default source | Tuned at | Moves which observable |
|---|---|---|---|---|
| `seg.*` | `SegmentationConfig` (`phase_segmenter.h`) | struct defaults | C1 | `seg.monotone`, `seg.tempo_ratio`, `truth.event_*_s` |
| `shaft.*` | `ShaftDetectConfig` (`shaft_tracker_math.h`) | struct defaults | C1 | `club.coverage`, `track.*`, `truth.theta_rms_deg`, `truth.head_median_px` |
| `assembly.*` | `AssemblyConfig` (`shaft_track_assembly.h`) | struct defaults | C1 | `club.coverage`, ŝ_hand residual, `xmodal.imu_vision_corr` |
| `score.*` | `kWristBands` + deadbands (`swing_scorer.cpp`) | `pp_tuned_constants.h` `scoring::` | **C2** | `analysis.score` (`r.score`) — **no Tier-1 check; objective is HackMotion** (§4) |
| `sampler.*` | `PpWristSamplingConfig` (`wrist_angle_sampler.h`) | `sampler::` | C1→C2 | `analysis.assessment.findings` → `diag.*` (gimbal proxy currently inert — §4) |
| `rules.*` | `RuleTuning` (`assessment_rule.h`) | `rules::` | C1→C3 | `analysis.assessment.findings`/`scoreV2` → `diag.*` |
| `bands.*` | `BandTuning` margins (`reference_bands.h`) | (table; margins runtime) | C2 | `analysis.assessment.findings` → `diag.*` |
| `filter.*` | `RefuseConfig` (`orientation_refuser.h`) | `filter::` | C1→C2→C3 | wrist angles → `xmodal.imu_vision_corr`, `diag.*`, `filter.impact_continuity` |
| `seed.*` (status **code**) | `kInit*` (`imu_base.h`) | `seed::` | C1 | live filter convergence (offline-unreachable) |
| `pose.intraOpThreads` | `ShotAnalysisRunnerOptions` / `PoseEstimatorViTPose::load` (`pose_runner.cpp`) | `pp_tuned_constants.h` `pose::` | perf | offline ViTPose ORT intra-op pool → compute wall-time (default 0 = legacy heuristic) |
| `poseSmooth.legsSigmaScale` / `poseSmooth.legsJerkScale` | `PoseSmootherConfig` (`pose_smoother.h`, via `fromOverrides` in `PoseSmoothStage`) | `pp_tuned_constants.h` `pose::smoother::` | C1 | the RTS smoother's effective window on keypoints 11–16 → residual jitter and PK RATE of every hip/knee series (`pelvisSway`, `hipLineTilt`, `plumbBobDistance`, `leadKneeDrift`) (**both DARK at 1.0** — ×1.0 is exact, so the shipped defaults are byte-identical; the only keys in this table that move persisted `value[]`) |
| `shaft.onsetReturn*` / `shaft.onsetRunBridgeFrames` / `shaft.onsetBridgeMinNetFrac` / `shaft.emitTakeaway` | `ShaftV3Config` (`shaft_track_assembly.h`) | `pp_tuned_constants.h` `shaft::` | C1 | `truth.p1_address`, `seg.tempo_ratio`, Address→Top duration (camera-only fidget swings; **ALL FROZEN ON** — box 7 / gap 15 / bridge 10 / Takeaway on (2026-07-17), m3gate 0.2 (2026-07-18); 0 disables each) |
| `ball.clubActivity` / `ball.activity*` / `positions.p1ClubQuietSigma` / `ball.tk0AddressOverride` | `BallActivityConfig` (`ball_runner.cpp`) / `PositionsConfig` (`shaft_positions.h`) / `applyBallAnchor` (`ball_anchor.cpp`) | `pp_tuned_constants.h` `ball::activity`, `ball::`, `positions::` | C1 | `truth.p1_address`, Address→Top duration (camera-only club-bob fidget swings; activity **FROZEN ON 2026-07-18** with `refine.enabled` — `false` still darks it out, byte-identical; tk0 override **FROZEN OFF 2026-07-17**) |
| `refine.*` | `EventRefineConfig` (`event_refine.h`) | `pp_tuned_constants.h` `refine::` | C1 | `truth.p1_address`, Takeaway vs truth (late-pipeline event refinement; **FROZEN ON 2026-07-18**, minConf 0.8 — `refine.enabled=false` restores the byte- and code-path-identical pre-refine ladder) |
| `kinematics.enabled` | `KinematicSeriesConfig` (`kinematic_series.h`) | `pp_tuned_constants.h` `kinematics::` | display | review-chart clubhead/hand speed (mph) + lag (°) — unscored display series from the shaft/pose products, all session types (**DARK** by default; `true` runs `KinematicsStage` on Wrist and the camera-kinematics profile on Swing/GRF/Coach — freeze ON after the corpus gate) |

### 2.1 `seg.*` — phase segmentation (≈25 keys)
Envelope cut-off, top/takeaway/transition windows, vote-agreement, finish stillness gates
(`fcEnvelopeHz`, `top*BeforeImpactUs`, `takeawayFracOfPeak`, `voteAgreeUs`, `finish*Us`, …). **Load-bearing:**
every wrist metric is sampled *at a phase*, so a Top-timing error propagates into every angle.

### 2.2 `shaft.*` / `assembly.*` — club track (≈30 + ≈12 keys)
Ridge/Hough detection (`ridgeKernelPx`, `noiseSigmaK`, `thresholdFloor`, `nmsSeparationDeg`,
`clutterMaskDeg`, `minScoreFrac`, `runMaxGapPx`, `interHandSigmaDeg`, …) and assembly/fusion
(`coverageMin`, `jerkPsd`, `transSigma*`, `visionSigmaFloorRad`, `calibAcceptRad`, …), plus the skeleton-aware flag-flips (`useArmScale`, `useKinematicPrior`, `useEnvelope`, `useBlurMode`,
`emitPredicted`, `useBackgroundSub`, `twoPassCalibration`, `autoChirality`, default OFF/byte-identical).

### 2.3 `score.*` — wrist scoring bands
Per metric × phase: `score.<metricKey>.mu / .sigma / .weight / .oneSidedDir`, plus the deadband shape
`score.zIn / .zOut / .p`. Metric keys: `leadWristFlexExt`, `leadWristRadUln`, `forearmPronation`,
`leadArmFlexion`. Frozen defaults (`pp_tuned_constants.h::scoring`):

| metric | μ | σ | oneSided | weight |
|---|---|---|---|---|
| leadWristFlexExt | 15 | 12 | +1 (penalise below) | 0.45 |
| leadWristRadUln | 0 | 12 | 0 | 0.15 |
| forearmPronation | 0 | 25 | 0 | 0.20 |
| leadArmFlexion | 5 | 12 | −1 (penalise above) | 0.20 |

Deadband: `zIn=1.0, zOut=3.0, p=2.0`. These set the **0–100 number the golfer sees**; re-seated at
Corpus 2 on the observed + HackMotion tour-range distribution.

### 2.4 `sampler.*` — windowed-median wrist sampler
`sampler.windowHalfUs` (15000), `sampler.gimbalThresholdDeg` (75.0), `sampler.minValidSamples` (1).
Governs the Gap / Indeterminate / Ok decision at each P-position. **Caveat:** `gimbalThresholdDeg` is
inert in the current offline path because the analysis adapter sets `pitchProxyDeg = 0` — see §4.

### 2.5 `rules.*` — Tier-2 fault/strength engine
`rules.confidenceFloor` (0.45), `rules.scoreScale` (18.0), `rules.severityWeightFault` (1.0),
`rules.severityWeightWatch` (0.5), `rules.corroborationBoost` (0.30),
`rules.strengthsRequireAdjacentFault` (true). Decides which faults fire and how hard they score.

### 2.6 `bands.*` — reference-corridor margins
`bands.{radUln,flexExt,forearm,trailWrist,elbow}Margin` — the amber margin (degrees either side of the
green corridor) per DOF; a negative value (default) means "use the compiled-in table margin". The full
8-position lo/hi corridor arrays stay in `reference_bands.cpp` (a Corpus-2 re-seat, not a sweep target).

### 2.7 `filter.*` — orientation re-fusion + phase-adaptive schedule
`filter.refuse` (bool — the master switch that feeds re-fused orientation into the fusion),
`filter.adaptive` (bool — phase-adaptive schedule vs fixed-gain), `filter.beta` / `filter.betaStatic`,
`filter.betaDynamic`, `filter.accelErrGateG` (0.30), `filter.gyroGateDps` (200), `filter.accelSatG` (16),
`filter.impactBlankPreMs` (5), `filter.impactBlankPostMs` (15). The deepest tuning surface in the IMU
path — see [`pipeline_validation_and_tuning.md` §5.3.1](pipeline_validation_and_tuning.md). **Open
tail:** ESKF `R` is not exposed (vendored lib, no warm-start — outside the re-fusion loop).

### 2.8 `seed.*` — stillness-gated seeding (status: code)
`kInitAccelTolG` (0.15), `kInitGyroMaxRadps` (0.5), `kInitMaxSeedAttempts` (200). Live I/O-thread only;
sourced from `pp_tuned_constants.h::seed` but not offline-reachable, so classified **code** not
dotted-key.

### 2.9 `pose.intraOpThreads` — offline ViTPose ORT intra-op pool (perf)
`pose.intraOpThreads` (**0**). Sizes the ONNX Runtime intra-op thread pool for the offline ViTPose
pass — 70%+ of analysis wall-time on CPU hosts. Resolved in `PoseRunner` from the job overrides (or the
`ShotAnalysisRunnerOptions::intraOpThreads` seed) and applied by `PoseEstimatorViTPose::load()` before
the pool is built. **Three-way**:

- **`0` (default)** — the legacy proxy heuristic `clamp(hardware_concurrency()/2, 1, 8)`, left exactly
  as-is so the default path is **thread-count-identical** to the historical behaviour.
- **`-1`** — physical-core **topology** auto: `clamp(physicalCoreCount(), 1, 16)` via the header-only
  `src/Core/cpu_topology.h` (Linux sysfs `thread_siblings_list` dedup / Windows
  `GetLogicalProcessorInformationEx` / macOS `hw.physicalcpu`, with the `/2` fallback). **Opt-in** — it
  does not become the default until a determinism A/B on the affected hardware (no-SMT, hybrid P/E-core
  Intels, >16-logical machines — all of which defeat the `/2` proxy) is run.
- **`> 0`** — pinned exactly (manual override).

A **performance** knob only — `0` is byte-identical to history, and the live 60 Hz MoveNet path is
untouched (pinned at 1). The topology header is discovered once and cached, so it is cheap per model
load.

### 2.10 `shaft.onsetReturn*` / `shaft.onsetRunBridgeFrames` / `shaft.emitTakeaway` — camera-only Address/Takeaway hardening (C1)
Frozen in `pp_tuned_constants.h::shaft`, consumed by `ShaftV3Config` (`shaft_track_assembly.h`). Three
independent camera-only fixes. **FROZEN ON 2026-07-17** (user-approved after the in-app eyeball;
17-swing truth evaluation: Address-error median **0.564 s → 0.060 s**): box 7 / gap 15 / bridge 10 /
Takeaway event on. Setting a key to **0** (`false` for the event) still disables that fix individually —
the all-dark combination remains the byte-identical-legacy baseline for soaks and A/Bs.
**Retired keys (2026-07-17):** the first-cut anchor-box veto's `shaft.onsetReturnPhiDeg` and
`shaft.onsetReturnStillFrames` are **gone** — the 17-swing dump diagnosis proved both of its premises
unsatisfiable on real capture (the lerped-pose grip keeps a 2–4 px/f smoothed-speed floor through every
fidget settle, so an absolute-rest gate never fires; and the golfer settles into an address **displaced**
from the pre-fidget stance — 60 px on w1s1 — so an anchor-box return test never fires either; the veto
fired on 0/17 truth swings). The revisit scan below replaces it and needs neither knob.

- **The "no-return" veto** (`segmentPhases`). On real capture the A1/A2 walk-back runs through the
  whole fidget to the deep pre-fidget stillness (0.5–1.5 s early on 12/17 truth swings). The veto runs
  after A1/A2 and before the A3 impact clamp, can only move the onset **later**, and is
  **departure-referenced**: `revisit(r) = min dist(gS[f], gS[r])` over `f ∈ [r + gap, bs0]`
  (`gS = gauss(median5(grip))`); the last `r` with `revisit(r) < onsetReturnBoxPx` is the no-return
  boundary — the last instant the track ever comes back to. Every fidget settle (and waggle burst) is
  revisited by the next excursion's return; the takeaway departs for good, so the boundary lands at the
  final settle. Real-dump validation: w2s6 −214 → **+13 ms**, w1s1 −516 → **+67 ms**, w2s4 (with
  bridging) +744 → **+74 ms** vs truth P1.
  - **`shaft.onsetReturnBoxPx`** (**7.0**, frozen ON; **0 = veto OFF**, the `swLow<=0` dark idiom).
    Revisit radius in px; the 6–8 px sweep window validated 7.
  - **`shaft.onsetReturnGapFrames`** (15) — forward exclusion (~100 ms @150 fps) before a revisit
    counts, so a frame isn't "revisited" by its own dwell. Coupling: a slow one-piece creep advances
    `creepSpeed × gap` px per window, so the box must stay below that to not clip it (box 7 / gap 15
    tolerates creep ≥ ~0.6 px/f).
  - The scan horizon is `bs0` — the selected (post-bridging) run start — so the revisit test never sees
    the top dwell (which revisits itself); bridging is what makes `bs0` the true takeaway run on
    fragmented backswings, and the A3 clamp is the backstop for an unbridged mis-pick.
  - **The boundary also floors the Address walk-back** (no extra key — it rides the box flag). The
    boundary is published as `PhaseModel::onsetFloor` (= `min(boundary, final bs0)`, so an A3 clamp
    that pushes the onset earlier keeps the floor reachable) and `addressHoldEndFrame` never considers
    frames below it; when nothing in `[floor, bs0]` passes the absolute `stillAt` thresholds — the
    normal case, since real settles floor at 2–4 px/f — the answer is the floor itself, the last
    settle. Without this, a corrected bs0 still yielded a 0.5–1.5 s-early Address (round-2 studio
    result: the walk-back skipped through the fidget to the deep pre-fidget hold on 15/17 swings).
  - Side effect (gated): `estimateSwingSpanUs` shares `segmentPhases`, so the veto also tightens the
    pose/shaft span bound on fidget swings.
- **`shaft.onsetRunBridgeFrames`** (**10**, frozen ON; **0 = OFF**, the legacy ranking). Merges
  min-length-qualified `>swSpd` runs separated by fewer than this many quiet frames before the
  two-longest ranking. A slow real backswing fragments into short bursts on the lerped-pose speed
  profile and loses the ranking to a follow-through fragment — `bs0` then lands at the top/downswing
  (w2s4: Takeaway/top/impact all mis-placed; bridging alone recovered +744 → +74 ms). Deliberately
  applied AFTER the ≥7-frame filter: letting sub-7 waggle bursts participate chains a fidget cluster
  into a false run that wins the race and disables the veto (observed on w2s6). Separate key from the
  veto so the evaluation can separate their effects.
- **`shaft.onsetBridgeMinNetFrac`** (**0.2**, FROZEN ON 2026-07-18; **0 = m3gate OFF**, the legacy
  ranking). The chain-qualified net-displacement gate on the two-longest ranking: a bridged run
  assembled from **≥3 raw runs** enters the ranking only if its smoothed net displacement ≥ this
  fraction of its raw path length; the gate falls back to the ungated ranking if it would empty the
  candidate list. Kills the s0002-class presentation-move mis-pick, where grip-anchor pose **flapping**
  produced seven 7–8-frame oscillation runs that bridged into a 79-frame going-nowhere chain (net/path
  **0.013**), tied the downswing for two-longest and pinned Takeaway at the A3 far edge. Freeze
  evidence: 17-swing truth — s0002 Takeaway 1.857 → 2.480 s (**+0.100** vs truth), s0001 Address →
  **+0.042**, the other 15 swings zero-movement; 61-swing corpus — 19 corrective moves, 0 score
  changes. The ≥3-chain qualifier is a fixed structural rule, not a knob — **m=2 merges are the frozen
  w2s4 evidence** (fragmented-backswing rescue; the reversal-containing downswing+follow-through merge
  legitimately nets only 0.08×path) and are permanently exempt. Separation margin: flap 0.013 vs ≥0.34
  for every legitimate chain (25×; Phase-0 dumps, 2026-07-18). s0002's remaining **Address** residual
  (−0.58 s) is accepted pending the upstream grip-anchor flapping fix (recorded in the ShaftV3Config
  contract).
- **`shaft.emitTakeaway`** (**true**, frozen ON; **false = OFF**; W2). When on, `phasesToSegmentation`
  emits an additive vision **Takeaway** event at `bs0` (the motion onset); the ladder becomes
  `{Address, Takeaway, Top, Impact, Finish}`. Address stays on the hold-end / `addressFrame` path, and
  `Address ≤ Takeaway` structurally. Separate key from the veto — disjoint failure modes: onset
  **placement** vs event-**set** change.
  *(Historical note: this key originally existed so SwingLab's `seg.tempo_ratio` — then defined
  Top−Takeaway / Impact−Top — could evaluate on camera swings. As of 2026-07-21 that check, and the
  shipped `tempoRatio` metric, are both **Address**-based, so the Takeaway event no longer gates it.
  The event remains valuable in its own right as a ladder rung.)*

### 2.11 `ball.clubActivity` / `positions.p1ClubQuietSigma` / `ball.tk0AddressOverride` — club-bob detector (C1)
W1's onset veto and W3 attack the same estimand from different signals: W1 is blind to a **pure club bob
about a frozen grip** (the grip-only stillness test can't see the club rotating while the wrist is
still). W3 supplies the only 150 Hz signal that covers the address reach-back — the frames BallRunner
already decodes — as a **club-corridor activity** trace, then uses it to corroborate the address hold.
Activity is **FROZEN ON 2026-07-18** (the V1 EventRefine evidence freeze — it is the load-bearing
Tier-B refine input; live cost ballMs +207 ms median on the corpus run). Setting
`ball.clubActivity=false` still darks it out completely (byte- AND code-path-identical — the soak
baseline); the tk0 override was **FROZEN OFF 2026-07-17** (part of the Address/Takeaway freeze — see
§2.10). Scope: activity is only produced by the offline BallRunner replay, so W3 fires on
**analysis-replay swings**, not live-recorded ball tracks (the live-detector twin is future work).

- **Producer — `ball.clubActivity`** (**true**, frozen on 2026-07-18; `false` = OFF; `BallRunner::run`,
  frozen in `pp_tuned_constants.h` `ball::activity`). When on, BallRunner keeps an 8-bit gray ROI crop per frame and, after the tracker
  locks, computes `act = mean(|crop − medRef|) / σ` over an **annulus** around the ball centre:
  - **`ball.activityInnerR`** (1.5) — inner radius (× ball r); **excludes the ball disc** so ball-lock
    jitter isn't read as activity.
  - **`ball.activityOuterR`** (5.0) — outer radius (× ball r); covers the resting clubhead beside the ball.
  - **`ball.activityRefFrames`** (9) — `medRef` is the per-pixel temporal **median** of the previous this
    many crops (a bob dwells at its travel extremes, so a median reference beats a raw frame-diff). σ is
    the crop's `robustNoise` (exposure normalisation). The first `refFrames` frames, and any frame where
    the ball isn't found, get activity `-1` (absent). Persisted as an additive `"act"` per-sample field in
    swing.json's ball block **only when ≥ 0** (dark ⇒ absent ⇒ byte-identical); the annulus/median math is
    factored into the unit-tested `ball_activity.h` helper.
- **Consumer — `positions.p1ClubQuietSigma`** (3.0; `PositionsConfig`, `shaft_positions.h`
  `addressHoldEndFrame`). A frame is **club-quiet** when its nearest ball sample's activity is present and
  `< p1ClubQuietSigma`. `addressHoldEndFrame` gains an optional `clubQuiet` mask (nullptr ⇒ every existing
  caller byte-identical); a frame counts as the hold end only if grip-still **and** its whole trailing
  window is club-quiet. **Two-tier fallback** (mirrors the `baByFrame` corroboration): if nothing passes
  still+quiet, the grip-still-only answer stands — the mask can only move the hold-end to a BETTER
  (also-quiet) frame, **never degrade below today**. The call site builds the mask only when a **majority
  (≥ 50%)** of pre-`bs0` frames carry activity — else it passes nullptr (live tracks, dark runs, ball
  never found), keeping legacy behaviour.
- **Two-consumer contract** (widened from single when EventRefine landed): `BallSample2D.clubActivity`
  feeds **only** the named pair — (1) this mask and (2) the EventRefine Tier-B at-ball gate
  (`refine.activityQuietSigma`, `event_refine.h`) — never tk0, length, launch, or DP evidence
  (`ball_anchor_test` asserts `applyBallAnchor` output is invariant to the field).
- **`ball.tk0AddressOverride`** (**false**, FROZEN OFF 2026-07-17; W4, `applyBallAnchor`). The
  earliest-departure `tk0` fires on the **first fidget departure** and overwrote a good hold-end
  Address (w2s4: −0.134 s → −1.533 s; the freeze evidence set had Address-error median 0.564 → 0.060 s
  with this off). Set `true` to restore the old overwrite for A/B comparison (`tk0` is computed either
  way). Long-term `tk0` is conceptually the **Takeaway** instant, not the Address hold end — the
  re-scope remains future work (see the `ball_anchor.cpp` TODO and plan §"Out of scope").

### 2.12 `refine.*` — late-pipeline timeline-event refinement (C1)
`EventRefineConfig` (`event_refine.h`; the engine is `refineEvents`, the glue is the file-local
`EventRefineStage` in `wrist_analyzer.cpp`, slotted between RequireProducts and BindDetail). Fine-tunes
the Takeaway/Address events users see from the FINISHED shaft/ball products — three at-ball evidence
tiers (A measured θ-vs-θ_ball / B club activity / C grip radius), a last-departure/no-return takeaway,
and an `addressHoldEndFrame` re-walk from it. Never refines Impact; never inserts events; abstains
below `refine.minConf` or beyond `refine.maxShiftS`. **FROZEN ON 2026-07-18** (V1 evidence freeze,
paired with `ball.clubActivity`): 17-swing truth A/B — median |p1 err| held 0.052 s, max 0.577 →
0.145 s, within-100ms 12 → 14, zero regressions at `minConf` 0.8 (frozen 0.5 → 0.8); 61-swing corpus —
3 movers, 0 score changes. `refine.enabled=false` restores the byte- AND code-path-identical
pre-refine ladder (the soak baseline). Keys: `enabled` (true) / `takeaway` (true) / `address` (true) /
`impactResidual` (true, log-only) / `departThetaDeg` (25) / `activityQuietSigma` (3.0, seeded from
`positions.p1ClubQuietSigma`) / `returnHoldMs` (200) / `minConf` (0.8) / `maxShiftS` (3.0).

### 2.13 `tempo.*` — tempo metrics (2026-07-21)
`TempoConfig` (`tempo_metrics.h`; the engine is `buildTempoSeries`, the glue is the file-local
`TempoStage` in `wrist_analyzer.cpp`, slotted between FootMetrics and Kinematics). Emits
`tempoBackswing` (Address→Top, s) and `tempoRatio` ((Top−Address)/(Impact−Top)) into
**detail->series** — unscored, so the resemblance score is untouched. **Basis is ADDRESS→Top**, not
the Takeaway→Top of the published tour figures; SwingLab's `seg.tempo_ratio` was realigned to match
(2026-07-21) so the harness and the shipped number agree.

Refuses rather than approximates: no series at all when `seg.conf <= minConf`, or when any of
Address/Top/Impact is missing (the IMU `clampFallback` ladder has **no Top**). Every emitted series
carries a propagated 1σ in `MetricSeries::sigma` — Top appears in both halves of the ratio with
opposite sign, so its error is doubly leveraged (a 30 ms Top error ≈ 15 % of the ratio) and
real-capture Top error has **never been measured** (the ≤30 ms `truth.event_top_s` target exists; no
result does). Confidence widens σ, never moves the value. Keys: `enabled` (true) / `minConf` (0.0) /
`baseSigmaS` (0.020) / `confInflate` (1.0). `tempo.enabled=false` is the OFF-parity path.

### 2.14 `ballpos.*` — ball position at address (2026-07-21)
`BallPositionConfig` (`ball_position.h`; `computeBallPosition`, called from `FootMetricsStage`).
Produces two independent things: the `ballPosition` metric (ball along the lead-heel→trail-heel line
as a % of stance width — a ratio of two same-plane distances, so scale-free and comparable across
captures) and the **ball-diameter px→mm ruler** that puts `stanceWidth` into real millimetres
(`kGolfBallDiameterMm / (2·radiusPx)`, `src/Core/pp_physical_constants.h`). The ruler resolves
independently of the heel geometry, and vice versa.

Robustness comes from a component-wise median over the address window plus a cluster gate — the same
order-independent construction as `ball_anchor`'s `medianGripBallLenPx` pass 1, for the same reason (a
leading detector mis-lock must not veto every later good sample). Refuses on too-few samples or an
implausible position. Keys: `enabled` (true) / `addrWindowUs` (250000) / `minSamples` (3) /
`maxJumpPx` (40.0) / `fracLo` (−0.5) / `fracHi` (1.5). `ballpos.enabled=false` is the OFF-parity path
and reverts `stanceWidth` to `×frame` as well as dropping `ballPosition`.

⚠ **mm stance width is an estimate, not a calibration.** The ruler is exact only at the ball's
ground-plane depth (face-on, essentially the feet's depth — so it is the right ruler here), but it
rests on a ~9.5 px radius measurement: ±1 px of radius is ~10 % of scale. No corridor has been set for
`stanceWidth` because no measured distribution exists yet.

### 2.15 `upperBody.*` / `bodyRotation.*` / `clubDelivery.*` — the face-on producer batch (2026-08-02)

Three new namespaces, all in `src/Core/pp_tuned_constants.h`, all swept without a rebuild.

**`upperBody.*`** (`UpperBodyConfig`, `upper_body_metrics.h`). Nine frontal-plane chest / shoulder /
arm channels. Keys: `confMin` (0.30) / `addrMinFrames` (5) / `addrWindowUs` (250000) /
`minShoulderSpanPx` (30.0). The first three deliberately mirror `foot.*`, `head.*` and `lowerBody.*`
exactly — a fourth set of defaults for the same conf-gate / address-window job would be four things
to sweep and one thing to reason about. `minShoulderSpanPx` guards the DENOMINATOR the same way
`lowerBody.minStanceSpanPx` does, and is smaller (30 vs 40 px) because the shoulder span really is
narrower than the stance in the same framing; reusing the stance floor would refuse usable swings
rather than absurd ones.

**`bodyRotation.*`** (`BodyRotationConfig`, `body_rotation.h`). Keys: `confMin` (0.30) /
`addrMinFrames` (5) / `addrWindowUs` (250000) / `minSpanPx` (30.0) / **`spanNoisePx` (3.0)** /
**`sinFloor` (0.0872 = sin 5°)**.

⚠ **The last two are an error budget, not a fudge factor, and they are the most consequential knobs
in this batch.** The camera tier inverts a cosine, so `dθ/dw = −1/(w₀·sin θ)` diverges as the body
squares up: the producer propagates `spanNoisePx` through that derivative into
`MetricSeries::sigma`, and `sinFloor` is what keeps the reported uncertainty finite instead of
infinite near zero turn. `spanNoisePx` is the pose endpoint jitter carried through a difference of
two endpoints — 3 px is a starting figure and **no measured distribution exists**. `sinFloor` caps
the reported sigma at roughly 11× the span noise expressed in radians. Sweeping either changes what
the app claims about its own confidence, so treat them as a validation target rather than a tuning
lever. See [`body_rotation_estimation.md`](../design/body_rotation_estimation.md) §4.

**`clubDelivery.*`** (`ClubDeliveryConfig`, `club_delivery.h`). Keys: `velHalfSpan` (2 samples) /
`lowPointWinUs` (60000) / `lowPointMinSamples` (5) / `headConfMin` (0.30). `velHalfSpan` is the
half-width of the centred difference the head velocity — and therefore `attackAngle` — is taken
over; one frame either side of a ~9 px head is mostly quantisation noise landing squarely on a
single-instant reading, so a few frames of span buys a usable angle at the cost of a little time
resolution. It has not been swept against truth.

⚠ **None of these three namespaces has been corpus-tuned.** Every default is a starting figure
chosen for shape, and the producers' unit tests pin their SIGNS and refusal gates rather than their
accuracy. Signs are what a synthetic track can pin exactly and a corpus cannot; accuracy is the
other way round, and is outstanding for all three.

### 2.16 `lowerBody.minHipSpanRatio` / `upperBody.minShoulderSpanRatio` / `upperBody.minElbowSpanPx` / `channel.*` — geometric validity gates (2026-09-04)

Five keys from the first phase of
[`metric_presentation_honesty.md`](../design/metric_presentation_honesty.md) §5.1. They add
ABSENCE, never a value: no persisted `value[]` moves, and on a swing where none of them fires no
`valid` array is emitted at all.

⚠ "Byte-identical" is the wrong word for the swings where one DOES fire, and it was used here in an
earlier draft. A masked instant emits no phase sample, so a confidence dropout wider than the bridge
allowance can remove a P1 or P7 reading that used to be emitted — from any channel, including the
four that predate this work. The corpus gate is therefore `value[]` equality plus an accounting of
every removed sample, not whole-file parity.

**`lowerBody.minHipSpanRatio` (0.40)** and **`upperBody.minShoulderSpanRatio` (0.40)**
(`LowerBodyConfig` / `UpperBodyConfig`). The BODY LINES' foreshortening gate — the live |Δx| of a
line over its address |Δx|, both medians over the same address reference frames. A body-line tilt is
`atan2(dy, |dx|)`, so as the golfer turns out of the image plane the two ends collapse toward the
same image column, `dx → 0` and the angle runs to ±90° with nothing about the posture having
changed: a review chart on the 2026-08-18 corpus swing shows **−88° of hip line tilt** just after
impact and **+88° of shoulder plane at the top**, and the second of those is a GRADED phase sample.
Below the ratio the frame has no line, its sample is absent from the channel, and the resampled
series carries a 0 in `MetricSeries::valid` there.

⚠ These are NOT the existing `minStanceSpanPx` / `minShoulderSpanPx` floors, which guard a
percentage DENOMINATOR in pixels. These guard an ANGLE whose divisor is the live horizontal
separation, and the two fail on different frames. Nor is the shoulder ratio taken against
`UpperBodyReference::shoulderSpanPx`, which is the EUCLIDEAN span "% shoulder width" has always
meant: the gate needs |Δx| in the image, and a Euclidean length stays comfortably large while Δx
goes to zero.

0.40 of the address span is roughly 66° out of the image plane (`acos 0.4`), where a 2 px keypoint σ
on a 120 px address span is about 2.4° of angle error — the same order as the residual jitter. Below
it the error grows as 1/ratio and passes 10° by ratio 0.1.

⚠ **What the corpus says about 0.40, measured in phase 0.** The hip ratio at P1 has a median of
**1.000**, so the address denominator is clean and the lower-body gate is quiet where it matters.
The shoulder ratio **at P4 has a median of 0.32, with 55 of 83 measured swings below 0.40** — so on
most swings `shoulderPlaneAngle`, `spineSideBend` and `trailElbowHeight` have **no reading at the
top at all**. That is the geometry (a golfer who has turned 90° has no shoulder line in a face-on
image) and not a defect, but it removes a graded Top sample from the majority of the corpus, so the
0.40 default is a **product decision pending sign-off**, not a settled tuning value. Nobody should
read this gate as a rare event.

`lowerBody.minHipSpanRatio` gates `hipLineTilt` **and** the hip half of `spineSideBend` in the
upper-body module: one ratio for one geometric question about one line, wherever it is asked.

⚠ **One ratio, two references.** Sharing the key does not make the two modules agree frame by frame.
They are separate analysis stages with no shared result, so each resolves the hip line against its
OWN address reference frames and its own median denominator, and on a marginal frame they can
disagree. The upper module's reference admission test does not require the hips at all, so an address
whose hips were unconfident leaves it with no hip denominator — and then `spineSideBend` is absent
for the **whole swing** while `hipLineTilt` is still produced. That asymmetry is deliberate (a ratio
with no denominator is not a measurement, and taking one from mid-swing frames would put the
denominator inside the collapse it is meant to detect) and it is pinned by a test.

`upperBody.minShoulderSpanRatio`
gates `shoulderPlaneAngle`, the shoulder half of `spineSideBend` (which needs BOTH lines) and
`trailElbowHeight`.

**`upperBody.minElbowSpanPx` (25.0)** is the elbow line's gate, and it is an ABSOLUTE PIXEL FLOOR
where the other two are ratios. That is a correction, not an inconsistency: a ratio needs an address
span that represents the line at its widest, and the elbows are at their **narrowest** at address —
the arms hang together and separate through the swing — so `|Δx| / address |Δx|` is ≈1 at address and
≥1 everywhere after it. The ratio form was written first and was **inert**: it could never fire, and
it read exactly 1.0 at address, which is precisely where `elbowAlignment` is read and where a 20 px
elbow separation is pure keypoint noise. 25 px is a few keypoint σ (≈2 px each); below it the tilt
error exceeds 10°. `UpperBodyReference` therefore carries no elbow entry at all.

`trailElbowHeight` is on the shoulder-ratio list although it is a height rather than a tilt, because
`heightAboveLine` interpolates the shoulder line's y at the elbow's x —
`lineY = a.y + (p.x − a.x)·(b.y − a.y)/dx` — and so divides by the same vanishing span. It is the
worse case of the two: an angle at least saturates at 90°, while a % shoulder width is unbounded.

**What is gated is exactly what divides by a line's live |Δx|**, audited channel by channel. Not
gated, with the divisor that makes each one safe: `secondaryAxisTilt` (the VERTICAL neck→pelvis
rise), `thoraxLateralDrift` (the Euclidean ankle-line length, scaled by the address ankle |Δx|),
`leadHandWidth` (the address lead-arm length), `leadUpperArmToChest` (the address Euclidean shoulder
span), `leadArmToTorso` (two Euclidean vector lengths), and on the lower-body side sway, lift, knee
drift, the plumb bob and `comOverLeadFoot` (the Euclidean stance-line length). Those are still
distorted by the projection of a turn — that is a phase-DOMAIN question answered in the metric
descriptor — but they are not divisions by a vanishing separation, and gating them would withhold
measurements that were actually made.

⚠ `feetAlignment` is the ONE ungated `lineTiltDeg`, and it is ungated by judgement rather than by
construction: it divides by the ankle line's live |Δx| exactly as the hip line does, but the feet
stay planted and the stance line does not turn out of the image plane during a swing, so the
denominator never collapses. If a corpus swing is ever found where it does — a full-finish pivot onto
the trail toe, say — it takes the same gate against the address ankle |Δx|, and the constant is
already there in `lowerBody.minHipSpanRatio`'s shape.

**`channel.maxBridgeUs` (60000)** and **`channel.bridgeSpacingFactor` (1.5)**
(`metric_channel.h channelValidityMask`, read by both body configs). Where a BRIDGE stops being a
measurement. The resample has always held at the ends and
coasted across gaps so the curve is continuous and never NaN, which is right for a renderer and
wrong for a reducer: PEAK, PK RATE and a phase sample taken on a bridged run are a confident reading
of a straight line the producer drew itself. A grid sample farther than this from any real channel
sample — or outside the channel's time extent, where the value is a constant hold rather than a
bridge — is still filled, and marked 0 in `MetricSeries::valid`. 60 ms keeps today's behaviour where
it was honest (a one- or two-frame confidence dropout at 150 fps still bridges silently). The mask is
**omitted entirely when every sample is valid**, the same discipline `sigma` follows — never an
all-ones array.

⚠ **THE BUDGET APPLIES ONLY TO CONFIDENCE HOLES, NEVER TO A GATED FRAME.** These two are different
statements and the first implementation conflated them, which the 11-swing gate caught: on
2026-08-18 Wrist_01 swing_0001 a 10-frame *gated* run in `hipLineTilt` at 7 ms spacing came back
flagged **valid**, because every frame of it sat within 60 ms of the last measurement — so the bridge
(−28…−13°, where the raw was −31…−78°) was drawn and graded as a reading. On swing_0003 that emitted
a P4 `shoulderPlaneAngle` of 25.8° taken from the bridge and changed `spineSideBend`'s P7 the same
way. That is precisely the fabrication design §4 principle 2 forbids.

- **The keypoints were unconfident** — the geometry was there and we did not see it. Holding across
  it is a hold, and the budget decides how long a hold stays honest.
- **The producer gated the frame** — the geometry was seen and refused. There is nothing to hold, so
  it is 0 whatever the spacing and whatever `maxBridgeUs` says.

`channelValidityMask` therefore takes the producer's list of *gated instants* alongside the channel's
measured ones, and both producers pass one per gated channel (`gatedHipLine`; `gatedShoulderLine`,
`gatedElbowLine`, `gatedSideBend`). A test puts a 10-frame gated run and a 10-frame confidence hole at
the same spacing in one track and asserts they come out opposite.

⚠ **A fixed budget is not enough, and that is what `bridgeSpacingFactor` is for.** The grid is not
uniformly sampled: `PoseRunner` poses every frame only inside the dense zone, and the address region
runs at `addressStride 15` (≈100 ms at 150 fps) or `coarseStride 12` (≈80 ms) — `pose_runner.h`. So a
single dropped sample there is 80–100 ms from its neighbours and a flat 60 ms would mark it, which is
the wrong answer: across one missing sample of a still, sparsely posed address, holding the previous
value is a hold, not a fabrication. (An earlier draft of this section justified 60 ms by citing
`sparseStride = 4` at 27 ms for the address region; that is the *dense-zone-adjacent* stride, not the
address one, and the justification was wrong.) The allowance is therefore
`max(maxBridgeUs, bridgeSpacingFactor × local grid spacing)`, where local spacing is the larger of the
two neighbour gaps so it does not collapse on the sparse side of a stride change. Mid-swing the
spacing is ≈8 ms and the 60 ms floor decides, so a genuinely gated run is still marked. At 1.5 a hole
of one or two missing samples still holds and the middle of a hole of three or more is marked.

⚠ **`channel.maxBridgeUs` negative is the documented OFF-SWITCH** — no mask at all, the pre-mask
behaviour, for a parity run. It is not simply passed through to the mask, because a negative budget
there would fail the distance test on every sample *including the measured ones* and hand back an
all-zeros mask, withdrawing every gated metric from every reducer at once. Both producers guard it and
both guards are pinned by a test.

**THE PHASE DOMAIN RIDES ON THE SAME MASK, and it is not a tunable — it is authored per metric.**
Design §5.1's domain table narrows ten channels to **Address→Impact**: `pelvisSway`, `pelvisLift`,
`leadKneeDrift`, `plumbBobDistance`, `hipLineTilt` in `lower_body_metrics.cpp`, and
`secondaryAxisTilt`, `spineSideBend`, `thoraxLateralDrift`, `shoulderPlaneAngle`, `elbowAlignment` in
`upper_body_metrics.cpp`. Past impact the pelvis and thorax have turned toward the target, so the
frontal-plane projection of a lateral quantity is measuring **rotation** — `MetricDescriptor::
stereoGain()`'s comment says it outright, "turning the pelvis moves the APPARENT hip centre sideways
with no sway at all" — and the +35 % sway step after impact in design §2's screenshot is that, not
noise. Since 2026-09-04 those producers mark **every grid sample past Impact invalid**
(`metric_channel.h applyPhaseDomainMask`), inclusive **at the nearest grid sample**
(`nearestIndex`, the same snap the chart's phase dots use, so the boundary sample the user sees is the
one the reducers keep). An unsegmented Impact is **unbounded**, which leaves the series untouched.
`value[]` does not move — the curve is still continuous, still drawn (dashed outside the domain) and
still hovers; only what may be *reduced* changes.

⚠ **THE DOMAIN'S OPEN SIDE IS THE HEAD, and that is not an oversight.** The first cut marked the
pre-Address samples too, and the 5-swing gate came back with `partial: true` on **every clamped
card**. Two reasons it stays open. (1) **The chart does not clip the start side.** A domain whose first
phase is Address *is* the default first phase, so `ChartMetrics::domainFor` reports
`firstNarrowed = false` and the card's window still begins at the series' first sample; marking the
head then puts the card's own start edge inside a masked run, `reduceAt` finds nothing within ±15 ms,
the edge falls back to interpolation and the card declares itself partial — for a head nobody was
reducing over. (2) **Those samples are honest.** The golfer is standing still and the reading is
referenced to the address frames themselves, so nothing is turned out of the image plane yet, and the
phase's own **still-address gate window is [Address − 300 ms, Address]** (§7 item 2) — it is measured
*entirely* on those samples, so marking them would withdraw the evidence for the number the phase is
judged by. The two-argument form of `applyPhaseDomainMask` still accepts a first phase for a future
metric whose domain genuinely starts later; no producer passes one.

⚠ **Why not in the reducers.** `reduceExtremum` deliberately does not clip its ±20 ms support to the
window it was asked about, because the diagnostics span cache and the whole-window card agree only
while a sample's windowed mean is query-independent (§2.17 carries W2's 20-of-514 measurement). So a
reducer cannot close a domain leak without breaking cache agreement, and a card that reduces a whole
window would leak anyway. Marking the samples closes it **once**, for every consumer and every query,
because outside its domain a sample has stopped being a measurement of that quantity — the same
statement a gated frame makes, by the same mechanism.

⚠ It is behind **the same off-switch**: `channel.maxBridgeUs` < 0 emits no validity mask at all,
domain included, because that knob exists to restore the pre-mask bytes for a parity run and half a
restoration is not one. And it is a **content change on swings that were never gated** — the four
channels that predate this work are among the ten — so the phase 2 corpus gate accounts for the
removed samples, not just for `value[]` equality.

### 2.17 `reduce.*` — the shared robust reducers (2026-09-04)

Four keys from the second phase of
[`metric_presentation_honesty.md`](../design/metric_presentation_honesty.md) §5.2, frozen in
`pinpoint::tuned::reduce` and consumed through `ReduceConfig` in
[`src/Analysis/series_reduce.h`](../../src/Analysis/series_reduce.h). They change nothing that is
persisted: `value[]` is untouched, no producer reads them. What they change is what is *derived* from
a curve — the review chart's summary card (`ChartMetrics::summary`) and the diagnostics phase grid
(`measure_sample.cpp buildPhaseGrid`), which from this phase on share one arithmetic instead of
carrying two. (`series_reduce.h` is std-only so that either consumer, a probe or a tool can use it;
the one function that knows about `MetricSeries` lives next door in `series_reduce_metric.h`.)

⚠ **NOT INJECTABLE YET.** There is no `fromOverrides` for these in this phase: both consumers
default-construct a `ReduceConfig` from the constants. The dotted keys below are the names the sweep
will use when the plumbing lands, and they are registered here now so that the freeze edit-point and
the key names are decided in one place rather than invented twice. Sweeping them today means editing
the header (§3).

**`reduce.extremumWindowUs` (40000)** — the support a PEAK has to have. `reduceExtremum` returns the
extremum not of the samples but of the **centred-window mean**: for every valid sample, the mean of
the valid samples within ±20 ms of it. A one-sample outlier therefore cannot *be* the peak, because a
peak has to be there for 40 ms. This is the direct fix for the raw-argmax behaviour design §2
measured — a summary card reporting a sway PEAK of 34 % on a curve that settles at 12 %, and a
`PhaseGridSpan.min/max` built from the same raw samples, so the diagnostics `Extremum` reducer
inherited the identical bias.

40 ms is ≈5 samples inside the dense pose zone (8 ms spacing) and ≈2 outside it (27 ms), so the
jitter averages down by about √5 where it is worst. It is short next to what it must not flatten:
the pelvis and thorax quantities this protects move over 100–300 ms. The dilution is arithmetic and
worth stating, because it bounds how wrong a spike can still make the answer — a single sample of
value *A* among *k* window samples otherwise bounded by *M* cannot push the mean above
`M + A/k`. On the test's ramp (0..8) with one 99 at 8 ms spacing that is `7.92 + 99/5 = 27.72`, and
the reducer returns **23.16** where the raw argmax returned **99**.

⚠ **[from, to] bounds the CANDIDATES, not the SUPPORT.** Only the anchors that may *win* have to lie
in the window; each anchor's ±20 ms mean draws on every valid sample near it, inside the window or
not. This went back and forth twice, so the argument is recorded: the diagnostics engine **caches** an
extreme per `(lo, hi]` span while the review card reduces a whole window in one call, and those two
can agree only if a sample's windowed mean is the same number whoever asked — i.e. only if the support
is query-independent. W2 measured the alternative on `rich_7iron`: with the support clipped to the
query, **20 of 514** authored extremum measures disagreed between the span cache and the whole-window
reduction; unclipped, **0 of 514**. Cache agreement is what design §5.2 exists for, so what the
reducer guarantees instead is the invariant the cache actually needs — the extreme over a **union** of
adjacent spans is the extreme *of* their extremes, pinned in `series_reduce_test` §8.

⚠ The price, recorded so nobody rediscovers it as a bug: on a rise-to-impact-then-reverse curve the
mean at a span's first anchor reaches back before the span, so a P6→P7 span whose samples run
18.4 → 20.0 reports a minimum of **17.92** — a value the curve never had inside it. **That is a phase
-domain leak and it is closed at the producer, not here** (§2.16, `applyPhaseDomainMask`): a sample
outside a metric's domain is marked INVALID, so it is not a measurement and no reducer at any query
can draw on it. Clipping the support would have hidden the leak for spans, left it wide open for every
whole-window card, and cost the cache agreement.

**`reduce.minExtremumSamples` (3)** — and this key exists *because* of that clamp plus the grid's
non-uniform sampling. At the 27 ms spacing outside the dense pose zone a ±20 ms centred window holds
exactly **one** sample, so the "windowed mean" was the sample itself and the extremum was the raw
argmax all over again — across the whole address and backswing, which is precisely where the still
-address PK RATE and PEAK numbers in design §2 were measured. The corpus probe's tell was
`peakSigma` printing **0.000** on every still-address row: a one-sample window has no residual, so it
cannot carry an uncertainty. The window therefore **widens symmetrically** — the same half-width both
sides, out to the next valid sample's distance — until it holds this many valid samples, or until the
series has no valid sample left to add. Widening follows the same rule as the window it grows — valid
samples wherever they are — which is what keeps a widened mean query-independent too, and it is
**inert wherever the grid is dense**: every 8 ms expectation in the test file is identical with the
floor at 1. 3 is the same floor `minRateSamples` uses, for the
same reason — it is the smallest window with a residual left over after a straight line.

**σ on the peak is the noise the MEAN carries, not the window's spread.** `reduceExtremum` reports
`sqrt(SSE/(k−2)) / sqrt(k)` where SSE is the window's residual about a **local straight line** —
0 for a clean ramp, ≈ σ/√k for a noisy still, and large when a spike is inside the window. The first
implementation reported the window's *sample standard deviation*, which reads a noiseless ramp as
**±0.08**: that is the curve's own motion across 40 ms printed beside the peak as if it were
measurement error, and design §5.3 renders this number as "± σ" on the card. Fewer than three samples
⇒ 0, because a two-point window is fitted exactly and has no residual to speak from.

**`reduce.rateWindowUs` (50000)** and **`reduce.minRateSamples` (3)** — the minimum **time base** a
rate may be fitted over, and the minimum evidence in it. `reduceRate` returns the largest-magnitude
**least-squares slope** over sliding windows `[t_i, t_i + 50 ms]`, per 100 ms as the card has always
reported it. The window is *extended* to the first valid sample at or beyond `t_i + 50 ms` when one
exists inside the reduction window, because outside the dense zone the spacing is 27 ms and a
literal 50 ms window holds two samples spanning 27 — fitting that would report a 50 ms rate taken
over half the time base. A window with fewer than `minRateSamples` valid samples, or a span still
short of `rateWindowUs`, is **skipped**; when none qualifies the answer is *no rate* (`rateOk: false`
on the card, `ok = false` in the reducer), never a fabricated number. That is what a sparsely posed
address — 80–100 ms strides — now returns. **Three is enforced in code whatever this key says**: a
two-point fit passes through both points, so its standard error is 0, and a rate of 39 per 100 ms
printed as "± 0" is a confident reading of two samples of noise.

What this replaces is the largest **adjacent-frame** |Δv/Δt| scaled to 100 ms, and the arithmetic of
why that had to go is design §2's whole table: at 8 ms spacing, half a degree of jitter reads as
6°/100 ms and the 95th-percentile 3° reads as 37°/100 ms, which is where the screenshot's 291°/100 ms
of hip-line tilt came from. On a still series with 0.2 units of noise the fitted slope comes back at
**1.1 per 100 ms** against the adjacent-frame **9.9**; the phase's definition of done (PK RATE under
2 units per 100 ms over a still address window) is a statement about this reducer on that noise.

⚠ **A LEAST-SQUARES SLOPE IS NOT A ROBUST ESTIMATOR, and no value of these two keys makes it one.**
The 50 ms base removes the "noise ÷ 8 ms" failure completely, but a single large outlier still moves
the fit: an outlier of magnitude *A* in a window of *n* samples spanning *T* seconds displaces the
slope by roughly `6A/(nT)`. The test's 99-unit spike on an 8-unit ramp reads **≈ 100 per 100 ms**
against the ramp's true 1.0 — 11.9× better than the adjacent-frame **1188**, and still not the
truth. Widening the window does not fix it (the displacement falls only as 1/nT while a real
excursion is flattened); what makes the *presentation* honest is that the fit's own standard error
comes back the same order as the slope — ≈ 57 against ≈ 100 — which is the `± σ` design §5.3 puts
beside PK RATE. **If a spike-proof rate is ever required, the answer is a robust regression
(Theil–Sen over the window's pairwise slopes) or a residual gate, and it is a design decision, not a
tuning of these keys.** Nobody should read `reduce.rateWindowUs` as a spike filter.

**Three properties of all four reducers that are not keys but are pinned by tests**, because each one
was a defect before it was a rule: ties go to the **earliest** window and need a relative margin to
do so (a clean ramp's windows have identical slopes to the last bit, and a bare `>` handed the
reported `atUs` — a dot on the chart — to whichever anchor accumulated 1e-16 higher); a **non-finite**
sample is ignored exactly as an invalid one is (a NaN reaching `std::nth_element` is undefined
behaviour, and one reaching a mean returns a confident `ok = true` holding NaN); and **coincident
timestamps** are tolerated but double-weighted in every reduction, which is the right reading of two
measurements sharing a clock tick and the wrong reading of a duplicated row — no producer emits one,
so it is stated rather than defended against.

Cost, since these run per metric per span: At and Rate are O(n·w) in the samples and the window
occupancy (w ≈ 5 dense, ≈ 3 sparse); Extremum is O(m²) in the **in-range** count, because symmetric
widening has to know how far away the next sample outside the window is, and m is a span's own
sample count — a handful — or the whole series for the chart's one full-range call.

The At reducer takes no new key: it is `sampler.windowHalfUs` (±15 ms) and
`sampler.minValidSamples`, the convention `measure_sample.cpp` has used since it was written. §2.4's
entry covers them. The chart adopting that median in place of a linear interpolation at one instant
is the other half of "the card and the engine cannot disagree", and it is a behaviour change on the
chart only.

### 2.18 `poseSmooth.legsSigmaScale` / `poseSmooth.legsJerkScale` — the legs smoother window (2026-09-05)

Two keys from phase 4 of [`metric_presentation_honesty.md`](../design/metric_presentation_honesty.md)
§5.4. They are the fourth per-group scale on the offline RTS pose smoother
(`PoseSmootherConfig`, `src/Analysis/pose_smoother.{h,cpp}`), over the **COCO body keypoints 11–16**
— left/right hip, knee and ankle. `legsSigmaScale` multiplies both measurement-σ constants
(`measSigBasePx` **and** `measSigSlopePx`); `legsJerkScale` multiplies `sigmaJerk`. They are applied
exactly as the existing feet/face/hand tail scales are, in the same `for (k < kWholeBodyJoints)`
branch chain, and they touch **no other keypoint**: 0–10 and 17–132 keep the frozen constants, which
a test pins by byte-comparing every kp/conf/tier/σ of every frame.

**Both ship at 1.0 and phase 4.1 changes no default.** ×1.0 is exact in IEEE-754, so every keypoint
of every frame — and therefore every persisted `value[]` — is byte-identical to the pre-phase-4 tree.

**Why the group exists.** One `sigmaJerk` (2.0e5 px/s³) is shared by every body keypoint and it was
tuned on a **wrist**: the derivation block in `pose_smoother.cpp` measures its effective smoothing
window at **≈33 ms at 150 fps** (≈42 ms at 30 fps — a fixed σ_jerk with variable dt auto-adapts). A
hip does nothing at that timescale, so the hips inherit the wrist's window and every hip-derived
series carries keypoint noise the smoother could have averaged away. On the motivating swing that is
a p95 frame-to-frame jitter of 3.1° on `hipLineTilt` and a whole-swing PK RATE of 291°/100 ms. The
design targets **80–100 ms** on the hips.

**The window as a function of the scale.** Treating the filter as its steady-state Wiener equivalent
— a 3rd-order integrated-white-noise signal (spectrum q/ω⁶) observed with noise density σ_m²·dt —
gives a cutoff at `ω_c = (σ_jerk²/(σ_m²·dt))^(1/6)`, so

* the window `T = 1/ω_c` scales as **σ_jerk^(−1/3)** at fixed dt, and as dt^(1/6) at fixed σ_jerk;
* on a **stationary** point, where the residual is pure noise averaging (`σ_out = σ_in/√(T/dt)`), the
  residual σ scales as **σ_jerk^(+1/6)**.

So `legsJerkScale = 0.1` predicts a window ×2.15 (**≈71 ms** at 150 fps) and a residual σ ×0.681;
`0.05` predicts ×2.71 (**≈90 ms**) and ×0.61. The law reproduces the derivation block's measured
1e5→2e5 step to within 1 % and its 150→30 fps step to within 3 %, and overestimates the 3e5 window by
≈12 % — it is a **chooser of sweep points, not a measurement**. `legWindowMsForJerkScale(scale)` in
`pose_smoother.h` is that arithmetic, and `pose_smoother_test.cpp` asserts both the 0.681 residual
ratio (within 25 %) and that a 0.5 Hz / 40 px hip excursion — the order of a P1→P4 sway — keeps its
amplitude within 5 % at scale 0.1.

⚠ **`legsSigmaScale` is registered, not motivated.** The design asks only for the window, and the
window is `legsJerkScale`. The σ scale is here because the two are the same knob viewed from either
side (the cutoff depends on σ_jerk²/σ_m², so raising σ_m widens the window too, but it *also* widens
the 3σ gate and the published posterior σ, which the jerk scale does not) and because a sweep that
can move only one of them cannot tell those apart. Nothing has yet been measured with it at anything
but 1.0.

⚠ **No shoulder/thorax scale, deliberately.** The shoulders move fast through transition and the
wrist-tuned window is nearer right for them; the design's words are "measure before assuming".

⚠ **The rest of `PoseSmootherConfig` is deliberately unreachable from a sweep.** `fromOverrides`
applies these two keys and nothing else. The frozen fields were validated *together* — σ_jerk was
nudged up from the pure best-window value for 3σ-gate robustness on fast joints — and a sweep that
moved one of them alone would break that balance silently.

**Promotion is phase 4.3 and it is Mark's decision, not a sweep's.** The gate is a corpus
before/after **with a control run** (pose is non-deterministic: ~20 metrics differ at 1e-14 between
identical runs, so a diff without a control attributes nothing), judged on residual jitter of the hip
series, the amplitude of the P1→P4 sway excursion (it must not shrink beyond the control's spread) and
the P4/P7 phase samples (they must move by less than σ). It is the only change in this design that
moves persisted `value[]`, and the P4 corridor content was seeded on the current smoothing, so a
systematic shift at P4 puts [`norm_shapes.md`](../design/norm_shapes.md) in scope rather than being
absorbed silently.

### 2.19 `poseSmooth.adapt.*` — the motion-adaptive smoother window (**PROMOTED** 2026-09-05)

Eight keys from phase 5 of [`metric_presentation_honesty.md`](../design/metric_presentation_honesty.md)
§5.4, on the same offline RTS smoother as §2.18. They exist because **§2.18's static scale was
measured and failed on its own terms**: at `legsJerkScale` 0.1 the hip jitter falls exactly as the
window law predicts, but the **P7 (impact) samples move 3–4 σ**, because the hips move fast through
impact, a ≈70 ms window blends the post-impact rotation into the impact frame, and the corridors are
seeded at P7. A window that is long while the joint is quiet and back to today's while it accelerates
is the honest version of the same idea.

**PROMOTED 2026-09-05 on the 11-swing subset — this moves persisted `value[]`.** The C15 gate ran 17
settings × 11 swings against a control; 6 passed every criterion; the shipped row is
`accel` / `legs` / `aRefPxS2 4000` / `expo 8` / `minScale 0.01` / `leadMs 20`:

| criterion | threshold | this row |
|---|---|---|
| (2) ΔP4 vs control | median < 1 σ, max < 2 σ | **0.12 / 0.34 σ** |
| (2) ΔP7 vs control | median < 1 σ, max < 2 σ | **0.10 / 0.38 σ** (`hipLineTilt` P7 **0.00**) |
| (3) P1–P7 excursion ratio | ±3 % | **0.997–1.004** |
| (4) still-address p95 jitter | ≥ 20 % reduction on the three named series | **×0.71 sway, ×0.71 hipLineTilt, ×0.52 plumbBob** (35.3 % gain) |
| (5) σ ratio inside the domain | < 1 | **0.89–0.99** |
| samples lost / guard fallbacks | 0 | **0 / 0** |

`innov` scored a comparable 36.8 % jitter gain and **lost on margin**, which the record should keep:
`pelvisLift` P7 moved up to **1.50 σ**, its excursion ratios ran **0.975–0.99**, and its σ shrank
**10–21 %** across the domain — it bought jitter by moving the readings — and it has **no divergence
guard** (one forward pass, no reference to compare against). ⚠ **The gate was the 11-swing subset; a
full-corpus confirmation is a follow-up.**

⚠ **The parity switch is `poseSmooth.adapt.mode=off`**, not an empty override map: with the window off
no scale vector is built at all, so the output is byte-identical to the pre-phase-5 tree by
construction. That is what a parity run against this feature has to use, and a test pins it with a
hand-built config.

| key | default | meaning |
|---|---|---|
| `poseSmooth.adapt.mode` | `"accel"` | `off` \| `accel` \| `innov` — the policy. **Promoted to `accel`**; unrecognised reads as `off`, i.e. a typo'd sweep line runs as a *control*. |
| `poseSmooth.adapt.group` | `"legs"` | `legs` = kp 11–16, `body` = kp 0–16. The wholebody tail (17+) **never** adapts. Unrecognised reads as `legs`. |
| `poseSmooth.adapt.minScale` | `0.01` | clamp floor on the scale (swept; ⇒ window ×2.154, 33 → 71 ms at 150 fps). **Clamped to [0, 1]** on read: > 1 would mean a *shorter* window than today's everywhere, which is a different experiment. |
| `poseSmooth.adapt.aRefPxS2` | `4000.0` | \|a\| that maps to scale 1.0, **px/s² at the 1280×1024 reference format** |
| `poseSmooth.adapt.expo` | `8.0` | contrast: `s = (\|a\|/aRef)^expo`. Swept to 8, which makes it a **near-switch** — floor below `minScale^(1/expo)` = 0.562·aRef (2249 px/s², just above the 2172 still-address p95), clamped to 1.0 at aRef. That sharpness is why the winning row moves P4/P7 so little; a lower expo spreads the transition across P4–P6, which is what moved samples in the losing rows. Unbounded on purpose. |
| `poseSmooth.adapt.leadMs` | `20.0` | symmetric ±max filter on the scale vector, **as a duration** — see the cadence note |
| `poseSmooth.adapt.innovRef` | `4.0` | innov policy divisor on the normalised innovation |
| `poseSmooth.adapt.innovRun` | `3` | innov policy window, in **accepted** steps. **Capped to [1, 32]** on read. |

**The mechanism is one multiplier.** `Kf3::predict(dt, qScale)` scales the process-noise variance
`q = σ_jerk²` for the transition INTO one frame. The 3σ gate, the coast budget, the segmentation, the
confirmed-run marking and the §2.18 static scales are untouched, and nothing new is persisted — the
RTS pass needed no change at all because it already reads each step's **stored** predicted covariance
and dt. `mode "off"` ⇒ no scale vector is built anywhere, so the output is byte-identical to the
pre-phase-5 tree **by construction**, not by an arithmetic identity (a test pins that with every other
adapt field set to a wild value).

⚠ **The scale is on q, so the window law's exponents halve.** §2.18's scales multiply σ_jerk (window
∝ σ_jerk^(−1/3), stationary residual σ ∝ σ_jerk^(+1/6)). A q scale `s` is a σ_jerk scale of `√s`, so
in q terms **window ∝ s^(−1/6)** and **residual σ ∝ s^(+1/12)**. So the shipped `minScale = 0.01` is a
window **×2.154** (33 → **71 ms** at 150 fps) and a stationary residual σ **×0.681** — the 80–100 ms hip
window §5.4 asked for, but only where the hip is actually still — and *not* the ×4.64 / ×0.46 that the
same number would mean as a `legsJerkScale`. (The law fixture pins its own `minScale = 0.05` ⇒ 0.779 so
the assertion does not move when a sweep moves the default; it holds it to ±10 % **and** requires it to
be nearer the q reading than the σ_jerk one.) Do not read the two families of numbers off one table.

**`aRefPxS2` is a per-FORMAT number, scaled by the GEOMETRIC MEAN of the two axes.** \|a\| is a pixel
quantity, so the same hip motion filmed smaller reads fewer px/s² and a fixed threshold would score it
quiet. The run uses

    aRefEff = aRefPxS2 × sqrt(frameW·frameH / (1280·1024))

⚠ Not `frameW` alone: the corpus's own other format is **720×1024 — the same height**, so a width-only
rule would move the threshold 44 % while a vertical motion's px/s² did not move at all. The geometric
mean splits that error between the axes. Measured in `pose_smoother_test.cpp` §17 on that format pair:

| motion | \|a\| ratio | mean rule (shipped) | width-only rule |
|---|---|---|---|
| horizontal (sway) | ×0.5625 | s ×**0.75** | s ×1.0 |
| vertical (lift) | ×1.0 | s ×**1.333** | s ×1.778 |

⚠ **Residual, documented not fixed:** \|a\| is one isotropic magnitude and this is one isotropic
threshold, so *no* single factor can undo a non-square pixel-scale change. The honest fix is per-axis
normalisation (two thresholds, or \|a\| normalised per axis before the hypot) and it needs a design
decision, not a constant. A similarity change (both axes ×k) **is** exact: §17 also scales a track and
every px-dimensioned filter constant by 1.5 and requires the scale vector to be unchanged.

**Where 4000 comes from — the IMPACT side, measured corpus-wide.** The requirement is `s == 1` through
**P6–P7 on every session**: that is the phase-4.2 failure this design exists to avoid. The address end
is a bonus. Over 83 corpus swings (hip centre, normalised to the reference format,
`tools/metrics/hip_accel_reference.py`):

* `min(|a| at P6, |a| at P7)`: **p05 5232 · p25 7355 · median 9978 px/s²** ⇒ **4000 holds s = 1 through
  P6–P7 on 80 of 83 swings**;
* **P4 median 4378**, so the top of the backswing saturates too;
* the 08-18 subset's **still-address p95 is 2172** ⇒ `s = 0.54` at `expo 1` (0.29 at `expo 2`, 0.16 at
  `expo 3`) — the address hold sits well down the curve without the impact end moving;
* the July sessions' addresses are **not still** (P1 p95 up to 21 k) and read as motion. The policy
  declining to engage there is the correct answer, not a miss.

**20000 would leave P7 at ≈ 0.69** — the phase-4 failure re-created at the one instant that matters — so
this must not be raised without re-measuring the P6/P7 ladder.

⚠ **One group aRef is not one joint's aRef.** The lead **ankle's** ladder runs at about half the hips'
level (P7 median 6912), so at a group-wide 4000 it is **un-saturated at P6/P7 on ≈23 % of swings**. If an
ankle-derived series fails the gate the fix is a **per-joint aRef** — lowering the group number to suit
the ankle would drag the hips' impact window back into the phase-4 failure.

⚠ **The two hips get two scales.** The policy is per keypoint (each reads its own \|a\|), so a
hip-line metric mixes two points whose windows can differ by a frame or two of lead. Both track the
same motion, so the effect is second-order, but a series that reads a *pair* is where it would show.

**The `innov` policy is registered to be rejected or kept on evidence, not recommended.** `innov²/S`
is the 3σ gate's own statistic, and for a *consistent* filter it is χ²(1) with mean 1 **whatever the
joint is doing**; the part of a real trajectory a constant-acceleration predictor cannot see over one
dense step is ~`jerk·dt³/6` — 0.03 px for a 4 Hz 40 px hip excursion at 150 fps, against σ_m ≈ 3.2 px.
So at dense sampling it is a noise-driven window, not a motion-driven one (the shipped `innovRef` 4.0
sits at the 2σ point of that noise, ≈5 % of steps). Its virtue is that it is exactly one forward pass.
The tests pin that it is wired to its knob (an unreachable `innovRef` pins the floor and reproduces the
window law; a zero `innovRef` saturates at 1.0 and is then byte-identical to `mode off`) and state
plainly which tolerances had to widen and why.

**The divergence guard: the adaptive window never removes a sample.** Pass 2 re-decides segmentation
from scratch, and a smaller q shrinks `Pp`, which shrinks `S`, which **tightens the 3σ gate**. At the
shipped `minScale` that is negligible; at the sweep grid's 0.0025 — σ_jerk a full 10× below the collapse
knee the `.cpp`'s derivation block measured — it is not. So after pass 2 the run compares the two
passes' `accepted[]` and `hasSmoothed[]` for that keypoint and, on **any** difference, keeps **pass 1's
(unadapted) output** and counts the keypoint. The count rides out as
**`analysis.pose2d.adaptFallbacks`** (written only when > 0, so the window-off default stays
byte-identical) via `PoseTrack2D::adaptFallbacks`. **A non-zero count is a rejected sweep setting, not a
warning** — it means the setting would have changed which samples exist. The innov policy has no
reference pass by design (one forward pass is its whole virtue), so it is **not** guarded; that is a
known gap, and a reason to prefer `accel` if both pass the gate.

⚠ **The guard as specified is sensitive, and that is a live question for promotion.** It falls back on
**any** change to `accepted[]`, and a reduced q tightens the 3σ radius by ≈10 % at a `minScale` of 0.05 —
enough that on a 3 px white synthetic still track one borderline sample in ~750 flips its accept flag and
the whole keypoint is handed back unadapted. Three test fixtures therefore widen `gateSig` to 6 so they
measure what they claim to (the window law, group selection, the no-measurement rule) rather than the
guard, and every one of them asserts `adaptFallbacks == 0`. The distinction worth drawing before
promotion: a flipped accept flag makes a sample a **coast the RTS still bridges** (`hasSmoothed` stays
true, the sample still exists), whereas a collapsed segment **removes** samples. If the corpus shows the
count firing on ordinary swings, the fix is to gate the fallback on `hasSmoothed` — the actual "never
removes a sample" invariant — and to count accept-flag flips separately rather than to loosen the rule.

**A step with no measurement never gets the reduced q.** A coasted step is the filter guessing, and its
posterior σ is the only honest statement of that; shrinking q there would shrink σ without adding
information, so a bridged sample would claim to be *more* certain than the measured samples either side
of it. Both policies force scale 1.0 on any step without a measurement.

**`accel` is two passes and that is deliberate.** Pass 1 is the ordinary smoother for the keypoint,
read only for its RTS acceleration; pass 2 re-runs it with the derived per-frame scale. The
acceleration estimate has to be **non-causal** — a causal one learns about the impact after the
corridor has read it — and the symmetric ±`leadMs` max filter is the same argument one derivative
down: the window must already be short *before* the acceleration arrives. Frames with no smoothed
value (outside every segment, or a trimmed coast tail) carry 1.0, so a segment break widens back to
today's window instead of inheriting the address one.

⚠ **The quiet/moving separation is partly the POSE CADENCE, and partly the noise colour.**
`PoseRunner` does not pose the address hold densely: the address region sits on the coarse/sparse
grid (the corpus's measured spacing there is ≈27 ms) and the dense ≈6.7 ms zone only opens ≈500 ms
before impact (`pose_runner.h`'s stride set — `addressStride 15` / `coarseStride 12` for the padded
address). Two consequences, and the second is the one to remember:

* the filter's own window grows as dt^(1/6), so the |a| noise floor rises only as dt^(1/12) — the
  coarse grid is worth ≈12 % and nothing more;
* **the lead is `leadMs`, a DURATION, exactly because of this grid.** A frame count would have meant
  ±81 ms out at the 27 ms address grid and ±20 ms in the dense zone — widest where the lead is least
  needed, narrowest where it protects the corridor samples. The flip side, so nobody rediscovers it as
  a bug: at a 27 ms grid **±20 ms reaches no neighbour**, so the max filter is a no-op out at the
  address and only bites in the dense zone. A **densely posed address still reads louder**, because
  there the max runs over several independent samples of the noise floor.

⚠ **Noise COLOUR decides the floor, more than either of the above — and no synthetic track settles
engagement.** |a| in a still stretch is driven by keypoint error near the filter's cutoff
`ω_c = (q/(σ_m²·dt))^(1/6)` (≈70–90 rad/s here, i.e. ≈11–14 ms): energy there reads as apparent
acceleration, energy well below it is tracked as real motion and barely shows. *White* per-frame noise
is the worst case — full power at ω_c — while real detector error is mostly a slow drift with pose and
appearance. Hence the corpus's measured still-address hip |a| p95 — **1652** on the 11-swing subset,
**2172** on the 08-18 sessions — against these synthetic fixtures at the same reference format:

| fixture | still \|a\| p95 | reads as |
|---|---|---|
| uniform 150 fps, white 3 px | above aRef | scale **1.000** — never engages at all |
| real cadence, 2 px as 0.5 px white + 1.94 px AR(1) τ 60 ms | **7546** | motion at aRef 4000 (it was 0.585 at the earlier aRef 8000 with a ±3-frame lead) |
| corpus, real swings, 08-18 | **2172** | `s = 0.54` at `expo 1` |

The AR(1) fixture is closer but still ≈4.6× the corpus, because an AR(1) drift has a **1/ω² tail** and
is therefore *not* quiet at ω_c — that 1.94 px drift carries ≈9× the 0.5 px white component's power
there. Matching the corpus would need a spectrally smoother error model (integrated or band-limited),
which nothing in this repo measures, and inventing one to make a test pass would prove nothing. So
`pose_smoother_test.cpp` §11c asserts only the **ordering** (a still stretch is smoothed harder than a
moving one; the moving stretch returns to 1.0) and **prints the |a| floor beside the corpus number**;
**whether the policy engages at the shipped aRef was decided by the bake-off on real swings**
(`tools/metrics/adapt_settings.jsonl` + the C15 gate criteria), not by a fixture — and it does: the
promoted row's still-address jitter falls 29–48 % on the three hip series.

⚠ **The floor is a real operating limit, not just a fixture artefact.** The corpus's still-address p95
(1652 / 2172 px/s²) sits below aRef 4000, so on real swings the address hold does engage — but a noisier joint, a coarser posing grid, a wider format or (above all) whiter keypoint error
eats the margin, and the honest reading of a quiet stretch that will not floor is "aRef is too near
this joint's own |a| noise", not "the policy is broken". The tests are split along exactly that line: a
**noiseless** track pins the discrimination exactly, a **white-noise** track pins the window law and
the excursion, and a **real-cadence** track pins the ordering and prints the |a| floor it presents to
aRef beside the corpus's number (it cannot decide engagement — see the colour note below).

⚠ **`kMode` / `kGroup` are the frozen header's first non-numeric literals.** §3 below describes
`pp_tuned_constants.h` as pure numeric literals; these two are `inline constexpr const char *`. They
live there anyway because **promoting phase 5 is a mode flip**, and the freeze file has to be the one
edit-point for it. §3's actual constraint is unaffected: they are `constexpr`, Qt-free and header-only,
so the deliberately Qt-free lowest layer still compiles. One `constexpr` parser in `pose_smoother.h`
reads both these defaults and a sweep's override string, so an unrecognised value means the same thing
in both places — `off` / `legs`, i.e. a typo'd sweep line runs as a control.

**Promotion happened exactly as phase 4.3's would have** — the C15 criteria (parity, ΔP4/ΔP7 in units of
the result's own σ, P1–P7 excursion ratio, still-address jitter reduction, σ ratio), each judged against
a **control run** because pose is non-deterministic. It is the only part of this design that moves
persisted `value[]`. The measured ΔP4 (median 0.12 σ, max 0.34 σ) is what keeps
[`norm_shapes.md`](../design/norm_shapes.md) out of scope — the P4 corridor content was seeded on the
old smoothing, and a systematic shift there would have put it in scope rather than being absorbed
silently. Re-check that if the full-corpus confirmation moves P4 further.

## 3. The frozen-defaults header — the single freeze edit-point

`src/Core/pp_tuned_constants.h` (`namespace pinpoint::tuned`) is the **single source of truth** for every
parameter that is tuned during validation and then frozen. `src/Core/` is the lowest common layer (both
`src/IMU/` live code and `src/Analysis/` offline code already include from it), and the header is
**pure numeric literals** (quaternions as `{w,x,y,z}` float arrays) so even the deliberately Qt-free
`orientation_filter.h` can include it.

The override path does **not** consult the header — overrides start from these as their *baseline*. When
validation locks a value: change the one literal here; every consumer (live + offline) picks it up; the
`tuned_constants_parity_test` proves the indirection stayed byte-identical. (Wrist axis **sign**
conventions stay in `wrist_angles.h` — they are code-structure choices, not single literals.)

## 4. Observability & objectives — *which check does this parameter move?*

The SwingLab scorecard objective is the **pass-rate of the score.py checks**. A sweep can only optimise a
parameter whose change is visible in a check. This is the make-or-break table:

| Namespace | Visible in scorecard? | Objective check(s) | Pre-condition |
|---|---|---|---|
| `seg.*`, `shaft.*`, `assembly.*` | **yes** | `seg.*`, `track.*`, `club.*`, `truth.*` | labelled / clean swings |
| `score.*` | **no** (moves `analysis.score`, not a check) | **HackMotion agreement** (Corpus 2) | paired IMU+HackMotion |
| `rules.*`, `bands.*` | **yes** (via offline assessment) | `diag.recall`, `diag.clean_no_fault` | **known-group labels** (`truth.json` `knownGroup`) |
| `sampler.*` | partial (inert `gimbalThresholdDeg`) | `diag.*` | labels; pitch-proxy populated |
| `filter.*` | **yes** (after C3) | `xmodal.imu_vision_corr` (vision), `diag.*` (labels), `filter.impact_continuity` (provisional) | vision shaft track / labels / real impact |

Three consequences worth internalising:

1. **`score.*` is injectable but not scorecard-optimisable.** No Tier-1/2/3 check reads the wrist *score*
   — scoring quality has no ground-truth on the lab corpus. Its objective is the HackMotion criterion
   (Corpus 2), so it is *described* by a sweep but *validated* against HackMotion.
2. **`rules.*`/`bands.*` need labels.** `diagnosis_metrics` returns no checks unless a swing declares a
   `knownGroup` (scripted fault or `clean`); without labels these knobs are injected but unobserved.
3. **`filter.*` became observable at C3.** Re-fusion feeds the wrist angles, so the filter moves
   `imu_vision_corr` (camera swings) and `diag.*` (labelled swings). `filter.impact_continuity` is an
   IMU-only diagnostic but **provisional** until a real impact-shock swing calibrates it (the synthetic
   corpus models no impact saturation).

## 5. Statistical methods — *how we decide a value is right*

Three activities, never conflated (the cardinal sin is tuning to a metric then reporting it as
validation): **Verification** (built it right — golden/parity tests), **Tuning** (best value — objective
on a *Tune* partition), **Validation** (built the right thing — agreement on *held-out* / against an
independent criterion). The sample size is set by the **statistic**, not the parameter
(full derivations in [`pipeline_validation_and_tuning.md` §3](pipeline_validation_and_tuning.md); the
operative results):

| Statistical goal | Driver / formula | Usable N | Tight N |
|---|---|---|---|
| Proportion (pass-rate, fault recall, FP) | `n = z²·p(1−p)/E²`; rule of three (`k/k` ⇒ rate ≥ `1−3/k`) | ~35 (±10 %) | ~140 (±5 %); ~60/fault for recall ≥ 0.95 |
| Agreement LoA (Bland–Altman) | `SE(LoA) ≈ 1.71·s/√n` | ~25 (±0.68 s) | ~50 (±0.48 s) |
| Reliability (SEM/MDC/ICC) | `CV(SEM) ≈ 1/√(2(n−1))` | ~25–30 (±14 %) | ~50 (±10 %) |
| RMSE/SD vs labels (track, phase timing) | χ² CI on SD | 10–15 swings × ~10 pts | — |
| Regression detection (A/B) | paired-t power | 50 pairs: d ≥ 0.5 @0.9 | 200 pairs: d ≈ 0.2 |

**Per parameter class — the statistic and the N:**

| Class | Validation statistic | Target | N (driver) |
|---|---|---|---|
| `seg.*` | Top-timing RMSE vs hand labels | `truth.event_top_s ≤ 0.03 s` | 10–15 labelled |
| `shaft.*` | θ-RMSE / head-px vs labels (proportion of clean passing coverage) | `theta_rms < 3°`, `head < 25 px`, coverage ≥ 0.6 on ≥ 90 % | 10–15 labelled (+ raw subset for sub-pixel θ) |
| `assembly.*` | coverage proportion; ŝ_hand residual | residual < `calibAcceptRad ≈ 7°` | 50 |
| `score.*` | Bland–Altman bias + 95 % LoA vs HackMotion; ICC(A,1) | FE/PS RMSE ≤ 4–5°, ICC ≥ 0.90; RUD ≤ 8° | ~50 paired |
| `sampler.*` | between-swing repeatability RMSE; Indeterminate rate | < 5° FE / < 8° RUD; < 2 % Indeterminate | ≥ 25–30 repeats |
| `rules.*` | known-groups recall + FP (point estimate + Wilson CI) | rule fires & is specific (*not* a numeric recall guarantee) | ~10–15 / fault |
| `bands.*` | distribution overlap vs observed + HackMotion tour ranges | corridors monotone, consistent | 50 (+ paired) |
| `filter.*` | per-phase `imu_vision_corr`; FE/RUD/PS just-after-impact vs HackMotion LoA; impact-continuity | corr ↑, within-LoA, continuous across impact | 50 + synth verification |

**Reliability ≠ Agreement ≠ Accuracy.** A parameter whose repeatability MDC (`1.96·√2·SEM`) exceeds the
effect a coach wants to see is useless *regardless of accuracy*; agreement (Bland–Altman, not
correlation) is bounded by the criterion's own LoA (HackMotion ≈ 1° FE / ≈ 5° RUD — never claim accuracy
finer than the reference's noise floor).

## 6. Optimisation methods — *how we search*

Escalate to the surface; do not over-engineer (full guidance in
[`pipeline_validation_and_tuning.md` §7.1](pipeline_validation_and_tuning.md)):

1. **Coordinate descent (default).** One knob at a time, accept on the diff gate, move on. Interpretable,
   diff-friendly, no dependencies — right for the largely-separable `seg.*`/`shaft.*`/`assembly.*`/`score.*`.
2. **Bayesian (TPE/GP) — on plateau.** Sample-efficient for ~14 `seg.*` + ~8 `shaft.*` against ~50
   swings when interactions are suspected (the "sweep plateau ×2" escalation trigger).
3. **CMA-ES — for the filter schedule.** Continuous, non-separable, gradient-free `filter.*` knobs.

**Partition the search, not just the report** (the overfitting guard): sweep on **Tune**, select on
**Validation**, touch **Held-out** once at freeze (`--freeze`). **Regression-gated objective:** maximise
the mean SUBJECT TO per-swing `regressions == 0` (the 5-pt diff), enforced *inside* the loop — a mean
gain that hides a 2-swing regression is rejected. **Convergence:** stop on no-Validation-improvement over
N trials, acquisition floor, or a trial/wall-clock budget — never a fixed count reported as best.

## 7. Acceptance criteria — by parameter class

A change is **kept only if mean ↑ AND `regressions: 0` per swing** (the universal gate), AND it meets the
class-specific bar:

- **seg/shaft/assembly:** monotone 100 %, Top ≤ 30 ms, coverage ≥ 0.6 (≥ 90 % clean), θ-RMSE < 3°.
- **score (C2):** bands consistent with observed + HackMotion; score monotone; locked against the
  criterion, not the lab scorecard.
- **sampler:** repeatability < 5° FE / < 8° RUD; < 2 % Indeterminate.
- **rules (known-groups):** scripted-fault recall point-estimate high + ≈ 0 FP on clean (with honest
  Wilson CI; a numeric recall ≥ 0.95 needs ~60/fault — deferred to a pooled fault library).
- **bands (C2):** corridors anchored on distribution + HackMotion ranges.
- **filter:** per-phase `imu_vision_corr` ↑; orientation continuous across impact; FE/RUD/PS
  just-after-impact within the Corpus-2 LoA; net drift bounded at the still finish.

---

# Part II — Developer Guide

## 8. The injection mechanism

The whole contract is five overloads in `src/Analysis/analysis_tuning.h`:

```cpp
namespace pinpoint::analysis::tuning {
// apply(map, "area.field", field) — write the override onto `field` if the key is present; no-op else.
inline void apply(const QVariantMap &ov, const char *key, float   &field);
inline void apply(const QVariantMap &ov, const char *key, double  &field);
inline void apply(const QVariantMap &ov, const char *key, int     &field);
inline void apply(const QVariantMap &ov, const char *key, int64_t &field);
inline void apply(const QVariantMap &ov, const char *key, bool    &field);
}
```

The map is `ShotAnalysisJob::tuningOverrides` (`shot_analyzer.h`), a `QVariantMap` of `"<area>.<field>"`
→ number. **Empty in production** (the app never sets it); the offline runner fills it from a params
JSON. Unknown keys are logged and ignored — a typo silently no-ops, visible in `runner.log`.

```
params.json ──flattenParams──▶ job.tuningOverrides ──┬─ segConfigFor(ov)            → seg.*
 (swinglab_run)                                       ├─ SwingScorer::score(.,.,ov)  → score.*
                                                      ├─ wristAssessmentConfigFor(ov)→ sampler./rules./bands.*
                                                      ├─ refuseConfigFromTuning(ov)  → filter.*
                                                      └─ ShaftTracker::track(.,job)  → shaft./assembly.*
```

`flattenParams` accepts both nested (`{"shaft":{"ridgeKernelPx":11}}`) and flat
(`{"shaft.ridgeKernelPx":11}`) forms.

## 9. How each stage consumes its keys

**The canonical pattern** (copy this for a new stage) — `segConfigFor` in `wrist_analyzer.cpp`:

```cpp
SegmentationConfig segConfigFor(const QVariantMap &ov) {
    namespace tn = pinpoint::analysis::tuning;
    SegmentationConfig c;                      // struct defaults (sourced from pp_tuned_constants.h)
    tn::apply(ov, "seg.fcEnvelopeHz", c.fcEnvelopeHz);
    tn::apply(ov, "seg.voteAgreeUs",  c.voteAgreeUs);
    // … one apply() per field …
    return c;                                  // empty ov ⇒ byte-identical defaults
}
```

**`score.*`** — `swing_scorer.cpp` copies the frozen band table into a mutable vector and overlays the
overrides; `SwingScorer::score(series, sessionType, overrides)` is called from `wrist_analyzer.cpp`:

```cpp
std::vector<ScoreBand> scoreBandsFor(int sessionType, const QVariantMap &ov) {
    std::vector<ScoreBand> bands(std::begin(kWristBands), std::end(kWristBands));
    for (ScoreBand &b : bands) {
        const QByteArray pfx = QByteArray("score.") + b.key + '.';
        tn::apply(ov, (pfx + "mu").constData(),          b.mu);
        tn::apply(ov, (pfx + "sigma").constData(),       b.sigma);
        tn::apply(ov, (pfx + "weight").constData(),      b.weight);
        tn::apply(ov, (pfx + "oneSidedDir").constData(), b.oneSidedDir);
    }
    return bands;                              // + deadbandFor(ov) for score.zIn/.zOut/.p
}
```

**`sampler.*`/`rules.*`/`bands.*`** — one builder, `src/Analysis/wrist_assessment_tuning.h`:

```cpp
inline WristAssessmentConfig wristAssessmentConfigFor(const QVariantMap &ov) {
    WristAssessmentConfig cfg;                 // sampling + rules + band.tuning, all frozen defaults
    tn::apply(ov, "sampler.gimbalThresholdDeg", cfg.sampling.gimbalThresholdDeg);
    tn::apply(ov, "rules.confidenceFloor",      cfg.rules.confidenceFloor);
    tn::apply(ov, "bands.flexExtMargin",        cfg.band.tuning.flexExtMargin);   // … etc
    return cfg;
}
```

**`filter.*`** — `src/Analysis/orientation_refuse_tuning.h` builds a `RefuseConfig` (shared by the
analyzer and the `--refuse-orientation` tool, so the key list lives once):

```cpp
inline bool tuningWantsRefusion(const QVariantMap &ov)            // the C3 master switch
{ return ov.value(QStringLiteral("filter.refuse")).toBool(); }

inline pinpoint::RefuseConfig refuseConfigFromTuning(const QVariantMap &ov, int64_t impactUs,
                                                     float betaDefault = pinpoint::tuned::filter::kBeta) {
    pinpoint::RefuseConfig cfg; cfg.betaStatic = betaDefault;
    tn::apply(ov, "filter.adaptive",   cfg.adaptive);
    tn::apply(ov, "filter.betaDynamic",cfg.betaDynamic);   // … betaStatic/gates/sat/blank …
    if (cfg.adaptive) cfg.impactUs = impactUs;             // impact-blank window armed only when adaptive
    return cfg;
}
```

## 10. The offline assessment path (`rules.*`/`sampler.*`/`bands.*` observability)

The Tier-2 engine is GUI-only in production; SwingLab opts into it via `ShotAnalysisJob::runAssessment`
(set by `swinglab_run`, default OFF). In `WristAnalyzer::analyze`, after the metric series exist:

```cpp
if (job.runAssessment && hasImu && !series.empty()) {
    const InMemoryWristAngleSource src = buildWristAngleSource(detail->series, detail->phases);
    const auto provider = makeReferenceBandProvider(BandProviderKind::Archetype);
    const WristAssessmentConfig acfg = wristAssessmentConfigFor(job.tuningOverrides);
    const PpWristAssessmentResult ar = WristAssessmentEngine::assess(src, *provider, acfg);
    detail->findings = ar.findings; detail->assessmentScore = ar.score.total;
}
```

Findings serialise **additively** to `swing.json` `analysis.assessment{scoreV2, findings[]}` (only when
`runAssessment` ran ⇒ production `swing.json` unchanged). `score.py`'s `diagnosis_metrics` reads them
against the swing's `truth.json` `meta.knownGroup` (a scripted fault id, or `clean`/`control`):
`diag.recall` (the fault is surfaced, low-confidence counts) / `diag.clean_no_fault` (no *confident*
false fault).

## 11. The adaptive orientation filter (`filter.*`)

Two layers, both header-only and standalone-testable
(`src/IMU/orientation_refuser.h`, `src/Analysis/tests/orientation_refuse_test.cpp`):

**`adaptiveBeta(...)`** — the per-sample gain. Returns 0 (gyro-only) under saturation (`|a| ≥ accelSatG`)
or inside the impact window `[impactUs ± blank]`, else a `betaStatic → betaDynamic` ramp on the worse of
the two dynamics gates (accel residual `|‖a‖−1g|` / `accelErrGateG`, gyro magnitude / `gyroGateDps`).

**`refuseOrientationAdaptive(filt, samples, cfg)`** — warm-starts the Madgwick filter from the stored
quat at the window's first sample, then re-integrates the raw accel+gyro with `filt.setBeta(adaptiveBeta(...))`
per sample. With `betaStatic == betaDynamic` and no gate tripped it reduces *exactly* to fixed-beta
(byte-identical) — the E1 unit-test invariant.

**C3 — feeding the metric.** `ImuVisionFuser::fuse(window, bindings, gridHz, const RefuseConfig*)`:
when the pointer is non-null it re-derives each source's orientation (slerped onto the grid) and uses
*that* as `qRaw` in `q_anat = A·qRaw·M`; null ⇒ the stored quaternion (production, byte-identical).
`WristAnalyzer::analyze` passes `&refusion` exactly when `tuningWantsRefusion(job.tuningOverrides)`. This
is what makes `filter.*` move the wrist metric (and hence `imu_vision_corr` / `diag.*`).

**C5 — the IMU-only objective.** `WristAnalyzer` computes `impactContinuityDeg` (max orientation step
across the impact window *beyond the gyro-predicted step* — the accel-correction/shock residual) →
`SwingAnalysis::filterImpactStepDeg` → `swing.json analysis.filter.impactStepDeg` → `score.py`
`filter.impact_continuity` (**warn**, provisional threshold pending real impact data).

## 12. The SwingLab harness — running a sweep

`space.json` is a flat map of bounds; `[lo, hi]` (float) or `[lo, hi, "int"]`:

```json
{ "seg.fcEnvelopeHz": [4.0, 12.0], "shaft.ridgeKernelPx": [5, 15, "int"],
  "score.leadWristFlexExt.mu": [8.0, 22.0], "filter.betaDynamic": [0.0, 0.05] }
```

`partitions.json` assigns swing names to roles (held-out is never run unless `--freeze`):

```json
{ "tune": ["s001","s002"], "validation": ["s010","s011"], "heldout": ["s020"] }
```

```bash
P=~/.swinglab-venv/bin/python
$P lab.py ingest  /data/corpus-1                      # corpus.json (surfaces knownGroup, calibration, …)
$P lab.py run     /data/corpus-1 /runs --id baseline  # the gate reference
$P lab.py sweep   /data/corpus-1 /runs space.json \
        --method coordinate --baseline /runs/baseline \
        --partition partitions.json --trials 40        # gate + partition enforced in-loop
$P lab.py diff    /runs/baseline /runs/candidate       # exit 1 ⇒ regressions
```

The sweep prints `tune`/`val` means + `reg` per trial, rejects gated trials, and writes
`sweep-result.json` (history + best, selected on Validation). The scorecard objective is the pass-rate of
`invariants + truth_metrics + diagnosis_metrics + filter_metrics` (`score.py`). **Always rebuild
`swinglab_run` after any analysis C++ change**, and **never diff across hosts** (CPU vs CUDA pose
differs).

## 13. Recipe — adding a new tunable parameter

1. **Frozen default** → add a named `inline constexpr` in `pp_tuned_constants.h` (the appropriate
   sub-namespace); have the config struct's member initialiser reference it.
2. **Inject** → add a `tn::apply(ov, "area.field", cfg.field)` line in the stage's builder (or write a
   builder following `segConfigFor`). Thread `job.tuningOverrides` to the call site if not already there.
3. **Observe** → confirm the change moves a `score.py` check. If it does not (like `score.*`), it is
   injectable but not scorecard-optimisable — document the real objective (e.g. HackMotion). If it needs
   a new check, add one to `score.py` following the additive five-step reader contract
   (sidecar/field → `Swing`/`RunResult` accessor → check group → synth stamp → `ingest` flag).
4. **Test** → extend `tuning_overrides_test.cpp` (the override moves the consumed value; empty map is a
   no-op) and `tuned_constants_parity_test.cpp` (the new default equals its historical literal).
5. **Document** → add the key to this catalog (§2) and flip its row in the §2.4 ledger of the backbone
   doc.

## 14. Tests (the safety net)

| Test (`src/Analysis/tests/`) | Guards |
|---|---|
| `tuned_constants_parity_test` | every frozen constant == its historical literal; default structs unchanged (the header refactor is byte-identical) |
| `tuning_overrides_test` | each namespace's override actually moves the consumed value; empty map is a no-op |
| `orientation_refuse_test` (E1–E4) | adaptive refuser reduces to fixed-beta when flat; impact-blank / continuous gate / saturation gates fire correctly |
| `swing_scorer_test`, `reference_bands_test`, `composite_score_v2_test`, … | default-path behaviour unchanged after the override plumbing |
| `swing_window_parity_test` | RAM-vs-disk SwingWindow parity holds through the `ImuVisionFuser` change |

All run via `cmake -S src/Analysis/tests -B build/analyzer-tests && ctest --test-dir build/analyzer-tests`.

---

## 15. Honest limits

- **`score.*` has no lab-scorecard objective** — it is validated against HackMotion (Corpus 2), not
  swept against Tier-1 checks.
- **`rules.*`/`bands.*` need known-group labels** to be observable; without them they are injected but
  unmeasured.
- **`sampler.gimbalThresholdDeg` is currently inert** offline (pitch-proxy not populated by the adapter).
- **`filter.impact_continuity` is provisional** — the synthetic corpus models no impact shock, so its
  threshold and the per-phase/net-drift checks await a real impact-shock swing.
- **ESKF `R` is not exposed** — the vendored `third_party/imu_ekf` is kept verbatim and ESKF cannot
  warm-start, so it sits outside the re-fusion tuning loop (a separate escalation if ESKF ever becomes
  the production filter).
- **Single-host, single-golfer within-subject** until Corpus 3 — no population claim before multiple
  golfers (the effective N for generalisation is *golfers*, not swings).

*This document is the parameter catalog + dev guide; `pipeline_validation_and_tuning.md` is the
methodological backbone; the `/swinglab` skill is the operator contract.*
