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

#include <QByteArray>
#include <QJsonValue>
#include <QString>
#include <QStringList>

#include <vector>

// What every content registry in src/Diagnostics does the same way.
//
// There are five of them — characteristics, norms, references, screens, drills — and only the
// first two were written. The other three were CLONED from them, one file at a time, and what a
// clone reliably brings across is the shape while leaving behind the arguments: the schema gate,
// the error vocabulary, the reason a temporary file is renamed rather than written in place. By the
// time this header was written the tmp-and-rename dance existed four times, the AppDataLocation
// computation six times, the "push a ValidationIssue" one-liner four times, and the load result
// five times with two different field orders. None of those copies disagreed on purpose. They
// disagreed because a copy only ever gets the thought its own author had.
//
// So: one copy of each, here, with the argument written once at the shared site. What belongs in
// this header is anything all five registries would otherwise each hold an opinion about. What does
// NOT belong is anything one of them means differently — every such difference in those files is
// argued in place, and the point of collapsing the mechanical duplication is that the deliberate
// asymmetries stop being camouflaged by it.
//
// This is a PUBLIC header rather than a `_p` one, because `LoadResult<T>` and the issue vocabulary
// are what the loaders hand back to their callers. `enum_table_p.h` is the internal counterpart —
// the (enum -> token) machinery, which nothing outside this directory ever sees.

namespace pinpoint::analysis {

// ── The issue vocabulary ────────────────────────────────────────────────────
//
// Lives here rather than in characteristic_pack.h, where it was first written, because all five
// registries report through it and only one of them owns characteristics. characteristic_pack.h
// includes this header, so every existing consumer still sees these types exactly where it always
// did.
//
// The CODES themselves are documented per registry, in each pack header's code table, because a
// code is a contract between one validator and the surfaces that key on it. What is shared is the
// spelling of the two that mean the same thing everywhere:
//
//   parse          the bytes are not JSON, or not a JSON object. One spelling across all five —
//                  a health view filtering for "this file is broken" cannot be asked to know which
//                  registry it happens to be looking at, and it used to have to: the same fault
//                  arrived as "parse", as "badNormFile", and once as "schemaVersion"
//   schemaTooNew   the file declares a schema version this build does not understand, so it is
//                  refused rather than partially read. Deliberately distinct from a malformed or
//                  missing version FIELD, which stays `schemaVersion`: one is content from the
//                  future and the other is content that is simply wrong, and an author fixes them
//                  by opposite means — upgrade the build, or correct the file
enum class IssueSeverity {
    Error,     // the content is not usable as-is
    Warning,   // it loads, but something is wrong or incomplete — this is the health list
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

// Appending an issue, which four of the five validators had written out for themselves. `err` and
// `warn` read at the call site the way the code tables read on the page, and the severity is then
// never chosen by an argument that can be typed the wrong way round.
void add(ValidationReport &r, IssueSeverity sev, const QString &code, const QString &subject,
         const QString &message);
void err(ValidationReport &r, const QString &code, const QString &subject, const QString &message);
void warn(ValidationReport &r, const QString &code, const QString &subject, const QString &message);

// Append the findings of a RE-VALIDATION over an assembled library, keeping only what the layers
// beneath it have not already said.
//
// Every merged provider does the same two things, and both are load-bearing. It copies each layer's
// own load-time report, so a fault stays attributed to the file it arrived in — the health list has
// to be able to say WHICH pack is broken. And it then re-validates the ASSEMBLED result, because a
// cross-layer edge can produce a cycle, a dangling reference or a duplicate that no layer contained
// on its own, and that is precisely what a per-layer check cannot see.
//
// Done naively the two overlap almost completely. A standalone warning on shipped content — an
// uncited tier, a single-tail axis — survives the merge untouched, so core reports it and the
// re-validation reports it again, and every shipped warning appears in the health list twice. A
// list read twice is a list nobody trusts the count of.
//
// Deduped on (code, subject) rather than by a hand-kept list of "codes only a merge can produce",
// because there is no such list. `cycle` is the archetype: assembling two packs is the ONLY way to
// get a cycle that spans them, and one pack can perfectly well contain a cycle on its own — a
// whitelist would have to admit `cycle` and would then double-report the single-pack case. The key
// asks the question that actually decides it: has this exact finding, about this exact subject,
// already been said? A genuinely new finding — a new code, or the same code about a different id,
// which is what every cross-pack referential error is — still lands, so the re-validation stays
// authoritative for the integrity of the assembly.
//
// The message is deliberately NOT part of the key. Two reports of one finding word it differently
// (a layer names the file it read, the assembly names the id at fault), so keying on the text would
// make the dedupe fail exactly where somebody had improved the wording.
//
// `into` is read as well as written: it is the accumulated layer report, and it seeds what counts
// as already-said.
void appendUnreported(ValidationReport &into, const ValidationReport &revalidation);

// ── What a load returns ─────────────────────────────────────────────────────
//
// One template over five payloads. The five structs it replaces were the same four fields with the
// same meanings; two declared `loaded` first and three declared `parsed` first, which is exactly
// the kind of difference that means nothing and still has to be read carefully by everyone who
// meets it.
//
// `parsed` is declared first because it happens first, and because the pair is easiest to get
// wrong in the other order: a caller reading down the struct stops at the first flag that sounds
// like success, and for a LAYER that flag is the wrong one.
//
// The payload member is `pack` for all five, including the three whose type is called a Set. One
// name is the point of one template, and `pack` is the family word this whole layer is named for;
// the alternative was five spellings of one member, which is what was here before.
template <typename T>
struct LoadResult {
    T                pack;
    ValidationReport report;

    // Parsed successfully, whatever validation said. An overlay layer (user or community)
    // routinely fails standalone referential integrity — its edges point at CORE content it does
    // not itself contain, its norm rows key on measures it does not carry — so referential checks
    // are only meaningful on the ASSEMBLED library. A caller merging layers must key off `parsed`;
    // keying off `loaded` silently discards every overlay, and then the next save writes the empty
    // result over the author's own file.
    bool             parsed = false;

    // Parsed AND validated clean. What a caller wants when it is reading a WHOLE library rather
    // than a layer of one.
    bool             loaded = false;
};

// ── Reading ─────────────────────────────────────────────────────────────────

// An array of strings, with anything that is not a string reading as empty rather than failing.
// The registries use it for id lists (aliases, detectedBy, drills, equipment, measures), where a
// malformed entry is caught downstream by the referential checks that already have to run — so
// refusing here would report the same fault twice and in the less useful place.
QStringList readStringList(const QJsonValue &v);

// ── Writing ─────────────────────────────────────────────────────────────────

// Where every user-authored diagnostics file lives: ONE directory under the writable app data
// location, shared by packs, norm sets, contexts, screens, drills and references. One directory
// rather than six because a user thinks of their diagnostics content as one thing, and six would
// guarantee that a backup captured a fraction of it. The cost is that a registry reading `*.json`
// there has to skip its siblings by name — see the note in file_pack_provider.cpp.
//
// Empty when there is no writable location at all, which is a real state on a locked-down system
// and is why every caller has to check.
QString diagnosticsDataDir();

// One file in that directory, or empty when the directory is. The isEmpty() dance was written out
// six times and is the whole reason a registry's path function can now be one line.
QString diagnosticsFilePath(const QString &fileName);

// Write `bytes` to `path` via a temporary file and a rename, creating the directory if it does not
// exist. False on failure, with `whyNot` set to something a user can act on.
//
// The rename is the point: an interrupted save must not truncate content the author has spent time
// on. A half-written pack is worse than no pack, because the app loads it, reports what survived,
// and looks like it has merely lost the rest — which it has.
//
// The four copies this replaces were behaviourally identical, so there was nothing to reconcile;
// what they shared was one hazard, and it is fixed here once. All four ignored the result of the
// WRITE. A full disk therefore produced a short temporary file, and the rename then put it over
// the good one — the exact outcome the temporary file exists to prevent, arrived at through the
// mechanism meant to prevent it. So the write and the close are both checked, and a failed write
// leaves the original untouched and takes the temporary with it.
//
// Deliberately NOT fsync'd. That would buy durability across a power cut, and what this is for is
// atomicity across a crash or a kill — the rename gives that on every filesystem the app runs on.
// Paying a sync on every keystroke-driven save of an authoring panel to insure against a case the
// user would re-do anyway is the wrong trade, and it is worth saying so here rather than leaving
// the next reader to wonder whether it was forgotten.
//
// An EMPTY path is accepted and reported as "no writable application data location", because that
// is what an empty path from diagnosticsFilePath() means and all four callers had to say so
// themselves.
bool atomicWrite(const QString &path, const QByteArray &bytes, QString *whyNot = nullptr);

} // namespace pinpoint::analysis
