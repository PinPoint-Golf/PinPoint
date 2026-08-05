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
#include <QVariantMap>

#include <vector>

// Norm pack persistence and validation.
//
// A norm set is content, exactly like a characteristic pack, and layers the same way: a shipped
// core set, plus whatever the user has authored or imported, with the user's own edits overriding
// core by (measureId, contextId).

namespace pinpoint::analysis {

// The schema version this build UNDERSTANDS. A pack declaring a HIGHER version is refused rather
// than partially read — same contract as kPackSchemaVersion.
//
// 2 adds the optional `cohort` on a norm row. The bump is what stops an older build from silently
// dropping the key and grading everyone against a row that describes women over 65 — precisely the
// failure `schemaTooNew` exists to prevent.
inline constexpr int kNormPackSchemaVersion = 2;

struct NormPack {
    QString id;                 // "core", or a community pack's namespace
    QString version;
    int     schemaVersion = kNormPackSchemaVersion;
    QString sourceLabel;        // where it was loaded from, for the UI
    bool    readOnly = false;   // the shipped core set is not editable in place

    std::vector<Norm> norms;

    // Exact match on the FULL key, cohort included. Resolution up the context tree — and through the
    // cohort probe order — is the provider's job, because it needs the tree and the athlete; this is
    // the primitive both are built from.
    //
    // The cohort defaults to unqualified, which is what every caller predating cohorts meant and
    // what every shipped row is. It is a real term of the key and not a filter: a qualified row and
    // an unqualified one at the same (measure, context) are two different rows, and an upsert of
    // one must never replace the other.
    const Norm *find(const QString &measureId, const QString &contextId,
                     const Cohort &cohort = {}) const;
    bool        contains(const QString &measureId, const QString &contextId,
                         const Cohort &cohort = {}) const;

    // Every context this measure has a row for, in pack order, ONCE EACH. Cohorts make one context
    // legitimately carry several rows, and this answers "which contexts", not "how many rows".
    QStringList contextsFor(const QString &measureId) const;

    // The QUALIFIED cohorts with a row at this exact (measure, context), in pack order. The
    // unqualified row is deliberately excluded: it is the one every existing surface already shows,
    // and this exists so a surface can list the segmented rows BESIDE it. Without it a cohort row
    // would be authored into a file and be invisible in every view — reachable by resolution and by
    // nothing a user can click.
    std::vector<Cohort> cohortsFor(const QString &measureId, const QString &contextId) const;

    // Insert or replace by (measureId, contextId, cohort), preserving pack order on replace so a
    // norm set does not reshuffle when one row is edited.
    void        upsert(const Norm &norm);
    bool        remove(const QString &measureId, const QString &contextId,
                       const Cohort &cohort = {});
};

// ── The norm key, as one string ─────────────────────────────────────────────
//
// ONE spelling, here, because four places wrote it and two of them disagreed: `norm_pack` used
// "%1 @ %2" and `diagnostics_health` used measureId + '@' + contextId with no spaces, while
// `CharacteristicLibraryModel` split the result on the first '@' to build the health view's
// deep-link — so half the deep-links carried a leading space into the context id. A subject string
// that is both rendered to a user and parsed back by a view has to be produced and consumed by one
// pair of functions.
//
// The cohort is appended only when the row carries one, so every unqualified row — which is all of
// them today — reads exactly as it always did.
QString normKeyLabel(const QString &measureId, const QString &contextId, const Cohort &cohort = {});
QString normKeyLabel(const Norm &n);

// The inverse, for the deep-link. Recovers the measure and the context; the cohort term is not
// parsed back, because what the link opens is a corridor at a (measure, context) and the editor
// resolves the rest.
void     splitNormKey(const QString &key, QString &measureId, QString &contextId);

// ── A cohort across the C++/QML boundary ────────────────────────────────────
//
// Spelled exactly as the JSON spells it — `{ "sex": "female", "age": "adult_55_64" }`, each key
// present only when that axis is set — so a cohort has ONE spelling in the file, in C++ and in QML.
// A second shape for the boundary would be a second thing to keep in step with the vocabulary.
//
// `cohortFromMap` REFUSES an unreadable token rather than dropping that axis, and its caller must
// refuse with it. Dropping an axis silently would broaden the key — a corridor for one segment
// quietly becoming the corridor for everyone — which is the same failure `unknownCohort` drops a
// row to prevent.
bool        cohortFromMap(const QVariantMap &map, Cohort &out);
QVariantMap cohortToMap(const Cohort &cohort);

// ── Validation ──────────────────────────────────────────────────────────────
//
// Split the same way the characteristic pack splits it, and for the same reason: an overlay set
// legitimately references measures and contexts it does not itself contain, so referential checks
// are only meaningful on the ASSEMBLED library.
//
// PARSE (loadNormPack) — ERRORS:
//   parse                the bytes are not JSON, or not a JSON object. The shared spelling — this
//                        was `badNormFile`, which meant the same thing in a word only this file
//                        used, so anything listing "the files that would not read" had to know the
//                        norm set was special in order to include it
//   schemaTooNew         the set declares a version above kNormPackSchemaVersion, so it is refused
//                        rather than partially read
//   unknownCohort        a `cohort` object naming a sex or an age band this build does not know.
//                        The ROW IS DROPPED, which is where this differs from `unknownShape`:
//                        falling back to the default there means Target, a corridor that grades
//                        both tails — conservative. Falling back here would mean UNQUALIFIED, a
//                        corridor that applies to everybody, and applying a segment's numbers to
//                        the whole population is the one outcome worse than having no row
//
// STANDALONE (validateNormPack) — ERRORS:
//   duplicateNorm        two rows share (measureId, contextId, cohort). The cohort is part of the
//                        key, so a male row and a female row at one context are two rows, not a
//                        duplicate
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
// does: shape is a property of the MEASURE (see measure_vocabulary.h), a norm row carries only numbers,
// and only the assembled library can join the two.
ValidationReport validateNormPack(const NormPack &pack);

ValidationReport validateNormsAgainst(const NormPack               &norms,
                                      const CharacteristicPack     &pack,
                                      const ContextTree            &contexts);

// ── Persistence ─────────────────────────────────────────────────────────────
// The four fields and the `parsed` / `loaded` distinction are LoadResult<T>'s — see pack_io.h.
using NormPackLoadResult = LoadResult<NormPack>;

NormPackLoadResult loadNormPack(const QJsonObject &root, const QString &sourceLabel = QString());
NormPackLoadResult loadNormPack(const QByteArray &json, const QString &sourceLabel = QString());

// The version a reader must understand to read this pack FAITHFULLY — 2 when any row carries a
// cohort, 1 otherwise. `saveNormPack` writes this rather than `pack.schemaVersion`.
//
// Content-driven on purpose, and it is the same argument as omitting `"shape"` when it is Target. A
// flat bump would make every user set unreadable by an older build over a feature none of them used;
// stamping the loaded version back would let a set that GAINED a cohort row keep declaring 1, which
// is exactly the silent-drop this version exists to prevent. Writing what the content needs is the
// only one of the three that is true in both directions.
int         requiredNormSchemaVersion(const NormPack &pack);

QJsonObject saveNormPack(const NormPack &pack);

} // namespace pinpoint::analysis
