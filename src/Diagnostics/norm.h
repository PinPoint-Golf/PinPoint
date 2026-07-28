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

#include "../Analysis/wrist_assessment_types.h"   // PpRag
#include "characteristic.h"                       // Shape — a property of the MEASURE

#include <QDate>
#include <QString>

#include <cmath>
#include <limits>
#include <optional>
#include <vector>

// Norms — the normative distribution a measured value is graded against.
//
// The word is `norm`, never "reference". `reference` is already taken: it is the "relative to" role
// in a measure's facets (Series::reference, CharacteristicEditorModel::referencesFor). A second
// meaning for the same word would collide in the API, the JSON and the UI at once.
//
// ALL NORMS ARE POPULATION NORMS. The inviolable rule is that a norm is NEVER KEYED BY ATHLETE ID,
// and nothing in this header should ever gain one — a norm that differs per player is a different
// feature with different storage and a different UI. Seating a norm from a selected set of swings
// (the editor's "seat from swings" route) records `n` and the selection scope in provenance, but the
// result still applies to everyone using that norm set.
//
// It used to read "one population for everyone", which the optional `Cohort` on the key retires: a
// cohort is a SEGMENTED population — men 55–64 — not a person. The rule that survives is the one
// about athlete ids, and it is the one that mattered.

namespace pinpoint::analysis {

// Where a norm's numbers came from. This is PROVENANCE ONLY — it never modifies how a grade
// renders. A grade derived from a heuristic norm looks exactly like one derived from a norm seated
// on 500 swings, because colour on a finding encodes distance from the norm and nothing else. A
// norm's standing is surfaced where a user goes to interrogate it (the norm row in MeasureDetail,
// the corridor editor, the health list, a finding's detail page) and nowhere else.
enum class NormSource {
    Heuristic,    // an authored starting figure; every one of these is expected to move
    Seated,       // fitted from a set of swings the user marked as well-positioned
    Literature,   // published figures; `citation` carries the DOI/PMID
    Imported,     // adopted from another norm pack
};

// The grade bands, best first. `NotMeasured` is not a band — it means no norm resolved, or the
// measure had no value. It must never be collapsed into a passing grade: "we could not assess this"
// and "this is fine" are different statements, and merging them turns a capture gap into a clean
// bill of health.
enum class Grade { Ideal, Good, Watch, Action, NotMeasured };

// ONE instance, from AppSettings, applied pack-wide. NOT per-norm and NOT per-context: if "Ideal"
// means one thing in one pack and something else in another, nothing is comparable across athletes
// or across shared packs. Exposed as a single control in Diagnostics settings; deliberately NOT
// exposed in the corridor editor.
//
// Each field is named for the band it CAPS, so the mapping reads off the struct without a table.
struct GradePolicy {
    double idealMaxZ = 1.0;   // |z| <= 1        -> Ideal
    double goodMaxZ  = 2.0;   // 1 < |z| <= 2    -> Good
    double watchMaxZ = 3.0;   // 2 < |z| <= 3    -> Watch;  beyond -> Action
};

// The policy PRESETS, by name. The name is what is persisted and what crosses into QML, because the
// policy has to be one comparable thing across athletes and shared packs — see GradePolicy's own
// comment. Here rather than in the panel that renders them, because more than one surface now needs
// name → numbers (the measures view, the corridor editor and the metric detail / dashboard
// corridors), and a second table would let two of them grade against different z's while showing
// the same word.
struct GradePolicyPreset {
    const char *name;      // stable, persisted token
    const char *label;     // user-facing, translated at the point of render
    const char *hint;
    GradePolicy policy;
};

const std::vector<GradePolicyPreset> &gradePolicyPresets();

// Unknown names resolve to `standard` rather than being kept as themselves: a stored name nothing
// recognises must grade against the default AND read as the default, or the control shows one thing
// while the app does another.
const GradePolicyPreset &gradePolicyPresetFor(const QString &name);
GradePolicy              gradePolicyByName(const QString &name);

// The shipped numbers an override was made against.
//
// Recorded on a USER row when it is saved and never on a shipped one. It is the BASE of a three-way
// comparison, and without it "the shipped corridor has been revised since you overrode it" is
// undecidable: your row differing from the shipped row is also just what an override IS, so a
// two-way comparison would report every edit, forever, as though core had moved.
struct NormBasis {
    double                mu      = 0.0;
    double                sigmaLo = 0.0;
    double                sigmaHi = 0.0;
    std::optional<double> monitorLo;
    std::optional<double> monitorHi;
    // The plausibility bounds are part of the base too. Without them a shipped row that GAINED a
    // cap — which changes a reading from Action to NotMeasured, a large change — would compare as
    // unmoved and the "core has been revised" notice would stay silent.
    std::optional<double> plausibleLo;
    std::optional<double> plausibleHi;
};

// ── Cohort: the optional third term of the norm key ─────────────────────────
//
// A norm may be qualified by the POPULATION SEGMENT it describes. This is not a per-athlete norm and
// must never become one (see the header comment): a cohort names a group, the group is resolved from
// the athlete record at grading time, and the corridor still applies to everyone in it.
//
// Cohort is deliberately NOT a context node. The context tree describes the SHOT — club, shot type,
// archetype — while age and sex are properties of the ATHLETE. Folding one into the other would push
// the cross-product (senior × driver × female × …) through a single-inheritance walk that cannot
// express two orthogonal dimensions without duplicating every row on both.

enum class Sex { Male, Female };

// The age vocabulary is CLOSED and hierarchical: a fixed enum, stable tokens, changed only by a
// schema version bump and NEVER extended by pack content. That is the whole point. The model is
// community-maintained and the literature bands age every way imaginable, so a free numeric range
// would put study-specific arithmetic into the key, where nothing can reconcile it across packs. A
// study whose range does not match a band is mapped by the AUTHOR to the nearest one, with the
// study's actual range recorded verbatim in `citation` — the key stays comparable, the provenance
// stays honest.
//
// Half-open, gapless, total:
//
//     junior                under 18
//     adult                 18+           — a parent, and authorable in its own right
//       adult_18_54
//       adult_55_64
//       adult_65plus
//
// 55 is the seniors threshold in UK club practice, so a golfer already knows which side of it they
// are on and a coach does not have to explain the band. It sits ABOVE the point where the
// trunk-rotation decline literature usually pivots, and that is deliberate: a boundary set later
// than the physiological one means a 55+ row describes a population that has unambiguously started
// to decline, rather than one straddling the onset.
//
// `junior` is one band and is the weakest of them — a 9-year-old and a 17-year-old are barely one
// population — but there is no junior corpus to split against. If one arrives it splits at peak
// height velocity (~14), as a version bump.
//
// `adult` is AUTHORABLE, not merely a parent, because most provenance is no better than "adult male"
// or "adult female". Without it the common case would force three duplicate rows that then drift.
enum class AgeBand {
    Junior,        // under 18
    Adult,         // 18+ — the parent band; never an athlete's OWN band, see cohortProbeOrder()
    Adult18_54,
    Adult55_64,
    Adult65Plus,
};

// The three bands beneath `adult`. An athlete derived from a date of birth always lands in one of
// these (or in Junior); `Adult` itself is a thing an AUTHOR can claim, never a thing a birthday
// produces.
inline bool ageBandIsAdultSubBand(AgeBand b)
{
    return b == AgeBand::Adult18_54 || b == AgeBand::Adult55_64 || b == AgeBand::Adult65Plus;
}

// Either field, both, or neither. Both absent ⇒ UNQUALIFIED, which matches everyone and is what
// every shipped row is.
struct Cohort {
    std::optional<Sex>     sex;
    std::optional<AgeBand> age;

    bool isUnqualified() const { return !sex.has_value() && !age.has_value(); }

    bool operator==(const Cohort &o) const { return sex == o.sex && age == o.age; }
    bool operator!=(const Cohort &o) const { return !(*this == o); }
};

// The cohort keys to try, most specific first, for an athlete whose own cohort is `athlete`.
//
// ONE walk, CONTEXT-MAJOR: the caller probes this whole list at each node of the upward context walk
// and moves up the tree only when none of them is present. That ordering has two intended
// consequences — an unqualified `driver` row beats a senior row at `any` for a driver shot (stance
// width is club-mechanical; if senior-driver matters, author it), and a senior row at `any` resolves
// for a senior wherever no club-specific row exists, which is the ROM family, exactly where cohorts
// matter.
//
//     1. sex + exact age band       (female, adult_55_64)
//     2. sex + adult                (female, adult)        — skipped for a junior
//     3. exact age band             (adult_55_64)
//     4. adult                      (adult)                — skipped for a junior
//     5. sex                        (female)
//     6. unqualified
//
// A FIXED order rather than a specificity score with a tie rule: a community pack must never fail to
// load, or resolve unpredictably, because two rows came out equally specific. Age band sits ahead of
// sex at equal specificity (3 before 5) because the age effect on the ROM measures where cohorts
// matter is larger and monotone, while sex differences are partly absorbed by the body-normalised
// units the setup measures already use.
//
// An axis the athlete has no answer on is SKIPPED, never guessed: unknown date of birth, unknown sex
// or a declined answer means only rows unqualified on that axis can match — and the reading STILL
// GRADES, against the universal corridor. "We don't know your age" and "we could not assess this"
// are different statements.
//
// An athlete with neither answer therefore yields exactly ONE probe, the unqualified one, so
// resolution costs precisely what it cost before cohorts existed.
std::vector<Cohort> cohortProbeOrder(const Cohort &athlete);

struct Norm {
    // Keys on the MEASURE (post-reducer), never the metric key. A measure carries its reducer and
    // its phase, so "Δ-from-address at P4" and "absolute at impact" are different measures and
    // cannot be confused for one another. Keying on the metric would need a deltaFromAddress flag
    // to say the same thing, and a flag can be ignored where a distinct key cannot.
    QString measureId;
    QString contextId;        // a node in the context tree; resolution walks upward from here

    // The third term of the key, and the only optional one. Absent on every shipped row.
    Cohort  cohort;

    double  mu      = 0.0;
    // Asymmetric by design, not as an option. Ball position forward is tolerated far more than back,
    // and the same holds for alignment and stance width. A symmetric norm would have to be centred
    // wrong to express either side correctly.
    double  sigmaLo = 0.0;    // tolerance below mu
    double  sigmaHi = 0.0;    // tolerance above mu

    // ABSOLUTE bounds on the Watch band. Present ONLY to preserve migrated content exactly, and the
    // corridor editor never exposes them.
    //
    // The bands migrated out of reference_bands.cpp add a fixed number of degrees either side of
    // green, which is not a fixed multiple of the green half-width and so cannot be reproduced by
    // any global z policy: kRadUln P1 is green ±3 with a 5.0 margin (amber = 2.67x green) while P2
    // is green ±10 with the same 5.0 margin (amber = 1.5x green). The tempo corridor is not
    // expressible at all — green 2.2..3.0 with amber 1.8..3.6 needs a low margin of 0.4 and a high
    // margin of 0.6. Absent (the normal case for anything authored in the editor) the band is
    // derived from GradePolicy.
    std::optional<double> monitorLo;
    std::optional<double> monitorHi;

    // OUTSIDE THESE THE READING IS NOT BELIEVED, at any grade.
    //
    // A different question from every other number on this row. The corridor asks "is this swing
    // good?"; this asks "is this reading real?", and the two must never be merged. A driver smash
    // of 1.62 is not a swing finding — it is a mis-tracked ball — so grading it, in EITHER
    // direction, would launder a capture fault into a confident diagnosis. Outside these bounds the
    // grade is NotMeasured and the reading carries a distinct `implausible` flag, so a surface can
    // say "reading outside the plausible range — check the capture" instead of "not measured".
    // Those are different statements and nothing may collapse them, exactly as NotMeasured must
    // never collapse into a passing grade.
    //
    // On the norm row rather than the measure — unlike `Shape` — because the physical cap IS
    // context-dependent: smash is bounded by loft, so a driver, an iron and a wedge cap differently
    // while all three are the same one-sided quantity. They parse like the monitor bounds and may
    // appear singly.
    //
    // This is also the far edge of an open tail: a floor is open above UP TO PLAUSIBILITY, never to
    // infinity.
    std::optional<double> plausibleLo;
    std::optional<double> plausibleHi;

    int        n      = 0;                        // sample size behind it; 0 for a heuristic
    NormSource source = NormSource::Heuristic;
    QString    unit;                              // MUST match the measure's unit; load fails if not
    QString    author;
    QString    citation;                          // DOI/PMID, or the note explaining a provisional figure
    QDate      setOn;

    // Set only on a row in a non-core layer, at save time — see NormBasis. Absent on every shipped
    // row, and absent on user rows written before it existed, where "has core moved?" is genuinely
    // unknown rather than answered no.
    std::optional<NormBasis> basedOn;

    // Does this norm state its Watch edge outright, rather than deriving it from the policy?
    //
    // SHAPE-AWARE, and it has to be. Requiring both bounds is right for a Target and wrong for
    // everything else: a floor's high tail is open, so `monitorLo` alone is a COMPLETE monitor
    // band there. Answering false would make grade() and bandEdgesOf() silently ignore a bound the
    // author wrote down — the corridor would look authored and grade as though it were not.
    bool hasExplicitMonitor(Shape shape = Shape::Target) const
    {
        switch (shape) {
        case Shape::Floor:   return monitorLo.has_value();
        case Shape::Ceiling: return monitorHi.has_value();
        case Shape::Target:  break;
        }
        return monitorLo.has_value() && monitorHi.has_value();
    }

    // Past the explicit Watch edge on a tail that grades. The open tail is never outside it,
    // whatever a stray bound says — `normShapeMonitor` refuses one, but a pack that loaded with
    // errors must not then grade against the thing that was refused.
    bool outsideMonitor(double value, Shape shape = Shape::Target) const
    {
        if (shape != Shape::Ceiling && monitorLo.has_value() && value < *monitorLo) return true;
        if (shape != Shape::Floor   && monitorHi.has_value() && value > *monitorHi) return true;
        return false;
    }

    // Outside what this norm is willing to believe. Shape-INDEPENDENT: an open tail is open up to
    // plausibility, and a capture fault is a capture fault on either side of any shape.
    bool isImplausible(double value) const
    {
        return (plausibleLo.has_value() && value < *plausibleLo)
            || (plausibleHi.has_value() && value > *plausibleHi);
    }

    // THE NORM'S OWN CLAIM, in the measure's own units: mu +/- one tolerance either side.
    //
    // This is NOT the Ideal band, and the two must never share a name again. The Ideal band is
    // `mu +/- idealMaxZ * sigma` — it MOVES when the user changes the grade policy. The claim does
    // not: it is what this norm asserts about the population, and a sensitivity setting has no
    // business editing an assertion.
    //
    // They coincide under the `standard` preset (idealMaxZ = 1.0), which is exactly why calling
    // both of them "ideal" survived for nine stages. Under `strict` (0.75) a value at 0.9 sigma was
    // DRAWN inside the green band and GRADED Good — and ragOf(Good) is Amber, so one number
    // produced a green band and an amber chip. `withinBand`'s comment below exists to eliminate
    // that class of disagreement at float-epsilon scale; this was the same disagreement at
    // whole-band scale.
    //
    // Use the claim for AUTHORING and DIFF — the corridor editor's handles, the comparison against
    // a shipped or parent row, the validator. Use `bandEdgesOf()` for anything that RENDERS or
    // GRADES. Those are no longer interchangeable, and each call site is a deliberate choice.
    double claimLo() const { return mu - sigmaLo; }
    double claimHi() const { return mu + sigmaHi; }
};

// The grade policy's bands must nest. Asserted rather than left as a property of three
// hand-written presets, because the engine silently depends on it: `onTail` compares a value
// against the Ideal edge while `deviated` comes from grade(), so a policy with goodMaxZ < idealMaxZ
// would let a reading be a deviation that is not on either tail — a signal that can never fire, for
// a reason nothing reports.
inline bool gradePolicyIsOrdered(const GradePolicy &p)
{
    return p.idealMaxZ > 0.0 && p.goodMaxZ >= p.idealMaxZ && p.watchMaxZ >= p.goodMaxZ;
}

// Signed distance from the norm, in tolerances, computed PER SIDE so an asymmetric norm grades
// asymmetrically. A zero tolerance on the relevant side yields infinity rather than a division by
// zero: a norm with no tolerance admits only its own centre, which is a degenerate but well-defined
// statement, and the validator warns about it separately.
//
// On the UNGRADED side of a one-sided norm the answer is exactly 0, not a positive distance. A
// future 0-100 characteristic score built on z must not reward overshooting a floor: a smash of
// 1.60 is not twice as good as 1.48, it is the same answer — "the strike is efficient" — and a
// score that climbed past the aspiration would invent a target nobody set.
//
// Continuous at mu by construction: both formulations give 0 there.
inline double normZ(double value, const Norm &norm, Shape shape = Shape::Target)
{
    if (shape == Shape::Floor   && value >= norm.mu) return 0.0;
    if (shape == Shape::Ceiling && value <= norm.mu) return 0.0;

    const double sigma = (value < norm.mu) ? norm.sigmaLo : norm.sigmaHi;
    if (!(sigma > 0.0))
        return (value == norm.mu) ? 0.0 : std::numeric_limits<double>::infinity();

    return (value - norm.mu) / sigma;
}

// Is `value` inside the band `threshold` tolerances wide?
//
// Expressed as a comparison against COMPUTED EDGES rather than as `|z| <= threshold`, and that is
// the whole point of it existing.
//
// grade() and bandEdgesOf() are not exact inverses in floating point. bandEdgesOf computes
// `mu - k*sigmaLo`; asking `(value - mu)/sigmaLo <= k` is a different calculation, and for
// mu = 1.40, sigmaLo = 0.08 the edge is 1.3199999999999998 while dividing that difference back
// gives 1.0000000000000009. A value sitting exactly ON the drawn edge therefore graded Good while
// the band it was drawn from called it Green — two user-visible paths disagreeing about one number.
// `reference_bands_test` sweeps every band edge to ±1e-9 hunting for exactly that.
//
// An epsilon does not fix it; it moves it. A tolerance on the z comparison makes grade() round in
// samples that the band still excludes, which is the same disagreement with the sign flipped. What
// removes the whole class is computing the edge the SAME WAY on both paths and comparing inclusively
// against it — then the two agree by construction, at every magnitude, with no fudge factor.
// The one-sided shapes INHERIT that doctrine rather than reproducing it: a floor's single computed
// edge is `mu - threshold * sigmaLo`, compared inclusively, exactly as a target's low edge is. The
// open side is admitted outright, so the good side of a floor is inside every band at every
// threshold — Ideal by construction, at every policy.
inline bool withinBand(double value, const Norm &norm, double threshold,
                       Shape shape = Shape::Target)
{
    // The degenerate case needs no branch of its own, and used to have one. With sigma = 0 the
    // computed edge is `mu - threshold * 0 == mu`, so the inclusive comparison already says
    // "only its own centre" — the same answer the old early return gave, by the same arithmetic
    // the rest of the function uses. One expression, no second path to keep in step.
    const bool okLo = (shape == Shape::Ceiling) || value >= norm.mu - threshold * norm.sigmaLo;
    const bool okHi = (shape == Shape::Floor)   || value <= norm.mu + threshold * norm.sigmaHi;
    return okLo && okHi;
}

// Grade a value against a norm.
//
// Precedence, when the norm carries explicit monitor bounds:
//     outside [monitorLo, monitorHi]  -> Action
//     otherwise                       -> the z-derived band, capped at Watch
//
// The cap is what makes migrated content exact. Across all 39 rows migrated from reference_bands
// the explicit monitor bounds are strictly tighter than 3 sigma, so a value inside them can never
// legitimately reach Action by z alone — and a value outside them was RED under the old classifier
// regardless of how few tolerances out it was. Both halves of the rule are needed to reproduce that.
// PLAUSIBILITY OUTRANKS EVERYTHING, including the monitor band. A reading the norm does not believe
// is not graded at all — not Action, not Ideal. Grading it either way would answer a question about
// the swing using a number that describes the capture, and the more confident the answer looked the
// worse it would be. See Norm::plausibleLo.
inline Grade grade(double value, const Norm &norm, const GradePolicy &policy = {},
                   Shape shape = Shape::Target)
{
    if (norm.isImplausible(value))
        return Grade::NotMeasured;

    if (norm.hasExplicitMonitor(shape)) {
        if (norm.outsideMonitor(value, shape))
            return Grade::Action;
        if (withinBand(value, norm, policy.idealMaxZ, shape)) return Grade::Ideal;
        if (withinBand(value, norm, policy.goodMaxZ, shape))  return Grade::Good;
        return Grade::Watch;                      // capped: inside the monitor band is never Action
    }

    if (withinBand(value, norm, policy.idealMaxZ, shape)) return Grade::Ideal;
    if (withinBand(value, norm, policy.goodMaxZ, shape))  return Grade::Good;
    if (withinBand(value, norm, policy.watchMaxZ, shape)) return Grade::Watch;
    return Grade::Action;
}

// True when the grade says something deviated. NotMeasured is NOT a deviation — see the enum.
inline bool isDeviation(Grade g) { return g == Grade::Watch || g == Grade::Action; }

// ── The edges a norm DRAWS as ───────────────────────────────────────────────
//
// ONE definition, because three surfaces project a norm into a corridor to render it — the wrist
// grid (NormBandProvider → Band → PpRagCell), the measures view (NormModel::normAt) and the metric
// detail page / dashboard rails (metric_corridor.h) — and each of them must draw the edge that
// grade() actually applies. A second copy is how a surface ends up showing a corridor the app does
// not use: the Watch edge is `monitorLo/Hi` when the norm states them and z-derived when it does
// not, and that precedence has to be stated once.
//
// EVERY edge here is policy-DEPENDENT. The Ideal edge is `mu -/+ idealMaxZ * sigma` and the Watch
// edge is `mu -/+ watchMaxZ * sigma` (or the explicit monitor band, which dominates). One edge,
// computed one way, on both the drawing path and the grading path.
//
// This was not always true. Until the fix, `idealLo/idealHi` here were `mu -/+ sigma` — the norm's
// own claim — while grade() applied `policy.idealMaxZ`. Under `standard` the two coincide; under
// `strict` and `lenient` they do not, and the band a surface drew was not the band the app graded.
//
// If you want the norm's own claim — for the editor's handles, or a diff against a shipped or
// parent row — that is `Norm::claimLo()/claimHi()`, and it is a DIFFERENT question. See the comment
// on those.
struct NormBandEdges {
    double idealLo = 0.0, idealHi = 0.0;   // mu −/+ idealMaxZ * sigma
    double watchLo = 0.0, watchHi = 0.0;   // where Action begins

    // This tail does not grade — the corridor runs off the end of the plot rather than stopping.
    //
    // The numeric edge on an open side is `mu`, the ASPIRATION POINT, and never a sentinel:
    // infinity and NaN must not cross into QML, where `.toDouble()` on a missing key already
    // yields 0.0 and every `|| 0` fallback would turn a sentinel into a corridor at the origin. mu
    // is the only number the norm actually states on that side, everything beyond it is Ideal by
    // construction, and a surface that ignores these flags therefore still draws something TRUE:
    // green up to the aspiration, amber out to the watch edge, which reads as "get to 1.48".
    bool   lowOpen = false, highOpen = false;
};

// `marginOverride` is the SwingLab `bands.*` sweep (negative ⇒ not set), which replaces the Watch
// edge with the Ideal band widened by a fixed number of units either side. It exists because half
// the shipped norms do not store their Watch edge as a margin at all, so sweeping "the margin"
// without it would silently do nothing on those.
inline NormBandEdges bandEdgesOf(const Norm &n, const GradePolicy &policy = {},
                                 double marginOverride = -1.0, Shape shape = Shape::Target)
{
    NormBandEdges e;
    e.lowOpen  = (shape == Shape::Ceiling);
    e.highOpen = (shape == Shape::Floor);

    // Computed the same way withinBand() computes it, so the drawn edge and the graded edge agree
    // by construction at every magnitude — the float-edge doctrine, applied to the band it had
    // never been applied to.
    e.idealLo = n.mu - policy.idealMaxZ * n.sigmaLo;
    e.idealHi = n.mu + policy.idealMaxZ * n.sigmaHi;
    if (marginOverride >= 0.0) {
        e.watchLo = e.idealLo - marginOverride;
        e.watchHi = e.idealHi + marginOverride;
    } else if (n.hasExplicitMonitor(shape)) {
        // value_or, not a dereference: on a one-sided norm the open side's bound is legitimately
        // absent, and hasExplicitMonitor() has already said the graded side's is there.
        e.watchLo = n.monitorLo.value_or(n.mu - policy.watchMaxZ * n.sigmaLo);
        e.watchHi = n.monitorHi.value_or(n.mu + policy.watchMaxZ * n.sigmaHi);
    } else {
        e.watchLo = n.mu - policy.watchMaxZ * n.sigmaLo;
        e.watchHi = n.mu + policy.watchMaxZ * n.sigmaHi;
    }

    // The open side collapses to the aspiration point, AFTER any margin sweep — so marginOverride
    // widens the graded side only, which is what sweeping "the margin" on a one-sided norm can
    // mean. See NormBandEdges for why mu rather than a sentinel.
    if (e.highOpen) e.idealHi = e.watchHi = n.mu;
    if (e.lowOpen)  e.idealLo = e.watchLo = n.mu;
    return e;
}

// The 4-band grade collapsed to the legacy 3-band PpRag the wrist grid renders.
//
// ONE definition, here, because two surfaces consume it and a second copy would let them drift:
// NormBandProvider projects a Norm into a Band and the wrist grid runs classifyDelta() over it,
// while the characteristic engine grades the same value through grade(). Those two paths must
// agree on colour for the same number, and `reference_bands_test` asserts they do over the whole
// shipped set — including a norm with no explicit monitor, where the Action edge is z-derived.
//
// KNOWN CONSEQUENCE, ACCEPTED: this is not a 2:1:1 mapping. For migrated rows Ideal is exactly the
// old green band, so Good AND Watch both sit inside the old amber. A surface showing a grade word
// and a RAG chip together will therefore look inconsistent — "Good" beside an amber dot reads as a
// contradiction. Show one or the other, never both.
inline PpRag ragOf(Grade g)
{
    switch (g) {
    case Grade::Ideal:       return PpRag::Green;
    case Grade::Good:        return PpRag::Amber;
    case Grade::Watch:       return PpRag::Amber;
    case Grade::Action:      return PpRag::Red;
    case Grade::NotMeasured: return PpRag::Grey;
    }
    return PpRag::Grey;
}

// ── Provenance standing ─────────────────────────────────────────────────────
//
// The sample size below which a seated norm is still an anecdote. Not a statistical threshold and
// not presented as one — it is the point at which "fitted from swings" stops overstating itself.
inline constexpr int kMinSeatedN = 30;

// True when a norm's numbers should be read as a starting point rather than a finding.
//
// THIS NEVER MODIFIES A GRADE. A value three tolerances outside a heuristic norm is three
// tolerances out, and rendering it more softly because the norm is young would hide the deviation
// behind a caveat about the corridor. Weakness is surfaced only where a user goes to interrogate
// the norm itself — the MeasureDetail norm row, the corridor editor, the health list, a finding's
// detail page — and nowhere else. See NormSource's own comment.
bool    normIsWeak(const Norm &n);

// Why, in one line, for the row that says so. Empty when the norm is not weak.
QString normWeakReason(const Norm &n);

// ── Enum <-> string (the JSON spelling) ─────────────────────────────────────
QString    normSourceName(NormSource s);
bool       normSourceFromName(const QString &s, NormSource &out);

QString    sexName(Sex s);
bool       sexFromName(const QString &s, Sex &out);
QString    sexLabel(Sex s);                    // "men" / "women"

QString    ageBandName(AgeBand b);
bool       ageBandFromName(const QString &s, AgeBand &out);
QString    ageBandLabel(AgeBand b);            // "under 18" · "18+" · "55–64" · "65+"

// A cohort as words: "" (unqualified) · "men" · "55–64" · "men 55–64".
//
// Empty for the unqualified cohort ON PURPOSE, so a caller can append it unconditionally and a row
// that applies to everyone says nothing rather than saying "everyone". Every surface that will name
// a cohort — the norm row, the corridor editor, a health notice — renders it through here, for the
// reason normSourceLabel exists: four surfaces spelling one segment three ways is how "men 55–64"
// and "male, 55-64" end up on the same screen.
QString    cohortLabel(const Cohort &c);

// The legal tokens, for an error message that has to name what WOULD have been accepted. Generated
// from the same tables the parser reads, so a vocabulary change cannot leave the message behind.
QString    sexTokenList();
QString    ageBandTokenList();

// The user-facing words for a norm's provenance. WITH the enum, not in a façade: the measures view
// and the metric detail page both render it, and two copies of four strings is how they drift.
QString    normSourceLabel(NormSource s);
QString    gradeName(Grade g);
bool       gradeFromName(const QString &s, Grade &out);

// The user-facing words. Ideal / Good / Watch / Action, plus "Not measured" — an ordinary,
// monotone ramp where the top band says what to do rather than describing a boundary.
QString    gradeLabel(Grade g);

// ── Saying what a corridor is, in words ─────────────────────────────────────
//
// WITH the vocabulary and not in any one façade, for the reason normSourceLabel is: six surfaces
// render a corridor as a sentence — the measures list, the measure detail rows, the catalogue, the
// corridor editor, its import list, the health notices — and six copies of "%1 to %2" is how a
// floor ends up reading "1.48 to 1.53" on five of them.
//
// A one-sided corridor has no second bound to name. Saying one anyway is not a cosmetic slip: it
// states a limit the norm does not have, on the side it explicitly refuses to grade.

// Enough decimals to be FAITHFUL — one to four, never more than the number needs.
//
// The pack is MOSTLY authored at one decimal, which is why every surface was fixed there. The
// ratios are not: smash factor is mu 1.48 with a tolerance of 0.05, and at one decimal its Ideal
// and Good edges (1.43, 1.38) render as the same "1.4".
QString    normNumber(double v);

// A corridor as a fragment: "1.4 to 1.5" · "at least 1.48" · "no more than 12".
//
// Takes `mu` as well as the pair, and uses ONLY mu on a one-sided norm. That is not an economy —
// it is the difference between the two things a caller might be holding. On a floor, a BAND's high
// edge is already collapsed onto mu by bandEdgesOf, but a CLAIM's is mu + a tolerance nothing
// grades. Passing mu explicitly makes both callers correct without either having to know which it
// is holding.
QString    rangePhrase(double lo, double hi, double mu, Shape shape);

// Where Action begins: "action beyond 1.3 to 1.6" · "action below 1.33" · "action above 12.5".
// Never "beyond" on a one-sided norm — the open tail has no fault edge to be beyond.
QString    actionPhrase(double watchLo, double watchHi, Shape shape);

// A reading the norm does not believe, in its own register — NOT a grade and NOT "not measured".
//
// The three are different statements and the difference is the point (norm.h's opening argument,
// one level further out). "Not measured" is a capture GAP: nothing arrived. Action is a swing
// FINDING: something arrived and it was poor. This is a capture FAULT: something arrived, and it
// is not a number this instrument can produce. The reading is SHOWN rather than hidden, because
// hiding it is what makes a mis-tracked ball look like an absence.
QString    implausibleLabel();
QString    implausibleNote(double value, const QString &unit);

} // namespace pinpoint::analysis
