# Session Diagnostics — build findings for design (August 2026)

The implementation follows `session_diagnostics_design.md` wherever it and the brief disagree,
per the brief's own rule. These are the places that rule fired, plus what real data showed.
**The mock (`Session Dashboard.dc.html` 12a/12b/12c/13a) needs regenerating against items 1–3.**

## Gate divergences (design doc implemented, mock disagrees)

1. **Established gate.** Design B1 requires ≥2 linked patterns *with links ≥ Coherent*; the mock
   enters Established on `patterns ≥ 2 && casting is one of them`. Implemented per the design doc:
   two patterns joined only at Present-together stay Forming (tested).
2. **Warm-up down-weighting (design §5.4)** is absent from the mock's arithmetic — `_sdRows`
   weighs every shot 1.0. Implemented as designed (first 3 shots × 0.5, injected). The effect is
   not cosmetic: a 5-of-8 condition whose first three shots all fired flips Pattern → Watching
   once weighting is on (Wilson LB 0.3057 → 0.2200). The mock parity test pins the mock's numbers
   with weighting disabled.
3. **Fisher tail.** One-sided, in the edge's authored direction — the edge asserts "upstream makes
   downstream more likely", so a two-sided test spends alpha on a direction nobody authored.
   Pinned: `fisher(5,0,0,5) = 1/252`, `fisher(0,5,5,0) = 1.0`. Confirm this is the intent.

## What real capture data showed (live_measure_source known-groups run)

4. **Coverage is better than the mock's caption.** The richest real capture answers **51 of 152**
   conditions (mock says "23 of 140"); 38 of 109 live measures resolve on it. An LM-only shot
   answers 12. The coverage line publishes real numbers.
5. **The phase ladder is the bottleneck.** The current segmenter emits P1, takeaway, P4, P7,
   finish; every measure reduced at P2/P3/P5/P6/P8/P9 is Unavailable. This is the model leading
   the producers, as intended — but it means chain B-style ghosts are common in practice.
6. **Not-assessable reasons are mechanical, not anatomical.** The pipeline can honestly say
   "metric not produced on this capture", "the phase this reads at was not segmented", "no launch
   monitor data on this shot" — it cannot say "shaft occluded at P6". The 13a reason cells use the
   mechanical vocabulary.
7. **Real sessions are busier than the mock.** Six real shots produced 15 patterns and 18 authored
   chains. The panel draws 2 chains wide (mock's composition) with a "+N more chains" tail, and the
   narrow arrangement collapses all but the first. Whether that is the right N is a design call.
8. **Club context.** The swing-doc pipeline stubs undeclared clubs as DRIVER, so context inference
   never demotes through this seam; a sparse capture grades against driver corridors. A
   `clubOverride` pass-through exists; the honest fix is upstream in the swing doc.

## Small copy/shape decisions awaiting sign-off (flagged `DESIGN-REVIEW` in code)

9. **Link notes the mock never showed**: a Conditionally-dependent-eligible link that fails the
   test reads "tested · not dependent"; below min pairs reads "N paired shots · too few to test".
10. **`screenRequested(screenRef, conditionId)`** is two-arg (a screened-root node has no screen
    ref of its own; the driver's recommendation does). The screen-protocol flow itself remains
    unbuilt per brief §9.
11. **"PAIR CLEANEST vs WORST ▸" omitted** — shot-to-shot comparison is §9 not-designed; a dead
    button would be a stub. Bookends columns render per pattern instead of the mock's flat trio.
12. **Producer UI is minimal**: focus = tap affordance on pattern cards; declared miss = header
    chip + small picker popup. Both need proper design.
13. **Selected carousel cell**: the accent ~10% fill is painted on the pip band, not the whole
    cell — washing the thumbnail fought what the cell exists to show.

## Rejected mock variant (deliberate)

The mock script carries a "hindsight off" review mode (counts "as of this shot"). Brief §6
rejects rewinding; only final-session-state review is implemented.
