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

#include "measure_facets.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

// The value types of a characteristic pack. Header-only, Qt-only, no Qt-GUI.
//
// The one structural idea that shapes everything here: FAULTS AND CAUSES ARE THE SAME TYPE. A
// Condition is a named state of the swing or the golfer; whether it is a fault or a cause is
// resolved per-swing by the explanation pass and is never stored. That is what lets a condition be
// cited by several characteristics AND have causes of its own — early extension causes loss of
// posture, and both are conditions.

namespace pinpoint::analysis {

// ── Localised narrative ─────────────────────────────────────────────────────
// Narrative strings are keyed by locale from the start. Localisation itself is out of scope, but
// retrofitting the key later would be a schema migration across every authored pack.
struct LocalisedText {
    QHash<QString, QString> byLocale;   // "en" -> "…"

    bool    isEmpty() const { return byLocale.isEmpty(); }
    QString text(const QString &locale = QStringLiteral("en")) const
    {
        const auto it = byLocale.constFind(locale);
        if (it != byLocale.constEnd()) return it.value();
        const auto en = byLocale.constFind(QStringLiteral("en"));
        return en != byLocale.constEnd() ? en.value() : QString();
    }
};

// ── Measure ─────────────────────────────────────────────────────────────────
enum class MeasureKind {
    Composed,   // built from facets: a Series sampled by a Reducer
    Provided,   // a first-class metric we ship with a producer (tempo ratio, X-factor, sequence)
};

enum class MeasureStatus {
    Live,           // a producer exists today
    Planned,        // a producer is planned
    NoProducer,     // nothing produces it yet — ROADMAP work, someone could pick this up
    NotCapturable,  // no sensor this product has can ever resolve it — a CAPTURE GAP, not roadmap
};

// The distinction between NoProducer and NotCapturable is the whole reason the roadmap can be
// trusted as a work queue. NoProducer means "we could build this"; NotCapturable means "a different
// sensor or view is required, and listing it as missing pipeline work would mislead every reader".
struct Measure {
    QString       id;                                   // stable, never reused
    MeasureKind   kind   = MeasureKind::Composed;
    Series        series;                               // Composed only — the facet-built series
    // The reducer applies to BOTH kinds. A Provided measure names its series with a metricKey
    // instead of facets, but a catalogue TimeSeries is still a curve: metric_type.h describes it as
    // "reduced to peak / @impact / Δ / rate by the chart layer". Three characteristics can sit on
    // one live series at different phases (sway, slide and hanging back are all pelvis lateral
    // displacement), and modelling that as three separate metrics would be the parallel registry
    // this design exists to avoid. For a PointInTime or Summary metric the reducer is At().
    Reducer       reducer;
    QString       metricKey;                            // link into MetricCatalogue (Provided)
    QString       label;                                // canonical for Composed, authored for Provided
    QStringList   aliases;                              // coach phrasing that resolved here; grows with use
    QString       unit;
    ViewNeeded    viewNeeded = ViewNeeded::Any;
    MeasureStatus status     = MeasureStatus::NoProducer;
    QString       gapReason;                            // NotCapturable: why, in one line, for the UI
};

// ── Signal ──────────────────────────────────────────────────────────────────
// Signals are purely COMPARATIVE. Change and rate are not tests — they are reducers, and live on
// the measure. One place expresses time.
enum class SignalTest {
    OutsideCorridor,   // preferred: authors no numbers, inherits the catalogue's NormativeCorridor
    Threshold,         // an authored number; needs a citation to be more than an opinion
    Order,             // two measures, temporal ordering (kinematic sequence)
    Ratio,             // two measures, a ratio (tempo)
};

// Which tail of the corridor fired. Required for the corridor and threshold tests: a condition is
// one tail of one measure, so a signal without a direction cannot identify a condition.
enum class Direction { High, Low };

struct Signal {
    QString                  id;
    SignalTest               test = SignalTest::OutsideCorridor;
    QStringList              measures;     // 1 for corridor/threshold, 2 for order/ratio
    std::optional<Direction> direction;
    std::optional<double>    threshold;    // ONLY when test == Threshold
};

// ── Condition ───────────────────────────────────────────────────────────────
enum class ConditionGroup { Setup, Posture, Lateral, ArmsAndClub, Release, Sequence };

enum class Observability {
    Observable,   // it can be seen in the swing
    Latent,       // it cannot; it is inferred from what it explains
    Both,
};

// How a condition can be established. The UI must never blur these three.
enum class ConfirmedBy {
    Measured,   // a signal fired — the app knows
    Screened,   // a physical screen confirms or refutes it; unknown until entered. NEVER measurable
                // by this product, so never a roadmap item.
    Asserted,   // only the coach or golfer can confirm it — intent, habit, perception. The app may
                // OFFER it as an explanation; it must never conclude it.
};

enum class ProvenanceTier {
    Proposed,     // no citation. The UI must badge it as such wherever it appears.
    Supported,    // a peer-reviewed source supports the direction/phase
    Established,  // consistently reproduced across sources
};

// Authoring lifecycle. The transitions are modelled now so a backtest harness can later gate
// draft -> candidate without a schema change.
enum class ConditionState { Draft, Candidate, Active, NeedsRevalidation, Superseded, Retired };

struct Provenance {
    QString        author;
    QString        citation;   // DOI or PMID. NEVER a commercial organisation, product or
                               // certification body — the domain terms are common property, the
                               // attributions are not.
    ProvenanceTier tier = ProvenanceTier::Proposed;
};

// Context binding. There is deliberately NO valence field and there must never be one: context
// never inverts the sign of a finding. Over-the-top is over-the-top in a bunker; reporting it is
// factual, calling it good or bad is a coaching judgement that belongs to the coach.
struct ContextBinding {
    QString       context;                 // context id (see the context tree)
    bool          applicable = true;
    bool          material   = true;       // RANKING WEIGHT ONLY — never "beneficial here"
    QString       corridorRef;             // which reference corridor to grade against; may be empty
    LocalisedText consequence;             // override only where the MECHANICS genuinely differ
};

struct Condition {
    QString                     id;                    // stable, never reused
    QString                     label;
    QString                     axis;                  // joins the two tails of one measure; may be empty
    ConditionGroup              group        = ConditionGroup::Setup;
    Observability               observability = Observability::Observable;
    QStringList                 detectedBy;            // signal ids; empty => Latent
    ConfirmedBy                 confirmedBy  = ConfirmedBy::Measured;
    QString                     screenRef;             // screen.* namespace; no UI in v1
    LocalisedText               consequence;
    LocalisedText               injuryNote;            // separate axis from performance; conservative
    QStringList                 drills;
    std::vector<ContextBinding> bindings;
    Provenance                  provenance;
    ConditionState              state = ConditionState::Draft;
    QString                     supersededBy;
};

// ── Edge ────────────────────────────────────────────────────────────────────
enum class EdgeType {
    Causes,        // from CAUSES to
    Corroborates,  // independent confirmation, NO causal claim. Illegal between conditions that
                   // already have a causal path — a pair that both causes and corroborates would
                   // double-count in the confidence ranking.
    Excludes,
};

// Three-valued and never continuous: nobody can author 0.73 meaningfully, and rendering strength as
// a percentage would imply a probability it is not.
enum class Strength { Weak, Moderate, Strong };

// ORIENTATION, NORMATIVE: `from` is the CAUSE, `to` is the EFFECT.
//
// The seed content's tables are written the way a coach reads them — the characteristic first, then
// what causes it — which is the reverse of this. Every row flips on transcription. This matters
// because no downstream count can catch the mistake: cause-coverage totals are identical under edge
// reversal, so a wholly inverted graph passes every coverage assertion. The structural check lives
// in the pack validator (screened causes have out-degree > 0 and in-degree 0).
struct Edge {
    QString  from;
    QString  to;
    EdgeType type     = EdgeType::Causes;
    Strength strength = Strength::Moderate;
    QString  citation;
};

// ── Pack ────────────────────────────────────────────────────────────────────
struct CharacteristicPack {
    QString                 id;             // "core", or a community pack's namespace
    QString                 version;
    int                     schemaVersion = 1;
    QString                 sourceLabel;    // where it was loaded from, for the UI
    bool                    readOnly = false;   // the shipped core pack is not editable in place

    std::vector<Measure>    measures;
    // Not `signals`: Qt's moc keyword macro expands that to an access specifier, so a member of
    // that name breaks every translation unit that includes both this header and QObject.
    std::vector<Signal>     signalDefs;
    std::vector<Condition>  conditions;
    std::vector<Edge>       edges;

    const Measure   *measure(const QString &id) const;
    const Signal    *signal(const QString &id) const;
    const Condition *condition(const QString &id) const;
};

// ── Enum <-> string (the JSON spelling) ─────────────────────────────────────
QString measureKindName(MeasureKind k);
bool    measureKindFromName(const QString &s, MeasureKind &out);
QString measureStatusName(MeasureStatus s);
bool    measureStatusFromName(const QString &s, MeasureStatus &out);
QString signalTestName(SignalTest t);
bool    signalTestFromName(const QString &s, SignalTest &out);
QString directionName(Direction d);
bool    directionFromName(const QString &s, Direction &out);
QString conditionGroupName(ConditionGroup g);
QString conditionGroupLabel(ConditionGroup g);
bool    conditionGroupFromName(const QString &s, ConditionGroup &out);
QString observabilityName(Observability o);
bool    observabilityFromName(const QString &s, Observability &out);
QString confirmedByName(ConfirmedBy c);
bool    confirmedByFromName(const QString &s, ConfirmedBy &out);
QString provenanceTierName(ProvenanceTier t);
bool    provenanceTierFromName(const QString &s, ProvenanceTier &out);
QString conditionStateName(ConditionState s);
bool    conditionStateFromName(const QString &s, ConditionState &out);
QString edgeTypeName(EdgeType t);
bool    edgeTypeFromName(const QString &s, EdgeType &out);
QString strengthName(Strength s);
QString strengthLabel(Strength s);      // words, never a percentage
bool    strengthFromName(const QString &s, Strength &out);

// The UI badge for how a condition can be reached. "Physical" and "Behavioural" are preferred over
// "biomechanical", which does not discriminate the body's CAPACITY from what it did with the club —
// over-the-top is biomechanical too.
QString reachLabel(ConfirmedBy c);
QString reachHint(ConfirmedBy c);

// True when this condition can never be established by capture, so it must not appear in the
// measure roadmap however many characteristics it blocks.
inline bool isOutsideCaptureReach(ConfirmedBy c)
{
    return c == ConfirmedBy::Screened || c == ConfirmedBy::Asserted;
}

} // namespace pinpoint::analysis
