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
#include "context_tree.h"
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
    // The POLICY's Ideal band, in the measure's own units — `mu -/+ idealMaxZ * sigma`, the same
    // edge bandEdgesOf() draws and grade() applies. NOT the norm's bare claim: `onTail` below
    // compares against these while `deviated` comes from the grade, and two scales would let a
    // reading be a deviation on neither tail.
    double greenLo     = 0.0;
    double greenHi     = 0.0;
    float  confidence  = 1.0f;   // 0..1; propagates into the finding

    // The resolved grade. NotMeasured whenever hasCorridor is false, so the two can never disagree.
    //
    // A SIGNAL FIRES ON A DEVIATION — Watch or Action — not merely on leaving the Ideal band. Ideal
    // is |z| <= 1, so firing there would trip roughly a third of any normal population on every
    // characteristic in the library, and a detector that flags a third of everyone is noise wearing
    // a diagnosis's clothes. Good (|z| <= 2) is ordinary variation and says so.
    Grade grade = Grade::NotMeasured;

    // Which tail of this corridor does not grade, from the MEASURE's shape. A signal pointing at an
    // open tail can never fire, and says so here rather than being discovered by arithmetic.
    bool  lowOpen  = false;
    bool  highOpen = false;

    // The value fell outside what the norm is willing to believe, so `grade` is NotMeasured. A
    // THIRD state, and it must not be folded into either neighbour: "we could not assess this"
    // (no producer, no corridor) and "this reading is not real — check the capture" call for
    // different actions, and neither is "this is fine". See Norm::plausibleLo.
    bool  implausible = false;

    // Where the norm came from, for the UI's "inherited from full swing" line. Empty when none
    // resolved.
    QString normContextId;
    // WHICH POPULATION graded it. Unqualified for every shipped corridor, and unqualified is not a
    // missing answer — it is "this corridor describes everyone". A finding that names its cohort is
    // how a golfer reads an improved grade as the corridor becoming right for them.
    Cohort  normCohort;
    // The shot did not declare a context, so it was graded against the default. The finding's
    // confidence is demoted for this — see the engine.
    bool    contextInferred = false;

    // Build a graded reading from a bare corridor, for producers that have a band but no Norm.
    //
    // Use this rather than setting hasCorridor by hand: a reading with a corridor and a default
    // NotMeasured grade silently never fires, which looks exactly like "nothing was wrong" and is
    // the one failure mode this whole module exists to prevent.
    //
    // The band handed in is an IDEAL band, so inverting it back to a Norm has to undo the same
    // scaling bandEdgesOf() applied: the half-width is `idealMaxZ * sigma`, not sigma. Dividing by
    // 2 alone was right only while the Ideal band was drawn at mu +/- sigma on every preset, which
    // is exactly the defect this stage removed — under `strict` it would have produced a norm with
    // three-quarters of the real tolerance and graded every reading harder than the corridor it
    // was built from.
    static MeasureReading fromCorridor(double value, double greenLo, double greenHi,
                                       float confidence = 1.0f, const GradePolicy &policy = {})
    {
        MeasureReading r;
        r.value       = value;
        r.confidence  = confidence;
        r.hasCorridor = true;
        r.greenLo     = greenLo;
        r.greenHi     = greenHi;

        const double halfWidth = (greenHi - greenLo) / 2.0;
        const double k         = policy.idealMaxZ > 0.0 ? policy.idealMaxZ : 1.0;

        Norm n;
        n.mu      = (greenLo + greenHi) / 2.0;
        n.sigmaLo = n.sigmaHi = halfWidth / k;
        // Shape::Target, explicitly, and not a stand-in for a missing Measure: this factory's own
        // inputs are a bare (greenLo, greenHi) pair, which is a TWO-SIDED band by construction — a
        // one-sided producer has no greenHi (or no greenLo) to hand in, so a caller reaching this
        // path could not express Floor or Ceiling even if it had a Measure to read one off.
        // Qualified: the `grade` MEMBER shadows the free grade() inside this scope.
        r.grade   = ::pinpoint::analysis::grade(value, n, Shape::Target, policy);
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

// THE NUMBER BEHIND A FINDING. Which measure graded it, what it read, the corridor it was tested
// against, and how far out it sat — carried for NotFired as well as Fired, because a ledger that
// records only what fired cannot show that a corridor was cleared by a hair on Tuesday and by a
// mile on Thursday. A verdict with no number behind it is an assertion; this is the receipt.
//
// hasEvidence is false for Unavailable findings and for those only. Nothing was read that graded
// them, and inventing a zero here would put a value in the ledger that no measure ever produced —
// the same false-bill-of-health mistake the FindingState enum exists to refuse.
struct MeasureEvidence {
    bool    hasEvidence = false;

    QString drivingMeasureId;   // the measure whose reading decided it
    QString drivingSignalId;    // and the signal that read it — Finding::direction is the LAST
                                // firing signal's tail, so the two can name different signals when
                                // several fire; this says which one the numbers below came from.

    double  value = 0.0;

    // The corridor as MeasureReading held it — the POLICY's Ideal band, not the norm's bare claim.
    // hasCorridor is false for Threshold and Ratio signals, which grade against an authored number
    // and legitimately have no band; value is still evidence, and z is left at 0 rather than being
    // faked against a corridor that does not exist.
    bool    hasCorridor = false;
    double  corridorLo  = 0.0;
    double  corridorHi  = 0.0;
    bool    lowOpen     = false;
    bool    highOpen    = false;

    // ── z: signed, monotone, |z| == 1 at the graded edge ────────────────────────
    //
    // The ledger reads z ORDINALLY — trends across a session, the best and worst swings of a set —
    // so MONOTONICITY IN `value` IS THE PROPERTY THAT MATTERS, and the scale is only there to make
    // two swings of the same measure comparable. Every branch below is therefore affine and
    // increasing in `value`; the branches differ only in where zero sits and what one unit means.
    //
    //   TWO-SIDED corridor:  z = (value - mid) / halfWidth,  mid = (lo+hi)/2, halfWidth = (hi-lo)/2
    //       0 at the middle of the band, ±1 exactly on each edge, sign naming the tail.
    //
    //   ONE-SIDED corridor:  z = (value - mu) / (hi - lo)
    //       On a floor bandEdgesOf() collapses the open (high) edge onto mu, so `hi` IS mu and the
    //       formula reads "distance from the aspiration point, in ideal-band widths": -1 at the
    //       graded low edge, positive out into the open side. A ceiling is the mirror, with `lo`
    //       collapsed onto mu.
    //
    //       WHY NOT THE MIDPOINT HERE. A one-sided band's midpoint is halfway between the
    //       aspiration and the fault edge — a point the measure says nothing about — so a z built
    //       on it would put 0 where nothing happens and ±1 where nothing happens either.
    //
    //       WHY NOT normZ(), which is the same quantity divided by sigma rather than by the ideal
    //       width. normZ() deliberately returns exactly 0 all along the open side of a one-sided
    //       norm, so a floor cleared by a hair and a floor cleared by a mile are the same number.
    //       That is right for scoring — overshooting an aspiration is not an achievement — and
    //       wrong for a ledger, which needs those two swings to be orderable. So: this z is not
    //       normZ and must not be substituted for it.
    //
    //   NO corridor, degenerate (zero-width) corridor, or a band open on both sides: z = 0. There
    //   is no scale to normalise by, and 0 is the only answer that does not invent one.
    double  z = 0.0;

    static MeasureEvidence fromReading(const QString &signalId, const QString &measureId,
                                       const MeasureReading &r)
    {
        MeasureEvidence e;
        e.hasEvidence      = true;
        e.drivingSignalId  = signalId;
        e.drivingMeasureId = measureId;
        e.value            = r.value;
        e.hasCorridor      = r.hasCorridor;
        if (!r.hasCorridor) return e;

        e.corridorLo = r.greenLo;
        e.corridorHi = r.greenHi;
        e.lowOpen    = r.lowOpen;
        e.highOpen   = r.highOpen;

        const double width = r.greenHi - r.greenLo;
        if (!(width > 0.0) || (r.lowOpen && r.highOpen)) return e;   // no scale to normalise by

        if (r.lowOpen)        e.z = (r.value - r.greenLo) / width;             // ceiling: lo IS mu
        else if (r.highOpen)  e.z = (r.value - r.greenHi) / width;             // floor:   hi IS mu
        else                  e.z = (r.value - (r.greenLo + r.greenHi) / 2.0) / (width / 2.0);
        return e;
    }
};

struct Finding {
    QString      conditionId;
    FindingState state      = FindingState::Unavailable;
    float        confidence = 0.0f;
    QStringList  firedSignals;
    QStringList  missingMeasures;   // Unavailable: exactly which measures were absent, for the UI
    Direction    direction  = Direction::High;   // which tail; meaningful when Fired

    // Resolved from the condition's context bindings for the shot's context. RANKING ONLY: an
    // immaterial finding is reported exactly like any other and is never softened, hidden or
    // reworded — it simply carries no weight when the explanation pass ranks root causes. See
    // ContextBinding::material, whose comment this implements.
    bool         material   = true;

    // The reading that graded this finding. See MeasureEvidence: present for Fired and NotFired
    // alike, absent for Unavailable.
    MeasureEvidence evidence;
};

struct DetectionResult {
    std::vector<Finding> findings;

    const Finding *find(const QString &conditionId) const;
    QStringList    fired() const;
    QStringList    unavailable() const;
};

// Evaluate every Observable condition in the pack. Latent conditions are not evaluated here — they
// have no signals by definition and are resolved by the explanation pass from what they explain.
//
// Retired and Superseded conditions are skipped: those two states mean "withdrawn", and withdrawn
// content must not diagnose. The other four states are editorial confidence rather than withdrawal
// and are all evaluated — most of the shipped pack is Draft, so treating Draft as withdrawn would
// silently dark the library.
//
// `contexts` + `contextId` say what KIND OF SHOT this was, and a condition whose bindings make it
// inapplicable there is OMITTED from the result entirely — not reported as NotFired (which would
// claim it was assessed and found absent) and not as Unavailable (which would claim the app tried
// and could not). It was never asked. Pass nothing and every condition is evaluated, which is what
// every caller did before bindings became editable and is what the shipped pack — which carries no
// binding rows at all — means either way.
DetectionResult detect(const CharacteristicPack &pack, const IMeasureSource &source,
                       const ContextTree *contexts = nullptr, const QString &contextId = QString());

} // namespace pinpoint::analysis
