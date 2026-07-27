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

#include <QString>

#include <vector>

// The bibliography behind every citation in the pack.
//
// `Provenance::citation` holds a DOI and nothing else, which is the right thing for the pack to
// store — one identifier, no duplicated metadata, nine edges citing one paper without nine copies
// of its title. But a DOI on its own is unreadable: a coach looking at "why does the app think
// this?" gets `10.1088/0034-4885/66/2/202`, which answers nothing. This registry is the join that
// turns it back into a paper somebody can read.
//
// It also carries `establishes`, which the DOI genuinely cannot: what the paper actually shows, in
// our own words, including where it DISAGREES with another reference. That sentence is the whole
// reason a reader can tell an `indirect` citation from a `supported` one without opening the PDF.
//
// PROVENANCE OF THE PROVENANCE. Every record was resolved against CrossRef and its title, journal,
// authors and year read off that record. Nothing here is recalled from memory, and nothing is
// copied out of another paper's reference list without resolving it first — a plausible-looking
// DOI is worse than no citation at all, because the null is honest and the wrong DOI is a lie that
// survives review. `references_test` re-asserts the shape; only a human re-resolving them can
// re-assert the content.
//
// Deliberately NOT behind an abstract provider, for the same reason as screens and drills: that
// polymorphism exists so a COMMUNITY pack can be namespaced against core, there is no community
// story for a bibliography, and three provider classes for a flat list would be ceremony standing
// in for a requirement. A user layer merges by id, and that is the whole story.

namespace pinpoint::analysis {

struct Reference {
    QString id;           // `ref.*`
    QString doi;          // the join key with Provenance::citation — an exact string match
    QString title;
    QString authors;      // family names in citation order, comma separated
    QString journal;
    int     year = 0;
    QString establishes;  // what it actually shows, and what it does not

    // https://doi.org/<doi> — the one thing a reader wants when they tap the row.
    QString url() const;
};

struct ReferenceSet {
    QString                id;
    QString                version;
    int                    schemaVersion = 1;
    QString                sourceLabel;
    bool                   readOnly = false;
    std::vector<Reference> references;

    const Reference *reference(const QString &id) const;
    const Reference *byDoi(const QString &doi) const;    // the join the UI actually makes
};

// ── Validation ──────────────────────────────────────────────────────────────
//
// ERRORS:
//   duplicateId         two references share an id
//   duplicateDoi        two references share a DOI — the join is by DOI, so the second is
//                       unreachable and the pack would silently resolve to whichever came first
//   referenceIdNamespace  an id outside the `ref.` namespace
//   referenceNoDoi      a reference with no DOI cannot be joined to anything or opened by anyone
// WARNINGS:
//   referenceNoTitle    a row that renders as a bare identifier
//   referenceNoYear     undated, so a reader cannot judge how current it is
ValidationReport validateReferenceSet(const ReferenceSet &set);

struct ReferenceLoadResult {
    ReferenceSet     set;
    ValidationReport report;
    bool             loaded = false;
    bool             parsed = false;
};

ReferenceLoadResult loadReferenceSet(const QByteArray &json, const QString &sourceLabel);
QByteArray          saveReferenceSet(const ReferenceSet &set);

const ReferenceSet &sharedReferenceSet();
void                resetSharedReferenceSet();

} // namespace pinpoint::analysis
