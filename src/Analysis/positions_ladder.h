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

// PositionsLadder — promote the club track's already-located coaching
// P-positions (analysis.club.positions[], shaft_position_first §2 Layer B) into
// Segmentation.events PhaseEvents, so phase-anchored measures resolve on
// camera-only swings without any new capture. This is the analyzer-layer
// emission that Layer B deferred on purpose (shaft_track_assembly.cpp kept its
// blast radius to the club block only) and that EventRefine's V1 scope note
// named as "a future refine.positionsLadder key".
//
// Pure over plain value types (Segmentation / ShaftPosition) — NO SwingWindow,
// NO AnalysisContext, same cv-free contract as event_refine.h — so it is
// unit-testable standalone (positions_ladder_test). The thin AnalysisStage glue
// (PositionsLadderStage, wrist_analyzer.cpp) slots between EventRefine and
// BindDetail, mutating the resolved-AND-refined ctx.seg so BindDetail persists
// the appended events with zero extra plumbing.
//
// MAPPING (ShaftPosition.p is the coaching index 1..8/10, NOT a Phase value):
// 2→ShaftParallelBack(12), 3→MidBackswing(8), 5→ArmParallelDown(13),
// 6→Delivery(9), 8→ShaftParallelThrough(14). p ∈ {1,4,7,10} are NEVER
// re-emitted — Address/Top/Impact/Finish anchors already exist and
// Segmentation::eventFor takes the FIRST match, so duplicates would shadow.
// conf is the position's own (real) confidence; provenance = SegmentRole::Club.
//
// INSERT-IF-MONOTONE, ABSTAIN OTHERWISE (event_refine's contract applied to
// insertion): a candidate is inserted only when (a) its Phase is not already in
// the ladder (an IMU-path proxy wins — no arbitration, no confidence-priority
// merge), (b) both of its bounding anchors exist (P2/P3 within Address..Top,
// P5/P6 within Top..Impact, P8 within Impact..Finish), (c) it lands STRICTLY
// inside that anchor window, and (d) it lands STRICTLY between its would-be
// time neighbours in the event list (guards Takeaway and earlier inserts).
// Existing events are never reordered, retimed, or dropped, and insertion is
// in time order (QML consumers assume time-ordered phases — timeline_labels,
// chart_metrics segments). On the known non-monotone vision ladders
// (Top==Finish degeneracy) the collapsed windows fail (c) and the pass
// naturally abstains. Timestamps: positions and events share the window time
// domain in memory (serializeAnalysis subtracts clock.t0_us exactly once at
// write) — NOTHING is converted here.
//
// Segmentation.version: any insertion bumps it to 4 ("positions ladder
// emitted", swing_analysis.h); an all-abstain pass leaves seg byte-identical.
// refine.positionsLadder=false ⇒ the stage never runs ⇒ ctx.seg is
// byte-identical AND code-path-identical (the A/B soak baseline).

#include <QVariantMap>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"                // Segmentation, PhaseEvent, ShaftPosition
#include "analysis_tuning.h"               // tuning::apply
#include "../Core/pp_tuned_constants.h"    // tuned::refine::

namespace pinpoint::analysis {

// Gate only — ON by default since the 2026-08-09 corpus gate went green
// (tuned::refine::kPositionsLadder); false darks the stage entirely, the
// byte-identical soak baseline. SwingLab sweeps it via the
// "refine.positionsLadder" dotted key.
struct PositionsLadderConfig {
    bool enabled = tuned::refine::kPositionsLadder;   // refine.positionsLadder

    static PositionsLadderConfig fromOverrides(const QVariantMap &ov)
    {
        PositionsLadderConfig c;
        tuning::apply(ov, "refine.positionsLadder", c.enabled);
        return c;
    }
};

// What a pass did, for the log line. emitted == false ⇒ seg untouched
// (version included), the byte-identical outcome for a run that contributes
// nothing.
struct PositionsLadderResult {
    bool emitted   = false;   // any event inserted (⇒ seg.version = 4)
    int  inserted  = 0;
    int  abstained = 0;       // missing anchor / outside window / time tie
    int  duplicate = 0;       // Phase already in the ladder (e.g. IMU proxy)
};

// Insert the mapped club P-positions into seg.events per the header contract.
// Mutates seg IN PLACE (inserts events in time order, bumps version to 4 when
// anything landed); never touches existing events or the swing bounds.
inline PositionsLadderResult emitPositionsLadder(Segmentation &seg,
                                                 const std::vector<ShaftPosition> &positions)
{
    struct Mapping { int p; Phase phase, lo, hi; };
    static constexpr Mapping kMap[] = {
        {2, Phase::ShaftParallelBack,    Phase::Address, Phase::Top},
        {3, Phase::MidBackswing,         Phase::Address, Phase::Top},
        {5, Phase::ArmParallelDown,      Phase::Top,     Phase::Impact},
        {6, Phase::Delivery,             Phase::Top,     Phase::Impact},
        {8, Phase::ShaftParallelThrough, Phase::Impact,  Phase::Finish},
    };

    PositionsLadderResult r;
    for (const ShaftPosition &pos : positions) {
        const auto *m = std::find_if(std::begin(kMap), std::end(kMap),
                                     [&](const Mapping &mm) { return mm.p == pos.p; });
        if (m == std::end(kMap))
            continue;                                   // p ∈ {1,4,7,10}: anchors already exist
        if (seg.eventFor(m->phase)) { ++r.duplicate; continue; }

        const PhaseEvent *lo = seg.eventFor(m->lo);
        const PhaseEvent *hi = seg.eventFor(m->hi);
        if (!lo || !hi || pos.t_us <= lo->t_us || pos.t_us >= hi->t_us) {
            ++r.abstained;                              // window absent or collapsed
            continue;
        }
        const auto at = std::lower_bound(seg.events.begin(), seg.events.end(), pos.t_us,
                                         [](const PhaseEvent &e, int64_t t) { return e.t_us < t; });
        if ((at != seg.events.begin() && std::prev(at)->t_us >= pos.t_us)
            || (at != seg.events.end() && at->t_us <= pos.t_us)) {
            ++r.abstained;                              // tie with a neighbour ⇒ not strict
            continue;
        }

        PhaseEvent e;
        e.phase      = m->phase;
        e.t_us       = pos.t_us;
        e.conf       = pos.conf;
        e.provenance = SegmentRole::Club;
        seg.events.insert(at, e);
        ++r.inserted;
    }
    if (r.inserted > 0) {
        seg.version = 4;
        r.emitted   = true;
    }
    return r;
}

} // namespace pinpoint::analysis
