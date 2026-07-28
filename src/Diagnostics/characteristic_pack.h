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

#include "characteristic.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

// Pack persistence and validation.
//
// The validator is the pack's only real defence. A characteristic library degrades in two ways —
// duplicate measures proliferating, and the causal graph quietly becoming wrong — and neither is
// visible by reading the JSON. Every rule below exists because of a specific way this content can
// be wrong while still looking right.

namespace pinpoint::analysis {

// The schema version this build writes. A pack declaring a HIGHER version is refused rather than
// partially read: silently dropping fields it does not understand would let a newer pack round-trip
// through an older build and lose content.
inline constexpr int kPackSchemaVersion = 1;

enum class IssueSeverity {
    Error,     // the pack is not usable as-is
    Warning,   // the pack loads, but something is wrong or incomplete — this is the health list
};

struct ValidationIssue {
    IssueSeverity severity = IssueSeverity::Error;
    QString       code;      // machine-readable, e.g. "unknownMeasure", "cycle", "singleTailAxis"
    QString       subject;   // the id at fault
    QString       message;   // human-readable
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;

    bool                         ok() const;            // no errors (warnings are fine)
    int                          errorCount() const;
    int                          warningCount() const;
    std::vector<ValidationIssue> withCode(const QString &code) const;
    std::vector<ValidationIssue> withSeverity(IssueSeverity s) const;
    QStringList                  messages(IssueSeverity s) const;
};

// ── Validation ──────────────────────────────────────────────────────────────
//
// ERRORS — the pack is structurally broken:
//   duplicateId          two entities share an id
//   unknownMeasure       a signal references a measure that does not exist
//   unknownSignal        a condition's detectedBy names a signal that does not exist
//   unknownCondition     an edge endpoint does not exist
//   cycle                the condition graph is not a DAG
//   selfEdge             an edge from a condition to itself
//   signalArity          wrong measure count for the test (2 for order/ratio, 1 otherwise)
//   signalDirection      a corridor/threshold signal with no direction — it cannot identify a tail
//   signalThreshold      a threshold on a non-threshold test, or a threshold test with no number
//   corroboratesCausal   Corroborates between conditions that already have a causal path. The pair
//                        would double-count in the confidence ranking; one relationship or the other
//   axisMismatch         two tails share an axis id but not a series — then they are not tails
//   badFacets            a measure's series fails the validity table
//   badReducer           a measure's reducer is malformed
//   unknownShape         a measure declares a shape that is not target/floor/ceiling. An error
//                        rather than a fall back to Target: "flooor" would grade the good tail
//   unknownDirection     a measure's unwatchedTail is not high or low — same reasoning
//   unwatchedTailShaped  a one-sided measure claiming a tail is "deliberately unwatched". The shape
//                        already says which tail does not grade; the two cannot both be true
//   unwatchedTailWatched a measure claiming a tail is unwatched that a corridor signal watches
//
// WARNINGS — the pack works, but the health list should show it:
//   observableNoSignal   an Observable and Measured condition nothing can detect. Scoped to
//                        Measured: Screened/Asserted conditions are signal-less by design
//   noCause              a condition with no cause: it can be reported but never explained
//   noResolvableCause    every cause is Asserted, so the resolver can offer but never conclude
//   orphanCause          a Latent condition that explains nothing — dead weight
//   unusedMeasure        a measure no signal uses
//   proposedTier         no citation; the UI must badge it
//   needsRevalidation    flagged for review
//   singleTailAxis       an axis with only one authored tail — deliberate, or an oversight?
//   inconsistentReach    a Screened/Asserted condition that also claims to be detected by a signal
//   screenedHasCause     a Screened cause with an incoming edge — the likeliest symptom of a
//                        wholly INVERTED graph, which no coverage count can detect
//   bothTailsOneCondition  one condition flagging BOTH sides of one measure's corridor. It fires
//                        whichever way the reading goes, so it cannot tell too much from too little
//   duplicateAlias       two conditions answer to one coach term. Search resolves to whichever came
//                        first in the file, so the term silently leads to the wrong page
//   externalDeviceNoReason  an ExternalDevice measure that does not name the device. The status says
//                        something is in the way; only the reason says what, and two surfaces quote it
//   unwatchedTailNoReason  a measure declaring a tail deliberately unwatched without saying why.
//                        The declaration silences `ungradedTail`; without the reason it is
//                        indistinguishable from a tail nobody has got to
//
// The checks that span the pack, the norm set, the context tree and the metric catalogue at once —
// "can this signal ever fire?" — live in `diagnostics_health.h`, because no single pack can answer
// them.
ValidationReport validatePack(const CharacteristicPack &pack);

// ── Persistence ─────────────────────────────────────────────────────────────
struct PackLoadResult {
    CharacteristicPack pack;
    ValidationReport   report;
    bool               loaded = false;   // parsed AND validated clean
    // Parsed successfully, whatever validation said. An overlay pack (user or community) routinely
    // fails standalone referential integrity — its edges point at CORE conditions it does not
    // itself contain — so referential checks are only meaningful on the ASSEMBLED library. A caller
    // merging packs must key off `parsed`; keying off `loaded` silently discards every overlay.
    bool               parsed = false;
};

// Parse + validate. Parse failures land in `report` as errors rather than being thrown or logged,
// so a bad community pack surfaces in the UI instead of vanishing.
PackLoadResult loadPack(const QJsonObject &root, const QString &sourceLabel = QString());
PackLoadResult loadPack(const QByteArray &json, const QString &sourceLabel = QString());

QJsonObject savePack(const CharacteristicPack &pack);

// ── Graph helpers (shared by the validator, the resolver and the UI) ────────
// `Causes` edges only. Direction follows Edge's contract: `from` causes `to`.
QStringList causesOf(const CharacteristicPack &pack, const QString &conditionId);   // upstream
QStringList effectsOf(const CharacteristicPack &pack, const QString &conditionId);  // downstream

// How many conditions this one explains, directly. The concentration of a handful of latent causes
// over many characteristics is the model's whole point, so this is a first-class query.
int  coverageOf(const CharacteristicPack &pack, const QString &conditionId);

// True when a `Corroborates` edge would be illegal between these two: a causal path already exists
// in either direction.
bool hasCausalPath(const CharacteristicPack &pack, const QString &fromId, const QString &toId);

// Both tails of an axis, in pack order. Empty for a condition with no axis.
QStringList tailsOfAxis(const CharacteristicPack &pack, const QString &axis);

// ── The metric → measure join ───────────────────────────────────────────────
//
// Which measure reads `metricKey` at `phase`. This is the bridge between the two registries: the
// metric catalogue speaks in (key, phase) and the diagnostics pack speaks in measures, and every
// corridor the app renders has to cross that gap. `metric_corridor.h` carries the answer on to the
// norm that grades it; both live in the pack layer rather than in a QML façade, because every rule
// written inside a façade is a rule nothing can test.
//
// A measure matches a phase through its REDUCER, not through a phase field, because that is where
// the phase actually lives:
//   at        -> the anchor phase
//   delta     -> the window's END phase; the measure IS the change observed at that phase
//   rate      -> likewise the window's end
//   extremum  -> never matches. "The lowest lag angle between P5 and P6" is not a reading at P5 or
//                at P6, and returning it for either would attach a corridor to the wrong number.
//
// Ambiguity is possible and is resolved by preferring `at` over `delta`: both m_leadWristAtTop
// (at P4) and a Δ-from-address measure name P4 on the same metric, and the absolute reading is the
// one a corridor keyed on a phase means. Beyond that PACK ORDER wins — deterministic, but arbitrary,
// and a tie there means two measures describe one number. The validator warns about exactly that
// case (`duplicateMeasure`), because no downstream check can: both answers look correct.

// EVERY measure that reads `metricKey` at `phase`, in that preference order.
//
// Callers that must produce something need the whole list, not the winner. `m_leadWristAtTop` is the
// case that proves it: it is the preferred measure at P4 and carries NO norm, so a caller taking only
// the winner draws no corridor at the top of the swing while the Δ-from-address measure beside it has
// carried one since v1. Empty when nothing matches.
std::vector<const Measure *> measuresForMetricAtPhase(const CharacteristicPack &pack,
                                                      const QString            &metricKey,
                                                      Phase                     phase);

// The preferred one — the first of the above. Null when nothing matches.
const Measure *measureForMetricAtPhase(const CharacteristicPack &pack,
                                       const QString            &metricKey,
                                       Phase                     phase);

// True when this measure is a change from address — the flag a corridor carries so a consumer knows
// whether to plot it against the raw curve or against Δ-from-address.
bool measureIsDeltaFromAddress(const Measure &m);

} // namespace pinpoint::analysis
