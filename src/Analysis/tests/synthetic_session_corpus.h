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

// SYNTHETIC SESSION CORPUS — a seeded, parameterised generator of ShotRecord sets with
// PLANTED GROUND TRUTH, for testing src/Analysis/diagnostic_ledger.h.
//
// TEST-SIDE ONLY. This lives in tests/ and ships in nothing. It exists because the design
// document (§C2.10) calls it the keystone test asset: the ledger's whole job is to decide
// what a session's evidence supports, and there is no way to check that judgement against
// real swings without first knowing the answer. So the answer is planted — a rate, a
// chain, a drift, a shank, an occlusion run — and the reduction is asked to find it.
//
// DETERMINISTIC BY CONSTRUCTION. A fixed 64-bit LCG, seeded per condition from the spec
// seed and the condition's index, and not one call to a clock, a device, or a global RNG.
// Two consequences worth stating: a failing assertion reproduces exactly, and ADDING a
// condition to a spec does not perturb the conditions already in it — which is what makes
// a corpus extensible rather than a set of numbers frozen by their own history.
//
// The generator plants, and the ground truth reports what it actually managed to plant —
// not what was asked for. A rate of 0.6 over 14 shots with two of them occluded cannot
// land on exactly 0.6, and a test that asserted the request rather than the outcome would
// be testing the generator's arithmetic instead of the ledger's.

#include "../diagnostic_ledger.h"

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace pinpoint::analysis::corpus {

// The states a plan holds, in the mock's own encoding so a fires array can be pasted in
// from `Session Dashboard.dc.html` without translation.
inline constexpr int kFired         =  1;
inline constexpr int kClean         =  0;
inline constexpr int kNotAssessable = -1;

// A 64-bit LCG (Knuth's MMIX constants), taking its output from the HIGH bits because the
// low bits of an LCG have famously short periods. Small, header-only, and identical on
// every platform — which std::mt19937 also is, but this needs no <random> and no
// distribution objects whose implementations are free to differ between standard
// libraries. Reproducibility across the CI matrix is the whole point.
struct Lcg {
    uint64_t s = 0;
    explicit Lcg(uint64_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t next()
    {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return uint32_t(s >> 33);                      // 31 bits
    }
    double unit() { return double(next()) / 2147483648.0; }          // [0, 1)
    double centred() { return unit() * 2.0 - 1.0; }                  // [−1, 1)
};

// One planted condition.
//
// Two ways to plant a firing pattern, and both are needed. `explicitStates` pins an exact
// sequence — that is how the design mock's nine fires arrays are replayed for the parity
// test, and how a boundary case is expressed without hunting for a seed that produces it.
// Everything else is parameterised, for the cases where the SHAPE matters and the exact
// swing does not.
struct PlantedCondition {
    QString id;
    QString measureId;

    // (a) exact. Non-empty wins over everything parameterised below except the naRuns and
    // outlier overrides, which are applied on top so a hand-written plan can still be
    // perturbed.
    std::vector<int> explicitStates;

    // (b) parameterised.
    double rate = 0.0;              // firing rate over the shots that are assessable
    bool   rangeRestricted = false; // fires on EVERY assessable shot — no variance to test

    // Plant a chain: this condition's firing is drawn conditional on its parent's state on
    // the same shot, which is what makes a 2×2 come out dependent for a reason rather than
    // by luck.
    QString coFireParent;
    double  pGivenParentFired = 0.85;
    double  pGivenParentClean = 0.10;

    // A single shank: one shot forced to fire, with a wild |z|. The Watching tier IS the
    // outlier discard, and Theil–Sen is meant to shrug this off — both are tested with it.
    int outlierShot = -1;
    double outlierAbsZ = 9.0;

    // Missing-measure stretches: {firstShot, length}. Occlusion does not arrive one shot
    // at a time — a camera that lost the shaft loses it for a while.
    std::vector<std::pair<int, int>> naRuns;

    // Direction. `modalFirings` pins the count exactly when a specific agreement is wanted
    // (the mock's face_roll is 5 open / 4 shut = 0.56); otherwise the count is derived from
    // `directionAgreement`, floored at a bare majority so the modal direction stays modal.
    int    direction = 1;
    double directionAgreement = 1.0;
    int    modalFirings = -1;

    // z, in sigma, as the driving measure's signed departure. The corridor is ±2 by
    // construction (see kCorridorHalf), so a fired shot must sit outside it and a clean one
    // inside — the generator is self-consistent with its own corridor or the rows are a lie.
    double baseAbsZ     = 2.6;
    double cleanAbsZ    = 0.5;
    double driftPerShot = 0.0;      // + worsening, − improving; applied to |z|
    double noise        = 0.0;      // ± this, uniform
};

// Corridor half-width in sigma. Every planted row is graded against ±2, so |z| > 2 is a
// firing and |z| < 2 is clean, and the two never contradict each other.
inline constexpr double kCorridorHalf = 2.0;

struct CorpusSpec {
    uint64_t seed  = 20260807;
    int      shots = 14;
    int      warmUpShots = 3;       // flagged on the record; the ledger's own §4 tunable
                                    // is separate and injected there
    FocusSplit focus;               // declared ⇒ baselineEnd splits the session
    QString  club = QStringLiteral("7i");
    QString  contextId = QStringLiteral("iron.full");

    std::vector<PlantedCondition> conditions;
    std::vector<NodeSpec>         nodes;
    std::vector<EdgeSpec>         edges;
};

// What the generator actually managed to plant, per condition.
struct GroundTruth {
    QString id;
    std::vector<int> states;              // the plan, in the mock's encoding
    int    firedShots = 0;
    int    assessableShots = 0;
    int    outlierShot = -1;
    double directionAgreement = 1.0;
    int    modalDirection = 0;
    bool   rangeRestricted = false;
    double driftPerShot = 0.0;
};

struct Corpus {
    std::vector<ShotRecord>  shots;
    std::vector<GroundTruth> truth;
    std::vector<NodeSpec>    nodes;
    std::vector<EdgeSpec>    edges;
    FocusSplit               focus;

    const GroundTruth *truthFor(const QString &id) const
    {
        for (const GroundTruth &g : truth)
            if (g.id == id) return &g;
        return nullptr;
    }
};

// Build the state plan for one condition. Split out because the co-firing case needs its
// parent's finished plan, so the plans are built in spec order and a parent must be listed
// before its child — which is also how a reader of the spec sees the chain.
inline std::vector<int> plantStates(const PlantedCondition &c, int shots, uint64_t seed,
                                    const std::vector<int> *parentPlan)
{
    Lcg rng(seed);
    std::vector<int> plan(size_t(std::max(shots, 0)), kClean);

    if (!c.explicitStates.empty()) {
        for (int i = 0; i < shots && i < int(c.explicitStates.size()); ++i)
            plan[size_t(i)] = c.explicitStates[size_t(i)];
    } else if (parentPlan) {
        for (int i = 0; i < shots; ++i) {
            const int p = i < int(parentPlan->size()) ? (*parentPlan)[size_t(i)] : kClean;
            // A parent the capture could not assess carries no signal either way, so the
            // child falls back to its own base rate rather than inheriting a fiction.
            const double pr = p == kFired ? c.pGivenParentFired
                            : p == kClean ? c.pGivenParentClean
                            : c.rate;
            plan[size_t(i)] = rng.unit() < pr ? kFired : kClean;
        }
    } else {
        for (int i = 0; i < shots; ++i)
            plan[size_t(i)] = rng.unit() < c.rate ? kFired : kClean;
    }

    // Occlusion runs go on top of whatever fired: the capture failing is independent of
    // the swing, which is exactly why NotAssessable may not be read as clean.
    for (const auto &run : c.naRuns)
        for (int i = run.first; i < run.first + run.second && i < shots; ++i)
            if (i >= 0) plan[size_t(i)] = kNotAssessable;

    // Range restriction after the occlusion: "fires on every MEASURABLE shot".
    if (c.rangeRestricted)
        for (int i = 0; i < shots; ++i)
            if (plan[size_t(i)] != kNotAssessable) plan[size_t(i)] = kFired;

    // The shank wins over everything — it is the shot the golfer remembers.
    if (c.outlierShot >= 0 && c.outlierShot < shots)
        plan[size_t(c.outlierShot)] = kFired;

    return plan;
}

// Materialise a spec into shots plus the ground truth that was planted into them.
inline Corpus generate(const CorpusSpec &spec)
{
    Corpus out;
    out.nodes = spec.nodes;
    out.edges = spec.edges;
    out.focus = spec.focus;

    const int n = std::max(spec.shots, 0);

    // Plans first, in spec order, so a co-firing child can see its parent.
    std::vector<std::vector<int>> plans;
    plans.reserve(spec.conditions.size());
    for (size_t ci = 0; ci < spec.conditions.size(); ++ci) {
        const PlantedCondition &c = spec.conditions[ci];
        const std::vector<int> *parent = nullptr;
        if (!c.coFireParent.isEmpty()) {
            for (size_t pj = 0; pj < ci; ++pj)
                if (spec.conditions[pj].id == c.coFireParent) { parent = &plans[pj]; break; }
        }
        // Per-condition seed: adding a condition cannot disturb the ones before it.
        plans.push_back(plantStates(c, n, spec.seed * 1000003ULL + uint64_t(ci) + 1ULL, parent));
    }

    // Directions, and the ground truth they achieve.
    std::vector<std::vector<int>> dirs(plans.size());
    for (size_t ci = 0; ci < spec.conditions.size(); ++ci) {
        const PlantedCondition &c = spec.conditions[ci];
        const std::vector<int> &plan = plans[ci];

        std::vector<int> firedIdx;
        for (int i = 0; i < n; ++i) if (plan[size_t(i)] == kFired) firedIdx.push_back(i);

        const int nf = int(firedIdx.size());
        int modal = c.modalFirings;
        if (modal < 0) modal = int(std::lround(c.directionAgreement * double(nf)));
        modal = std::clamp(modal, (nf + 1) / 2, std::max(nf, 0));

        dirs[ci].assign(size_t(n), 0);
        for (int k = 0; k < nf; ++k)
            dirs[ci][size_t(firedIdx[size_t(k)])] = k < modal ? c.direction : -c.direction;

        GroundTruth g;
        g.id     = c.id;
        g.states = plan;
        g.outlierShot = c.outlierShot;
        g.rangeRestricted = c.rangeRestricted;
        g.driftPerShot = c.driftPerShot;
        for (int i = 0; i < n; ++i) {
            if (plan[size_t(i)] == kNotAssessable) continue;
            ++g.assessableShots;
            if (plan[size_t(i)] == kFired) ++g.firedShots;
        }
        g.directionAgreement = nf > 0 ? double(modal) / double(nf) : 1.0;
        g.modalDirection = nf > 0 ? (modal * 2 > nf ? c.direction : 0) : 0;
        out.truth.push_back(std::move(g));
    }

    // Rows.
    out.shots.reserve(size_t(n));
    for (int i = 0; i < n; ++i) {
        ShotRecord s;
        s.shotId      = i + 1;
        s.club        = spec.club;
        s.contextId   = spec.contextId;
        s.timestampMs = qint64(1'700'000'000'000LL) + qint64(i) * 45'000LL;   // no clock
        s.warmUp      = i < spec.warmUpShots;

        for (size_t ci = 0; ci < spec.conditions.size(); ++ci) {
            const PlantedCondition &c = spec.conditions[ci];
            Lcg rng(spec.seed * 7919ULL + uint64_t(ci) * 131ULL + uint64_t(i) + 17ULL);

            ConditionRow r;
            r.conditionId      = c.id;
            r.drivingMeasureId = c.measureId.isEmpty() ? c.id + QStringLiteral(".measure")
                                                       : c.measureId;
            r.contextId        = spec.contextId;
            r.confidence       = 0.9f;
            r.corridorLo       = -kCorridorHalf;
            r.corridorHi       =  kCorridorHalf;

            const int st = plans[ci][size_t(i)];
            if (st == kNotAssessable) {
                r.state = ShotState::NotAssessable;
                // Never blank — the review strip prints this where a corridor would go.
                r.notAssessableReason = (i + int(ci)) % 2
                    ? QStringLiteral("shaft occluded at P6")
                    : QStringLiteral("ball not tracked");
                s.rows.push_back(std::move(r));
                continue;
            }

            const double drift = c.driftPerShot * double(i);
            const double jitter = c.noise * rng.centred();
            double absZ;
            if (st == kFired) {
                absZ = (i == c.outlierShot && c.outlierShot >= 0)
                     ? c.outlierAbsZ
                     : c.baseAbsZ + drift + jitter;
                // A fired row that landed inside its own corridor would contradict the
                // state beside it; the plan is authoritative, so the z is pushed out.
                absZ = std::max(absZ, kCorridorHalf + 0.05);
                r.state = ShotState::Fired;
                r.direction = dirs[ci][size_t(i)];
            } else {
                absZ = std::clamp(c.cleanAbsZ + jitter, 0.0, kCorridorHalf - 0.05);
                r.state = ShotState::Clean;
                r.direction = 0;
            }
            const double sign = r.direction < 0 ? -1.0 : 1.0;
            r.z     = sign * absZ;
            r.value = r.z;                    // sigma == 1 unit, so value and z agree
            s.rows.push_back(std::move(r));
        }
        out.shots.push_back(std::move(s));
    }
    return out;
}

// ── Convenience builders ────────────────────────────────────────────────────────

// A condition planted from an exact fires array — the mock-parity path.
inline PlantedCondition exact(const char *id, std::vector<int> states)
{
    PlantedCondition c;
    c.id = QString::fromLatin1(id);
    c.explicitStates = std::move(states);
    return c;
}

inline NodeSpec liveNode(const char *id, int phaseOrder)
{
    NodeSpec n;
    n.id = QString::fromLatin1(id);
    n.measurable = true;
    n.phaseOrder = phaseOrder;
    return n;
}

// A node the pack asserts but no capture can measure — a ghost with standing.
inline NodeSpec assertedGhost(const char *id, int phaseOrder)
{
    NodeSpec n;
    n.id = QString::fromLatin1(id);
    n.asserted = true;
    n.phaseOrder = phaseOrder;
    return n;
}

// A node whose measure is planned and not yet built — a ghost with none.
inline NodeSpec plannedGhost(const char *id, int phaseOrder)
{
    NodeSpec n;
    n.id = QString::fromLatin1(id);
    n.phaseOrder = phaseOrder;
    return n;
}

inline NodeSpec screenedRoot(const char *id, bool entered, int phaseOrder = 0)
{
    NodeSpec n;
    n.id = QString::fromLatin1(id);
    n.screened = true;
    n.screenEntered = entered;
    n.phaseOrder = phaseOrder;
    return n;
}

inline NodeSpec outcomeNode(const char *id, const char *outcome, int phaseOrder = 9)
{
    NodeSpec n;
    n.id = QString::fromLatin1(id);
    n.outcomeId = QString::fromLatin1(outcome);
    n.phaseOrder = phaseOrder;
    return n;
}

inline EdgeSpec edge(const char *from, const char *to, int type = 1, int strength = 1)
{
    EdgeSpec e;
    e.from = QString::fromLatin1(from);
    e.to   = QString::fromLatin1(to);
    e.type = type;
    e.strength = strength;
    return e;
}

} // namespace pinpoint::analysis::corpus
