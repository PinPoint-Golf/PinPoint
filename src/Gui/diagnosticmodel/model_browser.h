/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "../../Diagnostics/drill_pack.h"
#include "../../Diagnostics/norm_provider.h"
#include "../../Diagnostics/pack_provider.h"
#include "../../Diagnostics/screen_pack.h"
#include "../../Metrics/metric_catalogue.h"

#include <QFutureWatcher>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

// ModelBrowser — the whole Diagnostic Model panel's model: one connected content graph, read and
// edited through one object.
//
// ── Reuse; do not reimplement ───────────────────────────────────────────────────────────────────
//
// The brief's rule, obeyed where it counts: every RULE here is a call into the layer that owns it —
// causesOf/effectsOf/coverageOf/hasCausalPath and tailsOfAxis (characteristic_pack.h), validatePack,
// diagnosticsHealth, layoutDag, measureDisplayLabel, the shared screen/drill/reference registries,
// and the enum label tables. What is reimplemented is MARSHALLING, and only marshalling.
//
// It replaced an earlier façade that read the pack AS SAVED, which is correct for a read-only panel
// and fatal for this one. Editing here accumulates in an unsaved WORKING COPY of the user pack (rule
// 8: "Save is once, not per field"), and every surface has to show the library as it would be if you
// saved now — the table, the inspector, the graph, and above all the validation strip, which is
// worthless if it grades the file rather than the draft.
//
// ── The layering ────────────────────────────────────────────────────────────────────────────────
//
//   m_core        the shipped pack, read-only, and the ONLY way to answer "does this ship?"
//   m_savedUser   the user pack as it is on disk — the baseline every dirty mark is measured from
//   m_working     the user pack as edited. Unsaved.
//   m_assembled   core + memory(m_working). Rebuilt after every mutation; everything reads this.
//
// The NORM set is layered identically — m_savedNorms / m_workingNorms / m_norms — because corridors
// are edited here too, and ONE undo history spans both registries. A command therefore snapshots
// both packs, and one Save writes both files. Two histories was the alternative and it was rejected
// as too complex to reason about: an author who edits a characteristic, then its corridor, then hits
// ⌘Z twice has one mental model of what should happen, not two.
//
// Copy-on-write falls out of the layering rather than being coded: editing a field of a SHIPPED
// object copies that object into m_working, and undoing that first edit removes the copy — which is
// exactly the reset, with no separate "take theirs" path to keep in step. That is ADDENDUM-01's
// third point, satisfied structurally.
//
// ── QML holds no rules ──────────────────────────────────────────────────────────────────────────
//
// Sorting, filtering, faceting, search, legality of a proposed link, which cell may be edited and
// with what control, and which of two values is "yours" — all decided here. QML is handed columns()
// and rows() and renders them; it does not know what a Measure is. A rule written in a delegate is
// a rule nothing can test.

class ModelBrowser : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // The type rail: [{ key, label, count, hint }] for every content type, count derived by asking
    // the row list rather than by a stored figure that can go stale.
    Q_PROPERTY(QVariantList types READ types NOTIFY modelChanged)

    // Summed across the types, never hardcoded — the brief is explicit, and a literal 676 would be
    // wrong the first time anybody authored anything.
    Q_PROPERTY(int totalObjects READ totalObjects NOTIFY modelChanged)

    // "core v1.0.0 · schema 1" for the header line.
    Q_PROPERTY(QString packLabel READ packLabel NOTIFY modelChanged)

    // ── Draft state ─────────────────────────────────────────────────────────
    Q_PROPERTY(bool dirty READ dirty NOTIFY modelChanged)
    // Objects that differ from the saved pack, NOT commands on the stack. Three edits to one label
    // are one unsaved object, and the status bar says what would be written.
    Q_PROPERTY(int unsavedCount READ unsavedCount NOTIFY modelChanged)

    // ── How far this install has drifted from what shipped ──────────────────
    //
    // Two counts because they are two different things to lose, and the reset prompt says both.
    // `overridden` is shipped objects this install has changed — the ones that make its grading
    // differ from an unmodified install, which is what the first-save warning is about. `authored`
    // is objects written here that core has never heard of; those extend the model rather than
    // contradicting it, so they do not trigger that warning, but a reset still takes them.
    Q_PROPERTY(int overriddenCount READ overriddenCount NOTIFY modelChanged)
    Q_PROPERTY(int authoredCount   READ authoredCount   NOTIFY modelChanged)

    // ── Undo ────────────────────────────────────────────────────────────────
    Q_PROPERTY(bool    canUndo   READ canUndo   NOTIFY modelChanged)
    Q_PROPERTY(bool    canRedo   READ canRedo   NOTIFY modelChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY modelChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY modelChanged)
    // The Edits history: [{ index, label, detail, undone, saved }], oldest first. Clicking a row
    // calls undoTo(), which is what makes the stack navigable rather than a counter.
    Q_PROPERTY(QVariantList edits READ edits NOTIFY modelChanged)
    // Session-scoped, and SAID SO in the UI. A stack that silently empties teaches an author not to
    // trust it — see the panel's Edits header. (ADDENDUM-01's open question; recorded in the code
    // because a decision nobody can find is a decision that gets remade.)
    Q_PROPERTY(bool undoIsSessionScoped READ undoIsSessionScoped CONSTANT)

    // What is wrong with the DRAFT — the validation strip. Rows are the health shape, and clicking
    // one filters the table to the offending object.
    Q_PROPERTY(QVariantList validation READ validation NOTIFY modelChanged)
    Q_PROPERTY(int validationErrorCount   READ validationErrorCount   NOTIFY modelChanged)
    Q_PROPERTY(int validationWarningCount READ validationWarningCount NOTIFY modelChanged)

    // ── The corridor sample scan ────────────────────────────────────────────
    Q_PROPERTY(bool    corridorScanning READ corridorScanning NOTIFY corridorSamplesChanged)
    // Which measure the samples in hand belong to. Reported rather than assumed: a plot rendered
    // from another measure's readings would be a confident, wrong picture.
    Q_PROPERTY(QString corridorScanned  READ corridorScanned  NOTIFY corridorSamplesChanged)
    Q_PROPERTY(int     corridorSwings   READ corridorSwings   NOTIFY corridorSamplesChanged)
    // The library root to scan, set from appSettings.athleteLibraryPath by the panel — the same
    // seam every other holder of this setting uses, so this object stays free of the dependency.
    Q_PROPERTY(QString libraryRoot READ libraryRoot WRITE setLibraryRoot NOTIFY corridorSamplesChanged)

    // The pack-wide grade policy, by NAME. Load-bearing for the corridor picture: every band edge
    // is policy-dependent, so drawing under the default while the user has chosen Strict would
    // paint a corridor the app does not grade against — which norm.h records as a bug that already
    // happened once. Bound to AppSettings by the panel, so this object keeps no settings dependency.
    Q_PROPERTY(QString gradePolicy READ gradePolicy WRITE setGradePolicy NOTIFY modelChanged)

public:
    explicit ModelBrowser(QObject *parent = nullptr);
    ~ModelBrowser() override;

    QVariantList types() const;
    int          totalObjects() const;
    QString      packLabel() const;
    bool         dirty() const;
    int          unsavedCount() const;
    int          overriddenCount() const;
    int          authoredCount() const;
    bool         canUndo() const;
    bool         canRedo() const;
    QString      undoLabel() const;
    QString      redoLabel() const;
    QVariantList edits() const;
    bool         undoIsSessionScoped() const { return true; }
    QVariantList validation() const;
    int          validationErrorCount() const;
    int          validationWarningCount() const;

    bool    corridorScanning() const { return m_corridorScanning; }
    QString corridorScanned() const { return m_corridorScanned; }
    int     corridorSwings() const { return int(m_corridorValues.size()); }
    QString libraryRoot() const { return m_libraryRoot; }
    void    setLibraryRoot(const QString &root);
    QString gradePolicy() const { return m_policyName; }
    void    setGradePolicy(const QString &name);

    // ── Reading ─────────────────────────────────────────────────────────────

    // The column spec for one type. Rows come back with a `cells` array parallel to this, so the
    // table delegate renders any type without knowing which one it has:
    //   { key, title, width, flex, align, mono, sortable, defaultDescending }
    // `width` is in unscaled design pixels; QML runs it through Theme.sp(). `flex` marks the one
    // column that takes the slack — the name column, which is what gets starved otherwise.
    Q_INVOKABLE QVariantList columns(const QString &type) const;

    // Rows for one type. filters: { search, sort, descending, facets:{key:[values…]}, ids:[…] }.
    //
    // Sorting and filtering happen HERE. `sort` names a column key and falls back to the type's
    // default sort — the one that answers the question the author arrived with (measures by status
    // then least-read, so the 42 nothing reads are on screen without asking).
    //
    // Each row: { id, type, label, dirty, source, sourceLabel, dot, cells[] }
    // Each cell: { text, value, field, kind, options[], tone, mono, align, editable, own }
    Q_INVOKABLE QVariantList rows(const QString &type, const QVariantMap &filters = {}) const;

    // The facet rail under the type list: [{ key, label, options:[{ value, label, count }] }],
    // counted over the type's rows BEFORE its own facet is applied, so a count never reads zero for
    // the thing you are looking at.
    Q_INVOKABLE QVariantList facets(const QString &type) const;

    // One search across every registry at once (the brief's 4b). Rows carry a Type column and the
    // same cell shape as everything else, so "hip" returns characteristics, measures, signals,
    // screens and drills in one list. Cannot be assembled in QML — it spans nine registries.
    Q_INVOKABLE QVariantList searchAll(const QString &query) const;

    // The inspector: a RELATIONSHIP HUB, not a property sheet. Sections are
    //   { title, kind, note, rows:[{ type, id, label, detail, tone, navigable }] }
    // so every related object is one click away and QML never decides what relates to what.
    Q_INVOKABLE QVariantMap inspect(const QString &type, const QString &id) const;

    // The graph around ANY object, not only a condition.
    //
    // A condition gets the causal DAG (dag_layout.h) because it has ranks — causes to the left,
    // effects to the right — and that ordering is the thing the picture is for. Nothing else does:
    // a measure has readers and corridors, a screen has the conditions it would settle, and there is
    // no "distance" to rank those by. Those get a NEIGHBOURHOOD instead — the object in the middle,
    // what points at it on the left, what it points to on the right — which is the honest shape for
    // a relation that is one hop and has no direction to iterate.
    //
    // Same node and edge shape either way, so the renderer does not branch.
    Q_INVOKABLE QVariantMap graph(const QString &type, const QString &id,
                                  const QVariantMap &options = {}) const;

    // The laid-out causal DAG around one condition — every coordinate from dag_layout.h, over the
    // WORKING assembly so an unsaved edge is drawn. Same shape as
    // the graph pane renders.
    Q_INVOKABLE QVariantMap dag(const QString &conditionId, const QVariantMap &options = {}) const;

    // Legal targets for a link from `fromId`, pre-filtered: no self, no existing edge, no cycle,
    // and for `corroborates` no pair that already has a causal path (which the validator forbids).
    // The filtering is the point — an illegal edit cannot be CONSTRUCTED, rather than being built
    // and refused afterwards.
    Q_INVOKABLE QVariantList linkCandidates(const QString &relation, const QString &fromId,
                                            const QString &search = QString()) const;

    // Measures a characteristic could be detected by, minus the ones it already reads.
    Q_INVOKABLE QVariantList measureCandidates(const QString &conditionId,
                                               const QString &search = QString()) const;

    // Would this link be legal, and if not, why? Returns { ok, reason }.
    //
    // Asked DURING a drag in the graph, not on release: an illegal target has to refuse while the
    // pointer is still moving, with the reason stated ("X already leads to Y, so this would create
    // a cycle"). By release the author has already committed to the gesture, and a refusal then
    // reads as the tool being broken. addLink() asks the same function, so the drag cannot say yes
    // to something the write would then reject.
    Q_INVOKABLE QVariantMap linkLegality(const QString &fromId, const QString &toId,
                                         const QString &relation = QStringLiteral("causes")) const;

    // ── Editing ─────────────────────────────────────────────────────────────
    //
    // Every one of these is ONE command on the undo stack, and every one returns
    // { ok, message } — a refusal has to reach the author in their own terms, never a log.
    //
    // A cascade is ONE command (ADDENDUM-01's second point): the stack holds whole before/after
    // copies of the working pack, so an edit that touches rows the author never named undoes as a
    // unit. A cascade that undid partially would be worse than one that could not be undone,
    // because the author would believe they were back where they started.

    Q_INVOKABLE QVariantMap setField(const QString &type, const QString &id, const QString &field,
                                     const QVariant &value);

    // Multi-select bulk-set: one command, whatever N is. Re-tiering twenty links one at a time is
    // the single biggest time sink in the old panel.
    Q_INVOKABLE QVariantMap setFieldOnAll(const QString &type, const QStringList &ids,
                                          const QString &field, const QVariant &value);

    Q_INVOKABLE QVariantMap addLink(const QString &fromId, const QString &toId,
                                    const QString &relation = QStringLiteral("causes"),
                                    const QString &strength = QStringLiteral("moderate"));
    Q_INVOKABLE QVariantMap removeLink(const QString &fromId, const QString &toId,
                                       const QString &relation = QStringLiteral("causes"));

    // Attach a measure to a characteristic at a tail, minting the signal that reads it.
    Q_INVOKABLE QVariantMap addMeasureTo(const QString &conditionId, const QString &measureId,
                                         const QString &direction = QStringLiteral("high"));
    Q_INVOKABLE QVariantMap removeMeasureFrom(const QString &conditionId, const QString &measureId);

    // Duplicate beats blank: a new object pre-filled from an existing one. Returns the new id in
    // `id` so the table can select it and the author can type over the label.
    Q_INVOKABLE QVariantMap duplicate(const QString &type, const QString &id);

    Q_INVOKABLE QVariantMap removeObject(const QString &type, const QString &id);

    // ── Screens and drills ──────────────────────────────────────────────────
    //
    // Both registries have had rows, facets and an inspector section since this panel landed, and no
    // write path at all — so every cell was inert and the inspector was a dead end. They are ordinary
    // editable types now: setField(), createObject(), duplicate() and removeObject() all dispatch to
    // them, so the shell's inline edit, ⌘D, dirty gutter, Source badge and bulk-set need no changes.
    //
    // The relationships are the exception, because they are not stored where you would guess. A
    // screen does not list what it settles and a drill does not list what it answers — the CONDITION
    // names them (`Condition::screenRef`, `Condition::drills`), which is the join every reader in the
    // app already uses. So these four write the condition, and they live here rather than on the
    // condition because "what does this screen settle" is the question an author has open.
    Q_INVOKABLE QVariantMap addScreenSettles(const QString &screenId, const QString &conditionId);
    Q_INVOKABLE QVariantMap removeScreenSettles(const QString &screenId, const QString &conditionId);
    Q_INVOKABLE QVariantMap addDrillAnswers(const QString &drillId, const QString &conditionId);
    Q_INVOKABLE QVariantMap removeDrillAnswers(const QString &drillId, const QString &conditionId);

    // Legal, pre-filtered candidates, as linkCandidates() is: conditions this screen does not already
    // settle / this drill does not already answer. An illegal edit cannot be CONSTRUCTED.
    Q_INVOKABLE QVariantList screenCandidates(const QString &screenId,
                                              const QString &search = QString()) const;
    Q_INVOKABLE QVariantList drillCandidates(const QString &drillId,
                                             const QString &search = QString()) const;

    // Every link resting on one reference. The citation is imported and stays read-only; the CLAIM
    // resting on it is ours, and its strength is editable through the ordinary setField("links", …).
    Q_INVOKABLE QVariantList linksCitingReference(const QString &refId) const;

    // One record as CSL-JSON, for the reference pane's copy action — the same format the whole-set
    // export writes, because a coach pasting one citation into Zotero wants what the file would have
    // given them. Empty for an id that resolves to nothing.
    Q_INVOKABLE QString referenceCsl(const QString &refId) const;
    // The resolver URL for a DOI, or empty when the record has none. Built here rather than in QML:
    // which of three identifiers a record carries is a fact about the record.
    Q_INVOKABLE QString referenceDoiUrl(const QString &refId) const;

    // ── Corridors ───────────────────────────────────────────────────────────
    //
    // A corridor is a norm row keyed on (measureId, contextId, cohort). It is edited through
    // setField() like everything else — the fields are `mu`, `sigmaLo`, `sigmaHi`, `plausibleLo`,
    // `plausibleHi`, `unit`, `source` and `citation` — and the row id is the norm key.

    // Every context this measure could carry a corridor at, marked with what already resolves
    // there. Rows: { id, label, depth, own, inherited, inheritedFrom, mu, unit }.
    Q_INVOKABLE QVariantList corridorContexts(const QString &measureId) const;

    // Start a corridor of your own at this context. Seeded from whatever currently resolves there,
    // so the common edit is a nudge rather than authoring five numbers from nothing.
    Q_INVOKABLE QVariantMap addCorridor(const QString &measureId, const QString &contextId);

    // Take another context's numbers wholesale. `adoptFrom` in the old editor.
    Q_INVOKABLE QVariantMap adoptCorridor(const QString &measureId, const QString &contextId,
                                          const QString &fromContextId);

    // Drop your row. Where core ships one this restores it; where it does not, the parent context's
    // corridor resolves again. Those are different outcomes and the message says which.
    Q_INVOKABLE QVariantMap resetCorridor(const QString &measureId, const QString &contextId);

    // ── The corridor, as a picture ──────────────────────────────────────────
    //
    // The distribution the norm CLAIMS, drawn over the readings the library actually holds. All
    // geometry from corridor_plot.h — QML draws these points and positions nothing.
    //
    // `options` carries the pane's own metrics (width, height); anything omitted keeps the
    // CorridorPlotOptions default. Returns an empty `curve` for a key that resolves to nothing.
    Q_INVOKABLE QVariantMap corridorPlot(const QString &measureId, const QString &contextId,
                                         const QVariantMap &options = {}) const;

    // Read every swing in the library and reduce this measure over each. Asynchronous, because it
    // walks the whole library; the plot renders from whatever has been read so far and says so.
    Q_INVOKABLE void scanCorridor(const QString &measureId);

    // ── Context bindings — where a characteristic applies ───────────────────
    //
    // A binding is an EXCEPTION: a context with no row anywhere on its chain applies. Rows carry
    // both what is authored HERE and what is inherited, because a control that cannot tell one from
    // the other teaches the author that every row is an assertion.
    Q_INVOKABLE QVariantList bindingsOf(const QString &conditionId) const;
    Q_INVOKABLE QVariantMap  setBinding(const QString &conditionId, const QString &contextId,
                                        bool applicable, bool material);
    Q_INVOKABLE QVariantMap  clearBinding(const QString &conditionId, const QString &contextId);

    // ── Minting a measure ───────────────────────────────────────────────────
    //
    // A typed phrase SEEDS facet selections; it is not a query. Wrong guesses are corrected by
    // tapping chips, which is far easier than rephrasing a search that returned nothing.
    Q_INVOKABLE QVariantMap  seedFacetsFromPhrase(const QString &phrase) const;
    Q_INVOKABLE QVariantList anatomyRoles() const;
    Q_INVOKABLE QVariantList quantitiesFor(const QString &whatRole) const;
    Q_INVOKABLE QVariantList referencesFor(const QString &whatRole, const QString &quantity) const;
    Q_INVOKABLE QVariantList reducerKinds() const;
    Q_INVOKABLE QVariantList phases() const;
    // { valid, reason, label, id, exactMatch, nearDuplicates[] }. The near-duplicate check is the
    // defence against a library filling with almost-identical measures, and it has to fire AT
    // CREATION — after the fact nobody merges them.
    Q_INVOKABLE QVariantMap  previewMeasure(const QVariantMap &facets) const;
    Q_INVOKABLE QVariantMap  mintMeasure(const QVariantMap &facets);

    // A blank object of this type. Duplicate is the fast path and this is the second one, but
    // "second" is not "absent" — there has to be a way to author something unlike anything here.
    Q_INVOKABLE QVariantMap createObject(const QString &type);

    // ── Pack-wide settings ──────────────────────────────────────────────────
    //
    // Not content: these change how everything GRADES, so they are stated apart from the draft and
    // they are not undoable — they are not edits to the library, they are how it is being read.
    Q_INVOKABLE QVariantList gradePolicies() const;
    Q_INVOKABLE QVariantList normSets() const;

    // ── Artefacts that leave the app ────────────────────────────────────────
    Q_INVOKABLE QVariantMap exportRoadmap() const;
    Q_INVOKABLE QVariantMap exportReferences() const;

    // The roadmap and its two companions, which have no other home once Diagnostics retires.
    Q_INVOKABLE QVariantList roadmap() const;
    Q_INVOKABLE QVariantList captureGaps() const;
    Q_INVOKABLE QVariantList causeCoverage() const;
    Q_INVOKABLE QVariantList glossary(const QString &search = QString()) const;

    // ── Undo / save ─────────────────────────────────────────────────────────
    Q_INVOKABLE QVariantMap undo();
    Q_INVOKABLE QVariantMap redo();
    // Wind the stack to just after command `index` (-1 = before everything). This is what the Edits
    // list clicks into.
    Q_INVOKABLE QVariantMap undoTo(int index);

    // Write the working pack to the user pack file. Does NOT clear the stack — saving and
    // immediately spotting the mistake is the common case (ADDENDUM-01's fourth point).
    Q_INVOKABLE QVariantMap save();
    // Discard every unsaved edit, back to the file. Itself undoable.
    Q_INVOKABLE QVariantMap revert();

    // Put this install back on the model that ships: every override AND everything authored here,
    // across both registries. NOT revert() — that one discards the session's UNSAVED edits back to
    // the file; this discards what the file itself holds, and it writes.
    //
    // Recoverable twice over, because the panel's rule is that nothing here is unrecoverable:
    //   · it is a command like every other write, so ⌘Z restores the whole draft for the session
    //   · the user layers are COPIED ASIDE before they are replaced (`*-<stamp>.backup.json`), so an
    //     author who resets in error still has the file after the session's undo has expired
    // The returned map carries `backups` — the paths written — because a backup nobody is told about
    // is a backup nobody can use.
    Q_INVOKABLE QVariantMap resetToStandard();

    // Re-take the shared providers after somebody else wrote (the old panel, the corridor editor).
    Q_INVOKABLE void refresh();

signals:
    void modelChanged();
    void corridorSamplesChanged();
    // A save landed. The rest of the app caches its providers, so whoever is listening has to
    // re-take theirs or the edit is on disk and invisible until relaunch.
    void libraryChanged();

private:
    // ── Assembly ────────────────────────────────────────────────────────────
    void rebuild();                       // m_assembled = core + memory(m_working)
    const pinpoint::analysis::CharacteristicPack &pack() const;

    // ── Undo stack ──────────────────────────────────────────────────────────
    //
    // Whole-pack snapshots rather than field diffs. The user pack holds overrides only, so a copy is
    // small — and it is the only representation under which "a cascade is one command" is true by
    // construction rather than by remembering to capture the right extra rows.
    struct Command {
        QString                                label;    // "Strength → strong"
        QString                                detail;   // "Early extension → Loss of posture"
        // ALL FOUR registries, every time. A command that snapshotted only the one it happened to
        // touch would restore correctly on its own and corrupt the others as soon as the stack was
        // wound past it — the state a position on the stack describes is the whole draft, not a diff.
        pinpoint::analysis::CharacteristicPack before;
        pinpoint::analysis::CharacteristicPack after;
        pinpoint::analysis::NormPack           normsBefore;
        pinpoint::analysis::NormPack           normsAfter;
        pinpoint::analysis::ScreenSet          screensBefore;
        pinpoint::analysis::ScreenSet          screensAfter;
        pinpoint::analysis::DrillSet           drillsBefore;
        pinpoint::analysis::DrillSet           drillsAfter;
    };
    // Push the current state as a command, dropping any redo tail. `before` is the pack state the
    // command started from; the norm baseline is captured by the caller in the same breath.
    //
    // The four-argument form is for every write that cannot touch a screen or a drill, which is most
    // of them: it captures the CURRENT screen and drill layers as the before-state, which is correct
    // precisely because they did not change. Writes that do touch them use the long form.
    void        pushCommand(const QString &label, const QString &detail,
                            const pinpoint::analysis::CharacteristicPack &before,
                            const pinpoint::analysis::NormPack           &normsBefore);
    void        pushCommand(const QString &label, const QString &detail,
                            const pinpoint::analysis::CharacteristicPack &before,
                            const pinpoint::analysis::NormPack           &normsBefore,
                            const pinpoint::analysis::ScreenSet          &screensBefore,
                            const pinpoint::analysis::DrillSet           &drillsBefore);
    QVariantMap applyStackPosition(int newIndex);

    // ── Working-copy helpers ────────────────────────────────────────────────
    //
    // Copy-on-write: fetch the working row for an id, copying it out of the assembled library on
    // first touch. Null only when no such object exists anywhere.
    pinpoint::analysis::Condition *workingCondition(const QString &id);
    pinpoint::analysis::Measure   *workingMeasure(const QString &id);
    pinpoint::analysis::Signal    *workingSignal(const QString &id);
    pinpoint::analysis::Edge      *workingEdge(const QString &fromId, const QString &toId,
                                               pinpoint::analysis::EdgeType type);
    // The causal edge set of `id` as the user pack must hold it. An override REPLACES a condition's
    // whole incoming causal set, so removing a shipped cause means writing back all the others.
    void materialiseCausesOf(const QString &conditionId);

    // Which ids differ from the saved pack, by comparing the two packs' serialised entities. Derived
    // rather than tracked: undo and revert move the working copy wholesale, and a hand-kept touched
    // set would drift the first time either of them ran.
    const QSet<QString> &dirtyIds() const;
    void                 invalidateDerived();

    // How many objects in the DRAFT sourceOf() answers `want` for, across all four registries. The
    // two counts the reset prompt states are the same question asked twice.
    int countBySource(const QString &want) const;

    // Rows for one type, unsorted and unfiltered. Everything else composes over this.
    QVariantList rawRows(const QString &type) const;

    // Marshalling helpers shared by rows() and searchAll().
    QVariantMap conditionRow(const pinpoint::analysis::Condition &c, bool asCause) const;
    QVariantMap measureRow(const pinpoint::analysis::Measure &m) const;
    QVariantMap signalRow(const pinpoint::analysis::Signal &s) const;
    QVariantMap edgeRow(const pinpoint::analysis::Edge &e) const;

    // Every writable field of one object, as editor rows. THE list — the inspector renders exactly
    // these, so "what can I edit" has one answer per type rather than one per surface.
    QVariantList fieldsOf(const QString &type, const QString &id) const;

    // "shipped" | "yours" | "both" for one id — the Source column, and the thing that tells an
    // author whose content they are about to change.
    QString sourceOf(const QString &id) const;

    // The neighbourhood of a non-condition object, in the same shape layoutDag() returns.
    QVariantMap neighbourhood(const QString &type, const QString &id,
                              const QVariantMap &options) const;
    // The type's own colour key and glyph, and whatever one line a node can say about itself.
    void        decorateNode(QVariantMap &node, const QString &type, const QString &id) const;

    int  measureUsers(const QString &measureId) const;     // blast radius, as a count
    QVariantList measureUserRows(const QString &measureId) const;   // and as the actual list

    // The three columns the flat cross-type list adds — what an object is connected to, how many
    // links it has, and its status word. Per type, because "connected to" means something different
    // for a measure than for a screen, and the result list has one column for both.
    QVariantMap searchExtras(const QString &type, const QString &id) const;

    // The norm key, as the corridor table's row id. Round-trips through splitCorridorId().
    static QString corridorId(const QString &measureId, const QString &contextId);
    static bool    splitCorridorId(const QString &id, QString &measureId, QString &contextId);

    // Fetch the working norm row for a key, copying whatever currently resolves there on first
    // touch — the same copy-on-write the pack side uses, for the same reason.
    pinpoint::analysis::Norm *workingNorm(const QString &measureId, const QString &contextId);

    // The characteristics riding on one measure. Shared by usageOfMeasure (the count) and roadmap

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_core;
    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_assembled;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    pinpoint::analysis::MetricCatalogue                              m_cat;

    pinpoint::analysis::CharacteristicPack m_savedUser;
    pinpoint::analysis::CharacteristicPack m_working;

    // The user norm set, layered exactly as the pack is.
    pinpoint::analysis::NormPack m_savedNorms;
    pinpoint::analysis::NormPack m_workingNorms;

    // Screens and drills, layered exactly as the pack is — saved layer, working layer, and the
    // assembly everything reads. They are flat sets rather than providers (screen_pack.h says why),
    // so the assembly is done here instead of by a merged provider, but the shape is the same one:
    // core, plus the user's own by id, rebuilt after every mutation.
    pinpoint::analysis::ScreenSet m_savedScreens;
    pinpoint::analysis::ScreenSet m_workingScreens;
    pinpoint::analysis::ScreenSet m_screens;
    pinpoint::analysis::DrillSet  m_savedDrills;
    pinpoint::analysis::DrillSet  m_workingDrills;
    pinpoint::analysis::DrillSet  m_drills;

    // Copy-on-write for the two flat sets, mirroring workingCondition() and friends: fetch the row
    // from the working layer, copying it out of the assembly on first touch. Null only when no such
    // screen/drill exists anywhere.
    pinpoint::analysis::Screen *workingScreen(const QString &id);
    pinpoint::analysis::Drill  *workingDrill(const QString &id);

    // The corridor rows this session has explicitly RETIRED — a reset of a user override back to
    // whatever core says. Tracked rather than inferred from absence, because "no user row" is also
    // the state of every corridor nobody has touched.
    QSet<QString> m_resetCorridors;

    std::vector<Command> m_stack;
    int                  m_stackIndex = -1;   // index of the last APPLIED command
    int                  m_savedIndex = -1;   // stack position the file on disk corresponds to

    // Recomputed lazily; both are cleared by invalidateDerived() on every mutation.
    mutable QSet<QString> m_dirtyIds;
    mutable bool          m_dirtyIdsValid = false;

    // The readings behind the corridor picture. Held for ONE measure at a time: the scan is a walk
    // over the whole library, and keeping every measure's readings would be a cache nobody asked
    // for and nobody could invalidate.
    QString                          m_policyName;
    QString                          m_libraryRoot;
    QString                          m_corridorScanned;
    std::vector<double>              m_corridorValues;
    bool                             m_corridorScanning = false;
    QFutureWatcher<QVariantList>    *m_corridorWatcher  = nullptr;
    void onCorridorScanFinished();
};
