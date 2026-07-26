# Diagnostics norms — implementation plan

Companion to the brief (`docs/design/diagnostics_norms.md`). The brief is the *what*; this is
the *how*, sequenced, with every repo fact it depends on verified rather than assumed.

**This is a live working document.** Update the Progress table and Session log as stages land.
Work spans multiple sessions with context clears between them — the tables below are what makes
a cold restart possible. Do not treat any stage as done until its gate has actually run green.

**Anything deferred goes in [the clean-up sweep](#the-clean-up-sweep--everything-owed-before-this-work-is-done)
at the end of this document, not in the resume block.** The resume block is rewritten every stage;
the ledger is not. A final sweep after stage 10 must find that table with nothing left open.

---

## Progress

| # | Stage | State | Landed |
|---|---|---|---|
| 1 | Types, tree, pack, providers | ☑ complete | 2026-07-25 · 287f3ea |
| 2 | Migrate `reference_bands.cpp` — parity gate | ☑ complete | 2026-07-25 · 287f3ea |
| 3 | Wire into the engine — **the pack lights up** | ☑ complete — 8 live signals can fire | 2026-07-25 |
| 4 | Direction audit (all 30 signals) + `highMeans` | ☑ complete — **30/30, nothing exempt** | 2026-07-25 |
| — | **review gate · expect context clear here** | | |
| 5 | `NormModel` + read-only norm UI | ☑ complete — the measures view ships | 2026-07-25 · 0dea02a |
| — | **review gate** | | |
| 6 | `CorridorEditor.qml` | ☑ complete — **the pack meets real swings** | 2026-07-26 |
| 7 | Editable bindings + direction control | ☑ complete — bindings **resolve and are honoured**, not just stored | 2026-07-26 |
| — | **review gate** | | |
| 8 | The navigable DAG | ☐ not started | |
| 9 | Deletions and rewiring | ☐ not started | |
| 10 | Health checks | ☐ not started | |
| — | **clean-up sweep — the ledger at the end of this doc, nothing left open** | ☐ not started | |

State vocabulary: ☐ not started · ◐ in progress · ☑ complete (gate green) · ⚠ blocked.

## Session log

Newest last. One line per session: what landed, what the gate said, what the next session
picks up. Keep it factual — this is the handoff, not a summary.

| Date | Stages touched | Outcome |
|---|---|---|
| 2026-07-25 | — | Plan written and verified against the tree. Brief copied to `docs/design/diagnostics_norms.md`. Nothing built yet. |
| 2026-07-25 | 1 | **Stage 1 complete.** `norm.h`, `context_tree.{h,cpp}`, `norm_pack.{h,cpp}`, `norm_provider.h` + the three providers. Assets moved to `src/Resources/diagnostics/` (core.json via `git mv`, plus new norms.json — empty — and contexts.json). `:/diagnostics` prefix preserved by qrc alias, so `pack_provider.h:73` is unchanged. New suites `norm_test` / `context_tree_test` / `norm_pack_test` green; **full analyzer suite 70/70**; app builds clean and starts headless. Next: stage 2. |
| 2026-07-25 | 2 | **Stage 2 complete.** 39 cell-measures minted into core.json (28 → 67); 55 norm rows (39 full_swing + 16 archetype) in norms.json. `NormBandProvider` added; `BandContext` gained `contextId`; all 4 app call sites switched to `BandProviderKind::Norm` (now the factory default). **`reference_bands_parity_test` green: 117 cells bit-identical, 77,337 classified deltas identical.** `wrist_render_parity_test` green: 2,688 cells and 67 findings identical, and its empty-norm-set guard verified to actually fail without the content. Analyzer suite **72/72**; app, `swinglab_run` and `swing_window_parity_test` all build. Next: stage 3. |
| 2026-07-25 | 3 | **Stage 3 mechanism complete; content OWED — the pack is still dark.** `NormMeasureSource` joins values to norms; `MeasureReading` gained `grade`, `normContextId`, `contextInferred` and a `fromCorridor()` factory. Engine now fires on a **deviation (Watch/Action)**, not on leaving Ideal — see the decision below. `norm_measure_source_test` green (26 assertions incl. unknown-context, inferred-context demotion, both tails on one norm). Analyzer suite **73/73**; app + `swinglab_run` build. **BUT: all 26 corridor-signal measures still have no norm** (norms.json holds only the 39 wrist-grid rows), so no seed characteristic can fire yet. Authoring those 26 is the remaining work and needs Mark's numbers. Next: author the seed norms, then stage 4. |
| 2026-07-25 | 3 (closed) + 4 | **THE PACK LIGHTS UP.** Direction audit of all 30 signals (3 inversions + 2 structural defects fixed); sign conventions settled and documented; `highMeans` on every signal-bearing measure; 13 seed norms authored. **8 live corridor signals can now fire, 0 left dark** — `core_pack_test` asserts it. `ballPosition` reverted to the interoperable 0 %-lead-heel scale on Mark's call. Analyzer suite 74/74. Next: stage 5. |
| 2026-07-25 | 5 | **Stage 5 complete — the measures view ships.** `NormModel` (`src/Gui/characteristics/norm_model.{h,cpp}`, QML_ELEMENT), `MeasureCatalogue.qml` as the fourth `_view`, `MeasureDetail.qml`, and the grade-policy + norm-set strip. Both carried-forward debts paid: **`ragOf(Grade)`** promoted into `norm.h` and gated by a new `reference_bands_parity_test` section — 28,590 samples over all 68 shipped norms, covering BOTH precedence branches (56 monitor-dominated, 12 z-derived); **`measureForMetricAtPhase()`** built as a pack-layer free function (`characteristic_pack.h`) with the `NormModel` marshaller over it, so stage 9 reaches it without a QML façade. New `norm_model_test` (60 assertions). Also landed: `normIsWeak()`/`normWeakReason()` in `norm.h`, `INormProvider::layers()` (so the UI stops saying "merged"), `AppSettings::diagnosticsGradePolicy`, and a `duplicateMeasure` validator warning pulled forward from stage 10, which immediately found a real one (ledger `C1`). Analyzer suite **75/75**; app + `swinglab_run` build; verified headlessly with screenshots. Next: review gate, then stage 6. |
| 2026-07-26 | 6 | **Stage 6 complete — the corridor editor ships, and the first thing it did was find bugs.** Stage 6 needed a piece the plan never scoped: `IMeasureValueSource` was implemented only by test fakes, so nothing could read a measure off a stored swing and the "not optional" histogram had nothing to draw. Built `src/Diagnostics/measure_sample.{h,cpp}` — a per-swing **phase grid** (windowed median per metric per segmented phase, using WristAngleSampler's own ±15 ms convention, plus adjacent-span extrema for the 9 `extremum` measures), cached in a `swing_phasegrid.json` sidecar guarded on swing.json size+mtime. Pack-independent by design, so minting a measure does not stale 72 sidecars. Then `NormEditorModel` (draft + background library scan + histogram + save), `CorridorEditor.qml` (three routes, two IDEAL-band handles at 44 pt, live histogram with running grade counts), the norm rows in `MeasureDetail` made tappable, and the norm-set strip turned into a real selector. **Ledger C2 and C4 both closed**; `AppSettings::diagnosticsNormSetsOff` added. New suites `measure_sample_test` (34 assertions) and `norm_editor_model_test` (56); analyzer suite **77/77**, and all seven suites green at **105/105** after fixing two PRE-EXISTING link failures (`profiler_controller_test` missing `AnalysisProfileLog.cpp`, `reanalysis_controller_test` missing `pp_os_metrics.cpp` — both "Not Run", neither related to this work). Verified in the running app against the real 72-swing library. **Four new ledger entries (C18–C21) came straight off that screen** — see below. Next: review gate, then stage 7. |
| 2026-07-26 | 6 (follow-up) | **Reset-to-default and "edited from shipped", on Mark's ask — and it uncovered a data-loss bug.** The norm stack could not say WHICH LAYER a value came from, which both asks need, so `NormResolution` gained `overridden` and `INormProvider` gained `shippedNorm()` / `isOverridden()` (tracked at merge time, never derived by comparing numbers — a user row holding the shipped values is still a user row). The corridor editor now has **two distinct undos**: *Discard changes* (unsaved dragging, writes nothing) and *Reset to shipped* / *Remove your override* — one operation, two labels, because what you get back depends on whether core carries a row at that key. Edited-from-shipped markers land on the `MeasureDetail` norm rows (quoting the shipped band), the `MeasureCatalogue` rows, a new **Edited by me** filter chip, the census line, and the grade-policy / norm-set strip (each with its own reset). **Then the characteristic editor:** `revertToShipped()` DELETED a user-created characteristic while reporting *"Restored the shipped definition"* — the backwards `overridesCore` flag conflated "core ships this id" with "a user row exists", now split into `shippedExists` / `hasUserOverride` with the destructive case labelled and styled as destructive. **And underneath it, a silent data-loss defect:** the editor loaded its user pack keying off `PackLoadResult::loaded` where the type's own comment says a merging caller must key off `parsed` — an overlay routinely fails standalone referential validation, so the editor started with an EMPTY user pack and `save()` then wrote it back, **erasing every other override the user had ever made**. Fixed, with a regression test verified to go red against the old code. All seven suites **105/105**; verified in the running app. Next: review gate, then stage 7. |
| 2026-07-26 | 6 (drag fix) | **Handle drag ran away to absurd values; the cause was a feedback loop, not a scale factor.** The axis is derived from `mu ± watchMaxZ·sigma`, so dragging a handle widened the corridor, which widened the axis, which made the SAME pixel map to a larger value, which widened it further — a gain of about 3 per mouse-move event at ~60 Hz. Measured: with the pointer PARKED, 20 events took the corridor from 37.4 to **10,860**. Fixed by latching the axis for the duration of a gesture (`beginHandleDrag()` / `endHandleDrag()`, rule in C++ and regression-tested — the test was verified red against the old code), and by dropping `drag.target` for an absolute plot-x mapping: `drag.target` assigns `x` imperatively, which permanently broke the `x: xOf(value)` binding, so after one drag the mark stopped tracking the model and typing in the numeric field moved the number but not the handle. Ledger C24. All seven suites **105/105**; verified in the running app (30 events at one pixel: first 124.968, final 124.968). |
| 2026-07-26 | 7 | **Stage 7 complete — and the bindings it made editable now MEAN something.** The plan scoped a checkbox per context over a field nothing read; shipping that would have repeated `C3` (a control that says more than it does). So the rule was built first: `resolveContextBinding()` (`context_tree.h`) walks the chain exactly as a norm does — nearest row wins, nothing on the chain means applies-and-ranks — and `detect()` gained an optional `(ContextTree*, contextId)` that **OMITS** an inapplicable condition rather than reporting it NotFired (assessed and absent) or Unavailable (tried and failed); both would be wrong in a way a coach could read. `Finding::material` carries the second half into `explain()`, where an immaterial finding is still listed and still counted in coverage but contributes **zero** to the ranking score — zero rather than a fraction, because a fraction would be a number nobody could defend. `CharacteristicEditorModel` gained `contexts` (tree order + depth + own/inherited/from-where), `setBinding`/`clearBinding`/`undoBindingChange`, and **the cascade**: switching a parent off clears contradicting rows beneath it, reports how many, and is undoable in one tap — without it the untick silently would not take, because resolution stops at the nearest row. `C6` closed (`corridorRef` gone from all four sites). **Direction control:** High/Low is now the measure's own words everywhere it is chosen, composed in C++ (`directionOptions()`), and the picker **asks for the sentence when the measure has none** and gates minting on it — the one defect class that cannot be caught downstream, since an inverted signal fires happily with correct-sounding text. Two live defects found and fixed while verifying: the library list rendered *underneath* the authoring sheet on the "New characteristic" path (`C26`), and tapping a near-duplicate committed a tail the author had not seen yet, read against a different measure's convention (`C27`). Analyzer suite **77/77**; all seven suites **105/105**; verified in the running app headlessly. Next: review gate, then stage 8. |
| 2026-07-25 | pre-4 | **Three decisions from Mark, all landed.** (a) Fire-on-deviation confirmed. (b) `stanceWidth` is now **% of shoulder width** — invariant unit, computed from the shoulder pair over the same address reference frames; the millimetre reading moved to its own `stanceWidthMm` metric so both units stay invariant. (c) The two spinal measures are **roadmap items, not capture gaps**: they cannot come from the pose skeleton, but a DTL back-contour producer would resolve them, so `roleNeedsNonPoseSensor` keeps its detection and loses its "never" conclusion. Seed pack now has **zero capture gaps**. Tempo band re-cut DEFERRED pending a literature review. Analyzer suite 73/73; app + swinglab_run build. |

---

## ▶ Resuming at stage 8 — read this first

**Prompt to start with:**

> Start stage 8 of the diagnostics norms plan — read
> `docs/implementation/diagnostics_norms_impl_plan.md`, section "Resuming at stage 8".

Everything through stage 7 is complete. Analyzer suite **77/77**; all seven CTest suites
**105/105**; app builds clean and was verified running. Do not re-verify — the Progress table is
authoritative. **Uncommitted at time of writing.**

Stage 8 is `dag_layout.{h,cpp}` + `DagView.qml` — see the stage section below. It is the only stage
with no dependency on anything stage 7 built, so nothing here blocks it.

### What stage 7 established, which stage 8 should not contradict

- **A binding is an EXCEPTION, not a declaration.** `resolveContextBinding()` (`context_tree.h`)
  walks the chain exactly as a norm does: nearest row wins, and a condition with no row anywhere on
  the chain applies everywhere and ranks normally. All 50 shipped conditions carry no binding rows,
  and the UI is built so that reads as deliberate rather than as unset.
- **`detect()` OMITS an inapplicable condition.** Not NotFired (which claims it was assessed and
  found absent), not Unavailable (which claims the app tried and could not). Any surface that lists
  findings must not backfill the gap with either.
- **Immaterial means unweighted, never softened.** `Finding::material` reaches `explain()`, where
  such a finding is still listed and still counted in coverage but adds **zero** to the ranking
  score. Do not render it as a lesser finding — context never changes whether something is good or
  bad, only whether the question is worth asking.
- **The cascade is load-bearing.** Switching a context off clears contradicting rows beneath it,
  because resolution stops at the nearest row and an explicit "applies" at `wedge` would otherwise
  survive a "does not apply" at `full_swing`. It reports the count and is undoable in one tap.
- **High/Low is gone from every place a direction is chosen.** `directionOptions()` composes both
  tails in the measure's own words, in C++. The low tail quotes the SAME authored sentence as *the
  other end of that range* — no generated opposite, because a generated opposite would read like
  content and be nobody's words.
- **The picker will not mint a measure with no `highMeans`.** That gate is the whole reason stage 4
  existed; three signals shipped inverted for want of one sentence.

### Owed from stage 7

In the **clean-up sweep** at the end of this document, as always. `C6` is closed. `C25` is new and
is the real one: bindings now resolve and the engine honours them, **but nothing in the app
constructs the engine yet** — same gate as `C3`, and when it is wired it must be handed the shot's
context or every binding in the pack stays inert. `C26`/`C27` were live defects found while
verifying and are fixed. `C28` records that 40 core measures still carry no `highMeans` (all wrist
cell-measures with no signals; the new gate is a floor for new content only).

## ▶ Stage 7 — read this first (superseded, kept for the record)

**Prompt to start with:**

> Start stage 7 of the diagnostics norms plan — read
> `docs/implementation/diagnostics_norms_impl_plan.md`, section "Resuming at stage 7".

Everything through stage 6 is complete. Analyzer suite **77/77**; all seven CTest suites **105/105**;
app, `swinglab_run` and `swing_window_parity_test` build clean. Do not re-verify — the Progress
table is authoritative. **Uncommitted at time of writing.**

### The one thing to read before anything else

Stage 6 was the first time the pack met real swings, and **the corridors and the producers do not
agree**. Four new ledger entries — `C18` to `C21` — came off that screen, and `C18` in particular
(stance width reading ~2x its own corridor, in a unit both sides spell identically) is a defect the
norm-pack validator cannot catch by construction. Read them before treating any shipped corridor as
a number rather than a hypothesis. None of them blocks stage 7.

### What stage 6 built, in one paragraph

`src/Diagnostics/measure_sample.{h,cpp}` reads a MEASURE off a stored swing: a per-swing **phase
grid** (windowed median per metric at each segmented phase + adjacent-span extrema), cached in a
`swing_phasegrid.json` sidecar. `NormEditorModel`
(`src/Gui/characteristics/norm_editor_model.{h,cpp}`) holds the draft, scans the athlete library on
a worker, and computes the histogram and grade counts in C++. `CorridorEditor.qml` is the view:
three routes, two handles on the **Ideal** band, a live histogram, provenance, and a save that says
it is setting a population norm. `MeasureDetail`'s norm rows open it; the norm-set strip is now a
selector.

### Facts the next session should not re-derive

- **The scan is pack-independent on purpose.** The sidecar caches the phase GRID, not measure
  values, so minting a measure costs nothing. Do not "optimise" it into a measureId->value cache —
  that trades a one-time cost for a full re-parse of the library on every pack edit.
- **A metric with an EMPTY curve is normal.** Every setup metric in a real swing.json (stanceWidth,
  ballPosition, tempoRatio, foot flare, toe line) ships with no `value[]` at all and nothing but
  `phaseSamples`. The builder falls back to those, and a first implementation that read only the
  curve produced nothing for all of them while looking exactly like "no swing carries this measure".
- **`extremum` with an anchor is the SIGNED deviation**, not `max |value - anchor|`.
  `metric_reducer.h`'s comment said the latter, which cannot carry a `sense`; it was corrected.
- **The handles bind the IDEAL band.** `norm_editor_model_test` pins the drawn Good and Watch edges
  at exactly 2x and 3x the half-widths so "the Good band" cannot creep back in.
- **`canRevert` means YOU have an override**, not "something resolves here". A shipped row offers no
  revert — the core set is read-only, and offering an action that can only fail is worse than not
  offering it.
- **Qt's offscreen platform caps the screen at 800x800**, and there is no Xvfb on this box. Grab
  headless screenshots with `QT_SCALE_FACTOR=0.5` to fit a full page in frame.

### Shipped vs yours — the seam stage 6 added late

`NormResolution::overridden`, `INormProvider::shippedNorm()` and `INormProvider::isOverridden()`
answer "is this still what we ship, and what did we ship?" — the two questions every reset and every
"edited" marker rests on. Three rules worth not re-deriving:

- **Tracked, never compared.** A user row holding exactly the shipped numbers is still the user's.
- **`isOverridden` is per KEY; `resolve().overridden` is per RESOLUTION.** They differ exactly where
  inheritance does, and both are needed: a driver with no row of its own is still graded by the
  user's full-swing corridor and must say so.
- **Which reset a button may offer depends on `shippedNorm()`.** Core carries a row → "Reset to
  shipped"; core carries none → "Remove your override". Same operation, different promise.

### Owed from stage 6

In the **clean-up sweep** at the end of this document, as always. `C2` and `C4` are closed, and the
follow-up closed `C22` and `C23`. `C18`-`C21` are new and are all content/producer questions, not
code.

## ▶ Stage 5 — read this first (superseded, kept for the record)

**Prompt to start with:**

> Start stage 5 of the diagnostics norms plan — read
> `docs/implementation/diagnostics_norms_impl_plan.md`, section "Resuming at stage 5".

Everything through stage 4 is committed and pushed (`d7cf345`). Analyzer suite **74/74**; app and
`swinglab_run` build clean. Do not re-verify any of it — the Progress table is authoritative.

### Read before writing any QML

1. `docs/design/pinpoint_qml_design_system.md` — **mandatory** per CLAUDE.md. Theme tokens only:
   never hardcode a colour, font size or spacing, and every component must work in all three
   aesthetic directions.
2. `docs/design/pinpoint_sign_conventions.md` — the two-clause rule; stage 5 renders `highMeans`,
   which is where it surfaces to the user.
3. `src/Gui/characteristics/CharacteristicLibrary.qml` — the panel being extended.

### Facts already established (do not re-derive)

- **The Diagnostics settings panel IS `CharacteristicLibrary.qml`.** `ScreenSettings.qml:447`
  instantiates it as panel 10. There is no `DiagnosticsPanel.qml` and no settings form.
- The view switcher is `property string _view` (~line 51), chip row ~line 130. Add `"measures"` as
  a fourth chip — **extend, do not restructure**.
- `CharacteristicLibraryModel::usageOfMeasure()` and `usersOfMeasure()` already exist. Reuse them.
- `MeasurePicker.qml` already does mint mode. **New measure** opens it; do not write a second picker.
- Norms are reached through `makeNormProvider()` / `sharedNormProvider()`
  (`src/Diagnostics/norm_provider.h`). `INormProvider::resolve()` already returns
  `{norm, contextId, inherited}` — the inheritance line the UI needs is there, not to be recomputed.
- Standalone tests reach shipped content via `PINPOINT_CORE_NORMS` / `PINPOINT_CORE_CONTEXTS` /
  `PINPOINT_CORE_PACK`; in CMake use the existing `pp_norm_env(<target>)` helper.

### Carried forward — owed from earlier stages, easy to lose  ✅ BOTH PAID IN STAGE 5

- **`ragOf(Grade)` shared helper.** The 4-band grade collapses to the legacy 3-band `PpRag` as
  *Green iff `Ideal`, Red iff `Action`, Amber otherwise*. That rule currently exists **only inside
  `reference_bands_parity_test`**. Promote it to a real function next to `grade()` in
  `src/Diagnostics/norm.h` and have the parity test call it, so the wrist grid and the dashboard
  cannot drift from each other. Note the consequence: for migrated rows `Good` and `Watch` both sit
  inside the old amber, so **any surface showing a grade and a RAG chip together will look
  inconsistent — show one or the other.**
- **`measureForMetricAtPhase(metricKey, phase)`** on `NormModel`. Stage 5 needs it; **stage 9
  depends on it** to re-point `metric_catalog.cpp`. Build it now, with tests.
- **Grade policy + norm-set selector** go in the Diagnostics settings header strip (Mark's call),
  shown in the `measures` view where they apply.

### The first three things to do

1. `src/Gui/characteristics/norm_model.{h,cpp}` — QML façade over measures, norms and contexts,
   mirroring `characteristic_library_model.h`'s shape (`Q_PROPERTY QVariantList` + `Q_INVOKABLE`
   returning `QVariantMap`). **All logic in C++**; QML renders shapes and holds no rules.
2. `MeasureCatalogue.qml` — the fourth view. Grouped by measure group; filter chips for status /
   has-norm / layer; row shows label · unit · status dot · *"used by N"* · a norm glyph, hollow when
   unset.
3. `MeasureDetail.qml` — header; what it is; the **`highMeans`** line; **norms by context** in tree
   order with children indented, each row showing the resolved corridor in the measure's own units
   and either *inherited from &lt;parent&gt;* or *overridden*; used-by; availability.

**Read-only in stage 5.** No editing — the corridor editor is stage 6. Weak provenance is called out
**on the norm row only**, never on a finding, chip or chart band elsewhere.

### Two things to be honest about

- **Nothing has been run against a real swing.** The pack can fire, but the seed corridors are
  unvalidated against actual golfers, so the first live shot may over- or under-fire. Worth a
  sanity-check before trusting any finding; the corridor editor's live histogram (stage 6) is the
  real fix.
- **A literature review of every normative value is planned** before any corridor is treated as more
  than a starting heuristic. `m_lagAngleDown` is the weakest and the first candidate.

---

## Context

The Diagnostics module shipped in Phases 1–8 (`..79bb347`) with a working pack stack,
copy-on-write editing, measure picker, roadmap and health view. **It is dark.** 30 of the
31 signals in `src/Diagnostics/packs/core.json` use `outsideCorridor`, and
`MeasureReading::hasCorridor` (`characteristic_engine.h:46`) is written by nobody outside
`characteristic_engine_test.cpp`. The engine correctly reports `Unavailable` for all of
them, so the entire seed pack detects nothing. Corridors are not one input among many —
they are the detection mechanism.

At the same time the numbers that *do* exist are trapped in code: 39 green corridors and
five amber margins compiled into `src/Analysis/reference_bands.cpp`, plus a ±10° archetype
shift in `pp_tuned_constants.h`, plus one inline tempo corridor in the metric manifest.
None of it is inspectable, overridable or re-seatable by a user, and none of it is
reachable from the Diagnostics pack.

This work introduces **population norms** as first-class pack content, migrates every
hardcoded corridor into them without changing a single rendered pixel, lights up the seed
pack, and then builds the authoring and navigation surfaces on top. It is additive: the
pack stack, provider layering, measure picker, roadmap and health view are extended, never
restructured.

---

## Settled decisions

| | |
|---|---|
| **Vocabulary** | `norm` throughout — `Norm`, `INormProvider`, `norms.json`, `NormModel`. Never "reference": `referencesFor(whatRole, quantity)` and `Series::reference` already mean the *"relative to"* facet role. |
| **Grade labels** | **Ideal · Good · Watch · Action**, plus **Not measured**. |
| **Policy fields** | Named for the band they cap: `idealMaxZ = 1.0`, `goodMaxZ = 2.0`, `watchMaxZ = 3.0`, beyond = Action. (The brief's `solidZ`/`monitorZ`/`outsideZ` each named the band *below* the one they bounded.) |
| **Assets** | `src/Resources/diagnostics/` — lowercase, matching the existing `src/Resources/icons/`. `core.json` moves there too. |
| **Norms** | Population-based only. Never keyed by athlete id. `IReferenceBandProvider`'s player-baseline seam is left unimplemented. |
| **Grade policy + norm-set selector** | Diagnostics settings. |
| **Metric-side corridors** | `MetricCatalogue::corridor()` deleted; `metric_catalog.cpp` re-pointed at `NormModel`. |
| **Execution** | All ten stages planned; review gates after 4, 5 and 7. |

---

## Repo facts this plan is built on (verified, not assumed)

1. **The Diagnostics settings panel *is* `CharacteristicLibrary.qml`.** `ScreenSettings.qml:447`
   instantiates it directly as panel 10; there is no `DiagnosticsPanel.qml` and no settings
   form to add a control to. The grade policy and norm-set selector therefore land as a
   compact header strip in the library panel, shown in the new `measures` view where they
   apply.

2. **`metric_catalog.cpp` is the single choke point, not `chart_metrics.cpp`.**
   `metric_catalog.cpp:183–202` builds `normative.corridors` from
   `MetricCatalogue::corridor()`; QML then feeds that list to `PpBandRail`,
   `PpDashboardMotionZone:109`, `PpDashboardVerdictZone:91` and `MetricDetail.qml:266`.
   `chart_metrics.cpp:276` only marshals what QML hands it. Re-pointing the one C++ site
   lights every consumer up at once.

3. **Keep `IReferenceBandProvider` and `Band`; do not delete them.** `wrist_assessment_engine.cpp:82–94`
   reads `band.greenLo/greenHi` into `PpRagCell::bandLo/bandHi`, which the wrist grid
   renders — so the *band shape*, not just the `PpRag`, is user-visible. Adding a
   `NormBandProvider : IReferenceBandProvider` that projects a `Norm` into a `Band` makes
   the migration **structurally** byte-identical rather than merely tested, and still
   achieves the brief's goal: `ArchetypeBandProvider`'s compiled ±10° special case becomes
   two ordinary norm rows. `classifyDelta()` stays untouched.

4. **The explicit monitor bounds are always tighter than 3σ** across all 39 migrated rows
   (`kRadUln` P1 σ=3/monitor=8 vs 3σ=9; P2 σ=10/monitor=25 vs 30; `kTrailWrist` P1
   σ=4/monitor=10 vs 12; …). So the precedence rule below is total, and every `mu`/`sigma`
   lands on an exactly-representable half-integer, making boundary z-values exact.

5. **The wrist grid's 39 cells are not Measures today.** `core.json` has 28 measures; none
   is a (DOF, position) cell. Stage 2 mints them.

6. **There are four two-tailed measures, not six or seven.** Only `m_ballPosition`,
   `m_ballBodyGap`, `m_shoulderAlignment` and `m_stanceWidth` carry two corridor tails.
   `m_lumbarCurve`, `m_thoracicCurve` and `m_spineBendAtAddress` are single-tail, which is
   the brief's §9.2 case. Of the four, **`ballPosition` is inverted** (`sig_ballForward` is
   `direction: high`, but 0 % = lead heel so forward is *low*); `ballBodyGap` and
   `stanceWidth` are correct.

7. **`shoulderAlignment` cannot be audited — its sign convention is undocumented.** The
   descriptor (`metric_catalogue_manifest.cpp:968`) never says which sign is open, and it is
   `planned = true`, so the convention is still unclaimed. Its `highMeans` must be authored
   alongside its producer.

8. **The pack provider is core + N user providers**, not three fixed layers
   (`merged_pack_provider.cpp:173`). `merged_norm_provider` mirrors the real shape.

9. **`TrailWristFlexExt` has no metric key.** Four of the five DOFs map to catalogue metrics
   (`leadWristFlexExt`, `leadWristRadUln`, `forearmPronation`, `leadArmFlexion`); trail wrist
   has none, so its seven cell-measures mint as `NoProducer` until a descriptor is added.

10. **Diagnostics tests register in `src/Analysis/tests/CMakeLists.txt`** via the `${DIAG}`
    variable (`core_pack_test` at :436), even though the `.cpp` lives in
    `src/Diagnostics/tests/`.

---

## The grade rule

```cpp
enum class Grade { Ideal, Good, Watch, Action, NotMeasured };

struct GradePolicy {            // ONE instance, from AppSettings, pack-wide
    double idealMaxZ  = 1.0;
    double goodMaxZ   = 2.0;
    double watchMaxZ  = 3.0;    // beyond -> Action
};
```

`z = (v - mu) / (v < mu ? sigmaLo : sigmaHi)`.

Precedence, when the norm carries explicit `monitorLo`/`monitorHi` (migrated content only):

```
outside [monitorLo, monitorHi]        -> Action
otherwise                             -> z-derived band, capped at Watch
```

Absent explicit bounds (everything authored in the corridor editor), the band is purely
z-derived. `BandTuning`'s SwingLab margin overrides apply to the resolved monitor band
either way.

**RAG collapse for parity:** Green iff `Ideal`; Red iff `Action`; Amber otherwise.

**Known consequence, accepted:** for migrated rows, `Ideal` is exactly the old green band, so
`Good` and `Watch` both fall inside the old amber. The 4-band grade and the legacy 3-band
`PpRag` are not a 2:1:1 mapping. Any surface showing both simultaneously must show only one.

**Provenance never modifies grade rendering.** A grade from a heuristic norm renders exactly
as one from a norm seated on 500 swings. A norm's standing appears only where a user goes to
interrogate it: the `MeasureDetail` norm row, the corridor editor, the health list, the
finding's detail page.

---

## Stages

### 1 — Types, tree, pack, providers

New in `src/Diagnostics/`:

- `norm.h` — `Norm`, `NormSource`, `Grade`, `GradePolicy`, `grade()`. Header-only, Qt-only.
- `context_tree.h/.cpp` — parse, validate (DAG, unknown-parent rejection, cycle rejection),
  resolve upward. Ships `any / full_swing{driver,fairway_wood,iron,wedge} / partial{pitch,chip}
  / bunker / specialty` plus `archetype_bowed` / `archetype_cupped`.
- `norm_pack.h/.cpp` — schema, JSON load/save, validation, versioning. Unit mismatch against
  the measure's unit is a **load error naming both**, never a silent grade.
- `norm_provider.h` + `resource_norm_provider.cpp` / `file_norm_provider.cpp` /
  `merged_norm_provider.cpp`, mirroring `resource_pack_provider.cpp`, `file_pack_provider.cpp`
  and `merged_pack_provider.cpp` — same layering (core resource + N user providers), same
  copy-on-write into the user layer, same `revertToShipped()` semantics.

Assets: `src/Resources/diagnostics/{norms.json, contexts.json}`, plus **move** `core.json`
there from `src/Diagnostics/packs/`. Keep the `:/diagnostics` resource prefix so
`pack_provider.h:73` and `CMakeLists.txt:614` need only a path change.

Tests: `norm_test`, `context_tree_test`, `norm_pack_test`.

### 2 — Migrate `reference_bands.cpp`, parity green before anything is deleted

| Source | Becomes |
|---|---|
| `kRadUln`, `kFlexExt`, `kForearm`, `kTrailWrist`, `kElbow` | 39 norm rows, context `full_swing` |
| the five `margin` fields (5, 5, 5, 6, 4°) | explicit `monitorLo`/`monitorHi` on those rows |
| `kArchetypeFaceOffsetDeg = 10.0` | norm rows under `archetype_bowed` / `archetype_cupped` overriding `full_swing` for `LeadWristFlexExt` |

Conversion: `mu = (greenLo + greenHi) / 2`, `sigmaLo = sigmaHi = (greenHi - greenLo) / 2`,
`monitorLo = greenLo - margin`, `monitorHi = greenHi + margin`, `source = Heuristic`, `n = 0`.

**Mint the 39 cell-measures** in `core.json` (28 → 67). Each is `metricKey` + a
`{delta, anchor p1, window [p1, pN]}` reducer — e.g. `m_leadWristRadUln_p3`. This is what
makes the wrist grid inspectable in the Measures catalogue instead of a compiled special
case, and it is the brief's stated intent ("expect 39 rows", keyed on `measureId`). Two
consequences to handle rather than discover later:
- Their consumer is the wrist assessment view, not a characteristic, so `usedBy` gains an
  `assessment:wrist` entry and the "measure with no users" health check must except them.
- Trail-wrist cells mint as `NoProducer` (fact 9) and must not enter the roadmap as pipeline
  work without a note.

Add `NormBandProvider : IReferenceBandProvider` projecting `Norm` → `Band`. Widen
`BandContext` with a `contextId` (do not add a parallel struct); `archetype` folds into it.
**Delete `ArchetypeBandProvider`.** `ConfigReferenceBandProvider` and `classifyDelta()` are
untouched until the gate is green.

Gate — must pass before any deletion:
- `reference_bands_parity_test` — byte-identical `PpRag` from `ConfigReferenceBandProvider`
  and `NormBandProvider` across every (DOF, position, archetype) and a swept delta range
  including exact band edges.
- `archetype_bands_test`, `tuning_overrides_test`, `tier1_banding_test`, `fault_rules_test`
  unchanged and green.
- `wrist_render_parity_test` — the wrist assessment view's cell colours, pill states and
  finding severities unchanged, against a fixture swing set.

### 3 — Wire into the engine  ◄ **the pack lights up**

Implement `INormProvider` behind the engine's `IMeasureSource` path so `MeasureReading::hasCorridor`
can finally be true. Resolution walks the context tree upward: `driver → full_swing → any → none`.

- A shot with no declared context resolves to `full_swing` and is marked `inferred`; any
  finding whose grading depends on it demotes confidence through the **existing** low-confidence
  path in `assessment_rules.cpp` — no second mechanism.
- No context-appropriate norm: report the deviation, name the norm actually used, grade
  **neutral, not red**.
- Absent norm still yields `Unavailable`, never a pass.

Extend `characteristic_engine_test` (correct tail fires; both tails of an axis resolve one
norm; absent norm still `Unavailable`) and `core_pack_test` (every `outsideCorridor` signal
resolves a norm in at least one context, so "the pack is dark" cannot silently return).

### 4 — Fix the direction inversion  ◄ **review gate**

Swap `sig_ballForward` to `direction: low` and `sig_ballBack` to `high` in `core.json`.
Audit the other three two-tailed measures (fact 6). Add `axis_direction_test`: for every
two-tailed measure the tails carry opposite directions, and each agrees with a fixture table
of expected semantics.

Add `QString highMeans` to `Measure` and author it for every measure that has a signal on it.
`shoulderAlignment` gets an explicit convention decided here (fact 7).

### 5 — `NormModel` + read-only norm UI  ◄ **review gate, independently shippable**

- `src/Gui/characteristics/norm_model.h/.cpp` — QML façade over measures, norms, contexts and
  (later) corridor editing. Adds `measureForMetricAtPhase(metricKey, phase)`, the join stage 9
  needs.
- `MeasureCatalogue.qml` — the fourth `_view` in `CharacteristicLibrary.qml:51`; extend the
  existing chip row at :130, do not restructure the panel. Grouped by measure group; filter
  chips for status / has-norm / layer; rows show label · unit · status dot · *"used by N"* ·
  a norm glyph, hollow when unset. **New measure** opens the existing `MeasurePicker` in mint
  mode. Reuse `CharacteristicLibraryModel::usageOfMeasure()` and `usersOfMeasure()`.
- `MeasureDetail.qml` — header; what it is (facet sentence or metric-key link); the `highMeans`
  line; **norms by context** in tree order with children indented, each row showing the resolved
  corridor in the measure's own units and either *inherited from &lt;parent&gt;* or *overridden*
  plus the delta and source; used-by; availability. Weak provenance is called out **on this row
  only**.
- The Diagnostics settings header strip (fact 1): grade policy control + norm-set selector.

### 6 — `CorridorEditor.qml`  ☑ built

**As built it needed a piece this section did not scope.** The live histogram rests on reading a
MEASURE off a stored swing, and `IMeasureValueSource` (`norm_measure_source.h`) was implemented only
by test fakes — nothing in the app could produce a single number to draw. That gap is now
`src/Diagnostics/measure_sample.{h,cpp}`: a per-swing **phase grid**, cached in a
`swing_phasegrid.json` sidecar guarded on swing.json size+mtime, holding a windowed median per
metric at each segmented phase (WristAngleSampler's own convention, not a second one) plus the
extremes between adjacent phases so an `extremum` window is exact by aggregation. It caches the
GRID, not measure values, so minting a measure does not stale every sidecar in the library.

Works in the measure's own units. The words `mu`, `sigma` and `z` never appear. Segmented
control over three routes:

- **Set by hand** — two draggable handles bound the **Ideal** band; centre → `mu`, each half-width →
  `sigmaLo`/`sigmaHi`. 44 pt touch targets plus a numeric readout beside each handle.
  ⚠ This said "the Good band" until 2026-07-26, which contradicted its own next clause: `mu ± sigma`
  is `|z| ≤ 1`, which the grade rule calls **Ideal**; Good is `|z| ≤ 2`. Stale from the brief's
  original band names, before Mark renamed them. `norm.h:111` is authoritative — `idealLo()` /
  `idealHi()` are "what the corridor editor's two handles bind to". The Good and Watch edges follow
  from the grade policy and are drawn, not dragged.
- **Seat from swings** — filter shots, mark the well-positioned ones, fit. Sets `n` and
  `source = Seated`. Save states that this sets the population norm for everyone on this norm set.
- **Import** — adopt a row from another norm pack, then adjust.

**The live histogram is not optional.** Drawn-from swings sit above the band; every handle drag
updates *"31 Ideal · 8 Watch · 3 Action"*. A corridor grading almost everything Action is
visibly wrong to someone who has never heard of a standard deviation — that is the safety
mechanism.

Draw-from selector (all swings / this athlete / this session) selects the *sample*, not the
scope. Provenance block carries route, `n`, date, author and the inheritance line. Editor
never exposes `monitorLo`/`monitorHi` — authored norms inherit the policy. Not-capturable
measures are refused with an explanation.

### 7 — Editable bindings + direction control  ☑ built  ◄ **review gate**

As specified, plus **the rule underneath it**, which this section did not scope. `applicable` and
`material` were persisted, marshalled and read by nobody: an editor over them would have been a
control that says more than it does, which is exactly the `C3` wart this plan already regrets. So:

- **`resolveContextBinding()` / `ownContextBinding()`** (`context_tree.h`) — a binding is an
  EXCEPTION, resolved by the same upward walk as a norm. Nearest row wins; nothing on the chain
  means applies and ranks. That is why the shipped pack carries no binding rows at all.
- **`detect(pack, source, contexts, contextId)`** — an inapplicable condition is **omitted**, not
  NotFired (assessed and absent) and not Unavailable (tried and failed). Defaulted arguments, so
  every existing caller is unchanged and a pack with no bindings cannot move.
- **`Finding::material` → `explain()`** — an immaterial finding is listed, explained and counted in
  coverage, and contributes **zero** to the ranking score. Zero, not a fraction: a fraction would be
  a number nobody could defend when asked why one cause outranked another.

On `CharacteristicEditorModel`: `contexts` (tree order, depth, applicable/material, own/inherited
and the ancestor it came from), plus `setBinding` / `clearBinding` / `undoBindingChange`. The two
setters return `{ ok, message, cascaded, canUndo }` rather than `void` — **the cascade** made that
necessary: switching a parent off must clear contradicting rows beneath it or the untick silently
does not take, and an action that changes rows the user cannot see has to say so and be reversible
in the same breath. `CharacteristicEditor.qml` renders a checkbox per context indented by the
model's own depth, quiet where a row merely repeats its parent, with a `PpToast` UNDO.

**Direction control.** `directionOptions(highMeans)` composes both tails in the measure's own words
in C++ — the low tail is stated as *the other end of that range*, quoting the one authored sentence
rather than inventing an opposite. Rendered in the signal rows and in the picker. `attachMeasure()`
keeps its signature; `mintMeasure()` takes `highMeans` in its facets, and **the picker refuses to
mint without it**, because an inverted signal is the one defect that cannot be caught downstream —
it fires happily, on the wrong swings, with correct-sounding consequence text attached.

`ContextBinding::corridorRef` removed at all four sites (`C6`). A pack still carrying the key loads
clean and drops it on the next save.

### 8 — The navigable DAG

`dag_layout.h/.cpp` in C++ — **no layout logic in QML**. Rank nodes by signed distance from
focus (causes negative, effects positive); barycentre ordering within rank; emit
`{id, kind, x, y, w, h}` per node and a cubic path per edge. Render with `Repeater` + `Shape`.

`DagView.qml` **replaces** the causes/effects block in `CharacteristicDetail.qml` — it is a
navigation surface, not an illustration. Tapping any node re-centres and pushes a breadcrumb;
back pops. Depth 1 default, **Expand** to depth 2 and stops. Long-press opens detail / adds as
cause / removes the edge. A measure node navigates into `MeasureDetail`. Encoding: edge weight
= strength; latent causes outlined; `Asserted` visually distinct and never rendered as
concluded; unavailable greyed with the missing measure named on tap.

The global picture stays the existing `causeCoverage()` bar list. Do not draw 50 nodes and 81
edges.

`dag_layout_test`: no overlaps, deterministic for a given focus, depth bound respected,
isolated node handled.

### 9 — Deletions and rewiring

Only once stages 2–3 are green:

- Convert the tempo inline corridor (`metric_catalogue_manifest.cpp:782`) to a norm row —
  `mu = 2.6`, `sigmaLo = sigmaHi = 0.4`, `monitorLo = 1.8`, `monitorHi = 3.6`,
  `source = Heuristic` — **before** deleting anything. Its `contextNote` prose (the
  Address→Top vs Takeaway→Top basis mismatch, provisional pending a corpus re-seat) moves to
  the norm's citation/note field. That information must not be lost.
- Delete `NormativeCorridor` and `MetricNormative::{inlineCorridors, dof, contextNote, heuristic}`
  (`metric_descriptor.h:51–66`), and `MetricCatalogue::corridor()`.
- Re-point `metric_catalog.cpp:183–202` at `NormModel` via `measureForMetricAtPhase()`. This
  single site feeds `MetricDetail.qml`, `PpBandRail`, `PpDashboardMotionZone` and
  `PpDashboardVerdictZone` — verify all four visually, they are recent work.
- `dashboard_reductions.h` takes corridors as parameters and is unaffected.
- Delete `reference_bands.cpp`'s compiled table once `NormBandProvider` is the only provider.
- `MetricDescriptor` keeps `phases`, `requirement`, `usedBy` — it describes the metric, it no
  longer judges it.
- `manifest_migration_test` — the converted tempo row produces the same band as before.

### 10 — Health checks

Extend `CharacteristicLibraryModel::health()`:

- Axes with no norm.
- Norms at `n = 0` **in the personal layer only** — scope via the existing pack-layer
  resolution, no new field. The 39 migrated rows must not appear, or the list becomes 39
  items of noise about content that was fine yesterday.
- Norms grading an implausible share of the drawn corpus into one band.
- Measures whose `howToRead` mentions club-dependence with no context override.
- Norms whose unit no longer matches their measure.
- Contexts with no norms anywhere beneath them.
- Overridden items where core has since changed (diff + **Take theirs**).
- **Must not fire:** the unread edge of a single-tail axis (`s_posture` reads one edge of
  `lumbar_curve`; the norm is still two-sided).

---

## Stage 4 — the direction audit, in full

All **30** corridor signals across 26 measures (four measures are two-tailed). The question for each
was one thing: *does a HIGH value of this measure mean this condition is present?* — answered from
the condition's own consequence text against the metric's documented sign convention. Pinned by
`axis_direction_test`, whose fixture carries the catalogue quote that decides each row, so a new
signal cannot be added unaudited.

**All 30 audited and correct. Nothing exempt, nothing unverifiable.** Both exemption lists in
`axis_direction_test` are empty and asserted to stay that way.

Mark then set the conventions that resolved all seven unverifiable signals, now written down in
`docs/design/pinpoint_sign_conventions.md`. There are **two families**:

- **Displacement — positive toward the lead side.** Lead-relative rather than left/right or "toward
  the target", matching the rest of the vocabulary: the same statement holds for a right- and a
  left-handed golfer, and the lead side is a property of the golfer where the target is a property
  of the shot.
- **Aim and path — positive is closed / in-to-out**, the launch-monitor convention, so `face − path`
  carries the sign the shot shape implies.

Their positives run opposite for a right-hander (closed points to the trail side). That is correct
and deliberate — a translation and a rotation, each following the convention its own readership
expects — and the doc says so explicitly, so nobody "fixes" it later.

### Fixed

| what | change | why |
|---|---|---|
| `sig_scooping` | high → **low** | `leadWristFlexExt`: *"+ is bowed/flexed, − is cupped/extended"*; scooping *adds loft*, so cupped |
| **`ballPosition` metric** | re-origined and re-signed | was `0 %` at the lead heel running to `100 %` at the trail heel — backwards under the rule. Now signed from the **middle of the stance**: `+50 %` lead heel, `0 %` centre, `−50 %` trail heel. A driver sits near `+50 %`, a wedge near `0 %`. Converted at emission (`wrist_analyzer.cpp`); `fracOfStance` stays raw so the plausibility gate and its tests are untouched. |
| `sig_ballForward` / `sig_ballBack` | high / low | unchanged in *meaning* — the metric moved under them, not the signal |
| `pelvisSway` descriptor | states the convention | positive = toward the lead side |
| `m_pelvisSwayBack` | `max` → **`min`** | sway is the negative extreme, so the reducer was looking at the wrong end |
| `sig_sway` | high → **low** | movement away from the lead side |
| `sig_hangingBack` | high → **low** | not having moved to the lead side by impact |
| `sig_slide` | unchanged (high) | toward the lead side — the one of the three that was right |
| `thoraxLateralDrift` | reworded | *"toward the target"* → *"toward the lead side"*, matching its counterpart |

### Wrong in a way a direction cannot express — both fixed

- **`sig_insufficientSet` read the wrong DOF.** It claims *"too little wrist angle by the top …
  less stored angle to release"* — the **hinge**, `leadWristRadUln`, which the catalogue labels
  "Lead wrist — hinge". It read `leadWristFlexExt` (bow/cup), a different axis entirely. Minted
  **`m_leadWristSetAtTop`** (`leadWristRadUln` at P4, live) and repointed the signal; `low` was
  already right once it was on the right axis. The catalogue's `usedBy` reverse index moved with it,
  which `diagnostics_catalogue_integrity_test` caught immediately.
  ⚠ `m_leadWristAtTop` (bow/cup at the top) is now read by no signal and will show in the health
  list as unused. Kept deliberately: it is a real live measure and it is the axis the archetype
  contexts are about — a cupped-at-the-top condition would ride it.
- **`sig_lossOfPosture` read the wrong end.** `m_spineBendLoss` took `extremum MAX` of
  (value − address) over P4→P7 — the most *added* forward bend, which the catalogue calls *"a dip"*,
  a different fault. Now `sense: min` + `direction: low`.

### `highMeans` authored and enforced

Every measure a corridor signal reads now says what a HIGH value means in its own words —
*"further forward, toward the lead foot"*, *"a more closed shoulder line, aimed further right of the
target for a right-handed golfer"*. 27 measures. `Measure::highMeans` is a new field on the pack
type, round-tripped through JSON, and `axis_direction_test` fails if a signal-bearing measure is
silent. This is what an author reads **instead of High/Low** when choosing a direction, and it is
the mechanism that stops the next inversion — all three that shipped came from choosing a direction
against a convention the author had to remember correctly.

### Alignment, resolved by the aim-family rule

`sig_alignmentOpen` → **low**, `sig_alignmentClosed` → **high** (both flipped): open is negative.
`shoulderAlignment`, `toeLineAngle` and `faceAngle` all now state it — the latter two carry no
corridor signal yet, so they were latent rather than broken, and stating it now means the signal
that eventually rides them cannot guess wrong.

**Nothing is unverifiable any more.** `axis_direction_test` asserts the no-stated-convention count
is **zero** and must stay there — a new entry is a metric that shipped without saying which way is
positive, which is exactly how the three inversions got in.

### Also noticed, not acted on

`m_axisTiltAtTop` reads `secondaryAxisTilt` **at P4**, but that metric's `howToRead` says *"read at
Impact"* and quotes its figures there. The reverse-spine reading may want P4 anyway — worth a look
when its producer lands.

---

## Stage 3 — the seed norms

13 rows, and every LIVE corridor signal now resolves one. **8 signals can fire; 0 are dark.**
`core_pack_test` asserts that, scoped to live measures — a signal on a producer-less measure cannot
fire whatever norms exist, so requiring one there would assert nothing.

| measure | corridor | grounding |
|---|---|---|
| `m_tempoRatio` | 2.2–3.0, monitor 1.8–3.6 | the catalogue's own inline corridor, migrated verbatim |
| `m_leadWristAtImpact` | 15–30° more flexed than address | stated outright in the metric's `howToRead` |
| `m_stanceWidth` | driver 115 %, wood 110 %, iron 100 %, wedge 88 %, unknown 102 % | ordinary coaching guidance, per club |
| `m_ballPosition` | driver 5 %, wood 18 %, iron 33 %, wedge 50 %, unknown 30 % | Mark's numbers, on the interoperable scale |
| `m_lagAngleDown` | 48–92°, deliberately wide | **weakest of the set** — an estimate, flagged in its own citation |

All are `source: heuristic, n = 0`, which is the schema saying exactly what they are; the health
list and the corridor editor both read it. A **literature review of every normative value** is
planned before any of them is treated as more.

**`sig_insufficientSet` needed no new norm.** It was repointed at `m_leadWristRadUln_p4` — already
"lead wrist hinge, change from address at P4", which *is* wrist set, and already carrying a migrated
norm. The measure minted for it a commit earlier was a duplicate on a worse quantity (the absolute
angle depends on the grip; how much a golfer hinged is the change from address) and was removed.
Reuse over minting is the whole point of the facet model's duplicate detection.

**Two club-dependent measures now have per-club rows** — stance width and ball position — which is
the context tree doing the job it was built for. One corridor would have been right for one club and
misleading-red for every other.

---

## Files

**New** — `src/Diagnostics/`: `norm.h`, `norm_pack.h/.cpp`, `norm_provider.h`,
`resource_norm_provider.cpp`, `file_norm_provider.cpp`, `merged_norm_provider.cpp`,
`context_tree.h/.cpp`, `measure_sample.h/.cpp` (stage 6, unplanned — see that section), `tests/`.
`src/Gui/characteristics/`: `norm_model.h/.cpp`, `norm_editor_model.h/.cpp`, `dag_layout.h/.cpp`,
`MeasureCatalogue.qml`, `MeasureDetail.qml`, `CorridorEditor.qml`, `DagView.qml`.
`src/Diagnostics/tests/`: `norm_model_test.cpp` (registered in `src/Analysis/tests/CMakeLists.txt`
alongside the other `${DIAG}` suites, per repo fact 10).
`src/Resources/diagnostics/`: `norms.json`, `contexts.json`, `core.json` (moved).

**Modified** — `src/Analysis/reference_bands.{h,cpp}`, `src/Diagnostics/characteristic.{h,cpp}`,
`src/Diagnostics/characteristic_engine.cpp`, `core.json` (moved and extended),
`src/Gui/characteristics/{characteristic_library_model,characteristic_editor_model}.{h,cpp}`,
`CharacteristicLibrary.qml`, `CharacteristicDetail.qml`, `CharacteristicEditor.qml`,
`src/Gui/review/metric_catalog.cpp`, `src/Metrics/metric_descriptor.h`,
`src/Metrics/metric_catalogue.{h,cpp}`, `src/Metrics/metric_catalogue_manifest.cpp`,
`src/Gui/app/app_settings.{h,cpp}`, `CMakeLists.txt` (resource block :614, sources :453–472,
QML :415–422), `src/Analysis/tests/CMakeLists.txt`.

---

## Verification

**Per stage** — `cmake --build build/Desktop_Qt_6_11_0-Debug --parallel 4 --target analyzer-tests`
then `ctest --test-dir build/analyzer-tests`. Build parallelism stays at 4 (the box OOMs above
that). Targeted builds during iteration; the full app build once per stage.

**The gate that governs stage 2** — `reference_bands_parity_test` and `wrist_render_parity_test`
must both be green before `reference_bands.cpp`'s table is deleted. If parity is not exact, the
conversion is wrong, not the target.

**Stage 3 milestone check** — run the engine against a fixture swing and confirm findings appear
where previously every condition reported `Unavailable`. `core_pack_test` asserts no
`outsideCorridor` signal is left without a norm.

**Stage 9 visual check** — the post-shot dashboard (`PpDashboardMotionZone`,
`PpDashboardVerdictZone`, `PpBandRail`) and `MetricDetail.qml` render corridors identically
before and after the re-point.

**Full suite before any commit** — all seven CTest suites at `-j4`, per the release runbook §0.5.

**Not gated on a corpus run.** Every number here is migrated, not fitted, so accuracy is
unchanged by construction. Re-seating the tempo corridor and any norm from real swings is
corpus-scale work and a separate exercise — a single swing never judges it.

---

## Decisions taken during execution

- **A signal fires on a DEVIATION (Watch or Action), not on leaving the Ideal band.** The brief does
  not settle this and it materially changes what the library detects. Ideal is |z| ≤ 1, so firing
  there would trip roughly a third of any normal population on every characteristic — a detector
  that flags a third of everyone is noise wearing a diagnosis's clothes. Good (|z| ≤ 2) is ordinary
  variation and the vocabulary should mean what it says. Firing additionally requires the value to
  be on the signal's own tail, or both conditions of an axis would fire on any deviation in either
  direction. **Worth confirming** — it is the single knob that sets the library's sensitivity.

- **An inferred context costs confidence, not the finding.** A shot that declares no context is
  graded against `full_swing` and multiplied by `kInferredContextConfidence = 0.7`
  (`characteristic_engine.cpp`), reusing the existing confidence channel rather than inventing a
  second mechanism. The deviation is real; what is uncertain is whether the right norm judged it.
  A context the tree does *not* recognise is different — it resolves to nothing and reports
  Unavailable.

- **`MeasureReading::fromCorridor()` exists to close a trap.** Setting `hasCorridor = true` while
  leaving `grade` at its `NotMeasured` default makes the signal silently never fire, which looks
  exactly like "nothing was wrong". Producers that have a band but no `Norm` must use the factory.

- **Trail wrist got a `planned` metric descriptor** (`trailWristFlexExt`,
  `metric_catalogue_manifest.cpp`). It was forced, not chosen: `Provided` pack measures must name a
  catalogue key, so without it the seven trail-wrist cells made the shipped pack fail validation and
  took `core_pack_test`, `characteristic_editor_test` and
  `diagnostics_catalogue_integrity_test` down with it. `planned` rather than `live` because
  `PpJointDof` lists the trail side as reserved for a later instrumentation pass — the corridors
  have existed since v1 with nothing producing a value. It declares **no `imuRoles`**: `SegmentRole`
  has no trail-side arm roles, and adding enum surface for an unbuilt producer would put the model
  ahead of anything using it. Catalogue census updated 52 → 53.

- **The P1 cell-measures use an `at` reducer, not a Δ.** `validateReducer` refuses a Δ whose anchor
  is its own window end, and Δ-from-address at address is identically zero. The P1 band still has to
  exist — `DofTrajectoryStrip.qml` draws the corridor polygon across P1..P8 and takes its y-range
  from every banded point, so dropping it would visibly change the strip. The engine forces
  `PpRag::Ref` at P1 before consulting any corridor, so the row is never graded. Revisit if the
  reducer model ever gains a zero-width delta.

- **`ConfigReferenceBandProvider` and `ArchetypeBandProvider` were kept, not deleted.** They are now
  unreachable from the app (the factory defaults to `Norm`) and exist solely as the parity test's
  reference implementation. ⚠ That makes `reference_bands_parity_test` a **migration gate, not a
  permanent one**: it pins norms.json to a frozen table, so the first legitimate corpus re-seat of a
  wrist corridor will fail it. Delete the test, both providers and the table together at stage 9.

- **The norm provider is cached process-wide** (`sharedNormProvider()`, a function-local static).
  `MetricCatalogue::corridor()` builds a band provider per call and the table it replaced was free;
  re-reading two JSON files per corridor lookup would have been a real regression. Call
  `resetSharedNormProvider()` after writing a user norm set — stage 6 will need it.

- **Offline targets embed the diagnostics resources separately.** `qt_add_resources` attaches to one
  target, so `swinglab_run` and `swing_window_parity_test` each get their own copy via
  `pinpoint_add_diagnostics_resources()`. Without it they would start with an empty norm set and
  silently grey the entire wrist grid — no error, no findings.

- **The grade policy is three NAMED presets, not three z spinboxes.** `GradePolicy` is pack-wide and
  singular by design — if "Ideal" means one thing here and something else in a shared pack, no grade
  is comparable across athletes. Lenient 1.5/2.5/3.5, Standard 1.0/2.0/3.0 (shipped), Strict
  0.75/1.5/2.25; the z values are shown under the control but are not editable, because a hand-tuned
  policy is a private vocabulary. The table lives in `norm_model.cpp`; `AppSettings` stores only
  which one is chosen. An unknown name resolves to Standard rather than persisting as itself.

- **"New measure" opens the picker ON A NEW CHARACTERISTIC DRAFT, not on nothing.**
  `CharacteristicEditorModel::mintMeasure()` writes into `m_draftMeasures`, which only persists via
  the draft's `save()` — so opening the picker with no draft would silently discard the mint. It is
  also the right model: a measure with no characteristic reading it trips `unusedMeasure` the moment
  it lands. The catalogue therefore calls `beginNew()` and `openMeasurePicker()` together.

- **`duplicateMeasure` was pulled forward from stage 10.** The structural duplicate detector
  compares SERIES, and a `Provided` measure has no series — it names a metric key — so the one class
  of measure that most easily duplicates was the only one unguarded. The stage-5 catalogue renders
  such a pair adjacent, so shipping the view without surfacing it would have put the defect on
  screen with nothing saying it was one. See the finding in the resume block.

- **`INormProvider::layers()` was added so the UI stops saying "merged".** The merged provider
  reports its CHILDREN; a leaf reports itself, named by its pack's own id rather than the path it
  was read from; a file provider that read nothing reports NO layer, so an empty user directory does
  not appear as a norm set the user never created. Stage 6 needs the same list to decide which set a
  write lands in.

- **The sample sidecar caches the PHASE GRID, not measure values.** The obvious cache is
  `measureId -> value` per swing; it is the wrong one, because the pack is editable and minting a
  measure would stale every sidecar and force a re-parse of a 2 GB library to look at one new
  corridor. Phases are a property of the swing, not the pack, so caching the grid the reduction
  reads makes new measures over already-produced metrics free — which is exactly the case the
  corridor editor creates.

- **A metric with an EMPTY curve is the normal case for a whole class of metric.** Every setup
  metric in a real swing.json — `stanceWidth`, `ballPosition`, `tempoRatio`, the foot-flare and
  toe-line angles — ships with no `value[]` at all and nothing but `phaseSamples`: they are read once
  at a position, so there is no curve to sample. The first implementation read only the curve and
  produced nothing for all of them, and the symptom was indistinguishable from "no swing carries
  this measure". The grid falls back to `phaseSamples`, and admits a phase the ladder lacks when a
  producer labelled one.

- **The scan runs on a worker and the draft guards against its own result.** A first pass over a
  72-swing library is tens of seconds of JSON parsing; after that the sidecars make it instant. A
  scan that finishes after the draft closed is DISCARDED rather than applied — landing one measure's
  swings under another measure's corridor would look perfectly plausible and be about the wrong
  thing entirely.

- **`canRevert` means "you have an override", not "something resolves here".** A shipped row is not
  inherited, so the obvious test admits it — and then offers a Revert that can only fail, because
  the core set is read-only by design. It checks the user pack instead.

- **"Reset to default" is TWO actions, not one.** Discarding unsaved changes and dropping a saved
  override are different in kind — one writes nothing, the other is a write — and a single button
  doing "whichever applies" would be a button whose effect you cannot predict before pressing it.

- **Which reset you are offered depends on whether CORE carries a row at that key**, and the label
  has to say so. Dropping a user row either restores the shipped corridor or leaves the context
  inheriting; promising the first when the second is what happens is how a destructive action
  acquires a reassuring name. The same defect existed on the characteristic side and was worse
  there — the button said "Restore shipped version" over a plain deletion.

- **"Edited" is TRACKED, never derived by comparing values.** A user row holding exactly the
  shipped numbers is still the user's row, and a value comparison would silently un-mark it the
  moment someone dragged a handle back to where it started. The merged provider records the keys a
  non-core layer supplied.

- **An inherited row reads as edited too.** A context with no row of its own, inheriting the user's
  override, is being graded by the user's corridor — `resolve().overridden` follows the resolution,
  and saying otherwise would be false about the number displayed beside it.

- **The axis must not move while a handle is dragged, and that is a CORRECTNESS rule, not styling.**
  The axis is derived from the corridor, so a live axis puts the pixel→value map inside its own
  output: widen the corridor, widen the axis, and the same pixel means more than it did a frame ago.
  Gain of ~3 per event. It belongs in C++ (`beginHandleDrag`/`endHandleDrag`) precisely because it
  is testable and the failure is not subtle.

- **No `drag.target` on anything whose position is a binding.** It assigns `x` imperatively and the
  binding never comes back, so the mark silently stops tracking the model — the numeric field moved
  the number and not the handle. Map absolute coordinates and leave the position bound.

- **Two PRE-EXISTING suites were unlinkable and are now fixed.** `profiler_controller_test` (core)
  and `reanalysis_controller_test` (gui) had been reporting "Not Run" — each was missing one source
  from its target (`AnalysisProfileLog.cpp`, `pp_os_metrics.cpp`). Neither is related to this work;
  they were fixed because the plan's own gate is "all seven suites", and a gate with two suites
  silently short is not a gate.

## The clean-up sweep — everything owed before this work is done

**This is the ledger, and it is the only durable one.** The "Resuming at stage N" block at the top
is rewritten every stage, so anything recorded only there is lost at the next handover. Every
deferral, known defect and "revisit later" belongs here, with the stage that closes it. Nothing is
deleted when it closes — it is marked closed, so a reader can tell the difference between a
question that was answered and one that was never asked.

**A final sweep runs after stage 10** and must find this table with nothing left open.

| # | Owed | Closes at | State |
|---|---|---|---|
| C1 | Duplicate measure — **resolved**: it was one series needing two reductions (see below) | 5 | ☑ closed |
| C1b | The wrist grid's Δ corridor (mu 8.0) vs this metric's own `howToRead` (~22.5) — **~15° apart** | corpus / C8 | ☐ open |
| C1c | The new absolute impact row has **no archetype siblings**, while its Δ sibling shifts ±10° | decide, then 6 or corpus | ☐ open |
| C1d | The archetype shift is a **flat ±10° at every position** — migrated constant, not a fitted model | corpus / C8 | ☐ open |
| C3b | The resolved archetype reaches the wrist grid but **not** the characteristic engine's `contextId` | with C3 | ☐ open |
| C2 | Norm-set **selector** is a census; `makeMergedNormProvider()` cannot skip a layer | 6 | ☑ closed |
| C3 | `AppSettings::diagnosticsGradePolicy` reaches the UI, not the engine | engine wiring | ☐ open |
| C4 | `resetSharedNormProvider()` must be called after every user-norm write | 6 | ☑ closed |
| C5 | Delete `reference_bands_parity_test` + `ConfigReferenceBandProvider` + `ArchetypeBandProvider` + the compiled table, together | 9 | ☐ open |
| C6 | Remove `ContextBinding::corridorRef` — **four sites, not one** (see below) | 7 | ☑ closed |
| C7 | Delete `MetricCatalogue::corridor()`; re-point `metric_catalog.cpp` at the norm join | 9 | ☐ open |
| C8 | **Literature review of every normative corridor**; `m_lagAngleDown` is the weakest | before the numbers are trusted | ☐ open |
| C9 | Tempo band re-cut (its low-side Watch band is empty — Good → Action at 1.8) | with C8 | ☐ open |
| C10 | 20 norms for producer-less measures — author each **with its producer**, never in a batch | per producer | ☐ open |
| C11 | `trailWristFlexExt` is a `planned` descriptor with **no** `imuRoles`; its 7 cells mint `NoProducer` | when the trail side is instrumented | ☐ open |
| C12 | P1 cell-measures use an `at` reducer; revisit if the reducer model gains a zero-width delta | if ever | ☐ open |
| C13 | `m_axisTiltAtTop` reads `secondaryAxisTilt` at P4; the metric's `howToRead` says Impact. `status: planned`, so nothing grades it yet | when its producer lands | ☐ open |
| C14 | Two direction/measure errors on single-tail signals (`sig_scooping`, `sig_insufficientSet`) | 4 | ☑ closed |
| C15 | `pelvisSway` and `shoulderAlignment` sign conventions undocumented, so unauditable | 4 | ☑ closed |
| C16 | `ragOf(Grade)` existed only inside the parity test | 5 | ☑ closed |
| C17 | `measureForMetricAtPhase()` — the join stage 9 depends on | 5 | ☑ closed |
| C18 | **`stanceWidth` reads ~2x its own corridor on real swings**, and both sides spell the unit identically | corpus / producer | ☐ open |
| C19 | The only local swings carrying the wrist DOF series are one 2026-06-11 session, and those series are **±180 wrapped** | capture / re-analyse | ☐ open |
| C20 | `m_tempoRatio` reads `at p4`; its producer labels the phaseSample at **P7** — same shape as C13 | when tempo is re-seated | ☐ open |
| C21 | Half the `stanceWidth` readings in the library are **0.1** — a producer failure, not a corridor one | producer | ☐ open |
| C22 | `CharacteristicEditorModel` keyed its user pack off `loaded`, not `parsed`, and `save()` then erased every other override — **fixed**, regression-tested | 6 follow-up | ☑ closed |
| C23 | `revertToShipped()` DELETED a user-created characteristic while reporting a restore — **fixed** | 6 follow-up | ☑ closed |
| C24 | Handle drag ran away (axis derived from the corridor fed back into the pixel→value map) — **fixed**, regression-tested | 6 drag fix | ☑ closed |
| C25 | Bindings resolve and `detect()` honours them, but **nothing constructs the engine in the app** — same gate as `C3`, and the engine must be given the shot's context when it is wired | engine wiring | ☐ open |
| C26 | The characteristic library list rendered UNDERNEATH the authoring sheet on the "New characteristic" path (`_editing` was not in its `visible`) — **fixed** | 7 | ☑ closed |
| C27 | Tapping a near-duplicate in the measure picker COMMITTED a tail the author had not seen yet, read against a different measure's convention — now selects, and the tail is chosen after — **fixed** | 7 | ☑ closed |
| C28 | 40 of 67 core measures carry no `highMeans`. The picker now refuses to mint without one, so this is a floor for NEW content only; the existing 40 are all wrist cell-measures with no signals, and `axis_direction_test` still gates every signal-bearing one | with C10 / per producer | ☐ open |

### C18–C21 — what happened the first time the pack met real swings

Stage 6's histogram is described in the brief as the safety mechanism: *"a corridor grading almost
everything Action is visibly wrong to someone who has never heard of a standard deviation"*. The
first measure opened in the finished editor rendered **11 Action out of 11**, and every one of the
entries below is something that screen made obvious in a second and no test had caught.

The library is 72 swings, one athlete, all Wrist sessions (`/mnt/swingdata/Mark-Liversedge`).

**C18 — stance width is out by about 2x, and nothing could have caught it.** The shipped
`full_swing` corridor is `mu 102, sigma 12` in **% shoulder width**; the eleven real readings that
are not broken run **220.7 to 257.7** in a field the producer also labels *"% shoulder width"*
(`foot_metrics.cpp:305`). `normUnitMismatch` compares unit STRINGS and they match exactly, so the
referential validator is structurally incapable of seeing this. Either the producer's denominator is
not the shoulder width it claims, or the corridor was authored against a different convention. It
must be resolved by looking at one swing's actual pixels, not by moving either number to meet the
other.

**C19 — the wrist DOF series cannot be seated locally at all.** Only the **2026-06-11** session
carries `leadWristFlexExt` / `leadWristRadUln` (8 swings of 72); every session since carries none.
Those eight range **-180 to +180** and read -138 deg at the top and -160 deg at address, which is not
anatomy — the series is wrapped. The phase-grid reduction is not at fault: it reproduces each
file's own `phaseSamples` exactly (swing_0005 P1: computed -159.90 vs the file's -159.9). So the 30
wrist-grid measures have **no usable local sample**, and any corpus work on them needs a capture or
re-analyse pass first.

**C20 — `m_tempoRatio` asks a phase its producer does not label.** The measure is `at p4` (Top); the
producer emits a single `phaseSamples` entry at **P7 (Impact)**. The measure therefore resolves
unavailable on every swing in the library, silently — exactly the C13 shape, and worth a sweep for
others rather than a fix for this one.

**C21 — half the stance-width readings are 0.1.** Six of the eleven producing swings read `0.1`
where the other five read 220-258. That is a producer failure (a denominator or a keypoint that did
not resolve) reported as a value rather than as absent, which is the one thing
`IMeasureValueSource` exists to keep apart. A measure that cannot be produced must report nothing.

**What is NOT wrong:** `m_ballPosition` graded **4 Ideal, 2 Good** out of 6 against its shipped
`full_swing` corridor. The pack is not uniformly miscalibrated — which is why these four are worth
naming individually rather than filing as "the seed norms need work".

### C1, resolved — it was one series needing two reductions

`m_leadWristAtImpact` and `m_leadWristFlexExt_p7` were both authored as `delta p1→p7` on
`leadWristFlexExt`, so they computed the same number and the `duplicateMeasure` warning fired. The
warning was right; the diagnosis of "delete one" was wrong.

**Mark's reading was the correct one**: flexion is a time series, and these are two *reductions* of
it to a position — an absolute value at impact and a change from address to impact. That is exactly
what the series/reducer split exists for (`measure_facets.h`: "two measures over one series with
different reducers are related, not duplicates"). Both should exist. What was broken was that one
of them had the wrong reducer: `m_leadWristAtImpact`'s own **id** says "at impact", but its reducer
said Δ and its label said "change P1 to P7".

Three facts settled it, all verified rather than assumed:

- `metric_extractor.cpp:103` emits the **absolute** anatomical angle — no address subtraction. So
  the series is absolute and both reductions are expressible from it.
- `wrist_assessment_engine.cpp:80` grades `valueDeg − p1Value`, so the wrist grid is genuinely
  Δ-from-address and its cell measure is correctly a Δ. Corroborated by the corridor shape: the P1
  cell is `mu 0.0 ± 5`, which is meaningless as an absolute address flexion (address is cupped,
  −10 to −15°) but is exactly what a Δ must be at its own anchor.
- `swing_scorer.cpp:54` already grades an **absolute** impact band — `pp_tuned_constants.h:84`
  `kFlexExtMu = 15.0, kFlexExtSigma = 12.0`, one-sided `+1` ("penalise BELOW μ (cupping)"),
  *"locked against HackMotion in Corpus 2"*. So the absolute corridor did not have to be invented.

**What changed:** `m_leadWristAtImpact` is now `at p7` with unit `°`, `highMeans` reworded to the
absolute sense, and its norm re-seated to `mu 15.0, sigma 12.0` from the scorer constants.
`m_leadWristFlexExt_p7` is untouched — it is parity-locked and correct as the Δ. The
`duplicateMeasure` warning is gone, and `measureForMetricAtPhase(leadWristFlexExt, Impact)` now
returns the absolute reading, which is what a corridor keyed on (metric, phase) means.

The scorer's one-sided band is also the better fit for `sig_scooping`: scooping IS a cupped wrist at
impact, which is the low tail of the absolute reading, and the scorer penalises exactly that side.

### C1b — the residue, which is a real disagreement about the swing

Isolating the absolute reading did not make the numbers agree; it narrowed the disagreement to the
**Δ** alone:

| claim | source | Δ address→impact |
|---|---|---|
| *"impact sits roughly 15–30° more flexed than address"* | the metric's own `howToRead` | ≈ 22.5 |
| the compiled wrist-grid table, migrated at stage 2 | `reference_bands.cpp` | 8.0 (ideal 1–15) |

~15° apart — nearly the full width of either corridor. The three figures are self-consistent only
under the `howToRead` reading (address ≈ −7.5° cupped, impact ≈ +15° absolute, Δ ≈ +22.5°), which
makes the table's 8.0 the outlier.

The table's shape says something too: it **peaks at P6 (9.5) and falls to 8.0 at P7** — bow maxing
out before impact and releasing into it — where the descriptor's prose implies impact at or near
maximum bow. Those are different claims about the swing, not different arithmetic.

⚠ **If `howToRead` is right, the shipped wrist grid has been grading bow/cup against a corridor
~15° too low since v1.** This cannot be resolved by inspection and must not be "fixed" by editing
the table: it is pinned by `reference_bands_parity_test` until C5 deletes it, and that pin is
deliberate. It is a corpus question, and it belongs with the literature review (C8).

### C1c / C1d / C3b — bowed and cupped are both valid, and this is how that is carried

Playing from a cupped **or** a bowed top is legitimate, so a single corridor on the face axis would
red-flag one valid style. That is what the archetype contexts exist for: `archetype_bowed` and
`archetype_cupped` sit under `full_swing` and carry their own lead-wrist face corridors, so a
resolved bowed player is graded against the bowed model. 16 rows — 8 positions × 2 archetypes, all
on `leadWristFlexExt`. Every other DOF inherits `full_swing` and is archetype-invariant, which
`reference_bands_parity_test` asserts. Detection is `wrist_resemblance.h`; the resolved model lands
in `WristAssessmentResult::archetype` (0/1/2) and reaches bands via `BandContext`.

Three things that are NOT settled by the above:

**C1c.** The absolute impact row added when C1 was resolved (`m_leadWristAtImpact`, `mu 15.0,
sigma 12.0`) has only a `full_swing` row, while its Δ sibling shifts ±10° per archetype. There is a
plausible reason to leave it: σ=12 is wide enough that a ±10 archetype sits inside 1σ, so a bowed
player at +25° absolute still grades Ideal — the corridor absorbs the style spread by construction,
where the Δ corridor (σ=7) cannot. **That reasoning needs confirming or rejecting, not assuming.**
If it is rejected, the row needs two archetype siblings and they need grounded numbers.

**C1d.** The shift is `kArchetypeFaceOffsetDeg = 10.0` applied uniformly P1→P8 — the compiled
constant migrated faithfully, which was stage 2's whole remit. It is not a fitted per-position
model, and a real bowed player is unlikely to sit at exactly +10 at address, at the top AND at
impact. Re-seating per position is corpus work; goes with C8.

**C3b.** The archetype reaches the wrist grid today (`BandContext.archetype`), but the
characteristic engine takes a `contextId` **string** and nothing constructs a `NormMeasureSource`
yet (C3). When the engine is wired, it must pass the resolved archetype context — otherwise a bowed
player is graded correctly in the wrist grid and against neutral in every finding, from the same
swing, which is worse than either alone.

### C6, verified — it is not the one-line deletion the stage-7 text implies

`corridorRef` is described in stage 7 as "currently unused". It is unused as a *rule*, but it is
wired in four places and one of them reaches QML, so deleting it is a small migration rather than a
field removal:

- `src/Diagnostics/characteristic.h:168` — the field itself
- `src/Diagnostics/characteristic_pack.cpp:751` — read from JSON on load
- `src/Diagnostics/characteristic_pack.cpp:860` — written back on save
- `src/Gui/characteristics/characteristic_library_model.cpp:263` — marshalled into the binding map
  the detail page renders

Removing the field without the load path leaves any pack that carries the key failing to round-trip.

### C2 and C3, in full — stage 5 shipped these knowingly incomplete

**C2.** `NormModel::normSets()` lists the real layers through the new `INormProvider::layers()`, and
the strip renders them — but nothing can switch which set *resolves*, because
`makeMergedNormProvider()` has no way to skip a layer. There is exactly one set today, so a selector
would have been a dropdown with one entry and no effect. Stage 6 creates the second set (the user's
own), which is when the switch becomes meaningful and when the merged provider needs the filter.

**C3.** `NormModel` applies the grade policy to every band edge it renders, so the setting is real
where it is shown. `NormMeasureSource` already takes a `GradePolicy` constructor argument — but
**nothing in the app constructs a `NormMeasureSource` yet**, so the setting does not reach grading.
Wire it at the same time the engine is wired into the pipeline, or the control will silently mean
less than it says.

---

## Open questions, resolved (kept for the record)

- **Two more direction/measure errors found while preparing the seed norms**, both on
  SINGLE-tail signals, which the planned "audit the two-tailed axes" pass would not have reached:
  `sig_scooping` carries `direction: high` but its own consequence text is *"adding loft through
  impact"* — cupping, which is negative on `leadWristFlexExt`; and `sig_insufficientSet` reads
  `leadWristFlexExt` (bow/cup) when *"too little wrist angle by the top… less stored angle to
  release"* is the **hinge** (`leadWristRadUln`). Stage 4 must therefore audit **all 26** corridor
  signals, not the four two-tailed ones. — **CLOSED at stage 4**, which audited all 30.

- **Two sign conventions are undocumented, so their signals cannot be audited at all:**
  `pelvisSway` (is positive toward the target? decides whether `sig_hangingBack: high` is right)
  and `shoulderAlignment`. This is the argument for `highMeans` being authored WITH each producer.
  — **CLOSED at stage 4**: both stated, `docs/design/pinpoint_sign_conventions.md` written, and
  `axis_direction_test` now asserts the no-stated-convention count is **zero**.
