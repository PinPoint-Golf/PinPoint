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

#include <QString>
#include <QStringList>

#include <optional>

// The MEASUREMENT vocabulary: what a Measure is, which way its corridor is open, and how a Signal
// tests it. Split out of characteristic.h, which otherwise carried this alongside the condition
// graph (Condition, Edge, CharacteristicPack) — a reader wanting only "what is a Measure" had to
// pull in the causal model too. Header-only, Qt-only, no Qt-GUI, same as the file it came from.

namespace pinpoint::analysis {

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
//
// WHY THE VOCABULARY HOLDS SOME QUANTITIES TWICE — once as our own estimate (m_launchAngle,
// m_ballSpeed, m_attackAngle, m_clubPathAtImpact…) and once as a launch-monitor reading
// (m_lmLaunchAngle, m_lmBallSpeed, m_lmAttackAngle, m_lmClubPath…). The pairs look like drift and
// are deliberate, for two reasons that pull in the same direction. First, not every user owns a
// launch monitor, so every ball-flight quantity a diagnosis can rest on must be producible from
// our own pixels; the m_* rows are that commitment, and the `planned` ones among them are roadmap,
// not leftovers. Second, for a user who owns both, the two are INDEPENDENT measurements of one
// event — and the monitor's reading is the standard our estimate is validated against. One merged
// measure could do neither job: it would leave monitor-less users without the quantity, and a
// measure cannot validate itself. Resolution is an exact metricKey match with no fallback
// (measure_sample.cpp), so which of the pair a signal binds to is a per-quantity authoring
// decision made where the signal is written — not something a loader infers from what happens to
// be plugged in.

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
    Ratio,             // two measures IN THE SAME UNIT, their quotient against an authored number
                       // (tempo). A quotient is not a measure, so no norm can key on it — see the
                       // Ratio branch of characteristic_engine.cpp for why a stand-in measure with
                       // a dimensionless corridor cannot be made to work, and why the number is
                       // therefore authored on the signal exactly as Threshold's is.
};

struct Signal {
    QString                  id;
    SignalTest               test = SignalTest::OutsideCorridor;
    QStringList              measures;     // 1 for corridor/threshold, 2 for order/ratio
    std::optional<Direction> direction;
    // The authored number, for the two tests that grade against one no norm supplies: Threshold,
    // and Ratio — whose quotient cannot key a norm at all. Required on both and refused on the
    // other two, which inherit their numbers from the catalogue (signalThreshold).
    std::optional<double>    threshold;
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

} // namespace pinpoint::analysis
