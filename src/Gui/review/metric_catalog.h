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
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>

// MetricCatalog — the QML façade over pinpoint::analysis::MetricCatalogue (design §7), the same
// shape as ChartMetrics / SwingDataSource: a QML_ELEMENT declared `MetricCatalog { id: mc }` whose
// only job is to marshal registry value types into QVariant shapes. The catalogue is assembled once
// in the constructor (makeMetricCatalogue) and read-only thereafter.
//
// A `shotCtx` QVariantMap parameterises per-shot availability + normative resolution. Shape (all
// optional): { tier:int, sessionType:int, imuRoles:[roleName…], hasFaceOn, hasClubTrack,
// hasBallTrack, archetype, club, shape }. Omit it (or pass {}) for the context-free directory view;
// pass a shot's capability (or the studio's configured capability) to drive availability chips.
// Phases are emitted as Phase ints (the vocabulary QML already compares and TimelineLabels renders).

class MetricCatalog : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QVariantList groups READ groups CONSTANT)   // distinct groups, manifest order
    Q_PROPERTY(QVariantList types  READ types  CONSTANT)   // ["summary","pointInTime",…]

    // The pack-wide grade policy, by NAME ("standard" | "strict" | "lenient") — the same property
    // NormModel exposes, for the same reason: a corridor's Watch edge is z-derived for any norm
    // that states no monitor band (stance width and ball position, for example), so a façade
    // grading against the default while the user has chosen Strict would
    // draw a corridor the app does not use. Bound to AppSettings by whichever page hosts this
    // object, so the façade itself stays free of the settings dependency.
    Q_PROPERTY(QString gradePolicy READ gradePolicy WRITE setGradePolicy NOTIFY gradePolicyChanged)

public:
    explicit MetricCatalog(QObject *parent = nullptr);
    ~MetricCatalog() override;

    QString gradePolicy() const { return m_policyName; }
    void    setGradePolicy(const QString &name);

    QVariantList groups() const;
    QVariantList types() const;

    // Directory list. filters: { type?, group?, scored?, availableOnly?, sessionType? }.
    // With availableOnly + a non-empty shotCtx, Unavailable metrics are dropped.
    Q_INVOKABLE QVariantList query(const QVariantMap &filters,
                                   const QVariantMap &shotCtx = {}) const;

    // Full detail for the detail page (description / howToRead / normative corridors / requirement /
    // availability / usedBy). Empty map when the key is unknown.
    Q_INVOKABLE QVariantMap descriptor(const QString &key,
                                       const QVariantMap &shotCtx = {}) const;

    // Availability alone: { state, reason, tier } — e.g. a zone deciding whether to render a metric.
    Q_INVOKABLE QVariantMap availability(const QString &key,
                                         const QVariantMap &shotCtx) const;

signals:
    void gradePolicyChanged();

private:
    pinpoint::analysis::MetricCatalogue m_catalogue;
    // The pack and the norm set: a corridor is now (metric, phase) → measure → norm, so the façade
    // needs both registries. Assembled once and read-only thereafter, same as the catalogue.
    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_pack;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    QString                                                          m_policyName;
};
