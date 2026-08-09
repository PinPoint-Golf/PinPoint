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

#include "characteristic_pack.h"     // CharacteristicPack, ValidationIssue
#include "drill_pack.h"              // DrillSet, for the unknownDrillRef / drillNo* checks
#include "norm_provider.h"
#include "reference_pack.h"          // ReferenceSet, for referenceHealth()
#include "screen_pack.h"             // ScreenSet, for the unknownScreenRef / screenNo* checks
#include "../Metrics/metric_catalogue.h"

#include <optional>
#include <vector>

// The ASSEMBLED-LIBRARY health checks — the ones no single validator can make.
//
// `validatePack()` sees a characteristic pack. `validateNormPack()` sees a norm set. Each is
// complete for what it can see, and neither can answer the question an author actually asks — *can
// this signal ever fire?* — because that spans the pack, the norm set, the context tree and the
// metric catalogue at once. Those checks live here, as a free function over the registries, so the
// health list is computed somewhere it can be tested rather than inside a QML façade.
//
// EVERYTHING HERE IS A WARNING. Nothing in this file is a load error: a library with a signal that
// cannot fire is not broken, it is incomplete, and refusing to load it would take the app down over
// a missing corridor. The health list is the place incompleteness is stated out loud.
//
// Codes, and what each one means an author should DO:
//
//   signalNoNorm           A corridor signal on a live measure with no norm in any context. It
//                          cannot fire — not "does not fire on this swing", cannot, ever. Author a
//                          norm or accept that the characteristic is undetectable.
//   signalOnOpenTail       A corridor or threshold signal watching the tail a one-sided measure does not
//                          grade — a High signal on a floor, or a Low one on a ceiling. It can never
//                          fire, and unlike signalNoNorm no producer will ever change that: the
//                          measure has no fault on that side. Point it at the other tail, or accept
//                          that the condition is undetectable by design. NOT scoped to Live: this is
//                          an author's misreading of the measure, not a backlog item, and it should
//                          be visible the moment it is written rather than when a producer lands.
//   ungradedTail           The exact mirror of signalOnOpenTail: a tail that DOES grade with no
//                          signal watching it. A Target measure with a norm grades both sides —
//                          sigmaHi defaults to sigmaLo — so a corridor authored with one condition
//                          on it still puts a Watch or an Action on the metric surfaces for the other
//                          side, with no fault name, no consequence and no drill behind it. Three
//                          correct answers, and picking the wrong one is the whole risk: author the
//                          condition when the tail carries a real fault; set `shape` when the
//                          quantity is one-sided and that tail should stop grading; set
//                          `unwatchedTail` + `unwatchedReason` when the tail genuinely grades and
//                          there is still nothing to name. Do NOT reach for the first by default —
//                          a shape is a deletion of false grading rather than an addition to the
//                          model. Not scoped to Live, for signalOnOpenTail's reason: a producer
//                          landing would make this WORSE, not better.
//   personalNormNoSample   A corridor of YOUR OWN, seated on nothing (n = 0). Fine as a starting
//                          figure; worth knowing you typed it rather than measured it. Scoped to
//                          the personal layer through the provider's own override tracking, so the
//                          39 migrated shipped rows never appear here — unscoped, this check opens
//                          with 39 items of noise about content that was fine yesterday.
//   cohortGap              A (measure, context) carrying a sex-only corridor and an age-only one,
//                          with no corridor for the combination. Age is probed ahead of sex, so a
//                          golfer who is both resolves the age row and the sex row never applies to
//                          them — defensible as a default, almost certainly not what was meant.
//                          Author the combined rows, or drop one axis. A NUDGE: the set still loads
//                          and still grades.
//   shadowedCohort         An `adult` corridor at a node where all three of its sub-bands are
//                          authored. The exact band is always tried first, so the parent can never
//                          resolve for anyone — a corridor sitting in the pack looking authoritative
//                          and grading nobody, which is the same mistake as a monitor bound on a
//                          tail that does not grade.
//   measureUnitMismatch    A Provided measure's unit is not the unit the METRIC CATALOGUE states for
//                          the key it reads. The producer emits the catalogue's unit; grading uses
//                          the measure's corridor; nothing in between converts or even compares, so
//                          the reading is graded against a band in a different scale and the result
//                          is a confident wrong answer rather than a missing one. This is NOT the
//                          same check as `normUnitMismatch`, which compares the norm against the
//                          measure — those two agreed with each other on all three of the live
//                          measures this was written for. It shipped: `leadHeelLift` emitted
//                          ×frame-width against a 2 cm ceiling, so `sig_excessiveHeelLift` could not
//                          fire on any swing ever recorded, and `headSway` emitted millimetres
//                          against a 4 cm corridor, so it read Action on all of them. Two silent
//                          failures in opposite directions from one root. Fix the producer or the
//                          measure — but the measure and its corridor move together, so changing the
//                          measure means re-seating the corridor.
//                          A `Rate` reducer is EXEMPT: it legitimately divides by time, so mph/s over
//                          a mph metric is the reducer doing its job and not a mistake.
//   clubDependentNoContext The metric's own `howToRead` says the number is club-dependent, and every
//                          norm for it sits at full swing or above. One corridor is grading a driver
//                          and a wedge against the same band, which the descriptor already says is
//                          wrong. Add per-club rows.
//   emptyContext           A context with no norms of its own. Harmless — it grades as its parent
//                          does — but it is a control with no effect until something is authored
//                          under it.
//   ungradedContext        Worse, and a different statement: nothing resolves anywhere up its chain,
//                          so a shot in that context is graded by NOTHING. Not a wider corridor,
//                          none. This is what the shipped tree looks like today for partial, pitch,
//                          chip, bunker and specialty — five branches hanging off `any`, where every
//                          authored norm sits at `full_swing` or below.
//   unknownScreenRef       A condition names a `screen.*` id the screen registry does not have. The
//                          join is an exact string match, so this fails SILENTLY: the condition
//                          loads and renders, and the panel that should tell a coach how to run the
//                          test is simply blank. Fix the id, or author the screen.
//   unknownDrillRef        The same, for a `drill.*` id.
//   screenNoProtocol       A screen nobody could run — no protocol authored.
//   screenNoPass           A screen with no pass criterion, so its answer is unrecordable.
//   drillNoInstruction     A drill that does not say what the golfer does.
//   drillNoTarget          A drill that does not say what it is trying to change, so nobody can
//                          judge whether it is the right one.
//   referenceOrphan        A reference cited by nothing that does NOT claim to be general reading.
//                          The registry holds two kinds of record — the sources behind a citation,
//                          and the field's standard reading — and `generalReading` is how the second
//                          kind says so. A record that is neither is one nobody has explained: it
//                          may be a citation that was removed, an id that was retyped, or a paper
//                          somebody meant to come back to. A WARNING and not an error, because a
//                          record legitimately sits uncited while the conditions that will cite it
//                          are authored; what it must not do is sit there silently forever. The fix
//                          is to cite it, flag it, or delete it — and choosing is the point.
//   overrideCoreChanged    Your override was made against shipped numbers that have since been
//                          revised. Offers a diff and "Take theirs" (which is the existing
//                          drop-your-row operation — one operation, honest label).
//   oneBandCorpus          A corridor grading an implausible share of the drawn library into a
//                          single band. NOT produced here: it needs a library scan and arrives
//                          asynchronously. The code is declared here so the health list has one
//                          vocabulary.
//
// THERE IS NO LONGER A VIEW LABEL TABLE, and this comment used to say the codes were declared here
// so that one had a home. `HealthView.qml` and its `_codeLabel` map went with the old diagnostics
// panel; the health list is now marshalled raw by `ModelBrowser::rawRows(kHealth)`, which renders
// the `code` string itself and chips on it. So a new code needs no QML change and appears in the
// panel the moment something emits it — and the code string IS what a reader sees, which is a
// reason to name one carefully rather than a reason to stop caring.
//
// Deliberately NOT a check: the unread edge of a single-tail axis. `s_posture` reads one end of
// `lumbar_curve` and nothing reads the other, but the norm is a single two-sided row — the "missing"
// tail is a condition nobody has authored, not a corridor anybody is missing. Reporting it would
// make the health list argue for content the model does not need.

namespace pinpoint::analysis {

// The share of a drawn sample that has to land in one grade band before `oneBandCorpus` is worth
// reporting. Not a statistical threshold: it is the point at which the histogram would be visibly
// wrong to someone who has never heard of a standard deviation, which is the argument the corridor
// editor's histogram was built on.
inline constexpr double kOneBandShare  = 0.9;
// Below this many produced readings the share means nothing — three swings in one band is a Tuesday.
inline constexpr int    kMinCorpusForShare = 8;

// `oneBandCorpus` needs a library scan, so it is produced by the caller that can afford one and
// passed in as counts. One row per (measure, context) that resolved a norm and produced values.
struct CorpusGradeCounts {
    QString measureId;
    QString contextId;
    int     ideal  = 0;
    int     good   = 0;
    int     watch  = 0;
    int     action = 0;

    int total() const { return ideal + good + watch + action; }
};

// The decision `oneBandCorpus` is built from, factored out of corpusShareHealth() so a second caller
// with its own single corridor's counts — corridor_plot.cpp's per-plot note — can ask the same
// question without re-deriving it in a second hand-rolled loop. That happened once already: the plot
// note used to test the same kOneBandShare / kMinCorpusForShare thresholds inline, in its own loop,
// with its own wording and without the Ideal-vs-mislocated distinction below — two implementations of
// one check, free to drift and silently missing half the argument in one of them.
//
// Empty when there are too few readings to mean anything, or no band crosses the share.
struct OneBandShare {
    QString word;    // the band's own name: "Ideal", "Good", "Watch" or "Action"
    int     n     = 0;
    int     total = 0;

    // Which way it is wrong, in one clause — and the two are opposite fixes, not degrees of the same
    // one. Almost everything OUTSIDE Ideal means the corridor is centred wrong or scaled wrong;
    // almost everything Ideal means it is so wide it can never say anything. A hint that did not
    // distinguish them would send an author to check the wrong half of the corridor.
    QString hint;
};
std::optional<OneBandShare> oneBandShareOf(const CorpusGradeCounts &counts);

// The registry-only checks: everything decidable from the assembled library alone. Includes the
// REFERENTIAL norm validation (`validateNormsAgainst` — unknown measure/context, unit mismatch, a
// norm on a measure no sensor can produce), which existed from stage 1 and which nothing in the app
// had ever called.
//
// `screens` and `drills` are the registries the unknownScreenRef / unknownDrillRef / screenNo* /
// drillNo* checks join against — taken as arguments for the reason dag_layout.h states as this
// area's doctrine: a module takes what it needs as arguments, and a global read here would make the
// picture depend on process state no test could set. See referenceHealth() below for the same seam
// applied to the bibliography; this is that argument applied one registry further. Callers reading
// the shipped library pass sharedScreenSet() / sharedDrillSet() and see no change.
std::vector<ValidationIssue> diagnosticsHealth(const CharacteristicPack &pack,
                                               const INormProvider      &norms,
                                               const MetricCatalogue    &catalogue,
                                               const ScreenSet          &screens,
                                               const DrillSet           &drills);

// The corpus-share check, over counts a caller has already gathered.
std::vector<ValidationIssue> corpusShareHealth(const std::vector<CorpusGradeCounts> &counts);

// `referenceOrphan`, over the pack and the bibliography together — neither can answer it alone, and
// `validateReferenceSet()` takes a ReferenceSet and so cannot see who cites what.
//
// Split out rather than folded into diagnosticsHealth() for the same reason corpusShareHealth() is:
// it is the seam that makes the check testable. Taking the set as a parameter lets both directions be
// asserted over fixtures — and a check whose negative case is untested passes everything.
// diagnosticsHealth() calls this with sharedReferenceSet(), so callers see no change.
std::vector<ValidationIssue> referenceHealth(const CharacteristicPack &pack,
                                             const ReferenceSet       &refs);

} // namespace pinpoint::analysis
