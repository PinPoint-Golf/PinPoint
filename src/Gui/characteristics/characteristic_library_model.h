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

// CharacteristicLibraryModel — the QML façade over the assembled characteristic pack, mirroring
// src/Gui/review/metric_catalog.h exactly: a QML_ELEMENT whose only job is to marshal registry
// value types into QVariant shapes. The pack is assembled once in the constructor
// (makeCharacteristicPackProvider) and read-only thereafter.
//
// All logic stays in C++ per the project's QML rules — QML reads shapes and renders them, it never
// walks the graph, ranks anything, or decides what is a capture gap.

class CharacteristicLibraryModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Filter vocabularies, for the chip rows.
    Q_PROPERTY(QVariantList groups READ groups CONSTANT)
    Q_PROPERTY(QVariantList states READ states CONSTANT)

    // Census, for the header line.
    Q_PROPERTY(int characteristicCount READ characteristicCount CONSTANT)
    Q_PROPERTY(int causeCount          READ causeCount CONSTANT)
    Q_PROPERTY(int edgeCount           READ edgeCount CONSTANT)
    Q_PROPERTY(int healthCount         READ healthCount CONSTANT)

public:
    explicit CharacteristicLibraryModel(QObject *parent = nullptr);
    ~CharacteristicLibraryModel() override;

    QVariantList groups() const;
    QVariantList states() const;
    int          characteristicCount() const;
    int          causeCount() const;
    int          edgeCount() const;
    int          healthCount() const;

    // Directory list. filters: { group?, state?, reach?, hideProposed?, observableOnly? }.
    // Rows carry everything a row needs to render without a second call:
    //   { id, label, group, groupLabel, axis, axisPartner, reach, reachLabel, reachHint,
    //     tier, tierLabel, resolvability, resolvabilityLabel, measureCount, causeCount,
    //     effectCount, isCharacteristic }
    Q_INVOKABLE QVariantList query(const QVariantMap &filters = {}) const;

    // Full detail for the detail page. Empty map when the id is unknown, so a stale deep link
    // lands on the directory rather than a blank page.
    //   { …row fields…, consequence, injuryNote, screenRef, signals[], measures[],
    //     causes[], effects[], bindings[], citation, state }
    Q_INVOKABLE QVariantMap detail(const QString &conditionId) const;

    // The navigable causal DAG around one condition, already laid out — see dag_layout.h. QML
    // renders `nodes` and `edges` and positions nothing; every coordinate, rank and encoding flag in
    // here was decided in C++, where it can be tested.
    //
    // `options` carries the theme's own metrics (nodeH, gapX, gapY, laneGap, padX, charW, minW,
    // maxW, depth, maxPerRank, includeMeasures); anything omitted keeps the DagLayoutOptions
    // default. Returns { nodes[], edges[], width, height, focusX, focusY, truncated }, and an empty
    // node list for an unknown id.
    Q_INVOKABLE QVariantMap dag(const QString     &conditionId,
                                const QVariantMap &options = {}) const;

    // Measures with no producer, ranked by how many conditions they block. EXCLUDES capture gaps —
    // listing something no sensor can ever resolve as missing pipeline work would corrupt the
    // roadmap's meaning for every other row (see captureGaps()).
    Q_INVOKABLE QVariantList roadmap() const;

    // Measures no sensor this product has can resolve, with the reason. Shown under their own
    // heading, never in the roadmap.
    Q_INVOKABLE QVariantList captureGaps() const;

    // Causes ranked by how many characteristics they explain, split by reach. The screened block is
    // the highest-value list in the product: it tells a coach which three screens to run, and it
    // needs no capture hardware at all.
    Q_INVOKABLE QVariantList causeCoverage() const;

    // The health list — the validator's warnings, which is exactly what "what is wrong with this
    // library" means. { code, subject, message }.
    Q_INVOKABLE QVariantList health() const;

    // "Used by N characteristics", for the blast-radius affordance before an edit.
    Q_INVOKABLE int usageOfMeasure(const QString &measureId) const;

    // Which characteristics a measure carries — the blast radius itself, not just its size. Shown
    // before editing a shared measure, because the count alone does not tell an author WHAT they
    // are about to change.
    Q_INVOKABLE QVariantList usersOfMeasure(const QString &measureId) const;

    // The roadmap as shareable markdown. This artefact is meant to leave the app — it is what
    // prioritises pipeline work — so it is generated whole rather than scraped off the view.
    Q_INVOKABLE QString roadmapMarkdown() const;

    // Write that markdown to the user's Documents folder. Returns { ok, path, message }; a failed
    // write reports rather than throwing, so it reaches the user instead of a log.
    Q_INVOKABLE QVariantMap exportRoadmap() const;

private:
    // The characteristics riding on one measure. Shared by usageOfMeasure (the count) and roadmap
    // (which must union them across a series' reducers without double-counting).
    QStringList usersOfMeasureIds(const QString &measureId) const;

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_provider;
};
