# Brief — Settings › Diagnostic Model

## What this is
A **new settings panel** called **Diagnostic Model**: one screen for navigating and editing the whole
diagnostics content set. It sits beside the existing **Diagnostics** panel and does not touch it.

The long-term intent is that Diagnostic Model replaces Diagnostics once its UX is better in every
respect. Until then both ship, the old one is the safe fallback, and nothing in it is modified,
renamed, or refactored by this work. Two panels temporarily overlapping is the point, not an accident.

## Why
The content is **one connected graph**, and the current panel splits it across eight sibling views
(Characteristics · Measures & norms · Metrics · Causes & health · Drills · Glossary · References ·
Roadmap). Following a real chain — a metric, the measure that reads it, the corridor that judges it,
the characteristic that fires, its causes, the paper behind the link — means leaving the page at every
step. `CharacteristicLibrary.qml` says so itself, twice:

> "a metric, a measure and a corridor are one chain, and following it meant leaving the page"

Editing is worse: it is draft-and-modal-shaped, so changing one strength value costs a page
navigation, an Edit click, a form, and a Save.

**The goal of this panel is that every element of the model is editable in the fewest possible
clicks.** That requirement outranks visual polish, and it outranks consistency with the old panel.
Where this brief and the old panel disagree, this brief wins.

## The design
See `DAG Navigator.dc.html` — **design references created in HTML**, showing intended look, density
and information architecture. They are not production code to copy; rebuild them in QML with the
existing `Theme` singleton and `Pp*` components. Options, newest first:

| Option | What it shows | Phase |
|---|---|---|
| `#5a` | Editing in the inspector — dirty gutter, source column, validation strip, Save/Revert | 2 |
| `#5b` | The DAG as the middle pane — edge editing by drag, validator refusing a cycle | 2 |
| `#4a` | **The screen to build first** — type rail → table → relationship inspector, with a trail | 1 |
| `#4b` | One flat table over all 676 objects, one search across every type | 1 |
| `#3a` `#3b` | Superseded. These retro-fitted a table onto the OLD panel; that is no longer the plan | — |
| `#1a`–`#2b` | Early exploration in a non-PinPoint palette. Ignore every colour and font in them | — |

`4a` and `4b` are not rivals: **4b is what 4a's search results should look like.** Build 4a's shell,
and render 4b's flat cross-type list as the result state when the search field is non-empty.

## Integration points — exact
From `src/Gui/settings/ScreenSettings.qml` as it stands (read it; do not trust this list alone):

1. **Sidenav model** (the `Repeater` at ~line 211): insert
   `{ navIdx: 10, icon: "◈", label: qsTr("Diagnostic Model"), sectionHead: "", hasBadge: false }`
   directly after Diagnostics (`navIdx: 9`, `sectionHead: "Reference"`), so it joins the same
   **Reference** section. Pick an icon that is not already in the list.
2. **System renumbers 10 → 11.** It is an action row (`action: "system"`) whose navIdx must not
   collide with a panel index. This is the same renumber the Diagnostics panel itself did when it
   landed — see `docs/design/swing_characteristics_impl_plan.md`.
3. **StackLayout** (~line 424): append `DiagnosticModel { id: diagnosticModelPanel; … }` as child
   **10**, after `CharacteristicLibrary` (child 9).
4. **`navigateToResult()`'s literal `panels` array** (~line 59) currently covers 0–9. Append the new
   panel, or settings search silently no-ops on it.
5. **`SettingsIndex.qml`** — add searchable entries at `panelIndex: 10`. Without them the panel is
   invisible to ⌘F.
6. **Deep links.** `showMetricDetail`, `showCharacteristicDetail`, `showMeasureDetail` all hardcode
   `activeNavIndex = 9`. **Leave them pointing at Diagnostics** for now — routing them here is part
   of the eventual replacement, not of this change.
7. **CMakeLists.txt** — register every new QML file in the same block as the other
   `src/Gui/characteristics/*.qml` entries (~line 416).

Put the new files in `src/Gui/diagnosticmodel/` so the old panel's directory stays untouched.

## Phase 1 — navigate everything (build this first)

One shell, three panes, as in `#4a`:

**Type rail** (`Theme.sp(214)`) — every content type with its live count. From the real pack:
Characteristics 108 · Causes 32 · Measures 106 · Signals 112 · Causal links 276 · Screens 14 ·
Drills 12 · References 16 · Health (runtime). Below it, facets for the selected type. Total across all
types is **676 objects** — derive it by summing, never hardcode it.

**Table** — the primary surface. Columns change per type, the shell does not. Row height
`Theme.sp(32)` **inclusive of its 1px separator**. Every column sortable; sorting and filtering
happen in C++, never in QML.

| Type | Columns |
|---|---|
| Characteristics | Name · Group · Meas · Caus · Expl · Reach · Evidence |
| Causes | Name · Group · Explains · Reach · Screen · Evidence |
| Measures | Name · Unit · Anchor · Status · Read by |
| Signals | Id · Test · Direction · Measures · Used by |
| Causal links | From · To · Relation · Strength · Evidence |
| Screens | Id · Settles (conditions) |
| Drills | Id · Answers (conditions) |
| References | Citation · Supports · Tier |
| Health | Severity · Code · Subject · Message |

Default sorts that answer the question the author actually arrived with: Measures by **status, then
least-read** (surfacing the 42 measures nothing reads); Characteristics by group → axis pair → label;
References by how much of the library they hold up.

**Inspector** — a **relationship hub**, not a property sheet. Its job is that every related object is
one click away: a measure shows its metric, its signals, its **blast radius** (the characteristics
that would change), and its corridor state. A reference shows the claims resting on it. A trail
breadcrumb across the top shows the chain walked to get here, and its terminal item is always the
current selection.

**Search** applies across every type at once (`#4b`): one field, results as a flat list with a Type
column, so "hip" returns characteristics, measures, signals, screens and drills together. Derive the
match count and shown count from the arrays that feed the rows.

## The editing requirement — this is the part that matters
Phase 2, but design phase 1 so none of it needs unpicking.

### Rules
1. **No modals. Ever.** The list stays on screen through every edit.
2. **No edit mode.** `#5a` draws an Editing / Read-only toggle — **drop it.** A mode is a click before
   every edit and a state to get wrong. Rows are always editable; the pack stack already protects
   shipped content by copy-on-write.
3. **No Edit button and no begin-edit ceremony.** Selecting a row IS opening its editor. The inspector
   is always live.
4. **Click a cell, type, done.** Every table cell whose value is scalar edits in place: text inline,
   enums as a click-through segmented control or a one-click popup list, numbers as a spin field.
   `Enter` commits and stays, `Tab` commits and moves right, `Esc` cancels.
5. **Type-ahead for every link.** "+ add cause" must be: click, type three characters, `Enter`. Never
   a picker dialog, never a scroll through 140 rows. The candidate list is pre-filtered to **legal
   targets** — acyclicity, axis, and corroborates-shadowing rules applied in C++ — so an illegal edit
   cannot be constructed rather than being rejected after the fact.
6. **Duplicate beats blank.** `⌘D` on a row creates a new object pre-filled from it. Authoring a new
   characteristic from an empty form is the slow path; make it the second one.
7. **Multi-select and bulk-set.** Select N rows, set group / reach / tier / strength once. Re-tiering
   twenty links one at a time is the single biggest time sink in the current panel.
8. **Save is once, not per field.** Field commits accumulate in the draft
   (`CharacteristicEditorModel`); one **Save** (`⌘S`) writes to the user pack. `Revert` discards.
   Dirty rows carry a 2px `colorAccent` left gutter and an "edited" tag; the status bar shows the
   unsaved count.
9. **Every destructive action is reversible in place.** Removing a link or a measure shows an inline
   undo affordance. If the editor model has no undo stack, say so and propose one rather than
   shipping unrecoverable deletes.
10. **Keyboard-first.** `↑`/`↓` rows · `Tab`/`⇧Tab` fields · `Enter` commit · `Esc` cancel ·
    `⌘S` save · `⌘D` duplicate · `⌘F` search · `G` graph · `Delete` remove selected link.
    A power author should be able to re-tier a dozen edges without touching the mouse.

### Click budget — hold the implementation to this
Measured from "the row is on screen", not from app launch:

| Task | Budget |
|---|---|
| Change an edge's strength | **1 click** |
| Change group / reach / evidence tier | **1 click + 1 click** (open, pick) |
| Edit a label or consequence | **1 click**, then type |
| Add a cause | **1 click + 3 chars + Enter** |
| Remove a cause or measure | **1 click**, undoable inline |
| Add a measure to a characteristic | **1 click + type-ahead + Enter** |
| Create a new characteristic like an existing one | **⌘D**, then edit in place |
| Re-tier 12 links | **select 12 + 1 click** |
| Save everything | **⌘S** |

If any of these needs more, the design is wrong — raise it rather than adding a click.

### Provenance and safety
- The pack stack is **copy-on-write**: a **Source** column (shipped / yours) tells an author whose
  content they are changing, and per-field "yours vs shipped" markers plus a **reset** must exist
  wherever an override can be made. The old `MeasureDetail` already does this for corridors — match it.
- Shared measures show their blast radius **before** the edit, via `usersOfMeasure()` — the count and
  the actual list, because a count alone does not say what is about to change.
- A **validation strip** under the toolbar shows what is wrong with the current draft, and clicking it
  filters the table to the offending rows. Codes and their "what to DO" already live in
  `diagnostics_health.h`; use them rather than inventing messages.
- `refresh()` and the `_revision` bump pattern in `CharacteristicLibrary.qml` exist because the
  façade caches its provider: **after any write, re-take the provider or the edit is on disk and
  invisible until relaunch.** Copy that shape exactly.

## Phase 2 — the DAG in the same shell
Per `#5b`: the **Graph** segment swaps the middle pane, keeping the type rail and the inspector.
Selection is shared with the table, so switching views keeps your place.

- **All coordinates come from `library.dag(conditionId, options)`** — `dag_layout.h` returns
  `{ nodes[], edges[], width, height, focusX, focusY, truncated }` and `options` carries the theme's
  own metrics (`nodeH`, `gapX`, `gapY`, `laneGap`, `padX`, `charW`, `minW`, `maxW`, `depth`,
  `maxPerRank`, `includeMeasures`). QML positions nothing. Render with `QGraphicsView`-style
  `Shape`/`ShapePath` for edges and `Repeater`ed `Rectangle`s for nodes.
- **One node, one box.** Assign each node to its nearest rank only and dedupe across ranks — a
  condition that is both a direct cause and a cause-of-a-cause must not render twice. That bug
  appeared in the mockup; check `dag_layout.cpp` guarantees it.
- Never render the whole graph unfiltered: focus + radius (1/2/3), hide weak, hide proposed,
  optionally include measures.
- **Editing in the graph**: drag from a node's trailing edge onto another to add a cause; illegal
  targets refuse *during* the drag with the reason stated ("X already leads to Y, so this would create
  a cycle"), not on release. Click an edge to edit it in the inspector — relation
  (causes / corroborates / excludes), strength, evidence tier, citation. `Delete` removes it.
- **The selected edge must be identifiable on the canvas** — draw it heavier and leave the rest muted.

## Data contracts
Reuse; do not reimplement. `CharacteristicLibraryModel` (`src/Gui/characteristics/`) already gives:
`query(filters)`, `detail(id)`, `dag(id, options)`, `roadmap()`, `captureGaps()`,
`causeCoverage()`, `health()`, `screens()`, `drills()`, `references()`, `glossary(search)`,
`usageOfMeasure(id)`, `usersOfMeasure(id)`. `NormModel` gives `measureDetail(id)`;
`CharacteristicEditorModel` and `NormEditorModel` hold the drafts.

What phase 1 needs added, all in **C++**:
- `query` gains `sort` / `descending`, and filters for `tier` and `resolvability` (both returned
  per row today but not filterable).
- A cross-type search returning `{ type, id, label, identifier, linked, links, status }` rows over
  every registry at once — that is 4b, and it cannot be assembled in QML.
- A signals list façade; signals are reachable today only through a condition's `detectedBy`.

**One trap found in the content**, worth fixing on the C++ side rather than per-view: nine measures
carry `label: ""` in `core.json` — all `kind: "composed"` (`m_thoracicCurve`, `m_lumbarCurve`,
`m_shoulderPlane`, `m_ballBodyGap`, `m_thoraxDrift`, `m_leadKneeFlex`, `m_trailElbowRise`,
`m_leadArmToTorso`, + 1). Their name has to be **derived** from `series` + `reducer`
(`m_thoracicCurve` → "Thoracic segment angle to ground, at P1"), with a fallback to the id so no row
can render nameless. Eight of the nine are `planned` or `noProducer` — exactly the rows an author is
hunting for, so they must not be blank. Do it once in the façade.

Also worth knowing: **111 of 112 signals reference exactly one measure**, so signals-per-measure and
conditions-per-measure are the same number. Do not ship both as columns.

## Style
`docs/design/pinpoint_qml_design_system.md` is authoritative. Everything through `Theme` tokens —
never a hardcoded colour, font family, or pixel size. Active aesthetic is Studio, but the table must
survive `radius: 0` (Vector) and a serif `fontBody` (Instrument) without changing column widths.
`Font.Normal` or `Font.Light` only — emphasis is colour and case, never weight. No gradients, no
shadows, no `border.width` above 1. `Theme.sidenavWidth` for the sidenav; `Theme.sp()` on every
dimension. GPL v2 header on every new file.

**Pane budget.** The three panes plus the 84px rail (`Theme.railWidth * 1.6`) and the 275px sidenav
add up fast, and the name column is what gets starved. Give it `Layout.minimumWidth: Theme.sp(240)`;
collapse the facet rail to a filter popover below ~1500px panel width; drop the inspector below
~1150px and fall back to a full-width table. In any cell whose last child is a badge, the label
`Text` must be the `Layout.fillWidth` child with `elide: Text.ElideRight`, and the cell needs
trailing padding — otherwise a `nowrap` sibling collapses the label to zero width.

## Verification
Per `docs/developer/diagnostics_developer_guide.md`: any new `query()` filter, sort key, or façade
method gets a case in the `characteristic_library_model` tests — "every rule they hide is asserted
here or nowhere". Then `core_pack_test`, `diagnostics_catalogue_integrity_test`, `norm_pack_test`,
`diagnostics_health_test`.

## Sequence
1. Panel shell registered and reachable — sidenav entry, System renumber, StackLayout child,
   `navigateToResult` array, `SettingsIndex` entries. Empty content. Land this on its own.
2. Type rail + Characteristics table, read-only, over existing `query()`.
3. Remaining types; the C++ additions (sort, filters, signals façade, derived measure labels).
4. Relationship inspector + trail.
5. Cross-type search (4b as the result state).
6. **Inline editing**, against the click budget above. Dirty state, Source column, validation strip.
7. Graph pane, read-only.
8. Edge editing in the graph.

Keep it runnable at every step. Do not start step 6 before 1–5 are usable — the editing ergonomics
are the hard part and they need a working surface to be judged on.

## Read first
`src/Gui/settings/ScreenSettings.qml` · `src/Gui/characteristics/characteristic_library_model.h` ·
`CharacteristicLibrary.qml` · `CharacteristicRow.qml` · `src/Diagnostics/dag_layout.h` ·
`src/Diagnostics/diagnostics_health.h` · `docs/design/pinpoint_qml_design_system.md` ·
`docs/developer/diagnostics_developer_guide.md` · `src/Gui/shell/PpRail.qml`.

## Files here
- `DAG Navigator.dc.html` — all design options, anchored `#1a` … `#5b`. Open directly in a browser.
- `model-data.js` · `core-model.json` — the `core v1.0.0` content the mockups render. The app reads
  its own `src/Resources/diagnostics/core.json`; these are for reference only.
