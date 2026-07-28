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

#include "../../Diagnostics/norm_provider.h"   // the context tree lives with the norms
#include "../../Diagnostics/pack_provider.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

// CharacteristicEditorModel — authoring state for one characteristic, plus the faceted measure
// picker behind it. All the logic lives here; the QML holds no rules.
//
// Edits are written to the user's own pack (QStandardPaths::AppDataLocation/diagnostics/user.json),
// which OVERRIDES the shipped core pack entry of the same id. The shipped pack itself is never
// modified, so a user's changes survive an app update and can always be reverted by deleting the
// override.

class CharacteristicEditorModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool    editing READ editing NOTIFY draftChanged)
    Q_PROPERTY(bool    dirty   READ dirty   NOTIFY draftChanged)
    Q_PROPERTY(bool    isNew   READ isNew   NOTIFY draftChanged)
    // Does CORE ship a characteristic with this id? Not "is this an override" — the two came
    // apart the moment a user could create a characteristic of their own, and conflating them made
    // revertToShipped() DELETE such a characteristic while reporting that it had restored the
    // shipped definition.
    Q_PROPERTY(bool    shippedExists   READ shippedExists   NOTIFY draftChanged)
    // Is a row of the USER's own in play for this id? True once they have saved an edit — whether
    // it overrides a shipped characteristic or is one they wrote themselves.
    Q_PROPERTY(bool    hasUserOverride READ hasUserOverride NOTIFY draftChanged)
    Q_PROPERTY(QVariantMap draft READ draft NOTIFY draftChanged)

    // The context tree in render order, each row carrying its depth AND how this characteristic's
    // bindings resolve there. NOTIFY draftChanged because the second half moves with every edit.
    //
    // Rows are { id, label, parentId, depth, isDefault, applicable, material, own, inherited,
    //            inheritedFrom, inheritedFromLabel }. `own` and `inherited` are the same
    //            distinction the norms-by-context list draws, and for the same reason: a checkbox
    //            that cannot say whether it is stating something or repeating its parent teaches
    //            the author that every row is an assertion, which is the opposite of the design.
    Q_PROPERTY(QVariantList contexts READ contexts NOTIFY draftChanged)

    // Vocabulary for the picker chips — grouped so `what` is never a free text field.
    Q_PROPERTY(QVariantList anatomyGroups READ anatomyGroups CONSTANT)
    Q_PROPERTY(QVariantList phases        READ phases        CONSTANT)
    Q_PROPERTY(QVariantList reducerKinds  READ reducerKinds  CONSTANT)
    Q_PROPERTY(QVariantList conditionGroups READ conditionGroups CONSTANT)

public:
    explicit CharacteristicEditorModel(QObject *parent = nullptr);
    ~CharacteristicEditorModel() override;

    bool        editing() const { return m_editing; }
    bool        dirty() const { return m_dirty; }
    bool        isNew() const { return m_isNew; }
    bool        shippedExists() const { return m_shippedExists; }
    bool        hasUserOverride() const { return m_hasUserOverride; }
    QVariantMap draft() const;

    QVariantList contexts() const;
    QVariantList anatomyGroups() const;
    QVariantList phases() const;
    QVariantList reducerKinds() const;
    QVariantList conditionGroups() const;

    // ── Lifecycle ───────────────────────────────────────────────────────────
    Q_INVOKABLE bool beginEdit(const QString &conditionId);
    Q_INVOKABLE void beginNew();
    Q_INVOKABLE void discard();
    // Returns { ok, message }. Never throws: a failed save has to reach the user, not a log.
    Q_INVOKABLE QVariantMap save();
    // Drop this condition's row from the USER pack. The shipped pack is never edited, so what
    // happens next depends on whether core ships this id at all:
    //
    //   shippedExists  -> the shipped definition is restored   (recoverable, reassuring)
    //   !shippedExists -> the characteristic is DELETED         (destructive, and must say so)
    //
    // Both were previously labelled "Restore shipped version" and both reported "Restored the
    // shipped definition", which meant deleting somebody's own characteristic behind a message
    // promising the opposite.
    Q_INVOKABLE QVariantMap revertToShipped();

    // ── Field edits ─────────────────────────────────────────────────────────
    Q_INVOKABLE void setLabel(const QString &v);
    Q_INVOKABLE void setGroup(const QString &groupName);
    // The coach terms this characteristic answers to. Taken as one comma-separated line rather than
    // a list editor: an author types "flip, flipping, breakdown through impact" in one go, and a
    // per-row add/remove UI would make the common case the slow one. Trimmed, de-duplicated and
    // emptied-out here so the draft never carries a blank term into the duplicate lint.
    Q_INVOKABLE void setAliases(const QString &commaSeparated);
    Q_INVOKABLE void setConsequence(const QString &v);
    Q_INVOKABLE void setInjuryNote(const QString &v);
    Q_INVOKABLE void setCitation(const QString &v);
    Q_INVOKABLE void setState(const QString &stateName);

    // ── Where it applies (context bindings) ─────────────────────────────────
    //
    // A binding is an EXCEPTION. Writing one at `partial` covers pitch and chip beneath it, and a
    // context with no row anywhere on its chain applies — see resolveContextBinding().
    //
    // Both return { ok, message, cascaded, canUndo } rather than void: switching a parent off has
    // to clear any descendant row that says otherwise, or the untick would silently not take, and
    // an action that quietly changes rows the user cannot see needs to say so AND be undoable in
    // the same breath. (The plan specified `void`; a result the toast can render is the same
    // operation with the consequence attached.)
    Q_INVOKABLE QVariantMap setBinding(const QString &contextId, bool applicable, bool material);
    Q_INVOKABLE QVariantMap clearBinding(const QString &contextId);

    // Restore the binding set as it stood before the last setBinding/clearBinding. One level deep
    // and deliberately so: this backs the toast's UNDO, which is about the change just made.
    Q_INVOKABLE bool undoBindingChange();

    // ── Signals (the "flag X when …" clause) ────────────────────────────────
    // Attaches a measure at a direction, minting the signal. Returns the signal id.
    Q_INVOKABLE QString attachMeasure(const QString &measureId, const QString &direction);
    Q_INVOKABLE void    detachSignal(const QString &signalId);

    // The two tails, phrased in the MEASURE's own words rather than as High and Low. Rows are
    // { name, label, means, sentence }. Three signals shipped inverted because an author chose a
    // tail against a sign convention that was unstated or the opposite of what they assumed; the
    // fix is that the control says "further back, toward the trail foot" where it used to say
    // "Too much". With no `highMeans` to work from it falls back to Too much / Too little and the
    // caller is expected to ask for the missing sentence — see setMeasureHighMeans().
    // Rows are { name, label, means, sentence, enabled, reason }. `enabled` is false for a tail the
    // measure's SHAPE leaves ungraded — a signal there can never fire, whatever the swing does —
    // and `reason` says which, because a greyed option with no explanation is indistinguishable
    // from a rendering fault (the doctrine DagView states for its own disabled boxes). Pass
    // `measureId` where one exists; without it every tail is offered, which is right for a measure
    // that does not exist yet and is what this returned before shapes.
    Q_INVOKABLE QVariantList directionOptions(const QString &highMeans,
                                              const QString &measureId = QString()) const;

    // Change which tail fires, after the fact. Attaching a measure was the only way to set this
    // until 2026-07-26, which meant correcting an inverted signal required deleting it and adding
    // it back — and adding it back at the other tail did not replace anything, because the minted
    // id spells the direction out, so the characteristic quietly ended up flagging BOTH sides.
    // Returns { ok, message } with the new tail stated in the measure's own words.
    Q_INVOKABLE QVariantMap setSignalDirection(const QString &signalId, const QString &direction);

    // What a HIGH value of this measure means, authored where the direction is chosen. Writes to
    // the draft's copy of the measure, so a save carries it into the user pack as an override of a
    // shared measure — which is exactly what it is.
    Q_INVOKABLE void    setMeasureHighMeans(const QString &measureId, const QString &text);
    Q_INVOKABLE QString measureHighMeans(const QString &measureId) const;

    // ── Causes (the "usually caused by …" clause) ───────────────────────────
    // Causes ARE conditions, so this is a reuse picker over the same library.
    Q_INVOKABLE void         addCause(const QString &causeId, const QString &strength);
    Q_INVOKABLE void         removeCause(const QString &causeId);
    Q_INVOKABLE QVariantList candidateCauses(const QString &search = QString()) const;

    // ── One link at a time, from the graph ──────────────────────────────────
    //
    // The three above edit a DRAFT and take effect on save. The DAG on the detail page has no draft
    // — it is a read-only surface with one editing affordance — so these two do the whole cycle
    // (load the effect, change one edge, write) and report what happened. They refuse while a draft
    // is open rather than racing it: two unsynchronised edit paths onto one condition is how the
    // later save silently discards the earlier one.
    //
    // Both return { ok, message }. The refusals are the point: a self-edge, a link that would make
    // the graph cyclic, and a pair that already corroborates each other (the validator forbids
    // Corroborates alongside a causal path, so adding the edge would break the whole library rather
    // than just this row). Each is refused BEFORE anything is written, with the reason in the
    // message — a graph edit that half-lands is not recoverable by a reader.
    Q_INVOKABLE QVariantMap linkCause(const QString &causeId, const QString &effectId,
                                      const QString &strength = QStringLiteral("moderate"));
    Q_INVOKABLE QVariantMap unlinkCause(const QString &causeId, const QString &effectId);

    // Put back the link `unlinkCause()` just removed, strength and all — ledger C31. A recoverable
    // removal offers an undo in the same breath (the binding cascade sets the precedent); "the
    // inverse is one long-press away" is not an undo, and re-linking by hand would not restore the
    // strength, which is the part a reader cannot reconstruct. One level, consumed on use: the
    // removal it reverses is a whole load-edit-write cycle, and a stack of those would be a claim
    // about history the file cannot back up.
    Q_INVOKABLE QVariantMap undoUnlinkCause();

    // How often this cause produces this effect — sometimes / often / usually.
    //
    // Separate from `linkCause`, which refuses a pair that is already linked: re-stating an existing
    // edge to change one field would have to defeat that refusal, and a call that both creates and
    // silently overwrites is one an author cannot predict from its name.
    //
    // The strength is a RANKING WEIGHT and never a probability — `strengthLabel()` is words for that
    // reason, and nothing may render it as a percentage.
    Q_INVOKABLE QVariantMap setCauseStrength(const QString &causeId, const QString &effectId,
                                             const QString &strength);

    // ── The non-causal relations ────────────────────────────────────────────
    //
    // Corroborates and Excludes do NOT go through the draft, and that is a structural fact rather
    // than a shortcut. `beginEdit()` models "the effect owns its incoming causes" — it loads causal
    // edges only, and `save()` replaces that whole set. A symmetric edge belongs to neither end, so
    // whichever end held it would have its identity rewritten by the other end's next save.
    //
    // These four therefore own their own load-edit-write cycle against the user pack. Every refusal
    // is made BEFORE anything is written, in the author's own terms, because the alternative is the
    // assembled-library validator failing every characteristic over one bad edge.
    Q_INVOKABLE QVariantMap linkRelation(const QString &aId, const QString &bId,
                                         const QString &typeName,
                                         const QString &strength = QStringLiteral("moderate"));
    Q_INVOKABLE QVariantMap unlinkRelation(const QString &aId, const QString &bId,
                                           const QString &typeName);
    // Change what an existing relation SAYS: its type (corroborates ↔ excludes) or, for a
    // corroboration, how strongly. One call, because both are edits to one row and doing it as
    // remove-then-add would leave the graph briefly invalid and the undo pointing at nothing.
    Q_INVOKABLE QVariantMap editRelation(const QString &aId, const QString &bId,
                                         const QString &fromTypeName, const QString &toTypeName,
                                         const QString &strength = QString());
    Q_INVOKABLE QVariantMap undoUnlinkRelation();

    // Conditions this one could legally be related to, with the illegal ones EXCLUDED rather than
    // listed and refused on tap. A picker that offers a choice it will reject is a worse control
    // than one that does not offer it.
    Q_INVOKABLE QVariantList relationCandidates(const QString &focusId, const QString &typeName,
                                                const QString &search = {}) const;

    // The relations a condition already has, for the surface that edits them.
    Q_INVOKABLE QVariantList relationsOf(const QString &conditionId) const;

    // ── The measure picker ──────────────────────────────────────────────────
    // A typed phrase SEEDS facet selections; it is not a query. Wrong guesses are corrected by
    // tapping chips, which is far easier than rephrasing a search that returned nothing.
    Q_INVOKABLE QVariantMap  seedFacetsFromPhrase(const QString &phrase) const;

    // Chip rows, gated by the validity table so the picker never offers something it will reject.
    Q_INVOKABLE QVariantList quantitiesFor(const QString &whatRole) const;
    Q_INVOKABLE QVariantList referencesFor(const QString &whatRole, const QString &quantity) const;

    // Live preview of a candidate measure:
    //   { valid, reason, label, id, viewNeeded, status, statusLabel, gapReason,
    //     exactMatch:{…}|null, nearDuplicates:[…] }
    // `nearDuplicates` is the defence against a library filling with almost-identical measures, and
    // it is computed on the SERIES tuple so it fires even when reducers differ.
    Q_INVOKABLE QVariantMap previewMeasure(const QVariantMap &facets) const;

    // Create the measure, register it, and return its id. Never blocks the author: a measure with
    // no producer is a legitimate, expected outcome — it is the roadmap's input.
    Q_INVOKABLE QString mintMeasure(const QVariantMap &facets);

    // Measures already in the library, for reuse-first picking.
    Q_INVOKABLE QVariantList existingMeasures(const QString &search = QString()) const;

signals:
    void draftChanged();
    void libraryChanged();   // a save landed; the library view should re-query

private:
    void   reload();
    void   touch();
    QString mintConditionId(const QString &label) const;

    // The edge unlinkCause() removed, for undoUnlinkCause(). One level, and NOT part of the draft:
    // the removal is committed to the file, so this outlives any open edit.
    pinpoint::analysis::Edge m_edgeUndo;
    bool                     m_edgeUndoValid = false;

    // A separate slot from the causal one. They are offered by different toasts on different
    // surfaces, and sharing one would let "put the link back" restore the wrong kind of link —
    // silently, since both are edges and both would apply cleanly.
    pinpoint::analysis::Edge m_relationUndo;
    bool                     m_relationUndoValid = false;

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_provider;
    // Read-only, and only ever asked for its context tree. The shared instance because the corridor
    // editor may reset it after a norm write and this view must then see the same tree, not the one
    // it happened to assemble at construction.
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    // Core alone, so "does this ship?" can be answered without inferring it from the user pack.
    // Assembled once: it is read-only and never changes for the life of the process.
    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_core;
    pinpoint::analysis::CharacteristicPack m_userPack;   // the override file, as loaded/edited

    pinpoint::analysis::Condition            m_draft;
    std::vector<pinpoint::analysis::Signal>  m_draftSignals;
    std::vector<pinpoint::analysis::Measure> m_draftMeasures;
    std::vector<pinpoint::analysis::Edge>    m_draftEdges;   // causes of the draft

    // One level of undo over the binding set, held as the whole vector rather than as a diff: the
    // cascade touches rows the user never named, and restoring "what it was" is the only promise
    // that stays true however many of them there were.
    std::vector<pinpoint::analysis::ContextBinding> m_bindingUndo;
    bool m_bindingUndoValid = false;

    // Signal ids this draft re-minted (a tail was flipped). Dropped from the user pack on save when
    // nothing references them — otherwise every flip would leave a dead row behind, and the
    // validator's unused-signal warning would fill up with the user's own history.
    QStringList m_retiredSignalIds;

    bool m_editing       = false;
    bool m_dirty         = false;
    bool m_isNew         = false;
    bool m_shippedExists   = false;
    bool m_hasUserOverride = false;
};
