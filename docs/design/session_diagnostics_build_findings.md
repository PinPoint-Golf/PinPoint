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
18. **Condition detail — user-requested, shipped minimal, needs a design pass.** Brief §9 lists
    "tapping a chain node through" as not designed; a user asked for it anyway, so it is built
    from the panel's existing vocabulary and invents nothing — the same `PpPatternCard` at the
    head, the same `PpChainRail` / `PpChainNodeCard` / `PpChainLink` for the ancestry and the
    downstream, the same grade words, notes and marks. Four things want sign-off:
    - **Body swap, not a screen.** `detailConditionId` non-empty hides the composition inside the
      same chrome and shows the page; the composition is hidden, never torn down, so BACK
      restores it untouched (asserted by an objectName-visibility snapshot).
    - **One step back, no stack.** Opening a cause or an effect from inside the page re-targets it
      in place; BACK always returns to the panel in one step. Deliberate simplification — a
      breadcrumb four levels deep on a between-balls glance is worse than tapping the card again.
    - **Tap reassignment.** The whole card / whole node used to declare focus; it now opens the
      condition, and the `FOCUS ▸` chip that already rendered became the *only* focus target
      (grown by 5 design px). Consequence to weigh: the 12c slim node draws no chip, so focus
      cannot be declared from the narrow rail at all. The screened root keeps its single verb
      (RUN THE SCREEN) and has no detail affordance.
    - **Composition of the page itself.** Header card → `causeHeadline` + CAUSES rails → rival
      disclosure → `outcomeHeadline` + EFFECTS rails, stacked in one scrolling column, horizontal
      rails wide and vertical at 396. The mock has no frame for any of it; the above/left,
      below/right arrangement the request sketched is not drawn.
    Headlines are the model's and stay inside the panel's honesty rules: the cause line is the
    resolver's ranked root, else the strongest *≥ Coherent* direct parent with its grade word and
    note, else the screen CTA, else "No cause the capture can see today."; the outcome line is a
    count or an explicit "authored; not measurable with this capture" and carries no probability
    language. Outcome anchoring generalised past the declared miss — any outcome-kind condition
    with rows gets its count, the declared miss keeps its own sub-label.

## Found on first real use (2026-08-07, fixed same day) — mock regeneration items

14. **13a at real coverage.** The mock's review strip enumerates 9 conditions; a real capture
    measures ~31, and drawing them all swallowed the panel. Now: cells ordered by information
    (fired → in → unmeasured-here-but-seen), band bounded to ~2 rows, low-information tail
    ("clean all session, not measurable on this swing") collapsed behind one expandable row.
    The headline denominator is the measurable set, matching the grid. **The mock should be
    redrawn at ≥30 conditions** to sanction this composition.
15. **Forming at close.** A session can end without ever establishing (2 patterns, no authored
    edge — common on wrist-only capture). Review previously forced the rail composition via
    ghost scaffolding; now the rail requires the session to have actually reached Established,
    and a Forming close reviews as the mock's Forming picture (full cards, runs, no chain).
    **The mock has no Forming-at-close frame** — worth drawing.
16. **Driver footer at close** is definitive per B7 — the stability debounce is bypassed and
    an edgeless session states "No driver: this session's patterns share no authored cause."
    The mock never drew the no-driver close.
17. Consistent-direction wording is now a count ("The high side on 5 of its 6 firings." /
    "…, every firing."), never a percentage; the agreement % appears only in the dispersion
    sentence, as the mock authored it.

## Rejected mock variant (deliberate)

The mock script carries a "hindsight off" review mode (counts "as of this shot"). Brief §6
rejects rewinding; only final-session-state review is implemented.

## Coverage session addendum (2026-08-09) — what the studio corpus actually showed

The P5/P8 relabel (`48aa1d6`), LM launch repoint (`fb04260`) and swinglab coverage report
(`6ba8ae0`) were validated on GOLFSIMPC against the 61-swing Mark-Liversedge corpus,
baseline `bcf03d8` vs candidate `6ba8ae0`, real pose, `params-refine-on`. Result: **zero
regressions** — phase ladders, scores and metric sets byte-identical per swing
(`C:\PinPointStudio\p5p8gate\{base2,cand2}`, COVERAGE.md in each). Findings that outlive
the run:

1. **No corpus swing has IMU bindings.** All 61 swings are `bindings: 0` — including
   2026-07-10 Wrist_02/swing_0001, the `rich_7iron` fixture: its wrist metrics come from
   POSE, not IMUs. The IMU segmenter (P3/Transition/P6/MaxSpeed/P9, and now P5/P8) has
   therefore NEVER run on a real capture; the vision hands-only model supplies every real
   ladder and emits only P1/(takeaway)/P4/P7/finish. The "phase ladder is the bottleneck"
   coverage gap is a CAPTURE gap first (corpus_v1 §0.3's must-fix), a producer gap second.
2. **The analyzer-layer P-position bridge is the real unlock, now quantified.** On
   camera-only swings the club track already finds P2 on 20/20, P8 on 17/20, P6 on 8/20
   (`analysis.club.positions[]`, refon-live sample); P5 (arm parallel) is derivable from
   pose. Emitting these as PhaseEvents is exactly the deferral recorded in
   shaft_track_assembly.cpp:1768 — doing it would light up the p5(360)/p6(232)/p8(61)/
   p2(61) blocked-measure rows in COVERAGE.md without any new capture.
3. **`--pose-dir` injection silently kills the shaft stage.** Every historical stagegate
   run with injected pose (fidget*, offp*, onp1, final1, refinedark1) has `shaftMs: 0`,
   no club block, no phases, score 0 — those A/B gates never compared phases at all.
   Suspected mechanism: injected pose timestamps fall outside the window's timebase, so
   ShaftTracker's "frames inside pose coverage" early-out fires; not root-caused. Until
   fixed, corpus runs that need phases must run real pose (~7 s/swing on studio CUDA).
4. **Seven swings write a non-monotone vision ladder** (Top==Finish timestamp, Impact
   ~6.5 ms after Finish, flat 0.5 conf; six of seven in 07-05 Wrist_02). Present
   identically in baseline and candidate — a latent vision-model degeneracy, not a
   regression. Consumers that assume ladder order (alternating chart bands, segment
   chips) will misdraw these swings.
