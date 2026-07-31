# Addendum 01 — Undo stack

Applies to `BRIEF.md`. Raised after implementation started; **steps 1–5 of the Sequence are
unchanged**, so work already in progress is unaffected.

## What changed

1. **Editing rule 9** was "every destructive action is reversible in place… if the editor model has no
   undo stack, say so and propose one". It is now a hard requirement: *"Everything is undoable.
   Nothing in this panel may be unrecoverable."*
2. A new **Undo** section was added (immediately before "Click budget"), specifying the stack in full.
3. The **click budget** gained one row: *Undo anything, including after save — ⌘Z*.
4. The **Sequence** renumbered. Undo is now its own step, before inline editing:

   | Was | Now |
   |---|---|
   | 6. Inline editing | 6. **The undo stack**, wrapping every mutation, with the Edits history list |
   | 7. Graph pane, read-only | 7. Inline editing |
   | 8. Edge editing in the graph | 8. Graph pane, read-only |
   | — | 9. Edge editing in the graph |

   And: *do not start step 7 before 1–6 are usable.*

Nothing else in the brief moved. If you are on steps 1–5, carry on and read the Undo section before
starting step 6.

## Why it moved ahead of inline editing
There is no undo stack in the repo — no `QUndoStack`, no `QUndoCommand`, no `undo()`. The only trace
is the norm setters returning `{ ok, message, cascaded, canUndo }` and the house rule in
`docs/implementation/diagnostics_norms_impl_plan.md` that "a recoverable removal offers one in the
same breath". That is per-operation and partial.

Retrofitting a stack after inline editing ships means revisiting every mutation site, so it goes
first. The premise of this panel is fast low-ceremony editing of a 676-object graph; cheap editing
without cheap reversal is a worse tool than the panel it replaces.

## The four points that most affect implementation
- **Wrap the existing seam, don't add a second write path.** `CharacteristicEditorModel::reload()` is
  what every write finishes with — that is the choke point.
- **A cascade is ONE command.** The binding cascade is described as load-bearing; a cascade that
  undoes partially is worse than one that cannot be undone, because the author believes they are back
  where they started. Capture full prior state whenever `cascaded` is set.
- **Undo of a first edit to shipped content IS the reset.** Copy-on-write means undoing an override
  removes it. `MeasureDetail`'s "take theirs" already does this — unify, don't duplicate.
- **Do not clear the stack on Save.** Saving and immediately spotting the mistake is the common case.

## Open decision for Mark
Session-scoped stack (cleared on panel close) or persisted across launches? Session-scoped is much
simpler and probably right — but it must then be stated in the UI, because a stack that silently
empties teaches authors not to trust it.
