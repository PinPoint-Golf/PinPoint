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

#include "../../Diagnostics/diagnostics_health.h"
#include "../../Diagnostics/norm_provider.h"
#include "../../Diagnostics/pack_provider.h"
#include "../../Metrics/metric_catalogue.h"

#include <QFutureWatcher>
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
    // NOTIFY rather than CONSTANT: the health list now spans the norm set as well as the pack, and
    // the corridor editor writes to that — so "what is wrong with this library" changes while the
    // app is running, and a census that could not move would go stale the first time it was used.
    Q_PROPERTY(int healthCount         READ healthCount NOTIFY healthChanged)

    // The corpus-share check runs over the swing library and cannot be synchronous. It is opt-in
    // (startCorpusCheck) and reports its own state, so the health list shows what it has actually
    // checked rather than implying it checked everything.
    Q_PROPERTY(bool corpusScanning     READ corpusScanning NOTIFY corpusChanged)
    Q_PROPERTY(bool corpusEverScanned  READ corpusEverScanned NOTIFY corpusChanged)
    Q_PROPERTY(int  corpusSwings       READ corpusSwings NOTIFY corpusChanged)
    // True when the scan hit its cap. Reported, never silent: a capped scan that said nothing would
    // read as "that is the whole library".
    Q_PROPERTY(bool corpusTruncated    READ corpusTruncated NOTIFY corpusChanged)
    // The library root to scan, set from appSettings.athleteLibraryPath by the panel — same seam as
    // NormEditorModel, so this object stays free of the settings dependency.
    Q_PROPERTY(QString libraryRoot     READ libraryRoot WRITE setLibraryRoot NOTIFY corpusChanged)

    // The pack-wide grade policy, by name. The corpus check counts GRADES, and for a norm with no
    // explicit monitor band the band edges are the policy — so scanning under the default while the
    // user has chosen Strict would report a share of a distribution the app does not use.
    Q_PROPERTY(QString gradePolicy     READ gradePolicy WRITE setGradePolicy NOTIFY gradePolicyChanged)

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

    // The health list. { code, subject, message, severity, measureId, contextId }.
    //
    // THREE sources, merged here because no one of them can see the whole library: the characteristic
    // pack's own validator, the norm set's, and the assembled-library checks in diagnostics_health.h
    // that span both plus the context tree and the metric catalogue. The norm side had never reached
    // this list at all — norm_provider.h says its warnings "ARE part of the health list" and they
    // were not — and the referential norm validator had never been called by anything but its test.
    //
    // `measureId` / `contextId` are filled where a row names a norm, so the view can offer to open
    // the corridor or take the shipped one instead of describing a problem the reader then has to
    // go and find.
    Q_INVOKABLE QVariantList health() const;

    // Start the corpus-share check: one pass over the swing library, grading every drawn reading
    // against every norm that resolves for it, looking for a corridor that puts almost everything in
    // one band. This is the corridor editor's histogram argument at library scale — "a corridor
    // grading almost everything Action is visibly wrong to someone who has never heard of a standard
    // deviation" — and it is the only health check that can catch a corridor which is merely WRONG
    // rather than malformed.
    Q_INVOKABLE void startCorpusCheck();

    // What that scan found, once it has run: the same row shape as health(). Empty before the first
    // run, which is why `corpusEverScanned` exists — nothing found and nothing checked must not look
    // the same.
    Q_INVOKABLE QVariantList corpusHealth() const;

    // Re-take the shared providers after a write elsewhere (the corridor editor, the characteristic
    // editor), so the health list stops answering from the assembly it was built with.
    Q_INVOKABLE void refresh();

    // "Used by N characteristics", for the blast-radius affordance before an edit.
    Q_INVOKABLE int usageOfMeasure(const QString &measureId) const;

    // Which characteristics a measure carries — the blast radius itself, not just its size. Shown
    // before editing a shared measure, because the count alone does not tell an author WHAT they
    // are about to change.
    Q_INVOKABLE QVariantList usersOfMeasure(const QString &measureId) const;

    // ── The two reference registries ────────────────────────────────────────
    //
    // Marshalled here rather than from their own façade because both are read ALONGSIDE the pack
    // and never on their own: a screen is only interesting for what it would settle, and a drill for
    // what it answers. Each row therefore carries the conditions that point at it, which is the
    // question the view is really asking.
    Q_INVOKABLE QVariantList screens() const;
    Q_INVOKABLE QVariantList drills() const;

    // The bibliography, each entry carrying WHAT IT SUPPORTS.
    //
    // A reference list on its own is an appendix nobody opens. The question a reader actually has
    // is "why does the app believe this?", and the answer is the pairing — this paper, and the four
    // causal claims resting on it, at the tier each one earned. So every row carries its citing
    // edges and conditions, and the rows sort by how much of the library they hold up.
    //
    // Entries nothing cites are kept and marked, not dropped: one of them is the paper that
    // CONTRADICTS two claims the pack does make, and a bibliography that silently omitted it would
    // be the most misleading version of this view we could ship.
    Q_INVOKABLE QVariantList references() const;

    // The glossary: every condition, its coach terms, and what it means in plain language.
    //
    // No second dataset — the rule set IS the glossary, which is the whole reason `aliases` and the
    // outcome `consequence` texts were worth authoring. `search` matches labels, aliases and the
    // meaning text, so a golfer who only knows the word they were taught can still find the page.
    Q_INVOKABLE QVariantList glossary(const QString &search = {}) const;

    // The roadmap as shareable markdown. This artefact is meant to leave the app — it is what
    // prioritises pipeline work — so it is generated whole rather than scraped off the view.
    Q_INVOKABLE QString roadmapMarkdown() const;

    // Write that markdown to the user's Documents folder. Returns { ok, path, message }; a failed
    // write reports rather than throwing, so it reaches the user instead of a log.
    Q_INVOKABLE QVariantMap exportRoadmap() const;

    // The whole bibliography as CSL-JSON, written to the user's Documents folder. Same
    // { ok, path, message } contract as exportRoadmap(), for the same reason.
    //
    // CSL-JSON because it is what Zotero imports and what pandoc consumes, so a coach who wants
    // these sources in their own library, or a contributor writing them up, gets every citation
    // style for free rather than us picking one. Cited and general-reading records alike — a
    // filtered export is a UI question and there is no UI asking it.
    Q_INVOKABLE QVariantMap exportReferences() const;

    bool    corpusScanning() const { return m_corpusScanning; }
    bool    corpusEverScanned() const { return m_corpusEverScanned; }
    int     corpusSwings() const { return m_corpusSwings; }
    bool    corpusTruncated() const { return m_corpusTruncated; }
    QString libraryRoot() const { return m_libraryRoot; }
    void    setLibraryRoot(const QString &root);
    QString gradePolicy() const { return m_policyName; }
    void    setGradePolicy(const QString &name);

signals:
    void healthChanged();
    void corpusChanged();
    void gradePolicyChanged();

private:
    // The characteristics riding on one measure. Shared by usageOfMeasure (the count) and roadmap
    // (which must union them across a series' reducers without double-counting).
    QStringList usersOfMeasureIds(const QString &measureId) const;

    void onCorpusFinished();

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_provider;
    // The norm set and the metric catalogue: the health checks span all three registries, so the
    // façade holds all three. Assembled once, re-taken by refresh().
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    pinpoint::analysis::MetricCatalogue                              m_cat;

    QString m_libraryRoot;
    QString m_policyName;
    bool    m_corpusScanning    = false;
    bool    m_corpusEverScanned = false;
    int     m_corpusSwings      = 0;
    bool    m_corpusTruncated   = false;
    std::vector<pinpoint::analysis::CorpusGradeCounts> m_corpusCounts;
    QFutureWatcher<QVariantList> *m_corpusWatcher = nullptr;
};
