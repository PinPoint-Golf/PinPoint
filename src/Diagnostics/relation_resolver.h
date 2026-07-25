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

#include "characteristic_engine.h"

namespace pinpoint::analysis {

// The explanation pass: fired conditions -> ranked root causes + test recommendations.
//
// Two rules do most of the work here, and both exist to stop the output being confidently wrong.
//
// 1. A CHARACTERISTIC WITH AN IN-PACK CAUSE IS NEVER A ROOT. The graph is not two clean layers —
//    observable characteristics legitimately cause other observable characteristics (early
//    extension causes loss of posture; casting causes scooping). Presenting a leaf as a root would
//    hand the coach a symptom and call it the diagnosis.
//
// 2. AN `Asserted` CAUSE IS OFFERED, NEVER CONCLUDED. Intent, habit and perception are knowable
//    only by asking. They are surfaced, visually distinct, phrased as a question — but they never
//    count as resolving a finding. Dropping them instead would leave characteristics whose only
//    cause is habit with an empty explanation panel, when the truthful answer is "this may simply
//    be how they set up".

// A cause the resolver is putting forward, with what it accounts for.
struct RankedCause {
    QString     conditionId;
    QStringList explains;              // fired conditions this cause accounts for
    int         coverage   = 0;        // explains.size(), hoisted for sorting/display
    double      score      = 0.0;      // coverage weighted by edge strength
    ConfirmedBy confirmedBy = ConfirmedBy::Measured;
    bool        offeredOnly = false;   // Asserted: shown, never counted as resolving
    bool        unknown     = false;   // Screened and not yet entered — driving a recommendation
};

// "Screen this — it would explain four of your findings." The highest-value output of the whole
// model, and it costs no capture hardware: the dominant causes in the pack are all screen-backed,
// so a handful of physical tests explain the majority of what was detected.
struct TestRecommendation {
    QString     conditionId;
    QString     screenRef;
    QStringList wouldExplain;
    int         coverage = 0;
};

struct Explanation {
    std::vector<RankedCause>        roots;            // concluded, best first
    std::vector<RankedCause>        offered;          // Asserted — for the coach to confirm
    std::vector<TestRecommendation> recommendations;  // unknown screens, by value
    QStringList                     unexplained;      // fired, with nothing in the pack to explain it
};

// `knownScreenResults` carries any physical-screen answers already entered — condition id -> present.
// Absent from the map means "not screened yet", which is what generates a recommendation rather
// than an assumption either way.
Explanation explain(const CharacteristicPack &pack, const DetectionResult &detection,
                    const QHash<QString, bool> &knownScreenResults = {});

// Which fired findings a given candidate cause would account for. Exposed because the UI must
// always be able to answer "why is this being suggested?" for any proposed root — a ranking the
// user cannot interrogate is a ranking they cannot trust.
QStringList findingsCoveredBy(const CharacteristicPack &pack, const DetectionResult &detection,
                              const QString &causeId);

} // namespace pinpoint::analysis
