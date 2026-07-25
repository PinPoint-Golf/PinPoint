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
    Q_PROPERTY(bool    overridesCore READ overridesCore NOTIFY draftChanged)
    Q_PROPERTY(QVariantMap draft READ draft NOTIFY draftChanged)

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
    bool        overridesCore() const { return m_overridesCore; }
    QVariantMap draft() const;

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
    // Remove this condition's override, restoring the shipped definition. Only meaningful when
    // overridesCore is true — the shipped pack is never edited, so reverting is always possible.
    Q_INVOKABLE QVariantMap revertToShipped();

    // ── Field edits ─────────────────────────────────────────────────────────
    Q_INVOKABLE void setLabel(const QString &v);
    Q_INVOKABLE void setGroup(const QString &groupName);
    Q_INVOKABLE void setConsequence(const QString &v);
    Q_INVOKABLE void setInjuryNote(const QString &v);
    Q_INVOKABLE void setCitation(const QString &v);
    Q_INVOKABLE void setState(const QString &stateName);

    // ── Signals (the "flag X when …" clause) ────────────────────────────────
    // Attaches a measure at a direction, minting the signal. Returns the signal id.
    Q_INVOKABLE QString attachMeasure(const QString &measureId, const QString &direction);
    Q_INVOKABLE void    detachSignal(const QString &signalId);

    // ── Causes (the "usually caused by …" clause) ───────────────────────────
    // Causes ARE conditions, so this is a reuse picker over the same library.
    Q_INVOKABLE void         addCause(const QString &causeId, const QString &strength);
    Q_INVOKABLE void         removeCause(const QString &causeId);
    Q_INVOKABLE QVariantList candidateCauses(const QString &search = QString()) const;

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

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_provider;
    pinpoint::analysis::CharacteristicPack m_userPack;   // the override file, as loaded/edited

    pinpoint::analysis::Condition            m_draft;
    std::vector<pinpoint::analysis::Signal>  m_draftSignals;
    std::vector<pinpoint::analysis::Measure> m_draftMeasures;
    std::vector<pinpoint::analysis::Edge>    m_draftEdges;   // causes of the draft

    bool m_editing       = false;
    bool m_dirty         = false;
    bool m_isNew         = false;
    bool m_overridesCore = false;
};
