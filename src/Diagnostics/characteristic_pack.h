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
//
// WARNINGS — the pack works, but the health list should show it:
//   observableNoSignal   an Observable condition nothing can detect
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
ValidationReport validatePack(const CharacteristicPack &pack);

// ── Persistence ─────────────────────────────────────────────────────────────
struct PackLoadResult {
    CharacteristicPack pack;
    ValidationReport   report;
    bool               loaded = false;   // false => `pack` is meaningless
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

} // namespace pinpoint::analysis
