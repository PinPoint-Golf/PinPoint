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

#include "session_controller.h"
#include "athlete_controller.h"
#include "pp_debug.h"

SessionController::SessionController(AthleteController *athlete, QObject *parent)
    : QObject(parent), m_athlete(athlete)
{
    m_ticker.setInterval(1000);
    m_ticker.setTimerType(Qt::CoarseTimer);
    connect(&m_ticker, &QTimer::timeout, this, &SessionController::tick);
}

void SessionController::setActiveClub(const QString &club)
{
    if (club == m_activeClub)
        return;
    m_activeClub = club;
    emit activeClubChanged();
}

void SessionController::start(int sessionType)
{
    const Type type = static_cast<Type>(sessionType);

    // Single-active-session invariant: a running session owns the clock; a
    // start for a different type is refused (the UI gating makes this
    // unreachable, but the invariant lives here, not in the UI).
    if (m_running && type != m_sessionType) {
        ppWarn() << "[SessionController] start refused — session type"
                 << static_cast<int>(m_sessionType) << "already active, requested"
                 << sessionType;
        return;
    }

    if (type != m_sessionType) {
        m_sessionType = type;
        emit activeSessionTypeChanged();
    }

    // Seed the club from the athlete's preferred club unless one was already set
    // for this session (e.g. the Home CLUB chip picked a specific club). This
    // covers the toolbar Capture path, which starts a session without Home.
    if (m_activeClub.isEmpty() && m_athlete) {
        const QString club = m_athlete->effectivePrimaryClub(m_athlete->currentUuid());
        setActiveClub(club);
    }

    m_clock.restart();
    if (!m_running) { m_running = true; emit runningChanged(); }
    tick();
    m_ticker.start();

    // ⚠ LAST, AND AFTER THE FOLDER IS ALLOCATED.  Both start paths — the wizard
    // (Main.qml) and the toolbar's Capture button (PpSessionToolbar.qml) — call
    // shotProcessor.beginSessionFolder() and then this, so this is the one
    // funnel a session-start preflight can hang off without touching either of
    // them, and by here the folder allocation has already succeeded or failed
    // for real.  main.cpp owns what the preflight then checks; this class knows
    // only that a session began.
    emit sessionStarted(static_cast<int>(m_sessionType));
}

void SessionController::endSession()
{
    m_ticker.stop();
    if (m_running) { m_running = false; emit runningChanged(); }
    if (m_sessionType != Type::None) {
        m_sessionType = Type::None;
        emit activeSessionTypeChanged();
    }
    setActiveClub(QString());   // next session re-seeds from the athlete's preference
    // m_label keeps the final duration; the next start() restarts the clock.
}

void SessionController::stop()
{
    m_ticker.stop();
    if (m_running) { m_running = false; emit runningChanged(); }
}

void SessionController::reset()
{
    stop();
    m_label = QStringLiteral("00:00:00");
    emit elapsedLabelChanged();
}

void SessionController::tick()
{
    const qint64 s = m_clock.elapsed() / 1000;
    const QString next = QStringLiteral("%1:%2:%3")
        .arg(s / 3600,      2, 10, QChar('0'))
        .arg((s / 60) % 60, 2, 10, QChar('0'))
        .arg(s % 60,        2, 10, QChar('0'));
    if (next != m_label) { m_label = next; emit elapsedLabelChanged(); }
}
