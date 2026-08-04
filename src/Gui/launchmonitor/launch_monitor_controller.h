/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "launch_monitor_base.h"
#include "shot_pairing.h"

#include <QObject>
#include <QString>

class AppSettings;
class ShotListModel;

// The launch monitor's place in the app (QML context property `launchMonitor`).
//
// Owns the connector, applies the attribution rule, and writes a claimed reading into
// the shot's swing.json. The ImuManager analogue, and much smaller than one, because a
// file-polling source has nothing to enumerate, select or register with the buffer.
//
// Three things wire into it from main.cpp:
//   ShotController::shotDetected   → arm for this swing
//   ShotProcessor::shotProcessed   → a swingDir exists; flush anything parked
//   ShotProcessor::shotFailed      → drop anything parked for a swing that died
//
// State (the enum → string mapping) is translated here and nowhere else, so the QML
// contract cannot drift — the same arrangement UpdateController has.
class LaunchMonitorController : public QObject
{
    Q_OBJECT

    // Whether a connector is configured at all. The toolbar dot and the chime are both
    // gated on this: on a machine with no monitor, neither should ever appear.
    Q_PROPERTY(bool    configured   READ configured   NOTIFY stateChanged)
    Q_PROPERTY(QString state        READ stateName    NOTIFY stateChanged)
    Q_PROPERTY(QString stateLabel   READ stateLabel   NOTIFY stateChanged)
    Q_PROPERTY(QString errorText    READ errorText    NOTIFY stateChanged)
    // Where readings are being read from, for the settings panel to show back.
    Q_PROPERTY(QString sourceText   READ sourceText   NOTIFY stateChanged)
    // A one-line summary of the last reading applied ("283 · Irn · 87.2 mph"), so the
    // settings panel can prove the connection works without opening a shot.
    Q_PROPERTY(QString lastReading  READ lastReading  NOTIFY lastReadingChanged)

public:
    LaunchMonitorController(AppSettings *settings, ShotListModel *shotModel,
                            QObject *parent = nullptr);
    ~LaunchMonitorController() override;

    bool    configured() const;
    QString stateName()  const;
    QString stateLabel() const;
    QString errorText()  const;
    QString sourceText() const;
    QString lastReading() const { return m_lastReading; }

public slots:
    // ShotController::shotDetected. The extra arguments are ignored — which swing a
    // reading belongs to is decided by ORDER, not by timestamp, since the file gives
    // us no timestamp we could trust anyway.
    void onShotDetected();
    void onShotProcessed(int shotId, const QString &swingDir);
    void onShotFailed();

    // Rebuild the connector from settings. Called on construction and whenever the
    // configured kind or path changes.
    void reconfigure();

signals:
    void stateChanged();
    void lastReadingChanged();
    // A reading has been WRITTEN to a shot. The toolbar dot and the chime hang off
    // this rather than off the file changing: the light means "brought in", not
    // "noticed", and a reading we could not attribute must not flash anything.
    void readingApplied(const QString &swingDir);

private:
    void onReadingAvailable(const pinpoint::lm::LaunchMonitorReading &reading);
    // Write to swing.json and refresh the carousel row. Returns false (and logs) when
    // the document cannot be updated.
    bool applyToSwing(const QString &swingDir, const pinpoint::lm::LaunchMonitorReading &r);

    AppSettings                   *m_settings  = nullptr;
    ShotListModel                 *m_shotModel = nullptr;
    pinpoint::lm::LaunchMonitorBase *m_monitor = nullptr;   // never null — see makeLaunchMonitor
    pinpoint::lm::ShotPairing      m_pairing;
    QString                        m_lastReading;
};
