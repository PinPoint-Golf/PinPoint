# Addendum 02 — chrome revision, and the three missing panes

Supersedes nothing in `BRIEF.md`. That brief described a panel that did not exist; this one revises the
panel that now does. Where the two disagree about the toolbar, **this wins** — the brief never specified
one, so the built bar accreted.

Design reference: `DAG Navigator.dc.html`, options `#6b` (chrome) and `#6d` (panes). `#6a` and `#6c` are
the rejected alternatives and are kept only so the reasoning stays legible; do not build from them.

Two parts, independent of each other. Either can land first. Neither blocks the graph-editing work that
follows.

Every line reference below was written against `012f82d` and is accurate there. Part A edits
`DiagnosticModel.qml` heavily and Part B adds to `model_browser.cpp`, so anything below the first change
shifts — search by the quoted content rather than by number once work starts.

---

## Part A — chrome: four bands become two

### What is wrong with what shipped

`DiagnosticModel.qml` currently stacks **four horizontal bands** before a single row of content:

1. a page header — `REFERENCE` eyebrow, `PpDisplayText`, `browser.packLabel`
2. the toolbar — search · trail · unsaved · Table/Graph · New · Tools · Edits · Undo · Save
3. the validation strip
4. (at the bottom) a status bar repeating the unsaved count and hiding `revert`

Nine controls sit on one line at equal visual weight answering nine unrelated questions: *find*, *where
am I*, *what am I looking at*, *make content*, *history*, *persistence*, *configuration*, *artefacts*.
Only one of them (`New`) knows what type is selected. `Redo` exists but has no button. `Revert` is a
12px word in the status bar, nowhere near the `Save` it undoes. The unsaved count is drawn twice.

### The organising rule

> **The top bar belongs to the panel and never changes. Anything that depends on what you are looking
> at belongs to the pane it acts on.**

A bar that never moves can be learned. A bar whose contents shuffle per type cannot, which is why the
context controls move *down* into the middle pane rather than being made conditional in place.

### A1 · The global bar

One `RowLayout`, `Theme.sp(42)` tall, replacing **both** the page header and the current toolbar. Delete
the header row entirely — `REFERENCE` is the sidenav section the user is standing in, and repeating it is
chrome describing chrome.

Left to right:

| Item | Notes |
|---|---|
| `PpDisplayText` "Diagnostic Model" | as today, but on the toolbar line |
| `browser.packLabel` | `fontData`, `fontSzMicro`, `colorText3`. Beside the title, not on its own line |
| Search field | `Layout.fillWidth: true`, `Layout.maximumWidth: Theme.sp(420)`, `Layout.minimumWidth: Theme.sp(180)`. Trailing `⌘F` hint in `colorText3` |
| *spacer* | `Layout.fillWidth: true` |
| **Grading `<policy>` ▾** | new; see A4 |
| `⋯` | new; see A4 |
| separator | `Theme.colorBorderMid`, `Theme.sp(18)` tall |
| **Validation chip** | see A3 |
| **`↶` `↷`** | see A5 |
| **`n unsaved ▾`** | see A5 |
| `Save` | `primary: true`, `enabled: browser.dirty`, unchanged |

Every child gets `Layout.fillWidth: false` and an `implicitWidth` it will not shrink below. The existing
comment on `PpSegmentedControl` — *"sets an implicitHeight and no implicitWidth, so a RowLayout under
pressure will happily shrink it to nothing"* — is the general case, not a quirk of that one control. It
cost us a clipped control in the `#6a` drawing, which is precisely why `#6a` is not the recommendation.

Below `Theme.sp(1100)` of panel width, the title and pack label drop out before anything else does. The
commit cluster is the last thing that may ever shrink and it may never disappear.

### A2 · The context bar, and the trail as its heading

A second `RowLayout`, `Theme.sp(38)` tall, **directly below the global bar and spanning the panel**, on
the same `Theme.sp(24)` margins. It replaces the current middle-pane header row (type label · count ·
hint · selected count).

> **Revised after building it.** This was drawn *inside the middle pane*, starting where the table
> starts, on the argument that a bar belonging to the list should look like it belongs to the list. That
> argument was right about meaning and wrong about arithmetic. The middle pane is what is left after a
> 214px rail and a 528px inspector, so eight controls and an eliding breadcrumb were fighting over about
> a third of the window — **cramped at full screen**, not merely under pressure. A row that cannot hold
> its contents does not communicate what it belongs to either, so the meaning argument loses to the one
> that can be measured. Two bands, both full width: one that never changes, one that always does.

Left: **the trail is the heading.** `ModelTrail` moves out of the global bar and becomes this bar's
leading item, with `Layout.fillWidth: true` and elide. Its terminal item is the current selection, drawn
at `fontSzHeading` in `colorText` while the earlier steps are `fontSzBody2` in `colorText3` — so the
breadcrumb and the pane title are one thing, not two. With no trail yet, it degrades to the plain type
label, which is what it renders today. The count and hint follow it in `fontData`/`colorText3`.

**Selecting a table row RESETS the trail.** The trail is the chain you walked, and the test for whether
something belongs on it is whether a *relationship was followed*. Following a link in the inspector or a
node in the graph extends it; picking a row out of the table is the decision to start a different chain,
so it begins one item long. Keeping the old walk in front of a hand-picked row would make the breadcrumb
claim a route nobody took, and offer to step "back" to something the author had already left.

The same reasoning puts four more arrivals on the reset side: a dashboard deep link (`showMetric` and
friends), a health finding's subject, a newly created object, and a duplicate. None of them is a step
taken from where you were standing. `select()` walks, `selectFresh()` arrives, and every call site is one
or the other.

This is the row that pays for itself: it removes a band while putting the breadcrumb directly above the
list it walked.

Facet chips and `n selected` sit between the count and the right-hand controls, which is where the slack
is now that the bar has the panel's full width.

Right, in this order:

- **`n selected`** — only when `_selection.length > 1`, `colorAccent`, as today
- **Table / Graph** — moved down from the global bar. Hidden for `health` (a finding has no
  neighbourhood — `model_browser.cpp:4660` already returns `{}`), otherwise always shown
- **`+ New <type>`** — the button **names the type**: `+ New drill`, `+ New screen`, `+ New
  characteristic`. Same behaviour as today, including the `ModelMint` route for measures. Hidden where
  creation is not a thing: `links`, `signals`, `corridors`, `health`, and while searching
- **`⧉`** duplicate — visible only with a selection; tooltip `Duplicate  ⌘D`
- **`🗑`** — visible only with a selection. **Use the app's glyph and the app's wording**: the icon is
  `🗑` as in `PpShotActionBar.qml:221`, and the menu/tooltip text is **"Move to trash"**, as in
  `PpSessionDrawer.qml:369` and the comment above it. That wording is accurate here for the same reason
  it is accurate there — the undo stack makes it recoverable, so `Delete` would overstate it

Facet chips for the active filters belong on this bar too, after the trail, when the facet rail is
collapsed (`_showFacets === false`) — otherwise the collapsed rail hides the fact that a filter is on.

**The inspector edits every field, and Copy · Delete live there.** Revised against the built article,
like A2's placement was.

The pane was drawn as a *relationship hub, not a property sheet* — every prose field read-only, no
control for nine fields `setField()` already accepted, and a causal link's frequency changeable in the
table but not in the pane describing it. That reading was wrong about what the pane is for: it exists
precisely to see and edit ALL of an item, and the table is the fast path for the few fields that fit in
a column. Building both from one list is what stops them disagreeing.

`fieldsOf(type, id)` is now that one list — every writable field of every editable type, in the same
`{ field, kind, value, options }` grammar a table cell uses, rendered as a `Fields` section at the top
of the pane. `kind` gains **prose** for the paragraphs that never belonged in a cell. Writes go through
the same `setField()`, so a field cannot behave differently depending on where it was typed.

That closes the dead end too: a characteristic's tier could never be raised, because the tiers that
demand a citation had no way to set one. Both are on the pane now.

**One field shape, one gesture.** An enum is the same bordered box a text field gets, opened by
clicking it (`ModelEnumField.qml`). It was a `PpSegmentedControl` first, which is right for two or
three choices on a wide bar and wrong in a 480px pane: Evidence has six tiers, and six segments in
that width rendered as overlapping, unreadable pills. A reader should not have to learn a different
gesture per field type, and a control whose shape depends on the number of options renders differently
on every object. One click opens, one click picks — the same budget the table's enum cells keep.

**And no section may repeat a field.** The pane first shipped showing the editable fields at the top
AND the same values again below as read-only prose, so the obvious thing to click was the copy that
could not be typed into. Sixteen such sections came out — a characteristic's *What it costs · Injury
note · Also called*, a link's *How often · Evidence*, a screen's four, a drill's four, and the rest.
What stays is what is not a field: relationships, and derived read-outs like the corridor's *What good
looks like*, which computes the ideal and believed ranges rather than restating mu and sigma. A test
asserts the invariant, because it is not visible in a diff.

**`Copy` and `Delete` move to the pane's foot** — words, not glyphs, `Delete` in `colorError`. They act
on the selected OBJECT, not on the list, so they belong beside the thing they act on rather than in the
chrome next to controls that only change what you are looking at. `+ New` stays on the context bar,
because it does act on the list. `removeObject()` now unpacks a link's composed row id itself, so the
caller no longer has to know links are removed through a different function — which the pane's Delete,
acting on whatever is selected, had no way to know.

**One control type across both bars.** `+ New`, `⧉` and `🗑` are three actions of equal standing over
the same list, and drawing the first as a filled 34px `PpButton` beside two quiet 28px glyphs made
them read as three different kinds of thing. All three are `ModelBarButton`; **`Save` is the only
`PpButton` in the panel**, because it is the only action that leaves the draft. Weight among the three
is carried by TONE — accent on the one that makes something, plain on the ones that copy and remove —
which is a difference that means something, where a difference of shape did not.

**A picker opens where the click was.** Every type-ahead used to compute
`parent.width / 2 - popup.width / 2`, but `parent` there was the inspector while `x` is in the
*panel's* coordinates — so the sum put the popup hard against the panel's left edge however far right
the affordance that opened it sat. Full screen, that is the whole display to cross to answer a
question you asked on the other side. `ModelInspector` now reports `actionOrigin` (the bottom-left of
the affordance that fired, in its own coordinates) and the panel maps and clamps it in one place,
`_openPickerNear()`. The mint dialog hangs off the `+ New measure` button the same way.

### A3 · The validation strip becomes a chip

Delete the strip `Rectangle`. It becomes a chip in the global bar:

- hidden entirely when `validationErrorCount + validationWarningCount === 0` — a clean draft costs zero
  pixels, where today it costs a conditional band
- `⚠ 2 errors` in `colorError` on `colorErrorLight`, or `⚠ 5 warnings` in `colorWarn` on `colorWarnLight`;
  both present → `⚠ 2 errors · 5 warnings`, error colours win
- **same click behaviour as today**: it switches the table to `health`. Do not lose this — the strip's
  value was always that it was a way in, not a description

### A4 · `Tools` splits in two

`Tools` is a grab-bag: a grading policy, a read-only norm-set inventory, two saved views and two exports.
Split it along the line that already exists in `ModelTools.qml`'s own header comment — *"the things that
are not content"* is two different things.

**Grade policy is promoted to a live readout.** `Grading Studio ▾` sits in the global bar as a value you
can see and change, because it is the state every corridor on screen is drawn under — the answer to "why
is this corridor red" should not be behind a button called Tools. Clicking it opens the policy list from
`browser.gradePolicies()` exactly as today, and it still writes to the one global `AppSettings` and still
stays off the undo stack.

**Everything else goes in `⋯`.** The user's words: *a junk drawer is right for this*. Contents, in order:

```
Roadmap              what is not built yet
Glossary             what a term means
───────────────────
Export roadmap       markdown
Export references    CSL-JSON
───────────────────
Norm sets            core v1.0.0 · 412  ·  yours · 38     (inventory, not actions)
───────────────────
Reset to the standard model…       12 changed · 5 yours   (only when local content exists — A7)
```

`ModelTools.qml` keeps its `Action` and `Heading` sub-components and loses the policy `Repeater` to the
new readout. The `⋯` menu is the house pattern already — see the per-row `...` menu in
`PpSessionDrawer.qml:252`.

### A5 · One commit cluster

Three items, always adjacent, always in the same place, top right. This is the only part of the chrome an
author touches every few seconds and today it is spread across three bands.

- **`↶` `↷`** — `enabled: browser.canUndo` / `browser.canRedo`. **Redo currently has no button at all**,
  only `StandardKey.Redo`. Both get `browser.undoLabel` / `redoLabel` as their tooltip, so the human label
  the commands already carry is actually readable somewhere. (The properties are `…Label`, not `…Text` —
  see `model_browser.h:104-105`.)
- **`n unsaved ▾`** — replaces the `Edits` button and the `_editsOpen` boolean. Visible only when
  `browser.dirty`. Clicking opens `ModelEdits` as a popover anchored under it, and the popover gains
  **`Revert all`** at its foot — next to the history it would discard, which is where it belongs.
  `revert` leaves the status bar. `ModelEdits` already carries the session-scope sentence the popover
  needs (see *Settled* below); it moves with the component rather than being written again.
- **`Save`** — unchanged in behaviour (pack-wide), but it now carries the one-time deviation
  confirmation described under *Settled*.

The status bar keeps only what is genuinely status: `n of m shown`, the sort, and the keyboard hints. The
duplicate unsaved counter goes.

### A6 · The way back — reset to the standard model

The first-save warning under *Settled* tells an author their diagnostics now deviate from the standard.
A warning with no way back is just a warning, so the `⋯` drawer gets the exit: **`Reset to the standard
model…`**, in its own section at the foot, **visible only when there is local content on disk** — the
entry does not exist on an untouched install, which is also how the drawer stays a drawer rather than a
danger zone.

**It is not `Revert all`, and the two must never be confused.** `Revert all` (A5) lives in the unsaved
popover, discards the *session's* unsaved edits, and takes you back to the file. This takes you back to
what shipped, and what it discards is *already saved*. Different scope, different place, different word,
and the ellipsis says a prompt is coming.

The entry carries the two counts, because they are two different losses and `sourceOf()`
(`model_browser.h:435`) already distinguishes them: objects returning **`both`** are shipped items you
changed, objects returning **`yours`** are ones you created. Read as `12 changed · 5 yours`.

They are counted over the **draft**, not the file. An edit made a minute ago is as much a loss as one
saved last week, and a reset discards both — so a count that only saw the file would understate what the
prompt is about to take.

**Scope: it resets everything local.** Both files — `user.json` and the user norm pack. Authored objects
go too, and that is deliberate: a new characteristic is screened and graded like any other, so an install
carrying one is not running the standard model either. "Back to standard" that left content behind would
be a lie in the one place the app has to be exact.

**And it stays recoverable, because nothing in this panel is not** (the rule is stated at
`model_browser.cpp:2628`, where `revert()` pushes itself onto the stack rather than being the one
unrecoverable action). Two layers:

- reset is a command on the existing seam, so `⌘Z` puts it back for the rest of the session, exactly as
  `revert()` does
- before writing, every user layer is **copied** aside — `diagnostics/user.json` →
  `diagnostics/user-<timestamp>.backup.json`, same for the norms, the screens and the drills. Copied,
  not renamed: the write that follows renames a temporary over the original, and an original that had
  already been moved away would leave a window with no file at all. The copies happen FIRST and a
  failure to write one aborts the whole reset, because a reset that half-succeeded would have destroyed
  the thing the backup existed to protect

The reset **writes through** rather than only clearing the draft. Leaving it unsaved would put the panel
in a state reading "n unsaved" with the files it had just backed up still on disk, which is not what
"reset" means to anybody who clicked it. `⌘Z` then restores the draft, and saving again is what puts it
back on disk — the ordinary two-step this panel uses everywhere.

The confirmation uses the same *structure* as the first-save prompt — `Main.qml:178-280`: modal centred
`Popup`, dimmed, Esc cancels, display-font title, `fontSzBody2` / `colorText2` body, right-aligned
primary then neutral Cancel.

**But not the same colour.** `colorAttention` is defined as a call-to-action frame — *"draws the eye to a
row/control that needs the user to act"* (`Theme.qml:286-289`) — and the close prompt uses it because it
interrupts something live, not because it destroys anything. This destroys saved work, so it takes the
error family: `colorError` for the title, the border and the `Reset` primary's text and outline,
`colorErrorLight` for that button's hover fill. That is the app's existing destructive styling, not a new
token — `DagView.qml:1131` already draws a destructive menu row in `colorError`, with the comment
*"Removing a link is a write to the user's pack and is styled as one."* The `⋯` entry itself gets the same
treatment for the same reason.

So the two prompts read as two different kinds of event, which is the point: the first-save warning stays
amber, because nothing is lost when you accept it.

```
Reset to the standard model

This removes 12 changes to shipped items and 5 items you created, and
puts this install back on the diagnostic model that ships with PinPoint
Studio. Your current model is copied to a dated backup file first, and
⌘Z undoes this until you close the app.

                                       [ Reset ]   [ Cancel ]
```

The prompt says "a dated backup file" rather than naming it: there are up to four (pack, norms, screens,
drills) and the set depends on which registries this install has touched, so a single name in the prompt
would be wrong as often as right. `resetToStandard()` returns the paths it wrote in `backups`, and the
toast that follows names the first of them.

C++: `resetToStandard()` on `ModelBrowser`, pushing a command like every other write, plus whatever
`sourceOf()` roll-up the counts need. It also **clears `ui/diagnosticsBaseModelWarningAck`** — an install
back on the standard model has not been warned about leaving it, so the next deviating save warns again.

### A7 · Screen estate — the settings sidenav folds

The panel is losing 275px of settings sidenav plus 84px of rail to chrome it is not using, and the name
column is what starves. Give the sidenav a **46px collapsed state**, in `ScreenSettings.qml`:

- an explicit `‹‹` / `››` control at the sidenav's own foot — it is the sidenav's state, so it is the
  sidenav's control, not the panel's
- `⌘\` toggles it from anywhere in Settings
- collapsed shows each entry's icon only, at its full row height, with the active entry keeping its
  accent gutter; hovering the strip peeks the labels in a transient overlay
- **the state persists** in `AppSettings` and applies to all Settings panels, not just this one. A fold
  that resets on every visit is a fold you re-do forever

Deliberately **not** automatic. `#6a` proposed a width-driven auto-fold and it is the wrong trade: a
sidenav that vanishes on its own teaches an author that the app moves things when they are not looking.

**The inspector folds too, by the same pattern.** Same reasoning, same chrome, one modifier apart:
`››` at the pane's own leading edge to fold it away, `‹‹` in the 26px strip left behind to bring it
back, `⌘⇧\` from anywhere in the panel, and the state persisted in `AppSettings` under its own key —
`ui/diagnosticsInspectorCollapsed`, separate from the sidenav's, because they are separate decisions.
An author reading a wide table wants the inspector gone and the sidenav where it was, or the reverse.

Two details that are the whole difference between a fold and a trap:

- the strip is **not** zero width. Folding to nothing is tidier and strands the author: the only way
  back would be a shortcut nobody has been told about
- the control is anchored to the **pane**, not placed in the pane's header. The header only exists
  when something is selected, and a pane you can only fold while it has content is one that traps you
  on an empty one

It is distinct from `_showInspector`, which is the panel running out of room. That is the app's
decision and reverses itself when the window grows; this is the author's and does not. When the panel
drops the pane for width it takes the strip with it — a control offering to restore something the
layout will not give back would be a lie.

Once it can fold, re-derive `_showFacets` and `_showInspector` from the panel's **actual** width rather
than the current `Theme.sp(1500 - 275)` / `Theme.sp(1326 - 275)` literals, which bake in a sidenav width
that is now variable. Order of concession under pressure: fold the sidenav → collapse the facet rail →
drop the inspector. Today the first step does not exist, so a narrow window loses the inspector while
275px of sidenav sits there.

---

## Part B — the three missing panes

Screens, drills and references have rows (`model_browser.cpp:978 / 1007 / 1033`), facets
(`:1376-1377`) and inspector sections (`:2084 / 2102 / 2121`). What they do not have is any write path,
so `_typeEditable` excludes them, `+ New` hides, every cell is inert and the inspector is a dead end.
Confirmed with Mark: **screens and drills are writable; references are imported and stay read-only.**

### B1 · Screens and drills become ordinary editable types

Add to `_typeEditable`: `screens`, `drills`. Then everything the shell already does — inline cell edit,
`⌘D`, dirty gutter, Source badge, bulk-set, the validation strip — starts working with no shell change.
That is the point of the shell; do not special-case these two.

This is more than a flag. Screens and drills are **flat sets in their own files**
(`diagnostics/screens.json`, `diagnostics/drills.json`), not part of `CharacteristicPack` — so the
working-copy layering, the command snapshot and `save()` all have to grow from two registries to four.
`Command` carries `screensBefore/After` and `drillsBefore/After`; `pushCommand()` gains a six-argument
form and the old four-argument one forwards to it with the current screen and drill layers, which is
correct precisely because its callers cannot have touched them. `screen_pack.h` and `drill_pack.h` gain
the layers-apart seam the pack and norm registries already have — `coreScreenSet()`,
`loadUserScreenSet()`, `userScreenSetPath()`, `saveUserScreenSet()` — because `sharedScreenSet()` is the
merge, and an editor must write back the user's own entries and never a flattened copy of what shipped.

C++ additions on `ModelBrowser`, all on the **existing command seam** so `⌘Z` covers them from the first
edit (Addendum 01, requirement 3 — one stack, one write path):

```
setField("screens",  id, field, value)      // name, region, protocol, passCriterion,
                                            // passAtLeast, unit, note, citation
setField("drills",   id, field, value)      // name, instruction, targets, equipment, note
createObject("screens" | "drills")          // already dispatches by type — extend it
duplicate("screens" | "drills", id)
removeObject("screens" | "drills", id)      // "Move to trash" in the UI

addScreenSettles(screenId, conditionId)     // writes Condition::screenRef
removeScreenSettles(screenId, conditionId)
addDrillAnswers(drillId, conditionId)       // writes Condition::drills
removeDrillAnswers(drillId, conditionId)

screenCandidates(id, text)                  // legal, pre-filtered, as linkCandidates()
drillCandidates(id, text)
```

The four relationship commands write the **condition**, not the screen or the drill, because that is
where the join lives. So they behave like any other characteristic edit: pointing a shipped condition at
a screen makes an override of that condition, and undoing the first such edit removes it. `screenRef` is
a single field, so attaching a condition another screen already settles is a **reassignment** — allowed,
because an author may well mean it, and named in both the candidate row ("currently settled by X") and
the result so it is never silent.

Two refusals are worth stating because they are content rules rather than plumbing: clearing a screen's
`unit` while it states a numeric pass floor is refused (`screenUnitMissing` is a load ERROR, so it would
author a set that cannot be read back), and setting a floor with no unit is refused for the same reason.

Copy-on-write applies as everywhere else: editing a shipped screen writes an override, the badge flips to
`yours`, and `⌘Z` on the *first* edit removes the override and flips it back to `shipped` (Addendum 01,
requirement 6).

**Inspector fields** — per `#6d`, same field grammar as the characteristic pane, same type-ahead link
rows, no new components:

*Screen* — Name · Region · Protocol (prose) · Passing looks like (prose) · Numeric floor + unit · What
it does not settle (prose) · **Settles** — condition rows with type-ahead add and `✕` remove.

*Drill* — Name · What the golfer does (prose) · What it is trying to change (prose) · Equipment · When
it is the wrong drill (prose) · **Answers** — condition rows, type-ahead add, `✕` remove.

Both panes' link rows navigate on click, like every other relationship row in the inspector.

Three fields named in the drawing do not exist in the structs and are **not** being invented here:
`Screen::equipment`, `Drill::difficulty`, and a disposition (`confirms` / `rules out`) on Settles. Each
would be a schema change to shipped content — a new field in `screen_pack.h` / `drill_pack.h` /
`characteristic.h`, a serialiser change, a core-pack migration and a loader that tolerates its absence —
which is a session of its own and not what "make these two writable" bought. `Settles` is therefore the
`Condition::screenRef` join exactly as every reader in the app already resolves it: one screen per
condition, no disposition. If the disposition turns out to be load-bearing for `explain()`, it is a
separate change with its own migration.

**New columns worth adding** while the tables are being touched: `Drills` gets `Answers` (condition
count) and `Equipment`; `Screens` gets `Settles`. Default-sort both by that count ascending, so the
drill that answers nothing and the screen that settles nothing are the first rows an author sees — the
same principle as sorting measures by least-read. Both were sorted DESCENDING before, which put the
well-connected rows on top and buried exactly the work the panel exists for.

`Settles` and `Answers` are joins held on the condition, so those two columns are shown and not typed
into — they are edited from the inspector, where the add is a type-ahead over legal candidates. The
first column also stops calling itself `Id` and starts calling itself `Name`: it showed the label all
along, which was harmless while nothing could be typed into it and misleading the moment something can.

### B2 · References stay read-only, and stop being a dead end

The record is regenerated on every pack build, so editing it here would be overwritten. Say that, once,
in the pane — an inert pane that does not explain itself reads as a bug.

The pane shows: the formatted citation in a quiet recessed block · DOI with **copy CSL-JSON** and **open
DOI ↗** · Tier · Holds up `n claims` · one line of explanation (*"Imported from the bibliography and
regenerated on every pack build, so the record itself is not editable here. What rests on it is."*).

Then the part that makes it a working surface: **Supports these claims** — every link resting on this
reference, each with its **strength segmented control live and editable**. The citation is imported; the
claim resting on it is ours. This routes to the existing `setField("links", …)` and needs no new write
path — only `linksCitingReference(refId)` on the façade.

A condition whose own provenance cites the paper appears in the same list, because it rests on the same
paper — but it is not an edge and has no strength, so it is drawn as an ordinary navigable row rather
than given a control that would write nowhere.

The pane needs two more read-only calls, both trivial and both C++ because they are facts about the
record rather than about the view: `referenceCsl(refId)` (the single record through the real CSL
exporter, so the copied text is byte-identical to what the whole-set export would have written) and
`referenceDoiUrl(refId)`. The formatted citation is assembled in C++ too — which shape a book, a chapter
and a paper each take is a rule, and a rule written in a delegate is a rule nothing can test.

~~Also give the type rail's `References` a default sort by how much of the library each holds up.~~
**Already landed** — `model_browser.cpp:1293` sorts by `supports`, descending. Nothing to do.

---

## Not doing

- No command palette. It is at odds with the rest of the app, and this is a configuration screen — a
  sophisticated one, but a configuration screen. `#6c` is recorded and rejected.
- No `Editing / Read-only` mode toggle. Still dropped, per the brief.
- No auto-collapsing sidenav.
- Health stays read-only and stays out of `_typeEditable`. A finding is not an object.

## Settled

**Undo scope: session-scoped.** Addendum 01 left this open; it was decided and has shipped.
`ModelBrowser::undoIsSessionScoped` is a property, and `ModelEdits.qml:82` already states it where the
history is read — *"This history lasts until you close the app. Saved work is kept; the ability to step
back through it is not."* A5's popover inherits that sentence rather than inventing a second wording for
the same fact.

**Save is pack-wide.** Decided with Mark. `Save` writes everything dirty in one go — so the count in the
`n unsaved ▾` chip is the count `Save` will write, and A5 needs no per-object variant. The existing
half-save message stays as it is: it is a *partial-failure* report ("the characteristics were written and
the corridors were not"), not a description of scope.

**…and the first save warns that the base model is being changed.** Once per install, on the first
`Save` that would write over shipped content, a modal confirmation interrupts. Rationale: every other
edit in this panel is undoable and local; this one changes what the app grades against, and an author
should meet that fact deliberately once rather than never.

Follow the house prompt standard — the session-active close confirmation in `Main.qml:178-280` — and its
colours too, since this one is a call to act rather than a destruction (A6's reset is the destructive
sibling and is drawn differently):
a modal centred `Popup` on `Overlay.overlay`, `dim: true`, `closePolicy: Popup.CloseOnEscape` so Esc is
cancel, `width: Math.min(Theme.sp(420), …)`, `Theme.colorSurface` on a `Theme.colorAttention` border,
display-font title in `colorAttention`, body in `fontSzBody2` / `colorText2` / `lineHeight: 1.5`, and a
right-aligned `Row` of an attention-outlined primary followed by a neutral `colorBg2` Cancel.

```
Your diagnostics will differ from the standard

Saving writes your edits over the diagnostic model that ships with
PinPoint Studio. From here on this install screens, grades and explains
against your version, so its results are no longer directly comparable
with an unmodified install. Nothing is lost — shipped items keep their
original underneath and the Source badge shows which is which.

                            [ Save changes ]   [ Cancel ]
```

Cancel returns to the panel with everything still dirty and nothing written. The acknowledgement
persists in `AppSettings` — a `ui/diagnosticsBaseModelWarningAck` bool alongside
`ui/diagnosticsGradePolicy` in `src/Gui/app/app_settings.h:69` — and once set the prompt never appears
again. No "don't show me this again" checkbox: confirming *is* the acknowledgement.

## Sequence

1. A1 + A5 — global bar, commit cluster, delete the page header. Biggest visible win, no C++.
2. A2 — context bar, trail as heading. Deletes the middle-pane header row.
3. A3 + A4 — validation chip, `Grading` readout, `⋯` drawer.
4. A6 — reset to standard. First C++ of Part A: `resetToStandard()` plus its command and its backup
   copy. Land it with the first-save warning, not after — the warning and the way back are one feature.
5. A7 — sidenav fold, then re-derive the responsive thresholds.
6. B1 — screens and drills writable.
7. B2 — the reference pane.

Steps 1–3 are QML only. Keep it runnable at every step.

## Status

**All of it is built.** Every step of the sequence above landed in one pass, and the whole
`src/Analysis/tests` suite passes (82/82) including the new `model_browser_test` cases. The three points
where the build diverged from the drawing are stated in place above — the two structural ones are the
missing struct fields under B1 (equipment on a screen, difficulty on a drill, a disposition on Settles),
and the counting one is A6's counts being taken over the draft rather than the file.

Two things this document asked for are deliberately still absent, and both are named where they belong:
the `⌘F` hint renders as `Ctrl+F` off macOS, and the reset prompt does not name its backup file.

**A2 was revised against the built article.** Inside the middle pane the context bar was cramped at full
screen; it now spans the panel directly under the global bar. The reasoning is kept in A2 rather than
quietly corrected, because the mistake is instructive: a placement argument about meaning was allowed to
outrank a width budget nobody had added up.

What has NOT been done is the part that cannot be: **nothing here has been seen running.** The build is
clean, the QML lints clean and the panel instantiates without a runtime error under an offscreen
platform — but the machine was locked, so no pixel of this has been looked at. Every visual claim in
this document is therefore a claim about the code, not about the screen. The two hand checks below are
the minimum, and the first pass over the new chrome should assume nothing.

## Verification

Per `docs/developer/diagnostics_developer_guide.md`: every new façade method and every new command gets
a case in the `characteristic_library_model` / `model_browser` tests, and every command gets the
do-then-undo assertion Addendum 01 requires — including `addScreenSettles` and `addDrillAnswers`, whose
inverse is the easy one to get wrong. Then `core_pack_test`,
`diagnostics_catalogue_integrity_test`, `norm_pack_test`, `diagnostics_health_test`.

Two UI checks that are not unit-testable and must be done by hand, because both are regressions this
addendum exists to fix:

- at `Theme.sp(1240)` of panel width, no control in the global bar is clipped or wrapped
- with the sidenav folded and the window narrow, the concession order is sidenav → facets → inspector
- the first-save prompt appears once, Cancel writes nothing and leaves the edits dirty, and after one
  confirmation it never appears again — including across a restart, since the flag is in `AppSettings`
- reset: the `⋯` entry is absent on an untouched install; after a reset the backup file exists and loads,
  `⌘Z` restores the model in-session, and the first-save prompt returns on the next deviating save

`resetToStandard()` gets the usual do-then-undo case in `model_browser_test.cpp`, plus one asserting the
backup file is written and is a loadable pack — a backup nothing verifies is not a backup.
