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

// ── The one-sided seat fit ─────────────────────────────────────────────────────────────────────
//
// Free-standing, and deliberately: seating runs off a library scan, so a fit reachable only
// through seatFromSample() could not be gated without fabricating a swing library. The rule is
// the interesting part and it belongs where a test can reach it.
//
// A MEDIAN AND A PERCENTILE, NOT A MEAN AND A DEVIATION. Moments describe a distribution with two
// tails; a floor's sample has one. Its good side is unbounded by construction, so the very
// excellence the norm exists NOT to penalise drags the mean upward and inflates the spread, and
// the corridor is seated above where the population actually sits. The median and the 16th
// percentile read only the graded tail — the only tail the norm makes a claim about.
//
// 16th because on a normal sample it is one sigma below the median, so a seated one-sided corridor
// and a seated two-sided one mean the same thing by the same rule: the shape changes which tail is
// measured, never what "a tolerance" is. Ceiling mirrors at the 84th.
struct OneSidedFit {
    double mu        = 0.0;
    double tolerance = 0.0;
};
OneSidedFit fitOneSided(std::vector<double> values, pinpoint::analysis::Shape shape);

class NormEditorModel : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    // Is a draft open at all. Everything below is meaningless when false.
    Q_PROPERTY(bool open READ isOpen NOTIFY draftChanged)

    // The draft, flattened for rendering:
    //   { measureId, measureLabel, unit, highMeans, contextId, contextLabel,
    //     route, mu, claimLo, claimHi, idealLo, idealHi, goodLo, goodHi, watchLo, watchHi,
    //     shape, oneSided, lowOpen, highOpen, tolerance, gradedEdge, shapeNote, openEndLabel,
    //     claimPhrase, policyNote, parentNote,
    //     n, source, sourceLabel, author, citation, setOn, weak, weakReason,
    //     own, inherited, inheritedFrom, parentClaimLo, parentClaimHi,
    //     dirty, canSave, whyNot, refused, refusedReason }
    //
    // `claimLo/claimHi` is what the handles drag; `idealLo/idealHi` is what the green band draws.
    // They are equal under `standard` and deliberately not under the other presets — see draft().
    //
    // On a ONE-SIDED measure the ungraded side's claim is a number nothing reads: `gradedEdge` is
    // the only edge there is, `mu` is the headline, and the phrase fields carry the wording so no
    // surface has to assemble "at least 1.48" out of a pair that has no second half.
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

    // Open a draft for (measureId, contextId, cohort). Returns false when the measure is unknown or
    // cannot hold a norm at all — a NotCapturable measure is refused rather than given an editor
    // that produces a corridor no sensor can ever feed. `draft.refused` carries the reason for
    // display.
    //
    // The COHORT IS PART OF THE ROW IDENTITY, spelled as the JSON spells it and defaulting to
    // unqualified. Everything the draft keys on follows it: what it seeds from, what `save()`
    // upserts, what `resetToDefault()` removes and which shipped row the basis is stamped against.
    // An unreadable cohort is REFUSED outright rather than dropped to unqualified — a segment's
    // editor silently becoming the editor for everyone is the one outcome worth failing over.
    Q_INVOKABLE bool begin(const QString &measureId, const QString &contextId,
                           const QVariantMap &cohort = {});
    Q_INVOKABLE void cancel();

    // ── Set by hand ─────────────────────────────────────────────────────────
    // The two handles bind the norm's own CLAIM: centre becomes mu, each half-width becomes
    // sigmaLo / sigmaHi. Dragging them apart asymmetrically produces an asymmetric tolerance with
    // no statistics vocabulary anywhere in the interaction. See norm.h — claimLo()/claimHi() are
    // documented as exactly what these bind to.
    //
    // NOT the Ideal band, which is the claim scaled by the active grade policy and is drawn rather
    // than dragged. A handle that moved when a user changed a sensitivity setting would be a
    // control editing its own scale.
    //
    // TWO-SIDED ONLY. `setClaimBand` derives mu as the MIDPOINT of two handles, which is exactly
    // the operation a one-sided norm cannot express: on a floor the headline number is mu itself
    // ("at least 1.48") and there is no second handle to average it against. The one-sided
    // interaction is setAspiration + setTolerance below, and `nudgeClaimLo/Hi` route to them by
    // shape so a caller holding "the low field" keeps working on all three shapes.
    Q_INVOKABLE void setClaimBand(double lo, double hi);
    Q_INVOKABLE void nudgeClaimLo(double to);
    Q_INVOKABLE void nudgeClaimHi(double to);

    // ── Set by hand, one-sided ──────────────────────────────────────────────
    // `setAspiration` moves mu and leaves the tolerance alone; `setTolerance` does the reverse.
    // They are two independent numbers on a one-sided norm — "at least 1.48, and 0.05 of slack
    // below it" — where on a target norm they are two edges whose midpoint and half-width happen
    // to be mu and sigma. Both keep sigmaLo == sigmaHi, which validateNormsAgainst REQUIRES of a
    // one-sided row: the ungraded side's tolerance is not a second setting, it is a number nothing
    // reads, and letting it drift would author an invalid pack from a legal-looking gesture.
    //
    // `nudgeGradedEdge` is the same operation in the drag's coordinates — it takes the EDGE's value
    // off the plot and turns it into the tolerance, so QML still maps pixels to values and nothing
    // else. On a target norm all three are no-ops; the two-sided path is setClaimBand.
    Q_INVOKABLE void setAspiration(double mu);
    Q_INVOKABLE void setTolerance(double tolerance);
    Q_INVOKABLE void nudgeGradedEdge(double edgeValue);

    // ── Plausibility ────────────────────────────────────────────────────────
    // A DIFFERENT QUESTION FROM EVERYTHING ELSE ON THIS SCREEN. The corridor asks whether the
    // swing is good; these ask whether the reading is real. A driver smash of 1.62 is not a swing
    // finding, it is a mis-tracked ball, and grading it in either direction launders a capture
    // fault into a confident diagnosis (norm.h).
    //
    // Optional, independently — a floor caps above and says nothing below — so each has a clear as
    // well as a set. Unlike the monitor bounds, these ARE carried into a draft by begin(): a bound
    // the editor exposes must survive a round trip through it, or opening a capped row and saving
    // it would silently drop the cap.
    Q_INVOKABLE void setPlausibleLo(double v);
    Q_INVOKABLE void setPlausibleHi(double v);
    Q_INVOKABLE void clearPlausibleLo();
    Q_INVOKABLE void clearPlausibleHi();

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
    void dropDerivedProvenance();      // any hand edit demotes Seated/Imported and marks dirty
    void startScan();
    void onScanFinished();
    void recomputeDerived();

    pinpoint::analysis::GradePolicy policy() const;
    QString                         contextLabel(const QString &id) const;
    const pinpoint::analysis::Measure *measure() const;
    QString                         unitOf() const;

    // The MEASURE's shape, which is where one-sidedness lives — never the norm's, never guessed
    // from the unit. Target when no measure resolves, which is what every path here did before
    // shapes existed and is right for 105 of the 106 shipped measures.
    pinpoint::analysis::Shape shape() const;

    // Why this draft's plausibility bounds are not authorable as they stand, per side, or empty
    // when they are. ONE function, consulted by draft() for the inline message and by save() for
    // the refusal, so the editor cannot bless a row the pack validator will reject.
    //
    // Measured against the WIDEST shipped preset (`lenient`), NOT the active one — deliberately
    // the same reference validateNormsAgainst uses. An editor validating against the reader's own
    // sensitivity setting would let an author on `strict` save a row that fails to load for
    // everyone on `lenient`.
    QString plausibleLoProblem() const;
    QString plausibleHiProblem() const;

    // "1.4 to 1.5" / "at least 1.5" / "no more than 12.0" — the ONE place a corridor's claim
    // becomes a sentence. Built here and not in a delegate for the reason `editedNote` already
    // is: which of the three forms applies is a statement about the measure, not a formatting
    // choice, and there are no QML tests in this repo to catch it going wrong in a binding.
    QString claimPhrase(const pinpoint::analysis::Norm &n) const;

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
    pinpoint::analysis::Cohort  m_cohort;              // the third term of the row identity
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
