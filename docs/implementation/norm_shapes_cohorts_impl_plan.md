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
| 0 | Ideal-band policy divergence — **land before anything else** | ☑ complete — 79/79 green | 2026-07-28 · a82671a |
| — | **commit gate · Part A does not begin until Part 0 is in** | ☑ passed | |
| A1 | Shape enum, measure field, parse, validation | ☑ complete — 79/79 **untouched**, which is the gate | 2026-07-28 |
| A2 | Grading semantics, edges, plausibility | ☑ complete — 79/79, +~90 assertions | 2026-07-28 |
| A3 | Engine + health checks | ☑ complete — 79/79 | 2026-07-28 |
| — | **review gate · expect context clear here** | | |
| A4a | Rewire `oneSided` from the unit sniff to shape | ☑ complete — 79/79; **one rail changes appearance**, see N12 | 2026-07-28 |
| A4b | One-sided domain rules for the bars | ☑ complete — 79/79, +26 assertions | 2026-07-28 |
| A4c | Corridor editor — handles, readouts, diff rows | ☑ complete — 79/79, +89 assertions | 2026-07-28 |
| A4d | Corridor editor — plausibility fields and plot regions | ☑ complete — 79/79, +33 assertions | 2026-07-28 |
| A4e | Wording pass + signal direction picker | ☑ complete — 79/79, +43 assertions | 2026-07-28 |
| A5 | Seed conversion — smash factor becomes a floor | ☑ complete — **80**/80, +1 suite | 2026-07-28 |
| — | **merge gate · Part B does not begin until Part A is merged and green** | ☑ Part A complete, 80/80 | |
| B1 | Cohort schema, parse, validation, resolution | ☑ complete — 80/80, +53 assertion sites | 2026-07-28 |
| B2 | Cohort provenance threading + surfaces | ☑ complete — 80/80, +46 assertion sites | 2026-07-28 |
| B3 | Athlete DOB and sex, age at swing date | ☑ complete — 80/80, +24 assertion sites | 2026-07-28 |
| B4 | norms.json header — no cohort content, and why | ☐ not started | |

State vocabulary: ☐ not started · ◐ in progress · ☑ complete (gate green) · ⚠ blocked.

## Session log

Newest last. One line per session: what landed, what the gate said, what the next session
picks up. Keep it factual — this is the handoff, not a summary.

| Date | Stages touched | Outcome |
|---|---|---|
| 2026-07-28 | — | Plan written and verified against the tree. Brief copied to `docs/design/norm_shapes.md`. Baseline: analyzer suite 79/79 green at `ba7b360`. |
| 2026-07-28 | 0 | Stage 0 landed. `bandEdgesOf()` now scales the Ideal edge by `idealMaxZ`; `Norm::idealLo/idealHi` → `claimLo/claimHi`; all 22 call sites split into rendering vs authoring/diff; `MeasureReading::greenLo/Hi` policy-scaled and `fromCorridor()`'s inversion divisor follows; `gradePolicyIsOrdered()` added and asserted. **Three tests had written the defect down as a requirement** — `norm_editor_model_test` ("a policy change does not move the IDEAL band"), `norm_model_test` and `manifest_migration_test` — all three now assert the opposite, plus a per-preset edge sweep and a QML key-contract check. 79/79 green; app builds; headless launch clean. Next: A1. |
| 2026-07-28 | A1 | `Shape{Target,Floor,Ceiling}` on `Measure`, tables in `characteristic.cpp`, `"shape"` parsed and written (omitted when Target, so 105/106 measures round-trip byte-identically), `unknownShape` a named load error. Two referential rules in `validateNormsAgainst` — `normShapeTolerance` and `normShapeMonitor` — both gated in BOTH directions. **`norm.h` untouched**; the shape parameter and its branches arrive together in A2 rather than as a dead defaulted argument (see the stage note). 79/79 **unchanged**, which is the gate. Found and logged N10. Next: A2. |
| 2026-07-28 | A2 | Grading is shape-aware end to end: `normZ`/`withinBand`/`grade`/`bandEdgesOf` take a `Shape`, `hasExplicitMonitor(Shape)` and a new `outsideMonitor(value, Shape)` close N10, `plausibleLo/Hi` land on the row with `isImplausible()` outranking even the monitor band, `NormBandEdges` and `MetricCorridor` carry `lowOpen/highOpen` with **`mu` on the open side** (never a sentinel), and `NormBasis` gains the plausibility pair so a shipped row that acquires a cap does not compare as unmoved. `partialMonitor` MOVED to `validateNormsAgainst`; `plausibleOrder` and `plausibleInsideCorridor` are new, the latter measured against the **widest** preset so a pack's validity cannot depend on the reader's settings. `withinBand`'s degenerate early-return deleted as provably redundant under the computed-edge form. 79/79 with ~90 new assertions, incl. floor/ceiling tables, continuity at mu, per-preset edge sweeps, a one-sided monitor, plausibility precedence and corridor propagation over a hand-built floor. App builds; headless clean. Next: A3. |
| 2026-07-28 | A3 | `MeasureReading` carries `lowOpen`/`highOpen`/`implausible`; `NormMeasureSource` takes an optional `CharacteristicPack*` for the shape (null-safe, Target without it). A signal on the OPEN tail cannot fire — explicit in `evaluate()` for both `OutsideCorridor` and `Ratio`, rather than left as a coincidence of two other rules. **An implausible reading makes the finding `Unavailable`, not `NotFired`** — it was never assessed, and reporting it as "looked and fine" would be a false negative wearing a clean bill of health. New health code `signalOnOpenTail`, deliberately NOT scoped to Live (unlike `signalNoNorm`): no producer will ever give a floor an upper fault, so it is an authoring mistake, not a backlog row. Header table + `HealthView._codeLabel` updated in the same change. Both guards kept on purpose — runtime makes the answer right, health makes the mistake visible. Gated in both directions. 79/79; app builds; headless clean. Next: A4a. |
| 2026-07-28 | A4a | `_isOneSided()`'s unit sniff **deleted**. Shape flows `Measure` → `MetricCorridor` → `metric_catalog.cpp` (per-corridor `lowOpen`/`highOpen`/`shape`, plus a metric-level `shape`/`oneSided` requiring unanimity across phases, falling back to `target`) → `PpBandRail`. Openness now travels **per checkpoint** on `RailCorridor`/`RailPoint`, so `railRange`'s `oneSided` bool is **gone entirely** rather than becoming a shape — with `mu` on the open side there is nothing aspirational left to drop, and the ceiling mirror comes free (see the stage note). `PpBandRail`'s ribbon substitutes the plot edge per side, so a ceiling is representable for the first time. 79/79; app builds; headless clean. **One rail changes appearance — N12.** Next: A4b. |
| 2026-07-28 | A4b | `barDomain()` in `dashboard_reductions.h` + a `ChartMetrics::barDomain` façade; **both** bars now call it instead of computing a domain inside a binding (fact 16). **The defect was not the one the brief predicted** — A2 put `mu` on the open side, so the amber span is a healthy k×sigma and the domain never was degenerate. What actually broke: the domain STOPPED at `mu`, so every Ideal reading above a floor's aspiration clamped to the last pixel of the track and sat on a hard band edge reading as a bound. The open side now runs past the furthest of (aspiration, reading) by 35% of the graded span, and green runs off that end as a horizontal fade — **additive**, so two-sided bars are pixel-identical and the two-sided numbers are pinned exactly. Amber is deliberately untouched: on a floor it genuinely ends at `mu`, because above `mu` the grade is Ideal and the colour is green. Open-side tick labels re-position under the aspiration rather than pinning to the track end. `NormativeBar` gained the finite guards it never had (fact 13) and hides its bands on an invalid domain instead of collapsing them onto the left edge. `PpDashboardSetupZone` threads openness through; the `orientationLabel` audit became a one-term guard (N13). 79/79 with 26 new assertions, every one-sided case paired with a two-sided counterpart. App builds; headless clean; all six bar states rendered offscreen — see the stage note on what that does and does not prove. Next: A4c. |
| 2026-07-28 | A4c | Fact 14 closed: `setClaimBand` is **refused** on a one-sided draft rather than left to caller discipline, and `setAspiration` / `setTolerance` / `nudgeGradedEdge` land beside it — mu and the tolerance are INDEPENDENT numbers there, not two ends of a span. `nudgeClaimLo/Hi` became shape-aware routers so a caller holding "the low field" keeps working on all three shapes. The centre mark is draggable and is the headline; the handle `Repeater` is **one element**, and the dead handle is ABSENT. No swap-follow one-sided — `nudgeGradedEdge` clamps ON the centre rather than reflecting, so nothing can cross anything. `goodLo/goodHi`, the one pair computed outside `bandEdgesOf`, gained the shape collapse it was escaping: all THREE drawn bands now end at the aspiration. Wording moved into the model (`claimPhrase`, `policyNote`, `parentNote`, `shapeNote`, `openEndLabel`, import `rangeText`) per fact 16. Seat-from-swings is a median + 16th/84th percentile through a free-standing `fitOneSided()` — gateable without a swing library — with **no borrow fallback**. Monitor editing needed nothing: the editor never carries monitor bounds into a draft (`begin()` drops them on purpose). ⚠ **A defect found rather than planned — every readout was fixed at one decimal**, so smash factor's 1.48 ± 0.05 read "1.5" / "0.1" and its policy line named two different edges as one number; and a FIELD commits what it shows, so tabbing past an untouched corridor would have saved the rounding. Fixed with a faithful formatter on both sides (N15). 79/79 with 89 new assertions, every one-sided case paired with a two-sided control. App builds; headless clean; all three shapes rendered offscreen. Next: A4d. |
| 2026-07-28 | A4d | An **Implausible beyond** group with an optional bound per side, and two hatched plot regions covering the bands AND the bars — drawn after both and before the handles, because a reading out there is not graded at all. Deliberately NOT in the fault colour: Action is already drawn out here as bare track, and "the swing was poor" and "the reading was never believed" must not share one. Inline validation mirrors the pack validator's arithmetic exactly and reads the **widest** preset, never the active one — an editor validating against the reader's own sensitivity would let an author on `strict` save a row that fails to load on `lenient`. `save()` refuses on it too, because the call it already makes is `validateNormPack`, the standalone one, which cannot see the measure and so carries `plausibleOrder` but NOT `plausibleInsideCorridor`. **Two half-landed A2 items closed**: `begin()` now carries the bounds into the draft (a bound the editor SHOWS must survive a round trip, unlike the monitor bounds it deliberately drops), and `NormBasis`'s plausibility pair — on the struct since A2 but written by nothing and compared by nothing — is now stamped in `save()` and read in `overrideCoreChanged`, with a **second message** for the caps-only case so the notice never claims a corridor revision whose two quoted numbers would be identical. 79/79, +33 assertions; app builds; headless clean; the floor and the illegal target case both rendered offscreen. Next: A4e. |
| 2026-07-28 | A4e | The phrasing moved to **norm.h**, beside the vocabulary it formats — `normNumber` / `rangePhrase` / `actionPhrase` / `implausibleLabel` / `implausibleNote` — because six surfaces render a corridor as a sentence and six copies of `"%1 to %2"` is how a floor ends up reading "1.48 to 1.53" on five of them. `rangePhrase` takes **mu as well as the pair** and uses only mu one-sided, which is what lets a collapsed BAND edge and an uncollapsed CLAIM edge both phrase correctly without either caller knowing which it holds. Rewired: MeasureDetail's norm row, its shipped-diff and its action line, MeasureCatalogue's row, `overrideCoreChanged`, and the editor's own `fmtNum`/`claimPhrase`, which are now forwards. ⚠ **`monitorExcludesIdeal` MOVED to `validateNormsAgainst`** — it was gated on `hasExplicitMonitor()`, which without a shape demands BOTH bounds, so on a one-sided row it refused nothing and checked nothing; the same blindness A2 found in `partialMonitor`, in the check beside it. Its message now names the two EDGES, not two bands. **A live surface for implausibility turned up**: `grade()` returns NotMeasured for a capped reading, so the editor's swing list said "Not measured" for a mis-tracked ball, and — worse — `gradeCounts` let it fall through the switch entirely, so the running safety line silently under-reported. Both fixed, with `total` now accounting for every marked swing. Direction picker: `directionOptions` gains `enabled`+`reason` and takes a measureId; `TailChip` gained the disabled state it never had, with the reason in a caption beside the chips per DagView's doctrine. 79/79, +43 assertions; app builds; headless clean; qmllint unchanged or better on all four files. Next: A5. |
| 2026-07-28 | A5 | **Part A's first and only content change.** `m_smashFactor` ships as a `floor`; the four rows keep their mu/sigma and gain per-context `plausibleHi` caps (1.56 / 1.56 / 1.45 / 1.32) with the physics-of-loft reasoning in `citation` and no commercial sources. The norms.json header records where shape lives and why, what plausibility is FOR, the five candidates deliberately not converted with their reasons, and that cohort-relative bigger-is-better measures get no norm at all. New `seed_conversion_test` (**the suite goes 79 → 80**) gates the behavioural delta on shipped content: 1.55 Good → **Ideal**, 1.62 Watch → **NotMeasured + implausible**, 1.30 Action either way — plus that shape ships on exactly ONE measure, that the caps fall with loft, and that the shipped set still validates. It pins no mu or sigma: those are heuristics meant to move. ⚠ **Three suites failed on the content change and each was right to** — `reference_bands_test`'s parity sweep over-claimed (a `Band` cannot express a plausibility cap; it now states its domain and asserts no wrist cell is excluded, which is the guard the non-goals wanted from `NormBandProvider`), and two tests used smash factor as their two-sided control, which stopped controlling the moment it became a floor. Next: **Part B is now unblocked** — B1. |
| 2026-07-28 | B1 | `Cohort{optional<Sex>, optional<AgeBand>}` on the norm row; the age vocabulary is a closed enum with the boundary reasoning in `norm.h`. Key, parse, write, layering and resolution all carry it: `find`/`contains`/`upsert`/`remove` take a cohort, `contextsFor` DEDUPES (one context, several cohort rows), and `isOverridden`/`shippedNorm` key on the triple so a qualified override cannot mark the unqualified row beside it as edited. Resolution is **context-major** — `cohortProbeOrder()` is a free function returning the six keys, probed inside the context walk — and an athlete we know nothing about yields **exactly one probe**, so resolution costs what it always did. norm.h's doctrine reworded to "never keyed by athlete id". ⚠ **The three planned validations resolved to two and neither landed where the plan put them** — see the stage table: unknown token is a PARSE error that **drops the row** (the fallback is UNQUALIFIED, which grades everyone), duplicate-on-the-triple fell out of the key, and junior+adult-sub-band is unrepresentable, so `shadowedCohort` replaces it. Schema is 2, written **content-driven**. Fact 21 / N4 closed: one `normKeyLabel`/`splitNormKey` pair, deep-link fixed. ⚠ **`labelOf` was reading UTF-8 labels as Latin-1** — invisible while every label was ASCII, mojibake the moment an en dash arrived. 80/80, +53 assertion sites incl. the probe order exhaustively, the six-row drop-one-at-a-time walk, context-major precedence both ways, and a regression sweep asserting every shipped resolution answers identically for any athlete. App builds; headless clean. Next: B2. |
| 2026-07-28 | B2 | `NormResolution::cohort()` is an ACCESSOR over the answering row, not a second copy of it; `MetricCorridor` and `MeasureReading` each gain the answering cohort, and `corridorForMetricAtPhase` / `NormMeasureSource` each gain a defaulted `athlete` — read, testable, and the opposite of the A1 dead-parameter case, because without them the two new fields could only ever answer "unqualified". Marshalled at `metric_catalog` (per corridor and metric-wide), `normAt` and the editor's `draft()`, as BOTH a machine map and a label: `cohortLabel` lives with the vocabulary for the reason `normSourceLabel` does. **The measure-detail list now carries per-cohort rows** — the plan's own ⚠ about `editCorridor` only means something if a row can carry a non-default cohort, and without it an authored cohort row would appear in no view while still grading people. Own rows only; cohort inheritance is deliberately not rendered. The editor holds the cohort as row identity end to end (seed, `m_hadOwnRow`, save, basis, reset) and **refuses** an unreadable one; `CorridorEditor` states which population it is editing in a whole accented sentence, because the two panels are otherwise identical. ⚠ **Fixed in passing: `normAt` diffed a cohort row against the UNQUALIFIED shipped row.** ⚠ **`norm_model_test` had no isolated XDG_DATA_HOME** — it was reading the developer's own user norm set; it has one now, and the new block writes through the real layering path rather than a fixture. 80/80, +46 assertion sites across five suites. App builds; headless clean; qmllint deltas are `[unqualified]` Theme cascade only. Next: B3. |
| 2026-07-28 | B3 | `ageBandFor(dob, on)` and `cohortFor(dob, sexToken, on)` land in **norm.h**, beside the vocabulary whose boundaries they implement — derived at the SWING date and never stored, and never producing the parent `adult` band, which `cohortProbeOrder` depends on. The athlete record gains optional `dob` + `sex`, written through the generic `updateAthlete` rather than as arguments twelve and thirteen of `saveAthlete`. `sex` stores **`declined`** as its own token: identical to unanswered for a norm, different to the person, and a blank field would read as never having been asked. Every way of not answering — empty, declined, a token from a newer build — leaves the axis unset and **still grades**, and the unqualified cohort is the one-probe case, so knowing nothing costs nothing. ⚠ **Ledger N6 / fact 19 closed**: `PersistedShot` now carries `athleteUuid`/`athleteName`, which the reader had dropped for its whole existence — the uuid plus `wallclockMs` are what a cohort is derived from, and without them offline re-analysis and the live path could grade one swing two ways with nothing reporting the disagreement. NOT added to `SwingSummary`: it grades nothing, so the field would ride in the sidecar with no reader. **Screen: the athlete form's Recommended section gains both fields, and a live line saying what they resolve to** — "Today this places you in: women 55–64" — which is what gives `cohortFor` a real caller and makes the band boundaries discoverable instead of buried in a header comment. 80/80, +24 assertion sites: both sides of all three band boundaries, two swings a day apart resolving DIFFERENT bands, a leap-day birthday pinned to the 28th, and every not-answered path. App builds; headless clean; qmllint delta is the Theme cascade only. Next: B4. |

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

**Baseline to hold: 80/80 in the analyzer suite** — 79 until A5 added `seed_conversion_test`. Each
stage adds to it; none may subtract.

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
- **The whole existing suite passes UNTOUCHED.** That is the gate: this stage adds vocabulary
  and changes no behaviour.

  **Deviation from the brief, deliberate.** The brief has A1 thread `Shape shape = Target`
  through the grading functions and A2 implement the branches. That leaves a defaulted
  parameter no caller passes and no branch reads — dead weight nothing can test, and a
  compiler warning to suppress. A1 therefore touches `norm.h` not at all; the parameter and
  its branches arrive together in A2. The regression gate is unchanged and is arguably
  stronger: 79/79 passing with `characteristic.h`, `characteristic_pack.cpp` and
  `norm_pack.cpp` all modified proves the vocabulary is inert.
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
- Where a metric's corridors at different phases disagree on shape, the rail falls back to
  `target`: drawing an open tail that one phase *does* grade would state a freedom that phase
  lacks, which is the more dangerous of the two errors.
- `SwingScorer` stays frozen (fact 12).

**Deviation from the brief, deliberate.** The brief has `railRange()`'s parameter become a
shape and gain a ceiling mirror. It has **no parameter at all** now. That parameter existed to
drop an *aspirational* upper bound — a two-sided norm's high edge, far above anything an
athlete produced, which crushed the trace into the bottom of the tile. Both halves of that
premise are gone: one-sidedness comes from the measure, and the open side's numeric edge is
`mu`, the aspiration itself — precisely the number the rail exists to show. Dropping it would
hide the target. So every finite bound participates in the domain, openness travels **per
checkpoint** on `RailCorridor`/`RailPoint`, and it changes only how the ribbon is painted. The
ceiling mirror falls out of that for free rather than being a second special case, and
`PpBandRail` can represent a ceiling for the first time — the old code substituted `_rHi` for
the high edges and never touched the low ones.

**Screen:** the post-shot dashboard Motion zone. Rails with no corridor (`clubheadSpeed`,
`ballSpeed`) are sparklines and unaffected. ⚠ **`handSpeed` changes** — it was the only metric
matching both the unit sniff and a corridor, and it now draws as the two-sided norm that
actually grades it. See ledger N12: the rail was hiding a bad norm, not compensating for a
good one.

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

**The predicted failure mode did not happen, and the real one is worse.** The brief expected
the amber span to be OPEN on a one-sided norm and the domain to come out degenerate or
absurd. It does not: A2 put `mu` on the open side, so `amberHi − amberLo` is a healthy
k×sigma and every existing line of domain arithmetic returns a perfectly finite answer. What
is actually wrong is quieter. The domain STOPS at `mu` — so on a floor, every reading above
the aspiration (all of which grade **Ideal**) clamps to the last pixel of the track, 1.55 and
5.0 land in the same place, and both sit against a hard band edge that reads *"the corridor
ends here, you are outside it"*. The exact inverse of the grade. A degenerate domain would at
least have looked broken.

**Two decisions worth not relitigating:**

- **The open tail is drawn as an ADDITIVE element**, not by widening the existing band. A
  two-sided bar therefore renders the same two rectangles it always did, which is what makes
  "105 of 106 measures are pixel-identical" a structural claim rather than a hope.
- **Amber is not extended and does not fade.** On a floor the amber band genuinely ENDS at
  `mu`, because above `mu` the grade is Ideal and the colour there is green. Only green runs
  off the open end. The small step where the solid green (over amber) meets the fade (over
  bare track) falls exactly on `mu`, and is the only thing marking the aspiration on the
  track — kept, not smoothed away.

**On verification.** The domain rule is C++ and gated by `dashboard_reductions_test`. The
PAINTING was checked by rendering both components offscreen against a mock `ChartMetrics`
(scratchpad only, no committed file touched) in all six states — two-sided control, floor
below the aspiration, floor above it, ceiling, and both compact tiles. That proves the
geometry and the fade direction; it does not exercise the real façade, which `qmllint`
resolves statically instead. Note the consequence for later stages: both bars now depend on a
C++-registered type, so the pure-QML standalone harness needs that mock to render them at all.

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

**How fact 14 was actually closed.** `setClaimBand` is **refused** on a one-sided draft, not
merely bypassed. Left permissive it would take the midpoint of two edges — moving `mu`, the
headline, as a side effect of setting a tolerance — and split the width, leaving
`sigmaLo != sigmaHi`, which `validateNormsAgainst` refuses on a one-sided row. A legal-looking
gesture would author an invalid pack. `setAspiration` / `setTolerance` are the one-sided
operations, `nudgeGradedEdge` is the same thing in the drag's coordinates, and
`nudgeClaimLo/Hi` route by shape so a caller holding "the low field" keeps working on all three.

**Two decisions worth not relitigating:**

- **No swap-follow one-sided.** `nudgeGradedEdge` CLAMPS the tolerance at zero rather than
  taking a magnitude, so an edge dragged through the centre parks on it instead of reflecting
  out the far side and throwing the handle off the pointer. Nothing can cross anything, so
  there is no swap to follow — which is how the two-sided `pick()`/swap logic "simplifies".
- **`goodLo`/`goodHi` needed the collapse spelled out.** They are the one pair computed in
  `draft()` rather than by `bandEdgesOf`, so without it a floor drew its Good band running two
  sigma ABOVE the aspiration, straight over the Ideal band that owns that whole side. Stage 0's
  call-site audit flagged this pair as "easily missed"; it was.

**Monitor editing needed nothing.** The plan asked for "monitor editing: only the graded side
exists". `begin()` deliberately never carries monitor bounds into a draft (the editor does not
expose them, and silently preserving an invisible bound would make the Action edge disagree
with the drawn one), so there is no monitor UI to make one-sided.

**A defect found rather than planned — see ledger N15.** Every readout on this screen was fixed
at one decimal. That is right for the degree measures and wrong for the ratios: smash factor is
authored `mu 1.48, sigmaLo 0.05`, so its policy line read *"Ideal from 1.4 · good from 1.4"* —
two different edges as one number — and the new TOLERANCE field would have shown `0.1`, twice
its real width. In a label that is cosmetic; in a FIELD it is not, because `PpTextField` commits
on focus loss, so tabbing past an untouched corridor would have saved the rounding. Fixed with a
faithful formatter on both sides, which leaves every one-decimal measure reading exactly as
before.

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

**Hatched, and deliberately NOT in the fault colour.** Action is *already* drawn out at the
extremes — as the bare track beyond the amber band — so a red region would merge two answers
that must stay apart: Action says the swing was poor, this says the reading was never believed.
The regions cover the bands AND the histogram bars, and sit under the handles: a reading out
there is not graded at all, and these are not a control.

**The inline check reads the WIDEST preset, not the active one.** `validateNormsAgainst` has no
policy in hand and uses `lenient`, so an editor validating against the reader's own sensitivity
would let an author on `strict` save a row that fails to load for everyone on `lenient` — the
pack's validity would depend on who opened it. One function (`plausibleLoProblem` /
`plausibleHiProblem`) serves both the inline message and the save refusal.

**`save()` had to refuse it explicitly.** The validation it already ran is `validateNormPack` —
the standalone one, which cannot see the measure, so it carries `plausibleOrder` but **not**
`plausibleInsideCorridor`. Without the refusal the editor would write a row the referential
validator flags at the next load: trap 2, a rule that exists, is tested, and never runs where
the mistake is made.

**Two half-landed A2 items closed here, because this is the stage that makes them reachable:**

- `begin()` now carries the plausibility bounds INTO the draft. The monitor bounds are dropped
  on purpose (nothing renders them), but a bound the editor *shows* must survive a round trip
  through it — otherwise opening a capped row and saving it silently drops the cap, turning
  readings the norm had stopped believing back into confident diagnoses.
- **`NormBasis`'s plausibility pair was written by nothing and compared by nothing.** A2 put it
  on the struct and taught `norm_pack` to persist it; neither `save()`'s basis stamp nor
  `overrideCoreChanged`'s `moved` test ever read it — trap 1 exactly. Both closed, and
  `overrideCoreChanged` gained a **second message** for the caps-only case: the corridor numbers
  are identical either side of a cap change, and *"was 10 to 12, has since been revised to 10 to
  12"* is a notice nobody can act on.

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

**The phrasing lives in `norm.h`, not in any one façade** — `normNumber`, `rangePhrase`,
`actionPhrase`, `implausibleLabel`, `implausibleNote` — for the reason `normSourceLabel` does.
`rangePhrase` takes **`mu` as well as the pair** and uses only `mu` one-sided: a BAND's high edge
on a floor is already collapsed onto `mu` by `bandEdgesOf`, but a CLAIM's is `mu` plus a tolerance
nothing grades. Passing `mu` explicitly makes both callers right without either having to know
which it is holding.

**`monitorExcludesIdeal` MOVED to `validateNormsAgainst`.** It was gated on
`hasExplicitMonitor()`, which without a shape demands BOTH bounds — so on a one-sided row, where
one bound is the whole legal monitor band, the check silently did not run at all. The same
blindness A2 found in `partialMonitor`, sitting in the check beside it. Its message now names the
two EDGES rather than two bands: *"a monitor band of at least 1.46 does not contain a tolerance of
at least 1.48"* reads as false on its face.

**A LIVE surface for implausibility turned up, which the brief did not anticipate.** `grade()`
returns `NotMeasured` for a reading outside the caps, and the corridor editor grades real swings
today — so its swing list said "Not measured" for a mis-tracked ball, indistinguishable from a
swing the corridor never reached. Worse in `gradeCounts`: `NotMeasured` falls through the switch
without incrementing anything, so a capped corridor would quietly drop swings out of *"31 Ideal ·
8 Watch · 3 Action"* with nothing saying so. The running line IS the safety mechanism; one that
silently under-reports is worse than none. Both fixed, and `total` now accounts for every marked
swing.

**The greyed tail is greyed, not hidden.** Hiding it would leave one chip where there had been
two with nothing saying a choice existed — and the mistake it prevents is one an author makes
precisely because they believe the tail is there. The reason sits in a caption beside the chips,
never inside one: a reason has to fit as a sentence, and a chip is a word.

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

**The gate is its own suite** (`seed_conversion_test`, the 80th), because this is content and a
content regression — somebody editing `norms.json` — should fail a test whose *name* says what
broke. It pins the SHAPE of the answer (which band a value lands in, which side is open, that
the caps fall with loft) and one physical cap; it deliberately pins no `mu` or `sigma`, because
those are heuristics and are meant to move when a corpus re-seats them (trap 5).

**Three suites failed on the content change, and each was right to.** Worth recording, because
all three were tests that had quietly encoded "nothing shipped is one-sided yet":

- **`reference_bands_test`'s parity sweep over-claimed.** It swept all 149 rows asserting
  `ragOf(grade(v)) == classifyDelta(v)`, which held only while every row was expressible as a
  `Band` — and a `Band` has four numbers and no way to say *not believed*, so a capped row makes
  `grade()` answer Grey where `classifyDelta()` answers Amber, and neither is wrong. It now
  skips rows a `Band` cannot express, **counts** the skips, and asserts none of them is a cell
  the wrist grid renders. That last assertion is the guard the non-goals asked
  `NormBandProvider` for — a wrist DOF gaining a shape or a cap now fails a test that already
  runs, rather than needing a runtime assert nobody would see.
- **Two tests used `m_smashFactor` as their two-sided control** (`characteristic_editor_test`'s
  direction picker, `norm_editor_model_test`'s target block). A control that becomes the thing
  it controls for stops controlling; both moved to `m_ballPosition`, and the shipped floor
  became a *stronger* assertion beside the injected one — real content rather than a scratch
  pack.

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

**Audited at A5, item by item, rather than assumed** — the list spans A2–A5 and "we did that
somewhere" is how a gate goes unmet:

| Gate item | Where it actually runs |
|---|---|
| `norm_test` grading tables, continuity, z clamp, edge sweep, monitor, plausible, zero-sigma | `norm_test` (204 assertions) |
| `sigmaHi` differs on a floor · `monitorHi` on a floor · plausible inside the watch edge · unknown shape token | `norm_pack_test` (+ the ceiling mirror of each), `characteristic_pack_test` for the token |
| Engine: a High signal on a floor never fires and is a health finding; a Low signal fires as a target's low tail would | `characteristic_engine_test`, both directions; `diagnostics_health_test` for `signalOnOpenTail` |
| Openness through `corridorForMetricAtPhase`; `marginOverride` widens one side only | `manifest_migration_test`, `norm_test` |
| The smash behavioural delta | `seed_conversion_test` |
| Model-level for the QML-bound values | `dashboard_reductions_test` (bar domains), `norm_editor_model_test` (readouts, plausibility fields), `norm_model_test` (the row phrases) |

**Not covered by any test, and stated rather than left implied:** the PAINTING. Every surface
was rendered offscreen against a mock during A4b–A4d and looked right, but a mock is a hand
transcription (ledger N16) and the characteristic editor's chips were never rendered at all.
The QML has no test harness in this repo; that is fact 16 and it is unchanged by this work.

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

**The three validations resolved to two, and neither landed where the plan put them.** Worth
recording in full, because "we did that somewhere" is how a gate goes unmet:

| Planned | Where it went | Why |
|---|---|---|
| unknown cohort token | **PARSE** (`loadNormPack`), not `validateNormsAgainst` | an unknown token cannot survive into a `Cohort` — the enum has no seat for it — so there is nothing referential left to check by the time the referential validator runs. Same home as `unknownShape` |
| duplicate on (measure, context, cohort) | **`validateNormPack`**, via the key | `duplicateNorm` already keys on `normKeyLabel`, so giving the label a cohort term made the check triple-keyed with no new code. It needs no measure and belongs where it was |
| a `junior` row combined with an adult sub-band | **nowhere — it cannot be expressed** | `Cohort::age` is ONE optional token, so "junior AND adult_18_54" has no spelling in the schema. A check that can never fire is worse than none (trap 3), so it was not written |

**`shadowedCohort` replaces it**, in the same family — *a row that can never resolve*, which is what
`normShapeMonitor` already refuses on the open tail of a one-sided norm. An `adult` corridor at a
node where all three of its sub-bands are authored is unreachable for everybody, because the exact
band is always probed first. Health WARNING rather than a load error, beside its sibling `cohortGap`.

**The unknown token DROPS THE ROW, where `unknownShape` keeps the measure.** The asymmetry is about
which way the default is wrong. Falling back on shape means `Target`, a corridor that grades both
tails — conservative. Falling back on cohort would mean UNQUALIFIED, a corridor that grades
*everybody*, so a mistyped `"female"` would quietly apply one segment's numbers to the whole
population. `ResourceNormProvider` keeps the pack it parsed whatever validation said, so "error and
keep" would have shipped that outcome rather than merely reporting it.

**Schema versioning is content-driven, not a flat bump.** `kNormPackSchemaVersion` is 2 — what this
build UNDERSTANDS — but `saveNormPack` writes `requiredNormSchemaVersion(pack)`, which is 2 only when
some row carries a cohort. A flat bump would make every user's existing set unreadable by an older
build over a feature none of them used; stamping the loaded version back would let a set that GAINED
a cohort row keep declaring 1, which is the silent drop the bump exists to prevent. Writing what the
content needs is the only one of the three true in both directions — the same argument as omitting
`"shape"` when it is `Target`.

**Fact 21 / N4 closed.** `normKeyLabel(measureId, contextId, cohort)` and `splitNormKey()` are now
one pair in `norm_pack.h`, used by the validator, the merged provider, `diagnostics_health` and
`CharacteristicLibraryModel`. The health view's deep-link had been splitting on the first `@` against
a key two files spelled two ways, so half the links carried a leading space into the context id. The
cohort term is fenced behind ` · `, which no measure or context id can contain.

⚠ **A defect found rather than planned.** The `Row<E>` table helper reads its label column with
`QString::fromLatin1`, which was invisible while every label was ASCII. The age bands are not —
`"18–54"` carries an en dash — so read as Latin-1 each one became three mojibake characters, on the
key string, the health notices and every future cohort surface, with nothing failing anywhere to say
so. `labelOf` now reads UTF-8 and `nameOf`/`fromName` deliberately do not (a token is ASCII by
contract). Pinned by an assertion on the decoded label, not on the byte string.

**The corridor editor is unqualified end to end, on purpose.** `begin()` resolves with NO athlete
cohort and `m_draft` is left at the default, so `upsert` / `remove` / `shippedNorm` all key on the
unqualified row. Passing a cohort into that resolve would let a segment's row SEED a draft that then
saves at the unqualified key — one population's numbers promoted to the whole population by a
gesture that looks like an ordinary edit. B2 gives the editor a cohort identity; until then the
guard is a comment on the line that would break it.

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

**`NormResolution` gained an ACCESSOR, not a field.** The answering row already states its own
cohort, so a copy on the resolution would be a second source of truth for one fact — and a value
type carrying one answer twice is a value type where the two can disagree. `contextId` is a field
because it genuinely is not on the norm: a row does not know it was reached from a descendant. There
is deliberately no `cohortBroadened` flag either: with an unqualified pack it would be true on every
resolution and say nothing, which is trap 3.

**Two parameters were added that no production caller passes yet, and that is the opposite of the
A1 deviation rather than a repeat of it.** `corridorForMetricAtPhase(…, athlete)` and
`NormMeasureSource(…, athlete)` are read — they flow straight into `resolve()`, which has real
branches — and a test passes them directly. Without them the two NEW FIELDS would be the untestable
dead weight instead: every answer would be unqualified, and the stage's own gate item ("the
answering cohort reaches `MetricCorridor` and `MeasureReading` intact") could only be verified
against a value that cannot vary. B3 supplies the values.

**The measure-detail list gained per-cohort rows, and it had to.** The plan's own ⚠ about
`editCorridor` carrying the cohort only means something if some row can carry a non-default one —
otherwise the signal change is ceremony. Without it a cohort row would be authored into a file and
appear in no view: a corridor nobody can open, edit or delete, still grading people. The list is now
the universal row per context (exactly as before) plus one row per SEGMENTED row authored at that
exact context.

**Own rows only — cohort inheritance is not rendered.** Showing which cohort would answer at each
context for each of six probe keys would multiply thirteen contexts into a list nobody could read.
What the list has to guarantee is REACHABILITY of what was authored, not a rendering of the
resolution algorithm. `ownNormCount`/`editedNormCount` moved from counting contexts to counting
ROWS in the same change — the same number until a context could hold two.

**The corridor editor says which population it is editing, in a whole sentence.** A segmented
corridor and the universal one are edited on an identical panel, so without that line every control
below is editing the wrong row while looking right. Accented, not muted: it is not chrome. And
`begin()` REFUSES an unreadable cohort rather than dropping to unqualified — a segment's editor
silently becoming the editor for everyone is the one outcome worth failing over.

**`m_hadOwnRow` needed the cohort too.** A broader cohort's row at the same context is being
inherited from just as surely as an ancestor context's is, and calling it "own" would offer a reset
that drops a row the editor never opened.

**Fixed in passing: `normAt` compared against the wrong shipped row.** It keyed `shippedNorm` on the
context the row was found at but not on the cohort, so a cohort-qualified override would have been
diffed against the unqualified shipped row — "ships 8 to 15" about a corridor core never authored
for that segment, two populations quoted as one revision.

**`norm_model_test` gained an isolated `XDG_DATA_HOME`, which it should always have had.** Without
one the suite reads the DEVELOPER's own user norm set, so a corridor somebody overrode by hand could
fail it for them and nobody else. The cohort block also writes a user set, and it must land in a
scratch directory rather than in a real profile.

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

**The derivation lives in `norm.h`, not in the athlete controller.** `ageBandFor(dob, on)` and
`cohortFor(dob, sexToken, on)` are the vocabulary's own semantics — the boundaries they implement
are documented on `AgeBand` three lines above them — and putting them in the controller would make
the one rule that decides which corridor grades somebody a property of a QSettings-backed QML
façade. The controller's `cohortFor(uuid, date)` is a six-line lookup that forwards.

**`declined` is stored as its own token.** It means the same thing to a norm as an unanswered
question — only rows unqualified on that axis can match — but not to the person: an answer they gave
has to survive re-opening the form, and a blank field reads as never having been asked.

**Every unreadable sex token leaves the axis unset, which is the OPPOSITE of the norm parser's
rule.** An unreadable token on a norm row would silently widen a corridor's scope to the whole
population, so B1 drops the row. Here it means only "we do not know", which is a state resolution
already handles correctly and grades through. Same word, different failure, different answer.

**A LIVE LINE IN THE FORM, and it is the reason `cohortFor` has a caller at all.** Two fields whose
only consequence is invisible would be two fields nobody fills in — and the band boundaries would be
discoverable only by reading a header comment. The form now says *"Today this places you in: women
55–64"* under the picker. It says **today** out loud, because the band is derived at the swing date:
this is a preview of now, and a swing from four years ago resolves the band they were in then.

**⚠ `SwingSummary` deliberately did NOT get the athlete block**, though fact 19 names it alongside
`PersistedShot`. It is the session-picker row and it grades nothing, so a field on it would ride in
the sidecar with no reader — and every sidecar written before today would carry a blank one until
its `swing.json` happened to change. `PersistedShot` is the reload path that feeds anything which
could grade, and it is the one the fix belongs to.

**Leap-day birthdays fall on 28 February in a non-leap year** — `QDate::addYears`' clamping, and one
of the two readings jurisdictions actually use (the other is 1 March). Pinned by a test so it is a
decision rather than an accident. The disagreement is one day, once every four years, at a band
boundary that is itself a round number.

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
8. **A hoverEnabled MouseArea inside a hover-revealed item is an infinite loop.** Row affordances are
   `visible: rowMa.containsMouse`; a hoverEnabled child on top of `rowMa` STEALS the hover, which
   hides the item, which kills the hover, which shows it again — the row strobes every frame and the
   click can never land. Set `hoverEnabled: false` on the child: an area that does not accept hover
   is skipped for hover delivery, clicks still reach the topmost item, and `cursorShape` needs no
   hover. Same shape as trap 7 — invisible to bindings, tests and screenshots, visible only under a
   live pointer.
9. **A missing `Theme` token fails SILENTLY and renders white.** `Theme.colorBg1` does not exist;
   assigning it warned once at startup and left the `Rectangle` on its own default colour, so a
   modal ignored the dark theme entirely. Two symptoms, one cause. The warning names the file and
   line, so a single headless launch after any QML change catches it — which is why that check is
   not the one to skip when trimming a gate.

---

## Ledger

Deferred items, open questions and anything noticed in passing. Add, never remove; mark
resolved with the stage that closed it.

| # | Item | Raised | State |
|---|---|---|---|
| N1 | `m_leadHeelLiftTop` → ceiling? Author's call. | A5 | open |
| N2 | `m_leadHandWidth` (65 ±10) → floor? Author's call. `planned`, so inert either way. | A5 | open |
| N3 | `m_handSpeedP6P7` sign convention — deceleration vs release-drag — unresolved. | A5 | open |
| N4 | The two `@` norm-key spellings, and the whitespace they leave on the health view's deep-link. | B1 | ☑ **closed in B1** — one `normKeyLabel(measureId, contextId, cohort)` / `splitNormKey()` pair in `norm_pack.h`, used by all four sites. The cohort term is fenced behind ` · `, which no id can contain, so the split is an exact inverse rather than a heuristic |
| N5 | `contextFromMap()` never sets `band.contextId`, so every live UI norm lookup resolves at `full_swing`. Not caused by this work; it caps what cohort provenance can show. | B2 | open — belongs to *Diagnosis execution, V&V* |
| N6 | `SwingSummary`/`PersistedShot` drop the athlete block on read-back. | B3 | ☑ **closed in B3 for `PersistedShot`** — `athleteUuid`/`athleteName` are read and gated in `swing_doc_test`. `SwingSummary` deliberately NOT changed: it grades nothing, so the field would ride in the sidecar with no reader and every existing sidecar would carry a blank one |
| N29 | **`AthleteController::cohortFor(uuid, date)` has no caller.** The last hop — shot → athlete uuid + wallclock → cohort → `MetricCatalog` → `corridorForMetricAtPhase(…, athlete)` — needs `addPersistedShot` to carry the uuid through to a QML role and into `shotCtx`, which is the same shot-context plumbing N5 already blocks. Half of it would leave a second half-wired path. `cohortLabelFor` (the form's live preview) IS called, so the derivation itself is exercised end to end. | B3 | open — with N5, *Diagnosis execution, V&V* |
| N30 | **A swing does not record the athlete's demographics at capture time.** The cohort is derived from the CURRENT record plus the swing's date, which is right for a date of birth (it does not change) and questionable for offline tools: `swinglab_run` re-analyses with no QSettings, so it can resolve no cohort at all. Deliberately not added to `swing.json` — it is per-swing personal data with one consumer that does not exist yet. Revisit if offline grading needs a cohort. | B3 | open by design |
| N7 | The 16 male-only-sample rows (Meister 2011, Kim 2018) are the natural first cohort content, deferred to the ROM literature review by decision 6 of the brief. | B4 | open by design |
| N8 | `normZ()` has no production caller. It is made shape-aware for the future 0–100 score; nothing verifies it end to end today. | A2 | open by design |
| N9 | The engine (`detect()`, `NormMeasureSource`) is dormant, so A3 has no live verification. | A3 | open — belongs to *Diagnosis execution, V&V* |
| N10 | **`partialMonitor` will fire on a legal one-sided monitor.** It lives in `validateNormPack` (standalone), which cannot see the measure and so cannot know a floor carrying only `monitorLo` is complete. `hasExplicitMonitor()` has the same blindness (fact 3). | A1 | ☑ **closed in A2** — `hasExplicitMonitor(Shape)` is shape-aware, and `partialMonitor` moved to `validateNormsAgainst`, where the measure is in hand. Both directions gated |
| N12 | **`handSpeed` is the one rail whose appearance changes**, and it exposes a content defect. It is the only catalogue metric that both matched the old unit sniff (`mph`) and carries a corridor: `m_handSpeedP6P7 @ any` is `mu 20, sigmaLo 40`, whose own citation says *"sigma is twice mu, which is not a corridor"*. It was drawn as a floor and now draws as the two-sided norm that actually grades it, so its domain widens to roughly −100…140 and the trace squashes. That is the heuristic being deleted doing its job — the rail was hiding a bad norm, not compensating for a good one. **Fix by re-seating the norm, never by restoring a unit sniff.** `m_clubheadSpeedImpact` and `m_ballSpeed` carry no norm at all, so their rails are sparklines and are unaffected. | A4a | open — content, belongs with the corpus re-seat |
| N11 | `bandEdgesOf`'s signature is now `(norm, policy, marginOverride, shape)` — shape is the FOURTH argument, so a caller wanting shape must also pass `marginOverride = -1.0`. Only `reference_bands.cpp` passes a margin. Tolerable; revisit if a third caller wants shape without a margin. | A2 | open — cosmetic |
| N13 | The Setup zone's orientation glyph now falls back to a bar on a one-sided corridor. `orientationLabel()` itself is unchanged and still cannot see openness — the guard is at the call site, where the corridor is in hand. Every alignment measure is `target`, so the branch is unreachable today; it exists so that changing one's shape degrades visibly instead of labelling a floor's best possible reading "open". If a one-sided alignment measure is ever authored, decide then whether the glyph grows a one-sided vocabulary or the guard becomes permanent. | A4b | open by design |
| N20 | **The four annotated shape candidates remain author's-call**, now recorded in the norms.json header rather than only here: `m_leadHeelLiftTop` (ceiling, domain [0,∞)), `m_leadHandWidth` (floor?, and `planned` so inert), `m_handSpeedP6P7` (sign convention unresolved), `m_lagAngleDown` and `m_xFactorStretch` (both genuinely two-sided, reasons recorded). `seed_conversion_test` asserts exactly ONE one-sided measure ships, so a fifth arriving without that conversation fails a test. | A5 | open — author's call |
| N21 | `NormBandProvider` still has no runtime shape assertion; the non-goals asked for one. The guard landed instead in `reference_bands_test`'s parity sweep, which asserts no wrist-grid cell is excluded from it — a wrist DOF gaining a shape or a cap fails a test that already runs, which is better than an assert nobody would see. Add the runtime one only if `Band` is ever extended. | A5 | closed by other means |
| N18 | `MetricDetail.qml`'s `_normProvenance()` and the DAG / characteristic detail text were NOT swept in A4e — they describe provenance and causation rather than a corridor's bounds, so none of them renders a `%1 to %2`. Re-check when a one-sided measure actually ships (A5): if either ever says "above the corridor", it belongs to this sweep and was missed. | A4e | open — verify at A5 |
| N19 | `implausibleLabel()` / `implausibleNote()` have exactly one caller between them (the corridor editor's swing list). The engine that carries the `implausible` flag is still dormant, so the finding surfaces cannot use them yet — see N9. They are written now so the surface that eventually shows an implausible reading does not invent its own words for it. | A4e | open by design |
| N17 | The corridor editor's numeric fields ASSIGN `text` in their `onEditingFinished` handler, which permanently breaks the declarative binding — so after one edit a field only updates through its own handler, and a `discardChanges()` would not refresh it. Pre-existing on the two claim fields; A4c and A4d matched the pattern rather than diverging from it mid-stage. | A4d | open — pre-existing |
| N15 | **The corridor editor's readouts were fixed at one decimal**, which understated every measure authored finer — smash factor's policy line rendered its Ideal and Good edges (1.43 / 1.38) as the same "1.4". Fixed in A4c on both sides with a faithful formatter. The wider question is open: `MeasureDetail`, `MeasureCatalogue` and `norm_pack`'s validation messages all format at one decimal too, and A4e is the natural place to sweep them. | A4c | open — sweep in A4e |
| N16 | The corridor editor now depends on `NormEditorModel` and the bars on `ChartMetrics`, so the pure-QML standalone render harness needs a mock of each to draw them at all. Both mocks are hand transcriptions and can drift from the C++ silently — they verify PAINTING only, never a rule. Do not let an assertion migrate into one. | A4c | open by design |
| N22 | **The planned "a `junior` row combined with an adult sub-band" validation cannot be written.** `Cohort::age` is one optional token, so the combination has no spelling in the schema. `shadowedCohort` was written instead, in the same family (*a row that can never resolve*). If the age axis ever becomes a list — it should not — this row comes back. | B1 | closed by other means |
| N23 | **`shadowedCohort` and `cohortGap` cannot fire on any content that exists.** Both need cohort rows, and every shipped row is unqualified; `shadowedCohort` additionally needs four rows at one node. Gated in both directions in `diagnostics_health_test` against fixtures, which is the only gate they can have until B4's content arrives. Noted so a later reader does not read them as exercised. | B1 | open by design |
| N24 | **The corridor editor is unqualified end to end.** `begin()` resolves with no athlete cohort and the draft is left at the default, so save/reset/basis all key on the unqualified row. That is correct for B1 and is exactly what B2 changes — and the hazard B2 must not walk into is seeding a draft from a cohort row and saving it at the unqualified key. The guard today is a comment on the resolve line. | B1 | ☑ **closed in B2** — `begin(measureId, contextId, cohort)` seeds by resolving FOR that cohort, so its own row wins at probe 1 when one exists; the hazard is closed by the draft carrying the same cohort it was seeded for |
| N26 | **A cohort-qualified override records no `NormBasis`.** `save()` stamps the base from `shippedNorm` at the EXACT key, and core carries no cohort rows — so a new segmented override has no base and `overrideCoreChanged` is permanently silent for it. Correct today (there is no shipped cohort row that could move) and it is "as today" in the most literal reading of the plan. Revisit when B4's content lands: the alternative is a basis that records WHICH key it was seeded from, which is a schema change. | B2 | open — revisit at B4 |
| N31 | ☑ **CLOSED.** Was: no way to CREATE a cohort-qualified corridor from the UI. Editing one works end to end — identity, seed, save, basis, reset, the list, the editor's own "which population" line — but the only entry point to the editor is a row in MeasureDetail's list, and that list shows the universal row per context plus cohort rows that ALREADY EXIST. So an author cannot write their first segmented row without hand-editing the user norms JSON, after which everything works. B2's bullet was "cohort is part of the row identity it EDITS" and that was met; the authoring affordance was never in any stage's scope and should have been recorded here when the gap became visible. Closed by a "· for a cohort…" affordance on each context's universal row, opening a sex/age picker over `NormModel::cohortVocabulary()` and calling the `editCorridor(measureId, contextId, cohort)` signal B2 already built. **Verified by hand in the running app.** | B2 | ☑ closed after B3 |
| N27 | **The editor's import list and `adoptFrom` are unqualified-only.** Both walk `overriddenContextsFor` and then `find(measureId, cid)`, so a context carrying ONLY a segmented row contributes no candidate (skipped safely, no crash). Adopting numbers across cohorts is a second UI decision — the rows would need a cohort label and `adoptFrom` a cohort argument — and nothing can exercise it until cohort content exists. | B2 | open — with the content |
| N28 | **The editor's parent note models context inheritance only.** Dropping a `{female}@driver` row falls back to `{}@driver` — a broader COHORT at the same context — before it falls back to the parent context, and the note does not say that. It now resolves the parent for the draft's own cohort, which is the right population; the fallback ORDER it describes is still context-only. | B2 | open — with the content |
| N25 | **The diagnostics developer guide's `Norm` snippet is stale.** It predates `shape`, `plausibleLo/Hi` and now `cohort`, and §4's resolution description says nothing about the cohort probe order. Not corrected per stage (A1–A5 did not either); sweep it once at the end of the package, with §12's suite table. | B1 | open — sweep at package close |
| N14 | `PpRangeBar`'s **non-compact** form has no instantiation anywhere — the Verdict zone's tempo tile is absent until tempo has a producer, and the Setup zone is compact. So its tick row, and the one-sided tick placement in it, are unexercised. `NormativeBar`'s tick row does exercise the same rule. Not a defect; noted so a later reader does not read the code as covered. | A4b | open — latent surface |
