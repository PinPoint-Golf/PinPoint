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

#include "wrist_assessment_contract.h"
#include "swing_analysis.h"   // MetricSeries, PhaseEvent (in-memory analyzer types)

#include <QVariantMap>
#include <vector>

// The live adapter: turns a focused swing's `shotReplay.analysisDetail` (the real MetricExtractor
// series + PhaseSegmenter phases) into the engine's input contract. It also owns the canonical
// **assessment checkpoint → segmentation Phase** mapping — the engine bands are keyed by
// PpSwingPosition, but the whole review surface speaks the named-phase vocabulary
// (TimelineLabels::phaseFullName/phaseShortTag), so each checkpoint carries its Phase int.

namespace pinpoint::analysis {

// One assessment checkpoint: the engine band key, the segmentation Phase it corresponds to
// (swing_analysis.h Phase enum int), and the coaching note shown in the position meta line.
struct WristCheckpoint {
    PpSwingPosition pos;
    int             phase;   // Phase enum int (0 Address … 14 Shaft-parallel-through)
    const char     *note;
};

// The kNumPos checkpoints in P1..P8 order, each on the phase the segmenter actually emits for that
// position; a checkpoint whose phase a swing didn't produce simply greys out.
inline const WristCheckpoint *wristCheckpoints()
{
    static const WristCheckpoint k[kNumPos] = {
        { PpSwingPosition::P1, 0,  "reference" },                 // Address
        { PpSwingPosition::P2, 1,  "early set" },                 // Takeaway
        { PpSwingPosition::P3, 8,  "set building" },              // Mid-backswing
        { PpSwingPosition::P4, 2,  "face checkpoint" },           // Top
        { PpSwingPosition::P5, 13, "lag retention begins" },      // Arm parallel down
        { PpSwingPosition::P6, 9,  "lag-retention checkpoint" },  // Delivery
        { PpSwingPosition::P7, 5,  "strike checkpoint" },         // Impact
        { PpSwingPosition::P8, 14, "release complete" },          // Shaft parallel through
    };
    return k;
}

// Parse a `shotReplay.analysisDetail` QVariantMap — `series` ([{key,t_us[],value[],…}], degrees,
// neutral-relative) + `phases` ([{phase,t_us,conf}]) — into an in-memory angle source. Series whose
// key is not a wrist DOF are ignored; a checkpoint whose phase is absent stays unavailable. The
// source is right-handed (producer convention; no mirror).
InMemoryWristAngleSource parseAnalysisDetail(const QVariantMap &analysisDetail);

// In-memory sibling of parseAnalysisDetail: build the angle source straight from the analyzer's
// MetricSeries + PhaseEvent vectors (no QVariantMap round-trip), for the offline WristAnalyzer's
// Tier-2 assessment pass. Same conventions: only wrist DOFs kept, right-handed (lead-arm) producer
// convention, a checkpoint whose phase the swing didn't produce stays unavailable.
InMemoryWristAngleSource buildWristAngleSource(const std::vector<MetricSeries> &series,
                                               const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
