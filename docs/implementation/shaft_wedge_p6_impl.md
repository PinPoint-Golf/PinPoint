# Shaft blur-wedge implementation — fixing the downswing θ short-path defect

*Prepared 2026-08-10. Two sessions with a handoff through this document: Session 1
(evidence honesty) fills its results tables here; Session 2 (the wedge) executes
from this document in a fresh session. Plan of record approved 2026-08-10; source
finding: the P-position bridge corpus gate ([[p6-phantom-crossing]] memory,
`docs/design/shaft_detection_skeleton_design.md` §G6/R5/R6/R8).*

## Why wedge detection (the statement of intent)

At delivery the club rotates at up to ~1400°/s. At that rate a shaft does not
image as a line — it images as a **fan (wedge) about the grip**: a *plateau* in
the per-angle response S(θ) whose **centroid is the mid-exposure angle** and
whose **width measures ω·t_exp**. The v3 tracker hunts a thin straight ridge
(90 px of contiguous support) everywhere, so in the high-ω zone it sees nothing,
its per-frame percentile normalisation fabricates a winner from noise, and the
DP glides the short angular path. The fix is the design's R8: **read the wedge,
don't fight it** —

1. **Predict the fan before looking.** A minimal R6 double-pendulum predictor
   gives the club angle from the measured lead-arm angle and the stereotyped
   wrist-cock curve: `φ_club_pred = φ_arm + branch·β̂(s)`, envelope `±k·σ_β(s)`,
   and a predicted rate ω̂ — so we know *where* the fan is and *how wide* it
   should be before we look.
2. **Verify by integration, not per-pixel peaks.** Inside that tight envelope,
   sum swept energy across the fan extent (matched filter). A semi-transparent
   smear is faint per-pixel but coherent over the predicted region; because the
   envelope is tight, the threshold can drop below the frame floor without
   inviting false positives.
3. **Anchor proximally.** Image velocity ∝ ρ, so the innermost shaft barely
   blurs: the proximal tangent supplies θ, the distal fan supplies σ. The
   straight-line length gate (minLenPx 90) must not apply to wedge frames.
4. **Never fabricate.** If even the scene-median-difference channel drowns,
   emit no vision measurement and coast. The wedge *widens the band where
   vision still contributes*; it does not pretend to see a shaft that is not
   there.

## The defect (measured 2026-08-09, all refs at f056e1e)

Camera-only corpus (61 swings): the emitted θ(t) sweeps Top(+19°) → 0° →
Impact(−84°), ~100°, the SHORT way; hand labels show the real sweep +20° →
−90° → −179° (true P6, ~30 ms pre-impact) → −270°, ~280°. The phantom
horizontal crossing mid-window is what `locatePTimes` promotes as P6 (~120 ms
early, 7/7 measurable swings); the true crossing never exists in the series.
Mid-downswing frames are stamped `ShaftMeasured` while ~60° off the labelled
line.

Mechanism (`src/Analysis/shaft_track_assembly.cpp`): banded Viterbi DP over a
wrapped 1° grid (`viterbiDP` :825-863) with cost `kSmooth·Δθ²` and no rate
state; un-evidenced frames keep a flat `wE2` emission row (:1214, early-outs
:1227-1232) so the DP holds θ still; `normScores` (:973-980) normalises
percentile-within-frame so ~3% of bins mint 1.0 even in pure blur; the RAY
(`ShaftMeasured`) blessing (:1368-1374) tests that frame-RELATIVE score
(`rayEvMin 0.45`) — fabricated evidence near the DP's own wrong path clears it.
`ShaftWedge` (0x08) is consumed (:1702 snap, :1924 milestone fit) but never
produced. Wrapped-grid degeneracy: short and long paths end in the same state,
so only mid-path evidence (or a physics penalty) can fund the real sweep.

## Session boundaries

- **Session 1 — evidence honesty.** Absolute evidence floor + absolute RAY
  support gate + calibration trace. Kills fabricated evidence (and with it most
  phantom P6s); does NOT find the real path. Corpus A/B calibrates and
  validates. **Defaults stay dark** (keys ride params files).
- **Inter-session contract (REVISED by the S1 gate):** S1 changes labels, not
  the DP path — downswing `ShaftMeasured` drops modestly (76→74% at the chosen
  values) but **P6 phantoms survive (19/61)** because the wrong path sits on
  real off-shaft structure, not normalised noise. `track.valid` 55→52 (S2
  restores via the WEDGE tier). Both default flips happen together in ONE
  follow-up commit after Session 2's gate. S2 carries the whole path-correction
  load: wedge evidence inside the envelope + the kinCone off-envelope penalty
  (promoted to an expected arm, see Session 1 results).
- **Session 2 — R8-T1 wedge + minimal R6 predictor.** New pure headers
  `shaft_kinematics.h` + `shaft_wedge.h`, integration into `decideTrack`,
  three-arm corpus gate, then the default flip + a positions-ladder coverage
  re-run to record the corrected P6/P5 rows.

## Session 1 spec

Config (`ShaftV3Config`, `shaft_track_assembly.h`, keys in `fromOverrides`
beside `shaft.rayEvMin`):

| Key | Default | Meaning |
|---|---|---|
| `shaft.evAbsFloor` | 0.0 (off) | raw-p97 floor per channel; a channel whose raw p97 misses it zeroes its norm scores + support for that frame |
| `shaft.evAbsFloorDif` | −1.0 | dif-channel override; <0 ⇒ use `evAbsFloor` |
| `shaft.raySupportMin` | 0.0 (off) | absolute `RidgeResult.support` at the DP's θ required for the RAY/`ShaftMeasured` blessing |

Mechanics: floor applied in `evidenceBody` (:1234-1242) BEFORE `normScores`
(the normalisation itself is untouched — frozen invariant); keep
`RidgeResult.support` (`shaft_tracker_math.h:55`, currently discarded) in a new
`SUP[i]` beside `EV[i]`; RAY test (:1373) gains `SUP[i][θdp] ≥ raySupportMin`.
Demoted frames become PRED/`ShaftCoasted|ShaftHeadProjected` (the honest
label); BAND unaffected; RECON coherent for free (`evAt` reads floored EV).
Trace: `ShaftDecideTrace` gains `rawP97[]`, `difP97[]`, `supAtDp[]` for the
calibration histograms. Units anchor: a marginal genuine 90 px line at contrast
+10 scores ≈ 62; blur-flat frames ≈ 0. Sweep grid: floor {10,20,30,45} ×
support {0.15,0.25,0.35}.

Tests: `shaft_evidence_test` raw-unit pins (noise-only p97 < 10; painted
3 px line contrast +60 → score > 60, support > 0.5; weak +25 line still clears
the chosen default); `shaft_decide_test` synthetic swing (sharp backswing line,
noise downswing): keys on ⇒ downswing PRED not Measured while backswing stays
RAY; keys off ⇒ field-by-field byte-identity with a default run.

### Session 1 results (gate run 2026-08-10, sha 7dc9397, runs `C:\PinPointStudio\p6s1\`)

**HEADLINE FINDING (recalibrates Session 2):** on real footage the fabrication
is **structure-driven, not noise-driven**. Rays from the grip always cross real
structure (arms, clothing, background edges), so the frame-level raw p97 does
NOT discriminate — genuine backswing RAY frames (p50 447) overlap mid-downswing
frames (p50 345) completely, and `evAbsFloor=100` never fired on the corpus.
The DP's wrong downswing path sits on genuinely strong off-shaft ridge
evidence; demoting its labels does not move the path. **P6 phantoms survive S1
entirely (19/61 in every arm).** Consequences for S2: the wedge must
OUTCOMPETE real off-shaft structure inside the envelope (not merely beat
noise), and `shaft.wedge.kinCone` is promoted from optional fallback to an
expected arm — the off-envelope penalty is the lever that defunds
structure-backed wrong paths.

*Calibration (19 traced swings, 0611+0703 sessions, keys dark):*

| Cohort | rawP97 p5/p50/p95 | difP97 p5/p50/p95 | supAtDp p5/p50/p95 |
|---|---|---|---|
| genuine backswing RAY frames (n=1735) | 221 / 447 / 597 | 221 / 376 / 538 | 0.5 / 0.9 / 1.0 |
| mid-downswing frames (n=549) | 217 / 345 / 469 | 219 / 307 / 452 | 0.1 / 0.6 / 1.0 |
| mid-downswing RAY frames (n=385) | 210 / 322 / 448 | 213 / 275 / 427 | 0.4 / 0.6 / 1.0 |

*Chosen values (params-borne until the post-S2 flip):* `evAbsFloor = 100`
(deep-blur/low-light safety net only — inert on this corpus),
`evAbsFloorDif = −1`, **`raySupportMin = 0.4`** (arm A). Arm B (0.5) demoted
more (76→69%) but invalidated 2 further tracks and silenced one P2-truth swing
via the coverage coupling — rejected.

*Gate (61-swing corpus, live pose, vs `p2bridge\on` baseline):*

| Metric | Baseline | Arm A (0.4) | Arm B (0.5) |
|---|---|---|---|
| Corpus P6 detections | 19/61 (all phantom) | 19 (unchanged — labels ≠ path) | 19 |
| P_CHECKS p6 within 0.04 s | 0/7 (~120 ms early) | 0/7 identical errors | 0/7 |
| P2 / P8 truth checks | 11/13, 11/11 | 11/13, 11/11 | 11/12 (one truth swing's track went invalid), 11/11 |
| Downswing ShaftMeasured stamps | 1330/1748 (76%) | 1298 (74%) | 1204 (69%) |
| track.valid count | 55/61 | 52 (S2-recoverable) | 50 |
| Keys-off equivalence, pose2-pinned vs `p2bridge\dark2` | — | **61/61 identical** modulo the positions-ladder insertions the f056e1e default flip legitimately added (strip `segment==9 ∧ phase∈{12,8,13,9,14}` + version) | same build |

*Unit tests:* `shaft_evidence_test` 13/13 (raw-unit pins: noise p97 < 10;
+60 line score > 60 support > 0.5; +25 line ≥ 20), `shaft_decide_test` 68/68 —
including the synthetic proof of the mechanism (default config fabricated
Measured on 25/32 pure-noise downswing frames; keys demote all 32 to PRED while
22 backswing frames stay RAY) and the keys-off field-by-field identity pin.
NOTE the synthetic/real contrast: the floor kills *noise* fabrication (synthetic)
but not *structure* fabrication (real corpus) — both facts are true and the
distinction is the S2 design input. `swinglab_run --trace` now emits
`raw_p97`/`dif_p97`/`sup_dp` per frame for future calibration.

## Session 2 spec

**`src/Analysis/shaft_kinematics.h`** (new pure header, standalone test): the
design §R6 9-knot wrist-cock table (s / |β̂| midpoint / σ_β): 0.00/8°/8°,
0.15/27°/15°, 0.35/70°/20°, 0.50/92°/22°, 0.60/100°/30°, 0.80/47°/25°,
0.90/7°/10°, 0.95/27°/20° (sign flips to lead side), 1.00/95°/30°.
`swingProgress(f, bs0, top, impact, fin0)` piecewise-linear to those anchors
(bs0→0, top→0.5, impact→0.9, fin0→1.0, clamped); `betaHatDeg(s)`,
`sigmaBetaDeg(s)`; `phiClubPredDeg(phiArm, s, chir)` (trail side pre-impact,
lead after — branch from s + chirality); `omegaPredDegPerS()` finite-difference
of the prediction (no DP dependency — breaks the t_exp chicken-and-egg);
`envelope(s, phiArm, chir, kSigma)` → {centerDeg, halfDeg}.

**`src/Analysis/shaft_wedge.h`** (new pure header): `WedgeConfig`
(`shaft.wedge.*`, `enabled=false` dark): `omegaMinDegS 720` (trigger),
`tExpBootstrapS 0.003` clamp [0.001,0.008], `calOmegaLoDegS 286`, `kSigma 3.0`,
`threshScale 0.5` (× `evAbsFloor` — deliberately below the frame floor, safe
only inside the envelope), `minSpanDeg 2.0`, `proximalRHiFrac 0.35`,
`proximalMinLenPx 40`, `wWell 6.0`, `conf 0.45`, `dpTolDeg 4.0`,
`kinCone false` + `wKinCone 4.0`. `measureWedge(raw, dif, env, absFloorRef,
cfg)` → plateau finder: contiguous bins ≥ threshold in either channel, span ≥
minSpanDeg, energy-weighted circular centroid, width → σ; reject centroid
within `armVetoDeg` of φ+180 (trail-arm smear); both channels drown ⇒ no
candidate.

**Integration (`decideTrack`, no changes inside frozen
`frameEmission`/`viterbiDP`/`normScores` bodies):**
1. Per-frame trigger on DATA: `wedge.enabled && |ω̂| ≥ omegaMinDegS` (never
   sessionType, never phase-only).
2. Triggered frames: a second *proximal* `ridgeSweep` pair (raw + dif)
   restricted to the envelope's θ bins with derived RidgeConfig
   (`rHi = proximalRHiFrac·rmax`, `minLenPx = proximalMinLenPx`) — the R5 blur
   relaxation without touching the main sweep.
3. Post-loop, serial, before `viterbiDP` (:1262-1264): t_exp =
   clamp(median(widthRad/ω̂) over ω̂ ≥ 5 rad/s frames, bootstrap 3 ms);
   σ_θ = max(width/2, ω̂·t_exp/2); then well injection
   `emis[i][k] −= wWell·exp(−½(Δθ/σ_θ)²)` within 3σ, clamped at `−wBand`
   (a band lock always outranks a wedge).
4. Tier chain BAND > RAY > **WEDGE** (new trace tier value 4 — swinglab
   readers note): DP θ within σ_θ+dpTolDeg of a candidate ⇒ flags
   `ShaftWedge|ShaftHeadProjected` (deliberately NOT `ShaftMeasured`),
   conf = `wedge.conf`; WEDGE counts into `spanMeas` (restores S1 coverage).
   Snap (:1702) and milestone fit (:1924) already accept the flag.
5. `shaft.wedge.kinCone` — **expected arm, not optional** (S1 finding: the
   wrong path is funded by real off-shaft structure that evidence-level honesty
   cannot demote; the off-envelope penalty is what defunds it): `+wKinCone`
   outside the envelope on triggered frames only — prior-as-constraint (like
   the existing C4 cone); NEVER upgrades a tier.

Tests: `shaft_kinematics_test` (knots, branch signs, envelope contains truth
under ±8° φ_arm noise, ω̂ sanity); `shaft_wedge_test` (plateau centroid ±1.5°;
flat row ⇒ none; outside envelope ⇒ none; arm-smear rejected; well clamp);
`shaft_decide_test` end-to-end long-path fixture (sharp backswing + painted fan
downswing ⇒ DP takes the long path; exactly one horizontal crossing in
(top, impact) near truth, fed to `locatePTimes` to pin P6; wedge frames carry
`ShaftWedge` not `ShaftMeasured`).

### Session 2 gate (three arms: dark / wedge / wedge+kinCone)

| Metric | Post-S1 baseline | Gate |
|---|---|---|
| P_CHECKS p6 within 0.04 s (7 truth swings) | TBD (from S1) | ≥5/7, none grossly out |
| Corpus P6 detections + parallel_p6 geometry | TBD | recall up materially, median θ err ≤ ~10° |
| P2 / P8 | 11/13, 11/11 | unchanged |
| track.valid | S1 report | ≥ S1 AND ≥ original baseline |
| Dark run (all keys off), pose2-pinned | S1 dark | byte-identical |

Then ONE follow-up commit flips `evAbsFloor`/`raySupportMin`/`wedge.enabled`
defaults together, and `lab.py coverage` re-runs record the corrected P6/P5
blocking rows (P-position bridge follow-through).

## Traps (each has bitten before)

- **V1 evidence freeze (2026-07-18):** never retune `rayEvMin`, `wE2`,
  `kSmooth`, `wmax*`, `minLenPx`, or any frozen constant. New behaviour only
  behind new dark keys; default flips are separate commits.
- **Session-agnostic:** gate on data (ω̂, positions, ladder), never sessionType.
- **Corpus jitter:** live-pose runs can NEVER byte-match (CUDA jitter at the
  9th decimal, events can move 40 ms). Byte-identity claims require pose2-pinned
  runs (`--pose-dir C:\PinPointStudio\stagegate\pose2`). Baselines:
  `C:\PinPointStudio\p2bridge\{dark,on,dark2}`; studio protocol in
  [[golfsimpc-studio-build]] (build via `build_swinglab.cmd` pattern into
  `build\Release-Installer`, run `parity_run.py` via the venv python).
- **DP wrap degeneracy:** the long path ends −270° ≡ +90° on the wrapped grid;
  output wraps at :1596-1601; `locatePTimes` hysteresis crossing must be
  verified on a wrapped long-path series (unit test, not code change).
- **Coverage coupling:** demotions shrink `spanMeas` → `coverage` →
  `track.valid` (`coverageMin 0.60`); S1 drops are expected and recovered by
  the S2 WEDGE tier counting into `spanMeas`.
- **Runner.log does not echo tuning keys** (2-line log): verify key uptake via
  effects in result.json (e.g. Measured-stamp counts), not log grep.
- **Three P vocabularies + P5⊂P6:** unchanged from the bridge brief; the P6
  consumer is `locatePTimes` (`shaft_positions.h:412-424`) — downstream, do not
  modify it for this fix.

## Key/file index

| What | Where |
|---|---|
| S1 floor + RAY gate + SUP + trace | `src/Analysis/shaft_track_assembly.{h,cpp}` |
| `RidgeResult.support` (exists, unused pre-S1) | `src/Analysis/shaft_tracker_math.h:55` |
| S2 predictor | `src/Analysis/shaft_kinematics.h` (new) |
| S2 wedge measure | `src/Analysis/shaft_wedge.h` (new) |
| Tests | `src/Analysis/tests/{shaft_evidence,shaft_decide}_test.cpp` + new `shaft_kinematics_test.cpp`, `shaft_wedge_test.cpp`, `tests/CMakeLists.txt` |
| Default flip (post-S2 only) | `pp_tuned_constants.h` / `ShaftV3Config` defaults |
