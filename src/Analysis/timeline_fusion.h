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

// TimelineFusion — the positions ladder, grown up: ARBITRATE each coaching
// P-slot between the two witnesses instead of handing it to whoever got there
// first. Design: docs/design/timeline-fusion.md (V1 = §4, Part I is the
// diagnosis this exists to fix).
//
// THE DEFECT. positions_ladder.h fills only EMPTY slots ("an IMU-path proxy wins
// — no arbitration"), and SegResolve picks one segmentation whole. So on an
// IMU-bound swing the segmenter's conf-0.35 hand-orientation PROXY holds P6, its
// conf-0.35 forearm proxy holds P8, and its conf-0.20 WINDOW-EDGE CLAMP wears the
// P10 label — while the camera's measured values for all three are computed,
// persisted, and discarded. Measured against a full P1–P10 truth markup of the
// eleven 2026-08-18 wG3 swings: published P6 −39 ms vs club +6 ms, published P8
// +96 ms vs club +7 ms, published P10 +1686 ms vs club −0 ms.
//
// WHY NOT "HIGHER CONF WINS" (§3). Four incommensurable scales share PhaseEvent
// ::conf — the segmenter's duration-prior gate shaping, the club's PER-FRAME
// DETECTION TIER (which says how well the club was SEEN, and nothing about how
// well the crossing was located in TIME), the vision ladder's flat 0.5, and
// event_refine's at-ball tier score. Ordering those against each other is a
// static priority table in disguise, and score_uncertainty.cpp already reads conf
// as calibrated at Top/Impact. So conf stays a display hint and arbitration
// decides on two things that ARE comparable because we define them to be:
//
//   1. MEASUREMENT CLASS (TimingClass, swing_analysis.h) — each producer declares
//      whether its emission is a measurement, a proxy, or a fallback. They
//      already know; today they whisper it through magic capped conf constants.
//      Absolute: Anchor is never displaced, Measured beats Proxy beats Fallback.
//   2. ESTIMAND OWNERSHIP — each P is DEFINED by an observable, and the
//      instrument that observes it directly is the preferred witness. It breaks
//      Measured-vs-Measured. Not a heuristic: a forearm IMU measures arm
//      elevation directly (P3/P5/P9), only the camera sees the SHAFT (P2/P6/P8),
//      and the corpus voted for exactly that split before the table was written.
//
// Ties — same class, same owner, or identical times — RETAIN THE INCUMBENT:
// stability is worth more than an unmeasurable improvement. Nothing is ever
// averaged, nudged, or compromised; every published time stays some instrument's
// actual estimate with that instrument's provenance and class attached.
//
// GUARDED REPLACEMENT. A winner takes the slot only under the ladder's existing
// insertion guards, now applied to replacement too: strictly inside the slot's
// anchor window, strictly between its time-neighbours, and — ONLY when the
// incumbent is itself Measured — within the dispute cap. A PROXY incumbent gets
// no cap on purpose: its class already says the time is a stand-in, so no
// magnitude of disagreement rehabilitates it. That rule was written by data —
// on Wrist_01/0003 the club P8 sits 669 ms from the IMU proxy and is +1 ms
// against truth, so a cap-everything draft would have preserved a 667 ms error.
// On any doubt the incumbent stays and the doubt is COUNTED, never dropped.
//
// Pure over plain value types (Segmentation / ShaftPosition) — NO SwingWindow, NO
// AnalysisContext, same cv-free contract as event_refine.h / positions_ladder.h —
// so it is unit-testable standalone (timeline_fusion_test). The thin
// AnalysisStage glue (TimelineFusionStage, wrist_analyzer.cpp) sits at pipeline
// slot 10c, after EventRefine and before BindDetail, mutating the resolved-AND-
// refined ctx.seg so BindDetail persists the arbitration with zero extra
// plumbing. Insertion into an empty slot is just arbitration against an ABSENT
// incumbent, so emitPositionsLadder's behaviour is a strict subset of this one's
// and that function retires when the flag freezes ON.
//
// WHAT V1 ACTUALLY CHANGES on an IMU-bound swing (§4.4): P6 and P8 flip from the
// segmenter's proxies to the club's crossings, P10 flips from the window-edge
// clamp to the club's finish; P3/P4/P5 are RETAINED by ownership with their
// deltas logged, P7 is the acoustic anchor and is retained always, P2 is the
// unchanged insertion. On a camera-only swing the interior slots are pure
// insertions exactly as today and every anchor slot ties, so the outcome is the
// present ladder plus the additive audit trail.
//
// P1 IS IMPLEMENTED BUT SHIPS DARK (refine.fusionP1). The class rule already
// gives the right answer — the IMU Address was the conf-0.30 FALLBACK on 11/11
// wG3 swings while the club P1 is a stack-fit measurement — but Address is the
// reference instant for tempo, every Address-referenced pose metric, the replay
// trim and the segmenter's own Address/Takeaway co-timing, and it drags the
// event_refine.h "two copies of the SAME instant" twin contract with it (false on
// the IMU path: they sit ~97 ms apart, bimodally). That gets its own gate (§9.1).
//
// INVARIANTS. Impact is NEVER retimed or displaced under any configuration (the
// marker contract) — slot 7 is owned by the anchor, not by an instrument.
// swingStartUs/swingEndUs are untouched: fusion moves LABELS, never bounds, so
// export encode spans, replay spans and heavy-stage scan windows cannot change.
// (The disk-replay TRIM reads the Finish event itself, so a P10 replacement
// legitimately ends the replay at the real finish ~1.7 s earlier — intended, and
// on the gate's eyeball checklist.) Existing events are never reordered or
// dropped. Decisions gate on candidate availability and class — NEVER on session
// type. Timestamps: positions and events share the window time domain in memory
// (serializeAnalysis subtracts clock.t0_us exactly once at write) — NOTHING is
// converted here.
//
// Segmentation.version: any insertion or replacement bumps it to 5 ("fusion
// arbitrated") and publishes seg.fusion. An all-abstain pass leaves seg
// byte-identical — version, events and audit trail alike. refine.fusion=false ⇒
// the stage never runs AND fuseTimeline returns an empty result ⇒ ctx.seg is
// byte-identical AND code-path-identical to the pre-fusion pipeline (the parity
// baseline, §8 gate 2).

#include <QVariantMap>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <vector>

#include "swing_analysis.h"                // Segmentation, PhaseEvent, ShaftPosition,
                                           // TimingClass, FusionDecision, FusionReason
#include "analysis_tuning.h"               // tuning::apply
#include "../Core/pp_tuned_constants.h"    // tuned::refine::

namespace pinpoint::analysis {

// Fusion knobs. Defaults track the frozen constants (pp_tuned_constants.h
// refine::); SwingLab sweeps them via "refine.*" dotted keys without a rebuild.
// `enabled` has been ON since the 2026-08-19 corpus gate
// (docs/implementation/timeline_fusion_impl.md); false still darks the stage
// entirely, the byte- and code-path-identical soak baseline.
struct TimelineFusionConfig {
    bool enabled   = tuned::refine::kFusion;          // refine.fusion (master gate)
    bool p1        = tuned::refine::kFusionP1;        // refine.fusionP1 (Phase 2, dark)
    // Measured-vs-Measured only, so it is inert until P1 arbitration turns on —
    // every V1 replacement displaces a Proxy or a Fallback, which get no cap.
    int  disputeMs = tuned::refine::kFusionDisputeMs; // refine.fusionDisputeMs

    static TimelineFusionConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        TimelineFusionConfig c;
        apply(ov, "refine.fusion",          c.enabled);
        apply(ov, "refine.fusionP1",        c.p1);
        apply(ov, "refine.fusionDisputeMs", c.disputeMs);
        return c;
    }
};

// What a pass did, for the log line and the persisted audit trail. emitted ==
// false ⇒ seg untouched (version, events and seg.fusion included) — the
// byte-identical outcome for a run that contributes nothing.
//
// The four "nothing moved" outcomes are DISJOINT and separately counted, because
// they mean different things: `retained` = the arbitration ran and the incumbent
// won it; `disputed` = the candidate won on merit but the two witnesses
// disagreed beyond any plausible physiology; `abstained` = a guard refused a
// winner that would have broken the ladder's ordering.
struct TimelineFusionResult {
    bool emitted   = false;   // anything inserted or replaced (⇒ seg.version = 5)
    int  inserted  = 0;       // empty slot filled by the sole candidate
    int  replaced  = 0;       // incumbent retimed to the winning candidate
    int  retained  = 0;       // contested, incumbent won (anchor / class / owner / tie)
    int  disputed  = 0;       // contested, candidate won but exceeded the dispute cap
    int  abstained = 0;       // window or neighbour guard refused the winner
    std::vector<FusionDecision> decisions;   // one per slot that had a candidate
};

// Arbitrate every mapped P-slot between the resolved segmentation and the club
// track's located positions, per the header contract. Mutates seg IN PLACE
// (inserts in time order, retimes winners where they stand, bumps version to 5
// and publishes seg.fusion when anything landed); never touches the swing bounds,
// never reorders or drops an event, never touches Impact.
inline TimelineFusionResult fuseTimeline(Segmentation &seg,
                                         const std::vector<ShaftPosition> &positions,
                                         const TimelineFusionConfig &cfg)
{
    TimelineFusionResult r;
    if (!cfg.enabled)
        return r;

    // Who OWNS the estimand — which instrument observes the defining quantity
    // DIRECTLY (§4.1). Camera: only the camera sees the shaft, and the grip
    // stillness walk-back + stack fit measures the club at rest. Imu: a forearm
    // IMU measures arm elevation directly, where the camera infers φ from pose
    // keypoints. Either: both are legitimate when both are measurements (P10 —
    // the IMU's quiet-run detector really does find a finish; its window-edge
    // clamp is not a measurement at all, and loses on class, not ownership).
    // Anchored: the acoustic impact, inviolable — slot 7 is owned by neither
    // instrument and is never inserted, retimed or displaced.
    enum class Owner : uint8_t { Camera, Imu, Either, Anchored };

    // p           — ShaftPosition::p, the coaching index (NOT a Phase value)
    // lo/hi       — the slot's bounding anchor Phases; kNone = unbounded that side
    // insertable  — may fill an EMPTY slot. The four anchor rungs (P1/P4/P7/P10)
    //               are arbitration-only: a ladder that lost its Address, Top,
    //               Impact or Finish is degenerate, and those events are the
    //               window bounds every other slot measures against, so fusion
    //               arbitrates them but never INVENTS them.
    struct Slot { int p; Phase phase; int lo, hi; Owner owner; bool insertable; };
    constexpr int kNone = -1;
    static constexpr Slot kSlots[] = {
        {1,  Phase::Address,              kNone,               int(Phase::Takeaway), Owner::Camera,   false},
        {2,  Phase::ShaftParallelBack,    int(Phase::Address), int(Phase::Top),      Owner::Camera,   true },
        {3,  Phase::MidBackswing,         int(Phase::Address), int(Phase::Top),      Owner::Imu,      true },
        {4,  Phase::Top,                  kNone,               kNone,                Owner::Imu,      false},
        {5,  Phase::ArmParallelDown,      int(Phase::Top),     int(Phase::Impact),   Owner::Imu,      true },
        {6,  Phase::Delivery,             int(Phase::Top),     int(Phase::Impact),   Owner::Camera,   true },
        {7,  Phase::Impact,               kNone,               kNone,                Owner::Anchored, false},
        {8,  Phase::ShaftParallelThrough, int(Phase::Impact),  int(Phase::Finish),   Owner::Camera,   true },
        {10, Phase::Finish,               int(Phase::Impact),  kNone,                Owner::Either,   false},
        // P9 (lead arm parallel, follow-through) is IMU-owned and uncontested:
        // the camera defers it (shaft_positions.h), so no candidate can exist and
        // there is nothing to arbitrate. That both producers are silent there on
        // every truth-graded swing is an EMISSION gap, tracked outside fusion.
    };

    // Strictly inside the slot's anchor window (guard 6a). An unbounded side
    // passes; a NAMED but absent bound fails — we never arbitrate into a window
    // we cannot see.
    const auto insideWindow = [&](const Slot &s, int64_t t) {
        if (s.lo != kNone) {
            const PhaseEvent *lo = seg.eventFor(Phase(s.lo));
            if (!lo || t <= lo->t_us) return false;
        }
        if (s.hi != kNone) {
            const PhaseEvent *hi = seg.eventFor(Phase(s.hi));
            if (!hi || t >= hi->t_us) return false;
        }
        return true;
    };

    for (const Slot &s : kSlots) {
        if (s.p == 1 && !cfg.p1)
            continue;                       // Address arbitration is Phase 2 — dark, and
                                            // dark means ABSENT, not "decided and skipped"
        const auto pit = std::find_if(positions.begin(), positions.end(),
                                      [&](const ShaftPosition &q) { return q.p == s.p; });
        if (pit == positions.end())
            continue;                       // no candidate ⇒ nothing to say about this slot
        const ShaftPosition &cand = *pit;

        // The incumbent by phase — eventFor's first-match rule, but we need its
        // INDEX in the time-ordered list for the neighbour guard.
        size_t i = 0;
        while (i < seg.events.size() && seg.events[i].phase != s.phase) ++i;

        // ── Uncontested: the candidate is the only witness ──────────────────
        if (i == seg.events.size()) {
            if (!s.insertable)
                continue;
            if (!insideWindow(s, cand.t_us)) {
                ++r.abstained;
                r.decisions.push_back({s.phase, SegmentRole::Club, SegmentRole::Unknown, 0,
                                       FusionReason::GuardWindow});
                continue;
            }
            const auto at = std::lower_bound(seg.events.begin(), seg.events.end(), cand.t_us,
                                             [](const PhaseEvent &e, int64_t t) { return e.t_us < t; });
            if ((at != seg.events.begin() && std::prev(at)->t_us >= cand.t_us)
                || (at != seg.events.end() && at->t_us <= cand.t_us)) {
                ++r.abstained;              // tie with a neighbour ⇒ not strict
                r.decisions.push_back({s.phase, SegmentRole::Club, SegmentRole::Unknown, 0,
                                       FusionReason::GuardNeighbour});
                continue;
            }
            PhaseEvent e;
            e.phase      = s.phase;
            e.t_us       = cand.t_us;
            e.conf       = cand.conf;
            e.provenance = SegmentRole::Club;
            e.timing     = cand.timing;
            seg.events.insert(at, e);
            ++r.inserted;
            r.decisions.push_back({s.phase, SegmentRole::Club, SegmentRole::Unknown, 0,
                                   FusionReason::Inserted});
            continue;
        }

        // ── Contested: decide on class, then on ownership ───────────────────
        PhaseEvent  &inc   = seg.events[i];
        const int64_t delta = cand.t_us - inc.t_us;

        bool         candWins = false;
        FusionReason reason   = FusionReason::TieHeld;
        if (s.owner == Owner::Anchored || inc.timing == TimingClass::Anchor) {
            reason = FusionReason::AnchorHeld;                    // rule 3
        } else if (timingRank(cand.timing) > timingRank(inc.timing)) {
            candWins = true;
            reason   = FusionReason::ClassBeat;                   // rule 4
        } else if (timingRank(cand.timing) < timingRank(inc.timing)) {
            reason = FusionReason::ClassHeld;                     // rule 4
        } else if (delta == 0) {
            reason = FusionReason::TieHeld;                       // same instant, nothing to gain
        } else if (s.owner == Owner::Camera && inc.provenance != SegmentRole::Club) {
            candWins = true;
            reason   = FusionReason::OwnerBeat;                   // rule 5
        } else if (s.owner == Owner::Imu && inc.provenance != SegmentRole::Club) {
            reason = FusionReason::OwnerHeld;                     // rule 5
        } else {
            reason = FusionReason::TieHeld;   // Either, or the camera already holds a camera slot
        }

        // The dispute cap (guard 6c), keyed on the INCUMBENT being Measured. A
        // Proxy or Fallback incumbent is uncapped by design — see the header.
        if (candWins && inc.timing == TimingClass::Measured && cfg.disputeMs > 0
            && std::llabs(delta) > int64_t(cfg.disputeMs) * 1000) {
            candWins = false;
            reason   = FusionReason::Disputed;
        }

        if (!candWins) {
            if (reason == FusionReason::Disputed) ++r.disputed;
            else                                  ++r.retained;
            // winner − loser: the incumbent won, so the delta is signed its way.
            r.decisions.push_back({s.phase, inc.provenance, SegmentRole::Club, -delta, reason});
            continue;
        }

        if (!insideWindow(s, cand.t_us)) {
            ++r.abstained;
            r.decisions.push_back({s.phase, inc.provenance, SegmentRole::Club, delta,
                                   FusionReason::GuardWindow});
            continue;
        }
        // Strictly between the retained time-neighbours (guard 6b). The winner
        // takes the slot WHERE IT STANDS, so the list order is preserved exactly
        // when the new time still fits between index i−1 and i+1.
        if ((i > 0 && seg.events[i - 1].t_us >= cand.t_us)
            || (i + 1 < seg.events.size() && seg.events[i + 1].t_us <= cand.t_us)) {
            ++r.abstained;
            r.decisions.push_back({s.phase, inc.provenance, SegmentRole::Club, delta,
                                   FusionReason::GuardNeighbour});
            continue;
        }

        const SegmentRole loser = inc.provenance;
        inc.t_us       = cand.t_us;      // retime in place — phase identity, event
        inc.conf       = cand.conf;      // order and the swing bounds are untouched
        inc.provenance = SegmentRole::Club;
        inc.timing     = cand.timing;
        ++r.replaced;
        r.decisions.push_back({s.phase, SegmentRole::Club, loser, delta, reason});
    }

    if (r.inserted + r.replaced > 0) {
        seg.version = 5;
        seg.fusion  = r.decisions;   // the arbitration that DIDN'T happen is evidence too
        r.emitted   = true;
    }
    return r;
}

} // namespace pinpoint::analysis
