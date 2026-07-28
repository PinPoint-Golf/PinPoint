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

// fromUtf8 on the LABEL and fromLatin1 on the token, and the asymmetry is not an oversight. A token
// is ASCII by contract — it is a JSON key that must never change. A label is prose, and the age-band
// labels carry an en dash ("18–54"); read as Latin-1 that becomes three mojibake characters on
// screen, with nothing failing anywhere to say so.
template <typename E, size_t N>
QString labelOf(const Row<E> (&rows)[N], E v)
{
    for (const auto &r : rows)
        if (r.value == v) return QString::fromUtf8(r.label);
    return QString::fromUtf8(rows[0].label);
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

// The cohort vocabulary. Both token columns are part of the FILE FORMAT and must never change once a
// norm set has shipped with them.
const Row<Sex> kSexes[] = {
    { Sex::Male,   "male",   "men" },
    { Sex::Female, "female", "women" },
};

// The labels are plain age ranges rather than words like "adults", because they are read BOTH alone
// ("55–64") and after a sex ("men 55–64"), and "men adults" does not survive the second reading.
const Row<AgeBand> kAgeBands[] = {
    { AgeBand::Junior,      "junior",       "under 18" },
    { AgeBand::Adult,       "adult",        "18+" },
    { AgeBand::Adult18_54,  "adult_18_54",  "18–54" },
    { AgeBand::Adult55_64,  "adult_55_64",  "55–64" },
    { AgeBand::Adult65Plus, "adult_65plus", "65+" },
};

template <typename E, size_t N>
QString tokenList(const Row<E> (&rows)[N])
{
    QStringList out;
    for (const auto &r : rows) out << QString::fromLatin1(r.name);
    return out.join(QStringLiteral(", "));
}

} // namespace

QString normSourceName(NormSource s) { return nameOf(kNormSources, s); }
bool    normSourceFromName(const QString &s, NormSource &out) { return fromName(kNormSources, s, out); }
QString normSourceLabel(NormSource s) { return labelOf(kNormSources, s); }

QString gradeName(Grade g) { return nameOf(kGrades, g); }
bool    gradeFromName(const QString &s, Grade &out) { return fromName(kGrades, s, out); }
QString gradeLabel(Grade g) { return labelOf(kGrades, g); }

QString sexName(Sex s) { return nameOf(kSexes, s); }
bool    sexFromName(const QString &s, Sex &out) { return fromName(kSexes, s, out); }
QString sexLabel(Sex s) { return labelOf(kSexes, s); }

QString ageBandName(AgeBand b) { return nameOf(kAgeBands, b); }
bool    ageBandFromName(const QString &s, AgeBand &out) { return fromName(kAgeBands, s, out); }
QString ageBandLabel(AgeBand b) { return labelOf(kAgeBands, b); }

QString sexTokenList()     { return tokenList(kSexes); }
QString ageBandTokenList() { return tokenList(kAgeBands); }

QString cohortLabel(const Cohort &c)
{
    // Empty for the unqualified cohort, deliberately — see the declaration.
    if (c.isUnqualified()) return QString();
    if (c.sex.has_value() && c.age.has_value())
        return QObject::tr("%1 %2").arg(sexLabel(*c.sex), ageBandLabel(*c.age));
    return c.sex.has_value() ? sexLabel(*c.sex) : ageBandLabel(*c.age);
}

std::vector<Cohort> cohortProbeOrder(const Cohort &athlete)
{
    std::vector<Cohort> out;

    // Never probe one key twice. An athlete whose band is `Adult` — which a date of birth cannot
    // produce, but a caller can pass — would otherwise collide probes 1 with 2 and 3 with 4, and a
    // repeated probe is a second linear scan for an answer the first one already refused.
    const auto push = [&out](std::optional<Sex> s, std::optional<AgeBand> a) {
        Cohort c;
        c.sex = s;
        c.age = a;
        for (const Cohort &e : out)
            if (e == c) return;
        out.push_back(c);
    };

    const bool haveSex = athlete.sex.has_value();
    // The `adult` parent is probed only for an athlete who is IN one of its sub-bands. A junior must
    // never match it: an 18+ corridor describes a population a 14-year-old is not part of, and
    // grading them against it would be a wrong answer wearing a right answer's clothes.
    const bool underAdult = athlete.age.has_value() && ageBandIsAdultSubBand(*athlete.age);

    if (haveSex && athlete.age.has_value()) push(athlete.sex, athlete.age);        // 1
    if (haveSex && underAdult)              push(athlete.sex, AgeBand::Adult);     // 2
    if (athlete.age.has_value())            push(std::nullopt, athlete.age);       // 3
    if (underAdult)                         push(std::nullopt, AgeBand::Adult);    // 4
    if (haveSex)                            push(athlete.sex, std::nullopt);       // 5
    push(std::nullopt, std::nullopt);                                              // 6

    return out;
}

std::optional<AgeBand> ageBandFor(const QDate &dob, const QDate &on)
{
    if (!dob.isValid() || !on.isValid())
        return std::nullopt;

    // Whole years completed, birthday-aware. addYears() clamps 29 February to the 28th in a
    // non-leap year, which is the ordinary legal reading of a leap-day birthday and is the one
    // behaviour here that is worth not reinventing.
    int age = on.year() - dob.year();
    if (on < dob.addYears(age))
        --age;

    if (age < 0)
        return std::nullopt;        // born after the swing: nonsense, and not a junior

    if (age < 18) return AgeBand::Junior;
    if (age < 55) return AgeBand::Adult18_54;
    if (age < 65) return AgeBand::Adult55_64;
    return AgeBand::Adult65Plus;
}

Cohort cohortFor(const QDate &dob, const QString &sexToken, const QDate &on)
{
    Cohort c;

    Sex s{};
    if (sexFromName(sexToken, s))
        c.sex = s;                  // anything else — empty, "declined", unknown — leaves it unset

    c.age = ageBandFor(dob, on);
    return c;
}

// ── Saying what a corridor is, in words ─────────────────────────────────────

QString normNumber(double v)
{
    if (!std::isfinite(v)) return QStringLiteral("—");
    for (int dp = 1; dp <= 4; ++dp) {
        const QString s = QString::number(v, 'f', dp);
        if (std::fabs(s.toDouble() - v) < 1e-9) return s;
    }
    return QString::number(v, 'f', 4);
}

QString rangePhrase(double lo, double hi, double mu, Shape shape)
{
    switch (shape) {
    case Shape::Floor:   return QObject::tr("at least %1").arg(normNumber(mu));
    case Shape::Ceiling: return QObject::tr("no more than %1").arg(normNumber(mu));
    case Shape::Target:  break;
    }
    return QObject::tr("%1 to %2").arg(normNumber(lo), normNumber(hi));
}

QString actionPhrase(double watchLo, double watchHi, Shape shape)
{
    switch (shape) {
    case Shape::Floor:   return QObject::tr("action below %1").arg(normNumber(watchLo));
    case Shape::Ceiling: return QObject::tr("action above %1").arg(normNumber(watchHi));
    case Shape::Target:  break;
    }
    return QObject::tr("action beyond %1 to %2").arg(normNumber(watchLo), normNumber(watchHi));
}

QString implausibleLabel() { return QObject::tr("Not believed"); }

QString implausibleNote(double value, const QString &unit)
{
    // The reading is the first thing in the sentence. A capture fault is diagnosed by LOOKING at
    // the number — 1.62 says "mis-tracked ball" to anyone who knows the measure — so a message
    // that withholds it leaves the reader with nothing to act on.
    const QString shown = unit.isEmpty() ? normNumber(value)
                                         : QObject::tr("%1 %2").arg(normNumber(value), unit);
    return QObject::tr("%1 is outside the plausible range for this measure, so it was not graded. "
                       "Check the capture rather than the swing.").arg(shown);
}

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

const Norm *NormPack::find(const QString &measureId, const QString &contextId,
                           const Cohort &cohort) const
{
    for (const Norm &n : norms)
        if (n.measureId == measureId && n.contextId == contextId && n.cohort == cohort) return &n;
    return nullptr;
}

bool NormPack::contains(const QString &measureId, const QString &contextId,
                        const Cohort &cohort) const
{
    return find(measureId, contextId, cohort) != nullptr;
}

QStringList NormPack::contextsFor(const QString &measureId) const
{
    QStringList out;
    for (const Norm &n : norms)
        if (n.measureId == measureId && !out.contains(n.contextId)) out.append(n.contextId);
    return out;
}

std::vector<Cohort> NormPack::cohortsFor(const QString &measureId, const QString &contextId) const
{
    std::vector<Cohort> out;
    for (const Norm &n : norms) {
        if (n.measureId != measureId || n.contextId != contextId) continue;
        if (n.cohort.isUnqualified()) continue;      // the row every surface already shows
        out.push_back(n.cohort);
    }
    return out;
}

void NormPack::upsert(const Norm &norm)
{
    for (Norm &n : norms) {
        if (n.measureId == norm.measureId && n.contextId == norm.contextId
            && n.cohort == norm.cohort) { n = norm; return; }
    }
    norms.push_back(norm);
}

bool NormPack::remove(const QString &measureId, const QString &contextId, const Cohort &cohort)
{
    const auto it = std::find_if(norms.begin(), norms.end(), [&](const Norm &n) {
        return n.measureId == measureId && n.contextId == contextId && n.cohort == cohort;
    });
    if (it == norms.end())
        return false;
    norms.erase(it);
    return true;
}

// ── The norm key, as one string ─────────────────────────────────────────────

namespace {
// The cohort term is fenced off by a separator no id can contain: context ids are lowercase tokens
// (`driver`, `full_swing`) and measure ids are `m_*`, so neither an '@' nor a ' · ' can appear
// inside one. That is what lets splitNormKey() be an exact inverse rather than a heuristic.
//
// A function rather than a QLatin1String constant, because the separator is not ASCII — as Latin-1
// its two UTF-8 bytes would read back as two characters, and the key would still round-trip through
// splitNormKey while showing mojibake on every health row.
QString cohortSep() { return QStringLiteral(" · "); }
} // namespace

QString normKeyLabel(const QString &measureId, const QString &contextId, const Cohort &cohort)
{
    const QString base = QStringLiteral("%1 @ %2").arg(measureId, contextId);
    const QString who  = cohortLabel(cohort);
    return who.isEmpty() ? base : base + cohortSep() + who;
}

QString normKeyLabel(const Norm &n) { return normKeyLabel(n.measureId, n.contextId, n.cohort); }

void splitNormKey(const QString &key, QString &measureId, QString &contextId)
{
    measureId.clear();
    contextId.clear();

    const int at = key.indexOf(QLatin1Char('@'));
    if (at <= 0)
        return;

    measureId = key.left(at).trimmed();

    QString rest = key.mid(at + 1);
    const int sep = rest.indexOf(cohortSep());
    if (sep >= 0)
        rest = rest.left(sep);
    contextId = rest.trimmed();
}

// ── A cohort across the C++/QML boundary ────────────────────────────────────

bool cohortFromMap(const QVariantMap &map, Cohort &out)
{
    Cohort c;

    const QString sex = map.value(QStringLiteral("sex")).toString();
    if (!sex.isEmpty()) {
        Sex s{};
        if (!sexFromName(sex, s)) return false;
        c.sex = s;
    }

    const QString age = map.value(QStringLiteral("age")).toString();
    if (!age.isEmpty()) {
        AgeBand b{};
        if (!ageBandFromName(age, b)) return false;
        c.age = b;
    }

    out = c;
    return true;
}

QVariantMap cohortToMap(const Cohort &cohort)
{
    QVariantMap m;
    // Absent rather than empty-string, so the map round-trips through cohortFromMap and reads the
    // same way the JSON does: an unset axis is a key that is not there.
    if (cohort.sex.has_value()) m.insert(QStringLiteral("sex"), sexName(*cohort.sex));
    if (cohort.age.has_value()) m.insert(QStringLiteral("age"), ageBandName(*cohort.age));
    return m;
}

// ── Validation ──────────────────────────────────────────────────────────────

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

        // The key carries the cohort, so a male row and a female row at one (measure, context) are
        // two rows and not a duplicate — that is the point of the axis. Two rows on the SAME cohort
        // still collide, and unqualified is a cohort like any other.
        if (seen.contains(key))
            err(QStringLiteral("duplicateNorm"), key,
                QStringLiteral("Two norms share the key '%1'.").arg(key));
        seen.insert(key);

        if (n.sigmaLo < 0.0 || n.sigmaHi < 0.0)
            err(QStringLiteral("negativeSigma"), key,
                QStringLiteral("Norm '%1' has a negative tolerance.").arg(key));

        // `partialMonitor` used to live here and had to move to validateNormsAgainst: one side of a
        // monitor band is COMPLETE on a one-sided measure, and this validator cannot see the
        // measure. Deciding it here would refuse the correct authoring of every floor and ceiling.

        // Plausibility is a claim about capture, not about grading, so it needs no shape to check.
        if (n.plausibleLo.has_value() && n.plausibleHi.has_value()
            && *n.plausibleLo > *n.plausibleHi)
            err(QStringLiteral("plausibleOrder"), key,
                QStringLiteral("Norm '%1' has plausibleLo above plausibleHi.").arg(key));

        // Order needs BOTH bounds to mean anything, so this is the one monitor rule that can stay
        // here: a one-sided row may legally carry only one, and one bound cannot be out of order
        // with itself. `monitorExcludesIdeal` MOVED to validateNormsAgainst — it has to know which
        // tails grade, and deciding it here refused nothing and checked nothing on a one-sided row.
        if (n.monitorLo.has_value() && n.monitorHi.has_value() && *n.monitorLo > *n.monitorHi)
            err(QStringLiteral("monitorOrder"), key,
                QStringLiteral("Norm '%1' has monitorLo above monitorHi.").arg(key));

        if (!(n.sigmaLo > 0.0) || !(n.sigmaHi > 0.0)) {
            // Which case it is, because they are different mistakes. Both zero is a row that
            // grades everything but its exact centre as a fault; one zero is an asymmetry that may
            // well be deliberate on a target norm and is impossible on a one-sided one, where the
            // two are held equal.
            const bool both = !(n.sigmaLo > 0.0) && !(n.sigmaHi > 0.0);
            warn(QStringLiteral("zeroSigma"), key,
                 both ? QStringLiteral("Norm '%1' has no tolerance at all, so only its exact centre "
                                       "grades Ideal.").arg(key)
                      : QStringLiteral("Norm '%1' has no tolerance on one side, so on that side "
                                       "only its exact centre grades Ideal.").arg(key));
        }

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
        } else if (n.monitorLo.has_value() != n.monitorHi.has_value()) {
            // MOVED here from validateNormPack, which cannot see the measure. On a TARGET both
            // sides grade, so half a monitor band is half a rule: hasExplicitMonitor() answers
            // false, grade() falls back to the z-derived edge, and the bound the author wrote sits
            // in the file doing nothing. On a one-sided measure the same row is correct and
            // complete, which is why the check could not stay where it was.
            err(QStringLiteral("partialMonitor"), key,
                QStringLiteral("Norm '%1' sets only one side of its monitor band, and measure '%2' "
                               "grades both tails; set both or neither.").arg(key, m->id));
        }

        // ── The monitor band must contain the norm's own tolerance ──────────
        //
        // MOVED here from validateNormPack, for the reason partialMonitor was: the standalone
        // validator cannot see the measure, so `hasExplicitMonitor()` there answers for a Target
        // and a legal one-sided monitor — one bound, on the graded tail — reads as no monitor at
        // all. The check then silently did not run on exactly the rows shapes introduced.
        //
        // The band bounds the Watch/Action edge, so it MUST contain the tolerance. If it does not,
        // a value sitting inside its own tolerance grades Action, which reads as a detection and
        // is a data-entry error. Against the norm's own CLAIM and never a policy's Ideal band:
        // validation has no policy in hand, and a row that failed under `strict` but passed under
        // `lenient` would make a shared pack's validity depend on the reader's settings.
        // Naming the two EDGES rather than the two bands, because a band phrased one-sided cannot
        // express the comparison: "a monitor band of at least 1.5 does not contain a tolerance of
        // at least 1.48" reads as false on its face, when what is wrong is that the fault edge
        // (1.5) has been set inside the tolerance (down to 1.43).
        if (n.hasExplicitMonitor(m->shape)) {
            const bool loBad = m->shape != Shape::Ceiling && n.monitorLo.has_value()
                               && *n.monitorLo > n.claimLo();
            const bool hiBad = m->shape != Shape::Floor && n.monitorHi.has_value()
                               && *n.monitorHi < n.claimHi();
            if (loBad && hiBad)
                err(QStringLiteral("monitorExcludesIdeal"), key,
                    QStringLiteral("Norm '%1' calls readings a fault outside %2..%3, but its own "
                                   "tolerance runs %4..%5 — a value inside its own tolerance would "
                                   "grade Action.")
                        .arg(key).arg(*n.monitorLo).arg(*n.monitorHi)
                        .arg(n.claimLo()).arg(n.claimHi()));
            else if (loBad)
                err(QStringLiteral("monitorExcludesIdeal"), key,
                    QStringLiteral("Norm '%1' calls readings a fault below %2, but its own "
                                   "tolerance reaches down to %3 — a value inside its own "
                                   "tolerance would grade Action.")
                        .arg(key, normNumber(*n.monitorLo), normNumber(n.claimLo())));
            else if (hiBad)
                err(QStringLiteral("monitorExcludesIdeal"), key,
                    QStringLiteral("Norm '%1' calls readings a fault above %2, but its own "
                                   "tolerance reaches up to %3 — a value inside its own "
                                   "tolerance would grade Action.")
                        .arg(key, normNumber(*n.monitorHi), normNumber(n.claimHi())));
        }

        // ── Plausibility against the corridor ───────────────────────────────
        //
        // A corridor must never extend into territory the norm does not believe. Where a plausible
        // bound and a Watch edge both exist on a side, the bound must lie at or outside the edge —
        // otherwise a reading would be graded Action and disbelieved at the same time, and which
        // answer surfaced would depend on the order the two checks happened to run in.
        //
        // Measured against the WIDEST shipped preset, or against an explicit monitor bound where
        // one exists. Validation has no policy in hand, and a row that loaded under `standard` and
        // failed under `lenient` would make a shared pack's validity depend on the reader's
        // settings.
        {
            const GradePolicy widest = gradePolicyByName(QStringLiteral("lenient"));
            const NormBandEdges e = bandEdgesOf(n, widest, -1.0, m->shape);

            if (n.plausibleLo.has_value() && !e.lowOpen && *n.plausibleLo > e.watchLo)
                err(QStringLiteral("plausibleInsideCorridor"), key,
                    QStringLiteral("Norm '%1' stops believing readings below %2, but its own "
                                   "corridor reaches down to %3. A corridor cannot extend into "
                                   "implausible territory.")
                        .arg(key).arg(*n.plausibleLo).arg(e.watchLo));

            if (n.plausibleHi.has_value() && !e.highOpen && *n.plausibleHi < e.watchHi)
                err(QStringLiteral("plausibleInsideCorridor"), key,
                    QStringLiteral("Norm '%1' stops believing readings above %2, but its own "
                                   "corridor reaches up to %3. A corridor cannot extend into "
                                   "implausible territory.")
                        .arg(key).arg(*n.plausibleHi).arg(e.watchHi));
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

// Absent ⇒ unqualified, which is what every row authored before cohorts existed means.
//
// An unknown token DROPS THE ROW rather than falling back, and returns false to say so. The
// asymmetry with `unknownShape` — which leaves the measure at Target and keeps it — is deliberate
// and is about which way the default is wrong: Target grades both tails, so a misread shape is
// conservative, while unqualified matches EVERYONE, so a misread cohort would apply one segment's
// numbers to the whole population. A row nobody can place is a row nobody should be graded by.
bool readCohort(const QJsonObject &o, const QString &key, Cohort &out, ValidationReport &rep)
{
    const QJsonValue v = o.value(QStringLiteral("cohort"));
    if (!v.isObject())
        return true;

    const QJsonObject c = v.toObject();
    auto err = [&rep, &key](const QString &message) {
        rep.issues.push_back(ValidationIssue{ IssueSeverity::Error,
                                              QStringLiteral("unknownCohort"), key, message });
    };

    if (c.contains(QStringLiteral("sex"))) {
        const QString token = c.value(QStringLiteral("sex")).toString();
        Sex           s{};
        if (!sexFromName(token, s)) {
            err(QStringLiteral("Norm '%1' declares sex '%2'; the values are %3. The row was dropped "
                               "— a corridor nobody can place would otherwise grade everyone.")
                    .arg(key, token, sexTokenList()));
            return false;
        }
        out.sex = s;
    }

    if (c.contains(QStringLiteral("age"))) {
        const QString token = c.value(QStringLiteral("age")).toString();
        AgeBand       b{};
        if (!ageBandFromName(token, b)) {
            err(QStringLiteral("Norm '%1' declares age band '%2'; the bands are %3. The row was "
                               "dropped — a corridor nobody can place would otherwise grade "
                               "everyone.")
                    .arg(key, token, ageBandTokenList()));
            return false;
        }
        out.age = b;
    }

    return true;
}

std::optional<Norm> readNorm(const QJsonObject &o, ValidationReport &rep)
{
    Norm n;
    n.measureId = o.value(QStringLiteral("measure")).toString();
    n.contextId = o.value(QStringLiteral("context")).toString();
    if (!readCohort(o, normKeyLabel(n.measureId, n.contextId), n.cohort, rep))
        return std::nullopt;
    n.mu        = o.value(QStringLiteral("mu")).toDouble();

    // sigmaHi defaults to sigmaLo when absent, so a symmetric norm is written once rather than
    // twice. Asymmetry is opt-in; symmetry stays terse.
    n.sigmaLo = o.value(QStringLiteral("sigmaLo")).toDouble();
    n.sigmaHi = o.contains(QStringLiteral("sigmaHi"))
                    ? o.value(QStringLiteral("sigmaHi")).toDouble()
                    : n.sigmaLo;

    readOptionalDouble(o, QStringLiteral("monitorLo"), n.monitorLo);
    readOptionalDouble(o, QStringLiteral("monitorHi"), n.monitorHi);
    readOptionalDouble(o, QStringLiteral("plausibleLo"), n.plausibleLo);
    readOptionalDouble(o, QStringLiteral("plausibleHi"), n.plausibleHi);

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
            readOptionalDouble(b, QStringLiteral("plausibleLo"), basis.plausibleLo);
            readOptionalDouble(b, QStringLiteral("plausibleHi"), basis.plausibleHi);
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
    // Omitted entirely when unqualified, so every row authored before cohorts existed round-trips
    // byte-identically and the shipped set's diff stays empty. Each field is written only when it is
    // set: "sex only" and "sex plus a band" are different keys, and an unset axis written as null
    // would be a third spelling of the same absence.
    if (!n.cohort.isUnqualified()) {
        QJsonObject co;
        if (n.cohort.sex.has_value()) co.insert(QStringLiteral("sex"), sexName(*n.cohort.sex));
        if (n.cohort.age.has_value()) co.insert(QStringLiteral("age"), ageBandName(*n.cohort.age));
        o.insert(QStringLiteral("cohort"), co);
    }
    o.insert(QStringLiteral("mu"),      n.mu);
    o.insert(QStringLiteral("sigmaLo"), n.sigmaLo);
    if (n.sigmaHi != n.sigmaLo)
        o.insert(QStringLiteral("sigmaHi"), n.sigmaHi);
    if (n.monitorLo.has_value()) o.insert(QStringLiteral("monitorLo"), *n.monitorLo);
    if (n.monitorHi.has_value()) o.insert(QStringLiteral("monitorHi"), *n.monitorHi);
    // Singly is legal: a floor caps above and says nothing below.
    if (n.plausibleLo.has_value()) o.insert(QStringLiteral("plausibleLo"), *n.plausibleLo);
    if (n.plausibleHi.has_value()) o.insert(QStringLiteral("plausibleHi"), *n.plausibleHi);
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
        if (b.plausibleLo.has_value()) bo.insert(QStringLiteral("plausibleLo"), *b.plausibleLo);
        if (b.plausibleHi.has_value()) bo.insert(QStringLiteral("plausibleHi"), *b.plausibleHi);
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
    // The parse report is built FIRST and the validator's findings appended to it, so a row dropped
    // for an unreadable cohort is reported even though the set that survives validates clean. A
    // report that lost the reason for the drop would leave a corridor silently missing.
    ValidationReport parseReport;
    for (const QJsonValue &v : arr) {
        if (std::optional<Norm> n = readNorm(v.toObject(), parseReport))
            out.pack.norms.push_back(std::move(*n));
    }

    out.parsed = true;
    out.report = parseReport;
    for (const ValidationIssue &i : validateNormPack(out.pack).issues)
        out.report.issues.push_back(i);
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

int requiredNormSchemaVersion(const NormPack &pack)
{
    for (const Norm &n : pack.norms)
        if (!n.cohort.isUnqualified()) return 2;
    return 1;
}

QJsonObject saveNormPack(const NormPack &pack)
{
    QJsonArray arr;
    for (const Norm &n : pack.norms)
        arr.append(writeNorm(n));

    QJsonObject root;
    root.insert(QStringLiteral("id"),      pack.id);
    root.insert(QStringLiteral("version"), pack.version);
    // What the CONTENT needs, not what the pack was loaded as and not this build's ceiling — see
    // requiredNormSchemaVersion(). `pack.schemaVersion` records what was declared on the way in and
    // is deliberately not what goes back out.
    root.insert(QStringLiteral("schemaVersion"), requiredNormSchemaVersion(pack));
    root.insert(QStringLiteral("norms"),         arr);
    return root;
}

} // namespace pinpoint::analysis
