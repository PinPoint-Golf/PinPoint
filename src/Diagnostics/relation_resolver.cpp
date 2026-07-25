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

#include "relation_resolver.h"

#include <QSet>

#include <algorithm>

namespace pinpoint::analysis {

namespace {

double strengthWeight(Strength s)
{
    // Three-valued by design; these are ranking weights, not probabilities, and must never be
    // rendered as percentages.
    switch (s) {
    case Strength::Strong:   return 1.0;
    case Strength::Moderate: return 0.6;
    case Strength::Weak:     return 0.3;
    }
    return 0.6;
}

double edgeWeight(const CharacteristicPack &pack, const QString &from, const QString &to)
{
    for (const Edge &e : pack.edges)
        if (e.type == EdgeType::Causes && e.from == from && e.to == to)
            return strengthWeight(e.strength);
    return 0.0;
}

} // namespace

QStringList findingsCoveredBy(const CharacteristicPack &pack, const DetectionResult &detection,
                              const QString &causeId)
{
    // Bind the temporary before taking iterators: fired() returns by value, so calling begin() and
    // end() on it directly would iterate two DIFFERENT temporaries.
    const QStringList   firedList = detection.fired();
    const QSet<QString> firedSet(firedList.begin(), firedList.end());

    QStringList out;
    for (const QString &effect : effectsOf(pack, causeId))
        if (firedSet.contains(effect)) out << effect;
    return out;
}

Explanation explain(const CharacteristicPack &pack, const DetectionResult &detection,
                    const QHash<QString, bool> &knownScreenResults)
{
    Explanation ex;

    const QStringList   firedList = detection.fired();
    const QSet<QString> fired(firedList.begin(), firedList.end());
    if (fired.isEmpty()) return ex;

    // Every condition that explains at least one fired finding is a candidate — including other
    // fired characteristics, since a characteristic can be both symptom and cause.
    std::vector<RankedCause> candidates;
    for (const Condition &c : pack.conditions) {
        const QStringList covers = findingsCoveredBy(pack, detection, c.id);
        if (covers.isEmpty()) continue;

        // A screen that has been entered and came back NEGATIVE is not an explanation. An
        // unanswered screen stays a candidate — that is what produces a recommendation.
        if (c.confirmedBy == ConfirmedBy::Screened) {
            const auto it = knownScreenResults.constFind(c.id);
            if (it != knownScreenResults.constEnd() && !it.value()) continue;
        }

        RankedCause rc;
        rc.conditionId = c.id;
        rc.explains    = covers;
        rc.coverage    = int(covers.size());
        rc.confirmedBy = c.confirmedBy;
        rc.offeredOnly = (c.confirmedBy == ConfirmedBy::Asserted);
        rc.unknown     = (c.confirmedBy == ConfirmedBy::Screened
                          && !knownScreenResults.contains(c.id));

        for (const QString &e : covers) rc.score += edgeWeight(pack, c.id, e);
        candidates.push_back(std::move(rc));
    }

    // RULE 1: a fired characteristic that itself has an in-pack cause is a link in the chain, not a
    // root. Offering it as the answer would hand the coach a symptom and call it a diagnosis.
    const auto isRootEligible = [&pack, &fired](const QString &id) {
        if (!fired.contains(id)) return true;               // latent causes are always eligible
        return causesOf(pack, id).isEmpty();                 // a fired characteristic needs no cause
    };

    std::vector<RankedCause> concludable, offered;
    for (RankedCause &rc : candidates) {
        if (rc.offeredOnly) { offered.push_back(std::move(rc)); continue; }   // RULE 2
        if (!isRootEligible(rc.conditionId)) continue;
        concludable.push_back(std::move(rc));
    }

    const auto better = [](const RankedCause &a, const RankedCause &b) {
        if (a.score != b.score)       return a.score > b.score;
        if (a.coverage != b.coverage) return a.coverage > b.coverage;
        return a.conditionId < b.conditionId;   // deterministic tie-break
    };

    // Greedy set cover: repeatedly take the cause accounting for the most still-unexplained
    // findings. Greedy rather than exhaustive because the output is a ranked list a coach reads top
    // down, not a minimal cover — and a cause that only duplicates what is already explained adds
    // nothing to that list.
    QSet<QString> covered;
    while (true) {
        std::vector<RankedCause> remaining;
        for (const RankedCause &rc : concludable) {
            RankedCause trimmed = rc;
            trimmed.explains.clear();
            trimmed.score = 0.0;
            for (const QString &e : rc.explains)
                if (!covered.contains(e)) {
                    trimmed.explains << e;
                    trimmed.score += edgeWeight(pack, rc.conditionId, e);
                }
            trimmed.coverage = int(trimmed.explains.size());
            if (trimmed.coverage > 0) remaining.push_back(std::move(trimmed));
        }
        if (remaining.empty()) break;

        const auto best = std::min_element(remaining.begin(), remaining.end(), better);
        ex.roots.push_back(*best);
        for (const QString &e : best->explains) covered.insert(e);

        // Drop the chosen cause so it cannot be selected twice.
        concludable.erase(std::remove_if(concludable.begin(), concludable.end(),
                                         [&](const RankedCause &rc) {
                                             return rc.conditionId == best->conditionId;
                                         }),
                          concludable.end());
    }

    // Asserted causes are ranked and shown, never folded into the cover. They are for the coach to
    // confirm or dismiss.
    std::sort(offered.begin(), offered.end(), better);
    ex.offered = std::move(offered);

    // Test recommendations: an unanswered screen that would explain more than one finding. One
    // finding is not worth sending someone for a screen; two or more is the point of the model.
    for (const RankedCause &rc : ex.roots) {
        if (!rc.unknown || rc.coverage < 2) continue;
        const Condition *c = pack.condition(rc.conditionId);
        if (!c) continue;

        TestRecommendation tr;
        tr.conditionId  = rc.conditionId;
        tr.screenRef    = c->screenRef;
        // Report the cause's FULL reach, not just what the greedy pass left it — a coach deciding
        // whether to run a screen wants to know everything it would settle.
        tr.wouldExplain = findingsCoveredBy(pack, detection, rc.conditionId);
        tr.coverage     = int(tr.wouldExplain.size());
        ex.recommendations.push_back(std::move(tr));
    }
    std::sort(ex.recommendations.begin(), ex.recommendations.end(),
              [](const TestRecommendation &a, const TestRecommendation &b) {
                  if (a.coverage != b.coverage) return a.coverage > b.coverage;
                  return a.conditionId < b.conditionId;
              });

    // Anything fired that nothing in the pack accounts for. Surfaced rather than swallowed: it is
    // the authoring queue's most useful signal about where the content is thin.
    for (const QString &f : firedList)
        if (!covered.contains(f)) {
            const bool offeredElsewhere = std::any_of(
                ex.offered.begin(), ex.offered.end(),
                [&](const RankedCause &rc) { return rc.explains.contains(f); });
            if (!offeredElsewhere) ex.unexplained << f;
        }

    return ex;
}

} // namespace pinpoint::analysis
