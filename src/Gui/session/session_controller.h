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

#include <QElapsedTimer>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

class AthleteController;

// Owns the live session clock shown in the session toolbar AND the active
// session type. Capture state lives on CameraManager (user intent via
// startCapture()/stopCapture(); QML truth via the bufferState property).
//
// A session is *active* while the clock is running; start(type) sets which
// session type owns it. The single-active-session invariant is enforced here:
// start() with a different type while running is refused. Navigation gating
// derives from (running && activeSessionType >= 0) in NavigationController.
class SessionController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("SessionController is a context property; the type exists for enum access")
    Q_PROPERTY(bool    running           READ running           NOTIFY runningChanged)
    Q_PROPERTY(QString elapsedLabel      READ elapsedLabel      NOTIFY elapsedLabelChanged)
    Q_PROPERTY(int     activeSessionType READ activeSessionType NOTIFY activeSessionTypeChanged)
    // The club in play for this session (canonical vocabulary id). Seeded from the
    // current athlete's preferred club on start() when unset; the Home CLUB chip
    // (and future toolbar) can override it. Read by ShotProcessor at capture time.
    Q_PROPERTY(QString activeClub        READ activeClub        WRITE setActiveClub NOTIFY activeClubChanged)

public:
    // Matches the QML session-type indices (ScreenSessionWizard.sessionTypes
    // order; rail/stack index = type + 1). Registered with QML
    // (SessionController.Wrist etc.) — use the enum names in QML, never the
    // raw integers.
    enum class Type { None = -1, Swing = 0, Wrist = 1, Grf = 2, Coach = 3 };
    Q_ENUM(Type)

    explicit SessionController(AthleteController *athlete = nullptr, QObject *parent = nullptr);

    bool    running()           const { return m_running; }
    QString elapsedLabel()      const { return m_label; }
    int     activeSessionType() const { return static_cast<int>(m_sessionType); }
    QString activeClub()        const { return m_activeClub; }

    void setActiveClub(const QString &club);

    Q_INVOKABLE void start(int sessionType);  // begin a session of this type + start the clock
    Q_INVOKABLE void endSession();             // stop the clock + clear the type (unlocks navigation)
    Q_INVOKABLE void stop();    // freeze the clock (type untouched)
    Q_INVOKABLE void reset();   // back to 00:00:00, stopped

signals:
    // A session began, of this type.  Emitted after the clock starts and after
    // ShotProcessor::beginSessionFolder() has run, so a listener sees the real
    // outcome of the folder allocation rather than a guess at it.
    void sessionStarted(int sessionType);
    void runningChanged();
    void elapsedLabelChanged();
    void activeSessionTypeChanged();
    void activeClubChanged();

private:
    void tick();

    AthleteController *m_athlete = nullptr;   // preferred-club seed source (not owned)
    QElapsedTimer m_clock;
    QTimer        m_ticker;
    bool          m_running     = false;
    Type          m_sessionType = Type::None;
    QString       m_label       = QStringLiteral("00:00:00");
    QString       m_activeClub;               // "" until seeded/set; cleared on endSession
};
