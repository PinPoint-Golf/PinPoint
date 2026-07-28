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

#include "../../Diagnostics/norm_provider.h"
#include "../../Diagnostics/pack_provider.h"
#include "../../Metrics/metric_catalogue.h"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

// NormModel — the QML façade over measures, norms and contexts, mirroring
// CharacteristicLibraryModel exactly: a QML_ELEMENT whose only job is to marshal registry value
// types into QVariant shapes. The characteristic pack, the norm set and the metric catalogue are
// assembled once in the constructor and read-only thereafter.
//
// ALL LOGIC STAYS IN C++. QML renders shapes and holds no rules — it does not walk the context
// tree, does not decide what "inherited" means, does not compute a band edge from a policy, and
// does not decide when a norm's provenance is weak. Every one of those is a statement about
// correctness, and a statement about correctness written in a delegate is a statement nothing can
// test.

class NormModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Filter vocabularies, for the chip rows.
    Q_PROPERTY(QVariantList groups   READ groups   CONSTANT)
    Q_PROPERTY(QVariantList statuses READ statuses CONSTANT)

    // The context tree in render order, each row carrying its depth. The norms-by-context list
    // indents off this — QML never derives depth by walking parents.
    //
    // NOTIFY rather than CONSTANT: a user norm set may add contexts of its own, so authoring one
    // can change this list. Every property below moves for the same reason — the corridor editor
    // writes, and a census that could not change would go stale the first time it was used.
    Q_PROPERTY(QVariantList contexts READ contexts NOTIFY normsChanged)

    // The loaded norm layers: core plus whatever the user has authored. { id, label, origin,
    // normCount, readOnly, active }.
    Q_PROPERTY(QVariantList normSets READ normSets NOTIFY normsChanged)

    // Census, for the header line.
    Q_PROPERTY(int measureCount       READ measureCount       NOTIFY normsChanged)
    Q_PROPERTY(int normCount          READ normCount          NOTIFY normsChanged)
    Q_PROPERTY(int normedMeasureCount READ normedMeasureCount NOTIFY normsChanged)
    // How many corridors in the assembled set are the user's rather than the shipped ones.
    Q_PROPERTY(int editedNormCount     READ editedNormCount     NOTIFY normsChanged)

    // The pack-wide grade policy, by NAME ("standard" | "strict" | "lenient"). A name rather than
    // three z numbers because the policy has to be one comparable thing across athletes and shared
    // packs — see GradePolicy's own comment. Bound to AppSettings by the panel that hosts it, so
    // this object stays free of the settings dependency and remains testable standalone.
    Q_PROPERTY(QString gradePolicy READ gradePolicy WRITE setGradePolicy NOTIFY gradePolicyChanged)

    // The presets themselves — { name, label, hint, idealMaxZ, goodMaxZ, watchMaxZ } — so the
    // control renders from the same table that grades.
    Q_PROPERTY(QVariantList gradePolicies READ gradePolicies CONSTANT)

public:
    explicit NormModel(QObject *parent = nullptr);
    ~NormModel() override;

    QVariantList groups() const;
    QVariantList statuses() const;
    QVariantList contexts() const;
    QVariantList normSets() const;
    int          measureCount() const;
    int          normCount() const;
    int          normedMeasureCount() const;
    int          editedNormCount() const;

    QString gradePolicy() const { return m_policyName; }
    void    setGradePolicy(const QString &name);
    QVariantList gradePolicies() const;

    // Directory list. filters: { group?, status?, hasNorm?, search? }.
    //
    // `hasNorm` is tri-state on purpose and is NOT a bool: "" = any, "yes" = has a norm somewhere
    // on its chain, "no" = has none. A measure with no norm is the interesting case — it is a
    // corridor signal that cannot fire — so filtering to it has to be reachable in one chip.
    //
    // `edited` is the same shape over "has a corridor of the user's own": "" = any, "yes" = at
    // least one row here is yours, "no" = none is. After an afternoon in the corridor editor,
    // "what did I change?" has no other answer in the app.
    //
    // Rows carry everything a row needs without a second call:
    //   { id, label, unit, group, status, statusLabel, metricKey, usedBy, highMeans,
    //     hasNorm, ownNormCount, editedNormCount, userEdited, defaultContextId,
    //     defaultContextLabel, normInherited, idealLo, idealHi, weakProvenance }
    Q_INVOKABLE QVariantList measures(const QVariantMap &filters = {}) const;

    // Full detail for the detail page. Empty map when the id is unknown, so a stale deep link
    // lands on the catalogue rather than a blank page.
    //   { …row fields…, kind, reducerLabel, seriesLabel, gapReason, viewNeeded,
    //     norms[], usedBy[], availability }
    //
    // `norms[]` is EVERY context in tree order that resolves to something, each row marked either
    // own or inherited-from — that is the whole point of the tree and it must be visible, not
    // implied. A context resolving to nothing is omitted rather than shown empty.
    Q_INVOKABLE QVariantMap measureDetail(const QString &measureId) const;

    // One resolved norm, in the measure's own units, with its band edges already computed under
    // the active grade policy:
    //   { found, contextId, contextLabel, inherited, inheritedFrom, own,
    //     mu, idealLo, idealHi, goodLo, goodHi, watchLo, watchHi, explicitMonitor,
    //     unit, n, source, sourceLabel, author, citation, setOn, weak, weakReason,
    //     overridden, hasShipped, shippedClaimLo, shippedClaimHi }
    Q_INVOKABLE QVariantMap normAt(const QString &measureId, const QString &contextId) const;

    // The metric -> measure join, marshalled for QML. `phase` is the Phase enum as an int, which
    // is how metric_catalog.cpp already passes phases across the boundary.
    //   { found, measureId, label, unit, deltaFromAddress, reducerKind }
    //
    // Stage 9 re-points metric_catalog.cpp at this join. The ALGORITHM deliberately lives in
    // characteristic_pack.h (measureForMetricAtPhase) rather than here, so C++ callers reach it
    // without going through a QML façade; this method only marshals.
    Q_INVOKABLE QVariantMap measureForMetricAtPhase(const QString &metricKey, int phase) const;

    // Re-take the shared norm provider. Called after the corridor editor writes: the provider is
    // cached process-wide, so without this the façade keeps answering from the assembly it was
    // constructed with and an edit looks like it did nothing.
    Q_INVOKABLE void refresh();

    // Switch a norm-set layer in or out of the assembly. This is what makes the norm-set strip a
    // selector rather than a census (ledger C2) — until a second set existed there was nothing to
    // switch, and until makeMergedNormProvider() could skip a layer there was no way to switch it.
    //
    // Persisted by the panel into AppSettings::diagnosticsNormSetsOff; this object stays free of
    // the settings dependency, exactly as it does for the grade policy.
    Q_INVOKABLE void setNormSetActive(const QString &normSetId, bool active);
    Q_INVOKABLE void setDisabledNormSets(const QStringList &ids);

signals:
    void gradePolicyChanged();
    void normsChanged();

private:
    // The characteristics riding on one measure — shared by the row's `usedBy` count and the
    // detail page's list. Duplicated from CharacteristicLibraryModel rather than exported from it
    // because that model owns its own provider instance; sharing the count would mean sharing the
    // pack, and two façades over one pack is a lifetime problem for no gain.
    QStringList usersOfMeasureIds(const QString &measureId) const;

    pinpoint::analysis::GradePolicy policy() const;

    // The measure's own unit, falling back to the metric catalogue's when the pack does not state
    // one. A norm carries a unit and the loader refuses a mismatch, so an empty pack-side unit is
    // the catalogue's to fill, not ours to invent.
    QString unitOf(const pinpoint::analysis::Measure &m) const;
    QString groupOf(const pinpoint::analysis::Measure &m) const;

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_pack;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    // Assembled once. makeMetricCatalogue() is cheap but not free, and groupOf()/unitOf() are
    // called per measure per query — rebuilding it inside those would turn a directory render into
    // sixty-seven catalogue builds.
    pinpoint::analysis::MetricCatalogue                              m_cat;
    QString                                                          m_policyName;
};
