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

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace pinpoint::analysis {

QString Reference::url() const
{
    // A DOI resolves straight to the publisher, so it wins where both exist. Falling back to the
    // PMID matters more than it looks: building a doi.org URL out of a bare PubMed id yields a
    // link that is well-formed, clickable, and dead — the worst of the three outcomes, because it
    // fails only for the reader and never for us.
    if (!doi.isEmpty())  return QStringLiteral("https://doi.org/") + doi;
    if (!pmid.isEmpty()) return QStringLiteral("https://pubmed.ncbi.nlm.nih.gov/") + pmid
                              + QStringLiteral("/");
    return QString();
}

QString Reference::identifierLabel() const
{
    if (!doi.isEmpty())  return doi;
    if (!pmid.isEmpty()) return QStringLiteral("PMID ") + pmid;
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
        return (!r.doi.isEmpty() && r.doi == c) || (!r.pmid.isEmpty() && r.pmid == c);
    });
    return it == references.end() ? nullptr : &*it;
}

namespace {

void add(ValidationReport &r, IssueSeverity sev, const QString &code, const QString &subject,
         const QString &message)
{
    r.issues.push_back(ValidationIssue{ sev, code, subject, message });
}

QString userReferencePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/diagnostics/references.json");
}

} // namespace

QString citationLabel(const QString &citation)
{
    const QString c = citation.trimmed();
    if (c.isEmpty()) return QString();

    // Ask the registry first: it knows which field this string came out of, so the label is a
    // fact. Everything shipped resolves — `reference_sets_test` fails the build otherwise.
    if (const Reference *ref = sharedReferenceSet().byCitation(c)) return ref->identifierLabel();

    // A user-layer citation naming a paper the registry has never heard of still has to render as
    // something. Shape is enough to tell the two apart: a DOI is "10.<registrant>/<suffix>".
    const bool allDigits = std::all_of(c.begin(), c.end(), [](QChar ch) { return ch.isDigit(); });
    return allDigits ? QStringLiteral("PMID ") + c : c;
}

ValidationReport validateReferenceSet(const ReferenceSet &set)
{
    ValidationReport r;
    QSet<QString>    ids, dois, pmids;

    for (const Reference &ref : set.references) {
        if (ids.contains(ref.id))
            add(r, IssueSeverity::Error, QStringLiteral("duplicateId"), ref.id,
                QStringLiteral("Two references share the id '%1'.").arg(ref.id));
        ids.insert(ref.id);

        if (!ref.id.startsWith(QStringLiteral("ref.")))
            add(r, IssueSeverity::Error, QStringLiteral("referenceIdNamespace"), ref.id,
                QStringLiteral("Reference id '%1' is outside the 'ref.' namespace.").arg(ref.id));

        // An identifier is both the join key and the only way a reader reaches the paper. Without
        // one the row is decoration. Either kind will do — some journals issue no DOI at all.
        if (ref.doi.isEmpty() && ref.pmid.isEmpty())
            add(r, IssueSeverity::Error, QStringLiteral("referenceNoDoi"), ref.id,
                QStringLiteral("Reference '%1' has neither a DOI nor a PMID, so nothing can cite "
                               "it and nobody can open it.").arg(ref.id));

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
        add(out.report, IssueSeverity::Error, QStringLiteral("schemaVersion"), sourceLabel,
            QStringLiteral("Cannot parse %1: %2").arg(sourceLabel, pe.errorString()));
        return out;
    }

    const QJsonObject root = doc.object();
    out.set.id            = root.value(QStringLiteral("id")).toString();
    out.set.version       = root.value(QStringLiteral("version")).toString();
    out.set.schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(1);
    out.set.sourceLabel   = sourceLabel;

    for (const QJsonValue &v : root.value(QStringLiteral("references")).toArray()) {
        const QJsonObject o = v.toObject();
        Reference         ref;
        ref.id          = o.value(QStringLiteral("id")).toString();
        ref.doi         = o.value(QStringLiteral("doi")).toString();
        ref.pmid        = o.value(QStringLiteral("pmid")).toString();
        ref.title       = o.value(QStringLiteral("title")).toString();
        ref.authors     = o.value(QStringLiteral("authors")).toString();
        ref.journal     = o.value(QStringLiteral("journal")).toString();
        ref.year        = o.value(QStringLiteral("year")).toInt();
        ref.establishes = o.value(QStringLiteral("establishes")).toString();
        out.set.references.push_back(std::move(ref));
    }

    out.parsed = true;
    out.report = validateReferenceSet(out.set);
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
        if (!ref.title.isEmpty())       o.insert(QStringLiteral("title"), ref.title);
        if (!ref.authors.isEmpty())     o.insert(QStringLiteral("authors"), ref.authors);
        if (!ref.journal.isEmpty())     o.insert(QStringLiteral("journal"), ref.journal);
        if (ref.year > 0)               o.insert(QStringLiteral("year"), ref.year);
        if (!ref.establishes.isEmpty()) o.insert(QStringLiteral("establishes"), ref.establishes);
        arr.append(o);
    }
    root.insert(QStringLiteral("references"), arr);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

namespace {

ReferenceSet assembleReferenceSet()
{
    const QByteArray override = qgetenv("PINPOINT_CORE_REFERENCES");
    const QString    corePath = override.isEmpty()
                                    ? QStringLiteral(":/diagnostics/references.json")
                                    : QString::fromLocal8Bit(override);

    ReferenceSet out;
    QFile        f(corePath);
    if (f.open(QIODevice::ReadOnly)) {
        ReferenceLoadResult res = loadReferenceSet(f.readAll(), corePath);
        out = std::move(res.set);
    }
    out.readOnly = true;

    const QString userPath = userReferencePath();
    QFile         uf(userPath);
    if (!userPath.isEmpty() && uf.open(QIODevice::ReadOnly)) {
        ReferenceLoadResult res = loadReferenceSet(uf.readAll(), userPath);
        if (res.parsed) {
            for (const Reference &ref : res.set.references) {
                auto it = std::find_if(out.references.begin(), out.references.end(),
                                       [&](const Reference &x) { return x.id == ref.id; });
                if (it != out.references.end()) *it = ref;
                else                            out.references.push_back(ref);
            }
            out.readOnly = false;
        }
    }
    return out;
}

ReferenceSet *g_refs = nullptr;

} // namespace

const ReferenceSet &sharedReferenceSet()
{
    if (!g_refs) g_refs = new ReferenceSet(assembleReferenceSet());
    return *g_refs;
}

void resetSharedReferenceSet()
{
    delete g_refs;
    g_refs = nullptr;
}

} // namespace pinpoint::analysis
