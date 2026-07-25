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
#include "norm.h"

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
    double greenLo     = 0.0;    // the Ideal band, in the measure's own units
    double greenHi     = 0.0;
    float  confidence  = 1.0f;   // 0..1; propagates into the finding

    // The resolved grade. NotMeasured whenever hasCorridor is false, so the two can never disagree.
    //
    // A SIGNAL FIRES ON A DEVIATION — Watch or Action — not merely on leaving the Ideal band. Ideal
    // is |z| <= 1, so firing there would trip roughly a third of any normal population on every
    // characteristic in the library, and a detector that flags a third of everyone is noise wearing
    // a diagnosis's clothes. Good (|z| <= 2) is ordinary variation and says so.
    Grade grade = Grade::NotMeasured;

    // Where the norm came from, for the UI's "inherited from full swing" line. Empty when none
    // resolved.
    QString normContextId;
    // The shot did not declare a context, so it was graded against the default. The finding's
    // confidence is demoted for this — see the engine.
    bool    contextInferred = false;

    // Build a graded reading from a bare corridor, for producers that have a band but no Norm.
    //
    // Use this rather than setting hasCorridor by hand: a reading with a corridor and a default
    // NotMeasured grade silently never fires, which looks exactly like "nothing was wrong" and is
    // the one failure mode this whole module exists to prevent. The band is read as +/-1 sigma
    // about its midpoint — the same conversion the migrated corridors use — so a value inside the
    // band grades Ideal and the numbers mean the same thing on both paths.
    static MeasureReading fromCorridor(double value, double greenLo, double greenHi,
                                       float confidence = 1.0f, const GradePolicy &policy = {})
    {
        MeasureReading r;
        r.value       = value;
        r.confidence  = confidence;
        r.hasCorridor = true;
        r.greenLo     = greenLo;
        r.greenHi     = greenHi;

        Norm n;
        n.mu      = (greenLo + greenHi) / 2.0;
        n.sigmaLo = n.sigmaHi = (greenHi - greenLo) / 2.0;
        // Qualified: the `grade` MEMBER shadows the free grade() inside this scope.
        r.grade   = ::pinpoint::analysis::grade(value, n, policy);
        return r;
    }
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
