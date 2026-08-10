# Phase-model collapse repair (anchor-gated Top) — session record

*Executed 2026-08-10 (single session). Lead: the [[p7_impact_geom_impl]] close-out's
successor — `segmentPhases`' Top/fin0 collapse, the measured no-P6 blocker for the
truth trio. Gate runs: `C:\PinPointStudio\p7geo\{trdark,tron,trflip}`.*

## What shipped

`shaft.topRepair.{enabled,minDownswingUs,maxDownswingUs}` — an anchor-gated Top
re-derivation INSIDE `segmentPhases` (shaft_track_assembly.cpp, immediately after
the two-longest top/dsEnd derivation), frozen ON:

- **Gate**: with a supplied impact anchor, fire iff `impactFrame − top <
  minDownswingUs` (120 ms) in frames — a real top can never sit that close to
  (or after) impact; healthy tops sit ≥ ~200 ms before, so the separation is
  structural, not tuned.
- **Re-derivation** inside `[impact − maxDownswingUs, impact − minDownswingUs]`
  (600→120 ms pre-anchor): grip apex (`argmax(−gy)`, the existing 1-run rule,
  now anchor-bounded away from the finish hold) localizes; `spdS` argmin within
  a FIXED 100 ms half-window (the existing 2-run gap rule) pins the dwell.
- `fin0`/`bs0`/onset deliberately untouched (dsEnd is the legitimate
  motion-settle into the finish; the Stage A onset machinery is independent).
  `top' < bs0` is expected in collapse mode (the backswing run lost the
  ranking) and harmless.
- Repairing at the source makes `clamp(impf, top+1, ·)` inert automatically and
  heals every mid-pipeline consumer the impact-geom v1 had to skip: the
  per-frame phase labels (→ DP wmax selection), chirality, wedge
  swingProgress, blur band, tier windows, reconcilePsi — plus the
  event ladder, tempo, locatePTimes windows, and PositionsLadder bounds.
- Diagnostics: `PhaseModel.topPreRepair` (−1 = not fired) rides into
  `ShaftDecideTrace.phases`; swinglab summary keys
  `topRepairApplied`/`topPreRepairFrame`/`topFrame`.

Why anchor-gated rather than geometry-gated: the anchor is a `segmentPhases`
parameter already, measured good (13–22 ms early) on every collapse swing, and
present where the impact geometry must abstain (no accepted address-ball
cluster: 0611_0009, 0705_0001). The geometry arbitration downstream is
unchanged and now mostly resolves "kept".

## Gate evidence (61-swing corpus, pose2-pinned, over `02cf2b0`)

| Check | Result |
|---|---|
| Dark byte-identity (`trdark` vs `flip`) | 61/61 identical |
| Post-flip pure-defaults (`trflip` vs `tron`) | 61/61 identical |
| Repaired swings | 15 (Top −495..−1038 ms; every one had top < 120 ms pre-anchor — physically impossible) |
| P6 recall | 46 → **59** /61 (+13); P5 46 → 59; nonmono 0 |
| Truth trio | 0703_0002: P6 found −6.5 ms; 0705_0001: P7 +247.8 → −13.4 ms, P6 found −3.1 ms; 0611_0009: P7 +361.6 → −6.7 ms, P6 still absent |
| 12 sane truth swings | untouched (identical P7/P6 errors) |
| Coverage | RESOLVED 2751 → 2900 (+149), BLOCKED_PHASE 131 → 40, BLOCKED_METRIC 3462 → 3404 |
| Tempo | fabricated ratios (85.8, 103.5) → sane (1.16, 1.05); 56/61 in the 1–6 band; corpus absents 0 |

The repair fired on 15 swings, not the 8 carrying the exact
Top-within-7ms-of-Impact signature: the 120 ms gate also catches
late-but-not-fully-collapsed tops (all equally implausible, all converted).

## Residuals / successor leads

- **0611_0009 and 0611_0003 still lack P2/P3/P5/P6** with a now-sane ladder
  (Top ~270 ms pre-impact, P8 present): locatePTimes finds no θ horizontal
  crossing in the correct (top, impact) window — downswing θ coverage sparsity
  on the dark-lighting 0611 session. The window repair is done; the signal is
  the frontier (same as the address-ball residual).
- **5 sub-1.0 tempo ratios** (0611_0001 0.6, 0703_0002 0.7, 0704_0007 0.8,
  0705_0007 0.7, 0705_0008 0.5), all repaired swings: with Top and Impact now
  sane, `B = top − addr` comes out too short — the **Address event sits late**
  on these swings. Previously invisible (tempo absent or fabricated); now an
  honest, isolated Address-placement lead.
- The July 10th swing Mark marked up (impact quite a way off) is not in the
  pose2-pinned corpus (`no-run` in score_p7) — needs ingest before it can be
  scored; queued as its own lead.

## Traps confirmed this session

- `shaft_onset_test`'s bridging fixtures use the two-longest MIS-PICK's `top`
  as their observable; the frozen-ON repair heals exactly that, so the legacy
  baseline constructor `darkV3()` now darks `topRepairEnabled` too.
- The pose-injected byte gate stays segmentation-blind: the dark run is
  byte-identical while the repair-on run moves 15 ladders — judge the flip by
  the live-defaults A/B (`tron`/`trflip`), not the dark parity alone.
