/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "swing_paths.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include "../Core/pp_debug.h"

namespace pinpoint {

// Count of swing_* subdirectories in a session folder (disk truth).
static int countSwingDirs(const QString& sessionDir)
{
    return QDir(sessionDir)
        .entryList({QStringLiteral("swing_*")}, QDir::Dirs | QDir::NoDotAndDotDot)
        .size();
}

// Highest "_NN" index among "<base>_(\d+)" session folders under athleteDir
// (0 when none exist).
static int maxSessionIndex(const QString& athleteDir, const QString& base)
{
    int maxIndex = 0;
    const QRegularExpression sessionRe(
        QStringLiteral("^%1_(\\d+)$").arg(QRegularExpression::escape(base)));
    const QStringList existing =
        QDir(athleteDir).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& name : existing) {
        const auto m = sessionRe.match(name);
        if (m.hasMatch())
            maxIndex = std::max(maxIndex, m.captured(1).toInt());
    }
    return maxIndex;
}

QString SwingPaths::sanitise(const QString& raw)
{
    QString s = raw.trimmed();
    // Whitespace and path-hostile characters become '-'.
    static const QRegularExpression hostile(QStringLiteral("[^A-Za-z0-9._-]+"));
    s.replace(hostile, QStringLiteral("-"));
    // Collapse runs of separators and strip them from the ends.
    static const QRegularExpression runs(QStringLiteral("-{2,}"));
    s.replace(runs, QStringLiteral("-"));
    while (s.startsWith(QLatin1Char('-')) || s.startsWith(QLatin1Char('.')))
        s.removeFirst();
    while (s.endsWith(QLatin1Char('-')) || s.endsWith(QLatin1Char('.')))
        s.removeLast();
    s.truncate(64);
    return s.isEmpty() ? QStringLiteral("unknown") : s;
}

SwingPaths::Resolved SwingPaths::resolveSession(const QString& libraryRoot,
                                                const QString& athleteName,
                                                const QString& athleteUuid,
                                                const QString& namingPattern,
                                                const QString& sessionTypeLabel)
{
    Resolved r;

    r.root = libraryRoot.trimmed();
    if (r.root.isEmpty()) {
        r.root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                 + QStringLiteral("/swings");
        ppWarn() << "[SwingExport] athleteLibraryPath is empty — falling back to" << r.root;
    }

    // Athlete folder: name, then uuid, then "unknown".
    const QString athleteToken =
        sanitise(!athleteName.trimmed().isEmpty() ? athleteName : athleteUuid);
    r.athleteDir = r.root + QLatin1Char('/') + athleteToken;

    r.today = QDate::currentDate().toString(Qt::ISODate);

    // Compose the session-folder base name (pre-counter) per the naming pattern.
    // Tokens are filesystem-sanitised; the athlete token doubles as the name.
    const QString nameTok = athleteToken;
    const QString typeTok = sanitise(sessionTypeLabel.trimmed().isEmpty()
                                         ? QStringLiteral("session") : sessionTypeLabel);
    if (namingPattern == QLatin1String("date-type-name"))
        r.base = r.today + QLatin1Char('_') + typeTok + QLatin1Char('_') + nameTok;
    else if (namingPattern == QLatin1String("name-date-type"))
        r.base = nameTok + QLatin1Char('_') + r.today + QLatin1Char('_') + typeTok;
    else if (namingPattern == QLatin1String("date-only"))
        r.base = r.today;
    else   // "date-name-type" (default)
        r.base = r.today + QLatin1Char('_') + nameTok + QLatin1Char('_') + typeTok;

    return r;
}

SwingPaths::Allocation SwingPaths::allocateSwingDir(const QString& libraryRoot,
                                                    const QString& athleteName,
                                                    const QString& athleteUuid,
                                                    const QString& namingPattern,
                                                    const QString& sessionTypeLabel)
{
    Allocation out;

    const Resolved r =
        resolveSession(libraryRoot, athleteName, athleteUuid, namingPattern, sessionTypeLabel);

    // Session folder: resolved once per (root, athlete, day, base) and cached so
    // all swings of an app run share it; a new day / athlete / root / pattern /
    // session-type reallocates. A "_NN" counter keeps runs distinct. When
    // beginSession() has primed the cache, the key already matches and this
    // no-ops — every swing lands in the session folder chosen at start.
    if (m_cachedSessionDir.isEmpty() || m_cachedRoot != r.root ||
        m_cachedAthleteUuid != athleteUuid || m_cachedDate != r.today ||
        m_cachedBase != r.base) {
        const int maxIndex = maxSessionIndex(r.athleteDir, r.base);
        m_cachedSessionId  = QStringLiteral("%1_%2")
                                 .arg(r.base)
                                 .arg(maxIndex + 1, 2, 10, QLatin1Char('0'));
        m_cachedSessionDir = r.athleteDir + QLatin1Char('/') + m_cachedSessionId;
        m_cachedRoot        = r.root;
        m_cachedAthleteUuid = athleteUuid;
        m_cachedDate        = r.today;
        m_cachedBase        = r.base;
        ppInfo() << "[SwingExport] session folder:" << m_cachedSessionDir;
    }

    // Swing folder: count existing swing_* dirs, then probe upward in case of
    // gaps or collisions until an unused name is found.
    QDir sessionDir(m_cachedSessionDir);
    int index = sessionDir.entryList({QStringLiteral("swing_*")},
                                     QDir::Dirs | QDir::NoDotAndDotDot).size() + 1;
    QString swingId  = QStringLiteral("swing_%1").arg(index, 4, 10, QLatin1Char('0'));
    while (sessionDir.exists(swingId)) {
        ++index;
        swingId = QStringLiteral("swing_%1").arg(index, 4, 10, QLatin1Char('0'));
    }

    const QString swingDir = m_cachedSessionDir + QLatin1Char('/') + swingId;
    if (!QDir().mkpath(swingDir)) {
        ppError() << "[SwingExport] failed to create" << swingDir;
        return out;   // swingDir empty signals failure
    }

    out.swingDir   = swingDir;
    out.sessionId  = m_cachedSessionId;
    out.swingId    = swingId;
    out.swingIndex = index;
    return out;
}

QString SwingPaths::findTodaySessionDir(const QString& libraryRoot,
                                        const QString& athleteName,
                                        const QString& athleteUuid,
                                        const QString& namingPattern,
                                        const QString& sessionTypeLabel)
{
    const Resolved r =
        resolveSession(libraryRoot, athleteName, athleteUuid, namingPattern, sessionTypeLabel);
    const int maxIndex = maxSessionIndex(r.athleteDir, r.base);
    if (maxIndex <= 0)
        return {};   // no "<base>_NN" folder exists for today
    const QString id  = QStringLiteral("%1_%2").arg(r.base).arg(maxIndex, 2, 10, QLatin1Char('0'));
    const QString dir = r.athleteDir + QLatin1Char('/') + id;
    return QDir(dir).exists() ? dir : QString{};
}

QString SwingPaths::beginSession(const QString& libraryRoot,
                                 const QString& athleteName,
                                 const QString& athleteUuid,
                                 const QString& namingPattern,
                                 const QString& sessionTypeLabel,
                                 bool extendExisting)
{
    const Resolved r =
        resolveSession(libraryRoot, athleteName, athleteUuid, namingPattern, sessionTypeLabel);

    QString sessionDir;
    QString sessionId;

    // Extend today's most-recent folder when asked and one exists.
    if (extendExisting) {
        sessionDir = findTodaySessionDir(libraryRoot, athleteName, athleteUuid,
                                         namingPattern, sessionTypeLabel);
        if (!sessionDir.isEmpty())
            sessionId = QFileInfo(sessionDir).fileName();
    }

    // Otherwise (or when no today folder exists) allocate + create a fresh "_NN".
    if (sessionDir.isEmpty()) {
        const int maxIndex = maxSessionIndex(r.athleteDir, r.base);
        sessionId  = QStringLiteral("%1_%2").arg(r.base).arg(maxIndex + 1, 2, 10, QLatin1Char('0'));
        sessionDir = r.athleteDir + QLatin1Char('/') + sessionId;
        if (!QDir().mkpath(sessionDir)) {
            ppError() << "[SwingExport] failed to create session folder" << sessionDir;
            return {};
        }
    }

    // Prime the allocation cache so every subsequent allocateSwingDir() reuses it.
    m_cachedRoot        = r.root;
    m_cachedAthleteUuid = athleteUuid;
    m_cachedDate        = r.today;
    m_cachedBase        = r.base;
    m_cachedSessionDir  = sessionDir;
    m_cachedSessionId   = sessionId;
    m_sessionBaselineSwings = countSwingDirs(sessionDir);

    ppInfo() << "[SwingExport] begin session:" << sessionDir
             << (extendExisting ? "(extend)" : "(new)")
             << "baseline swings" << m_sessionBaselineSwings;
    return sessionDir;
}

void SwingPaths::endSession(bool discardIfNoNewSwings)
{
    if (discardIfNoNewSwings && !m_cachedSessionDir.isEmpty()) {
        // Only discard when the session captured nothing new since it began. The
        // baseline is 0 for a fresh folder and N for an extended one; a mid-flight
        // shot has already mkpath'd its swing_NNNN/, so it is never in this set.
        const int now = countSwingDirs(m_cachedSessionDir);
        if (now <= m_sessionBaselineSwings) {
            // Recoverable removal (OS trash), not a permanent delete — matches the
            // shot-trash convention, so an accidentally-discarded session survives.
            QString where;
            if (trashPath(m_cachedSessionDir, &where))
                ppInfo() << "[SwingExport] discarded session (to" << where << "):" << m_cachedSessionDir;
            else
                ppWarn() << "[SwingExport] could not trash empty session:" << m_cachedSessionDir;
        }
    }

    // Clear the cache so the next session re-resolves (and lazy allocation resumes
    // if no explicit beginSession() follows).
    m_cachedRoot.clear();
    m_cachedAthleteUuid.clear();
    m_cachedDate.clear();
    m_cachedBase.clear();
    m_cachedSessionDir.clear();
    m_cachedSessionId.clear();
    m_sessionBaselineSwings = -1;
}

bool SwingPaths::trashPath(const QString& path, QString* where)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    if (QFile::moveToTrash(path)) {
        if (where) *where = QStringLiteral("the system trash");
        return true;
    }
    // The volume has no trash (a network share).  Same volume, so this is a
    // rename: `<root>/.pinpoint-trash/<athlete>/<stamp>_[<session>_]<name>`.
    // A swing folder sits in a session which sits in an athlete folder; a
    // session sits in an athlete folder.
    const QFileInfo fi(path);
    const QString   name    = fi.fileName();
    const bool      isSwing = name.startsWith(QStringLiteral("swing_"));
    const QString   parent  = QDir::cleanPath(fi.absolutePath());
    const QString   athleteDir = isSwing ? QDir::cleanPath(parent + QStringLiteral("/.."))
                                         : parent;
    const QString   root       = QDir::cleanPath(athleteDir + QStringLiteral("/.."));
    const QString   athlete    = QFileInfo(athleteDir).fileName();
    const QString   trashDir   = root + QStringLiteral("/.pinpoint-trash/") + athlete;
    if (!QDir().mkpath(trashDir)) {
        ppWarn() << "[SwingPaths] no trash on this volume and could not create" << trashDir;
        return false;
    }
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString tag   = isSwing ? QFileInfo(parent).fileName() + QStringLiteral("_") : QString();
    QString dest = trashDir + QStringLiteral("/") + stamp + QStringLiteral("_") + tag + name;
    for (int n = 2; QFileInfo::exists(dest) && n < 100; ++n)
        dest = trashDir + QStringLiteral("/") + stamp + QStringLiteral("_") + tag + name
               + QStringLiteral("-") + QString::number(n);
    if (!QDir().rename(path, dest)) {
        ppWarn() << "[SwingPaths] no trash on this volume and could not move" << path
                 << "to" << dest;
        return false;
    }
    ppInfo() << "[SwingPaths] no trash on this volume — moved to" << dest;
    if (where) *where = dest;
    return true;
}

} // namespace pinpoint
