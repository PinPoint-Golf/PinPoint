# Session brief — the analyzer-layer P-position bridge

*Prepared 2026-08-09. Execute in a fresh session. Source finding: coverage session
addendum item 2, `docs/design/session_diagnostics_build_findings.md` ("The
analyzer-layer P-position bridge is the real unlock, now quantified").*

## Goal

Promote the club-track's already-located P-positions (P2/P3/P5/P6/P8) into
`Segmentation.events` PhaseEvents, so phase-anchored measures unblock on
camera-only swings **without any new capture**. The detections exist and are
persisted (`analysis.club.positions[]`); nothing emits them into the ladder. The
deferral is recorded verbatim at `src/Analysis/shaft_track_assembly.cpp:1768-1770`:
*"PhaseEvent emission for P2/P5/P8 belongs to the analyzer layer and is DEFERRED —
the enum values exist but nothing emits them here."*

**Quantified prize** (61-swing Mark-Liversedge corpus, camera-only, refon-live
sample): club track finds P2 on 20/20, P8 on 17/20, P6 on 8/20; P5 comes free
whenever P6 exists (see Constraints). COVERAGE.md's top blocking phases:
p5(360) / p6(232) / p8(61) / p2(61) swing×measure rows.

## What exists today (all file:line refs current at `HEAD` 474a279)

### Producer side — positions are already located, never emitted
- Detector: `src/Analysis/shaft_positions.h` — `locatePTimes()` (:376-437) finds
  P1–P8 + P10 as hysteresis-confirmed horizontal crossings of shaft θ(t) (P2/P6/P8)
  and lead-forearm φ(t) (P3/P5), sub-frame interpolated, output ordered by p AND
  strictly ascending t_us. P4/P7 are the tracker's Top/Impact landmarks; P9 is
  reserved and never emitted; p==10 is Finish.
- Population: `src/Analysis/shaft_track_assembly.cpp:1761-1876` (Layer B),
  B2 milestone fit :1886-1975. Gated on `cfg.positions.enabled` (default true).
- Data shape: `ShaftPosition` (`src/Analysis/swing_analysis.h:440-452`) —
  `p` is **1..8 or 10, NOT a Phase enum value**; `t_us` is window-domain (same
  domain as PhaseEvents — no conversion needed); `conf` from nearest track sample;
  `sigmaThetaDeg >= 0` only for `source == MilestoneFit`.

### Ladder side — one real producer, no merge
- `Phase` enum (`swing_analysis.h:110-119`, APPEND-ONLY, ints persisted):
  P1=Address(0), P2=ShaftParallelBack(12), P3=MidBackswing(8), P4=Top(2),
  P5=ArmParallelDown(13), P6=Delivery(9), P7=Impact(5), P8=ShaftParallelThrough(14),
  P9=FollowThrough(11), P10=Finish(7). **P-order ≠ enum order.**
- `PhaseEvent` (:122-129) carries `conf` and `provenance`; `SegmentRole::Club = 8`
  exists and is the right provenance for these events.
- Vision producer (the only one that has ever run on real data — all 61 corpus
  swings are `bindings: 0`): `phasesToSegmentation()`
  (`shaft_track_assembly.cpp:1001-1028`) emits ONLY Address/(Takeaway)/Top/Impact/
  Finish, flat conf 0.5, bare stable_sort, **no monotonicity enforcement**.
- Resolution: `SegResolveStage` (`src/Analysis/wrist_analyzer.cpp:506-517`) picks
  IMU else vision — strict preference, no merge. `EventRefineStage` (10b, :552-586)
  retimes existing events (never inserts, never touches Impact, abstains on
  monotone violation — `src/Analysis/event_refine.h:65-70`); it already re-times
  the P1 position twin so event and position never diverge, and already documents
  this exact work as "a future `refine.positionsLadder` key"
  (`wrist_analyzer.cpp:576-578`). `BindDetailStage` (11, :588-597) copies
  `ctx.seg.events` → `detail->phases`.

### Consumer side — how a measure unblocks
- `src/Diagnostics/measure_sample.cpp` — ladder read/dedupe :80-105 (round-trips
  ints through the token vocabulary; first occurrence wins), phase-labelled
  phaseSamples as a second route :120-152, `reduceOverGrid()` :219-289 (missing
  anchor/window phase ⇒ measure unavailable).
- Phase↔token vocabulary: `src/Diagnostics/measure_facets.cpp:74-101`.
- 5 metrics already declare `P::ArmParallelDown` in the manifest
  (`metric_catalogue_manifest.cpp:526, 734, 935, 993, 1789`).

## Recommended design (decide at session start, then execute)

1. **Plug point**: a new `AnalysisStage` ("PositionsLadderStage") between
   `EventRefineStage` (10b) and `BindDetailStage` (11) in `wristProfile()`
   (`wrist_analyzer.cpp`). At that point `ctx.detail->shaft.positions[]` is
   populated and `ctx.seg` is resolved AND refined; BindDetail then persists the
   appended events with zero extra plumbing. (Alternative rejected: emitting
   inside `phasesToSegmentation` — that only runs in the no-IMU branch and
   widens Layer B's blast radius, the exact thing the deferral avoided.)
2. **Mapping** (positions `p` → `Phase`): 2→12, 3→8, 5→13, 6→9, 8→14, provenance
   `SegmentRole::Club`, conf = the position's own `conf`. Do NOT re-emit
   p∈{1,4,7,10} — Address/Top/Impact/Finish anchors already exist; the ladder's
   `eventFor()` takes the FIRST match and duplicates would shadow.
3. **Monotonicity policy — insert-if-monotone, abstain otherwise** (event_refine's
   contract, applied to insertion): a candidate is inserted only if it lands
   strictly between its neighbouring surviving anchors in time. Never reorder or
   drop existing events. On the 7 corpus swings with non-monotone vision ladders
   (Top==Finish etc.) this naturally abstains for windows that don't exist.
4. **Gating**: a params key (`refine.positionsLadder`, per the recorded intent),
   **dark by default** for the A/B soak; flip the default in a follow-up commit
   once the gate is green. Unknown params keys are silently ignored — verify the
   key echoes in `runner.log`.
5. **`Segmentation.version`**: 3 currently means "shaft refinement ran". Bump to 4
   ("positions ladder emitted") — version is persisted and read back, so state it
   in the header comment (`swing_analysis.h:139-153`).
6. **Coverage tooling in the same change**: `tools/swinglab/swinglab/coverage.py`
   — add P2 to `MATRIX_COLUMNS` (:47-50) and delete the now-stale ":41-46 'P2 has
   no analyzer event'" comment. `score.py` `P_CHECKS` (:213-224) already checks
   p5/p6/p8 event times (tolerances 0.04/0.04/0.05 s); consider adding a p2 event
   check beside the existing geometry fallback (:248+). Note the scorecard is
   100×passed/total — adding checks re-normalises scores across runs.

## Constraints and traps (each of these has bitten before)

- **Three P vocabularies**: `Phase` enum, `ShaftPosition.p` (plain 1..10), and the
  string tokens (`measure_facets.cpp`). Any int→Phase must round-trip through
  `phaseFromToken(phaseToken(p))` or a stray int lands on a neighbour.
- **P5 requires P6**: `locatePTimes` bounds the P5 search window as (P4, P6)
  (`shaft_positions.h:418-423`), so at P6=8/20, p5(360) rows only partially
  resolve. Widening the P5 window (independent downswing-end bound) is a
  **stretch goal, separate commit** — it changes detection, not just emission.
- **Blocked-measure accounting shifts, doesn't just shrink**: most p2/p5/p8
  measures are wrist-DOF measures whose *metric* is absent on camera-only swings —
  emitting phases moves some rows BLOCKED_PHASE → RESOLVED and exposes others as
  BLOCKED_METRIC. Report success as the delta in BOTH columns.
- **Timestamps**: positions and events share the window domain in memory;
  `serializeAnalysis` subtracts `clock.t0_us` exactly once at write. Do not
  convert anything. (Rounds one/two/three of the relative/absolute war:
  `swing_reanalyzer.cpp:570-586`, the extract_pose `+t0` bug — fixed 2026-08-09,
  use `stagegate/pose2/`, never `stagegate/pose/`.)
- **Analysis is session-agnostic** (hard rule): gate on available data
  (positions present, ladder present), never on sessionType.
- **Vision-ladder conf is flat 0.5**; position conf is real. Fine for insertion
  (no arbitration), but don't build a confidence-priority merge without stating it.
- QML consumers assume time-ordered phases (`timeline_labels.cpp:81`,
  `chart_metrics.cpp` segments) — insert-in-order, don't append-then-hope.

## Validation plan (build/test economy: one build per platform, targeted tests once)

1. **Unit**: a `positions_ladder` test beside `src/Analysis/tests/` covering
   mapping, insert-if-monotone, abstain on the non-monotone fixture shape
   (Top==Finish), no-duplicate-anchor, conf/provenance carry-through.
2. **Bytes at rest**: with the key dark, a corpus A/B must be byte-identical
   (`tools/swinglab/parity_diff.py`, excludes only `analysis.timings`).
3. **Corpus A/B on GOLFSIMPC** (`ssh developer@GOLFSIMPC.local`; swinglab exes
   MUST be colocated with the DLL set + `models/` or pose silently no-ops):
   corpus `C:\PinPointStudio\Mark-Liversedge`, prior runs at
   `C:\PinPointStudio\p5p8gate\{base2,cand2}` for baselines. Run
   `parity_run.py` from the NAS `stagegate/` with the key ON; injected-pose runs
   use `--pose-dir stagegate/pose2`.
4. **Coverage delta**: `lab.py coverage <run> --pack core.json` before/after;
   expect p2/p6/p8 blocking rows to drop by roughly the detection rates
   (P2 20/20, P8 17/20, P6 8/20), p5 to follow P6, and record the
   BLOCKED_METRIC increase honestly.
5. **Truth checks**: `score.py` P_CHECKS p5/p6/p8 must pass within tolerance on
   the truth-marked swings; `seg.monotone` must not regress on the 7 known-bad
   ladders (insertion abstains there).

## Success criteria

- COVERAGE.md blocking-phase rows for p2/p6/p8 reduced on the 61-swing corpus with
  zero regressions elsewhere (phase ladders on swings where insertion abstained,
  scores, metric sets byte-identical).
- `seg.monotone` pass-rate ≥ baseline; P_CHECKS p5/p6/p8 within tolerance.
- Dark-key run byte-identical to baseline.

## Out of scope (explicitly)

- Widening the P5 window / independent P6-free P5 bound (stretch, separate).
- The vision ladder's own non-monotone degeneracy (7 swings; latent vision-model
  issue, tracked in the findings doc §4).
- IMU-path P emission (`phase_segmenter.cpp` already emits P5/P6/P8 proxies; it
  has never run on real data — capture gap, corpus_v1 §0.3).
- swinglab_run refusing loudly on empty injected-pose ∩ window (owed hardening,
  findings doc §3 — separate small commit if time allows).
