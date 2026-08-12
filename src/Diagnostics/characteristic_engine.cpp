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

#include "characteristic_engine.h"

#include <algorithm>

namespace pinpoint::analysis {

// Confidence multiplier applied when the shot declared no context and the norm was resolved against
// the default. Chosen to land a full-confidence reading below assessment_rules' confidence floor,
// so an inferred-context finding is kept and marked rather than dropped — the deviation is real,
// what is uncertain is whether the right norm was used to judge it.
constexpr float kInferredContextConfidence = 0.7f;

const Finding *DetectionResult::find(const QString &conditionId) const
{
    const auto it = std::find_if(findings.begin(), findings.end(),
                                 [&](const Finding &f) { return f.conditionId == conditionId; });
    return it == findings.end() ? nullptr : &*it;
}

QStringList DetectionResult::fired() const
{
    QStringList out;
    for (const Finding &f : findings)
        if (f.state == FindingState::Fired) out << f.conditionId;
    return out;
}

QStringList DetectionResult::unavailable() const
{
    QStringList out;
    for (const Finding &f : findings)
        if (f.state == FindingState::Unavailable) out << f.conditionId;
    return out;
}

namespace {

// One signal's verdict. `available == false` propagates all the way to the finding — it is never
// collapsed into "did not fire".
struct SignalVerdict {
    bool        available = false;
    bool        fired     = false;
    float       confidence = 0.0f;
    QStringList missing;

    // The reading this verdict was reached on, for the finding's evidence. Set only when the signal
    // was actually evaluated — an unavailable verdict has no number to report and must not pretend
    // otherwise.
    std::optional<MeasureReading> driving;
    QString                       drivingMeasureId;
};

SignalVerdict evaluate(const Signal &sig, const IMeasureSource &src)
{
    SignalVerdict v;

    std::vector<MeasureReading> readings;
    readings.reserve(size_t(sig.measures.size()));
    for (const QString &mid : sig.measures) {
        const auto r = src.read(mid);
        if (!r) {
            v.missing << mid;
            continue;
        }
        readings.push_back(*r);
    }
    if (!v.missing.isEmpty()) return v;   // stays unavailable

    const int want = (sig.test == SignalTest::Order || sig.test == SignalTest::Ratio) ? 2 : 1;
    if (int(readings.size()) != want) {
        // A malformed signal reaching the engine is a pack bug the validator should have caught.
        // Report unavailable rather than guessing: a wrong answer here is indistinguishable from a
        // real finding downstream.
        v.missing = sig.measures;
        return v;
    }

    v.available  = true;
    v.confidence = readings.front().confidence;
    for (const MeasureReading &r : readings) v.confidence = std::min(v.confidence, r.confidence);

    switch (sig.test) {
    case SignalTest::OutsideCorridor: {
        const MeasureReading &r = readings.front();
        if (!r.hasCorridor) {
            // The corridor is the whole test. Without one there is nothing to compare against, so
            // this is unavailable — NOT a pass. This is the single most important branch in the
            // engine: most of the seed pack has no corridor yet.
            v.available = false;
            v.missing << sig.measures.value(0);
            return v;
        }
        // A reading the norm does not believe was NOT ASSESSED, and the finding must say so. It is
        // not "we looked and this is fine" — the value is a capture fault, and reporting it as
        // NotFired would be a false negative wearing a clean bill of health. Unavailable is the
        // state that exists for exactly this, and `implausible` is what lets the UI say which of
        // the two reasons applies.
        if (r.implausible) {
            v.available = false;
            v.missing << sig.measures.value(0);
            return v;
        }

        const Direction d = sig.direction.value_or(Direction::High);

        // A signal pointing at a tail the norm never grades CANNOT fire, and says so outright
        // rather than reaching the same answer by arithmetic. On a floor the high edge is mu and
        // everything above it grades Ideal, so `deviated` would already be false — but that is a
        // coincidence of two other rules, and an author who wrote this signal has misunderstood
        // the measure. `diagnostics_health` reports it as `signalOnOpenTail` at validation time.
        const bool tailOpen = (d == Direction::High) ? r.highOpen : r.lowOpen;

        // Two conditions, both required. The GRADE decides whether this is a deviation at all
        // (Watch or Action — see MeasureReading::grade for why not merely "outside Ideal"), and the
        // SIDE decides whether it is this tail's deviation. An axis has two conditions on one norm,
        // so without the side check both tails would fire on any deviation in either direction.
        const bool deviated = isDeviation(r.grade);
        const bool onTail   = (d == Direction::High) ? (r.value > r.greenHi) : (r.value < r.greenLo);
        v.fired = !tailOpen && deviated && onTail;

        // A context the shot never declared is a weaker basis for a finding than one it did. Reuse
        // the confidence channel rather than inventing a second signal for it — assessment_rules
        // already demotes on low confidence, and a parallel mechanism would need its own UI.
        if (r.contextInferred) v.confidence *= kInferredContextConfidence;
        break;
    }
    case SignalTest::Threshold: {
        const MeasureReading &r = readings.front();

        // The two refusals its siblings make, made here too — and their absence was not a
        // simplification. A threshold test reads the SAME reading off the same norm-joined source
        // as a corridor test; only the number it compares against differs. So every argument for
        // refusing over there holds here word for word, and a branch that skipped both was the one
        // place in the engine where an authored number was trusted more than the norm that said the
        // reading was not real.
        //
        // A reading the norm does not believe was NOT ASSESSED. That an author typed the number
        // rather than inheriting it changes nothing: a smash of 1.62 is a mis-tracked ball, and
        // answering "yes, above 1.55" about it launders a capture fault into a confident diagnosis
        // — exactly what Norm::plausibleLo exists to prevent. Unavailable is the state for it.
        if (r.implausible) {
            v.available = false;
            v.missing << sig.measures.value(0);
            return v;
        }

        const double    t = sig.threshold.value_or(0.0);
        const Direction d = sig.direction.value_or(Direction::High);

        // A signal watching a tail the measure's own shape says never grades cannot fire, and says
        // so outright rather than reaching the answer by arithmetic — the corridor branch's rule,
        // and the same authoring mistake behind it. A floor has no upper fault whoever writes the
        // number.
        //
        // NOTE WHAT IS NOT REQUIRED: a corridor. Threshold is the test kind that exists to work
        // where no norm has been authored, so demanding hasCorridor here would dark precisely the
        // signals this branch is for. With no norm both flags are false and this costs nothing;
        // with a norm behind the reading, its shape is believed.
        const bool tailOpen = (d == Direction::High) ? r.highOpen : r.lowOpen;

        v.fired = !tailOpen && ((d == Direction::High) ? (r.value > t) : (r.value < t));
        break;
    }
    case SignalTest::Order:
        // Measures are ordered: the first is expected to peak before the second. Values carry the
        // event time, so "out of order" is first >= second.
        v.fired = readings[0].value >= readings[1].value;
        break;
    case SignalTest::Ratio: {
        // THE RATIO CONTRACT, and why it is not the obvious one.
        //
        // This branch divides one measure by another, and the quotient is DIMENSIONLESS. It used to
        // grade that quotient against readings.front()'s corridor — a norm keyed on
        // sig.measures[0], whose unit norm.h requires to match THAT measure's unit ("MUST match the
        // measure's unit; load fails if not"). So the corridor was in degrees, or seconds, while
        // the value was a pure number. That is not a strict test or a lenient one, it is a category
        // error: no author could write a ratio signal that meant what they thought it meant, and
        // the reason the shipped pack contains none of them may simply be that nobody could.
        //
        // THE TEMPTING REPAIR DOES NOT WORK, and the reason is structural rather than a matter of
        // taste. Let the ratio carry its own norm — name a measure whose unit is dimensionless and
        // author a corridor on it. But a norm reaches this engine only through IMeasureSource::read
        // and A READING REQUIRES A VALUE: NormMeasureSource returns nullopt the instant the value
        // source cannot produce one, corridor or no corridor. Nothing produces the quotient. That
        // is the entire reason this test kind exists rather than one corridor signal on a produced
        // ratio measure — which is what m_tempoRatio already is, unit ":1", norm and all. So the
        // stand-in measure would resolve to nullopt, the signal would report Unavailable forever,
        // and the corridor authored for it would never once be read.
        //
        // So: THE NUMBER THAT GRADES A DERIVED QUANTITY IS AUTHORED ON THE SIGNAL THAT DERIVES IT,
        // because the pack has nowhere else to put a number for something that is not a measure. A
        // norm keys on a measure; a quotient is not one and cannot be made into one without a
        // producer. The mechanism already exists and already carries the right doctrine —
        // Signal::threshold, "an authored number; needs a citation to be more than an opinion",
        // which is exactly the standing of a published 3:1 tempo figure. Ratio is Threshold applied
        // to a quotient, and the validator demands the number and the direction from it the same
        // way (signalThreshold, signalDirection).
        //
        // What the two inputs must share is a UNIT, and `signalRatioUnit` enforces that at load:
        // seconds over seconds cancels and the authored figure means something, while degrees over
        // seconds is a rate wearing a ratio's name and the figure would be in a unit nothing in the
        // model records.
        const double denom = readings[1].value;
        if (denom == 0.0) { v.available = false; v.missing = sig.measures; return v; }

        if (readings[0].implausible || readings[1].implausible) {
            // Same rule as the corridor branch: a ratio built from a reading nobody believes was
            // not assessed, whichever half of it was wrong.
            v.available = false;
            v.missing   = sig.measures;
            return v;
        }

        const double    ratio = readings[0].value / denom;
        const double    t     = sig.threshold.value_or(0.0);
        const Direction d     = sig.direction.value_or(Direction::High);
        v.fired = (d == Direction::High) ? (ratio > t) : (ratio < t);

        // NO corridor requirement, and NO open-tail check. Both belong to a measure's norm and the
        // quotient has neither. Requiring a corridor would dark every ratio signal for want of a
        // band it does not consult; and the numerator's shape describes which tail of the NUMERATOR
        // faults, which says nothing whatever about the quotient — a small enough denominator sends
        // it high however one-sided the measure above the line is. `diagnostics_health` follows the
        // same line: signalOnOpenTail now reads corridor and threshold signals, not ratios.
        //
        // Nor is confidence demoted for an inferred context. That demotion asks "was the right norm
        // used to judge this?", and here no norm judges it — the number came from the author.
        break;
    }
    }

    // THE DRIVING READING IS measures[0], for every test kind — reached only on the paths that
    // actually evaluated the signal, since every unavailable branch above returns early. The
    // corridor and threshold tests read exactly one measure, so there is no choice to make; Order
    // and Ratio name their subject first (the event expected to LEAD, the numerator), and the pack
    // validator enforces that order. Its corridor may be absent — a threshold or ratio signal grades
    // against an authored number — and MeasureEvidence handles that without inventing a band.
    v.driving          = readings.front();
    v.drivingMeasureId = sig.measures.value(0);

    return v;
}

// One signal's contribution to a finding's evidence, in c.detectedBy order.
struct EvidenceCandidate {
    QString        signalId;
    QString        measureId;
    MeasureReading reading;
    float          confidence = 0.0f;
    bool           fired      = false;
};

// WHICH READING SPEAKS FOR THE FINDING. The most confident signal, ties broken by pack order (the
// order the author wrote c.detectedBy in), restricted to the signals that FIRED when the finding
// fired and drawn from every assessed signal when it did not.
//
// The SAME rule in both states, deliberately. The alternative for NotFired — pick whichever measure
// sat closest to its edge, the near-miss — reads better on one swing and ruins the ledger, because
// the measure it names changes from swing to swing and a trend line would silently splice two
// different quantities together. Confidence is a property of the CAPTURE, not of the swing, so it
// picks the same measure every time a condition is assessed the same way, which is what makes
// "this corridor is being cleared by less and less" a sentence about the golfer.
//
// Confidence here is the SIGNAL's — after the inferred-context demotion — not the raw reading's, so
// a signal graded against a norm the shot never declared yields to a sibling judged on solid ground.
MeasureEvidence pickEvidence(const std::vector<EvidenceCandidate> &candidates, bool firedOnly)
{
    const EvidenceCandidate *best = nullptr;
    for (const EvidenceCandidate &c : candidates) {
        if (firedOnly && !c.fired) continue;
        if (best == nullptr || c.confidence > best->confidence) best = &c;
    }
    if (best == nullptr) return {};
    return MeasureEvidence::fromReading(best->signalId, best->measureId, best->reading);
}

} // namespace

DetectionResult detect(const CharacteristicPack &pack, const IMeasureSource &source,
                       const ContextTree *contexts, const QString &contextId)
{
    DetectionResult out;

    for (const Condition &c : pack.conditions) {
        // Latent conditions have no signals by definition — they are resolved by the explanation
        // pass from what they explain, not detected here.
        if (c.observability == Observability::Latent) continue;

        // Withdrawn content does not diagnose. Retired and Superseded are the two states that mean
        // "do not use this any more", and until now the engine read all six identically — a
        // retired characteristic would have gone on firing exactly as it did the day it was sound.
        //
        // ONLY those two. Draft and Candidate are editorial confidence, not withdrawal, and 62 of
        // the 112 shipped conditions are Draft — gating on them would dark more than half the
        // library on a reading of the field nobody has agreed to. A Superseded condition is
        // additionally required by the validator to name its successor, so the replacement is
        // already in the pack and evaluates in its place; dropping it here loses nothing.
        if (c.state == ConditionState::Retired || c.state == ConditionState::Superseded) continue;

        // Does this condition apply to this kind of shot? Resolved through the context tree, so an
        // author writes one row at `partial` rather than four beneath it.
        bool material = true;
        if (contexts != nullptr && !contextId.isEmpty()) {
            const BindingResolution br = resolveContextBinding(c, *contexts, contextId);
            if (!br.applicable) continue;
            material = br.material;
        }

        Finding f;
        f.conditionId = c.id;
        f.material    = material;

        if (c.detectedBy.isEmpty()) {
            f.state = FindingState::Unavailable;
            out.findings.push_back(std::move(f));
            continue;
        }

        bool  anyFired      = false;
        bool  anyUnavailable = false;
        bool  anyAssessedNotFired = false;   // ALL mode only — see the verdict block below
        float conf          = 1.0f;

        std::vector<EvidenceCandidate> candidates;   // in c.detectedBy order — see pickEvidence()

        for (const QString &sid : c.detectedBy) {
            const Signal *sig = pack.signal(sid);
            if (!sig) { anyUnavailable = true; f.missingMeasures << sid; continue; }

            const SignalVerdict v = evaluate(*sig, source);
            if (!v.available) {
                anyUnavailable = true;
                f.missingMeasures << v.missing;
                continue;
            }
            if (v.fired) {
                anyFired = true;
                f.firedSignals << sid;
                f.direction = sig->direction.value_or(Direction::High);
            } else {
                anyAssessedNotFired = true;
            }
            conf = std::min(conf, v.confidence);

            if (v.driving) candidates.push_back({ sid, v.drivingMeasureId, *v.driving,
                                                  v.confidence, v.fired });
        }

        // ── ALL: a conjunction, and the precedence INVERTS ───────────────────
        //
        // Every signal must have fired. What is worth spelling out is the order of the two failure
        // cases, because it is the mirror image of the ANY rule below and for the same reason.
        //
        // A CONJUNCT THAT WAS ASSESSED AND DID NOT FIRE SETTLES IT. The conjunction is false, and
        // it is false whatever the signals we could not read would have said — an AND with one
        // known-false term needs no other term. So that is NotFired, a real negative answer, and
        // reporting Unavailable instead would throw away a verdict we actually hold. This is the
        // common case by a distance: `top` asks for a thin strike AND a low point behind the ball
        // AND an upward attack, and the overwhelming majority of swings settle it on the first.
        //
        // ONLY WITH NOTHING FALSE does an unreadable conjunct make it unassessable, because then
        // the answer really does turn on what we could not see.
        if (c.detection == DetectionMode::All) {
            if (anyAssessedNotFired) {
                f.state      = FindingState::NotFired;
                f.confidence = conf;
                f.evidence   = pickEvidence(candidates, /*firedOnly*/ false);
            } else if (anyUnavailable) {
                f.state      = FindingState::Unavailable;
                f.confidence = 0.0f;
            } else {
                f.state      = FindingState::Fired;
                f.confidence = conf;
                f.evidence   = pickEvidence(candidates, /*firedOnly*/ true);
            }
            out.findings.push_back(std::move(f));
            continue;
        }

        // Precedence: a signal that fired is a positive observation and stands even if a sibling
        // signal could not be evaluated. Only when NOTHING fired does an unavailable signal make
        // the whole finding unassessable — otherwise a partially-producible condition would be
        // silently downgraded to "not present".
        if (anyFired) {
            f.state      = FindingState::Fired;
            f.confidence = conf;
            f.evidence   = pickEvidence(candidates, /*firedOnly*/ true);
        } else if (anyUnavailable) {
            f.state      = FindingState::Unavailable;
            f.confidence = 0.0f;
            // NO evidence, even when a sibling signal did read a number. The finding's verdict is
            // "could not be assessed", and attaching a reading to it would invite a ledger row that
            // reads as an assessment. See MeasureEvidence.
        } else {
            f.state      = FindingState::NotFired;
            f.confidence = conf;
            f.evidence   = pickEvidence(candidates, /*firedOnly*/ false);
        }

        out.findings.push_back(std::move(f));
    }

    return out;
}

} // namespace pinpoint::analysis
