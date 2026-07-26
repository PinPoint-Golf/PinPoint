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

    bool hasExplicitMonitor() const { return monitorLo.has_value() && monitorHi.has_value(); }

    // The Ideal band's edges, in the measure's own units. This is what the corridor editor's two
    // handles bind to, and what projects into a reference Band for the wrist grid.
    double idealLo() const { return mu - sigmaLo; }
    double idealHi() const { return mu + sigmaHi; }
};

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
    const double az = std::fabs(normZ(value, norm));

    if (norm.hasExplicitMonitor()) {
        if (value < *norm.monitorLo || value > *norm.monitorHi)
            return Grade::Action;
        if (az <= policy.idealMaxZ) return Grade::Ideal;
        if (az <= policy.goodMaxZ)  return Grade::Good;
        return Grade::Watch;                      // capped: inside the monitor band is never Action
    }

    if (az <= policy.idealMaxZ) return Grade::Ideal;
    if (az <= policy.goodMaxZ)  return Grade::Good;
    if (az <= policy.watchMaxZ) return Grade::Watch;
    return Grade::Action;
}

// True when the grade says something deviated. NotMeasured is NOT a deviation — see the enum.
inline bool isDeviation(Grade g) { return g == Grade::Watch || g == Grade::Action; }

// The 4-band grade collapsed to the legacy 3-band PpRag the wrist grid renders.
//
// ONE definition, here, because two surfaces consume it and a second copy would let them drift:
// NormBandProvider projects a Norm into a Band and the wrist grid runs classifyDelta() over it,
// while the characteristic engine grades the same value through grade(). Those two paths must
// agree on colour for the same number, and reference_bands_parity_test asserts they do — over the
// migrated content AND over a norm with no explicit monitor, where the Action edge is z-derived.
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
QString    gradeName(Grade g);
bool       gradeFromName(const QString &s, Grade &out);

// The user-facing words. Ideal / Good / Watch / Action, plus "Not measured" — an ordinary,
// monotone ramp where the top band says what to do rather than describing a boundary.
QString    gradeLabel(Grade g);

} // namespace pinpoint::analysis
