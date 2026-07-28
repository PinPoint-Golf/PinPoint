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

#include "norm_pack.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QObject>
#include <QSet>

#include <algorithm>

namespace pinpoint::analysis {

namespace {

// One table per enum, so a spelling exists in exactly one place — same shape as characteristic.cpp.
// Every `name` is a stable JSON token and must never change once a norm set has shipped with it.
template <typename E>
struct Row {
    E           value;
    const char *name;
    const char *label;
};

template <typename E, size_t N>
QString nameOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromLatin1(r.name);
    return QString::fromLatin1(rows[0].name);
}

template <typename E, size_t N>
QString labelOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromLatin1(r.label);
    return QString::fromLatin1(rows[0].label);
}

template <typename E, size_t N>
bool fromName(const Row<E> (&rows)[N], const QString &s, E &out)
{
    for (const auto &r : rows)
        if (s == QLatin1String(r.name)) { out = r.value; return true; }
    return false;
}

// The label column carries the user-facing words: "authored figure" says what a heuristic norm IS
// to a reader who has never seen the enum, where the token is only the JSON spelling.
const Row<NormSource> kNormSources[] = {
    { NormSource::Heuristic,  "heuristic",  "Authored figure" },
    { NormSource::Seated,     "seated",     "Seated on swings" },
    { NormSource::Literature, "literature", "Published" },
    { NormSource::Imported,   "imported",   "Imported" },
};

// The user-facing words. An ordinary, monotone ramp: nobody has to be told Ideal outranks Good, and
// the last two say what to do rather than describing a boundary.
const Row<Grade> kGrades[] = {
    { Grade::Ideal,       "ideal",       "Ideal" },
    { Grade::Good,        "good",        "Good" },
    { Grade::Watch,       "watch",       "Watch" },
    { Grade::Action,      "action",      "Action" },
    { Grade::NotMeasured, "notMeasured", "Not measured" },
};

} // namespace

QString normSourceName(NormSource s) { return nameOf(kNormSources, s); }
bool    normSourceFromName(const QString &s, NormSource &out) { return fromName(kNormSources, s, out); }
QString normSourceLabel(NormSource s) { return labelOf(kNormSources, s); }

QString gradeName(Grade g) { return nameOf(kGrades, g); }
bool    gradeFromName(const QString &s, Grade &out) { return fromName(kGrades, s, out); }
QString gradeLabel(Grade g) { return labelOf(kGrades, g); }

// ── Grade-policy presets ────────────────────────────────────────────────────
//
// The label and hint are marked for translation in the "NormModel" context because that panel is
// where they are rendered — the strings live here so the numbers and the words they promise cannot
// be edited apart, and the render site translates them at the point of use.
const std::vector<GradePolicyPreset> &gradePolicyPresets()
{
    static const std::vector<GradePolicyPreset> kPresets = {
        { "lenient",  QT_TRANSLATE_NOOP("NormModel", "Lenient"),
          QT_TRANSLATE_NOOP("NormModel", "Flags less. Suits a wide range of styles."),
          GradePolicy{ 1.5, 2.5, 3.5 } },
        { "standard", QT_TRANSLATE_NOOP("NormModel", "Standard"),
          QT_TRANSLATE_NOOP("NormModel", "The shipped setting. Ordinary variation is not a finding."),
          GradePolicy{ 1.0, 2.0, 3.0 } },
        { "strict",   QT_TRANSLATE_NOOP("NormModel", "Strict"),
          QT_TRANSLATE_NOOP("NormModel", "Flags more. Suits a narrow, coached population."),
          GradePolicy{ 0.75, 1.5, 2.25 } },
    };
    return kPresets;
}

const GradePolicyPreset &gradePolicyPresetFor(const QString &name)
{
    const std::vector<GradePolicyPreset> &presets = gradePolicyPresets();
    for (const GradePolicyPreset &p : presets)
        if (name == QLatin1String(p.name)) return p;
    return presets[1];                                       // standard
}

GradePolicy gradePolicyByName(const QString &name) { return gradePolicyPresetFor(name).policy; }

// ── Provenance standing ─────────────────────────────────────────────────────

bool normIsWeak(const Norm &n)
{
    switch (n.source) {
    case NormSource::Heuristic:  return true;                       // every one of these will move
    case NormSource::Seated:     return n.n < kMinSeatedN;
    case NormSource::Literature: return n.citation.trimmed().isEmpty();
    case NormSource::Imported:   return false;                      // it stood somewhere else first
    }
    return true;
}

QString normWeakReason(const Norm &n)
{
    switch (n.source) {
    case NormSource::Heuristic:
        return QObject::tr("A starting figure, not a fitted one. Expected to move once it is "
                           "seated on real swings.");
    case NormSource::Seated:
        return n.n < kMinSeatedN
                   ? QObject::tr("Seated on %n swing(s) — too few to be more than indicative.", "", n.n)
                   : QString();
    case NormSource::Literature:
        return n.citation.trimmed().isEmpty()
                   ? QObject::tr("Claims a published source but carries no citation.")
                   : QString();
    case NormSource::Imported:
        return QString();
    }
    return QString();
}

// ── NormPack ────────────────────────────────────────────────────────────────

const Norm *NormPack::find(const QString &measureId, const QString &contextId) const
{
    for (const Norm &n : norms)
        if (n.measureId == measureId && n.contextId == contextId) return &n;
    return nullptr;
}

bool NormPack::contains(const QString &measureId, const QString &contextId) const
{
    return find(measureId, contextId) != nullptr;
}

QStringList NormPack::contextsFor(const QString &measureId) const
{
    QStringList out;
    for (const Norm &n : norms)
        if (n.measureId == measureId) out.append(n.contextId);
    return out;
}

void NormPack::upsert(const Norm &norm)
{
    for (Norm &n : norms) {
        if (n.measureId == norm.measureId && n.contextId == norm.contextId) { n = norm; return; }
    }
    norms.push_back(norm);
}

bool NormPack::remove(const QString &measureId, const QString &contextId)
{
    const auto it = std::find_if(norms.begin(), norms.end(), [&](const Norm &n) {
        return n.measureId == measureId && n.contextId == contextId;
    });
    if (it == norms.end())
        return false;
    norms.erase(it);
    return true;
}

// ── Validation ──────────────────────────────────────────────────────────────

namespace {

QString normKeyLabel(const Norm &n)
{
    return QStringLiteral("%1 @ %2").arg(n.measureId, n.contextId);
}

} // namespace

ValidationReport validateNormPack(const NormPack &pack)
{
    ValidationReport rep;
    auto err = [&rep](const QString &code, const QString &subject, const QString &message) {
        rep.issues.push_back(ValidationIssue{ IssueSeverity::Error, code, subject, message });
    };
    auto warn = [&rep](const QString &code, const QString &subject, const QString &message) {
        rep.issues.push_back(ValidationIssue{ IssueSeverity::Warning, code, subject, message });
    };

    QSet<QString> seen;
    for (const Norm &n : pack.norms) {
        const QString key = normKeyLabel(n);

        if (n.measureId.isEmpty() || n.contextId.isEmpty()) {
            err(QStringLiteral("emptyNormKey"), key,
                QStringLiteral("A norm is missing its measure or its context."));
            continue;
        }

        if (seen.contains(key))
            err(QStringLiteral("duplicateNorm"), key,
                QStringLiteral("Two norms share the key '%1'.").arg(key));
        seen.insert(key);

        if (n.sigmaLo < 0.0 || n.sigmaHi < 0.0)
            err(QStringLiteral("negativeSigma"), key,
                QStringLiteral("Norm '%1' has a negative tolerance.").arg(key));

        if (n.monitorLo.has_value() != n.monitorHi.has_value())
            err(QStringLiteral("partialMonitor"), key,
                QStringLiteral("Norm '%1' sets only one side of its monitor band; set both or "
                               "neither.").arg(key));

        if (n.hasExplicitMonitor()) {
            if (*n.monitorLo > *n.monitorHi)
                err(QStringLiteral("monitorOrder"), key,
                    QStringLiteral("Norm '%1' has monitorLo above monitorHi.").arg(key));
            // The explicit band bounds the Watch/Action edge, so it MUST contain the Ideal band.
            // If it does not, a value sitting inside its own tolerance grades Action — which reads
            // as a detection but is a data-entry error.
            // Against the norm's own CLAIM, never the policy's Ideal band: validation runs at load
            // with no policy in hand, and a row that failed to load under `strict` but passed under
            // `lenient` would make a shared pack's validity depend on the reader's settings.
            else if (*n.monitorLo > n.claimLo() || *n.monitorHi < n.claimHi())
                err(QStringLiteral("monitorExcludesIdeal"), key,
                    QStringLiteral("Norm '%1' has a monitor band (%2..%3) that does not contain its "
                                   "own tolerance (%4..%5).")
                        .arg(key)
                        .arg(*n.monitorLo).arg(*n.monitorHi)
                        .arg(n.claimLo()).arg(n.claimHi()));
        }

        if (!(n.sigmaLo > 0.0) || !(n.sigmaHi > 0.0))
            warn(QStringLiteral("zeroSigma"), key,
                 QStringLiteral("Norm '%1' has no tolerance on one side, so only its exact centre "
                                "grades Ideal.").arg(key));

        if (n.source == NormSource::Literature && n.citation.isEmpty())
            warn(QStringLiteral("noProvenance"), key,
                 QStringLiteral("Norm '%1' claims a published source but carries no citation.")
                     .arg(key));
    }

    return rep;
}

ValidationReport validateNormsAgainst(const NormPack           &norms,
                                      const CharacteristicPack &pack,
                                      const ContextTree        &contexts)
{
    ValidationReport rep;
    auto err = [&rep](const QString &code, const QString &subject, const QString &message) {
        rep.issues.push_back(ValidationIssue{ IssueSeverity::Error, code, subject, message });
    };

    for (const Norm &n : norms.norms) {
        const QString key = normKeyLabel(n);

        const Measure *m = pack.measure(n.measureId);
        if (m == nullptr) {
            err(QStringLiteral("unknownNormMeasure"), key,
                QStringLiteral("Norm '%1' keys on a measure that is not in the library.").arg(key));
            continue;
        }

        if (!contexts.isEmpty() && !contexts.contains(n.contextId))
            err(QStringLiteral("unknownNormContext"), key,
                QStringLiteral("Norm '%1' keys on a context '%2' that is not in the context tree.")
                    .arg(key, n.contextId));

        // Unit drift is the quiet one. A norm authored in degrees against a measure that later
        // became a percentage still loads, still grades, and is wrong every time — so it is an
        // error naming BOTH units, never a coercion and never a warning.
        if (!n.unit.isEmpty() && !m->unit.isEmpty() && n.unit != m->unit)
            err(QStringLiteral("normUnitMismatch"), key,
                QStringLiteral("Norm '%1' is in '%2' but measure '%3' is in '%4'.")
                    .arg(key, n.unit, m->id, m->unit));

        if (m->status == MeasureStatus::NotCapturable)
            err(QStringLiteral("normNotCapturable"), key,
                QStringLiteral("Measure '%1' can never be captured, so a norm on it can only "
                               "mislead.").arg(m->id));

        // ── The norm's numbers against the measure's SHAPE ──────────────────
        //
        // Here rather than in validateNormPack for the same reason normUnitMismatch is: only the
        // assembled library knows what the measure claims. A norm row carries numbers and nothing
        // else — shape lives on the measure — so this is where the two are joined.
        if (shapeIsOneSided(m->shape)) {
            // One side of the tolerance is meaningless on a one-sided norm, and a row stating two
            // different values has said something about a tail that does not grade. Equal values
            // are fine and indistinguishable from the parse default: readNorm mirrors sigmaLo into
            // sigmaHi when absent, so the terse form a one-sided row should use lands here anyway.
            if (!qFuzzyCompare(1.0 + n.sigmaLo, 1.0 + n.sigmaHi))
                err(QStringLiteral("normShapeTolerance"), key,
                    QStringLiteral("Measure '%1' is '%2', so only one tail grades — but norm '%3' "
                                   "states different tolerances either side (%4 and %5). State one.")
                        .arg(m->id, shapeLabel(m->shape), key)
                        .arg(n.sigmaLo).arg(n.sigmaHi));

            // A monitor bound on the OPEN side names an edge the norm never grades against. Left
            // to load it would sit in the pack looking authoritative and doing nothing.
            const bool openSideMonitor = (m->shape == Shape::Floor)   ? n.monitorHi.has_value()
                                                                      : n.monitorLo.has_value();
            if (openSideMonitor)
                err(QStringLiteral("normShapeMonitor"), key,
                    QStringLiteral("Measure '%1' is '%2', so norm '%3' cannot carry a %4 bound — "
                                   "that tail is open and nothing grades against it.")
                        .arg(m->id, shapeLabel(m->shape), key,
                             m->shape == Shape::Floor ? QStringLiteral("monitorHi")
                                                      : QStringLiteral("monitorLo")));
        }
    }

    return rep;
}

// ── Persistence ─────────────────────────────────────────────────────────────

namespace {

void readOptionalDouble(const QJsonObject &o, const QString &key, std::optional<double> &out)
{
    const QJsonValue v = o.value(key);
    if (v.isDouble())
        out = v.toDouble();
}

Norm readNorm(const QJsonObject &o)
{
    Norm n;
    n.measureId = o.value(QStringLiteral("measure")).toString();
    n.contextId = o.value(QStringLiteral("context")).toString();
    n.mu        = o.value(QStringLiteral("mu")).toDouble();

    // sigmaHi defaults to sigmaLo when absent, so a symmetric norm is written once rather than
    // twice. Asymmetry is opt-in; symmetry stays terse.
    n.sigmaLo = o.value(QStringLiteral("sigmaLo")).toDouble();
    n.sigmaHi = o.contains(QStringLiteral("sigmaHi"))
                    ? o.value(QStringLiteral("sigmaHi")).toDouble()
                    : n.sigmaLo;

    readOptionalDouble(o, QStringLiteral("monitorLo"), n.monitorLo);
    readOptionalDouble(o, QStringLiteral("monitorHi"), n.monitorHi);

    n.n    = o.value(QStringLiteral("n")).toInt();
    n.unit = o.value(QStringLiteral("unit")).toString();
    if (!normSourceFromName(o.value(QStringLiteral("source")).toString(), n.source))
        n.source = NormSource::Heuristic;
    n.author   = o.value(QStringLiteral("author")).toString();
    n.citation = o.value(QStringLiteral("citation")).toString();

    const QString setOn = o.value(QStringLiteral("setOn")).toString();
    if (!setOn.isEmpty())
        n.setOn = QDate::fromString(setOn, Qt::ISODate);

    // The shipped values this override was made against. Written only on a user row; a shipped set
    // carrying one would be claiming to override itself. Read unconditionally so a hand-edited file
    // round-trips, and validated only in the sense that a partial object is taken as absent —
    // half a base cannot answer the question it exists to answer.
    const QJsonValue basedOn = o.value(QStringLiteral("basedOn"));
    if (basedOn.isObject()) {
        const QJsonObject b = basedOn.toObject();
        if (b.contains(QStringLiteral("mu")) && b.contains(QStringLiteral("sigmaLo"))) {
            NormBasis basis;
            basis.mu      = b.value(QStringLiteral("mu")).toDouble();
            basis.sigmaLo = b.value(QStringLiteral("sigmaLo")).toDouble();
            basis.sigmaHi = b.contains(QStringLiteral("sigmaHi"))
                                ? b.value(QStringLiteral("sigmaHi")).toDouble()
                                : basis.sigmaLo;
            readOptionalDouble(b, QStringLiteral("monitorLo"), basis.monitorLo);
            readOptionalDouble(b, QStringLiteral("monitorHi"), basis.monitorHi);
            n.basedOn = basis;
        }
    }

    return n;
}

QJsonObject writeNorm(const Norm &n)
{
    QJsonObject o;
    o.insert(QStringLiteral("measure"), n.measureId);
    o.insert(QStringLiteral("context"), n.contextId);
    o.insert(QStringLiteral("mu"),      n.mu);
    o.insert(QStringLiteral("sigmaLo"), n.sigmaLo);
    if (n.sigmaHi != n.sigmaLo)
        o.insert(QStringLiteral("sigmaHi"), n.sigmaHi);
    if (n.monitorLo.has_value()) o.insert(QStringLiteral("monitorLo"), *n.monitorLo);
    if (n.monitorHi.has_value()) o.insert(QStringLiteral("monitorHi"), *n.monitorHi);
    if (n.n != 0)                o.insert(QStringLiteral("n"), n.n);
    if (!n.unit.isEmpty())       o.insert(QStringLiteral("unit"), n.unit);
    o.insert(QStringLiteral("source"), normSourceName(n.source));
    if (!n.author.isEmpty())     o.insert(QStringLiteral("author"), n.author);
    if (!n.citation.isEmpty())   o.insert(QStringLiteral("citation"), n.citation);
    if (n.setOn.isValid())       o.insert(QStringLiteral("setOn"), n.setOn.toString(Qt::ISODate));

    if (n.basedOn.has_value()) {
        const NormBasis &b = *n.basedOn;
        QJsonObject bo;
        bo.insert(QStringLiteral("mu"),      b.mu);
        bo.insert(QStringLiteral("sigmaLo"), b.sigmaLo);
        if (b.sigmaHi != b.sigmaLo) bo.insert(QStringLiteral("sigmaHi"), b.sigmaHi);
        if (b.monitorLo.has_value()) bo.insert(QStringLiteral("monitorLo"), *b.monitorLo);
        if (b.monitorHi.has_value()) bo.insert(QStringLiteral("monitorHi"), *b.monitorHi);
        o.insert(QStringLiteral("basedOn"), bo);
    }
    return o;
}

NormPackLoadResult loadFrom(const QJsonObject &root, const QString &sourceLabel)
{
    NormPackLoadResult out;

    const int schema = root.value(QStringLiteral("schemaVersion")).toInt(kNormPackSchemaVersion);
    if (schema > kNormPackSchemaVersion) {
        // Refused rather than partially read: dropping fields this build does not understand would
        // let a newer set round-trip through an older build and lose content silently.
        out.report.issues.push_back(ValidationIssue{
            IssueSeverity::Error, QStringLiteral("schemaTooNew"), sourceLabel,
            QStringLiteral("Norm set '%1' declares schema %2; this build understands %3.")
                .arg(sourceLabel.isEmpty() ? QStringLiteral("(unnamed)") : sourceLabel)
                .arg(schema).arg(kNormPackSchemaVersion) });
        return out;
    }

    out.pack.id            = root.value(QStringLiteral("id")).toString();
    out.pack.version       = root.value(QStringLiteral("version")).toString();
    out.pack.schemaVersion = schema;
    out.pack.sourceLabel   = sourceLabel;

    const QJsonArray arr = root.value(QStringLiteral("norms")).toArray();
    out.pack.norms.reserve(size_t(arr.size()));
    for (const QJsonValue &v : arr)
        out.pack.norms.push_back(readNorm(v.toObject()));

    out.parsed = true;
    out.report = validateNormPack(out.pack);
    out.loaded = out.report.ok();
    return out;
}

} // namespace

NormPackLoadResult loadNormPack(const QJsonObject &root, const QString &sourceLabel)
{
    return loadFrom(root, sourceLabel);
}

NormPackLoadResult loadNormPack(const QByteArray &json, const QString &sourceLabel)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        NormPackLoadResult out;
        out.report.issues.push_back(ValidationIssue{
            IssueSeverity::Error, QStringLiteral("badNormFile"), sourceLabel,
            QStringLiteral("Could not parse %1: %2")
                .arg(sourceLabel.isEmpty() ? QStringLiteral("the norm set") : sourceLabel,
                     perr.errorString()) });
        return out;
    }
    return loadFrom(doc.object(), sourceLabel);
}

QJsonObject saveNormPack(const NormPack &pack)
{
    QJsonArray arr;
    for (const Norm &n : pack.norms)
        arr.append(writeNorm(n));

    QJsonObject root;
    root.insert(QStringLiteral("id"),            pack.id);
    root.insert(QStringLiteral("version"),       pack.version);
    root.insert(QStringLiteral("schemaVersion"), pack.schemaVersion);
    root.insert(QStringLiteral("norms"),         arr);
    return root;
}

} // namespace pinpoint::analysis
