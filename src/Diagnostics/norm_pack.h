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

#include "characteristic_pack.h"   // ValidationReport, ValidationIssue
#include "context_tree.h"
#include "norm.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <vector>

// Norm pack persistence and validation.
//
// A norm set is content, exactly like a characteristic pack, and layers the same way: a shipped
// core set, plus whatever the user has authored or imported, with the user's own edits overriding
// core by (measureId, contextId).

namespace pinpoint::analysis {

// The schema version this build writes. A pack declaring a HIGHER version is refused rather than
// partially read — same contract as kPackSchemaVersion.
inline constexpr int kNormPackSchemaVersion = 1;

struct NormPack {
    QString id;                 // "core", or a community pack's namespace
    QString version;
    int     schemaVersion = kNormPackSchemaVersion;
    QString sourceLabel;        // where it was loaded from, for the UI
    bool    readOnly = false;   // the shipped core set is not editable in place

    std::vector<Norm> norms;

    // Exact match only. Resolution up the context tree is the provider's job, because it needs the
    // tree; this is the primitive it is built from.
    const Norm *find(const QString &measureId, const QString &contextId) const;
    bool        contains(const QString &measureId, const QString &contextId) const;

    // Every context this measure has its own row for, in pack order.
    QStringList contextsFor(const QString &measureId) const;

    // Insert or replace by (measureId, contextId), preserving pack order on replace so a norm set
    // does not reshuffle when one row is edited.
    void        upsert(const Norm &norm);
    bool        remove(const QString &measureId, const QString &contextId);
};

// ── Validation ──────────────────────────────────────────────────────────────
//
// Split the same way the characteristic pack splits it, and for the same reason: an overlay set
// legitimately references measures and contexts it does not itself contain, so referential checks
// are only meaningful on the ASSEMBLED library.
//
// STANDALONE (validateNormPack) — ERRORS:
//   duplicateNorm        two rows share (measureId, contextId)
//   emptyNormKey         a row with no measureId or no contextId
//   negativeSigma        a tolerance below zero
//   monitorOrder         monitorLo > monitorHi
//   monitorExcludesIdeal the explicit monitor band does not contain the Ideal band — the value
//                        would grade Action while sitting inside its own tolerance
//   plausibleOrder       plausibleLo > plausibleHi
// STANDALONE — WARNINGS:
//   zeroSigma            a norm with no tolerance on a side; only its exact centre grades Ideal
//   noProvenance         Literature source with no citation
//
// REFERENTIAL (validateNormsAgainst) — ERRORS:
//   unknownNormMeasure   a norm keys on a measure the library does not contain
//   unknownNormContext   a norm keys on a context the tree does not contain
//   normUnitMismatch     the norm's unit is not the measure's unit. NAMED IN FULL, both sides, and
//                        an error rather than a coercion: silently grading degrees against a
//                        percentage produces a confident, plausible, wrong answer.
//   normNotCapturable    a norm on a NotCapturable measure — no sensor can ever produce a value for
//                        it, so a corridor on it can never do anything but mislead
//   normShapeTolerance   a one-sided measure's norm states different tolerances either side. One of
//                        them describes a tail that does not grade
//   normShapeMonitor     a monitor bound on the OPEN side of a one-sided measure — an edge nothing
//                        grades against, sitting in the pack looking authoritative
//   partialMonitor       one of monitorLo/monitorHi without the other, on a measure that grades
//                        BOTH tails. MOVED here from the standalone validator: half a monitor band
//                        is a complete one on a one-sided measure, and only this layer knows which
//   plausibleInsideCorridor  a plausible bound sits inside the Watch edge, so a reading would be
//                        graded Action and disbelieved at once. Measured against the WIDEST shipped
//                        preset, so the answer cannot depend on the reader's grade-policy setting
//
// Shape checks live HERE and not in the standalone validator for the same reason normUnitMismatch
// does: shape is a property of the MEASURE (see characteristic.h), a norm row carries only numbers,
// and only the assembled library can join the two.
ValidationReport validateNormPack(const NormPack &pack);

ValidationReport validateNormsAgainst(const NormPack               &norms,
                                      const CharacteristicPack     &pack,
                                      const ContextTree            &contexts);

// ── Persistence ─────────────────────────────────────────────────────────────
struct NormPackLoadResult {
    NormPack         pack;
    ValidationReport report;
    bool             loaded = false;   // parsed AND validated clean
    bool             parsed = false;   // parsed, whatever validation said — see the note above
};

NormPackLoadResult loadNormPack(const QJsonObject &root, const QString &sourceLabel = QString());
NormPackLoadResult loadNormPack(const QByteArray &json, const QString &sourceLabel = QString());

QJsonObject saveNormPack(const NormPack &pack);

} // namespace pinpoint::analysis
