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

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class AthleteController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool         hasCurrentAthlete READ hasCurrentAthlete NOTIFY currentAthleteChanged)
    Q_PROPERTY(QString      currentName       READ currentName       NOTIFY currentAthleteChanged)
    Q_PROPERTY(QString      currentInitials   READ currentInitials   NOTIFY currentAthleteChanged)
    Q_PROPERTY(QString      currentHandedness READ currentHandedness NOTIFY currentAthleteChanged)
    Q_PROPERTY(QString      currentUuid       READ currentUuid       NOTIFY currentAthleteChanged)
    Q_PROPERTY(QVariantList athletes          READ athletes          NOTIFY athletesChanged)
    Q_PROPERTY(QVariantList recentSessions   READ recentSessions    NOTIFY athletesChanged)

public:
    explicit AthleteController(QObject *parent = nullptr);

    bool         hasCurrentAthlete() const { return !m_currentUuid.isEmpty(); }
    QString      currentName()       const { return m_currentName; }
    QString      currentInitials()   const { return m_currentInitials; }
    QString      currentHandedness() const { return m_currentHandedness; }
    QString      currentUuid()       const { return m_currentUuid; }
    QVariantList athletes()          const { return m_athletes; }
    QVariantList recentSessions()    const { return {}; }

    // Upsert: empty uuid creates a new athlete (returns its new uuid); a non-empty,
    // existing uuid updates in place (returns that uuid). Returns "" on failure
    // (empty name, or unknown uuid on update). Performs cm->ft / kg->lb conversion.
    Q_INVOKABLE QString saveAthlete(
        const QString &uuid,                                    // "" = create
        const QString &name,
        const QString &handedness  = QStringLiteral("Right"),
        double         heightValue = 0.0,
        const QString &heightUnit  = QStringLiteral("ft"),
        double         weightValue = 0.0,
        const QString &weightUnit  = QStringLiteral("lb"),
        double         handicap    = -999.0,
        const QString &primaryClub = QStringLiteral("DRIVER"),
        double         speedTarget = 0.0,
        const QString &notes       = QString()
    );

    Q_INVOKABLE QString createAthlete(
        const QString &name,
        const QString &handedness  = QStringLiteral("Right"),
        double         heightValue = 0.0,
        const QString &heightUnit  = QStringLiteral("ft"),
        double         weightValue = 0.0,
        const QString &weightUnit  = QStringLiteral("lb"),
        double         handicap    = -999.0,
        const QString &primaryClub = QStringLiteral("DRIVER"),
        double         speedTarget = 0.0,
        const QString &notes       = QString()
    );

    Q_INVOKABLE bool updateAthlete(const QString &uuid, const QString &fieldName, const QVariant &value);

    // Per-athlete club records (Settings live under athletes/<uuid>/clubs as a
    // QVariantMap keyed by the canonical club-vocabulary name — the same id
    // markup writes to truth.json meta.club). Record shape:
    //   { shaftType: "steel"|"graphite", loftDeg: double, lengthMm: int, bandWidthMm: int,
    //     bandCentersMm: list<int> (retro-band CENTRES from the butt, mm;
    //     empty = untaped), hoselFromButtMm: int, headPatch: bool,
    //     tapedOn: "YYYY-MM-DD", notes: string }
    // Consumers: shaft-tracker search radius (lengthMm) and the
    // instrumented-club pipeline (docs/validation/instrumented_club_protocol.md).
    Q_INVOKABLE QVariantMap clubsFor(const QString &uuid) const;
    Q_INVOKABLE QVariantMap defaultClubRecord(const QString &clubId) const;  // factory spec defaults (loft/length/shaft)
    Q_INVOKABLE QStringList clubOptions() const;   // canonical vocabulary (club_vocabulary.h)
    // The athlete's default/preferred club, always resolved to a real bag club:
    // the stored primaryClub (legacy tokens normalised to canonical) if it is in
    // the bag, else "DRIVER" if present, else the first bag club, else "". Read on
    // both the QML side (Home / form pickers) and the C++ side (session seeding).
    Q_INVOKABLE QString effectivePrimaryClub(const QString &uuid) const;
    Q_INVOKABLE bool setClubRecord(const QString &uuid, const QString &clubId,
                                   const QVariantMap &record);
    Q_INVOKABLE bool removeClubRecord(const QString &uuid, const QString &clubId);
    Q_INVOKABLE bool deleteAthlete(const QString &uuid);
    Q_INVOKABLE void selectAthlete(const QString &uuid);
    Q_INVOKABLE void clearCurrentAthlete();

signals:
    void currentAthleteChanged();
    void athletesChanged();

private:
    void reload();
    static QString computeInitials(const QString &name);

    QVariantList m_athletes;
    QString      m_currentUuid;
    QString      m_currentName;
    QString      m_currentInitials;
    QString      m_currentHandedness;
};
