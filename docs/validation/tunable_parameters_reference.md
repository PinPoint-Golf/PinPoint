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
