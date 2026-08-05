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

#include "session_list_model.h"
#include "shot_list_model.h"

class AppSettings;
class AthleteController;

// SessionReviewController — the single source of truth for "am I reviewing a
// saved session, and if so which one". Global (one instance, a QML context
// property `sessionReviewController`), so all four mode screens show the same
// loaded session: switching Swing↔Wrist↔GRF↔Coach while reviewing keeps the
// selection. Sibling to SessionController (which stays clock-only) — it owns the
// disk enumeration and load that a clock controller has no business holding.
//
//  • sessionsModel — every session on disk for the current athlete, plus the
//    live session synthesized from the in-memory shotModel (pinned first,
//    isLive). The live session's own disk dir is excluded so it never appears
//    twice.
//  • shots         — a private ShotListModel populated from a loaded session's
//    swing.json files (same roles + invokables as the live shotModel, so the
//    carousel's mutation calls and rating/note write-through work unchanged).
//
// Entering review (loadSession) flips reviewActive true; main.cpp reacts by
// stopping live capture (which disarms the shot trigger) and gating ShotController
// — the live session/clock is left running, so resumeLive() returns to it.
class SessionReviewController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool     reviewActive   READ reviewActive   NOTIFY reviewActiveChanged)
    Q_PROPERTY(QObject *sessionsModel  READ sessionsModel  CONSTANT)
    Q_PROPERTY(QObject *shots          READ shots          CONSTANT)
    // THE SHOTS THE APP IS ACTUALLY SHOWING: the loaded session's while reviewing, the
    // live carousel's otherwise. One answer, in one place, because every panel that
    // boards "the session" has to agree with the carousel about which session that is —
    // the launch monitor board took the live model unconditionally and so drew an empty
    // board over a loaded session that was full of readings.
    Q_PROPERTY(ShotListModel *activeShots READ activeShots NOTIFY reviewActiveChanged)
    // Loaded-session header bits for the toolbar review strip + carousel chip.
    Q_PROPERTY(QString  activeDayLabel  READ activeDayLabel  NOTIFY reviewActiveChanged)
    Q_PROPERTY(QString  activeTimeLabel READ activeTimeLabel NOTIFY reviewActiveChanged)
    Q_PROPERTY(QString  activeClubMix   READ activeClubMix   NOTIFY reviewActiveChanged)
    Q_PROPERTY(int      activeShotCount READ activeShotCount NOTIFY activeShotCountChanged)

public:
    SessionReviewController(ShotListModel     *liveModel,
                            AppSettings       *settings,
                            AthleteController *athlete,
                            QObject           *parent = nullptr);

    bool     reviewActive()   const { return m_reviewActive; }
    QObject *sessionsModel()        { return &m_sessionsModel; }
    QObject *shots()                { return &m_reviewModel; }
    ShotListModel *activeShots()    { return m_reviewActive ? &m_reviewModel : m_liveModel; }
    QString  activeDayLabel()  const { return m_activeDayLabel; }
    QString  activeTimeLabel() const { return m_activeTimeLabel; }
    QString  activeClubMix()   const { return m_activeClubMix; }
    int      activeShotCount() const { return m_reviewModel.activeCount(); }

    // Rebuild sessionsModel from disk + the live model. Call when the drawer
    // opens; also run on construction and whenever review state changes.
    Q_INVOKABLE void refresh();

    // Load a past session (sessionId == its absolute dir path) into the review
    // model and enter review. Empty/the live row's id falls through to resumeLive().
    Q_INVOKABLE void loadSession(const QString &sessionId);

    // Return to the live model. Does not resume capture — the user presses
    // Capture again; the session clock kept running throughout.
    Q_INVOKABLE void resumeLive();

    // The session's swing_NNNN dirs (absolute) for the bulk export sheet — the
    // same list the carousel feeds to swingExporter. Empty for the live row's
    // sentinel id (its shots export through the carousel itself).
    Q_INVOKABLE QVariantList swingDirsForSession(const QString &sessionId) const;

    // Move a whole saved session folder to the OS trash (recoverable there — the
    // same recovery path as the carousel's per-shot trash) and rebuild the list.
    // No-op (false) for the live row or a path that can't be trashed; if the
    // trashed session was being reviewed, falls back to live. Returns true on
    // success.
    Q_INVOKABLE bool trashSession(const QString &sessionId);

public slots:
    // The session list is athlete-scoped. Nothing used to react to an athlete switch, so
    // the drawer kept showing the previous athlete's sessions until something else forced
    // a rebuild. Drops any loaded session (it belongs to the previous athlete) and rebuilds.
    void onAthleteChanged();

signals:
    void reviewActiveChanged();
    void activeShotCountChanged();

private:
    SessionListModel::Row buildLiveRow(qint64 nowMs) const;
    SessionListModel::Row buildDiskRow(const QString &sessionDir, qint64 nowMs) const;
    // Absolute path of the session the live model is writing into (parent of any
    // live shot's swingDir), or empty if no shot has been exported yet.
    QString liveSessionDir() const;

    ShotListModel     *m_liveModel = nullptr;
    AppSettings       *m_settings  = nullptr;
    AthleteController *m_athlete   = nullptr;

    SessionListModel   m_sessionsModel;
    ShotListModel      m_reviewModel;     // the loaded session's shots

    bool    m_reviewActive = false;
    QString m_activeDayLabel;
    QString m_activeTimeLabel;
    QString m_activeClubMix;
    QString m_loadedSessionDir;        // absolute dir of the reviewed session, empty when live
};
