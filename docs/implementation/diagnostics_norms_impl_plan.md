# Diagnostics norms — implementation plan

Companion to the brief (`docs/design/diagnostics_norms.md`). The brief is the *what*; this is
the *how*, sequenced, with every repo fact it depends on verified rather than assumed.

**This is a live working document.** Update the Progress table and Session log as stages land.
Work spans multiple sessions with context clears between them — the tables below are what makes
a cold restart possible. Do not treat any stage as done until its gate has actually run green.

---

## Progress

| # | Stage | State | Landed |
|---|---|---|---|
| 1 | Types, tree, pack, providers | ☑ complete | 2026-07-25 · 287f3ea |
| 2 | Migrate `reference_bands.cpp` — parity gate | ☑ complete | 2026-07-25 · 287f3ea |
| 3 | Wire into the engine — **the pack lights up** | ◐ mechanism done, **content owed** | 2026-07-25 · 287f3ea |
| 4 | Direction audit (all 30 signals) + `highMeans` | ◐ **26/30 audited, 2 owed + `highMeans`** | 2026-07-25 |
| — | **review gate · expect context clear here** | | |
| 5 | `NormModel` + read-only norm UI | ☐ not started | |
| — | **review gate** | | |
| 6 | `CorridorEditor.qml` | ☐ not started | |
| 7 | Editable bindings + direction control | ☐ not started | |
| — | **review gate** | | |
| 8 | The navigable DAG | ☐ not started | |
| 9 | Deletions and rewiring | ☐ not started | |
| 10 | Health checks | ☐ not started | |

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
| 2026-07-25 | pre-4 | **Three decisions from Mark, all landed.** (a) Fire-on-deviation confirmed. (b) `stanceWidth` is now **% of shoulder width** — invariant unit, computed from the shoulder pair over the same address reference frames; the millimetre reading moved to its own `stanceWidthMm` metric so both units stay invariant. (c) The two spinal measures are **roadmap items, not capture gaps**: they cannot come from the pose skeleton, but a DTL back-contour producer would resolve them, so `roleNeedsNonPoseSensor` keeps its detection and loses its "never" conclusion. Seed pack now has **zero capture gaps**. Tempo band re-cut DEFERRED pending a literature review. Analyzer suite 73/73; app + swinglab_run build. |

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

### 6 — `CorridorEditor.qml`

Works in the measure's own units. The words `mu`, `sigma` and `z` never appear. Segmented
control over three routes:

- **Set by hand** — two draggable handles bound the Good band; centre → `mu`, each half-width →
  `sigmaLo`/`sigmaHi`. 44 pt touch targets plus a numeric readout beside each handle.
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

### 7 — Editable bindings + direction control  ◄ **review gate**

On `CharacteristicEditorModel`:

```cpp
Q_PROPERTY(QVariantList contexts READ contexts NOTIFY draftChanged)   // tree order, with depth
Q_INVOKABLE void setBinding(const QString &contextId, bool applicable, bool material);
Q_INVOKABLE void clearBinding(const QString &contextId);
```

Rendered in `CharacteristicEditor.qml` as a checkbox per context, children indented; unticking
a parent unticks children with an undo toast. Where a signal's direction is chosen, High/Low is
replaced by the measure's own `highMeans` words. `attachMeasure(measureId, direction)` keeps
its signature.

Remove `ContextBinding::corridorRef` (`characteristic.h:156`) — redundant once norms key on
`(measureId, contextId)`, and currently unused.

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

**26 of 30 audited and correct. 2 wrong in a way direction cannot express. 2 unverifiable.**

Mark then set a project-wide rule that resolved five of them at once — **positive is toward the
lead side, in every context** — now written down in `docs/design/pinpoint_sign_conventions.md`. It
is lead-relative rather than left/right or "toward the target", matching the rest of the vocabulary:
the same statement holds for a right- and a left-handed golfer, and the lead side is a property of
the golfer where the target is a property of the shot.

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

### Wrong in a way a direction cannot express — need a decision

- **`sig_insufficientSet` reads the wrong DOF.** It claims *"too little wrist angle by the top …
  less stored angle to release"*, which is the **hinge** — `leadWristRadUln`, labelled "Lead wrist —
  hinge". `m_leadWristAtTop` reads `leadWristFlexExt` (bow/cup). Needs a new measure on the hinge at
  P4; the `low` direction is already right for it.
- **`sig_lossOfPosture` reads the wrong end.** `m_spineBendLoss` is `extremum MAX` of
  (value − address) over P4→P7, which finds the most *added* forward bend — what the catalogue
  calls *"a dip"*. Losing posture is standing up: the *minimum*. As authored it detects the opposite
  fault. Needs `sense: min` + `direction: low`, or a measure defined as the loss magnitude.

### Still unverifiable — 2 left

`sig_alignmentOpen` / `sig_alignmentClosed`: `shoulderAlignment` never says which sign is open. It
is a `planned` metric, so the convention is free to choose, and the natural reading under the rule
is **positive = open** (an open line for a right-hander points left, the lead side). Not decided —
decide it when the producer is written. The two tails are at least opposite each other, so they are
internally consistent whichever way it lands.

### Also noticed, not acted on

`m_axisTiltAtTop` reads `secondaryAxisTilt` **at P4**, but that metric's `howToRead` says *"read at
Impact"* and quotes its figures there. The reverse-spine reading may want P4 anyway — worth a look
when its producer lands.

---

## Files

**New** — `src/Diagnostics/`: `norm.h`, `norm_pack.h/.cpp`, `norm_provider.h`,
`resource_norm_provider.cpp`, `file_norm_provider.cpp`, `merged_norm_provider.cpp`,
`context_tree.h/.cpp`, `tests/`.
`src/Gui/characteristics/`: `norm_model.h/.cpp`, `dag_layout.h/.cpp`, `MeasureCatalogue.qml`,
`MeasureDetail.qml`, `CorridorEditor.qml`, `DagView.qml`.
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

## Open, to raise when reached

- **Two more direction/measure errors found while preparing the seed norms**, both on
  SINGLE-tail signals, which the planned "audit the two-tailed axes" pass would not have reached:
  `sig_scooping` carries `direction: high` but its own consequence text is *"adding loft through
  impact"* — cupping, which is negative on `leadWristFlexExt`; and `sig_insufficientSet` reads
  `leadWristFlexExt` (bow/cup) when *"too little wrist angle by the top… less stored angle to
  release"* is the **hinge** (`leadWristRadUln`). Stage 4 must therefore audit **all 26** corridor
  signals, not the four two-tailed ones.

- **Two sign conventions are undocumented, so their signals cannot be audited at all:**
  `pelvisSway` (is positive toward the target? decides whether `sig_hangingBack: high` is right)
  and `shoulderAlignment`. This is the argument for `highMeans` being authored WITH each producer.

- **A literature review of every normative corridor** is planned before the numbers are treated as
  anything but starting heuristics. The tempo band re-cut waits for it: migrating its explicit
  1.8–3.6 monitor leaves the low-side Watch band empty (the lower amber edge sits at exactly −2σ),
  so a rushed transition jumps Good → Action at 1.8. Deliberate, and revisited with the review.
- `shoulderAlignment`'s sign convention (fact 7) — needs deciding at stage 4, alongside
  whoever writes its producer.
