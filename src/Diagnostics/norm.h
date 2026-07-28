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
// ALL NORMS ARE POPULATION NORMS. There is no per-athlete norm and no personal baseline here. A
// norm is never keyed by athlete id, and nothing in this header should ever gain one — a norm that
// differs per player is a different feature with different storage and a different UI. Seating a
// norm from a selected set of swings (the editor's "seat from swings" route) records `n` and the
// selection scope in provenance, but the result still applies to everyone using that norm set.

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
};

struct Norm {
    // Keys on the MEASURE (post-reducer), never the metric key. A measure carries its reducer and
    // its phase, so "Δ-from-address at P4" and "absolute at impact" are different measures and
    // cannot be confused for one another. Keying on the metric would need a deltaFromAddress flag
    // to say the same thing, and a flag can be ignored where a distinct key cannot.
    QString measureId;
    QString contextId;        // a node in the context tree; resolution walks upward from here

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

    bool hasExplicitMonitor() const { return monitorLo.has_value() && monitorHi.has_value(); }

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
inline double normZ(double value, const Norm &norm)
{
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
inline bool withinBand(double value, const Norm &norm, double threshold)
{
    const double sigma = (value < norm.mu) ? norm.sigmaLo : norm.sigmaHi;
    if (!(sigma > 0.0))
        return value == norm.mu;      // a norm admitting only its own centre; the validator warns
    return (value >= norm.mu - threshold * norm.sigmaLo)
        && (value <= norm.mu + threshold * norm.sigmaHi);
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
inline Grade grade(double value, const Norm &norm, const GradePolicy &policy = {})
{
    if (norm.hasExplicitMonitor()) {
        if (value < *norm.monitorLo || value > *norm.monitorHi)
            return Grade::Action;
        if (withinBand(value, norm, policy.idealMaxZ)) return Grade::Ideal;
        if (withinBand(value, norm, policy.goodMaxZ))  return Grade::Good;
        return Grade::Watch;                      // capped: inside the monitor band is never Action
    }

    if (withinBand(value, norm, policy.idealMaxZ)) return Grade::Ideal;
    if (withinBand(value, norm, policy.goodMaxZ))  return Grade::Good;
    if (withinBand(value, norm, policy.watchMaxZ)) return Grade::Watch;
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
};

// `marginOverride` is the SwingLab `bands.*` sweep (negative ⇒ not set), which replaces the Watch
// edge with the Ideal band widened by a fixed number of units either side. It exists because half
// the shipped norms do not store their Watch edge as a margin at all, so sweeping "the margin"
// without it would silently do nothing on those.
inline NormBandEdges bandEdgesOf(const Norm &n, const GradePolicy &policy = {},
                                 double marginOverride = -1.0)
{
    NormBandEdges e;
    // Computed the same way withinBand() computes it, so the drawn edge and the graded edge agree
    // by construction at every magnitude — the float-edge doctrine, applied to the band it had
    // never been applied to.
    e.idealLo = n.mu - policy.idealMaxZ * n.sigmaLo;
    e.idealHi = n.mu + policy.idealMaxZ * n.sigmaHi;
    if (marginOverride >= 0.0) {
        e.watchLo = e.idealLo - marginOverride;
        e.watchHi = e.idealHi + marginOverride;
    } else if (n.hasExplicitMonitor()) {
        e.watchLo = *n.monitorLo;
        e.watchHi = *n.monitorHi;
    } else {
        e.watchLo = n.mu - policy.watchMaxZ * n.sigmaLo;
        e.watchHi = n.mu + policy.watchMaxZ * n.sigmaHi;
    }
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

// The user-facing words for a norm's provenance. WITH the enum, not in a façade: the measures view
// and the metric detail page both render it, and two copies of four strings is how they drift.
QString    normSourceLabel(NormSource s);
QString    gradeName(Grade g);
bool       gradeFromName(const QString &s, Grade &out);

// The user-facing words. Ideal / Good / Watch / Action, plus "Not measured" — an ordinary,
// monotone ramp where the top band says what to do rather than describing a boundary.
QString    gradeLabel(Grade g);

} // namespace pinpoint::analysis
