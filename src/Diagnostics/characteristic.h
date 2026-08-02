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

#include <QDate>
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
    ExternalDevice, // a producer is intended, but it reads from hardware the user may not own
};

// Which way a measure's corridor is open.
//
// ON THE MEASURE, NOT THE NORM, and that is the load-bearing decision. One-sidedness is semantics
// of the QUANTITY and invariant across contexts: smash factor has no upper fault with a driver and
// none with a wedge either, only a different implausibility. If shape sat on the norm, a driver row
// and an iron row could disagree about the shape of one measure and the corridor editor could flip
// shape per context — so a norm row carries only NUMBERS, and validateNormsAgainst checks those
// numbers against the measure's shape exactly the way it already checks `unit`.
//
// This changes which values reach which band. It does NOT add a band: Ideal/Good/Watch/Action/
// NotMeasured and ragOf() are untouched, and a one-sided norm consults the same z ladder on its one
// graded tail.
//
// The open tail is open UP TO PLAUSIBILITY, not to infinity — see Norm::plausibleLo/plausibleHi. A
// driver smash of 1.62 is not a swing finding, it is a mis-tracked ball, and grading it either way
// would launder a capture fault into a confident diagnosis.
//
// `Floor` is for self-normalising ratios and mechanically-universal thresholds ONLY. Cohort-relative
// bigger-is-better outcomes — ball speed, club speed, carry — get no norm at all: a population floor
// on those grades a golfer Action for their age. They are benchmarks, a different feature.
enum class Shape {
    Target,    // the default and ~90% of the catalogue: a corridor, both tails grade
    Floor,     // higher is better. mu is the aspiration; only the LOW tail grades
    Ceiling,   // lower is better. Mirror of Floor; only the HIGH tail grades
};

// The distinction between NoProducer and NotCapturable is the whole reason the roadmap can be
// trusted as a work queue. NoProducer means "we could build this"; NotCapturable means "a different
// sensor or view is required, and listing it as missing pipeline work would mislead every reader".
//
// ExternalDevice is the third answer, and it is not a shade of either. A launch monitor resolves
// face angle, spin and strike location; integrating one is work we intend to do, so calling it
// NotCapturable would be false. But the work is an INTEGRATION, not a producer written from our own
// pixels, and a roadmap row reading "build a spin-axis producer" would send somebody the wrong way.
// So it is roadmap-eligible and sectioned apart. The per-shot half lives on the requirement
// (MetricRequirement::launchMonitor): a user with no launch monitor gets "needs a launch monitor"
// through the same path a missing face-on camera already takes, which is what makes the absence
// graceful rather than a hole. `gapReason` is required here exactly as it is for NotCapturable.
// Which tail of the corridor a signal watches, or a measure deliberately does not. Required for the
// corridor and threshold tests: a condition is one tail of one measure, so a signal without a
// direction cannot identify a condition. Declared here rather than beside Signal because Measure
// needs it too — see `unwatchedTail`.
enum class Direction { High, Low };

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

    // Which way this quantity's corridor is open. See Shape. Absent in JSON => Target, so every
    // measure authored before shapes existed keeps grading exactly as it did.
    Shape         shape      = Shape::Target;

    // What a HIGH value of this measure means, in the measure's own words — "further back, toward
    // the trail foot". Rendered wherever a signal's direction is chosen, INSTEAD of High/Low.
    //
    // This is a correctness mechanism, not decoration. Three signals shipped inverted because an
    // author picked High or Low against a sign convention that was either unstated or the opposite
    // of what they assumed; an inverted signal then fires happily on the wrong swings with
    // correct-sounding consequence text attached. An author who reads "further back, toward the
    // trail foot" cannot make that mistake in the same way. See
    // docs/design/pinpoint_sign_conventions.md.
    QString       highMeans;

    // A tail that GRADES and is deliberately not watched, with the reason it is not.
    //
    // Same contract as `gapReason` for NotCapturable: the field says something is intentional, the
    // reason says what, and the UI quotes the reason. It exists because there are three correct
    // answers to a tail with no condition behind it and `shape` only expresses two of them. Author
    // the condition when the tail carries a fault; set `shape` when the quantity is one-sided and
    // the tail should stop grading altogether; set this when the tail genuinely grades, the reading
    // out there is real, and there is still no fault to name — the corridor is mis-centred, or the
    // deviation is a known artefact of how the number is measured. Silences `ungradedTail` for that
    // tail ONLY; the other tail is unaffected.
    //
    // NOT a quiet way to dismiss a tail. On a one-sided measure it is a contradiction (that tail
    // does not grade, so there is nothing to be unwatched) and on a tail a signal already watches it
    // is the opposite of true — both are load ERRORS, not judgements. And an unwatchedTail with no
    // reason is indistinguishable from a tail nobody got to, which is what `unwatchedTailNoReason`
    // is for.
    std::optional<Direction> unwatchedTail;
    QString                  unwatchedReason;
};

// ── Signal ──────────────────────────────────────────────────────────────────
// Signals are purely COMPARATIVE. Change and rate are not tests — they are reducers, and live on
// the measure. One place expresses time.
enum class SignalTest {
    OutsideCorridor,   // preferred: authors no numbers, inherits the measure's norm
    Threshold,         // an authored number; needs a citation to be more than an opinion
    Order,             // two measures, temporal ordering (kinematic sequence)
    Ratio,             // two measures, a ratio (tempo)
};

// (`Direction` is declared above `Measure`, which needs it for `unwatchedTail`.)

struct Signal {
    QString                  id;
    SignalTest               test = SignalTest::OutsideCorridor;
    QStringList              measures;     // 1 for corridor/threshold, 2 for order/ratio
    std::optional<Direction> direction;
    std::optional<double>    threshold;    // ONLY when test == Threshold
};

// ── Condition ───────────────────────────────────────────────────────────────
//
// The order here is the order the library and the editor list them in, and it is the order of the
// swing: what you set up, what the body does, what the arms and club do, then the strike and what
// the ball did about it. BallFlight sits last because an outcome is where an explanation ENDS —
// the chain runs fault -> strike -> flight, and the golfer reads it from the bottom up.
enum class ConditionGroup {
    Setup, Posture, Lateral, ArmsAndClub, Release, Sequence, Impact, Finish, BallFlight
};

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

// How well sourced a claim is — and, just as importantly, whether anybody has LOOKED.
//
// `Proposed` and `NoSourceFound` are the pair that carries the most information between them.
// Collapsed into one value (which is what shipped for a year) the library cannot distinguish "we
// have not checked this" from "we checked and the literature is silent", and those call for
// opposite actions: the first is a task, the second is a finding. A null result is a result, and it
// is only worth anything if it records WHEN it was taken — see `Provenance::searchedOn`.
//
// `Indirect` exists because of what golf literature actually contains. A paper will establish that
// limited hip internal rotation produces greater lumbopelvic compensation; it will not test
// "early extension", because that is a coaching label and not a measured variable. Filing such a
// source under Supported would make the citation look like it backs the EDGE when it backs the
// reasoning behind the edge — the same overclaim in a different costume.
// `Practice` is the tier for a claim that coaching teaches universally and nobody has tested. That
// is NOT the same as unsupported — a fault every competent coach works from is evidence of a kind,
// and filing it as NoSourceFound would throw away the distinction between "the field is silent" and
// "the field agrees but has never measured it".
//
// It carries no citation WHERE THE SOURCE IS A COMMERCIAL SCREENING OR LAUNCH-MONITOR BODY, and
// there the absence is forced rather than lazy: this repo's standing rule is that the TERMS are
// common domain while the ATTRIBUTION must not enter the content in any form — not a citation, not
// an author, not a note. `core_pack_test` greps the raw bytes for exactly that. So the tier records
// the STATE of the evidence and `searchTerms` records what was looked for; naming who says it is
// what we cannot do, and it is also the part that carries the least information.
//
// It MAY carry one where the source is a PUBLISHED BOOK. Coaching doctrine has a documented
// literature going back decades, an ISBN names it without naming a vendor, and "the field agrees
// but has never measured it" is better evidence when we can say who the field is. No code change
// was needed to permit this — `citationRequired(Practice)` is already false and nothing forbids a
// citation on the tier — so this comment is the code catching up with what it always allowed, and
// it is the thing a future author would otherwise get wrong. The guard that keeps it honest sits
// one tier up: a book citation may not hold `Supported` or `Established` (reference_pack.h).
enum class ProvenanceTier {
    Proposed,       // nobody has searched yet. The UI must badge it wherever it appears.
    NoSourceFound,  // searched, and nothing supports it. Requires `searchedOn` to mean anything.
    Practice,       // established coaching practice; no peer-reviewed test found. No citation.
    Indirect,       // a source supports the MECHANISM; the named pair is our inference
    Supported,      // a peer-reviewed source tests this cause and this effect
    Established,    // consistently reproduced across independent sources
};

// Authoring lifecycle. The transitions are modelled now so a backtest harness can later gate
// draft -> candidate without a schema change.
enum class ConditionState { Draft, Candidate, Active, NeedsRevalidation, Superseded, Retired };

struct Provenance {
    QString        author;
    QString        citation;   // DOI, PMID or ISBN. NEVER a commercial organisation, product or
                               // certification body — the domain terms are common property, the
                               // attributions are not.
    ProvenanceTier tier = ProvenanceTier::Proposed;

    // What was actually done to look, and when. Without these a NoSourceFound is unfalsifiable:
    // nobody can tell a thorough search from a lazy one, or know whether it predates the paper
    // that would have answered it. `searchedOn` is what makes the null re-openable — a 2026 null
    // on a question the field is actively publishing on is worth re-running in 2028, and a null
    // with no date is worth nothing at all.
    QDate          searchedOn;   // null QDate == never searched
    QString        searchTerms;  // the query, so a re-run starts from what was already tried

    bool searched() const { return searchedOn.isValid(); }
};

// Context binding. There is deliberately NO valence field and there must never be one: context
// never inverts the sign of a finding. Over-the-top is over-the-top in a bunker; reporting it is
// factual, calling it good or bad is a coaching judgement that belongs to the coach.
//
// A binding row is an EXCEPTION, not a declaration. Bindings resolve by walking UP the context
// tree exactly as norms do (resolveContextBinding(), context_tree.h): the nearest row on the
// chain wins, and a condition with no row anywhere on the chain applies everywhere and ranks
// normally. That is why 50 shipped conditions carry no bindings at all rather than 50 x 13 rows
// saying "yes" — an author writes a row only where the answer differs from the parent, and a
// reader can then see at a glance which contexts somebody deliberately distinguished.
//
// There was a `corridorRef` field here until stage 7. It was redundant the moment norms keyed on
// (measureId, contextId) — the corridor a signal grades against is found by that join, not named
// by the binding — and nothing ever read it. Removed rather than left as a field a future author
// might reasonably assume was load-bearing.
struct ContextBinding {
    QString       context;                 // context id (see the context tree)
    bool          applicable = true;
    bool          material   = true;       // RANKING WEIGHT ONLY — never "beneficial here"
    LocalisedText consequence;             // override only where the MECHANICS genuinely differ
};

struct Condition {
    QString                     id;                    // stable, never reused
    QString                     label;
    // Coach phrasing that resolves here — "flip", "early release", "standing up", "OTT". One concept
    // has several names and a golfer searches by the one they were taught, so the aliases are how the
    // library is reachable at all for anybody who did not write it. They also carry the glossary:
    // rendering "Scooping — also called flipping: …" needs no second dataset. Two conditions may not
    // claim one term (`duplicateAlias`), or a search would resolve to whichever came first.
    QStringList                 aliases;
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

// Five-valued and never continuous: nobody can author 0.73 meaningfully, and rendering strength as
// a percentage would imply a probability it is not.
//
// The two outer rungs are spelled in the same MAGNITUDE idiom as the middle three so that `weak`,
// `moderate` and `strong` keep the spelling every shipped and user pack already stores — the words a
// reader actually sees are the labels (rarely · sometimes · often · usually · always), and they have
// never matched the stored names. See `strengthLabel()`.
enum class Strength { VeryWeak, Weak, Moderate, Strong, VeryStrong };

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

    // The same Provenance a Condition carries, deliberately — an edge is a CLAIM, and for a long
    // while it was the only claim in the pack that could not say how well founded it was. A
    // condition with no citation is forced to badge as Proposed; an edge held a bare citation
    // string with no tier, so an uncited edge and a cited one drew identically and `strength`
    // (which ranks which cause a coach is shown first) looked equally authoritative either way.
    Provenance provenance;
};

// ── Pack ────────────────────────────────────────────────────────────────────
struct CharacteristicPack {
    QString                 id;             // "core", or a community pack's namespace
    QString                 version;
    int                     schemaVersion = 1;
    QString                 sourceLabel;    // where it was loaded from, for the UI
    bool                    readOnly = false;   // the shipped core pack is not editable in place

    // Edges this pack RETIRES from the layers beneath it. Only `from`, `to` and `type` are read.
    //
    // A causal edge needs no such thing: a LocalUser pack replaces the whole incoming causal set of
    // any condition it names, so "absent from my list" already means "removed". A SYMMETRIC edge
    // belongs to neither end and is written individually, so there is no list for it to be absent
    // from — without a tombstone, a shipped corroboration could be re-typed but never dropped, and
    // the honest answer to "delete this" would have been "you cannot", in the user's own library.
    //
    // Additive and ignorable: a build that does not know the key simply sees the edge again, which
    // is the safe direction to fail in.
    std::vector<Edge>       retiredEdges;

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
QString measureStatusLabel(MeasureStatus s);   // "Live", "No producer", … — the UI's own words
bool    measureStatusFromName(const QString &s, MeasureStatus &out);
QString shapeName(Shape s);
QString shapeLabel(Shape s);                   // "Higher is better", … — never the bare enum word
bool    shapeFromName(const QString &s, Shape &out);

// True when only ONE tail of this shape grades, so a signal watching the other can never fire and a
// corridor drawn with two hard edges would state a bound the norm does not hold.
inline bool shapeIsOneSided(Shape s) { return s != Shape::Target; }
QString signalTestName(SignalTest t);
bool    signalTestFromName(const QString &s, SignalTest &out);
QString directionName(Direction d);
bool    directionFromName(const QString &s, Direction &out);
QString conditionGroupName(ConditionGroup g);
QString conditionGroupLabel(ConditionGroup g);
bool    conditionGroupFromName(const QString &s, ConditionGroup &out);

// The groups, in list order. One definition, because the order IS the swing and two hand-written
// copies of it (the library's filter row and the editor's picker) had already drifted apart by the
// time a third group was added.
const std::vector<ConditionGroup> &allConditionGroups();
QString observabilityName(Observability o);
bool    observabilityFromName(const QString &s, Observability &out);
QString confirmedByName(ConfirmedBy c);
bool    confirmedByFromName(const QString &s, ConfirmedBy &out);
QString provenanceTierName(ProvenanceTier t);
QString provenanceTierLabel(ProvenanceTier t);  // "Coaching practice", "No source found", … — the UI's own words
bool    provenanceTierFromName(const QString &s, ProvenanceTier &out);
QString conditionStateName(ConditionState s);
bool    conditionStateFromName(const QString &s, ConditionState &out);
QString edgeTypeName(EdgeType t);
bool    edgeTypeFromName(const QString &s, EdgeType &out);
QString strengthName(Strength s);
QString strengthLabel(Strength s);      // words, never a percentage
bool    strengthFromName(const QString &s, Strength &out);

// The rungs, weakest first. One definition, for the same reason allConditionGroups() exists: the
// order IS the ladder, and the editor's picker, the graph's collar and the analysis weight below all
// have to walk it the same way.
const std::vector<Strength> &allStrengths();

// The ONE weight a strength carries: P(effect | cause), in 0…1 exclusive of both bounds. It is the
// quantity the labels have always described — how OFTEN this cause produces this effect — and the
// five rungs are the only values of it anybody may author, because nobody can defend 0.73.
//
// TWO THINGS IT IS NOT, both of which it looks like.
//
// It is not P(cause | effect), which is what ranking causes from an observed effect actually wants.
// Bayes needs a base rate for how common each cause is, and no Condition carries one. Until the pack
// can supply that half, a caller that treats this as a posterior is ranking by out-degree — the
// cause with the most outgoing edges wins — while looking rigorous.
//
// And `RankedCause::score` is not a probability, whatever these are: it SUMS one of these per
// covered finding, so a cause explaining four of them scores past 1 and is meant to. The sum is
// ordinal and only ever reaches a comparison. Normalising it would break the greedy set cover it
// feeds, which depends on the additive behaviour.
//
// Values are not free to move. They were re-cut once, from an ordinal ladder that ran to 1.5, after
// measuring that the rescale preserved 98% of cause pairings on the shipped pack and that every
// pair it reordered was a near-tie. Anything that changes them owes the same measurement.
double strengthWeight(Strength s);

// The UI badge for how a condition can be reached. "Physical" and "Behavioural" are preferred over
// "biomechanical", which does not discriminate the body's CAPACITY from what it did with the club —
// over-the-top is biomechanical too.
QString reachLabel(ConfirmedBy c);
QString reachHint(ConfirmedBy c);

// ── Which tail fires, in the measure's own words ────────────────────────────
//
// ONE rule, here, because every surface that shows or sets a direction has to say the same thing:
// the picker where it is chosen, the signal row where it is changed, and the read-only detail page
// where a reader decides whether it is right. Three copies of this phrasing would eventually
// disagree, and the disagreement would be about which side of a corridor fires.
//
// With no `highMeans` authored it degrades to Too much / Too little — authorable, but saying
// nothing about the swing, which is the state that let three signals ship inverted.
//
// The LOW tail quotes the SAME authored sentence as "the other end of that range". No opposite is
// generated: a generated opposite reads like content and would be nobody's words.
struct DirectionPhrase {
    QString label;      // "Too much" / "Too little" — the chip
    QString means;      // this tail in the measure's own words; empty when none is authored
    QString sentence;   // the whole statement, for a row or a detail page
};

DirectionPhrase directionPhrase(Direction d, const QString &highMeans);

// True when the tier is a positive claim about the LITERATURE, so it MUST name the source it is
// claiming. NoSourceFound and Practice are deliberately excluded, for opposite reasons: the first
// is a claim about the ABSENCE of a source, and demanding a citation for it would make the null
// unrecordable; the second cites a body of practice this repo is not permitted to name.
inline bool citationRequired(ProvenanceTier t)
{
    return t == ProvenanceTier::Indirect || t == ProvenanceTier::Supported
           || t == ProvenanceTier::Established;
}

// True when somebody has actually looked, whatever the answer was. The complement is the work
// queue: `Proposed` is the only tier that means the question is still open.
inline bool provenanceSettled(ProvenanceTier t) { return t != ProvenanceTier::Proposed; }

// True when the tier is a claim about the OUTCOME OF A SEARCH and carries no citation to evidence
// it. Only NoSourceFound qualifies: "the literature is silent" is meaningless without a date,
// because it cannot be told from an unasked question and cannot be re-opened when the field
// publishes. A cited tier needs no date — the citation is its own evidence.
//
// `Practice` is deliberately NOT here, and the reason is the whole shape of this model: the tier
// says what the EVIDENCE is, `searchedOn` says whether anybody has LOOKED, and those are
// independent questions. An edge asserted from coaching knowledge is `practice` on the day it is
// written, with no date, because it is orthodoxy that nobody has yet checked against the
// literature. Searching it later either finds a paper (it becomes indirect/supported) or does not
// (it stays practice, now dated). Requiring the date up front collapsed the two axes and forced
// every honestly-authored edge to masquerade as `proposed` — as though it had come from nowhere.
inline bool searchDateRequired(ProvenanceTier t)
{
    return t == ProvenanceTier::NoSourceFound;
}

// The work queue, and the reason it is not `tier == Proposed`. What makes a claim outstanding is
// that nobody has been to the literature for it — not that nobody can say where it came from.
inline bool needsLiteratureSearch(const Provenance &p)
{
    return !p.searched() && p.citation.isEmpty();
}

// True when this condition can never be established by capture, so it must not appear in the
// measure roadmap however many characteristics it blocks.
inline bool isOutsideCaptureReach(ConfirmedBy c)
{
    return c == ConfirmedBy::Screened || c == ConfirmedBy::Asserted;
}

} // namespace pinpoint::analysis
