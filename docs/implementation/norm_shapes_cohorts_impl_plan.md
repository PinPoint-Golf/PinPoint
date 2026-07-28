# Norm shapes and cohort keying — implementation plan

Companion to the brief (`docs/design/norm_shapes.md`). The brief is the *what*; this is the
*how*, sequenced, with every repo fact it depends on verified rather than assumed.

**This is a live working document.** Update the Progress table and Session log as stages
land. Work spans multiple sessions with context clears between them — the tables below are
what makes a cold restart possible. Do not treat any stage as done until its gate has
actually run green.

**Anything deferred goes in the ledger at the end of this document, not in the resume
block.** The resume block is rewritten every stage; the ledger is not.

Read before writing anything: `docs/developer/diagnostics_developer_guide.md` (§5 grading,
§8 the live/dormant audit, §13 the traps), then `src/Diagnostics/norm.h` for the doctrine
comments — population norms, asymmetry, the float-edge lesson, NotMeasured ≠ passing. Every
rule in there survives this work.

---

## Progress

| # | Stage | State | Landed |
|---|---|---|---|
| 0 | Ideal-band policy divergence — **land before anything else** | ☑ complete — 79/79 green | 2026-07-28 · uncommitted |
| — | **commit gate · Part A does not begin until Part 0 is in** | | |
| A1 | Shape enum, measure field, parse, validation | ☐ not started | |
| A2 | Grading semantics, edges, plausibility | ☐ not started | |
| A3 | Engine + health checks | ☐ not started | |
| — | **review gate · expect context clear here** | | |
| A4a | Rewire `oneSided` from the unit sniff to shape | ☐ not started | |
| A4b | One-sided domain rules for the bars | ☐ not started | |
| A4c | Corridor editor — handles, readouts, diff rows | ☐ not started | |
| A4d | Corridor editor — plausibility fields and plot regions | ☐ not started | |
| A4e | Wording pass + signal direction picker | ☐ not started | |
| A5 | Seed conversion — smash factor becomes a floor | ☐ not started | |
| — | **merge gate · Part B does not begin until Part A is merged and green** | | |
| B1 | Cohort schema, parse, validation, resolution | ☐ not started | |
| B2 | Cohort provenance threading + surfaces | ☐ not started | |
| B3 | Athlete DOB and sex, age at swing date | ☐ not started | |
| B4 | norms.json header — no cohort content, and why | ☐ not started | |

State vocabulary: ☐ not started · ◐ in progress · ☑ complete (gate green) · ⚠ blocked.

## Session log

Newest last. One line per session: what landed, what the gate said, what the next session
picks up. Keep it factual — this is the handoff, not a summary.

| Date | Stages touched | Outcome |
|---|---|---|
| 2026-07-28 | — | Plan written and verified against the tree. Brief copied to `docs/design/norm_shapes.md`. Baseline: analyzer suite 79/79 green at `ba7b360`. |
| 2026-07-28 | 0 | Stage 0 landed. `bandEdgesOf()` now scales the Ideal edge by `idealMaxZ`; `Norm::idealLo/idealHi` → `claimLo/claimHi`; all 22 call sites split into rendering vs authoring/diff; `MeasureReading::greenLo/Hi` policy-scaled and `fromCorridor()`'s inversion divisor follows; `gradePolicyIsOrdered()` added and asserted. **Three tests had written the defect down as a requirement** — `norm_editor_model_test` ("a policy change does not move the IDEAL band"), `norm_model_test` and `manifest_migration_test` — all three now assert the opposite, plus a per-preset edge sweep and a QML key-contract check. 79/79 green; app builds; headless launch clean. Next: A1. |

---

## ▶ Read this first

**The order is a hard chain, not a preference.**

Part 0 fixes a defect *in the exact functions Part A modifies*. Landing it second would mean
Part A's regression gate was pinned against known-wrong behaviour, and the two changes would
be indistinguishable in the diff when one of them broke something. It goes in on its own
commit.

Part A must be merged and green before Part B starts. Shape and cohort are orthogonal —
*what a corridor means* versus *which corridor resolves* — and share no code path except the
norm key. Interleaving them means every failure has two candidate causes.

**Baseline to hold: 79/79 in the analyzer suite.** Each stage adds to it; none may subtract.

```bash
cmake -S src/Analysis/tests -B build/analyzer-tests
cmake --build build/analyzer-tests --parallel 4
ctest --test-dir build/analyzer-tests --output-on-failure
```

---

## Context

Every norm in the pack is one shape: a (possibly asymmetric) corridor around a target,
`mu ± sigma`, with both tails grading. That is right for ~90% of the catalogue and wrong for
a small, important family.

The live case is smash factor. `m_smashFactor @ driver` is `mu = 1.48, sigmaLo = 0.05`, and
because `readNorm` mirrors `sigmaLo` into `sigmaHi` when absent, that is a **symmetric**
corridor: a golfer approaching perfect energy transfer is graded away from centre for being
too efficient. There is no upper fault in smash factor. There is an upper *implausibility* —
a driver reading of 1.62 is not a swing finding, it is a mis-tracked ball — and grading it
either way launders a capture fault into a confident diagnosis.

So the work introduces two things:

- **shape** (`target` / `floor` / `ceiling`) as a property of the MEASURE, because
  one-sidedness is semantics of the quantity and invariant across contexts;
- **plausibility bounds** on the norm row, because the physical cap *is* context-dependent
  (it is loft-dependent), and because the far edge of an open tail is plausibility, not
  infinity.

Part B is separate and orthogonal: an optional **cohort** — sex plus a closed age vocabulary
— as part of the norm key. This is the mechanism a recorded blocker has been waiting for.
**16 shipped rows rest on male-only samples**: Meister 2011 (10 pro + 5 amateur men) seats
every thorax/pelvis rotation corridor, and Kim 2018 (11 male pros) seats ball position and
spine bend. Nothing in the schema records that, and the single-parent context tree cannot
express a sex axis orthogonal to club.

---

## Settled decisions — do not relitigate

1. **Shape is a property of the MEASURE, not the norm.** If shape sat on the norm, a driver
   row and an iron row could disagree about the shape of one measure, and the corridor editor
   could flip shape per context. Norm rows carry only numbers; `validateNormsAgainst` checks
   those numbers against the measure's shape exactly the way it already checks `unit`.
2. **No new grade bands.** `Ideal/Good/Watch/Action/NotMeasured` and `ragOf` are unchanged.
   Shape changes which values reach which band, never what the bands are.
3. **`GradePolicy` is unchanged and stays pack-wide.** A floor norm consults the same z
   ladder on its single graded tail.
4. **No mixture/bimodal shape, no transform functions.** Multimodality is what the context
   tree is for (the archetype rows prove it); skew is what asymmetric sigma approximates.
5. **Sequencing is NOT a norm shape.** Ordering lives in `SignalTest::Order`. The continuous
   companions of the kinematic sequence are future *measures* with ordinary `target` norms
   once producers exist. Nothing here touches `Order`.
6. **Cohort-relative bigger-is-better measures get NO norm.** Ball speed, club speed, carry:
   a population floor on these grades a golfer Action for their age. `floor` is for
   self-normalising ratios and mechanically-universal thresholds only.
7. **Cohort is NOT a context node.** The context tree describes the SHOT; age and sex are
   properties of the ATHLETE. Folding athlete attributes into the shot tree would force the
   cross-product through a single-inheritance walk that cannot express orthogonal dimensions
   without duplicating rows.
8. **No per-athlete norms, ever.** A cohort is a segmented *population*. The inviolable rule
   is reworded from "one population for everyone" to **"never keyed by athlete id"**.

### Decisions taken during planning

| Decision | Choice | Why |
|---|---|---|
| Name for the norm's own claim | `Norm::claimLo()` / `claimHi()`; `NormBandEdges::idealLo/idealHi` keeps its name and becomes the **policy-scaled** band | the two ideas must stop sharing a name; the norm's claim must not move when a user changes a sensitivity setting |
| Open-side numeric edge | **`mu`**, the aspiration point, with `highOpen`/`lowOpen` set | never infinity, never NaN, never an absent key. A flag-blind renderer then draws green from the graded ideal edge up to the aspiration — true, and non-degenerate (span = k×sigmaLo) |
| One-sided editing | **draggable centre + one edge handle** | on a floor the headline number is `mu` ("at least 1.48"), so it must be the thing you manipulate; the graded edge sets the tolerance |
| Plausibility validation reference | explicit monitor bound when present, else the **widest** shipped preset (`lenient`, watchMaxZ 3.5) | `validateNormPack` is standalone and has no policy, and the invariant must hold under every policy a user can select |
| `m_leadHandWidth` | annotate-only, alongside the brief's four | previously flagged as penalising a wider arc; the brief does not list it. Author's call, recorded not taken |
| `m_smashFactor @ any` | gets `plausibleHi` 1.56 | the physical cap across clubs, so an unknown-club shot still rejects impossible readings |

---

## Repo facts this plan is built on (verified, not assumed)

1. **`normZ()` has zero production call sites** — `src/Diagnostics/tests/norm_test.cpp` only.
   `withinBand()` has zero external call sites; it is used only inside `grade()`. Making them
   shape-aware is correct for the future 0–100 score, but nothing in the app reads them today.
2. **`readNorm` mirrors `sigmaLo` into `sigmaHi` when absent** (`norm_pack.cpp:338-340`).
   Only 2 of 149 rows state `sigmaHi`. So "a floor with `sigmaHi ≠ sigmaLo` is an error" is
   enforceable, and an explicitly-equal value is indistinguishable from the default and
   harmless.
3. **`hasExplicitMonitor()` requires BOTH bounds** (`norm.h:150`). A floor carrying only
   `monitorLo` would be silently ignored by `grade()` *and* `bandEdgesOf()`. It must become
   shape-aware, and `partialMonitor` must not fire on a legal one-sided monitor.
4. **`MeasureReading` carries no Watch edge** — only `greenLo/greenHi`
   (`characteristic_engine.h:47-53`). The engine's `onTail` test (`:121`) can compare against
   the Ideal edge and nothing else.
5. **The engine is correct today only by accident.** `deviated` comes from `grade()` (which
   uses `policy.idealMaxZ`) while `onTail` compares against `mu ± sigma` (which does not).
   They agree only because every shipped preset has `goodMaxZ >= 1`. Part 0 makes that an
   asserted invariant instead of a coincidence.
6. **`MeasureReading::fromCorridor()` inverts a band to a Norm** as
   `sigma = (greenHi − greenLo)/2` (`characteristic_engine.h:86-87`), which assumes the input
   band is ±1σ. Part 0 changes what greenLo/greenHi mean, so the divisor must follow.
7. **`detect()`, `relation_resolver` and `NormMeasureSource` are DORMANT** — compiled into
   the app, no caller outside their own tests (developer guide §8.2). Tests are the only gate
   on the engine stage. Do not claim a live verification.
8. **`m_smashFactor` is `status: externalDevice`** — it needs a launch monitor. The seed
   conversion is content-only and cannot be verified on a live swing.
9. **`chart_metrics.cpp:338-341` reads corridor edges with `.toDouble()`**, so a missing
   QVariantMap key silently becomes `0.0`. Openness must be an explicit boolean at every hop,
   never an absent key.
10. **`railRange()` already solves the open-tail axis problem correctly**
    (`dashboard_reductions.h:129-164`) — it drops the upper bounds so an aspirational ceiling
    does not crush the trace, and `take()` already skips non-finite values. It is
    **high-tail-only**: `amberLo`/`greenLo` are always taken, so a ceiling has no
    representation. Same in `PpBandRail.qml:314-317`.
11. **One-sidedness is currently decided by string-matching a unit**
    (`PpDashboardMotionZone.qml:72-75`). A presentation-layer heuristic standing in for a
    semantic property of the measure.
12. **`SwingScorer` is the second independent invention of the same clamp** —
    `if (b.oneSidedDir > 0 && z > 0) z = 0.0;` (`swing_scorer.cpp:126-127`), tunable via
    SwingLab `score.<metric>.oneSidedDir`. It is **dead code**. Leave it frozen; do not add a
    third.
13. **`NormativeBar.qml` has no finite guards at all** (`:41-45`) and no green fallback —
    weaker than `PpRangeBar.qml:62-73`, which does guard but whose `_fx()` maps a non-finite
    value to `0`, i.e. the *left* edge.
14. **`setIdealBand` defines `mu` as the midpoint of two handles**
    (`norm_editor_model.cpp:243-245`). This is the hard blocker for one-sided editing.
15. **`NormBasis` compares mu/sigmaLo/sigmaHi/monitorLo/monitorHi**
    (`diagnostics_health.cpp:231-236`). Without the plausibility pair, a shipped row that
    gains a cap compares as unmoved and `overrideCoreChanged` stays silent.
16. **There are no QML tests anywhere in this repo** — no `qmltest`, no `QuickTest`, no
    `tst_*.qml`. Every surface is covered only through its C++ model. A rule left inside a
    binding is a rule nothing can test.
17. **`directionOptions` emits `{name,label,means,sentence}`**
    (`characteristic_editor_model.cpp:678-693`) with no `enabled`. `TailChip`
    (`CharacteristicEditor.qml:89-118`) has **no disabled state**. The nearest precedent for
    a greyed option is `drawFromOptions` (`norm_editor_model.cpp:450-469`), which carries
    `enabled` but **no reason**; the doctrine on reasons is `DagView.qml:176-199` — *"a greyed
    box with no explanation is indistinguishable from a rendering fault"*.
18. **The athlete record is QSettings**, `athletes/<uuid>/<key>`, no struct — a `QVariantMap`
    built in `athlete_controller.cpp:105-140`. Name, handedness, height, weight, handicap,
    primaryClub, speedTarget, notes, timestamps, clubs. **No DOB, no sex, nothing
    demographic.** "junior" appears once in `src/`: a notes placeholder
    (`ScreenAthleteForm.qml:587`). "cohort" appears zero times.
19. **`swing.json` already carries `athlete.uuid` and `clock.wallclock`**
    (`swing_exporter.cpp:726-746`) — everything age-at-swing-date needs. But
    `SwingSummary`/`PersistedShot` (`swing_doc.h:57-94`) **drop the athlete block on
    read-back**; only `swing_data_source.cpp:653-658` re-reads it, for the properties panel.
20. **`INormProvider::resolve()` is non-virtual, defined once**
    (`resource_norm_provider.cpp:30-61`), over `NormPack::find(measureId, contextId)`
    (`norm_pack.cpp:162`). That pair is where cohort lands.
21. **Two incompatible norm-key spellings already exist**: `"%1 @ %2"` with spaces
    (`norm_pack.cpp:205`) versus `measure + '@' + context` with none
    (`diagnostics_health.cpp:40`). `characteristic_library_model.cpp:938-945` splits on the
    first `@`, so the norm_pack form already leaves stray whitespace on the health view's
    deep-link.
22. **Every live UI norm lookup resolves at `full_swing` today** — `contextFromMap()`
    (`metric_catalog.cpp:62-80`) never sets `band.contextId`, only the legacy ints. Cohort
    provenance will have little to show on the dashboard until context wiring lands.
23. **All tests register in one file**: `src/Analysis/tests/CMakeLists.txt`. There is no
    `src/Diagnostics/tests/CMakeLists.txt`. Shipped content is reached via `pp_norm_env()`
    and `-DPP_CORE_PACK_PATH`.

---

## Stage 0 — the Ideal-band policy divergence

**Land this first, on its own commit.** A pre-existing defect, found while designing this
work, in the exact functions Part A modifies.

### The defect

`bandEdgesOf()` (`norm.h:245`) sets `idealLo = mu − sigmaLo`, `idealHi = mu + sigmaHi`,
ignoring `policy.idealMaxZ`. `grade()` (`norm.h:206`) uses
`withinBand(value, norm, policy.idealMaxZ)`. Under `standard` (idealMaxZ = 1.0) they
coincide, which is why nothing has caught it. Under the other two shipped presets:

| preset | idealMaxZ | symptom |
|---|---|---|
| `strict` | 0.75 | a value at 0.9σ is DRAWN inside the green band and GRADES `Good`; `ragOf(Good)` is Amber. Green band, amber chip, one number. |
| `lenient` | 1.5 | a value at 1.3σ is drawn outside green and grades `Ideal`. |

This is the disagreement `withinBand`'s own comment exists to eliminate, at whole-band scale
rather than float-epsilon scale. The `norm.h` claim that the Ideal band is policy-independent
is true of the drawing path and false of the grading path.

### The work

1. `bandEdgesOf()` applies `policy.idealMaxZ` to the ideal edges the same way it already
   applies `watchMaxZ` to the watch edges. One edge, computed one way, on both paths.
2. `Norm::idealLo()/idealHi()` → **`claimLo()/claimHi()`**, documented as the norm's own
   claim: policy-free, and it must not move when a user changes a sensitivity setting.
   `NormBandEdges::idealLo/idealHi` keeps its name and becomes the policy's Ideal band.
3. Audit every call site (table below). Each becomes a deliberate choice between "the norm's
   claim" and "what this policy grades as Ideal".
4. `MeasureReading::greenLo/greenHi` become policy-scaled, so the engine's `onTail` and
   `deviated` checks finally derive from one scale.
5. `MeasureReading::fromCorridor()`'s inversion divisor becomes `2 × policy.idealMaxZ`
   (fact 6).
6. Add the preset ordering invariant with a test: `goodMaxZ >= idealMaxZ`,
   `watchMaxZ >= goodMaxZ`, all three positive — rather than leaving it a property of three
   hand-written presets.

### Call-site audit — complete

**Rendering — take the policy-scaled Ideal edge:**

| Site | Feeds |
|---|---|
| `norm_measure_source.h:94-95` | `MeasureReading::greenLo/greenHi` → engine `onTail` |
| `norm_model.cpp:389-390` | measures-list row |
| `norm_editor_model.cpp:559-560` | import-candidate rows |
| `norm_model.cpp:441-442`, `norm_editor_model.cpp:764-765` | `goodLo/goodHi`, computed **inline** rather than via `bandEdgesOf` — two more sites, easily missed |
| `metric_corridor.h:88` · `reference_bands.cpp:72` · `norm_model.cpp:436` · `norm_editor_model.cpp:759` | already via `bandEdgesOf` — inherit the fix |

**Authoring / diff — take `claimLo()/claimHi()`, behaviour unchanged:**

| Site | Why |
|---|---|
| `norm_editor_model.cpp:258-259` `nudgeIdealLo/Hi` | the editor's handles ARE the claim |
| `norm_editor_model.cpp:789-790` | parent diff |
| `norm_editor_model.cpp:810-811`, `:821-822` | shipped diff + "core has moved" |
| `norm_model.cpp:467-468` | shipped diff |
| `norm_pack.cpp:253-259` `monitorExcludesIdeal` | validation must stay policy-free |
| `diagnostics_health.cpp:251-254` `overrideCoreChanged` | three-way diff against `NormBasis` |

### What changes on screen

- **Corridor editor** (`CorridorEditor.qml:486-496`): the read-only "what the grade policy
  makes of it" line gains the policy-scaled Ideal edge alongside Good and Watch. Under
  `strict` the handles now sit *outside* the drawn green core, and the panel says why. Under
  `standard` — the shipped default — **nothing visibly changes anywhere**.
- **Measures & norms, metric detail, dashboard rails, wrist grid**: identical under
  `standard`; correct rather than contradictory under `strict`/`lenient`.

### Gate

- Per shipped preset, `bandEdgesOf` and `grade` agree at both ideal edges to ±1e-9. Extend
  the existing sweep (`reference_bands_test.cpp:283-345`) to run per preset, not only under
  `standard`.
- A value inside the drawn green band never carries an Amber chip under any preset.
- The preset ordering invariant.
- 79/79 still green.

---

## Part A — shapes

### A1 — Shape enum, measure field, parse, validation  ◄ **the regression gate**

- `Shape { Target, Floor, Ceiling }` in `characteristic.h`, with `shapeName`/`shapeFromName`
  via the `Row<E>{value,name,label}` table convention (`characteristic.cpp:78-82`, one-liners
  at `:173-174`).
- `Measure::shape`; JSON key `"shape"` on the measure entry in `core.json`; absent ⇒
  `target`.
- **Every grading function takes `Shape shape = Shape::Target` as a trailing parameter**, so
  `reference_bands_test` and the whole existing suite pass UNTOUCHED. That is the gate: this
  stage adds vocabulary and changes no behaviour.
- `validateNormsAgainst` gains shape rules, in the existing style (both sides named): a floor
  norm whose `sigmaHi ≠ sigmaLo` is an error; a `monitorHi` on a floor is an error; an unknown
  shape token is an error. Mirror for ceiling.

Semantics, exact, for `Floor` (mirror everything for `Ceiling`):

- `normZ`: `value >= mu → 0`; else `(value − mu) / sigmaLo` (negative). Continuous at `mu`.
  The good side reports **z = 0**, not a raw positive distance — a future 0–100 score must not
  reward overshooting a floor.
- `withinBand`: `value >= mu − threshold × sigmaLo`, computed-edge, inclusive.
- `grade`: same ladder, one tail. Only `monitorLo` is legal; `value < monitorLo → Action`,
  else capped at Watch inside it.

**Screen: nothing changes.** This stage is invisible by construction.

### A2 — Grading semantics, edges, plausibility

- Implement the `Floor`/`Ceiling` branches of `normZ` / `withinBand` / `grade`.
- **`hasExplicitMonitor()` becomes shape-aware** (fact 3), and `partialMonitor` must not fire
  on a legal one-sided monitor.
- `plausibleLo` / `plausibleHi` on the Norm row, parsed via the existing `readOptionalDouble`
  helper, written omit-when-absent like the monitor bounds, and allowed to appear singly.
  Outside `[plausibleLo, plausibleHi]` ⇒ `Grade::NotMeasured` **plus a distinct `implausible`
  flag**. `ragOf(NotMeasured)` stays Grey.
- **Plausibility validation** (all shapes): where a plausible bound and a watch edge both
  exist on a side, the plausible bound must lie at or outside the watch edge — a corridor must
  never extend into implausible territory. Reference edge: the explicit monitor bound when
  present, else the **widest** shipped preset (`lenient`, watchMaxZ 3.5), so the invariant
  holds under every policy a user can select.
- **`NormBasis` gains `plausibleLo/Hi`** (fact 15), written at
  `norm_editor_model.cpp:612-629` and compared at `diagnostics_health.cpp:224-254`.
- `NormBandEdges` gains `bool lowOpen, highOpen`; the open-side numeric edge is **`mu`**.
  `MetricCorridor` gains the same, propagated in `corridorForMetricAtPhase`.
  `marginOverride` widens the graded side only.
- ⚠ Fact 9: openness is an explicit boolean at every hop.

**Screen: nothing changes** — no shipped measure is one-sided yet, and no shipped row carries
a plausibility bound. That is deliberate: A5 is the first stage with a visible effect.

### A3 — Engine + health checks

- `MeasureReading` gains the openness flags and `implausible`.
- In `OutsideCorridor`, a signal whose `Direction` points at the OPEN tail can never fire —
  `fired = false` at runtime (`characteristic_engine.cpp:121`). Same for the `Ratio` branch
  (`:151`).
- At validation time it is a `diagnostics_health` finding: *"signal watches a tail the norm
  never grades"*. An author who wrote it has misunderstood the measure. Three edits per the
  developer guide §11: the code table in `diagnostics_health.h:39-82`, the producing block,
  and the label in `HealthView.qml:121-136`.
- ⚠ Fact 7: the engine is dormant. Tests are the only gate here; say so rather than implying
  a live verification.

**Screen:** a new row can appear in Settings → Diagnostics → Causes & health. Nothing else.

### A4a — Rewire `oneSided` from the unit sniff to shape

The smallest and most valuable of the surface stages, because it *deletes* a heuristic.

- Delete `PpDashboardMotionZone._isOneSided()` (`:72-75`).
- Feed one-sidedness from the measure's shape through `MetricCorridor` →
  `metric_catalog.cpp:227-262` (`normative.corridors`, plus the metric-level `normative` block
  at `:285-301`) → `PpBandRail.oneSided`.
- `railRange()`'s parameter becomes a shape rather than a bool, and gains the **ceiling
  mirror** (fact 10). Same for `PpBandRail.qml:314-317`, which substitutes `_rHi` for the high
  edges and never touches the low ones.
- Where a metric's corridors at different phases disagree on shape, the rail falls back to
  `target` and the disagreement is a health finding — never a silent choice.
- `SwingScorer` stays frozen (fact 12).

**Screen:** the post-shot dashboard Motion zone. Speed rails keep the exact rendering they
have today — they simply arrive at it from the measure instead of from a string comparison —
and any speed metric whose unit is spelled differently stops being silently two-sided.

### A4b — One-sided domain rules for the bars

`NormativeBar.qml` (metric detail, one bar per phase) and `PpRangeBar.qml` (dashboard Setup
zone) both derive their axis domain from the amber span, which is open on a one-sided norm.

- Explicit one-sided domain rule for each: anchor on the graded edge, extend the open side by
  a fixed fraction of the graded span past the furthest of (value, ideal edge), and render
  that side as a fade running off the track.
- Re-check the degenerate-span fallbacks under the new rule (`PpRangeBar.qml:66-73`).
- `NormativeBar` gains the finite guards it does not have (fact 13).
- `PpDashboardSetupZone.qml:115-134` feeds `PpRangeBar` and has no one-sided path at all —
  it inherits the fix, but audit the `orientationLabel` call at `:131`:
  `dashboard_reductions.h:226` is open/closed/square and two-sided by construction. Every
  alignment measure stays `target`, so this is an audit, not a change.

**Screen:** metric detail's normative bars and the dashboard Setup tiles. No visible change
for any `target` norm; a one-sided norm draws a bar that runs off its open end instead of a
collapsed or absurd one.

### A4c — Corridor editor: handles, readouts, diff rows

The bulk of the work. The blocker is fact 14.

- Add `setAspiration(mu)` / `nudgeGradedEdge(v)` beside `setIdealBand`. The centre mark
  (`CorridorEditor.qml:253-259`, today a static tick) becomes draggable and is the headline
  number.
- The handle `Repeater` (`:276-308`) becomes one-element on a one-sided norm. **The dead
  handle is ABSENT, not disabled** — a disabled handle invites "why can't I drag this?" on
  every visit. The grab's `pick()` / swap-follow logic (`:333-353`) simplifies accordingly.
- Shape shown read-only with the measure's `highMeans` beside it, so the author reads "higher
  is better: more of the clubhead speed reaching the ball" rather than a bare enum word.
- The open side draws to the plot edge with a fade and an explicit end-cap label ("no upper
  limit" / "no lower limit"). It must never terminate in a hard edge that reads as a bound.
- Numeric entry, keyboard nudge and monitor editing: only the graded side exists.
- One-sided forms for the band-width readout (`:867-873`, two `hi − lo` spans in one string),
  the import-candidate "%1 to %2" (`:769`), and the parent/shipped diff rows. `NormBasis`
  comparison ignores the ungraded side.
- Seat-from-swings on a floor: `mu` = sample median, `sigmaLo` = median − 16th percentile
  (robust); record `n` and provenance as today. The UI must not offer to seat the ungraded
  side, and `seatFromSample`'s borrow-the-other-side fallback (`:515-523`) must not fire.

**Screen:**

```
FLOOR — smash factor, driver
  ╭─────────────────────────────────────╮
  │  amber ▓▓▓▓▓▓▓▓▓▓                   │
  │  green      ░░░░░░░░░ ▸fade▸        │
  │            ╷         ╷              │
  │            ●         ┃              │
  │         handle     centre           │
  │       (tolerance)   (mu)            │
  ╰─────────────────────────────────────╯
     AT LEAST  [ 1.48 ]  ratio
     TOLERANCE [ 0.05 ]
           "no upper limit"
```

A `target` norm keeps the two handles and the static centre tick exactly as today.

### A4d — Corridor editor: plausibility fields and plot regions

Plausibility bounds are authored data, not a compiled constant, so the editor must expose
them.

- An optional pair of numeric fields in an **"Implausible beyond"** group, clearly separated
  from the corridor handles because they answer a different question — *is this reading real?*
  — not a grading question — *is this swing good?*
- Drawn on the plot as hatched or dimmed regions OUTSIDE the corridor, visually distinct from
  Action.
- Live validation against the watch edge, inline, at the moment of the mistake — consistent
  with how the facet picker reports errors (`MeasurePicker.qml:479-489`).

**Screen:** a new field group in the corridor editor, below the numeric readouts; two new
shaded regions at the extremes of the plot.

### A4e — Wording pass + signal direction picker

Eleven format strings assume two finite edges:

| Site | String |
|---|---|
| `MeasureDetail.qml:308` | `"%1 to %2"` (the norm row) |
| `MeasureDetail.qml:345` | `"· edited, ships %1 to %2"` |
| `MeasureDetail.qml:368` | `"· action beyond %1 to %2"` ×2 |
| `MeasureCatalogue.qml:671` | `"%1 – %2"` |
| `CorridorEditor.qml:490` | `"Good to %1 – %2 · action beyond %3 – %4"` |
| `CorridorEditor.qml:769` | `"%1 to %2"` (import candidate) |
| `CorridorEditor.qml:867` | `"%1 sets %2 to %3. This corridor is %4 %5 wide against its %6."` |
| `norm_editor_model.cpp:820` | `"You changed this. PinPoint ships %1 to %2 %3."` |
| `diagnostics_health.cpp:247` | `"You overrode %1 when the shipped corridor was %2 to %3…"` |
| `norm_pack.cpp:255` | `"…monitor band (%2..%3) that does not contain its own ideal band (%4..%5)."` |
| `norm_pack.cpp:263` | `"…has no tolerance on one side, so only its exact centre grades Ideal."` |

Findings, grade chips, MeasureDetail's norm row and the DAG/characteristic detail text must
never say "above the corridor" for a floor or "below" for a ceiling. The norm row reads
**"at least 1.48"**, not "1.48 to 1.53".

**Implausible readings get their own wording**, distinct from both a grade and from "not
measured": something in the register of *"reading outside the plausible range — check the
capture"*, with the reading **shown** rather than hidden. Never merge it into "not measured"
(a capture gap) or into Action (a swing finding). This is the same distinction `norm.h`
already makes between NotMeasured and a passing grade, one level further out.

Direction picker: `directionOptions` gains `enabled` + a reason per option (fact 17);
`TailChip` gains a disabled state it does not have. Follow `drawFromOptions` for the model
shape and `DagView.qml:176-199` for the doctrine — the reason goes in a caption beside the
chip, not inside it.

**Screen:** Measures & norms (norm rows read one-sided), the corridor editor (readouts and
diff rows), the characteristic editor (one tail chip greyed with a caption saying why), and
wherever an implausible reading surfaces.

### A5 — Seed conversion  ◄ **the first visible content change**

`m_smashFactor` gains `"shape": "floor"` on the measure. Norm rows keep mu/sigmaLo as they
are; add per-context plausible caps — heuristic, with the physics-of-loft note in the
`citation` field, **no commercial sources**:

| context | mu | sigmaLo | plausibleHi |
|---|---|---|---|
| `any` | 1.40 | 0.08 | 1.56 |
| `driver` | 1.48 | 0.05 | 1.56 |
| `iron` | 1.38 | 0.06 | 1.45 |
| `wedge` | 1.20 | 0.10 | 1.32 |

Behavioural delta to assert (driver):

| value | before | after |
|---|---|---|
| 1.55 | Good | **Ideal** |
| 1.62 | Watch | **NotMeasured (implausible)** |
| 1.30 | Action | Action |

⚠ Fact 8: the measure is `externalDevice`, so this is content-only and unverifiable on a live
swing. The behavioural test is the gate.

**Annotate in the norms.json header, but DO NOT convert** — author's call pending:

| measure | why it is a candidate | why not now |
|---|---|---|
| `m_leadHeelLiftTop` | ceiling candidate, domain [0,∞) | author's call |
| `m_leadHandWidth` | 65 ±10 "% arm length" penalises a wider arc | author's call; `planned`, so inert either way |
| `m_handSpeedP6P7` | | sign convention unresolved — deceleration vs release-drag |
| `m_lagAngleDown` | | genuinely two-sided: over-retention → flip |
| `m_xFactorStretch` | | stays target: the high tail carries lumbar-load risk |

Also record in the header that **cohort-relative bigger-is-better measures get no norm at
all** — ball speed, club speed and carry are benchmarks, a different future feature, and a
population floor on them would grade a golfer Action for their age.

**Screen:** the smash factor rows in Measures & norms read "at least 1.48"; the corridor
editor opens them one-sided; a driver smash of 1.55 now grades Ideal.

### Part A gate

- `norm_test`: floor/ceiling grading tables; continuity at `mu` (z = 0 from both sides);
  good-side z clamps to 0; ±1e-9 edge sweep on the single computed edge; monitor precedence on
  a floor; plausible → NotMeasured with the flag set; zero-sigma degenerate one-sided.
- Pack validation: `sigmaHi` differs on a floor, `monitorHi` on a floor, a plausible bound
  inside the watch edge, an unknown shape token — each a named load error in the existing
  style, both sides named.
- Engine: a High-direction signal on a floor norm never fires and is a health finding; a
  Low-direction signal fires exactly as a target norm's low tail would.
- Corridor: openness flags propagate through `corridorForMetricAtPhase`; `marginOverride`
  widens one side only.
- The smash behavioural-delta test.
- **Model-level tests for the derived values the QML binds to** — the one-sided domain rules,
  the one-sided readout strings, and the plausibility field validation. Fact 16 is why: a rule
  that lives only inside a `.qml` binding is a rule nothing can test.

---

## Part B — cohort keying

Gated on Part A merged and green.

### B1 — Schema, parse, validation, resolution  ◄ **all shipped rows unqualified**

- `Cohort { std::optional<Sex>, std::optional<AgeBand> }` on the Norm row; JSON
  `"cohort": { "sex": "male"|"female", "age": "adult_55_64" }`. Either field, both or neither;
  absent `cohort` ⇒ unqualified, matches everyone. Parse follows the `basedOn` precedent
  exactly (`norm_pack.cpp:360-374`).
- **The age vocabulary is CLOSED and hierarchical.** A fixed enum in code, stable tokens,
  changed only by a schema version bump — never extended by pack content:

```
junior                    under 18
adult                     18+          — authorable in its own right
  adult_18_54
  adult_55_64
  adult_65plus
```

  Half-open, gapless, total. Boundary rationale for the header comment: **55** is the seniors
  threshold in UK club practice, so a golfer already knows which side of it they are on and a
  coach does not have to explain the band. It sits above the point where the trunk-rotation
  decline literature usually pivots, and that is deliberate — a band boundary set later than
  the physiological one means a 55+ row describes a population that has unambiguously started
  to decline, rather than straddling the onset. `junior` is deliberately one band and is the
  weakest of them; a 9- and a 17-year-old are barely one population, but there is no junior
  corpus to split against. If one arrives it splits at peak height velocity (~14), as a
  version bump.
  `adult` is authorable, not merely a parent, because most provenance is no better than
  "adult male" or "adult female". Without it the common case would force three duplicate rows
  that then drift.
- **A study whose range does not match a band is mapped by the AUTHOR to the nearest band,
  with the study's actual range recorded verbatim in `citation`.** The key stays comparable;
  the provenance stays honest. Never widen the vocabulary to fit a paper.
- **Bump `kNormPackSchemaVersion` to 2.** An older build silently dropping a `cohort` key
  would grade everyone against a female-65+ row — exactly what `schemaTooNew` exists to
  prevent.
- `NormPack::find/contains/upsert/remove` (`:162-199`) and `normKeyLabel` (`:205`) gain the
  cohort axis. **Reconcile the two `@` spellings while you are there** (fact 21) and fix the
  split in `characteristic_library_model.cpp:938-945`.
- **Resolution: one walk, context-major.** At each node of the existing upward context walk,
  probe the cohort keys in a FIXED order, most specific first, and take the first row present.
  Only if none is present move up the tree:

```
1. sex + exact age band          (female, adult_55_64)
2. sex + adult                   (female, adult)          — skipped when the athlete is junior
3. exact age band                (adult_55_64)
4. adult                         (adult)                  — skipped when the athlete is junior
5. sex                           (female)
6. unqualified
```

  A fixed probe order rather than a specificity score with a tie rule: a community pack must
  never fail to load, or resolve unpredictably, because two rows were equally specific. Age
  band sits ahead of sex at equal specificity (3 before 5) because the age effect on the ROM
  measures where cohorts matter is larger and monotone, while sex differences are partly
  absorbed by the body-normalised units the setup measures already use.

  Two consequences, both intended: an unqualified `driver` row beats a senior row at `any` for
  a driver shot (stance width is club-mechanical; if senior-driver matters, author it); and a
  senior row at `any` resolves for a senior wherever no club-specific row exists — which is
  the ROM family, exactly where cohorts matter.
- `validateNormsAgainst` gains: unknown cohort token (named in full, both the token and the
  legal vocabulary); a `junior` row combined with an adult sub-band; duplicate rows on one
  (measure, context, cohort) key. Where both a sex-only and a band-only row exist at one node
  with no combined row, `diagnostics_health` raises a **WARNING** naming both and inviting the
  combined row — a nudge, never a load failure.
- **Regression gate: every existing resolution answers identically; the full suite passes
  untouched.**

**Screen: nothing changes.** All shipped rows are unqualified.

### B2 — Provenance threading + surfaces

`NormResolution` gains cohort provenance — which cohort answered, alongside the existing
context/inherited/overridden fields — threaded through `MetricCorridor`
(`metric_corridor.h:90-101`, one line) and `MeasureReading`, and into QML at
`metric_catalog.cpp:243-249`.

Corridor surfaces must be ABLE to say "graded against: men 60+". A golfer whose grades
improve after entering their DOB must read that as the corridor becoming right for them, not
the app going soft.

- `MeasureDetail.qml:326-392` — the `· `-prefixed provenance row is the natural slot,
  marshalled from the single point `NormModel::normAt` (`norm_model.cpp:412-472`).
- `MetricDetail.qml:67-78` `_normProvenance()`.
- The corridor editor: cohort is part of the row identity it edits; creating a
  cohort-qualified override from an unqualified base records the base in `NormBasis` as today.
- The dashboard does not need it.
- ⚠ `MeasureDetail.qml:51` `signal editCorridor(measureId, contextId)` must carry the cohort,
  or the editor opens the wrong row.
- ⚠ Fact 22: there will be little to show until context wiring lands. That is expected, not a
  defect in this stage.

**Screen:** one more `· `-separated term on the MeasureDetail norm row and the MetricDetail
provenance line, empty for every shipped row.

### B3 — Athlete DOB and sex, age at swing date

- DOB and sex as **optional** fields on the athlete record (fact 18). Extend
  `athlete_controller.cpp:27-42` (keys), `:105-140` (reload), `:179-252` (`saveAthlete`, or
  route through the existing generic `updateAthlete(uuid, field, value)` at `:272-292`), and
  `ScreenAthleteForm.qml` `resetForm`/`loadForEdit`/`doSave` (`:58-127`) plus a field block in
  the **Recommended** section.
- **The band is derived from DOB at the SWING date, never stored**: an athlete ages across
  their own history, and a stored band would grade old swings against today's cohort.
- Unknown DOB, unknown sex, or a declined sex answer ⇒ only rows unqualified on that axis
  match, and the reading **STILL GRADES** — never NotMeasured. Demographics gaps degrade to
  the universal corridor. *"We don't know your age"* and *"we could not assess this"* are
  different statements.
- ⚠ **Fix the read-back gap in this stage** (fact 19). Without it, offline re-analysis
  resolves a different cohort from the live path on the same swing — a silent grade
  divergence, which is exactly the class of defect this codebase's doctrine exists to prevent.

**Screen:** two new optional fields in the athlete form's Recommended section — date of birth
and sex, the latter with a decline option. Nothing else, until cohort content exists.

### B4 — norms.json header

Record in the header that cohort rows arrive with the ROM literature review (thorax rotation,
x-factor, wrist ranges — the age-decline and sex-difference cases the literature actually
supports), and that setup measures are largely cohort-invariant because they are already
body-normalised (% shoulder width, % stance width) while smash is self-normalising and stays
universal — **so nobody pads cohorts for symmetry**.

Amend the doctrine comments in the same change: `norm.h:37-41` and `norms.json:6-8` both say
norms are keyed "(measure, context) — never by athlete". The inviolable rule becomes **"never
keyed by athlete id"**; a cohort is a segmented population, not a person. Restate there that
cohort shifts the corridor and **never inverts valence** — same rule as context — and that
band-edge cliffs are accepted: a golfer's grade can shift on their 55th birthday, and there
is no interpolation, because interpolation implies a continuity the banded literature does
not support.

### Part B gate

- The six-step probe order, exhaustively: every combination of rows present/absent at one node
  resolves to the expected row, with no configuration ambiguous.
- Context-major precedence: an unqualified narrow-context row beats a cohort-qualified
  broad-context row.
- Band derivation from DOB at the SWING date, not today — an athlete with swings either side
  of a boundary resolves different rows for them.
- Unknown DOB, unknown sex, declined sex: falls back to unqualified and still GRADES.
- A junior athlete never matches an `adult` row (probes 2 and 4 skipped).
- Validation: unknown token, junior + adult sub-band, duplicate key — each a named load error;
  the sex-only/band-only-without-combined case a health WARNING, and the pack still loads.
- Provenance: the answering cohort reaches `MetricCorridor` and `MeasureReading` intact.

---

## Non-goals, restated

No ordinal norm shape (that is `Order` signals plus future gap measures). No per-athlete
norms — cohorts are segmented populations, never athlete-keyed. Still no norms at all for
cohort-relative speed/distance outcome measures; a cohort corridor does not fix those, they
are benchmarks and a different future feature. No free numeric age ranges and no
pack-extensible age vocabulary. No cohort interpolation. No mixture shapes, no log transforms,
no new grade bands, no change to `GradePolicy`, `ragOf`, or the wrist grid's `Band`.

The wrist grid stays entirely `target`: `NormBandProvider` **asserts** shape == target rather
than growing `Band`. Extend `Band` only if that assertion ever fires.
⚠ `BandContext` already has an unused legacy `int shape` field (`reference_bands.h:73`) — do
not confuse the two.

---

## Traps this plan is already guarding against

1. **A field can be complete on both sides and reach nothing.** When shape, openness,
   plausibility or cohort is added to a value type, grep the marshaller in the same change —
   `metric_catalog.cpp`, `norm_model.cpp`, `norm_editor_model.cpp`,
   `characteristic_library_model.cpp`. QML reads `undefined` and renders nothing; nothing
   warns.
2. **A rule can exist, be tested, and never run.** `validateNormsAgainst` had exactly one
   caller — its own test — for nine stages. Add the call site in the same change, or write
   down that you did not.
3. **A warning that fires on the design is worse than no warning.** Scope each new check to
   the state it actually means, and gate it in BOTH directions.
4. **`.toDouble()` on a missing QVariantMap key yields `0.0`** (fact 9). Openness must be an
   explicit boolean at every hop.
5. **Do not pin shipped numbers in a test.** Gate shapes and relationships. The smash
   behavioural-delta test is about band transitions, not about 1.48.
6. **Never derive "edited" by comparing values** — hence `NormBasis` gaining the plausibility
   pair rather than a value diff.
7. **Inside a Repeater delegate, only the component root id resolves** — and a handler on a
   composite type that declares its own `id: root` cannot see even that. It throws only on
   click, so no binding, test or screenshot will show it.

---

## Ledger

Deferred items, open questions and anything noticed in passing. Add, never remove; mark
resolved with the stage that closed it.

| # | Item | Raised | State |
|---|---|---|---|
| N1 | `m_leadHeelLiftTop` → ceiling? Author's call. | A5 | open |
| N2 | `m_leadHandWidth` (65 ±10) → floor? Author's call. `planned`, so inert either way. | A5 | open |
| N3 | `m_handSpeedP6P7` sign convention — deceleration vs release-drag — unresolved. | A5 | open |
| N4 | The two `@` norm-key spellings, and the whitespace they leave on the health view's deep-link. | B1 | open — fix in B1 |
| N5 | `contextFromMap()` never sets `band.contextId`, so every live UI norm lookup resolves at `full_swing`. Not caused by this work; it caps what cohort provenance can show. | B2 | open — belongs to *Diagnosis execution, V&V* |
| N6 | `SwingSummary`/`PersistedShot` drop the athlete block on read-back. | B3 | open — fix in B3 |
| N7 | The 16 male-only-sample rows (Meister 2011, Kim 2018) are the natural first cohort content, deferred to the ROM literature review by decision 6 of the brief. | B4 | open by design |
| N8 | `normZ()` has no production caller. It is made shape-aware for the future 0–100 score; nothing verifies it end to end today. | A2 | open by design |
| N9 | The engine (`detect()`, `NormMeasureSource`) is dormant, so A3 has no live verification. | A3 | open — belongs to *Diagnosis execution, V&V* |
