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

#include "norm.h"

#include "enum_table_p.h"

#include <QCoreApplication>
#include <QObject>

namespace pinpoint::analysis {

namespace {

// The Row/nameOf/labelOf/fromName/tokenList machinery is shared with characteristic.cpp — see
// enum_table_p.h, which carries this file's reason for decoding a label as UTF-8 and a token as
// Latin-1. What stays here is the tables: one per enum, so a spelling exists in exactly one place.
// Every `name` is a stable JSON token and must never change once a norm set has shipped with it.
using detail::fromName;
using detail::labelOf;
using detail::nameOf;
using detail::Row;
using detail::tokenList;

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

} // namespace pinpoint::analysis
