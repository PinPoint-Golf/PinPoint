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

#include "reference_pack.h"

#include "pack_io.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// Hyphens in an identifier are presentation: "978-1-875378-37-1" and "9781875378371" are the same
// ISBN. They are stripped for anything that has to reason about the identifier — the URL, the shape
// test — while the stored form stays exactly as the author wrote it.
QString bareIsbn(const QString &isbn)
{
    QString out = isbn;
    out.remove(QLatin1Char('-'));
    out.remove(QLatin1Char(' '));
    return out;
}

// Is this string an ISBN by SHAPE alone? Only ever asked of a citation the registry could not
// resolve — and it has to be asked before "a run of digits is a PubMed id", because an ISBN-13 is
// thirteen digits and would otherwise render as a confident, well-formed, wrong `PMID 9781875378371`.
bool looksLikeIsbn(const QString &s)
{
    const QString d = bareIsbn(s);

    if (d.size() == 13)
        return std::all_of(d.begin(), d.end(), [](QChar c) { return c.isDigit(); })
               && (d.startsWith(QStringLiteral("978")) || d.startsWith(QStringLiteral("979")));

    if (d.size() == 10) {
        // The ISBN-10 check digit is computed mod 11, so it has ELEVEN possible values and the
        // eleventh is written 'X'. A trailing X is CORRECT here, not a typo and not a placeholder —
        // dropping the case would send every book whose check digit happens to be 10 down the PMID
        // branch. This is the line a future reader will assume is a mistake; it is not.
        const QChar last = d.back();
        return std::all_of(d.begin(), d.end() - 1, [](QChar c) { return c.isDigit(); })
               && (last.isDigit() || last == QLatin1Char('X') || last == QLatin1Char('x'));
    }

    return false;
}

} // namespace

QString Reference::url() const
{
    // A DOI resolves straight to the publisher, so it wins where several exist. Falling back
    // matters more than it looks: building a doi.org URL out of a bare PubMed id or ISBN yields a
    // link that is well-formed, clickable, and dead — the worst of the outcomes, because it fails
    // only for the reader and never for us.
    if (!doi.isEmpty())  return QStringLiteral("https://doi.org/") + doi;
    if (!pmid.isEmpty()) return QStringLiteral("https://pubmed.ncbi.nlm.nih.gov/") + pmid
                              + QStringLiteral("/");
    // No ISBN resolver has doi.org's standing, so this is a choice rather than a lookup, and it is
    // made once here. Open Library is a non-commercial catalogue, it takes a bare ISBN in the path,
    // and an ISBN it does not hold lands on a "not in catalogue" page rather than a dead link.
    // Deliberately NOT a bookseller: the standing rule against naming commercial organisations
    // governs the URLs this repo generates, not only the content it stores.
    if (!isbn.isEmpty()) return QStringLiteral("https://openlibrary.org/isbn/") + bareIsbn(isbn);
    return QString();
}

QString Reference::identifierLabel() const
{
    if (!doi.isEmpty())  return doi;
    if (!pmid.isEmpty()) return QStringLiteral("PMID ") + pmid;
    if (!isbn.isEmpty()) return QStringLiteral("ISBN ") + isbn;
    return QString();
}

const Reference *ReferenceSet::reference(const QString &rid) const
{
    const auto it = std::find_if(references.begin(), references.end(),
                                 [&](const Reference &r) { return r.id == rid; });
    return it == references.end() ? nullptr : &*it;
}

const Reference *ReferenceSet::byCitation(const QString &c) const
{
    if (c.isEmpty()) return nullptr;
    const auto it = std::find_if(references.begin(), references.end(), [&](const Reference &r) {
        return (!r.doi.isEmpty() && r.doi == c) || (!r.pmid.isEmpty() && r.pmid == c)
            || (!r.isbn.isEmpty() && r.isbn == c);
    });
    return it == references.end() ? nullptr : &*it;
}

namespace {

QString userReferencePath() { return diagnosticsFilePath(QStringLiteral("references.json")); }

} // namespace

QString citationLabel(const QString &citation, const ReferenceSet &set)
{
    const QString c = citation.trimmed();
    if (c.isEmpty()) return QString();

    // Ask the registry first: it knows which field this string came out of, so the label is a
    // fact. Everything shipped resolves — `reference_sets_test` fails the build otherwise.
    if (const Reference *ref = set.byCitation(c)) return ref->identifierLabel();

    // A user-layer citation naming a source the registry has never heard of still has to render as
    // something. Shape is enough to tell the three apart, PROVIDED the ISBN test runs first: an
    // ISBN-13 is thirteen digits, so under a bare all-digits rule it renders "PMID 9781875378371",
    // which is well-formed, confident and wrong. A DOI is whatever survives — "10.<reg>/<suffix>".
    if (looksLikeIsbn(c)) return QStringLiteral("ISBN ") + c;
    const bool allDigits = std::all_of(c.begin(), c.end(), [](QChar ch) { return ch.isDigit(); });
    return allDigits ? QStringLiteral("PMID ") + c : c;
}

QString citationLabel(const QString &citation)
{
    return citationLabel(citation, sharedReferenceSet());
}

ValidationReport validateReferenceSet(const ReferenceSet &set)
{
    ValidationReport r;
    QSet<QString>    ids, dois, pmids, isbns;

    for (const Reference &ref : set.references) {
        if (ids.contains(ref.id))
            add(r, IssueSeverity::Error, QStringLiteral("duplicateId"), ref.id,
                QStringLiteral("Two references share the id '%1'.").arg(ref.id));
        ids.insert(ref.id);

        if (!ref.id.startsWith(QStringLiteral("ref.")))
            add(r, IssueSeverity::Error, QStringLiteral("referenceIdNamespace"), ref.id,
                QStringLiteral("Reference id '%1' is outside the 'ref.' namespace.").arg(ref.id));

        // An identifier is both the join key and the only way a reader reaches the source. Without
        // one the row is decoration. Any of the three will do — some journals issue no DOI at all,
        // and a book was never going to have one. The CODE NAME predates both the PMID and the ISBN
        // and is kept deliberately: it is the contract the health view and the tests key off.
        if (ref.doi.isEmpty() && ref.pmid.isEmpty() && ref.isbn.isEmpty())
            add(r, IssueSeverity::Error, QStringLiteral("referenceNoDoi"), ref.id,
                QStringLiteral("Reference '%1' has no DOI, PMID or ISBN, so nothing can cite it "
                               "and nobody can open it.").arg(ref.id));

        if (!ref.doi.isEmpty()) {
            if (dois.contains(ref.doi))
                add(r, IssueSeverity::Error, QStringLiteral("duplicateDoi"), ref.id,
                    QStringLiteral("Two references share the DOI '%1'. The join from a citation is "
                                   "by identifier, so the second could never be reached.")
                        .arg(ref.doi));
            dois.insert(ref.doi);
        }

        if (!ref.pmid.isEmpty()) {
            if (pmids.contains(ref.pmid))
                add(r, IssueSeverity::Error, QStringLiteral("duplicatePmid"), ref.id,
                    QStringLiteral("Two references share the PMID '%1'. The join from a citation is "
                                   "by identifier, so the second could never be reached.")
                        .arg(ref.pmid));
            pmids.insert(ref.pmid);
        }

        if (!ref.isbn.isEmpty()) {
            if (isbns.contains(ref.isbn))
                add(r, IssueSeverity::Error, QStringLiteral("duplicateIsbn"), ref.id,
                    QStringLiteral("Two references share the ISBN '%1'. The join from a citation is "
                                   "by identifier, so the second could never be reached.")
                        .arg(ref.isbn));
            isbns.insert(ref.isbn);
        }

        // A periodical and a book are ONE field in every citation format, so a record claiming both
        // cannot be typed at all — it says it is a paper in a journal and a chapter in a book at the
        // same time. There is no half of that worth keeping, which is why it is an error: picking
        // one silently would put a book title in the byline slot the view labels as the periodical.
        if (!ref.journal.isEmpty() && !ref.containerTitle.isEmpty())
            add(r, IssueSeverity::Error, QStringLiteral("referenceContainerConflict"), ref.id,
                QStringLiteral("Reference '%1' carries both a journal ('%2') and a containerTitle "
                               "('%3'). They are the same field in a citation, so the record cannot "
                               "be typed — a paper has a journal, a chapter has a container.")
                    .arg(ref.id, ref.journal, ref.containerTitle));

        if (ref.title.isEmpty())
            add(r, IssueSeverity::Warning, QStringLiteral("referenceNoTitle"), ref.id,
                QStringLiteral("Reference '%1' has no title and would render as a bare identifier.")
                    .arg(ref.id));

        if (ref.year <= 0)
            add(r, IssueSeverity::Warning, QStringLiteral("referenceNoYear"), ref.id,
                QStringLiteral("Reference '%1' has no year, so a reader cannot judge how current "
                               "it is.").arg(ref.id));
    }
    return r;
}

ReferenceLoadResult loadReferenceSet(const QByteArray &json, const QString &sourceLabel)
{
    ReferenceLoadResult out;

    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        err(out.report, QStringLiteral("parse"), sourceLabel,
            QStringLiteral("Cannot parse %1: %2").arg(sourceLabel, pe.errorString()));
        return out;
    }

    const QJsonObject root = doc.object();
    const int schema = root.value(QStringLiteral("schemaVersion")).toInt(kReferenceSchemaVersion);

    // Refused rather than partially read — see the same gate in loadPack(). A bibliography is the
    // registry where a dropped field is HARDEST to notice: the record still resolves, still opens,
    // and simply renders without whatever the newer build knew about it, so nothing on any screen
    // looks wrong.
    if (schema > kReferenceSchemaVersion) {
        err(out.report, QStringLiteral("schemaTooNew"), sourceLabel,
            QStringLiteral("Reference set '%1' declares schema %2; this build understands %3. "
                           "Refusing to load rather than silently dropping content.")
                .arg(sourceLabel.isEmpty() ? QStringLiteral("(unnamed)") : sourceLabel)
                .arg(schema).arg(kReferenceSchemaVersion));
        return out;
    }

    out.pack.id            = root.value(QStringLiteral("id")).toString();
    out.pack.version       = root.value(QStringLiteral("version")).toString();
    out.pack.schemaVersion = schema;
    out.pack.sourceLabel   = sourceLabel;

    for (const QJsonValue &v : root.value(QStringLiteral("references")).toArray()) {
        const QJsonObject o = v.toObject();
        Reference         ref;
        ref.id          = o.value(QStringLiteral("id")).toString();
        ref.doi         = o.value(QStringLiteral("doi")).toString();
        ref.pmid        = o.value(QStringLiteral("pmid")).toString();
        ref.isbn        = o.value(QStringLiteral("isbn")).toString();
        ref.title       = o.value(QStringLiteral("title")).toString();
        ref.authors     = o.value(QStringLiteral("authors")).toString();
        ref.journal        = o.value(QStringLiteral("journal")).toString();
        ref.publisher      = o.value(QStringLiteral("publisher")).toString();
        ref.containerTitle = o.value(QStringLiteral("containerTitle")).toString();
        ref.editor         = o.value(QStringLiteral("editor")).toString();
        ref.year           = o.value(QStringLiteral("year")).toInt();
        ref.volume         = o.value(QStringLiteral("volume")).toString();
        ref.issue          = o.value(QStringLiteral("issue")).toString();
        ref.pages          = o.value(QStringLiteral("pages")).toString();
        ref.establishes = o.value(QStringLiteral("establishes")).toString();
        // Absent reads as false, which is the whole reason this needed no schema bump.
        ref.generalReading = o.value(QStringLiteral("generalReading")).toBool(false);
        out.pack.references.push_back(std::move(ref));
    }

    out.parsed = true;
    out.report = validateReferenceSet(out.pack);
    out.loaded = out.report.ok();
    return out;
}

QByteArray saveReferenceSet(const ReferenceSet &set)
{
    QJsonObject root;
    root.insert(QStringLiteral("id"), set.id);
    root.insert(QStringLiteral("version"), set.version);
    root.insert(QStringLiteral("schemaVersion"), set.schemaVersion);

    QJsonArray arr;
    for (const Reference &ref : set.references) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), ref.id);
        if (!ref.doi.isEmpty())         o.insert(QStringLiteral("doi"), ref.doi);
        if (!ref.pmid.isEmpty())        o.insert(QStringLiteral("pmid"), ref.pmid);
        if (!ref.isbn.isEmpty())        o.insert(QStringLiteral("isbn"), ref.isbn);
        if (!ref.title.isEmpty())       o.insert(QStringLiteral("title"), ref.title);
        if (!ref.authors.isEmpty())     o.insert(QStringLiteral("authors"), ref.authors);
        if (!ref.journal.isEmpty())     o.insert(QStringLiteral("journal"), ref.journal);
        if (!ref.publisher.isEmpty())   o.insert(QStringLiteral("publisher"), ref.publisher);
        if (!ref.containerTitle.isEmpty())
                                        o.insert(QStringLiteral("containerTitle"), ref.containerTitle);
        if (!ref.editor.isEmpty())      o.insert(QStringLiteral("editor"), ref.editor);
        if (ref.year > 0)               o.insert(QStringLiteral("year"), ref.year);
        if (!ref.volume.isEmpty())      o.insert(QStringLiteral("volume"), ref.volume);
        if (!ref.issue.isEmpty())       o.insert(QStringLiteral("issue"), ref.issue);
        if (!ref.pages.isEmpty())       o.insert(QStringLiteral("pages"), ref.pages);
        if (!ref.establishes.isEmpty()) o.insert(QStringLiteral("establishes"), ref.establishes);
        if (ref.generalReading)         o.insert(QStringLiteral("generalReading"), true);
        arr.append(o);
    }
    root.insert(QStringLiteral("references"), arr);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// ── CSL-JSON ────────────────────────────────────────────────────────────────

namespace {

// Our editorial judgement travels with the record, so it has to say whose judgement it is. Exported
// bare, `establishes` lands in someone else's library as an unattributed opinion about a paper.
// ONE constant, used ONCE, rather than a format string repeated through the mapper.
const QLatin1String kCslNotePrefix("PinPoint diagnostics: ");

// Every field except `id` and `type` is omitted when empty rather than written as "". A CSL consumer
// treats an empty string as present-but-blank and renders the punctuation around it — an empty
// `page` becomes a stray comma in the middle of a citation.
void putIf(QJsonObject &o, QLatin1String key, const QString &value)
{
    if (!value.isEmpty()) o.insert(key, value);
}

// CSL wants structured names: {"family": "Wallace", "given": "Eric S."}. `authors` is one
// comma-separated string of family names and cannot produce that, so we use `literal` — the spec's
// escape hatch for corporate and unparseable names, which every CSL processor handles.
//
// THE TRADE-OFF, because the next reader will assume this was an oversight: a literal name renders
// correctly in most styles but sorts wrong, and cannot be initialised or abbreviated to "Brown et
// al." The correct fix is restructuring `authors` into an array of family/given pairs — a migration
// across every record AND a genuine schemaVersion bump, because an old reader would find an array
// where it expects a string. That is a separate decision and it has deliberately not been taken.
// Changing this function alone does not achieve it.
QJsonArray literalName(const QString &names)
{
    QJsonObject one;
    one.insert(QLatin1String("literal"), names);
    return QJsonArray{ one };
}

} // namespace

QByteArray exportReferenceSetCsl(const ReferenceSet &set)
{
    QJsonArray items;

    for (const Reference &ref : set.references) {
        QJsonObject o;

        // Required by CSL, so written unconditionally. The `ref.` prefix stays: the id must be
        // unique within the export and it already is, and a reader who sees one in a rendered
        // bibliography can find it again here.
        o.insert(QLatin1String("id"), ref.id);

        // Inferred rather than stored, from what the record already carries. An ISBN means a book;
        // a book that also names the volume it sits inside is a chapter of that volume.
        const bool isBook = !ref.isbn.isEmpty();
        o.insert(QLatin1String("type"),
                 !isBook                         ? QStringLiteral("article-journal")
                 : !ref.containerTitle.isEmpty() ? QStringLiteral("chapter")
                                                 : QStringLiteral("book"));

        putIf(o, QLatin1String("title"), ref.title);

        if (!ref.authors.isEmpty()) o.insert(QLatin1String("author"), literalName(ref.authors));
        if (!ref.editor.isEmpty())  o.insert(QLatin1String("editor"), literalName(ref.editor));

        // One container, whichever of the two is set — validateReferenceSet() refuses both.
        putIf(o, QLatin1String("container-title"),
              ref.journal.isEmpty() ? ref.containerTitle : ref.journal);

        putIf(o, QLatin1String("publisher"), ref.publisher);

        // `date-parts` is an ARRAY OF ARRAYS holding an integer, and it is the single most commonly
        // botched part of CSL-JSON. Omitted entirely for an undated record rather than emitting a
        // zero, which a processor would render as the year 0.
        if (ref.year > 0) {
            // Built by append, NOT QJsonArray{ QJsonArray{ ref.year } }: a brace list holding a
            // single element of the SAME type copy-constructs from that element instead of
            // selecting the initializer_list constructor, so the outer array collapses into the
            // inner one and `date-parts` serialises as [2005] rather than [[2005]].
            QJsonArray ymd;
            ymd.append(ref.year);
            QJsonArray dateParts;
            dateParts.append(ymd);
            QJsonObject issued;
            issued.insert(QLatin1String("date-parts"), dateParts);
            o.insert(QLatin1String("issued"), issued);
        }

        putIf(o, QLatin1String("volume"), ref.volume);
        putIf(o, QLatin1String("issue"), ref.issue);
        putIf(o, QLatin1String("page"), ref.pages);   // CSL is `page`, singular

        // The identifier fields are the ONLY uppercase names in CSL-JSON. Everything else is
        // lowercase-hyphenated, and getting this backwards produces a file that parses cleanly and
        // drops every identifier.
        putIf(o, QLatin1String("DOI"), ref.doi);
        putIf(o, QLatin1String("PMID"), ref.pmid);    // a legal CSL-JSON field, not an extension
        putIf(o, QLatin1String("ISBN"), ref.isbn);
        putIf(o, QLatin1String("URL"), ref.url());    // the accessor, so doi->pmid->isbn stays in one place

        putIf(o, QLatin1String("note"),
              ref.establishes.isEmpty() ? QString()
                                        : QString(kCslNotePrefix) + ref.establishes);

        // Consumer support for `categories` varies, and that is acceptable: it is the spec-correct
        // field for this, and a consumer that ignores it loses nothing else about the record.
        if (ref.generalReading)
            o.insert(QLatin1String("categories"),
                     QJsonArray{ QStringLiteral("general reading") });

        items.append(o);
    }

    // A BARE ARRAY, not an object wrapping one. That is what the format is, and a consumer handed
    // the wrong outer shape fails with an error that says nothing about which end is wrong.
    return QJsonDocument(items).toJson(QJsonDocument::Indented);
}

namespace {

ReferenceSet loadCoreReferenceSet()
{
    const QByteArray override = qgetenv("PINPOINT_CORE_REFERENCES");
    const QString    corePath = override.isEmpty()
                                    ? QStringLiteral(":/diagnostics/references.json")
                                    : QString::fromLocal8Bit(override);

    ReferenceSet out;
    QFile        f(corePath);
    if (f.open(QIODevice::ReadOnly)) {
        ReferenceLoadResult res = loadReferenceSet(f.readAll(), corePath);
        out = std::move(res.pack);
    }
    out.readOnly = true;
    return out;
}

ReferenceSet assembleReferenceSet()
{
    ReferenceSet out = loadCoreReferenceSet();

    // The user's own layer, by id. A user record REPLACES the shipped one rather than merging field
    // by field: half of one paper's metadata carrying half of another's is worse than either, and
    // the identifier is the join key — a record whose DOI came from one source and whose title came
    // from another is the "plausible-looking identifier" this registry's header calls worse than no
    // citation at all.
    const ReferenceSet user = loadUserReferenceSet();
    if (!user.references.empty()) {
        for (const Reference &ref : user.references) {
            auto it = std::find_if(out.references.begin(), out.references.end(),
                                   [&](const Reference &x) { return x.id == ref.id; });
            if (it != out.references.end()) *it = ref;
            else                            out.references.push_back(ref);
        }
        out.readOnly = false;
    }
    return out;
}

ReferenceSet *g_refs = nullptr;
ReferenceSet *g_core = nullptr;

} // namespace

const ReferenceSet &coreReferenceSet()
{
    if (!g_core) g_core = new ReferenceSet(loadCoreReferenceSet());
    return *g_core;
}

QString userReferenceSetPath() { return userReferencePath(); }

ReferenceSet loadUserReferenceSet()
{
    // `parsed`, NOT `loaded`, for the reason the screen and drill layers are read that way. A user
    // bibliography holds the records that author added, so it fails nothing standalone — but it is
    // read through the same door, and a door that behaved differently here would be a difference
    // nobody could explain later.
    const QString path = userReferencePath();
    QFile         f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly)) return {};
    ReferenceLoadResult res = loadReferenceSet(f.readAll(), path);
    return res.parsed ? std::move(res.pack) : ReferenceSet{};
}

bool saveUserReferenceSet(const ReferenceSet &set, QString *whyNot)
{
    return atomicWrite(userReferenceSetPath(), saveReferenceSet(set), whyNot);
}

const ReferenceSet &sharedReferenceSet()
{
    // Cached process-wide, exactly like sharedScreenSet(). Not thread-safe against concurrent
    // readers — call resetSharedReferenceSet() from the UI thread after writing a user layer, or
    // every reader keeps the assembly it already has.
    if (!g_refs) g_refs = new ReferenceSet(assembleReferenceSet());
    return *g_refs;
}

void resetSharedReferenceSet()
{
    delete g_refs;
    g_refs = nullptr;
    // The core layer is immutable and cached separately, so it deliberately survives — nothing a
    // user write can do changes what shipped.
}

} // namespace pinpoint::analysis
