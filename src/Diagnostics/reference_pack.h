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
// THREE IDENTIFIERS, ONE JOIN. Most records carry a DOI. Some journals issue none at all — not as
// an oversight, as a policy — and for those the PubMed id is the identifier the world actually
// uses. And most golf coaching doctrine was never published in a journal at all: it lives in books
// and in chapters of edited volumes, whose identifier is an ISBN. Refusing any of the three would
// mean the pack could cite a source no reader could ever be shown, which is the exact failure this
// registry exists to prevent. So `citation` joins against ANY of the three fields, and a record
// needs one of them rather than the DOI specifically. None is preferred where several exist; a DOI
// simply resolves most directly, so it wins the URL.
//
// A BOOK IS NOT PEER REVIEW, and the ISBN is where that distinction has to be kept. `Supported`
// means a peer-reviewed source tested this cause and this effect; `Established` means independent
// sources reproduced it. Admitting books without a guard would quietly promote coaching doctrine
// into the two tiers that exist to mean the opposite. So the pack loader demotes any condition or
// edge whose citation resolves to a reference carrying ONLY an ISBN out of those tiers and down to
// `Indirect` — see `bookCitationAtPeerTier` in characteristic_pack.cpp. The source is real and it
// supports the reasoning; it just does not TEST the named pair. Nothing stops a book backing
// `Practice`, which is the tier most of them belong to and the reason this was worth building.
//
// TWO KINDS OF RECORD, AND THE FLAG IS ADDITIVE. Most records are here because something cites
// them. Some are here because a coach or a contributor opening the bibliography should find the
// field's standard reading, not only the specific papers that happened to back a specific edge —
// those carry `generalReading`.
//
// The flag says NOTHING about whether the record is cited, and that is the design decision worth
// defending. A record may be cited AND be general reading: `ref.hume2005` is the field's standard
// review and will pick up citations as the pack matures. Modelling this as a two-value `role` enum
// would force a choice that does not exist, and the first time a general-reading record acquired a
// citation somebody would have to decide which list it LEAVES. So the flag is one more fact about
// the record — it earns its place on its own — and the two populations overlap freely.
//
// A general-reading record obeys every rule a cited one does: identifier, title, authors, year,
// namespace, no duplicates. It is not a lower standard, it is a different reason to be here. The
// mirror of the flag is `referenceOrphan` (diagnostics_health.h): cited by nothing AND unflagged,
// which is a record nobody has explained.
//
// PROVENANCE OF THE PROVENANCE. Every record was resolved against CrossRef (or PubMed, or a library
// catalogue for the ISBNs) and its title, journal or publisher, authors and year read off that
// record. Nothing here is recalled from memory, and nothing is copied out of another paper's
// reference list without resolving it first — a plausible-looking identifier is worse than no
// citation at all, because the null is honest and the wrong identifier is a lie that survives
// review. `references_test` re-asserts the shape; only a human re-resolving them can re-assert the
// content.
//
// A PERIODICAL AND A BOOK ARE DIFFERENT CONTAINERS, and `journal` cannot be asked to mean both.
// A chapter in an edited volume needs the volume's title, and that is `containerTitle` rather than
// a second meaning for `journal` — because `journal` already means "a periodical" everywhere in
// this codebase and in the UI. `ReferencesView` renders the byline as authors · journal-or-publisher
// · year, so a book title arriving in the journal slot would render as a small, checkable lie for
// whichever case lost the argument. The two fields are therefore MUTUALLY EXCLUSIVE — they map to
// the same CSL container, and a record carrying both cannot be typed at all. That is an ERROR
// (`referenceContainerConflict`), not a warning: there is no reading of such a record to salvage.
//
// Deliberately NOT behind an abstract provider, for the same reason as screens and drills: that
// polymorphism exists so a COMMUNITY pack can be namespaced against core, there is no community
// story for a bibliography, and three provider classes for a flat list would be ceremony standing
// in for a requirement. A user layer merges by id, and that is the whole story.

namespace pinpoint::analysis {

// Refused above this, never partially read — the same contract as kPackSchemaVersion. It has stayed
// at 1 through five additive fields (containerTitle, editor, volume, issue, pages) and the
// generalReading flag, deliberately: each is optional and a reader that has never heard of it still
// loads every record and still resolves every citation. See the notes on those fields — a schema
// version that moves for a no-op teaches the next author that the number means nothing.
inline constexpr int kReferenceSchemaVersion = 1;

struct Reference {
    QString id;           // `ref.*`
    QString doi;          // the join key with Provenance::citation — an exact string match
    QString pmid;         // the SAME join key, for journals that issue no DOI at all
    QString isbn;         // the THIRD join key — books, and chapters in edited volumes
    QString title;
    QString authors;      // family names in citation order, comma separated
    QString journal;
    QString publisher;    // a book has a publisher where a paper has a journal. NOT interchangeable
                          // with `journal`: the view labels that field as the periodical, so a
                          // publisher sitting in it renders as a small, checkable lie.

    // The BOOK a chapter appears in — never a journal, and never set alongside `journal`. See the
    // header block: the two are the same container in every citation format, so a record carrying
    // both raises `referenceContainerConflict` rather than picking one.
    QString containerTitle;
    QString editor;       // the volume's editor(s), for a chapter. Same convention as `authors`

    int     year = 0;

    // Article locators. STRINGS, not ints, and not because it was easier: page ranges ("1156-1163"),
    // article numbers used in place of pages ("91") and issue designators ("Suppl 1") all occur in
    // this literature, and not one of them is an integer.
    QString volume;
    QString issue;
    QString pages;

    // None of the five fields above — containerTitle, editor, volume, issue, pages — bumped
    // `schemaVersion`, on the reasoning spelled out for `generalReading` below: each is optional and
    // additive, so a reader that has never heard of them still loads every record and still resolves
    // every citation.

    QString establishes;  // what it actually shows, and what it does not

    // Worth reading regardless of what cites it. ADDITIVE, not a category — see the header block.
    //
    // No `schemaVersion` bump came with this field, deliberately. It is optional and additive: a
    // reader that has never heard of it still loads every record correctly and still resolves every
    // citation, because an absent flag and a false one are indistinguishable. Bumping would force a
    // migration for a default, and a schema version that moves for a no-op teaches the next author
    // that the number means nothing.
    bool    generalReading = false;

    // Where the reader goes when they tap the row: doi.org, PubMed when there is no DOI, or the
    // Open Library catalogue when the only identifier is an ISBN.
    QString url() const;

    // The identifier as a reader should SEE it. A DOI announces what it is — it starts "10." and
    // has a slash in it. A bare PubMed id is eight digits that could be anything: a year, a count,
    // an internal key, and a bare ISBN is thirteen digits with the same problem. So both are
    // labelled, and this is the one place that decides how.
    QString identifierLabel() const;
};

struct ReferenceSet {
    QString                id;
    QString                version;
    int                    schemaVersion = kReferenceSchemaVersion;
    QString                sourceLabel;
    bool                   readOnly = false;
    std::vector<Reference> references;

    const Reference *reference(const QString &id) const;

    // The join the UI actually makes: a Provenance::citation against a DOI, a PMID *or* an ISBN.
    const Reference *byCitation(const QString &citation) const;
};

// ── Validation ──────────────────────────────────────────────────────────────
//
// LOAD-TIME ERRORS (loadReferenceSet):
//   parse               the bytes are not JSON, or not a JSON object. This was reported as
//                       `schemaVersion` until the vocabularies were aligned — a code that named a
//                       fault the file had not committed, on the one registry where a reader
//                       chasing "which version is wrong?" would find nothing to fix
//   schemaTooNew        the set is from a newer build — see kReferenceSchemaVersion
//
// ERRORS:
//   duplicateId         two references share an id
//   duplicateDoi        two references share a DOI — the join is by DOI, so the second is
//                       unreachable and the pack would silently resolve to whichever came first
//   duplicatePmid       the same, for the PMID join key
//   duplicateIsbn       and the same again for the ISBN. Two chapters of ONE edited volume share
//                       its ISBN, so this is the code an author will meet first — and it is right
//                       that they do: an identifier that names two records joins to neither.
//   referenceIdNamespace  an id outside the `ref.` namespace
//   referenceNoDoi      a reference with NONE of a DOI, a PMID or an ISBN cannot be joined to
//                       anything or opened by anyone. (The code name predates both PMID and ISBN
//                       support and is DELIBERATELY kept: it is the stable contract the health view
//                       and tests key off, and renaming it would break both silently.)
//   referenceContainerConflict  a reference carries BOTH a journal and a containerTitle. They are
//                       one field in every citation format, so the record cannot be typed: it
//                       claims to be a paper in a periodical and a chapter in a book at once. An
//                       error rather than a warning because there is no half of it worth keeping.
// WARNINGS:
//   referenceNoTitle    a row that renders as a bare identifier
//   referenceNoYear     undated, so a reader cannot judge how current it is
ValidationReport validateReferenceSet(const ReferenceSet &set);

// The same rule for a citation string with no Reference in hand — every provenance block in the UI
// shows one of these, not just the bibliography. Resolves against the shared registry so the label
// is a fact rather than a guess, and falls back to SHAPE when a citation does not resolve: a DOI
// always starts "10."; an ISBN is a run of digits of a known length with a known prefix; and only
// what survives both of those can be a PubMed id.
//
// Returns a string for DISPLAY ONLY. The stored citation stays the bare identifier — it is the
// join key, and "PMID 30479527" joins to nothing.
//
// Two forms. The one-argument version reads the shared registry and is what almost every caller
// wants; the overload takes the set explicitly, for code that has already been HANDED one and must
// not reach for a global instead — a label that disagreed with the resolution the caller just made
// would be the same citation rendered two ways on one screen.
QString citationLabel(const QString &citation, const ReferenceSet &set);
QString citationLabel(const QString &citation);

using ReferenceLoadResult = LoadResult<ReferenceSet>;

ReferenceLoadResult loadReferenceSet(const QByteArray &json, const QString &sourceLabel);
QByteArray          saveReferenceSet(const ReferenceSet &set);

// The registry as CSL-JSON: a bare JSON ARRAY of items, which is what the format is.
//
// CSL-JSON is the interchange format of the citation-style ecosystem — Zotero exports it natively,
// pandoc consumes it, and every CSL style renders from it. Emitting it once means the bibliography
// leaves this app as APA, Vancouver, Harvard or anything else without us writing a single citation
// style. It is a one-way door: there is no importer, and the app is not a reference manager.
//
// PURE — takes a set, returns bytes, touches no filesystem. That is what makes it testable without
// a temp directory, and it is the same split as `roadmapMarkdown()` from `exportRoadmap()`.
//
// Two things in the mapping are easy to get wrong and silently so, and both are pinned by tests:
// CSL names every field lowercase-hyphenated EXCEPT the identifiers (`DOI`, `PMID`, `ISBN`, `URL`),
// which are uppercase; and `issued` is `{"date-parts": [[2005]]}` — an array of arrays holding an
// integer, not a year, not a string, not one level of nesting.
//
// AUTHORS ARE EMITTED AS A CSL `literal`, DELIBERATELY — see the comment on the mapper. Anyone
// tempted to "fix" that should read it first: the fix is a schema change, not a serialiser change.
QByteArray          exportReferenceSetCsl(const ReferenceSet &set);

const ReferenceSet &sharedReferenceSet();
void                resetSharedReferenceSet();

// ── The two layers, separately ──────────────────────────────────────────────
//
// sharedReferenceSet() is the MERGE, which is what everybody who only reads wants. Anything that
// WRITES needs the layers apart: it must write back the user's own records and never a flattened
// copy of the shipped bibliography, and it has to be able to answer "does this ship?", which the
// merge cannot.
//
// The same split screens and drills already have, arriving here for parity rather than for a
// caller: the Diagnostic Model panel holds references READ-ONLY on purpose — they are regenerated
// on every pack build, so an edit made there would be overwritten — and that argument is about the
// panel, not about the registry. A user layer has always been read and merged here; it simply had
// no way to be written, which made "imported, so read-only" a property of the code rather than a
// decision anyone had taken.
const ReferenceSet &coreReferenceSet();          // shipped only, cached like the merge
ReferenceSet        loadUserReferenceSet();      // the user layer alone; empty when there is no file
QString             userReferenceSetPath();
bool                saveUserReferenceSet(const ReferenceSet &set, QString *whyNot = nullptr);

} // namespace pinpoint::analysis
