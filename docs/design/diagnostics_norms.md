# Brief — Diagnostics: population norms, corridor editing, and the navigable DAG

**For:** Claude Code (Opus 5), working in `~/Projects/PinPoint`
**Read first:** `CLAUDE.md`, `src/Diagnostics/characteristic.h`, `src/Diagnostics/characteristic_engine.h`, `src/Diagnostics/pack_provider.h`, `src/Gui/characteristics/characteristic_editor_model.h`, `src/Gui/characteristics/characteristic_library_model.h`, `src/Gui/characteristics/CharacteristicLibrary.qml`, `src/Analysis/reference_bands.h`, `src/Metrics/metric_descriptor.h`

**This work is additive.** The Diagnostics module, the pack stack, copy-on-write, the measure picker, the roadmap and the health view all exist and work. Do not restructure them. Almost everything below either fills a hole (`hasCorridor` is never true) or extends an existing seam.

---

## 0. Why this is urgent

30 of 31 signals in `packs/core.json` use `SignalTest::OutsideCorridor`. `MeasureReading::hasCorridor` is filled by nobody outside `characteristic_engine_test.cpp`. The engine correctly reports `Unavailable`, so **the entire seed pack is dark**. Corridors are not one input among many — they are the detection mechanism.

---

## 1. Naming — settle this before writing code

`reference` is already taken in this codebase. `CharacteristicEditorModel::referencesFor(whatRole, quantity)` and `Series::reference` mean the facet *"relative to"* role. Introducing "reference" for normative distributions will collide in the API, the JSON and the UI.

**Use `norm` throughout**: `Norm`, `NormProvider`, `norms.json`, `NormModel`, "normative values" in user-facing strings. Never "reference" for this concept.

The settings panel is already labelled **Diagnostics** (`navIdx: 10`, `SettingsIndex.qml` `panelIndex: 10`). Keep it.

---

## 2. All norms are population norms

Decided: norm data is **population-based only**. There is no per-athlete norm, no personal baseline, in this work.

Consequences you must honour:

- A norm is never keyed by athlete id. The schema must not carry one.
- "Seat from swings" (§6.3) fits a norm from a *set* of swings the user selects. It records `n` and the selection scope in provenance, but the resulting norm applies to everyone using that norm pack. The UI must say so at the point of saving: *"This sets the population norm for everyone using this norm set."*
- `IReferenceBandProvider`'s comment anticipates a future "player-baseline" source. Leave the seam; do not implement it. If you find yourself adding an athlete id to a norm record, stop — that is a different feature.

---

## 3. Data model

### 3.1 Norm

```cpp
enum class NormSource { Heuristic, Seated, Literature, Imported };

struct Norm {
    QString     measureId;      // keys on the MEASURE (post-reducer), never the metric key
    QString     contextId;      // node in the context tree
    double      mu       = 0.0;
    double      sigmaLo  = 0.0; // tolerance below mu
    double      sigmaHi  = 0.0; // tolerance above mu; defaults to sigmaLo when absent
    std::optional<double> monitorLo;  // absolute value; absent => derived from GradePolicy
    std::optional<double> monitorHi;  // absent => derived from GradePolicy
    int         n        = 0;
    NormSource  source   = NormSource::Heuristic;
    QString     unit;           // MUST match the measure's unit; load fails if not
    QString     author;
    QString     citation;       // DOI/PMID where the norm comes from literature
    QDate       setOn;
};
```

Asymmetric sigma is required, not optional — ball position forward is tolerated far more than back, and the same holds for alignment and stance width.

**`monitorLo` / `monitorHi` exist solely to preserve migrated content exactly.** The existing amber margins are a fixed number of degrees added either side of green, so they are not a fixed multiple of the green half-width and cannot be reproduced by a global z policy. `kRadUln` P1 has green ±3.0 with a 5.0 margin (amber = 2.67× green); P2 has green ±10.0 with the same 5.0 margin (amber = 1.5× green). The tempo corridor is not expressible at all — green 2.2–3.0 with amber 1.8–3.6 needs a low margin of 0.4 and a high margin of 0.6.

Rules:
- Norms produced by §5 migration set `monitorLo`/`monitorHi` explicitly. Old bands reproduce byte-identically.
- Norms authored in the corridor editor leave them **absent** and inherit `GradePolicy`. The editor never exposes them, and the two-handle interaction is unchanged.
- `BandTuning`'s SwingLab margin overrides apply to the resolved monitor band, whether explicit or derived.

**Key on `measureId`, not `metricKey`.** A measure carries its reducer and phase. `NormativeCorridor::deltaFromAddress` exists today precisely because a Δ corridor must not be compared against an absolute value; keying on the measure makes that structural rather than a flag.

### 3.2 Grade bands — one global policy

```cpp
struct GradePolicy {          // ONE instance, from AppSettings, applies pack-wide
    double solidZ   = 1.0;    // |z| <= 1        -> Good
    double monitorZ = 2.0;    // 1 < |z| <= 2    -> Solid
    double outsideZ = 3.0;    // 2 < |z| <= 3    -> Monitor;  > 3 -> Outside
};

enum class Grade { Good, Solid, Monitor, Outside, NotMeasured };
```

Not per-norm and not per-condition. If "Good" means ±1 SD in one pack and ±1.5 in another, nothing is comparable across athletes or across shared packs. Expose one control in Diagnostics settings; do not expose it in the corridor editor.

`z` is computed per side: `z = (v - mu) / (v < mu ? sigmaLo : sigmaHi)`.

**Provenance never modifies grade rendering.** A grade derived from a heuristic norm renders exactly as one derived from a norm seated on 500 swings. Colour on a finding encodes distance from the norm and nothing else. A norm's standing — source, `n`, date, inheritance — is surfaced where a user goes to interrogate it: the norm row in `MeasureDetail`, the corridor editor, the health list, and the finding's own detail page. It is never smeared across the dashboard.

This is a migration requirement as much as a design one: the wrist assessment view must look identical before and after §5.1. See §10.

### 3.3 Context tree

New pack data, not compiled. A context is `{ id, label, parentId }`. Ships as:

```
any
└── full_swing            DEFAULT — an unstated binding means this
    ├── driver
    ├── fairway_wood
    ├── iron
    └── wedge
partial
├── pitch
└── chip
bunker
specialty
```

Norm resolution **walks up the tree**: no norm for `driver` → try `full_swing` → try `any` → none. Authors write a row only where it genuinely differs from its parent.

Contexts are user-editable (add a node, set its parent). Anatomy roles are not — they map to keypoint indices.

### 3.4 New field on `Measure`

```cpp
QString highMeans;   // "further back, toward the trail foot"
```

Rendered wherever a signal direction is chosen (§6.5). This is a correctness mechanism, not decoration — see §9.1.

---

## 4. New files

```
src/Diagnostics/
  norm.h                    Norm, NormSource, Grade, GradePolicy, grade()
  norm_pack.h/.cpp          schema, JSON load/save, validation, versioning
  norm_provider.h           INormProvider (abstract) + factory
    resource_norm_provider.cpp   shipped norms (Qt resource)
    file_norm_provider.cpp       user norms, QStandardPaths::AppDataLocation
    merged_norm_provider.cpp     layered, copy-on-write, mirrors merged_pack_provider.cpp
  context_tree.h/.cpp       parse, validate (DAG, single root per branch), resolve upward
  packs/norms.json          the shipped norm set
  packs/contexts.json       the shipped context tree
  tests/

src/Gui/characteristics/
  norm_model.h/.cpp         QML façade: measures, norms, contexts, corridor editing
  dag_layout.h/.cpp         layered ego-graph layout — C++, no QML logic
  MeasureCatalogue.qml      the fourth _view
  MeasureDetail.qml         norms by context
  CorridorEditor.qml        the corridor editor
  DagView.qml               the navigable graph
```

`merged_norm_provider.cpp` must mirror `merged_pack_provider.cpp` exactly: three layers (core resource → community → personal), first hit wins, copy-on-write into personal on edit, `revertToShipped()` equivalent.

---

## 5. Migration — the part that must not break anything

### 5.1 Absorb `reference_bands.cpp`

Every hardcoded value survives as content. The inventory is exactly:

| Source | Content | Becomes |
|---|---|---|
| `kRadUln`, `kFlexExt`, `kForearm`, `kTrailWrist`, `kElbow` | 39 valid green corridors (5 DOFs × 8 positions, less TrailWrist P8) | 39 norm rows, context `full_swing` |
| the five `margin` fields (5.0, 5.0, 5.0, 6.0, 4.0°) | amber half-widths | explicit `monitorLo`/`monitorHi` on those rows |
| `kArchetypeFaceOffsetDeg = 10.0` (`Core/pp_tuned_constants.h`) | bowed / cupped face shift | two context nodes with norm rows overriding `full_swing` for `LeadWristFlexExt` |

Each (DOF, position) pair is a distinct `measureId`, so expect 39 rows.

Conversion: `mu = (greenLo + greenHi) / 2`, `sigmaLo = sigmaHi = (greenHi - greenLo) / 2`, `monitorLo = greenLo - margin`, `monitorHi = greenHi + margin`, `source = Heuristic`, `n = 0`.

**`ArchetypeBandProvider` is deleted, not ported.** The ±10° shift stops being a compiled special case for one DOF and becomes two ordinary norm rows under `archetype_bowed` and `archetype_cupped` contexts. `BandContext::archetype` folds into `contextId` with the rest. This is the point of the refactor: a number that was code becomes content anyone can inspect, override or re-seat.

**Write a test that asserts byte-identical band output before and after.** `classifyDelta()` becomes a thin wrapper over `grade()`; for every (DOF, position, archetype, delta) tuple across a swept range, the old and new paths must return the same `PpRag`. With explicit monitor bands this is achievable exactly — if it is not passing, the conversion is wrong, not the target. Do not delete `reference_bands.cpp` until that test is green.

`BandContext{archetype, club, shape}` widens to carry a `contextId`. Do not add a parallel struct.

### 5.2 Delete the metric-side corridors

Once §5.1 is green:

- `MetricNormative::inlineCorridors`, `::dof`, `::contextNote`, `::heuristic` — deleted.
- `NormativeCorridor` — deleted.
- The one populated `inlineCorridors` site in `metric_catalogue_manifest.cpp` (~line 782, tempo ratio) converts to a norm row **before** deletion: `mu = 2.6`, `sigmaLo = sigmaHi = 0.4`, `monitorLo = 1.8`, `monitorHi = 3.6`, `source = Heuristic`. Its `contextNote` prose moves to the norm's `citation`/note field — it records that the figures are measured on a different basis and are provisional pending a corpus re-seat, which is information that must not be lost. Run the converter first; do not lose the numbers.
- `MetricDescriptor` keeps `phases`, `requirement`, `usedBy`. It describes the metric; it no longer judges it.

`Gui/review/chart_metrics.cpp` and `Analysis/dashboard_reductions.h` currently consume corridors. `dashboard_reductions` takes them as parameters and is unaffected. `chart_metrics.cpp` must ask `NormModel` instead — `Gui` already depends on both modules, so the dependency direction is fine.

### 5.3 Remove `ContextBinding::corridorRef`

Redundant once norms key on `(measureId, contextId)`. It is currently unused, so no data is lost. A second way to say the same thing is how they diverge.

---

## 6. GUI

### 6.1 The fourth view

`CharacteristicLibrary.qml` has `property string _view: "library" // "library" | "roadmap" | "health"`. Add `"measures"`. Extend the existing chip row; do not restructure the panel.

### 6.2 Measure catalogue (`MeasureCatalogue.qml`)

Grouped by measure group. Filter chips: status (live / planned / no producer / not capturable), has-norm / no-norm, layer. Row: label · unit · status dot · *"used by N"* · a norm glyph, hollow when unset. **New measure** opens the existing `MeasurePicker` in mint mode.

Reuse `CharacteristicLibraryModel::usageOfMeasure()` and `usersOfMeasure()` — both already exist.

### 6.3 Measure detail (`MeasureDetail.qml`)

1. Header: label, unit, status, layer badge.
2. What it is: facets as a sentence (Composed) or metric key with a link into the Metric catalogue (Provided).
3. **Sign convention** — the `highMeans` line, editable.
4. **Norms by context** — one row per context in tree order, children indented. Each row shows the resolved corridor in the measure's own units, and either `inherited from <parent>` or `overridden` plus the delta from parent and the source (`seated · n = 42` / `heuristic · n = 0`). A weak provenance is called out **on this row only** — never on a finding, a chip or a chart band elsewhere in the app. Tapping opens the corridor editor. Inherited rows offer **Override for this context**; overridden rows offer **Revert to inherited**.
5. **Used by** — conditions on this measure, tails grouped by axis, each stating which edge it reads.
6. Availability: what capture this needs, and whether it is met.

### 6.4 Corridor editor (`CorridorEditor.qml`)

Works in the measure's own units on its own scale. The words `mu`, `sigma` and `z` never appear.

**Three routes, a segmented control:**

- **Set by hand** — two draggable handles bound the Good band. Centre becomes `mu`; each half-width becomes `sigmaLo` / `sigmaHi`. Dragging asymmetrically produces asymmetric tolerance with no statistics vocabulary. Handles need a 44pt touch target and a numeric readout beside them so precise values are reachable without pixel-accurate dragging.
- **Seat from swings** — filter shots, mark the ones considered well-positioned, fit. Sets `n` and `source = Seated`. Save must state that this sets the population norm for everyone on this norm set (§2).
- **Import** — adopt a row from another norm pack, then adjust.

**Live consequence.** A histogram of the drawn-from swings sits above the band. Every handle drag updates counts: *"31 Good · 8 Monitor · 3 Outside"*. A corridor grading almost everything Outside is visibly wrong to someone who has never heard of a standard deviation. This is the safety mechanism — it is not optional.

**Draw-from selector**: all swings / this athlete / this session. Recorded in provenance. It selects the sample, not the scope of the norm.

**Provenance block**: route, `n`, date, author, and the inheritance line (*"Overrides full swing (centre 45%) — this context sits 37% forward of its parent"*).

### 6.5 Editable bindings

`CharacteristicLibraryModel::detail()` already returns `bindings` read-only. Add to `CharacteristicEditorModel`:

```cpp
Q_PROPERTY(QVariantList contexts READ contexts NOTIFY draftChanged)   // tree order, with depth
Q_INVOKABLE void setBinding(const QString &contextId, bool applicable, bool material);
Q_INVOKABLE void clearBinding(const QString &contextId);
```

Render in `CharacteristicEditor.qml` as a checkbox per context, children indented under parents. Unticking a parent unticks children with an undo toast.

**Direction control.** Where a signal's direction is chosen, replace High/Low with the measure's own words from `highMeans`: *"further back, toward the trail foot"* / *"further forward, toward the lead foot"*. `attachMeasure(measureId, direction)` keeps its signature; only the label changes. See §9.1.

---

## 7. The navigable DAG

**This is a navigation surface, not an illustration.** Doxygen collaboration diagrams are the model because they are ego-centric, depth-bounded and *clickable* — you browse the codebase through them. The same must be true here: a user should be able to traverse the whole 50-condition library by clicking nodes, never seeing more than a dozen at once.

`CharacteristicDetail.qml` already lists causes and effects with click-through. The DAG view replaces that block with a graph carrying the same navigation semantics, not a picture beside it.

### 7.1 Behaviour

- Focus condition centred. Causes ranked above, effects below, its own signals to the side.
- **Tapping any node re-centres on it** and pushes a breadcrumb. Back pops it. This is the primary way to move around the library.
- Depth 1 by default; **Expand** goes to depth 2 and stops there.
- Long-press / right-click a node → open its detail page, add it as a cause, or remove the edge. Editing from the graph, not just reading.
- A node that is a measure (a signal's source) navigates into `MeasureDetail`, which is how a user gets from "why is this flagged" to "what is normal" in two taps.

### 7.2 Encoding

- Edge stroke weight: Weak / Moderate / Strong.
- Latent causes outlined rather than filled.
- `ConfirmedBy::Asserted` causes visually distinct and never rendered as concluded.
- Unavailable conditions greyed, with the missing measure named on tap.
- The focus node visually anchored so re-centring is legible.

### 7.3 Implementation

Layout is computed in **C++** (`dag_layout.h/.cpp`) and exposed to QML as positioned nodes and edge paths. No layout logic in QML.

- Rank nodes by signed distance from focus (causes negative, effects positive).
- Order within rank to minimise crossings; with ≤4 per rank a single-pass barycentre heuristic is sufficient.
- Emit `{id, kind, x, y, w, h}` per node and a cubic path per edge.
- Render with `Repeater` + `Shape` / `ShapePath`. No new dependency; works on mobile.
- Pan and pinch; depth 1 only on narrow screens.

### 7.4 The global picture is not a graph

For "how does this all hang together", use the existing `causeCoverage()` — causes ranked by how many characteristics they explain, split by `Measured` / `Screened` / `Asserted`. Four latent causes explain most of the pack; that is a bar list, and it doubles as "which screens should I run". Do not attempt to draw 50 nodes and 81 edges.

---

## 8. Health checks to add

Extend `CharacteristicLibraryModel::health()`:

- Axes with no norm — today, all of them
- Norms at `n = 0` **in the personal layer only** — a user's own hand-set guess is worth flagging; a shipped core norm is not. The 39 migrated wrist rows must not appear here, or the health list is 39 items of noise about content that was fine yesterday. Use the existing pack-layer resolution to scope this; no new field is needed.
- Norms grading an implausible share of the drawn corpus into one band
- Measures whose `howToRead` mentions club-dependence with no context override
- Norms whose unit no longer matches their measure
- Contexts with no norms anywhere beneath them
- Overridden items where core has since changed (diff + **Take theirs**)

---

## 9. Edge cases — do not skip these

### 9.1 Direction inversion (a live bug)

`ballPosition` defines 0% at the lead heel, so *forward* is a LOW value. But `sig_ballForward` has `direction: high` and `sig_ballBack` has `direction: low`. **They are inverted.** Once norms exist both will fire happily on the wrong swings with correct-sounding consequence text.

Fix the seed pack, then **audit all six axis pairs** (`ball_position`, `ball_body_distance`, `alignment`, `stance_width`, `lumbar_curve`, `thoracic_curve`, `address_hip_hinge`) for the same error. The `highMeans` control in §6.5 is what stops this recurring.

### 9.2 Others

- **Single-tail axes.** `s_posture` reads one edge of `lumbar_curve`. The norm is still two-sided. Health must not report the unread edge as a fault.
- **Ratio signals need norms.** Tempo ratio has a normal range. Only `Order` genuinely does not.
- **Δ-from-address.** Norm keys on the measure post-reducer (§3.1). A Δ norm compared against an absolute value is silent nonsense.
- **Unit drift.** `Norm::unit` must match the measure's unit at load; mismatch is a load error naming both, never a silent grade.
- **Handedness.** Measures use lead/trail, so norms are handedness-invariant. Assert this in a test — a norm that differs by handedness is a modelling error.
- **Not-capturable measures.** Two measures are `NotCapturable`. They must never accept a norm; the editor should refuse and explain.
- **Missing context.** A shot with no declared context resolves to `full_swing` and is marked `inferred`. Any finding whose grading depends on the context must demote confidence — reuse the existing low-confidence demotion in `assessment_rules.cpp`, do not invent a second mechanism.
- **No context-appropriate norm.** Report the deviation, name the norm actually used, and grade **neutral, not red**. A short-game shot graded red against a full-swing norm is misleading.
- **Grade never appears without its norm** being one tap away.

---

## 10. Tests

- `norm_test` — asymmetric z per side; grade boundaries; `NotMeasured` when no norm resolves.
- `context_tree_test` — upward resolution, missing intermediate nodes, cycle rejection, unknown parent rejection.
- `norm_pack_test` — round-trip, unit-mismatch rejection, unknown measure rejection, layer merge and copy-on-write, revert.
- `reference_bands_parity_test` — **byte-identical `PpRag` output** from `classifyDelta()` and `grade()` across every (DOF, position, archetype) and a swept delta range. Gates the deletion in §5.1.
- `wrist_render_parity_test` — the wrist assessment view's cell colours, pill states and finding severities are unchanged by the migration. Provenance must not alter any of them (§3.2). Run against a fixture swing set; a diff here is a regression, not an improvement.
- `manifest_migration_test` — the converted `inlineCorridors` row produces the same band as before.
- `characteristic_engine_test` — extend: with norms present, `OutsideCorridor` fires on the correct tail; both tails of an axis resolve one norm; absent norm still yields `Unavailable`, never a pass.
- `axis_direction_test` — for every axis pair, the two tails carry opposite directions, and each condition's declared direction agrees with a fixture table of expected semantics. This is what makes §9.1 non-recurring.
- `dag_layout_test` — no overlapping nodes, deterministic output for a given focus, depth bound respected, isolated node handled.
- `core_pack_test` — extend: assert every `OutsideCorridor` signal resolves a norm in at least one context, so "the pack is dark" cannot silently return.

---

## 11. Build order

1. `norm.h`, `context_tree`, `norm_pack`, `INormProvider` + resource provider. Tests first.
2. `reference_bands_parity_test` and the DOF converter. Green before anything is deleted.
3. Wire `INormProvider` into the engine's `IMeasureSource` path so `hasCorridor` can finally be true. **The seed pack lights up here — this is the milestone.**
4. Fix §9.1 and audit the six axis pairs. Add `axis_direction_test`.
5. `NormModel`, `MeasureCatalogue`, `MeasureDetail` (read-only). Shippable — the norms are visible and inherited.
6. `CorridorEditor`, all three routes.
7. Editable bindings + the `highMeans` direction control.
8. `dag_layout` + `DagView`, replacing the causes/effects block.
9. §5.2 deletions and `chart_metrics.cpp` rewiring.
10. Health checks (§8).

---

## 12. Open items — raise, do not guess

- Grade labels. Proposed: **Good · Solid · Monitor · Outside**, plus **Not measured**. Confirm before writing `qsTr()` strings.
- Whether `norms.json` and `contexts.json` also live as reviewable files at the repo root so community contributions arrive as pull requests. Recommendation: yes, with the Qt resource generated from them at build time.
- Whether the norm-set selector sits in the Packs view or in Diagnostics settings.
