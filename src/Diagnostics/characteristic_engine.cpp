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

QString findingStateName(FindingState s)
{
    switch (s) {
    case FindingState::Fired:       return QStringLiteral("fired");
    case FindingState::NotFired:    return QStringLiteral("notFired");
    case FindingState::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}

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
};

SignalVerdict evaluate(const Signal &sig, const CharacteristicPack &pack, const IMeasureSource &src)
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
        const Direction d = sig.direction.value_or(Direction::High);
        // Two conditions, both required. The GRADE decides whether this is a deviation at all
        // (Watch or Action — see MeasureReading::grade for why not merely "outside Ideal"), and the
        // SIDE decides whether it is this tail's deviation. An axis has two conditions on one norm,
        // so without the side check both tails would fire on any deviation in either direction.
        const bool deviated = isDeviation(r.grade);
        const bool onTail   = (d == Direction::High) ? (r.value > r.greenHi) : (r.value < r.greenLo);
        v.fired = deviated && onTail;

        // A context the shot never declared is a weaker basis for a finding than one it did. Reuse
        // the confidence channel rather than inventing a second signal for it — assessment_rules
        // already demotes on low confidence, and a parallel mechanism would need its own UI.
        if (r.contextInferred) v.confidence *= kInferredContextConfidence;
        break;
    }
    case SignalTest::Threshold: {
        const MeasureReading &r = readings.front();
        const double          t = sig.threshold.value_or(0.0);
        const Direction       d = sig.direction.value_or(Direction::High);
        v.fired = (d == Direction::High) ? (r.value > t) : (r.value < t);
        break;
    }
    case SignalTest::Order:
        // Measures are ordered: the first is expected to peak before the second. Values carry the
        // event time, so "out of order" is first >= second.
        v.fired = readings[0].value >= readings[1].value;
        break;
    case SignalTest::Ratio: {
        const double denom = readings[1].value;
        if (denom == 0.0) { v.available = false; v.missing = sig.measures; return v; }
        const double ratio = readings[0].value / denom;
        const MeasureReading &r = readings.front();
        if (!r.hasCorridor) { v.available = false; v.missing = sig.measures; return v; }
        const Direction d = sig.direction.value_or(Direction::High);
        // The ratio is computed here, so the reading's own grade (which grades readings[0]'s value,
        // not the ratio) does not apply. The corridor edges do.
        v.fired = (d == Direction::High) ? (ratio > r.greenHi) : (ratio < r.greenLo);
        if (r.contextInferred) v.confidence *= kInferredContextConfidence;
        break;
    }
    }

    (void)pack;
    return v;
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
        float conf          = 1.0f;

        for (const QString &sid : c.detectedBy) {
            const Signal *sig = pack.signal(sid);
            if (!sig) { anyUnavailable = true; f.missingMeasures << sid; continue; }

            const SignalVerdict v = evaluate(*sig, pack, source);
            if (!v.available) {
                anyUnavailable = true;
                f.missingMeasures << v.missing;
                continue;
            }
            if (v.fired) {
                anyFired = true;
                f.firedSignals << sid;
                f.direction = sig->direction.value_or(Direction::High);
            }
            conf = std::min(conf, v.confidence);
        }

        // Precedence: a signal that fired is a positive observation and stands even if a sibling
        // signal could not be evaluated. Only when NOTHING fired does an unavailable signal make
        // the whole finding unassessable — otherwise a partially-producible condition would be
        // silently downgraded to "not present".
        if (anyFired) {
            f.state      = FindingState::Fired;
            f.confidence = conf;
        } else if (anyUnavailable) {
            f.state      = FindingState::Unavailable;
            f.confidence = 0.0f;
        } else {
            f.state      = FindingState::NotFired;
            f.confidence = conf;
        }

        out.findings.push_back(std::move(f));
    }

    return out;
}

} // namespace pinpoint::analysis
