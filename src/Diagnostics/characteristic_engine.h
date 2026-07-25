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

#include "characteristic_pack.h"

#include <QString>

#include <optional>
#include <vector>

// The detection pass: signals -> conditions.
//
// NOT WIRED INTO THE LIVE ANALYSIS PATH. This runs against an abstract measure source so the pack's
// semantics can be proven by test; surfacing findings on a real swing is a separate change. A pack
// no consumer has ever executed is a pack whose semantics are unverified, which is why this exists
// now rather than later.
//
// The rule that shapes the whole design: AN ABSENT MEASURE IS UNAVAILABLE, NEVER A PASS. A
// characteristic whose measure has no producer must report that it could not be assessed. Reporting
// it as "not detected" would be a false negative dressed as a clean bill of health, and with most
// of the seed pack currently unproducible that single mistake would make the entire library look
// like it works.

namespace pinpoint::analysis {

// One measure's value for one swing, plus the corridor to grade it against.
struct MeasureReading {
    double value       = 0.0;
    bool   hasCorridor = false;
    double greenLo     = 0.0;
    double greenHi     = 0.0;
    float  confidence  = 1.0f;   // 0..1; propagates into the finding
};

// Where readings come from. Deliberately abstract: the tests feed synthetic values, and the future
// live wiring implements the same seam without this module gaining an analysis dependency.
class IMeasureSource {
public:
    virtual ~IMeasureSource() = default;
    // nullopt => this measure could not be produced for this swing.
    virtual std::optional<MeasureReading> read(const QString &measureId) const = 0;
};

enum class FindingState {
    Fired,        // the signal tripped — the condition is present
    NotFired,     // assessed, and it is not present
    Unavailable,  // could NOT be assessed. Distinct from NotFired, and never merged with it.
};

QString findingStateName(FindingState s);

struct Finding {
    QString      conditionId;
    FindingState state      = FindingState::Unavailable;
    float        confidence = 0.0f;
    QStringList  firedSignals;
    QStringList  missingMeasures;   // Unavailable: exactly which measures were absent, for the UI
    Direction    direction  = Direction::High;   // which tail; meaningful when Fired
};

struct DetectionResult {
    std::vector<Finding> findings;

    const Finding *find(const QString &conditionId) const;
    QStringList    fired() const;
    QStringList    unavailable() const;
};

// Evaluate every Observable condition in the pack. Latent conditions are not evaluated here — they
// have no signals by definition and are resolved by the explanation pass from what they explain.
DetectionResult detect(const CharacteristicPack &pack, const IMeasureSource &source);

} // namespace pinpoint::analysis
