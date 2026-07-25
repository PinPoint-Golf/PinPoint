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

#include "athlete_controller.h"

#include "pp_settings.h"
#include "club_vocabulary.h"
#include <QUuid>
#include <QDateTime>
#include <algorithm>

static constexpr auto kGroup         = "athletes";
static constexpr auto kCurrentUuid   = "currentAthleteUuid";
static constexpr auto kName          = "name";
static constexpr auto kHandedness    = "handedness";
static constexpr auto kHeightValue   = "heightValue";
static constexpr auto kHeightUnit    = "heightUnit";
static constexpr auto kWeightValue   = "weightValue";
static constexpr auto kWeightUnit    = "weightUnit";
static constexpr auto kHandicap      = "handicap";
static constexpr auto kPrimaryClub   = "primaryClub";
static constexpr auto kSpeedTarget   = "speedTarget";
static constexpr auto kNotes         = "notes";
static constexpr auto kCreatedAt     = "createdAt";
static constexpr auto kLastSessionAt = "lastSessionAt";
static constexpr auto kSessionCount  = "sessionCount";
static constexpr auto kClubs         = "clubs";

// Defined below clubsFor(): factory per-club defaults + the standard seeded bag.
static QVariantMap defaultClubRecordFor(const QString &clubId);
static const QStringList &defaultBag();

// Map a stored primaryClub token to the canonical vocabulary (club_vocabulary.h).
// Handles the legacy non-canonical picker values ("Driver", "3-wood", "5-iron",
// "7-iron", "Wedge") so old data self-heals on read; canonical values pass
// through. Empty in / empty out. Unknown tokens are returned uppercased (they
// simply won't match a bag key, so effectivePrimaryClub falls back).
static QString normalizeClubId(const QString &raw)
{
    const QString t = raw.trimmed();
    if (t.isEmpty())
        return QString();

    const QStringList &vocab = pinpoint::clubVocabulary();
    if (vocab.contains(t))
        return t;                                  // already canonical

    QString up = t.toUpper();
    up.replace(u'-', u' ');
    up = up.simplified();                          // "3-wood" -> "3 WOOD"
    if (vocab.contains(up))
        return up;

    if (up == QLatin1String("WEDGE"))              // legacy catch-all wedge
        return QStringLiteral("SAND WEDGE");
    return up;                                      // best effort
}


AthleteController::AthleteController(QObject *parent)
    : QObject(parent)
{
    reload();
}

QString AthleteController::computeInitials(const QString &name)
{
    const QStringList words = name.trimmed().split(u' ', Qt::SkipEmptyParts);
    if (words.size() >= 2)
        return QString(words.first()[0]).toUpper() + QString(words.last()[0]).toUpper();
    if (!words.isEmpty() && words.first().length() >= 2)
        return words.first().left(2).toUpper();
    if (!words.isEmpty())
        return words.first().left(1).toUpper();
    return QStringLiteral("?");
}

void AthleteController::reload()
{
    QSettings s = ppSettings();

    // Collect all athlete UUIDs
    s.beginGroup(kGroup);
    const QStringList uuids = s.childGroups();
    s.endGroup();

    QVariantList list;
    list.reserve(uuids.size());

    for (const QString &uuid : uuids) {
        s.beginGroup(kGroup);
        s.beginGroup(uuid);

        QVariantMap m;
        m[QStringLiteral("uuid")]          = uuid;
        m[QStringLiteral("name")]          = s.value(kName,         QString()).toString();
        m[QStringLiteral("handedness")]    = s.value(kHandedness,   QStringLiteral("Right")).toString();
        m[QStringLiteral("heightValue")]   = s.value(kHeightValue,  0.0).toDouble();
        m[QStringLiteral("heightUnit")]    = s.value(kHeightUnit,   QStringLiteral("ft")).toString();
        m[QStringLiteral("weightValue")]   = s.value(kWeightValue,  0.0).toDouble();
        m[QStringLiteral("weightUnit")]    = s.value(kWeightUnit,   QStringLiteral("lb")).toString();
        m[QStringLiteral("handicap")]      = s.value(kHandicap,     -999.0).toDouble();
        m[QStringLiteral("primaryClub")]   = normalizeClubId(s.value(kPrimaryClub, QStringLiteral("DRIVER")).toString());
        m[QStringLiteral("speedTarget")]   = s.value(kSpeedTarget,  0.0).toDouble();
        m[QStringLiteral("notes")]         = s.value(kNotes,        QString()).toString();
        m[QStringLiteral("createdAt")]     = s.value(kCreatedAt,    0LL).toLongLong();
        m[QStringLiteral("lastSessionAt")] = s.value(kLastSessionAt,0LL).toLongLong();
        m[QStringLiteral("sessionCount")]  = s.value(kSessionCount, 0).toInt();
        if (!s.contains(kClubs)) {
            // First sight of this athlete's bag: seed the standard set
            // (driver, 5-9 iron, GW, SW, putter) with factory defaults.
            // Written once — an intentionally emptied bag stays empty.
            QVariantMap bag;
            for (const QString &id : defaultBag())
                bag.insert(id, defaultClubRecordFor(id));
            s.setValue(kClubs, bag);
        }
        m[QStringLiteral("clubs")]         = s.value(kClubs,        QVariantMap{}).toMap();
        m[QStringLiteral("initials")]      = computeInitials(m[QStringLiteral("name")].toString());

        s.endGroup(); // uuid
        s.endGroup(); // athletes

        list.append(m);
    }

    // Sort: lastSessionAt desc, createdAt desc as tie-breaker
    std::sort(list.begin(), list.end(), [](const QVariant &a, const QVariant &b) {
        const auto ma = a.toMap();
        const auto mb = b.toMap();
        const qint64 aLast = ma[QStringLiteral("lastSessionAt")].toLongLong();
        const qint64 bLast = mb[QStringLiteral("lastSessionAt")].toLongLong();
        if (aLast != bLast) return aLast > bLast;
        return ma[QStringLiteral("createdAt")].toLongLong() > mb[QStringLiteral("createdAt")].toLongLong();
    });

    m_athletes = list;
    emit athletesChanged();

    // Resolve current athlete
    const QString wantedUuid = s.value(kCurrentUuid, QString()).toString();
    bool found = false;
    for (const QVariant &v : std::as_const(m_athletes)) {
        const auto m = v.toMap();
        if (m[QStringLiteral("uuid")].toString() == wantedUuid) {
            m_currentUuid       = wantedUuid;
            m_currentName       = m[QStringLiteral("name")].toString();
            m_currentInitials   = m[QStringLiteral("initials")].toString();
            m_currentHandedness = m[QStringLiteral("handedness")].toString();
            found = true;
            break;
        }
    }
    if (!found) {
        m_currentUuid       = {};
        m_currentName       = {};
        m_currentInitials   = {};
        m_currentHandedness = {};
    }

    emit currentAthleteChanged();
}

QString AthleteController::saveAthlete(
    const QString &uuid,
    const QString &name,
    const QString &handedness,
    double         heightValue,
    const QString &heightUnit,
    double         weightValue,
    const QString &weightUnit,
    double         handicap,
    const QString &primaryClub,
    double         speedTarget,
    const QString &notes)
{
    if (name.trimmed().isEmpty())
        return {};

    const bool creating = uuid.isEmpty();
    QString    targetUuid = uuid;

    QSettings s = ppSettings();

    if (creating) {
        targetUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    } else {
        s.beginGroup(kGroup);
        const bool exists = s.childGroups().contains(uuid);
        s.endGroup();
        if (!exists)
            return {};                       // unknown uuid -> no-op
    }

    // Convert to base unit before storing (always ft / lb).
    double storedHeight = heightValue;
    if (heightUnit == QLatin1String("cm") && heightValue > 0.0)
        storedHeight = heightValue / 30.48;

    double storedWeight = weightValue;
    if (weightUnit == QLatin1String("kg") && weightValue > 0.0)
        storedWeight = weightValue * 2.20462;

    s.beginGroup(kGroup);
    s.beginGroup(targetUuid);

    s.setValue(kName,        name.trimmed());
    s.setValue(kHandedness,  handedness);
    s.setValue(kHeightValue, storedHeight);
    s.setValue(kHeightUnit,  heightUnit);
    s.setValue(kWeightValue, storedWeight);
    s.setValue(kWeightUnit,  weightUnit);
    s.setValue(kHandicap,    handicap);
    s.setValue(kPrimaryClub, normalizeClubId(primaryClub));
    s.setValue(kSpeedTarget, speedTarget);
    s.setValue(kNotes,       notes);

    if (creating) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        s.setValue(kCreatedAt,     now);
        s.setValue(kLastSessionAt, 0LL);
        s.setValue(kSessionCount,  0);
    }
    // On update: createdAt / lastSessionAt / sessionCount are intentionally
    // left untouched.

    s.endGroup(); // uuid
    s.endGroup(); // athletes

    if (creating)
        s.setValue(kCurrentUuid, targetUuid);   // auto-select new athlete
    // On update: do NOT change the current selection.

    s.sync();
    reload();
    return targetUuid;
}

QString AthleteController::createAthlete(
    const QString &name,
    const QString &handedness,
    double         heightValue,
    const QString &heightUnit,
    double         weightValue,
    const QString &weightUnit,
    double         handicap,
    const QString &primaryClub,
    double         speedTarget,
    const QString &notes)
{
    return saveAthlete(QString(), name, handedness,
                       heightValue, heightUnit,
                       weightValue, weightUnit,
                       handicap, primaryClub, speedTarget, notes);
}

bool AthleteController::updateAthlete(const QString &uuid, const QString &fieldName, const QVariant &value)
{
    QSettings s = ppSettings();

    s.beginGroup(kGroup);
    const bool exists = s.childGroups().contains(uuid);
    s.endGroup();

    if (!exists)
        return false;

    s.beginGroup(kGroup);
    s.beginGroup(uuid);
    s.setValue(fieldName, value);
    s.endGroup();
    s.endGroup();
    s.sync();

    reload();
    return true;
}

// Factory defaults per vocabulary club — lofts/lengths guided by Titleist
// T100 irons, Vokey wedges (52/56/60 stock) and stock Titleist metals; club
// length converted to mm (1" = 25.4 mm). Bands default to untaped.
static QVariantMap defaultClubRecordFor(const QString &clubId)
{
    struct Spec { const char *id; double loft; int lengthMm; const char *shaft; };
    static const Spec kSpecs[] = {
        { "DRIVER",          10.0, 1156, "graphite" },   // 45.5"
        { "3 WOOD",          15.0, 1092, "graphite" },   // 43.0"
        { "5 WOOD",          18.0, 1080, "graphite" },   // 42.5"
        { "3 HYBRID",        21.0, 1035, "graphite" },   // 40.75"
        { "4 HYBRID",        24.0, 1016, "graphite" },   // 40.0"
        { "3 IRON",          21.0,  991, "steel"    },   // 39.0"
        { "4 IRON",          24.0,  978, "steel"    },   // 38.5"
        { "5 IRON",          27.0,  965, "steel"    },   // 38.0"
        { "6 IRON",          30.0,  953, "steel"    },   // 37.5"
        { "7 IRON",          34.0,  940, "steel"    },   // 37.0"
        { "8 IRON",          38.0,  927, "steel"    },   // 36.5"
        { "9 IRON",          42.0,  914, "steel"    },   // 36.0"
        { "PITCHING WEDGE",  46.0,  908, "steel"    },   // 35.75"
        { "GAP WEDGE",       52.0,  902, "steel"    },   // Vokey 52, 35.5"
        { "SAND WEDGE",      56.0,  895, "steel"    },   // Vokey 56, 35.25"
        { "LOB WEDGE",       60.0,  889, "steel"    },   // Vokey 60, 35.0"
        { "PUTTER",           3.0,  864, "steel"    },   // 34.0"
    };
    QVariantMap rec;
    rec[QStringLiteral("shaftType")]       = QStringLiteral("steel");
    rec[QStringLiteral("loftDeg")]         = 0.0;
    rec[QStringLiteral("lengthMm")]        = 0;
    rec[QStringLiteral("bandWidthMm")]     = 25;
    rec[QStringLiteral("bandCentersMm")]   = QVariantList{};
    rec[QStringLiteral("hoselFromButtMm")] = 0;
    rec[QStringLiteral("headPatch")]       = false;
    rec[QStringLiteral("tapedOn")]         = QString();
    rec[QStringLiteral("notes")]           = QString();
    for (const Spec &sp : kSpecs) {
        if (clubId == QLatin1String(sp.id)) {
            rec[QStringLiteral("loftDeg")]   = sp.loft;
            rec[QStringLiteral("lengthMm")]  = sp.lengthMm;
            rec[QStringLiteral("shaftType")] = QLatin1String(sp.shaft);
            break;
        }
    }
    return rec;
}

// The bag every golfer starts with; fairway woods / hybrids / specialty
// wedges are added by the user (with defaults from defaultClubRecordFor).
static const QStringList &defaultBag()
{
    static const QStringList kBag = {
        QStringLiteral("DRIVER"),
        QStringLiteral("5 IRON"), QStringLiteral("6 IRON"),
        QStringLiteral("7 IRON"), QStringLiteral("8 IRON"),
        QStringLiteral("9 IRON"),
        QStringLiteral("GAP WEDGE"), QStringLiteral("SAND WEDGE"),
        QStringLiteral("PUTTER"),
    };
    return kBag;
}

QVariantMap AthleteController::defaultClubRecord(const QString &clubId) const
{
    return defaultClubRecordFor(clubId);
}

QVariantMap AthleteController::clubsFor(const QString &uuid) const
{
    QSettings s = ppSettings();
    s.beginGroup(kGroup);
    s.beginGroup(uuid);
    const QVariantMap clubs = s.value(kClubs, QVariantMap{}).toMap();
    s.endGroup();
    s.endGroup();
    return clubs;
}

QStringList AthleteController::clubOptions() const
{
    return pinpoint::clubVocabulary();
}

QString AthleteController::effectivePrimaryClub(const QString &uuid) const
{
    const QVariantMap bag = clubsFor(uuid);

    QSettings s = ppSettings();
    s.beginGroup(kGroup);
    s.beginGroup(uuid);
    const QString stored = normalizeClubId(s.value(kPrimaryClub, QStringLiteral("DRIVER")).toString());
    s.endGroup();
    s.endGroup();

    if (!stored.isEmpty() && bag.contains(stored))
        return stored;
    if (bag.contains(QStringLiteral("DRIVER")))
        return QStringLiteral("DRIVER");
    // First configured club in canonical (bag) order.
    for (const QString &c : pinpoint::clubVocabulary())
        if (bag.contains(c))
            return c;
    return QString();
}

bool AthleteController::setClubRecord(const QString &uuid, const QString &clubId,
                                      const QVariantMap &record)
{
    if (uuid.isEmpty() || clubId.trimmed().isEmpty())
        return false;
    QVariantMap clubs = clubsFor(uuid);
    clubs.insert(clubId, record);
    return updateAthlete(uuid, QString::fromLatin1(kClubs), clubs);
}

bool AthleteController::removeClubRecord(const QString &uuid, const QString &clubId)
{
    QVariantMap clubs = clubsFor(uuid);
    if (clubs.remove(clubId) == 0)
        return false;
    return updateAthlete(uuid, QString::fromLatin1(kClubs), clubs);
}

bool AthleteController::deleteAthlete(const QString &uuid)
{
    QSettings s = ppSettings();

    s.beginGroup(kGroup);
    s.remove(uuid);
    s.endGroup();

    if (s.value(kCurrentUuid).toString() == uuid)
        s.remove(kCurrentUuid);

    s.sync();
    reload();
    return true;
}

void AthleteController::selectAthlete(const QString &uuid)
{
    QSettings s = ppSettings();
    s.setValue(kCurrentUuid, uuid);
    s.sync();
    reload();
}

void AthleteController::clearCurrentAthlete()
{
    QSettings s = ppSettings();
    s.remove(kCurrentUuid);
    s.sync();
    reload();
}
