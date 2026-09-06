# Brief — editing the graph by press and hold

Self-contained. This is the whole specification for writing to the DAG from the canvas: what the graph is
allowed to change, how a change starts, and what C++ owes the view. It assumes no other addendum.

`ADDENDUM-03` was rejected and is void — ignore it. `BRIEF.md` specified the graph as a **read-only**
neighbourhood view drawn from `library.dag(conditionId, options)`; that still stands, and this adds writing
to it. The one live dependency is the **undo stack** (`ADDENDUM-01`): every action here pushes a command,
and none may mutate the pack directly. If the undo stack is not in, this does not land.

Design reference: `DAG Navigator.dc.html` — `#8a` held open on a node, `#8b` the three states the gesture
must cover, `#8c` the dictionary per selection type.

---

## 1 · What the graph is allowed to edit

Only the edits that are about **shape** — the ones the table is genuinely bad at, because they are about
two objects and a direction:

| Edit | How it starts |
|---|---|
| Add a causal link | hold a node → `Add cause of this…` → drag to the target |
| Add a node *and* a link | same drag, released over empty canvas |
| Move an endpoint | hold a link → `Re-point from…` / `Re-point to…` → drag to a node |
| Delete a link | hold a link → `Delete link` (or marquee, then hold, for several) |

Everything else stays where it already works. **Strength is a value, not a shape** — it edits from the
inspector on the graph exactly as it does from a cell in the table.

**Direction flip is not offered.** It is a delete plus a draw with a cycle check in the middle; as one
button it can half-fail — delete succeeds, reverse edge refused — leaving the author with less than they
started with and a message explaining why. Two steps, two answers. If it proves common, revisit it as a
*macro command* so undo still treats it as one step.

## 2 · Three rules

**2.1 · Position is decoration, not data.** Layout stays computed by `dag()`. Dragging a node *body*
nudges it for this session only: not written to the pack, not in the unsaved list, not undoable, gone on
reload, radius change, or focus change. `Tidy layout` clears every nudge. This preserves the rule the panel
already relies on — *everything in the unsaved popover is content, and everything content is in the unsaved
popover.* A nudge that produced an undo entry would break both halves.

**2.2 · The gesture is the permission.** There is **no editing mode and no editing toggle.** A structural
edit begins with a deliberate press-and-hold on the thing you mean (§3). A mode makes permission a place
you are standing rather than something you did: it must be found, remembered and turned off, it is invisible
at the moment of the accident it exists to prevent, and it forces permanent affordance chrome onto every
node to advertise a state. 350 ms of pressure is a stronger consent signal than a switch flipped twenty
minutes ago, and it cannot be left on.

The cost to accept: **no rest-state affordance for structural editing.** §7 handles that, cheaply.

**2.3 · Illegality is refused inside the gesture, never after it.** The validation strip is for problems
that already exist in the pack; it must never be how an author discovers the UI let them make one. So the
legality of every visible node is computed **once**, at the moment a link drag arms, and refused nodes drop
to 35% with the reason on them (§5.1).

## 3 · Scope of the gesture: canvas only

| | |
|---|---|
| Open | press and hold **350 ms** on a node, a link, a multi-selection, or bare canvas |
| Scope | whatever is **selected** at press; pressing an unselected node selects it first |
| Aim | keep the button down and move — the spoke under the cursor is hot |
| Commit | release over a spoke |
| Cancel | release inside the hub, release beyond the rim, or `Esc` |
| Alt entrance | **right-click opens the same ring**, latched (no button held); click to commit |

Movement beyond `Theme.sp(6)` before 350 ms is a pan or a node nudge, not a hold — cancel the timer. The
ring opens centred on the press point, clamped so rim and collar stay inside the canvas.

**The table gets no ring.** A row already has a context bar, a right-click flyout and a keyboard; a ring
there would be a fourth route to verbs that already have three. Holding a table row does nothing, and that
is a decision, not an omission. `#8c`, last column, states it on screen.

## 4 · Eight slots, fixed by meaning

Clockwise from north the ring reads **look · make · set · unmake**, and a direction means the same thing
for every selection type:

| Slot | Class | Node | Link | Several nodes | Bare canvas |
|---|---|---|---|---|---|
| **N** | look | Focus here | Open in inspector | — | Fit to window |
| **NE** | make | Duplicate | — | Duplicate *n* | Tidy layout |
| **E** | make | Add cause of this… *(drag)* | Re-point from… *(drag)* | — | New condition here |
| **SE** | make | New cause here | Re-point to… *(drag)* | — | Paste *n* nodes |
| **S** | set | Group ▸ | Strength ▸ | Group ▸ *(all n)* | Radius ▸ |
| **SW** | unmake | Revert to shipped | Revert to shipped | Revert *n* to shipped | Hide weak links |
| **W** | unmake | Move to trash | Delete link | Trash *n* conditions | — |
| **NW** | cross | Show in table | Show in table | Show *n* in table | Switch to table |

**An unfilled slot is a hairline gap — never a disabled button.** The silhouette is identical every time
the ring opens, so muscle memory survives a change of selection; the only variable is how many wedges are
lit. Do not reflow live spokes to close a gap. Bulk labels carry the count.

`Move to trash` / `Delete link` render `colorError` **text** on the ordinary wedge fill — no red wedge, no
confirmation inside the ring. Wording matters: objects go to a **trash** they can be found in; a link is a
row that ceases to exist, so it is `Delete link`, never "Move to trash". Undo is the whole of a link's
recoverability — see §9.1.

### 4.1 · The hub

Text, not a button: the selection's name on line one, its type and state on line two (`condition ·
shipped`, `causal link`, `4 conditions`, `11 of 108 drawn`). When a spoke is hot, line two becomes that
spoke's spoken label — the only place the ring explains itself, and why wedges can stay icon-plus-short-
label. Releasing on the hub cancels. Nothing is ever committed from the centre.

### 4.2 · The collar — one level, three cells

A spoke holding a **value** (`Strength`, `Group`, `Radius`) carries `▸` and expands on continued outward
drag into a **3-cell collar past the rim**, aligned to that spoke's arc. The current value is the cell the
cursor is already on, so hold-and-release with no travel is a no-op, not an accidental change.

Depth is **one level, full stop.** A value with more than three options gets no collar — its spoke opens
the inspector instead. `Group` shows the three most-used plus `More…`. Never paginate a collar.

Mockup geometry: hub `r0 = 52`, rim `r1 = 126`, collar `+46`, `3°` inter-wedge gap. Match the proportions,
not the pixels — `Theme.sp()` everything. `#8b` left frame.

## 5 · The drags the ring hands off to

Committing `Add cause of this…`, `Re-point from…` or `Re-point to…` dissolves the ring and drops straight
into a drag **with the button still down** — one continuous gesture from hold to release on the target.
`#8b` middle frame.

### 5.1 · Refusal, computed once

On arm, compute the refusal set for the fixed endpoint:

```
illegal(target) =
     target == source                                  // no self-loops
  || edgeExists(source → target, "causes")             // "already linked"
  || descendants(target).contains(source)              // "cycle"
```

The third case is the expensive one, and the reason this happens on arm rather than per hover: walk the
`causes` graph *once* from the source **upwards**, and mark. `descendants(target) ∋ source` ⟺
`target ∈ ancestors(source)`. One traversal, then O(1) per node.

The predicate must be authoritative, so **it lives in C++, not QML** (§8). QML must not reimplement
reachability over a truncated view — a node that is off-canvas is still in the graph, and a UI-side check
would happily draw a cycle through it.

### 5.2 · During the drag

- refused nodes → `opacity: 0.35`, meta line replaced by the reason: `cycle` in `colorWarn`,
  `already linked` in `colorText3`. Not drop targets; releasing over one cancels
- hovered legal node → `colorAccent` border, `colorAccentLight` fill, 3px accent ring, meta line reads
  `drop to add cause`
- the drag path is a dashed accent bezier from the source to the cursor, using the **same curve function
  the drawn edges use**, so it reads as the same kind of object
- the arrowhead is drawn **at the held node** from the first frame, so the direction is never in question

**Release over a legal node:** `AddCauseCommand(from, to, strength = "moderate")`, label
`Link Poor pelvic disassociation → Over the top`. The inspector opens on the new link, where strength is
one click away. **Release anywhere else:** cancel, no command, no message. A cancelled drag is not an error.

### 5.3 · Released over empty canvas → node, then link

A dashed ghost box appears at the release point with a small popover **below it** asking exactly two things:

- **type** — four chips: `characteristic` · `measure` · `screen` · `drill`, defaulting to `characteristic`.
  Measures route through `ModelMint` exactly as `+ New measure` does today
- **name** — one text field, focused, `Enter` commits

Nothing else. Every other field already has a home in the right pane, and asking for them inside a drag
makes the fast gesture slow. The object is created as a **draft in the author's pack**; the inspector opens
on it.

Node and link are **one undo step** — a macro command: `Create Early hip stall and link from Over the top`.
Two entries would let an author undo the node and keep a link to nothing.

Refusals do not apply here (a new node has no ancestors), which makes this the escape hatch when everything
on screen is refused.

### 5.4 · Re-point

`Re-point from…` / `Re-point to…` run the same machinery, with the refusal set computed for the endpoint
that is **staying put**. Release on a legal node → `RepointEdgeCommand`, label
`Re-point Over the top → Loss of width to Early extension`.

The inspector's `FROM` / `TO` rows each carry a `Re-point…` affordance arming the identical drag, for
anyone who does not think to hold the link. One gesture, two entrances — not two features.

### 5.5 · Marquee → bulk

**Shift**+drag on empty canvas draws a marquee. **A marquee touching only edges selects
edges; if it touches any node it is a node selection** and behaves as the table's does. The bare drag
is the pan (§6): the graph is read far more often than it is edited, so moving the picture is what the
unmodified drag on empty canvas — or on a link, or on any node when the graph is not editable — does. Then hold inside
the selection for the bulk ring (§4, column three), where every live spoke is the bulk form with the count
in the label. Bulk strength and bulk delete are **one command each** over a list, so one `⌘Z` restores all
of them. `#8b` right frame.

## 6 · Pan, Nudge and Tidy

A drag that is not a nudge and not a marquee **pans the canvas** — the wheel and the scroll bars keep
working, but a picture wider than the pane is now reachable by hand, which is what the reading case
needs. A pan changes nothing but the view, so it leaves the selection standing.

Node-body drag moves a node within the canvas for this session, when the graph is editable. `Tidy layout` re-runs `dag()` and drops
every nudge. Nudges also drop on focus change, radius change and reload — none of which needs a warning,
because rule 2.1 means nothing was at stake. If saved layouts are asked for later, that is a **view**
feature (a named arrangement per user), not a pack feature. Do not let it in through this door.

## 7 · Discoverability, given there is no handle

Three measures, in order of importance:

1. **Right-click opens the same ring.** Every desktop author tries this. It must not open a different menu
2. **The first hold of a session holds its labels a beat longer** — 600 ms before settling to the steady
   state. Once per session, not once per ring
3. **The inspector keeps every verb the ring has, worded identically.** The ring is the fast path, never
   the only one — an author who never discovers it loses no capability. This is a hard requirement, not a
   nicety: it is what makes shipping without a mode safe

## 8 · What the C++ side owes this

Additions to `CharacteristicLibraryModel`, all routed through the undo stack:

| Method | Returns |
|---|---|
| `canLinkCause(fromId, toId)` | `{ ok, reason }` — `reason ∈ self · exists · cycle` |
| `linkRefusals(fromId, ids[])` | `{ id: reason }` for the visible set, one traversal |
| `addCause(fromId, toId, strength)` | `{ ok, message, edgeId, canUndo }` |
| `repointCause(edgeId, end, newId)` | `end ∈ from · to` |
| `removeCause(edgeId[])` | list form, so bulk is one command |
| `setCauseStrength(edgeId[], strength)` | list form, same reason |
| `createObject(type, name)` | draft in the active pack; `{ ok, id }` |

`dag()` is unchanged. It already returns `{ nodes[], edges[], width, height, focusX, focusY, truncated }`
and everything above addresses objects by id, so the view can re-request a layout after any command and
redraw from it. **Every command finishes the way writes finish today** — via the existing reload path — so
graph, table and validation chip stay in agreement without the graph telling them anything.

If a ring spoke needs a model call that is not in this table, the spoke is wrong.

## 9 · QML notes

- One `RingMenu.qml` taking `model: [{icon, label, hint, kind, enabled, danger, values[]}]` of length
  **exactly 8**, `null` for gaps. Selection types differ only by the array passed in — no subclass per type
- Open with a `TapHandler`, `longPressThreshold: 350`, on the **canvas root**, hit-testing the press point.
  Not a handler per node — the ring must also open on links and on bare canvas
- Wedges are one `Shape` with a `ShapePath` per sector; labels are `Text` items at the arc midpoint, not
  inside the paths. The drag path shares that same `Shape` with `strokeStyle: ShapePath.DashLine` — do not
  add a second `Canvas` for the preview
- Hot-spoke tracking is angle arithmetic on the cursor vector (`atan2`, snapped to 45°) with a dead zone
  inside `r0`. Not per-wedge `HoverHandler`s; they fight during a held drag
- The refusal set is a plain JS object held for the duration of the drag, cleared on release. Do not
  re-query per `positionChanged`
- Ring, create popover and bulk bar are `Popup`s parented to the **canvas**, not the panel, so they cannot
  escape the pane the way the old status bar did. Ring: `modal: false`,
  `closePolicy: CloseOnEscape | CloseOnPressOutside`
- Keyboard equivalents live on the pane's `Shortcut`s, not inside the ring

## 10 · Not doing

- A ring in the table (§3)
- Collar depth > 1, or a collar wider than 3 cells (§4.2)
- Reflowing a slot to a different direction when a selection does not fill it (§4)
- Direction flip (§1); persisted node positions (§2.1)
- Creating `corroborates` / `excludes` edges by drag — the canvas draws `causes`; the other two edit in the
  link inspector, where their own rules are already stated
- Editing from the graph while `health` is the active type — there is no neighbourhood for a finding
- Multi-source drag ("link these three to that one"). The marquee covers the bulk case that has been asked
  for, which is strength

## 11 · Still open

1. **Does `Add cause of this…` need to split into two spokes** (`Add cause…` / `Add effect…`)? **My read:
   no — one spoke, and the wording plus the arrowhead carry the direction.** A second spoke costs the `SE`
   slot `New cause here` occupies and doubles the make column for a case authors hit rarely. Instead the
   direction is stated three times: the spoke label, the arrowhead drawn at the held node from frame one
   (§5.2), and the hovered target reading `drop to make this a cause of Over the top`. If downstream
   drawing turns out to be common, add `shift` to invert mid-drag with the hub line changing to `Add effect
   of this…` — an inversion, not a new slot. **Needs a yes before implementation.**
2. **Does `Delete link` need a blast-radius confirmation,** or is undo enough? My read: enough, *provided
   undo persists across a session boundary.* If undo is session-scoped, a delete followed by a restart is
   unrecoverable and the interruption earns itself.
3. **A drag target that is off-canvas** because the neighbourhood is truncated: (a) auto-expand radius on
   hover at the canvas edge, (b) drop on the edge to open a picker, (c) nothing — use the table. I lean
   (c) for the first landing; it is the honest answer and costs nothing.
4. **`Esc` vs release-outside on a latched (right-click) ring** — dismiss only, or also clear the
   selection? Lean dismiss only.
