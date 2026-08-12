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

#include "characteristic_pack.h"

#include "reference_pack.h"   // the book gate resolves a citation against the bibliography

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>

#include <algorithm>
#include <functional>

namespace pinpoint::analysis {

namespace {

// ── The book gate ───────────────────────────────────────────────────────────
//
// True only where the citation RESOLVES to a reference whose sole identifier is an ISBN — a book,
// or a chapter in an edited volume. Everything else, including a citation that resolves to nothing
// at all, is false: an unresolvable citation is a different fault with its own check, and folding
// the two together would make both harder to read and neither easy to fix.
//
// This is the one place the pack loader consults process-global state, and it is worth being
// explicit about the trade. Where no bibliography is on the path — most standalone test binaries —
// the shared set is empty, nothing resolves, and the gate below is a no-op. That is the SAFE
// direction: the gate can only ever soften a claim, never harden one, so an absent registry
// under-corrects rather than inventing a tier nobody authored.
bool bookOnlyCitation(const QString &citation)
{
    if (citation.isEmpty()) return false;
    const Reference *ref = sharedReferenceSet().byCitation(citation);
    return ref && ref->doi.isEmpty() && ref->pmid.isEmpty() && !ref->isbn.isEmpty();
}

// A book is not peer review. `Supported` is documented as "a peer-reviewed source tests this cause
// and this effect" and `Established` as reproduction across independent sources; a coaching text,
// however good, does neither. Admitting ISBNs to the bibliography without this guard would quietly
// promote doctrine into the two tiers that exist to mean the opposite of doctrine.
//
// Demoted to `Indirect` rather than to `Proposed`, which is what the sibling demotion above it
// does. The distinction is the whole point: a missing citation means nobody has shown their
// working, while a book citation is a REAL source that supports the reasoning and simply does not
// test the named pair — which is precisely what `Indirect` was added to record.
bool demoteBookCitation(ProvenanceTier &tier, const QString &citation)
{
    if (tier != ProvenanceTier::Supported && tier != ProvenanceTier::Established) return false;
    if (!bookOnlyCitation(citation)) return false;
    tier = ProvenanceTier::Indirect;
    return true;
}

// ── JSON helpers ────────────────────────────────────────────────────────────

LocalisedText readLocalised(const QJsonValue &v)
{
    LocalisedText t;
    if (v.isString()) {
        // A bare string is accepted as English. Authoring by hand is painful enough without
        // demanding a locale map for every sentence; the loader normalises it.
        t.byLocale.insert(QStringLiteral("en"), v.toString());
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it)
            t.byLocale.insert(it.key(), it.value().toString());
    }
    return t;
}

QJsonValue writeLocalised(const LocalisedText &t)
{
    QJsonObject o;
    for (auto it = t.byLocale.constBegin(); it != t.byLocale.constEnd(); ++it)
        o.insert(it.key(), it.value());
    return o;
}

QJsonArray writeStringList(const QStringList &l)
{
    QJsonArray a;
    for (const QString &s : l) a.append(s);
    return a;
}

// Series / Reducer ───────────────────────────────────────────────────────────

bool readSeries(const QJsonObject &o, Series &out, QString &whyNot)
{
    if (!roleFromName(o.value(QStringLiteral("what")).toString(), out.what)) {
        whyNot = QStringLiteral("unknown 'what' role '%1'").arg(o.value(QStringLiteral("what")).toString());
        return false;
    }
    if (!quantityFromName(o.value(QStringLiteral("quantity")).toString(), out.quantity)) {
        whyNot = QStringLiteral("unknown quantity '%1'").arg(o.value(QStringLiteral("quantity")).toString());
        return false;
    }
    if (!roleFromName(o.value(QStringLiteral("reference")).toString(), out.reference)) {
        whyNot = QStringLiteral("unknown reference role '%1'").arg(o.value(QStringLiteral("reference")).toString());
        return false;
    }
    return true;
}

QJsonObject writeSeries(const Series &s)
{
    QJsonObject o;
    o.insert(QStringLiteral("what"), roleName(s.what));
    o.insert(QStringLiteral("quantity"), quantityName(s.quantity));
    o.insert(QStringLiteral("reference"), roleName(s.reference));
    return o;
}

bool readReducer(const QJsonObject &o, Reducer &out, QString &whyNot)
{
    const auto kind = reducerKindFromName(o.value(QStringLiteral("kind")).toString());
    if (!kind) {
        whyNot = QStringLiteral("unknown reducer kind '%1'").arg(o.value(QStringLiteral("kind")).toString());
        return false;
    }
    out.kind = *kind;

    if (o.contains(QStringLiteral("anchor"))) {
        Phase p{};
        if (!phaseFromToken(o.value(QStringLiteral("anchor")).toString(), p)) {
            whyNot = QStringLiteral("unknown anchor phase '%1'").arg(o.value(QStringLiteral("anchor")).toString());
            return false;
        }
        out.anchor = p;
    }

    if (o.contains(QStringLiteral("window"))) {
        const QJsonArray w = o.value(QStringLiteral("window")).toArray();
        if (w.size() != 2) {
            whyNot = QStringLiteral("window must be exactly two phases");
            return false;
        }
        Phase a{}, b{};
        if (!phaseFromToken(w.at(0).toString(), a) || !phaseFromToken(w.at(1).toString(), b)) {
            whyNot = QStringLiteral("unknown phase in window");
            return false;
        }
        out.window = { a, b };
    }

    if (o.contains(QStringLiteral("sense"))) {
        const auto s = extremumSenseFromName(o.value(QStringLiteral("sense")).toString());
        if (!s) {
            whyNot = QStringLiteral("unknown extremum sense '%1'").arg(o.value(QStringLiteral("sense")).toString());
            return false;
        }
        out.sense = *s;
    }
    return true;
}

QJsonObject writeReducer(const Reducer &r)
{
    QJsonObject o;
    o.insert(QStringLiteral("kind"), reducerKindName(r.kind));
    if (r.anchor.has_value()) o.insert(QStringLiteral("anchor"), phaseToken(*r.anchor));
    if (reducerUsesWindow(r.kind)) {
        QJsonArray w;
        w.append(phaseToken(r.window.first));
        w.append(phaseToken(r.window.second));
        o.insert(QStringLiteral("window"), w);
    }
    if (r.kind == ReducerKind::Extremum) o.insert(QStringLiteral("sense"), extremumSenseName(r.sense));
    return o;
}

// ── Provenance, shared by conditions and edges ───────────────────────────────
// One reader and one writer for both, so an edge's provenance can never drift from a condition's.
// The tier vocabulary is the same vocabulary; the thing being claimed is the only difference.

Provenance parseProvenance(const QJsonObject &o)
{
    Provenance p;
    p.author      = o.value(QStringLiteral("author")).toString();
    p.citation    = o.value(QStringLiteral("citation")).toString();
    p.searchTerms = o.value(QStringLiteral("searchTerms")).toString();
    provenanceTierFromName(o.value(QStringLiteral("tier")).toString(), p.tier);

    const QString on = o.value(QStringLiteral("searchedOn")).toString();
    if (!on.isEmpty()) p.searchedOn = QDate::fromString(on, Qt::ISODate);
    return p;
}

// Written only when it says something. A provenance block of five default fields on all 178 edges
// would treble the file for no information, and `git diff` on the content is how this library is
// reviewed.
QJsonObject writeProvenance(const Provenance &p)
{
    QJsonObject o;
    if (!p.author.isEmpty())      o.insert(QStringLiteral("author"), p.author);
    if (!p.citation.isEmpty())    o.insert(QStringLiteral("citation"), p.citation);
    if (p.tier != ProvenanceTier::Proposed)
        o.insert(QStringLiteral("tier"), provenanceTierName(p.tier));
    if (p.searchedOn.isValid())
        o.insert(QStringLiteral("searchedOn"), p.searchedOn.toString(Qt::ISODate));
    if (!p.searchTerms.isEmpty()) o.insert(QStringLiteral("searchTerms"), p.searchTerms);
    return o;
}

} // namespace

// ── The name a measure renders under ────────────────────────────────────────

QString measureDisplayLabel(const Measure &m)
{
    if (!m.label.isEmpty()) return m.label;
    // Only a Composed measure has facets to generate from. Asking a Provided measure for its
    // canonical label would name it after a default-constructed series — a confident wrong answer,
    // which is worse than falling through to the id.
    if (m.kind == MeasureKind::Composed) {
        const QString canonical = canonicalMeasureLabel(m.series, m.reducer);
        if (!canonical.isEmpty()) return canonical;
    }
    return m.id;
}

// ── Graph helpers ───────────────────────────────────────────────────────────

QStringList causesOf(const CharacteristicPack &pack, const QString &conditionId)
{
    QStringList out;
    for (const Edge &e : pack.edges)
        if (e.type == EdgeType::Causes && e.to == conditionId) out << e.from;
    return out;
}

QStringList effectsOf(const CharacteristicPack &pack, const QString &conditionId)
{
    QStringList out;
    for (const Edge &e : pack.edges)
        if (e.type == EdgeType::Causes && e.from == conditionId) out << e.to;
    return out;
}

int coverageOf(const CharacteristicPack &pack, const QString &conditionId)
{
    return int(effectsOf(pack, conditionId).size());
}

bool causallyReaches(const CharacteristicPack &pack, const QString &fromId, const QString &toId)
{
    QSet<QString> seen;
    QStringList   stack{ fromId };
    while (!stack.isEmpty()) {
        const QString cur = stack.takeLast();
        if (cur == toId && cur != fromId) return true;
        if (seen.contains(cur)) continue;
        seen.insert(cur);
        for (const QString &next : effectsOf(pack, cur)) {
            if (next == toId) return true;
            stack << next;
        }
    }
    return false;
}

bool hasCausalPath(const CharacteristicPack &pack, const QString &fromId, const QString &toId)
{
    // Either direction: a Corroborates edge is symmetric in meaning, so a causal path either way
    // makes it a double count.
    return causallyReaches(pack, fromId, toId) || causallyReaches(pack, toId, fromId);
}

QSet<QString> causalClosure(const CharacteristicPack &pack, const QString &id, bool downstream)
{
    // One pass over the edge list per frontier node rather than an adjacency map built up front:
    // the frontier is bounded by the condition count, and building the map would cost the same walk
    // it saves. `seen` is the visited set AND the result, so a cycle already in the pack terminates
    // here instead of hanging the drag that asked.
    QSet<QString> seen;
    QStringList   stack{ id };
    while (!stack.isEmpty()) {
        const QString cur = stack.takeLast();
        for (const QString &next : downstream ? effectsOf(pack, cur) : causesOf(pack, cur)) {
            if (seen.contains(next)) continue;
            seen.insert(next);
            stack << next;
        }
    }
    // Only if a cycle led back to it. The contract is "what this reaches", and a self entry would
    // read as a self-loop that is not in the pack.
    seen.remove(id);
    return seen;
}

int outcomeReachOf(const CharacteristicPack &pack, const QString &conditionId)
{
    // Asks the KIND, not the group. "Is this an outcome" is a question about what sort of thing a
    // condition is, and the group answers where in the swing it sits — this line was the only place
    // in the model that had to press the group into the other job, because there was nowhere else to
    // ask. The two sets are exactly coextensive over shipped content and `core_pack_test` asserts it
    // in both directions, so the change is a no-op by construction rather than by hope.
    int n = 0;
    for (const QString &id : causalClosure(pack, conditionId, /*downstream*/ true))
        if (const Condition *c = pack.condition(id))
            if (c->kind == ConditionKind::Outcome) ++n;
    return n;
}

QStringList tailsOfAxis(const CharacteristicPack &pack, const QString &axis)
{
    QStringList out;
    if (axis.isEmpty()) return out;
    for (const Condition &c : pack.conditions)
        if (c.axis == axis) out << c.id;
    return out;
}

bool measureIsDeltaFromAddress(const Measure &m)
{
    return (m.reducer.kind == ReducerKind::Delta || m.reducer.kind == ReducerKind::Rate)
           && m.reducer.window.first == Phase::Address;
}

std::vector<const Measure *> measuresForMetricAtPhase(const CharacteristicPack &pack,
                                                      const QString            &metricKey,
                                                      Phase                     phase)
{
    std::vector<const Measure *> absolute;   // `at` — the reading a phase-keyed corridor means
    std::vector<const Measure *> changes;    // `delta` / `rate` — the change observed AT that phase

    if (metricKey.isEmpty()) return {};

    for (const Measure &m : pack.measures) {
        // ANY RUNG OF THE LADDER, not just the measure's own key. This is the join a chart uses to
        // ask "what corridor grades this curve", and a measure that prefers `lm.attackAngle` over
        // our `attackAngle` grades both — so a plot of the device's reading has to find it, or the
        // corridor drawn on the chart and the corridor the diagnosis used would be different rows.
        if (!measureKeyLadder(m).contains(metricKey)) continue;

        switch (m.reducer.kind) {
        case ReducerKind::At:
            // `anchor` is optional on the type; an At reducer without one reads at the window
            // start, which is what the loader defaults it to.
            if (m.reducer.anchor.value_or(m.reducer.window.first) == phase)
                absolute.push_back(&m);
            break;
        case ReducerKind::Delta:
        case ReducerKind::Rate:
            if (m.reducer.window.second == phase)
                changes.push_back(&m);
            break;
        case ReducerKind::Extremum:
            break;                        // a peak across a window is not a reading at a phase
        }
    }

    absolute.insert(absolute.end(), changes.begin(), changes.end());
    return absolute;
}

const Measure *measureForMetricAtPhase(const CharacteristicPack &pack,
                                       const QString            &metricKey,
                                       Phase                     phase)
{
    const std::vector<const Measure *> all = measuresForMetricAtPhase(pack, metricKey, phase);
    return all.empty() ? nullptr : all.front();
}

// ── Validation ──────────────────────────────────────────────────────────────

ValidationReport validatePack(const CharacteristicPack &pack)
{
    ValidationReport r;

    // --- ids are unique across each kind -------------------------------------
    auto checkUnique = [&r](const QStringList &ids, const QString &what) {
        QSet<QString> seen;
        for (const QString &id : ids) {
            if (id.isEmpty()) {
                err(r, QStringLiteral("duplicateId"), id, QStringLiteral("A %1 has an empty id.").arg(what));
                continue;
            }
            if (seen.contains(id))
                err(r, QStringLiteral("duplicateId"), id,
                    QStringLiteral("Duplicate %1 id '%2'. Ids are permanent and must be unique.").arg(what, id));
            seen.insert(id);
        }
    };

    QStringList measureIds, signalIds, conditionIds;
    for (const Measure &m : pack.measures)     measureIds   << m.id;
    for (const Signal &s : pack.signalDefs)       signalIds    << s.id;
    for (const Condition &c : pack.conditions) conditionIds << c.id;
    checkUnique(measureIds, QStringLiteral("measure"));
    checkUnique(signalIds, QStringLiteral("signal"));
    checkUnique(conditionIds, QStringLiteral("condition"));

    const QSet<QString> measureSet(measureIds.begin(), measureIds.end());
    const QSet<QString> signalSet(signalIds.begin(), signalIds.end());
    const QSet<QString> conditionSet(conditionIds.begin(), conditionIds.end());

    // Which tails a corridor signal already watches, per measure. Built up here rather than inside
    // the signals loop below because the measure that CLAIMS a tail is unwatched is checked against
    // it, and splitting one three-rule family across two loops reads worse than one early pass.
    QHash<QString, QSet<int>> watchedTails;
    for (const Signal &s : pack.signalDefs) {
        if (s.test != SignalTest::OutsideCorridor || !s.direction.has_value()) continue;
        for (const QString &mid : s.measures)
            watchedTails[mid].insert(static_cast<int>(*s.direction));
    }

    // --- measures ------------------------------------------------------------
    for (const Measure &m : pack.measures) {
        if (m.kind == MeasureKind::Composed) {
            const FacetCheck fc = validateSeries(m.series);
            if (!fc.valid)
                err(r, QStringLiteral("badFacets"), m.id,
                    QStringLiteral("Measure '%1': %2").arg(m.id, fc.reason));
        } else if (m.metricKey.isEmpty()) {
            err(r, QStringLiteral("badFacets"), m.id,
                QStringLiteral("Provided measure '%1' names no metric key.").arg(m.id));
        }

        // --- the instrument ladder ------------------------------------------
        //
        // Structure only. That every rung EXISTS in the catalogue and states the measure's unit is
        // checked by diagnostics_catalogue_integrity_test, which is the one place that holds both
        // registries at once; this file has never been able to see the catalogue and must not start
        // guessing at it. What it can see is a row that contradicts itself.
        if (!m.preferKeys.isEmpty()) {
            if (m.kind == MeasureKind::Composed)
                err(r, QStringLiteral("badFacets"), m.id,
                    QStringLiteral("Composed measure '%1' names preferred metric keys. A composed "
                                   "measure is built from facets and has no catalogue key to prefer "
                                   "over.").arg(m.id));

            QSet<QString> seen;
            for (const QString &k : m.preferKeys) {
                if (k.isEmpty())
                    err(r, QStringLiteral("badFacets"), m.id,
                        QStringLiteral("Measure '%1' has an empty entry in preferKeys.").arg(m.id));
                // Both of these would read as a ladder while behaving as a single rung — the first
                // silently, the second by putting the fallback ahead of itself.
                else if (seen.contains(k))
                    err(r, QStringLiteral("badFacets"), m.id,
                        QStringLiteral("Measure '%1' lists '%2' twice in preferKeys.").arg(m.id, k));
                else if (k == m.metricKey)
                    err(r, QStringLiteral("badFacets"), m.id,
                        QStringLiteral("Measure '%1' prefers '%2' over itself — that key IS its "
                                       "metricKey.").arg(m.id, k));
                seen.insert(k);
            }
        }

        // The reducer is checked for BOTH kinds: a Provided measure over a catalogue TimeSeries
        // still has to say how the curve becomes a number.
        const ReducerCheck rc = validateReducer(m.reducer);
        if (!rc.valid)
            err(r, QStringLiteral("badReducer"), m.id,
                QStringLiteral("Measure '%1': %2").arg(m.id, rc.reason));

        // An ExternalDevice measure without a reason is indistinguishable, on every surface that
        // shows it, from one nobody has got round to. The status says WHAT stands in the way; the
        // reason has to say WHICH device, or the roadmap's integration section is a list of labels.
        if (m.status == MeasureStatus::ExternalDevice && m.gapReason.isEmpty())
            warn(r, QStringLiteral("externalDeviceNoReason"), m.id,
                 QStringLiteral("Measure '%1' reads from an external device but does not say which. "
                                "Name it — the roadmap and the measure page both quote this.").arg(m.id));

        // --- a deliberately unwatched tail ----------------------------------
        //
        // The reason is a WARNING and the two contradictions are ERRORS, and the split is about what
        // each one means. A missing reason is a claim nobody wrote down — annoying, recoverable, and
        // the same shape as externalDeviceNoReason above. The other two are the field asserting
        // something the pack itself says is false, and loading them would leave `ungradedTail`
        // silenced on a tail that either never graded or is watched perfectly well — a check turned
        // off by a statement that was never true.
        if (m.unwatchedTail.has_value()) {
            if (m.unwatchedReason.isEmpty())
                warn(r, QStringLiteral("unwatchedTailNoReason"), m.id,
                     QStringLiteral("Measure '%1' says its %2 tail is deliberately unwatched but "
                                    "does not say why. Without the reason it is indistinguishable "
                                    "from a tail nobody has got to.")
                         .arg(m.id, directionName(*m.unwatchedTail)));

            if (shapeIsOneSided(m.shape))
                err(r, QStringLiteral("unwatchedTailShaped"), m.id,
                    QStringLiteral("Measure '%1' is '%2', so neither tail can be 'deliberately "
                                   "unwatched' — the shape already says which one does not grade. "
                                   "Remove one of the two.")
                        .arg(m.id, shapeLabel(m.shape)));

            if (watchedTails.value(m.id).contains(static_cast<int>(*m.unwatchedTail)))
                err(r, QStringLiteral("unwatchedTailWatched"), m.id,
                    QStringLiteral("Measure '%1' says its %2 tail is unwatched, and a corridor "
                                   "signal watches exactly that tail. One of the two is wrong.")
                        .arg(m.id, directionName(*m.unwatchedTail)));
        }
    }

    // --- signals -------------------------------------------------------------
    QSet<QString> usedMeasures;
    for (const Signal &s : pack.signalDefs) {
        for (const QString &mid : s.measures) {
            usedMeasures.insert(mid);
            if (!measureSet.contains(mid))
                err(r, QStringLiteral("unknownMeasure"), s.id,
                    QStringLiteral("Signal '%1' references unknown measure '%2'.").arg(s.id, mid));
        }

        const bool needsTwo = (s.test == SignalTest::Order || s.test == SignalTest::Ratio);
        const int  want     = needsTwo ? 2 : 1;
        if (s.measures.size() != want)
            err(r, QStringLiteral("signalArity"), s.id,
                QStringLiteral("Signal '%1' (%2) needs exactly %3 measure(s), has %4.")
                    .arg(s.id, signalTestName(s.test)).arg(want).arg(s.measures.size()));

        // A condition is ONE TAIL of one measure. Without a direction the signal cannot say which
        // tail fired, so it cannot identify a condition. Ratio is in this set: its quotient has a
        // high side and a low side exactly as a measure does — too much backswing for the
        // downswing, or too little — and those are two different faults. Order is not, because the
        // ordering it tests is inherent in which measure the author wrote first.
        const bool tailed = (s.test == SignalTest::OutsideCorridor
                             || s.test == SignalTest::Threshold
                             || s.test == SignalTest::Ratio);
        if (tailed && !s.direction.has_value())
            err(r, QStringLiteral("signalDirection"), s.id,
                QStringLiteral("Signal '%1' needs a direction — which side of the corridor is it?").arg(s.id));

        // WHO AUTHORS THE NUMBER. Two tests grade against a figure no norm supplies. Threshold is
        // the obvious one; Ratio is the other, and it is not a relaxation — a quotient is not a
        // measure, so nothing in the norm set can key on it, and the engine's Ratio branch sets out
        // at length why a stand-in measure carrying a dimensionless corridor cannot be made to
        // work. The remaining two inherit their numbers from the catalogue and must not author one.
        const bool authorsNumber = (s.test == SignalTest::Threshold || s.test == SignalTest::Ratio);
        if (authorsNumber && !s.threshold.has_value())
            err(r, QStringLiteral("signalThreshold"), s.id,
                QStringLiteral("Signal '%1' is a %2 test with no threshold — there is no norm for "
                               "it to inherit one from.").arg(s.id, signalTestName(s.test)));
        if (!authorsNumber && s.threshold.has_value())
            err(r, QStringLiteral("signalThreshold"), s.id,
                QStringLiteral("Signal '%1' carries a threshold but is a %2 test. Corridor tests "
                               "inherit their numbers from the catalogue and must not author one.")
                    .arg(s.id, signalTestName(s.test)));

        // ── A ratio must be DIMENSIONLESS ───────────────────────────────────
        //
        // The engine divides the first measure by the second and compares the quotient against the
        // authored number. That number only means anything if the units cancel: seconds over
        // seconds is a pure figure and "3.0" is a tempo, while degrees over seconds is a rate
        // wearing a ratio's name and "3.0" is stated in a unit nothing in this model records. The
        // model has no vocabulary for compound units, and this is the check that keeps it from
        // needing one.
        //
        // An UNSTATED unit fails too, deliberately. "We cannot tell whether these cancel" and "they
        // cancel" are different answers and only one of them makes the authored figure defensible;
        // two measures that really are commensurate need only say so once each.
        if (s.test == SignalTest::Ratio && s.measures.size() == 2) {
            const Measure *num = pack.measure(s.measures.at(0));
            const Measure *den = pack.measure(s.measures.at(1));
            const auto     unitOf = [](const Measure *m) {
                return m->unit.isEmpty() ? QStringLiteral("no unit") : m->unit;
            };
            if (num != nullptr && den != nullptr
                && (num->unit.isEmpty() || num->unit != den->unit))
                err(r, QStringLiteral("signalRatioUnit"), s.id,
                    QStringLiteral("Signal '%1' divides '%2' (%3) by '%4' (%5). A ratio is a pure "
                                   "number only when the units cancel, so its authored figure means "
                                   "nothing here — state one unit on both measures.")
                        .arg(s.id, num->id, unitOf(num), den->id, unitOf(den)));
        }
    }

    for (const Measure &m : pack.measures)
        if (!usedMeasures.contains(m.id))
            warn(r, QStringLiteral("unusedMeasure"), m.id,
                 QStringLiteral("Measure '%1' is not used by any signal.").arg(m.id));

    // Two PROVIDED measures reading the same metric with the same reducer are the same number
    // twice. The structural duplicate detector above cannot see this: it compares SERIES, and a
    // Provided measure has no series — it names a metric key instead. So the one class of measure
    // that most easily duplicates is the one that was unguarded.
    //
    // This matters beyond tidiness. Norms key on the measure, so a duplicated measure can carry
    // two different corridors for one quantity, and whichever the (metricKey, phase) join happens
    // to return decides the grade. Nothing downstream can detect that — both answers look correct.
    {
        QHash<QString, QString> firstWithKey;      // reduction signature -> measure id
        for (const Measure &m : pack.measures) {
            if (m.kind != MeasureKind::Provided || m.metricKey.isEmpty()) continue;

            const Reducer &rd = m.reducer;
            const QString  sig = QStringLiteral("%1|%2|%3|%4|%5|%6")
                                    .arg(m.metricKey, reducerKindName(rd.kind),
                                         rd.anchor ? phaseToken(*rd.anchor) : QStringLiteral("-"),
                                         phaseToken(rd.window.first), phaseToken(rd.window.second),
                                         extremumSenseName(rd.sense));

            const auto it = firstWithKey.constFind(sig);
            if (it == firstWithKey.constEnd()) { firstWithKey.insert(sig, m.id); continue; }

            warn(r, QStringLiteral("duplicateMeasure"), m.id,
                 QStringLiteral("Measure '%1' reads '%2' with exactly the same reduction as '%3'. "
                                "They are one number described twice, and a norm on each can grade "
                                "the same value two different ways.")
                     .arg(m.id, m.metricKey, it.value()));
        }
    }

    // --- conditions ----------------------------------------------------------
    //
    // The kind rules below are gated on whether ANYTHING in this pack has an opinion about kind. A
    // pack authored before the field existed loads with every condition at the default — Fault — and
    // an ungated `faultNotObservable` would then accuse every Latent condition in it of an omission
    // that is really the field's own age. Same reasoning as scoping `personalNormNoSample` to the
    // personal layer so the migrated shipped rows never appear: a check that fires on the design is
    // worse than no check.
    //
    // The stated cost, so nobody rediscovers it as a bug: a pack whose conditions genuinely are ALL
    // Faults escapes these four rules. That is acceptable and arguably correct — a pack where
    // everything is a fault has nothing for a kind rule to compare.
    const bool kindsAuthored =
        std::any_of(pack.conditions.begin(), pack.conditions.end(),
                    [](const Condition &c) { return c.kind != ConditionKind::Fault; });

    for (const Condition &c : pack.conditions) {
        for (const QString &sid : c.detectedBy)
            if (!signalSet.contains(sid))
                err(r, QStringLiteral("unknownSignal"), c.id,
                    QStringLiteral("Condition '%1' references unknown signal '%2'.").arg(c.id, sid));

        // Scoped by ConfirmedBy, or it accuses the content of exactly what the content says.
        // "Observable" answers CAN IT BE SEEN; "Asserted"/"Screened" answers HOW IT IS ESTABLISHED.
        // A thin shot is plainly visible and equally plainly not measurable from our pixels, so it
        // ships Observable + Asserted with no signal ON PURPOSE — the golfer knows and the app does
        // not. Unscoped, this fired on all seven of those, and a health list carrying seven rows
        // that describe the design is a health list people learn to scroll past. The inverse of
        // this pair is already scoped the same way, one check below.
        if (c.observability == Observability::Observable && c.detectedBy.isEmpty()
            && !isOutsideCaptureReach(c.confirmedBy))
            warn(r, QStringLiteral("observableNoSignal"), c.id,
                 QStringLiteral("'%1' is Observable and Measured, but nothing detects it.").arg(c.id));

        // A CONJUNCTION OF ONE IS NOT A CONJUNCTION. With a single signal, `all` and `any` are the
        // same test written two ways — so the field is either a leftover from signals that were
        // removed, or an author part-way through adding the rest. Both are worth a look, and
        // neither is worth failing the load over: the pack still behaves correctly.
        if (c.detection == DetectionMode::All && c.detectedBy.size() < 2)
            warn(r, QStringLiteral("conjunctionOfOne"), c.id,
                 QStringLiteral("'%1' combines its signals with `all` but has %2. A conjunction "
                                "needs at least two terms to mean anything.")
                     .arg(c.id, c.detectedBy.isEmpty() ? QStringLiteral("none")
                                                       : QStringLiteral("only one")));

        // A condition that is only reachable by a screen or by asking cannot also be measured.
        if (isOutsideCaptureReach(c.confirmedBy) && !c.detectedBy.isEmpty())
            warn(r, QStringLiteral("inconsistentReach"), c.id,
                 QStringLiteral("'%1' is %2 but also claims a detecting signal.")
                     .arg(c.id, confirmedByName(c.confirmedBy)));

        // A Fault is a movement error IN THE SWING, so one that cannot be seen there is misfiled —
        // it is a Capacity, an Intent, or an event this pack has no measure for and should not be
        // calling a fault. `Both` passes: it can be seen SOMETIMES, which is enough.
        //
        // This is the check the whole kind axis was worth adding for. `over_the_top` shipped Latent
        // and Asserted for a year — the commonest fault in amateur golf, tagged as though it were a
        // belief — and nothing could say so, because with no kind field there was no claim to
        // contradict.
        if (kindsAuthored && c.kind == ConditionKind::Fault
            && c.observability == Observability::Latent)
            warn(r, QStringLiteral("faultNotObservable"), c.id,
                 QStringLiteral("'%1' is a Fault that cannot be seen in the swing. A movement nobody "
                                "can see is a capacity or an intent, not a swing fault.").arg(c.id));

        // Two halves of one authoring mistake, under one code: an author fixes either by moving the
        // kind or moving the reach, and two codes for one fix is what this file keeps complaining
        // about. The message names which pair disagreed so the row is still actionable.
        if (kindsAuthored && c.kind == ConditionKind::Capacity
            && c.confirmedBy != ConfirmedBy::Screened)
            warn(r, QStringLiteral("kindReachMismatch"), c.id,
                 QStringLiteral("'%1' is a Capacity but is reached by %2. What the body can do is "
                                "established by a physical screen, not from the swing.")
                     .arg(c.id, confirmedByName(c.confirmedBy)));

        if (kindsAuthored && c.kind == ConditionKind::Intent
            && c.confirmedBy != ConfirmedBy::Asserted)
            warn(r, QStringLiteral("kindReachMismatch"), c.id,
                 QStringLiteral("'%1' is an Intent but is reached by %2. What a golfer is trying to "
                                "do is knowable only by asking.")
                     .arg(c.id, confirmedByName(c.confirmedBy)));

        // THE ONE RULE THAT COUPLES THE TWO AXES, and it is here on sufferance. Kind and group are
        // meant to be orthogonal; this says they are not, for one value. It is worth having only
        // while the two sets are exactly coextensive over shipped content, which `core_pack_test`
        // asserts in both directions. If a legitimate Outcome ever needs to sit outside BallFlight,
        // DELETE THIS RULE rather than bending the content to it.
        if (kindsAuthored && c.kind == ConditionKind::Outcome
            && c.group != ConditionGroup::BallFlight)
            warn(r, QStringLiteral("outcomeNotBallFlight"), c.id,
                 QStringLiteral("'%1' is an Outcome but sits in the %2 group rather than ball "
                                "flight.").arg(c.id, conditionGroupName(c.group)));

        if (c.provenance.tier == ProvenanceTier::Proposed)
            warn(r, QStringLiteral("proposedTier"), c.id,
                 QStringLiteral("'%1' has no citation and must be badged as proposed.").arg(c.id));

        if (c.state == ConditionState::NeedsRevalidation)
            warn(r, QStringLiteral("needsRevalidation"), c.id,
                 QStringLiteral("'%1' is flagged for revalidation.").arg(c.id));

        if (c.state == ConditionState::Superseded && c.supersededBy.isEmpty())
            err(r, QStringLiteral("unknownCondition"), c.id,
                QStringLiteral("'%1' is superseded but names no successor.").arg(c.id));
        if (!c.supersededBy.isEmpty() && !conditionSet.contains(c.supersededBy))
            err(r, QStringLiteral("unknownCondition"), c.id,
                QStringLiteral("'%1' is superseded by unknown condition '%2'.").arg(c.id, c.supersededBy));
    }

    // --- one term, one condition ---------------------------------------------
    //
    // Aliases are how the library is reachable for anyone who did not write it, so two conditions
    // claiming one term is not untidiness — it decides which page a search for "flip" lands on, and
    // whichever came first in the file wins. The comparison folds case and trims, because "OTT" and
    // "ott " are the same word to the person typing it. A condition's own LABEL counts as a claim on
    // that term too.
    {
        QHash<QString, QString> claimedBy;      // folded term -> condition id
        for (const Condition &c : pack.conditions) {
            QStringList terms = c.aliases;
            terms << c.label;
            for (const QString &raw : terms) {
                const QString t = raw.trimmed().toCaseFolded();
                if (t.isEmpty()) continue;
                const auto it = claimedBy.constFind(t);
                if (it == claimedBy.constEnd()) { claimedBy.insert(t, c.id); continue; }
                if (it.value() == c.id) continue;
                warn(r, QStringLiteral("duplicateAlias"), c.id,
                     QStringLiteral("'%1' and '%2' both answer to \"%3\". One term can only lead "
                                    "one place — rename it on whichever owns it less.")
                         .arg(c.id, it.value(), raw.trimmed()));
            }
        }
    }

    // --- axis pairing --------------------------------------------------------
    // Two tails of one corridor must sit on the SAME measure — that is what makes them tails rather
    // than two unrelated conditions that happen to share a label.
    {
        QSet<QString> axes;
        for (const Condition &c : pack.conditions)
            if (!c.axis.isEmpty()) axes.insert(c.axis);

        for (const QString &axis : axes) {
            const QStringList tails = tailsOfAxis(pack, axis);
            if (tails.size() == 1) {
                warn(r, QStringLiteral("singleTailAxis"), tails.first(),
                     QStringLiteral("Axis '%1' has only one authored tail ('%2'). Deliberate, or missing?")
                         .arg(axis, tails.first()));
                continue;
            }
            if (tails.size() > 2) {
                err(r, QStringLiteral("axisMismatch"), axis,
                    QStringLiteral("Axis '%1' has %2 tails; a corridor has two.").arg(axis).arg(tails.size()));
                continue;
            }

            // Resolve each tail's series through its signals' measures and require agreement.
            auto seriesFor = [&](const QString &cid, Series &out) {
                const Condition *c = pack.condition(cid);
                if (!c) return false;
                for (const QString &sid : c->detectedBy) {
                    const Signal *s = pack.signal(sid);
                    if (!s || s->measures.isEmpty()) continue;
                    const Measure *m = pack.measure(s->measures.first());
                    if (!m || m->kind != MeasureKind::Composed) continue;
                    out = m->series;
                    return true;
                }
                return false;
            };
            Series sa, sb;
            const bool ha = seriesFor(tails.at(0), sa);
            const bool hb = seriesFor(tails.at(1), sb);
            if (ha && hb && sa != sb)
                err(r, QStringLiteral("axisMismatch"), axis,
                    QStringLiteral("Axis '%1' joins two conditions built on different series. They "
                                   "are not two tails of one corridor.").arg(axis));
        }
    }

    // --- one condition flagging BOTH tails of one measure --------------------
    //
    // Ledger C30. Two tails of a corridor are two CONDITIONS — high and low mean different things
    // about the swing, which is what an axis pairs. One condition carrying both fires whichever way
    // the value goes, so it is not a detector at all: it says "this is off" for every swing that is
    // not exactly average, with whichever consequence text the author wrote for one side.
    //
    // `setSignalDirection()` already refuses to create the pair when flipping a tail in place. The
    // ATTACH path cannot see it — "Add a measure" twice builds two ids that are both legitimate on
    // their own — so it is caught here. A warning, not an error: refusing to load would lock an
    // author out of the library they need to open in order to fix it.
    for (const Condition &c : pack.conditions) {
        QHash<QString, QSet<int>> tailsPerMeasure;      // measureId -> the directions attached
        for (const QString &sid : c.detectedBy) {
            const Signal *s = pack.signal(sid);
            if (!s || s->test != SignalTest::OutsideCorridor || !s->direction.has_value()) continue;
            for (const QString &mid : s->measures)
                tailsPerMeasure[mid].insert(static_cast<int>(*s->direction));
        }
        for (auto it = tailsPerMeasure.cbegin(); it != tailsPerMeasure.cend(); ++it) {
            if (it.value().size() < 2) continue;
            warn(r, QStringLiteral("bothTailsOneCondition"), c.id,
                 QStringLiteral("'%1' flags BOTH sides of '%2'. It will fire whichever way the "
                                "reading goes, so it cannot distinguish too much from too little — "
                                "the two sides belong on the two tails of an axis. Remove one.")
                     .arg(c.id, it.key()));
        }
    }

    // --- edges ---------------------------------------------------------------
    for (const Edge &e : pack.edges) {
        if (!conditionSet.contains(e.from))
            err(r, QStringLiteral("unknownCondition"), e.from,
                QStringLiteral("Edge references unknown condition '%1'.").arg(e.from));
        if (!conditionSet.contains(e.to))
            err(r, QStringLiteral("unknownCondition"), e.to,
                QStringLiteral("Edge references unknown condition '%1'.").arg(e.to));
        if (e.from == e.to)
            err(r, QStringLiteral("selfEdge"), e.from,
                QStringLiteral("'%1' cannot relate to itself.").arg(e.from));
    }

    // Cycle detection over Causes edges only. Corroborates and Excludes are not orderings.
    {
        QHash<QString, int> state;   // 0 unvisited, 1 on stack, 2 done
        std::function<bool(const QString &)> visit = [&](const QString &id) -> bool {
            const int st = state.value(id, 0);
            if (st == 1) return true;
            if (st == 2) return false;
            state[id] = 1;
            for (const QString &next : effectsOf(pack, id))
                if (visit(next)) return true;
            state[id] = 2;
            return false;
        };
        for (const Condition &c : pack.conditions) {
            if (state.value(c.id, 0) == 0 && visit(c.id)) {
                err(r, QStringLiteral("cycle"), c.id,
                    QStringLiteral("The condition graph has a cycle through '%1'. Causes must form a DAG.")
                        .arg(c.id));
                break;   // one report is enough; the author fixes the graph, not each edge
            }
        }
    }

    // Corroborates must not shadow a causal claim.
    for (const Edge &e : pack.edges) {
        if (e.type != EdgeType::Corroborates) continue;
        if (!conditionSet.contains(e.from) || !conditionSet.contains(e.to)) continue;
        if (hasCausalPath(pack, e.from, e.to))
            err(r, QStringLiteral("corroboratesCausal"), e.from,
                QStringLiteral("'%1' and '%2' are already causally linked, so they cannot also "
                               "corroborate — the pair would count twice in the ranking.")
                    .arg(e.from, e.to));
    }

    // --- explanation health --------------------------------------------------
    for (const Condition &c : pack.conditions) {
        const QStringList causes = causesOf(pack, c.id);

        // A Latent condition exists to explain things. One that explains nothing is dead weight.
        if (c.observability == Observability::Latent && effectsOf(pack, c.id).isEmpty())
            warn(r, QStringLiteral("orphanCause"), c.id,
                 QStringLiteral("'%1' is Latent but explains nothing.").arg(c.id));

        if (c.observability == Observability::Latent) continue;

        if (causes.isEmpty()) {
            warn(r, QStringLiteral("noCause"), c.id,
                 QStringLiteral("'%1' can be reported but never explained.").arg(c.id));
            continue;
        }

        // Asserted causes are OFFERED, never concluded — so a characteristic whose every cause is
        // Asserted has nothing the resolver can settle on. That is a legitimate state (some traits
        // really are just habit) but the author should have decided it deliberately.
        const bool anyResolvable = std::any_of(causes.begin(), causes.end(), [&](const QString &cid) {
            const Condition *cause = pack.condition(cid);
            return cause && cause->confirmedBy != ConfirmedBy::Asserted;
        });
        if (!anyResolvable)
            warn(r, QStringLiteral("noResolvableCause"), c.id,
                 QStringLiteral("Every cause of '%1' is Behavioural, so it can be offered but never "
                                "concluded.").arg(c.id));
    }

    // --- edge orientation, structurally --------------------------------------
    // The seed tables read effect-first while Edge is cause-first, so every row flips on
    // transcription. No coverage count can catch a mistake here: cause-coverage totals are
    // IDENTICAL under edge reversal. This is the check that can.
    //
    // A Screened condition is a physical capacity. It explains things; nothing in a swing explains
    // it. An incoming Causes edge therefore means the graph is pointing the wrong way.
    for (const Condition &c : pack.conditions) {
        if (c.confirmedBy != ConfirmedBy::Screened) continue;
        const QStringList incoming = causesOf(pack, c.id);
        if (!incoming.isEmpty())
            warn(r, QStringLiteral("screenedHasCause"), c.id,
                 QStringLiteral("'%1' is a physical screen result but something in the swing claims "
                                "to cause it ('%2'). Check the edge direction — `from` is the cause.")
                     .arg(c.id, incoming.first()));
    }

    // The other end of the same chain, and the same class of mistake. An Outcome is where an
    // explanation ENDS: what the ball did cannot cause the swing that produced it. An outgoing
    // Causes edge from one is an edge written back to front.
    //
    // Kept SEPARATE from screenedHasCause rather than folded into a "kind orientation" rule, and
    // screenedHasCause is deliberately left keyed on ConfirmedBy rather than on Capacity: it is the
    // inverted-graph detector and its value is that it is UNSCOPED, catching an incoming edge from a
    // fault, a delivery or an outcome alike. It also keys on the field `relation_resolver` actually
    // reads. A kind-keyed replacement would guard a field nothing downstream consumes yet.
    if (kindsAuthored) {
        for (const Condition &c : pack.conditions) {
            if (c.kind != ConditionKind::Outcome) continue;
            const QStringList outgoing = effectsOf(pack, c.id);
            if (!outgoing.isEmpty())
                warn(r, QStringLiteral("outcomeHasEffect"), c.id,
                     QStringLiteral("'%1' is an Outcome but claims to cause '%2'. What the ball did "
                                    "cannot cause the swing that produced it — check the direction.")
                         .arg(c.id, outgoing.first()));
        }
    }

    return r;
}

// ── Load / save ─────────────────────────────────────────────────────────────

PackLoadResult loadPack(const QJsonObject &root, const QString &sourceLabel)
{
    PackLoadResult out;
    ValidationReport &r = out.report;

    out.pack.id            = root.value(QStringLiteral("id")).toString();
    out.pack.version       = root.value(QStringLiteral("version")).toString();
    out.pack.schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(kPackSchemaVersion);
    out.pack.sourceLabel   = sourceLabel;

    // Its own code, not `duplicateId`. A pack with no id is not a collision — it is content that
    // cannot be namespaced, layered, or named in any issue it goes on to raise, and an author
    // hunting the word "duplicate" for a file that has exactly one of everything is being sent the
    // wrong way by the only word the message gave them.
    if (out.pack.id.isEmpty()) {
        err(r, QStringLiteral("noPackId"), QString(), QStringLiteral("Pack has no id."));
        return out;
    }

    // A newer schema is refused outright rather than partially read. Dropping fields this build
    // does not understand would let a newer pack round-trip through an older one and lose content
    // silently — the worst possible failure for authored content.
    //
    // `schemaTooNew`, which is what the norm set has always called it, rather than `schemaVersion`:
    // the version field being ABSENT or malformed is a different fault with a different fix, and
    // one code for both left a reader unable to tell "your build is old" from "your file is wrong".
    if (out.pack.schemaVersion > kPackSchemaVersion) {
        err(r, QStringLiteral("schemaTooNew"), out.pack.id,
            QStringLiteral("Pack '%1' declares schema version %2; this build understands %3. "
                           "Refusing to load rather than silently dropping content.")
                .arg(out.pack.id).arg(out.pack.schemaVersion).arg(kPackSchemaVersion));
        return out;
    }

    // --- measures ------------------------------------------------------------
    for (const QJsonValue &v : root.value(QStringLiteral("measures")).toArray()) {
        const QJsonObject o = v.toObject();
        Measure           m;
        m.id = o.value(QStringLiteral("id")).toString();

        // `unknownKind`, not `badFacets`: the facets are the series, and a measure whose KIND was
        // misread has not failed the validity table — it has been filed as the other sort of thing
        // entirely, which is what decides whether the series is even consulted.
        if (!measureKindFromName(o.value(QStringLiteral("kind")).toString(QStringLiteral("composed")), m.kind))
            err(r, QStringLiteral("unknownKind"), m.id,
                QStringLiteral("Measure '%1' declares kind '%2'; the kinds are composed and provided.")
                    .arg(m.id, o.value(QStringLiteral("kind")).toString()));

        QString why;
        if (m.kind == MeasureKind::Composed
            && !readSeries(o.value(QStringLiteral("series")).toObject(), m.series, why))
            err(r, QStringLiteral("badFacets"), m.id, QStringLiteral("Measure '%1': %2").arg(m.id, why));
        if (!readReducer(o.value(QStringLiteral("reducer")).toObject(), m.reducer, why))
            err(r, QStringLiteral("badReducer"), m.id, QStringLiteral("Measure '%1': %2").arg(m.id, why));

        m.metricKey  = o.value(QStringLiteral("metricKey")).toString();
        m.preferKeys = readStringList(o.value(QStringLiteral("preferKeys")));
        m.label     = o.value(QStringLiteral("label")).toString();
        m.aliases   = readStringList(o.value(QStringLiteral("aliases")));
        m.unit      = o.value(QStringLiteral("unit")).toString();
        m.gapReason = o.value(QStringLiteral("gapReason")).toString();
        m.highMeans = o.value(QStringLiteral("highMeans")).toString();

        m.unwatchedReason = o.value(QStringLiteral("unwatchedReason")).toString();

        // Unknown token is an ERROR for the reason `shape` below is: a measure that meant "high" and
        // typed "hgih" would silently keep reporting the tail it was written to explain away.
        if (o.contains(QStringLiteral("unwatchedTail"))) {
            Direction d = Direction::High;
            if (directionFromName(o.value(QStringLiteral("unwatchedTail")).toString(), d))
                m.unwatchedTail = d;
            else
                err(r, QStringLiteral("unknownDirection"), m.id,
                    QStringLiteral("Measure '%1' says its '%2' tail is deliberately unwatched; the "
                                   "tails are high and low.")
                        .arg(m.id, o.value(QStringLiteral("unwatchedTail")).toString()));
        }

        // An absent `viewNeeded` on a Composed measure is DERIVED from the series rather than
        // defaulted — the facets already say which camera can see the thing — and a misspelt one is
        // an error like every other closed vocabulary here. The two are not the same case: derived
        // is an answer, and a fall back to Any would be a claim that any camera will do, which is
        // how a measure that needs the down-the-line view ends up asked for on a face-on capture.
        if (o.contains(QStringLiteral("viewNeeded"))) {
            if (!viewNeededFromName(o.value(QStringLiteral("viewNeeded")).toString(), m.viewNeeded))
                err(r, QStringLiteral("unknownViewNeeded"), m.id,
                    QStringLiteral("Measure '%1' declares viewNeeded '%2'; the views are any, faceOn "
                                   "and downTheLine.")
                        .arg(m.id, o.value(QStringLiteral("viewNeeded")).toString()));
        } else if (m.kind == MeasureKind::Composed) {
            m.viewNeeded = deriveViewNeeded(m.series);
        }

        // Absent => Live, and a misspelt token must not land there: `status` is what the roadmap,
        // the capture-gap classification and `normNotCapturable` all read, so a measure that meant
        // "notCapturable" and typed "notCaptureable" would carry a norm nothing can ever grade and
        // report itself as working.
        if (o.contains(QStringLiteral("status"))
            && !measureStatusFromName(o.value(QStringLiteral("status")).toString(), m.status))
            err(r, QStringLiteral("unknownStatus"), m.id,
                QStringLiteral("Measure '%1' declares status '%2'; the statuses are live, planned, "
                               "noProducer, notCapturable and externalDevice.")
                    .arg(m.id, o.value(QStringLiteral("status")).toString()));

        // Absent => Target, which is what every measure authored before shapes existed means. An
        // unknown token is an ERROR rather than a silent fall back to Target: a pack that meant
        // "floor" and typed "flooor" would grade the good tail as a fault, confidently, with no
        // sign anywhere that a word had been misread.
        if (o.contains(QStringLiteral("shape"))
            && !shapeFromName(o.value(QStringLiteral("shape")).toString(), m.shape))
            err(r, QStringLiteral("unknownShape"), m.id,
                QStringLiteral("Measure '%1' declares shape '%2'; the shapes are target, floor and "
                               "ceiling.")
                    .arg(m.id, o.value(QStringLiteral("shape")).toString()));

        // A composed measure naming a spinal role cannot come from the pose SKELETON, and the
        // loader stamps that rather than trusting the author to remember — a measure claiming to
        // be live on a role with no keypoint would silently never produce a value.
        //
        // It is NoProducer, not NotCapturable: the back contour of a down-the-line silhouette shows
        // both the thoracic round and the lumbar arch, so this is a producer worth building rather
        // than a gap that can never close. An author's own weaker status is respected; only an
        // over-claim is corrected.
        if (m.kind == MeasureKind::Composed && seriesNeedsNonPoseSensor(m.series)) {
            // The reason is attached whatever the authored status says — it explains a permanent
            // property of the series, not the correction. Only an OVER-claim is corrected.
            if (m.gapReason.isEmpty())
                m.gapReason = QStringLiteral("No keypoint exists between the shoulders and the hips "
                                             "in any pose layout, so this cannot come from the "
                                             "skeleton — it needs a back-contour producer on the "
                                             "down-the-line view.");
            if (m.status == MeasureStatus::Live || m.status == MeasureStatus::Planned)
                m.status = MeasureStatus::NoProducer;
        }

        if (m.label.isEmpty() && m.kind == MeasureKind::Composed)
            m.label = canonicalMeasureLabel(m.series, m.reducer);
        if (m.unit.isEmpty() && m.kind == MeasureKind::Composed)
            m.unit = quantityUnitHint(m.series.quantity);

        out.pack.measures.push_back(std::move(m));
    }

    // --- signals -------------------------------------------------------------
    for (const QJsonValue &v : root.value(QStringLiteral("signals")).toArray()) {
        const QJsonObject o = v.toObject();
        Signal            s;
        s.id       = o.value(QStringLiteral("id")).toString();
        s.measures = readStringList(o.value(QStringLiteral("measures")));

        // `unknownSignalTest`, not `signalArity`: arity is a count, and a misread test has not
        // brought the wrong number of measures — it has silently become an outsideCorridor test,
        // which then inherits a corridor the author never meant it to grade against.
        if (!signalTestFromName(o.value(QStringLiteral("test")).toString(QStringLiteral("outsideCorridor")), s.test))
            err(r, QStringLiteral("unknownSignalTest"), s.id,
                QStringLiteral("Signal '%1' declares test '%2'; the tests are outsideCorridor, "
                               "threshold, order and ratio.")
                    .arg(s.id, o.value(QStringLiteral("test")).toString()));

        if (o.contains(QStringLiteral("direction"))) {
            Direction d{};
            if (directionFromName(o.value(QStringLiteral("direction")).toString(), d)) s.direction = d;
            else err(r, QStringLiteral("signalDirection"), s.id,
                     QStringLiteral("Signal '%1' has an unknown direction.").arg(s.id));
        }
        if (o.contains(QStringLiteral("threshold")))
            s.threshold = o.value(QStringLiteral("threshold")).toDouble();

        out.pack.signalDefs.push_back(std::move(s));
    }

    // --- conditions ----------------------------------------------------------
    for (const QJsonValue &v : root.value(QStringLiteral("conditions")).toArray()) {
        const QJsonObject o = v.toObject();
        Condition         c;
        c.id         = o.value(QStringLiteral("id")).toString();
        c.label      = o.value(QStringLiteral("label")).toString();
        c.aliases    = readStringList(o.value(QStringLiteral("aliases")));
        c.axis       = o.value(QStringLiteral("axis")).toString();
        c.detectedBy = readStringList(o.value(QStringLiteral("detectedBy")));
        // Absent => Any, so every condition authored before conjunctions existed keeps combining
        // its signals exactly as it did. An unknown token is an error rather than a silent fall
        // back to Any: "all" misread as "any" turns a conjunction into three separate faults that
        // each fire on their own, which is the loudest possible way to be wrong.
        if (o.contains(QStringLiteral("detection"))
            && !detectionModeFromName(o.value(QStringLiteral("detection")).toString(), c.detection))
            err(r, QStringLiteral("unknownDetection"), c.id,
                QStringLiteral("Condition '%1' declares detection '%2'; the modes are any and all.")
                    .arg(c.id, o.value(QStringLiteral("detection")).toString()));
        c.screenRef  = o.value(QStringLiteral("screenRef")).toString();
        c.drills     = readStringList(o.value(QStringLiteral("drills")));
        c.consequence = readLocalised(o.value(QStringLiteral("consequence")));
        c.injuryNote  = readLocalised(o.value(QStringLiteral("injuryNote")));
        c.supersededBy = o.value(QStringLiteral("supersededBy")).toString();

        // ONE REGIME for every closed vocabulary a condition carries: absent means the documented
        // default, a MISSPELT token is an error.
        //
        // Group, observability, confirmedBy and state swallowed an unknown token silently until
        // now, on the argument that kind and prominence were the two worth refusing because the
        // kind rules go on to accuse a misfiled row of the wrong defect. That was right about kind
        // and wrong about the split. `postrue` lands in Setup, where the group decides which section of
        // the browser the condition appears in and which coaching context it is read against;
        // `screend` lands in Measured, where `inconsistentReach` and `screenedHasCause` then reason
        // about a condition that is not reached the way they think; `latnet` lands in Observable,
        // which is the input to `observableNoSignal` and `faultNotObservable` both. Every one of
        // them is a token whose misreading is INVISIBLE — the row loads, renders, and validates
        // clean against the wrong answer. There is no version of that which is worth a silent
        // default, and there never was: the four were unguarded because nobody had written the
        // guard, not because anybody had argued they should be.
        //
        // Every message names the FIELD, because `setup` is a legal token of two different enums
        // here and `kind` is the third meaning of that JSON key in this file (Measure::kind,
        // Reducer::kind).
        if (o.contains(QStringLiteral("group"))
            && !conditionGroupFromName(o.value(QStringLiteral("group")).toString(), c.group))
            err(r, QStringLiteral("unknownGroup"), c.id,
                QStringLiteral("Condition '%1' declares group '%2'; the groups are setup, posture, "
                               "lateral, armsAndClub, release, sequence, impact, finish and "
                               "ballFlight.")
                    .arg(c.id, o.value(QStringLiteral("group")).toString()));

        if (o.contains(QStringLiteral("observability"))
            && !observabilityFromName(o.value(QStringLiteral("observability")).toString(),
                                      c.observability))
            err(r, QStringLiteral("unknownObservability"), c.id,
                QStringLiteral("Condition '%1' declares observability '%2'; it is observable, latent "
                               "or both.")
                    .arg(c.id, o.value(QStringLiteral("observability")).toString()));

        if (o.contains(QStringLiteral("confirmedBy"))
            && !confirmedByFromName(o.value(QStringLiteral("confirmedBy")).toString(), c.confirmedBy))
            err(r, QStringLiteral("unknownConfirmedBy"), c.id,
                QStringLiteral("Condition '%1' declares confirmedBy '%2'; it is measured, screened "
                               "or asserted.")
                    .arg(c.id, o.value(QStringLiteral("confirmedBy")).toString()));

        if (o.contains(QStringLiteral("state"))
            && !conditionStateFromName(o.value(QStringLiteral("state")).toString(), c.state))
            err(r, QStringLiteral("unknownState"), c.id,
                QStringLiteral("Condition '%1' declares state '%2'; the states are draft, candidate, "
                               "active, needsRevalidation, superseded and retired.")
                    .arg(c.id, o.value(QStringLiteral("state")).toString()));

        if (o.contains(QStringLiteral("kind"))
            && !conditionKindFromName(o.value(QStringLiteral("kind")).toString(), c.kind))
            err(r, QStringLiteral("unknownConditionKind"), c.id,
                QStringLiteral("Condition '%1' declares kind '%2'; the kinds are fault, setup, "
                               "delivery, outcome, capacity, intent and equipment.")
                    .arg(c.id, o.value(QStringLiteral("kind")).toString()));

        if (o.contains(QStringLiteral("prominence"))
            && !prominenceFromName(o.value(QStringLiteral("prominence")).toString(), c.prominence))
            err(r, QStringLiteral("unknownProminence"), c.id,
                QStringLiteral("Condition '%1' declares prominence '%2'; the rungs are rare, "
                               "uncommon, occasional, common and ubiquitous.")
                    .arg(c.id, o.value(QStringLiteral("prominence")).toString()));

        c.provenance = parseProvenance(o.value(QStringLiteral("provenance")).toObject());
        // A tier is only as good as its citation. An author cannot claim Supported without one.
        if (citationRequired(c.provenance.tier) && c.provenance.citation.isEmpty()) {
            c.provenance.tier = ProvenanceTier::Proposed;
            warn(r, QStringLiteral("proposedTier"), c.id,
                 QStringLiteral("'%1' claimed a citation tier with no citation; demoted to proposed.")
                     .arg(c.id));
        }
        // …and a citation is only as good as what it points AT. See demoteBookCitation().
        if (demoteBookCitation(c.provenance.tier, c.provenance.citation))
            warn(r, QStringLiteral("bookCitationAtPeerTier"), c.id,
                 QStringLiteral("'%1' claims a peer-reviewed tier on a citation that resolves to a "
                                "book; demoted to indirect.").arg(c.id));
        // NoSourceFound is the one tier that is MEANT to have no citation, and demoting it would
        // destroy the finding it records — turning "we looked and the literature is silent" back
        // into "nobody has looked". What it needs instead is the date that makes it re-openable.
        if (searchDateRequired(c.provenance.tier) && !c.provenance.searched()) {
            c.provenance.tier = ProvenanceTier::Proposed;
            warn(r, QStringLiteral("searchNoDate"), c.id,
                 QStringLiteral("'%1' records the outcome of a search with no date to have made it "
                                "on; an undated result cannot be told from an unasked question.")
                     .arg(c.id));
        }

        for (const QJsonValue &bv : o.value(QStringLiteral("bindings")).toArray()) {
            const QJsonObject bo = bv.toObject();
            ContextBinding    b;
            b.context     = bo.value(QStringLiteral("context")).toString();
            b.applicable  = bo.value(QStringLiteral("applicable")).toBool(true);
            b.material    = bo.value(QStringLiteral("material")).toBool(true);
            // `corridorRef` was read here until stage 7. A pack that still carries the key loads
            // cleanly and drops it on the next save: the corridor a signal grades against is
            // resolved by the (measureId, contextId) norm join, so the key never meant anything a
            // reader could act on.
            b.consequence = readLocalised(bo.value(QStringLiteral("consequence")));
            c.bindings.push_back(std::move(b));
        }

        out.pack.conditions.push_back(std::move(c));
    }

    // --- edges ---------------------------------------------------------------
    for (const QJsonValue &v : root.value(QStringLiteral("edges")).toArray()) {
        const QJsonObject o = v.toObject();
        Edge              e;
        e.from = o.value(QStringLiteral("from")).toString();
        e.to   = o.value(QStringLiteral("to")).toString();
        edgeTypeFromName(o.value(QStringLiteral("type")).toString(QStringLiteral("causes")), e.type);
        strengthFromName(o.value(QStringLiteral("strength")).toString(QStringLiteral("moderate")), e.strength);

        e.provenance = parseProvenance(o.value(QStringLiteral("provenance")).toObject());
        // An edge used to carry a bare `citation` string with no tier. No shipped edge ever did,
        // but a community pack written against the old schema might, and dropping it silently
        // would lose the one thing that schema COULD say. A citation with no tier is a claim
        // somebody made and did not grade, which is exactly what Supported means.
        if (e.provenance.citation.isEmpty()) {
            const QString legacy = o.value(QStringLiteral("citation")).toString();
            if (!legacy.isEmpty()) {
                e.provenance.citation = legacy;
                if (e.provenance.tier == ProvenanceTier::Proposed)
                    e.provenance.tier = ProvenanceTier::Supported;
            }
        }
        if (citationRequired(e.provenance.tier) && e.provenance.citation.isEmpty()) {
            e.provenance.tier = ProvenanceTier::Proposed;
            warn(r, QStringLiteral("edgeTierNoCitation"), e.from,
                 QStringLiteral("The edge '%1' -> '%2' claims a citation tier with no citation; "
                                "demoted to proposed.").arg(e.from, e.to));
        }
        // …and a citation is only as good as what it points AT. See demoteBookCitation().
        if (demoteBookCitation(e.provenance.tier, e.provenance.citation))
            warn(r, QStringLiteral("bookCitationAtPeerTier"), e.from,
                 QStringLiteral("The edge '%1' -> '%2' claims a peer-reviewed tier on a citation "
                                "that resolves to a book; demoted to indirect.").arg(e.from, e.to));
        if (searchDateRequired(e.provenance.tier) && !e.provenance.searched()) {
            e.provenance.tier = ProvenanceTier::Proposed;
            warn(r, QStringLiteral("searchNoDate"), e.from,
                 QStringLiteral("The edge '%1' -> '%2' records the outcome of a search with no date "
                                "to have made it on; an undated result cannot be told from an "
                                "unasked question.").arg(e.from, e.to));
        }
        out.pack.edges.push_back(std::move(e));
    }

    // --- retired edges -------------------------------------------------------
    // Tombstones, not edges: they name something to REMOVE from the layers beneath, so they are not
    // validated for referential integrity. An id that no longer exists anywhere is a tombstone for
    // content that has already gone, which is harmless and must not fail the load.
    for (const QJsonValue &v : root.value(QStringLiteral("retiredEdges")).toArray()) {
        const QJsonObject o = v.toObject();
        Edge              e;
        e.from = o.value(QStringLiteral("from")).toString();
        e.to   = o.value(QStringLiteral("to")).toString();
        edgeTypeFromName(o.value(QStringLiteral("type")).toString(QStringLiteral("causes")), e.type);
        if (!e.from.isEmpty() && !e.to.isEmpty()) out.pack.retiredEdges.push_back(std::move(e));
    }

    out.parsed = true;

    const ValidationReport structural = validatePack(out.pack);
    r.issues.insert(r.issues.end(), structural.issues.begin(), structural.issues.end());

    out.loaded = r.ok();
    return out;
}

PackLoadResult loadPack(const QByteArray &json, const QString &sourceLabel)
{
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        PackLoadResult out;
        err(out.report, QStringLiteral("parse"), sourceLabel,
            QStringLiteral("Could not parse pack '%1': %2").arg(sourceLabel, pe.errorString()));
        return out;
    }
    return loadPack(doc.object(), sourceLabel);
}

QJsonObject savePack(const CharacteristicPack &pack)
{
    QJsonObject root;
    root.insert(QStringLiteral("id"), pack.id);
    root.insert(QStringLiteral("version"), pack.version);
    root.insert(QStringLiteral("schemaVersion"), pack.schemaVersion);

    QJsonArray measures;
    for (const Measure &m : pack.measures) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), m.id);
        o.insert(QStringLiteral("kind"), measureKindName(m.kind));
        if (m.kind == MeasureKind::Composed)
            o.insert(QStringLiteral("series"), writeSeries(m.series));
        o.insert(QStringLiteral("reducer"), writeReducer(m.reducer));
        if (!m.metricKey.isEmpty()) o.insert(QStringLiteral("metricKey"), m.metricKey);
        if (!m.preferKeys.isEmpty()) o.insert(QStringLiteral("preferKeys"), writeStringList(m.preferKeys));
        if (!m.label.isEmpty())     o.insert(QStringLiteral("label"), m.label);
        if (!m.aliases.isEmpty())   o.insert(QStringLiteral("aliases"), writeStringList(m.aliases));
        if (!m.unit.isEmpty())      o.insert(QStringLiteral("unit"), m.unit);
        if (!m.gapReason.isEmpty()) o.insert(QStringLiteral("gapReason"), m.gapReason);
        if (!m.highMeans.isEmpty()) o.insert(QStringLiteral("highMeans"), m.highMeans);
        o.insert(QStringLiteral("viewNeeded"), viewNeededName(m.viewNeeded));
        o.insert(QStringLiteral("status"), measureStatusName(m.status));
        // Omitted when Target, so 105 of 106 shipped measures round-trip byte-identically and the
        // key appears only where somebody actually said something.
        if (m.shape != Shape::Target) o.insert(QStringLiteral("shape"), shapeName(m.shape));
        if (m.unwatchedTail.has_value())
            o.insert(QStringLiteral("unwatchedTail"), directionName(*m.unwatchedTail));
        if (!m.unwatchedReason.isEmpty())
            o.insert(QStringLiteral("unwatchedReason"), m.unwatchedReason);
        measures.append(o);
    }
    root.insert(QStringLiteral("measures"), measures);

    QJsonArray signalArray;
    for (const Signal &s : pack.signalDefs) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), s.id);
        o.insert(QStringLiteral("test"), signalTestName(s.test));
        o.insert(QStringLiteral("measures"), writeStringList(s.measures));
        if (s.direction.has_value()) o.insert(QStringLiteral("direction"), directionName(*s.direction));
        if (s.threshold.has_value()) o.insert(QStringLiteral("threshold"), *s.threshold);
        signalArray.append(o);
    }
    root.insert(QStringLiteral("signals"), signalArray);

    QJsonArray conditions;
    for (const Condition &c : pack.conditions) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), c.id);
        o.insert(QStringLiteral("label"), c.label);
        if (!c.axis.isEmpty()) o.insert(QStringLiteral("axis"), c.axis);
        o.insert(QStringLiteral("group"), conditionGroupName(c.group));
        // Written unconditionally, beside the three fields they belong with — NOT omitted when they
        // hold the default, the way `shape` is. That omission exists to keep 105 of 106 measures
        // byte-identical across a save, and the argument evaporates here: every shipped condition
        // authors both, so omitting the default would mean the 69 hand-written `"kind": "fault"`
        // rows are silently dropped by the writer and the file disagrees with its own round-trip.
        o.insert(QStringLiteral("kind"), conditionKindName(c.kind));
        o.insert(QStringLiteral("prominence"), prominenceName(c.prominence));
        o.insert(QStringLiteral("observability"), observabilityName(c.observability));
        o.insert(QStringLiteral("confirmedBy"), confirmedByName(c.confirmedBy));
        if (!c.detectedBy.isEmpty()) o.insert(QStringLiteral("detectedBy"), writeStringList(c.detectedBy));
        if (c.detection != DetectionMode::Any)
            o.insert(QStringLiteral("detection"), detectionModeName(c.detection));
        if (!c.screenRef.isEmpty())  o.insert(QStringLiteral("screenRef"), c.screenRef);
        if (!c.consequence.isEmpty()) o.insert(QStringLiteral("consequence"), writeLocalised(c.consequence));
        if (!c.injuryNote.isEmpty())  o.insert(QStringLiteral("injuryNote"), writeLocalised(c.injuryNote));
        if (!c.drills.isEmpty())      o.insert(QStringLiteral("drills"), writeStringList(c.drills));
        if (!c.aliases.isEmpty())     o.insert(QStringLiteral("aliases"), writeStringList(c.aliases));

        // Through the shared writer, which omits a default tier — but a condition writes its
        // provenance block unconditionally, because every condition HAS a tier claim to make and a
        // missing block on one of them would read as content that lost its provenance rather than
        // content that never claimed any.
        QJsonObject prov = writeProvenance(c.provenance);
        if (!prov.contains(QStringLiteral("tier")))
            prov.insert(QStringLiteral("tier"), provenanceTierName(c.provenance.tier));
        o.insert(QStringLiteral("provenance"), prov);

        if (!c.bindings.empty()) {
            QJsonArray bs;
            for (const ContextBinding &b : c.bindings) {
                QJsonObject bo;
                bo.insert(QStringLiteral("context"), b.context);
                bo.insert(QStringLiteral("applicable"), b.applicable);
                bo.insert(QStringLiteral("material"), b.material);
                if (!b.consequence.isEmpty())  bo.insert(QStringLiteral("consequence"), writeLocalised(b.consequence));
                bs.append(bo);
            }
            o.insert(QStringLiteral("bindings"), bs);
        }

        o.insert(QStringLiteral("state"), conditionStateName(c.state));
        if (!c.supersededBy.isEmpty()) o.insert(QStringLiteral("supersededBy"), c.supersededBy);
        conditions.append(o);
    }
    root.insert(QStringLiteral("conditions"), conditions);

    QJsonArray edges;
    for (const Edge &e : pack.edges) {
        QJsonObject o;
        o.insert(QStringLiteral("from"), e.from);
        o.insert(QStringLiteral("to"), e.to);
        o.insert(QStringLiteral("type"), edgeTypeName(e.type));
        o.insert(QStringLiteral("strength"), strengthName(e.strength));
        const QJsonObject prov = writeProvenance(e.provenance);
        if (!prov.isEmpty()) o.insert(QStringLiteral("provenance"), prov);
        edges.append(o);
    }
    root.insert(QStringLiteral("edges"), edges);

    if (!pack.retiredEdges.empty()) {
        QJsonArray retired;
        for (const Edge &e : pack.retiredEdges) {
            QJsonObject o;
            o.insert(QStringLiteral("from"), e.from);
            o.insert(QStringLiteral("to"), e.to);
            o.insert(QStringLiteral("type"), edgeTypeName(e.type));
            retired.append(o);
        }
        root.insert(QStringLiteral("retiredEdges"), retired);
    }

    return root;
}

} // namespace pinpoint::analysis
