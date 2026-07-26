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

#include "../../Diagnostics/measure_sample.h"
#include "../../Diagnostics/norm_provider.h"
#include "../../Diagnostics/pack_provider.h"
#include "../../Metrics/metric_catalogue.h"

#include <QFutureWatcher>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

// NormEditorModel — the draft norm behind CorridorEditor.qml, and the swings drawn beneath it.
//
// It mirrors CharacteristicEditorModel's shape exactly: begin() opens a draft, mutators change it,
// save() commits, and QML never holds a rule. Everything that decides anything lives here —
// what the band edges are under the active grade policy, which swings the sample includes, how a
// value grades, whether a draft can be saved and why not.
//
// ── The histogram is the safety mechanism ──────────────────────────────────────────────────────
//
// "A corridor grading almost everything Action is visibly wrong to someone who has never heard of a
// standard deviation" — that is the whole reason the drawn-from sample is not optional. It is also
// not decoration for the editor: running the shipped seed corridors over a real library is how a
// unit disagreement between a corridor and its producer becomes visible at all, because both sides
// declare the same unit string and the validator cannot tell them apart.
//
// ── Population, not personal ───────────────────────────────────────────────────────────────────
//
// Seating from a drawn sample records `n` and the sample's scope in provenance, but the resulting
// norm still applies to EVERYONE using that norm set. The draw-from selector picks the sample, not
// the scope. Nothing here is ever keyed by athlete — see norm.h.

class NormEditorModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Is a draft open at all. Everything below is meaningless when false.
    Q_PROPERTY(bool open READ isOpen NOTIFY draftChanged)

    // The draft, flattened for rendering:
    //   { measureId, measureLabel, unit, highMeans, contextId, contextLabel,
    //     route, mu, idealLo, idealHi, goodLo, goodHi, watchLo, watchHi,
    //     n, source, sourceLabel, author, citation, setOn, weak, weakReason,
    //     own, inherited, inheritedFrom, parentIdealLo, parentIdealHi,
    //     dirty, canSave, whyNot, refused, refusedReason }
    Q_PROPERTY(QVariantMap draft READ draft NOTIFY draftChanged)

    // Which authoring route the segmented control is on: "hand" | "seat" | "import".
    Q_PROPERTY(QString route READ route WRITE setRoute NOTIFY draftChanged)

    // Which swings the sample is drawn from: "all" | "athlete" | "session". Selects the SAMPLE,
    // never the scope of the norm.
    Q_PROPERTY(QString drawFrom READ drawFrom WRITE setDrawFrom NOTIFY sampleChanged)
    Q_PROPERTY(QVariantList drawFromOptions READ drawFromOptions NOTIFY sampleChanged)

    // The library to scan. Set from appSettings.athleteLibraryPath by the panel, so this object
    // keeps no settings dependency and stays testable standalone.
    Q_PROPERTY(QString libraryRoot READ libraryRoot WRITE setLibraryRoot NOTIFY sampleChanged)

    // The drawn sample. One row per swing:
    //   { swingDir, label, session, club, value, grade, gradeLabel, included, ordinal }
    Q_PROPERTY(QVariantList samples READ samples NOTIFY sampleChanged)

    // { scanned, produced, included, min, max, median, unit, scanning, everScanned }
    // `produced` and `scanned` differ on purpose: a library of 72 swings where 8 carry the metric
    // is a different situation from one where 72 do, and collapsing them hides which.
    Q_PROPERTY(QVariantMap sampleSummary READ sampleSummary NOTIFY sampleChanged)

    // Bins over the included values: { lo, hi, count } — plus, per bin, the grade its centre takes
    // under the CURRENT draft, so the bars recolour live as a handle moves.
    Q_PROPERTY(QVariantList histogram READ histogram NOTIFY sampleChanged)

    // { ideal, good, watch, action, total } under the current draft. This is the running
    // "31 Ideal · 8 Watch · 3 Action" line.
    Q_PROPERTY(QVariantMap gradeCounts READ gradeCounts NOTIFY sampleChanged)

    Q_PROPERTY(bool scanning READ scanning NOTIFY sampleChanged)

    // Rows this draft could adopt from — every other context that carries a norm for this measure.
    // { contextId, contextLabel, mu, idealLo, idealHi, sourceLabel, n }
    Q_PROPERTY(QVariantList importCandidates READ importCandidates NOTIFY draftChanged)

public:
    explicit NormEditorModel(QObject *parent = nullptr);
    ~NormEditorModel() override;

    bool         isOpen() const { return m_open; }
    QVariantMap  draft() const;
    QString      route() const { return m_route; }
    QString      drawFrom() const { return m_drawFrom; }
    QVariantList drawFromOptions() const;
    QString      libraryRoot() const { return m_libraryRoot; }
    QVariantList samples() const;
    QVariantMap  sampleSummary() const;
    QVariantList histogram() const;
    QVariantMap  gradeCounts() const;
    bool         scanning() const { return m_scanning; }
    QVariantList importCandidates() const;

    void setRoute(const QString &r);
    void setDrawFrom(const QString &d);
    void setLibraryRoot(const QString &path);

    // The pack-wide grade policy, by name, exactly as NormModel takes it. The band edges DRAWN must
    // be the band edges that grade, so the editor cannot hold its own idea of the policy.
    Q_INVOKABLE void setGradePolicy(const QString &name);

    // Open a draft for (measureId, contextId). Returns false when the measure is unknown or cannot
    // hold a norm at all — a NotCapturable measure is refused rather than given an editor that
    // produces a corridor no sensor can ever feed. `draft.refused` carries the reason for display.
    Q_INVOKABLE bool begin(const QString &measureId, const QString &contextId);
    Q_INVOKABLE void cancel();

    // ── Set by hand ─────────────────────────────────────────────────────────
    // The two handles bind the IDEAL band: centre becomes mu, each half-width becomes sigmaLo /
    // sigmaHi. Dragging them apart asymmetrically produces an asymmetric tolerance with no
    // statistics vocabulary anywhere in the interaction. See norm.h — idealLo()/idealHi() are
    // documented as exactly what these bind to.
    Q_INVOKABLE void setIdealBand(double lo, double hi);
    Q_INVOKABLE void nudgeIdealLo(double to);
    Q_INVOKABLE void nudgeIdealHi(double to);

    // ── The axis must not move while a handle is being dragged ──────────────
    //
    // NOT a view detail — a correctness rule, and the defect it prevents is violent. The axis is
    // derived from the corridor (mu +/- watchMaxZ*sigma unioned with the data), so without this:
    //
    //     drag right -> the value under the cursor rises -> sigmaHi grows
    //                -> axisHi = mu + 3*sigmaHi grows, widening the SPAN by ~3x that
    //                -> the same pixel now maps to a larger value -> repeat, ~60x a second
    //
    // A gain of roughly 3 per mouse-move event, so the corridor diverges to absurd numbers before
    // the pointer has travelled a centimetre. Latching the axis for the duration of the gesture
    // breaks the loop and makes the drag 1:1 — which is also simply what a number line under your
    // finger should do.
    Q_INVOKABLE void beginHandleDrag();
    Q_INVOKABLE void endHandleDrag();

    // ── Seat from swings ────────────────────────────────────────────────────
    // Fit mu and both tolerances from the INCLUDED samples, set n, and mark the source Seated.
    // Refused with a reason when the sample is too small to mean anything.
    Q_INVOKABLE QVariantMap seatFromSample();
    Q_INVOKABLE void setSampleIncluded(const QString &swingDir, bool on);
    Q_INVOKABLE void setAllIncluded(bool on);
    Q_INVOKABLE void rescan();

    // ── Import ──────────────────────────────────────────────────────────────
    Q_INVOKABLE void adoptFrom(const QString &contextId);

    // ── Commit ──────────────────────────────────────────────────────────────
    // { ok, message }. A save writes the USER norm set and resets the shared provider, so the edit
    // is visible everywhere immediately rather than at the next launch.
    Q_INVOKABLE QVariantMap save();

    // Throw away UNSAVED changes and re-seed from what currently resolves. Nothing is written and
    // nothing on disk moves — this is the "I was just poking at it" undo, and it is deliberately a
    // different action from resetToDefault() below, which is a write.
    Q_INVOKABLE QVariantMap discardChanges();

    // Drop this measure's own row for this context from the USER layer. The shipped set is never
    // edited in place, so this always means "stop overriding" — but what you get afterwards
    // depends on whether core carries a row at this exact key:
    //
    //   core has a row here  -> the SHIPPED corridor grades again  ("Reset to shipped")
    //   core has none        -> the PARENT context's corridor does ("Remove override")
    //
    // One operation, two honest promises. `draft.resetLabel` carries which one applies, so the
    // button never offers to restore something that was never shipped — that mislabelling is
    // exactly how a destructive action acquires a reassuring name.
    Q_INVOKABLE QVariantMap resetToDefault();

signals:
    void draftChanged();
    void sampleChanged();
    // A save landed. The library re-queries off this rather than polling.
    void normsChanged();

private:
    struct Sample {
        QString swingDir;
        QString athlete;              // the athlete FOLDER, which is what "this athlete" filters on
        QString session;
        QString club;
        int     ordinal = 0;
        qint64  wallclockMs = 0;
        double  value    = 0.0;
        bool    included = true;
    };

    void reload();                     // re-take the shared provider after a write
    void startScan();
    void onScanFinished();
    void recomputeDerived();

    pinpoint::analysis::GradePolicy policy() const;
    QString                         contextLabel(const QString &id) const;
    const pinpoint::analysis::Measure *measure() const;
    QString                         unitOf() const;

    // Samples that pass the draw-from filter, in sample order. The filter is applied HERE and not
    // in QML, so "included" means one thing everywhere it is counted.
    std::vector<const Sample *> visible() const;
    bool                        passesScope(const Sample &s, const QString &scope) const;

    std::unique_ptr<pinpoint::analysis::ICharacteristicPackProvider> m_pack;
    std::shared_ptr<const pinpoint::analysis::INormProvider>         m_norms;
    pinpoint::analysis::MetricCatalogue                              m_cat;

    QString m_policyName;

    bool                        m_open = false;
    QString                     m_measureId;
    QString                     m_contextId;
    QString                     m_route;
    pinpoint::analysis::Norm    m_draft;
    pinpoint::analysis::Norm    m_original;
    bool                        m_hadOwnRow = false;   // an own row existed when the draft opened
    bool                        m_dirty     = false;
    QString                     m_refused;             // non-empty => this measure cannot hold a norm

    QString                     m_libraryRoot;
    QString                     m_drawFrom;
    QString                     m_athleteFolder;       // most recent, for the "this athlete" filter
    QString                     m_sessionFolder;       // most recent, for the "this session" filter
    std::vector<Sample>         m_samples;
    // Latched axis, held for the duration of a handle drag. See beginHandleDrag().
    bool                        m_axisLocked  = false;
    double                      m_lockedAxisLo = 0.0;
    double                      m_lockedAxisHi = 1.0;

    bool                        m_scanning    = false;
    bool                        m_everScanned = false;
    bool                        m_truncated   = false; // the scan cap bit; reported, never silent
    int                         m_scanned     = 0;     // swings looked at, produced or not

    QFutureWatcher<QVariantList> *m_watcher = nullptr;
};
