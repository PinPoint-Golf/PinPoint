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
#include <cmath>

namespace pinpoint::analysis {

namespace {

// The ladder itself now lives beside the enum, in characteristic.h — this file was where it was
// first needed, not where it belongs, and the graph view had already grown a second copy of it.
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

QStringList relatedBy(const CharacteristicPack &pack, const QString &conditionId, EdgeType type)
{
    QStringList out;
    for (const Edge &e : pack.edges) {
        if (e.type != type) continue;
        if (e.from == conditionId) out << e.to;
        else if (e.to == conditionId) out << e.from;
    }
    out.removeDuplicates();
    return out;
}

Explanation explain(const CharacteristicPack &pack, const DetectionResult &detection,
                    const QHash<QString, bool> &knownScreenResults)
{
    Explanation ex;

    const QStringList firedList = detection.fired();
    QSet<QString>     fired(firedList.begin(), firedList.end());
    if (fired.isEmpty()) return ex;

    // ── Exclusions, resolved BEFORE anything is ranked ──────────────────────
    //
    // Two findings that exclude each other cannot both describe one swing — a shot was chunked or
    // it was topped, not both. Leaving both in would put a contradiction in front of a coach and,
    // worse, let the ranking count a cause twice for explaining two versions of one event.
    //
    // The one kept is the more confident; ties break on the id so the answer is the same every run.
    // Confidence, not edge strength: an Excludes edge says the pair is incompatible, and says
    // nothing about which of them happened.
    {
        QHash<QString, float> confidence;
        QHash<QString, int>   specificity;
        for (const Finding &f : detection.findings)
            if (f.state == FindingState::Fired) {
                confidence.insert(f.conditionId, f.confidence);
                specificity.insert(f.conditionId, int(f.firedSignals.size()));
            }

        for (const QString &a : firedList) {
            if (!fired.contains(a)) continue;                 // already suppressed
            for (const QString &b : relatedBy(pack, a, EdgeType::Excludes)) {
                if (!fired.contains(b) || a == b) continue;

                // Confidence, then SPECIFICITY, then the id. The middle rung was added with the
                // first conjunctions and it is not a tweak: `top` is `thin` AND two further facts,
                // so when a swing is genuinely topped BOTH fire, they exclude each other, and they
                // tie on confidence because they are read off the same capture. Falling straight to
                // the id kept "thin" — alphabetically before "top" — and the more specific finding,
                // the one that had strictly more evidence behind it, was the one thrown away.
                //
                // MORE FIRED SIGNALS IS MORE EVIDENCE, which is why the count is the right measure
                // of it rather than anything about the content. A condition that cleared three
                // independent tests is a stronger reading of the same swing than one that cleared
                // one of them, and it stays true for any future pair without either of them having
                // to know the other exists.
                const float ca = confidence.value(a, 0.0f);
                const float cb = confidence.value(b, 0.0f);
                const int   sa = specificity.value(a, 0);
                const int   sb = specificity.value(b, 0);
                const bool  keepA = (ca != cb) ? (ca > cb)
                                  : (sa != sb) ? (sa > sb)
                                               : (a < b);

                const QString kept    = keepA ? a : b;
                const QString dropped = keepA ? b : a;

                const Condition *kc = pack.condition(kept);
                const Condition *dc = pack.condition(dropped);
                Suppression s;
                s.conditionId = dropped;
                s.excludedBy  = kept;
                s.reason      = QStringLiteral("'%1' and '%2' cannot both describe one swing; "
                                               "'%1' was the more confident reading.")
                                    .arg(kc ? kc->label : kept, dc ? dc->label : dropped);
                ex.suppressed.push_back(std::move(s));
                fired.remove(dropped);
                if (dropped == a) break;                      // stop examining a; it is gone
            }
        }
    }

    // ── Corroboration ───────────────────────────────────────────────────────
    // Reported for every fired finding another fired finding independently confirms, and used below
    // only to break ties. See the type's comment for why it is not a weight.
    QHash<QString, int> corroborationCount;
    for (const QString &f : fired) {
        QStringList by;
        for (const QString &other : relatedBy(pack, f, EdgeType::Corroborates))
            if (fired.contains(other)) by << other;
        if (by.isEmpty()) continue;
        by.sort();
        corroborationCount.insert(f, int(by.size()));
        ex.corroborations.push_back(Corroboration{ f, by });
    }
    std::sort(ex.corroborations.begin(), ex.corroborations.end(),
              [](const Corroboration &a, const Corroboration &b) {
                  if (a.corroboratedBy.size() != b.corroboratedBy.size())
                      return a.corroboratedBy.size() > b.corroboratedBy.size();
                  return a.conditionId < b.conditionId;
              });

    // Findings the context marked immaterial. They are still findings — listed, counted in
    // coverage, reported to the coach exactly as any other — but they carry NO weight when ranking
    // which root cause to put first. That is the whole of what ContextBinding::material means, and
    // the weight is zero rather than some fraction because a made-up fraction would be a number
    // nobody could defend when asked why one cause outranked another.
    QSet<QString> immaterial;
    for (const Finding &f : detection.findings)
        if (f.state == FindingState::Fired && !f.material) immaterial.insert(f.conditionId);

    // THE ONE CHOKEPOINT, and the reason the prominence factor goes here rather than at either of
    // the two places `score` is accumulated. Both of them (the candidate pass below, and the greedy
    // re-score that recomputes over the still-uncovered subset) call through this lambda, so one
    // edit covers both. Applying it at the call sites would be two edits that must agree forever,
    // and they would not.
    //
    // score(cause) = P(cause) x SUM over covered findings of P(effect | cause)
    //
    // The second term was always here. The first is the base rate that `strengthWeight()`'s own
    // header spent a year saying the pack could not supply — "Bayes needs a base rate for how common
    // each cause is, and no Condition carries one". One does now. Without it the ranking is out-
    // degree wearing a posterior's clothes: a cause is put first for having many arrows, not for
    // being a likely explanation of THIS swing, so a settled tempo habit outranks early extension.
    //
    // Still not a probability, and `RankedCause::score` never was — it SUMS over covered findings,
    // so a cause explaining four of them scores past any single term and is meant to. The quantity
    // is ordinal and only ever reaches a comparison.
    const auto rankWeight = [&pack, &immaterial](const QString &from, const QString &to) {
        if (immaterial.contains(to)) return 0.0;
        const Condition *cause = pack.condition(from);
        const double     base  = cause ? prominenceWeight(cause->prominence)
                                       : prominenceWeight(Prominence::Occasional);
        return base * edgeWeight(pack, from, to);
    };

    // Every condition that explains at least one fired finding is a candidate — including other
    // fired characteristics, since a characteristic can be both symptom and cause.
    // What a cause explains, AFTER exclusions. `findingsCoveredBy()` reads the raw detection because
    // it is the public "why is this being suggested?" query and must answer about the whole result;
    // the ranking below must not count a finding an exclusion has already ruled out.
    const auto coversNow = [&pack, &detection, &fired](const QString &causeId) {
        QStringList out;
        for (const QString &e : findingsCoveredBy(pack, detection, causeId))
            if (fired.contains(e)) out << e;
        return out;
    };

    std::vector<RankedCause> candidates;
    for (const Condition &c : pack.conditions) {
        const QStringList covers = coversNow(c.id);
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
        rc.prominence  = c.prominence;
        rc.confirmedBy = c.confirmedBy;
        rc.offeredOnly = (c.confirmedBy == ConfirmedBy::Asserted);
        rc.unknown     = (c.confirmedBy == ConfirmedBy::Screened
                          && !knownScreenResults.contains(c.id));

        for (const QString &e : covers) rc.score += rankWeight(c.id, e);
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

    // Corroboration enters HERE and nowhere else: as a tie-break, once score and coverage have both
    // failed to separate two causes. A tie-break needs no magnitude, so it commits none — but a
    // cause whose findings were each seen two independent ways is the better answer to put first,
    // and leaving the choice to alphabetical order would throw that away.
    const auto corroborationOf = [&corroborationCount](const RankedCause &rc) {
        int total = 0;
        for (const QString &e : rc.explains) total += corroborationCount.value(e, 0);
        return total;
    };
    // The epsilon guards FLOAT NOISE AND NOTHING ELSE, and its size is the whole of its meaning.
    //
    // Before prominence, `score` was a sum of five discrete edge weights, so two causes with the
    // same structure produced bit-identical doubles and `!=` was exact and correct. It is now a sum
    // of PRODUCTS, and two structurally different causes can land on the same value by arithmetic
    // that differs in the last bit — 0.35 x 0.60 against 0.20 x 1.05 — which would hand the ordering
    // to the id and look like a bug nobody could reproduce.
    //
    // What it must NOT do is restore the tie-breaks below to how often they used to fire. Two causes
    // that used to tie exactly and now differ because one is Common and the other Occasional are
    // being separated BY PROMINENCE, which is the entire point of the change; widening this until
    // they tie again would silently undo it. Sized for representation error, never for policy.
    constexpr double kScoreEpsilon = 1e-9;

    const auto better = [&corroborationOf](const RankedCause &a, const RankedCause &b) {
        if (std::abs(a.score - b.score) > kScoreEpsilon) return a.score > b.score;
        if (a.coverage != b.coverage) return a.coverage > b.coverage;
        const int ca = corroborationOf(a), cb = corroborationOf(b);
        if (ca != cb)                 return ca > cb;
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
                    trimmed.score += rankWeight(rc.conditionId, e);
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
        // whether to run a screen wants to know everything it would settle. Still only what stands
        // after exclusions: a screen does not earn credit for a finding already ruled out.
        tr.wouldExplain = coversNow(rc.conditionId);
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
    // A suppressed finding is not unexplained — it was ruled out, which `ex.suppressed` already
    // says. Listing it here as well would report one event twice under two contradictory headings.
    for (const QString &f : firedList)
        if (fired.contains(f) && !covered.contains(f)) {
            const bool offeredElsewhere = std::any_of(
                ex.offered.begin(), ex.offered.end(),
                [&](const RankedCause &rc) { return rc.explains.contains(f); });
            if (!offeredElsewhere) ex.unexplained << f;
        }

    return ex;
}

} // namespace pinpoint::analysis
