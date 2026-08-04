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

#include "launch_monitor_controller.h"

#include "app_settings.h"
#include "athlete_controller.h"
#include "camera_manager.h"
#include "launch_monitor_factory.h"
#include "pp_debug.h"
#include "session_controller.h"
#include "standalone_gate.h"
#include "shot_list_model.h"
#include "../../Export/swing_doc.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QUrl>

using namespace pinpoint::lm;

LaunchMonitorController::LaunchMonitorController(AppSettings *settings, ShotListModel *shotModel,
                                                 AthleteController *athleteController,
                                                 SessionController *sessionController,
                                                 CameraManager *cameraManager,
                                                 QObject *parent)
    : QObject(parent), m_settings(settings), m_shotModel(shotModel),
      m_athletes(athleteController), m_session(sessionController), m_cameras(cameraManager)
{
    reconfigure();

    if (m_settings) {
        connect(m_settings, &AppSettings::launchMonitorKindChanged,
                this, &LaunchMonitorController::reconfigure);
        connect(m_settings, &AppSettings::launchMonitorPathChanged,
                this, &LaunchMonitorController::reconfigure);
        connect(m_settings, &AppSettings::launchMonitorPollMsChanged, this, [this]() {
            if (m_monitor) m_monitor->setPollIntervalMs(m_settings->launchMonitorPollMs());
        });
    }
}

LaunchMonitorController::~LaunchMonitorController()
{
    if (m_monitor)
        m_monitor->stop();
}

void LaunchMonitorController::reconfigure()
{
    const Kind kind = m_settings ? kindFromKey(m_settings->launchMonitorKind()) : Kind::None;

    // Rebuild only when the kind actually changes; a path edit must not tear down and
    // recreate the connector, because that would re-prime the watermark and could lose
    // a reading in flight.
    if (!m_monitor || m_monitor->kind() != kind) {
        if (m_monitor) {
            m_monitor->stop();
            m_monitor->deleteLater();
        }
        m_monitor = makeLaunchMonitor(kind, this);
        connect(m_monitor, &LaunchMonitorBase::readingAvailable,
                this, &LaunchMonitorController::onReadingAvailable);
        connect(m_monitor, &LaunchMonitorBase::stateChanged,
                this, &LaunchMonitorController::stateChanged);
    }

    if (m_settings) {
        m_monitor->setPollIntervalMs(m_settings->launchMonitorPollMs());
        m_monitor->setSourcePath(m_settings->launchMonitorPath());
    }
    m_monitor->start();
    emit stateChanged();
}

bool LaunchMonitorController::configured() const
{
    return m_monitor && m_monitor->kind() != Kind::None;
}

QString LaunchMonitorController::stateName() const
{
    if (!m_monitor)
        return QStringLiteral("disabled");
    switch (m_monitor->state()) {
    case State::Disabled: return QStringLiteral("disabled");
    case State::Waiting:  return QStringLiteral("waiting");
    case State::Ready:    return QStringLiteral("ready");
    case State::Error:    return QStringLiteral("error");
    }
    return QStringLiteral("disabled");
}

QString LaunchMonitorController::stateLabel() const
{
    if (!m_monitor)
        return tr("Not configured");
    switch (m_monitor->state()) {
    case State::Disabled: return tr("Not configured");
    case State::Waiting:  return tr("Waiting for a shot");
    case State::Ready:    return tr("Connected");
    case State::Error:    return tr("Cannot read the folder");
    }
    return tr("Not configured");
}

QString LaunchMonitorController::errorText() const
{
    return m_monitor ? m_monitor->errorText() : QString();
}

QString LaunchMonitorController::sourceText() const
{
    return m_monitor ? m_monitor->sourceDescription() : QString();
}

void LaunchMonitorController::onShotDetected()
{
    m_destinationHasDoc = true;   // assume the pipeline will produce one, until it says otherwise
    m_destinationShotId = -1;
    m_pairing.noteShotDetected();
}

void LaunchMonitorController::onShotProcessed(int shotId, const QString &swingDir)
{
    Q_UNUSED(shotId)
    if (swingDir.isEmpty())
        return;
    m_destinationHasDoc = true;
    if (const auto parked = m_pairing.noteSwingDir(swingDir))
        applyToSwing(swingDir, *parked);
}

void LaunchMonitorController::onShotFailed(const QString &swingDir, int shotId)
{
    // NOT the end of the shot. The processor allocates the swing folder BEFORE it runs
    // analysis or export, so a shot with no cameras and no IMUs — the whole point of
    // owning a launch monitor without them — reaches here with a real, empty folder and
    // a reading that describes what happened. Filling it is the honest outcome; leaving
    // it empty and saying "analysis failed" describes the pipeline rather than the shot.
    //
    // No setting gates this. The app DETECTED a shot and the monitor MEASURED it; there
    // is nothing to be cautious about, which is what separates it from a standalone shot
    // arriving with nothing else going on at all.
    if (swingDir.isEmpty()) {
        if (m_pairing.hasParked())
            ppWarn() << "LaunchMonitor: shot failed with no folder allocated; reading discarded";
        m_pairing.noteShotFailed();
        return;
    }

    m_destinationHasDoc = false;
    m_destinationShotId = shotId;
    if (const auto parked = m_pairing.noteSwingDir(swingDir))
        writeDeviceOnly(swingDir, *parked, QString(), 0, QString(), m_destinationShotId);
}

void LaunchMonitorController::onReadingAvailable(const LaunchMonitorReading &reading)
{
    const ShotPairing::Offer offer = m_pairing.offerReading(reading);
    switch (offer.disposition) {
    case ShotPairing::Disposition::Discarded:
        // Nothing was waiting for it. Normally that is the end of the matter — a ball
        // struck while we were not capturing, or a second write with no shot between.
        // With standalone shots switched on it becomes a shot of its own instead.
        if (createStandaloneShot(reading))
            return;
        ppInfo() << "LaunchMonitor: reading" << reading.deviceShotId
                 << "arrived with no shot waiting; discarded";
        return;
    case ShotPairing::Disposition::Parked:
        // The usual case — analysis is still running, so there is nowhere to put it
        // yet. onShotProcessed() will flush it.
        return;
    case ShotPairing::Disposition::Claimed:
        // The destination is known. Whether it already holds a document decides which
        // way the reading goes in — the shot may have failed before this arrived.
        if (m_destinationHasDoc)
            applyToSwing(offer.swingDir, reading);
        else
            writeDeviceOnly(offer.swingDir, reading, QString(), 0, QString(), m_destinationShotId);
        return;
    }
}

bool LaunchMonitorController::writeDeviceOnly(const QString &swingDir,
                                              const LaunchMonitorReading &r,
                                              const QString &swingId, int swingIndex,
                                              const QString &sessionId, int existingShotId)
{
    if (m_settings && !m_settings->saveLaunchMonitorData()) {
        ppInfo() << "LaunchMonitor: storing device data is switched off; nothing written";
        return false;
    }

    pinpoint::SwingDocWriter::DeviceOnlyMeta meta;
    // The rescue path is handed a folder somebody else allocated, so it reads the
    // identity back off the path rather than being told: <session>/<swing_NNNN>.
    const QDir dir(swingDir);
    meta.swingId    = swingId.isEmpty() ? dir.dirName() : swingId;
    meta.sessionId  = sessionId.isEmpty() ? QDir(dir).dirName() : sessionId;
    if (sessionId.isEmpty()) {
        QDir parent(swingDir);
        if (parent.cdUp()) meta.sessionId = parent.dirName();
    }
    meta.swingIndex = swingIndex;
    if (swingIndex == 0) {
        // "swing_0007" → 7. Falls back to 0, which is only an ordering hint.
        const QString digits = meta.swingId.section(QLatin1Char('_'), -1);
        meta.swingIndex = digits.toInt();
    }
    if (m_athletes) {
        meta.athleteName = m_athletes->currentName();
        meta.athleteUuid = m_athletes->currentUuid();
    }
    if (m_session) {
        meta.club        = m_session->activeClub();
        meta.sessionType = m_session->activeSessionType();
    }
    meta.wallclockMs = r.readAtMs > 0 ? r.readAtMs : QDateTime::currentMSecsSinceEpoch();

    QString error;
    if (!pinpoint::SwingDocWriter::writeDeviceOnlySwing(swingDir, r, meta, &error)) {
        ppWarn() << "LaunchMonitor: cannot write a device-only shot to" << swingDir << ":" << error;
        return false;
    }

    if (m_shotModel) {
        if (existingShotId >= 0) {
            // The processor already made a row for this shot — an in-memory one with no
            // folder, because nothing was written. Point it at the folder we just filled.
            m_shotModel->attachSwingDir(existingShotId, swingDir);
        } else {
            const pinpoint::PersistedShot ps = pinpoint::SwingDocReader::readSwingJson(swingDir);
            m_shotModel->addShot(swingDir, ps.timestampLabel, ps.club,
                                 /*hasVideo*/ false, QUrl(), /*tracePoints*/ {},
                                 /*score*/ 0, ps.metrics, ps.analysisDetail);
        }
    }

    ppInfo() << "LaunchMonitor: device-only shot" << r.deviceShotId << "written to" << swingDir;

    QStringList bits;
    if (!r.deviceShotId.isEmpty()) bits << r.deviceShotId;
    if (!r.deviceClub.isEmpty())   bits << r.deviceClub;
    if (r.clubheadSpeed)           bits << tr("%1 mph").arg(*r.clubheadSpeed, 0, 'f', 1);
    bits << tr("monitor only");
    m_lastReading = bits.join(QStringLiteral(" · "));
    emit lastReadingChanged();

    emit deviceOnlyShotSaved(swingDir);
    emit readingApplied(swingDir);
    return true;
}

bool LaunchMonitorController::createStandaloneShot(const LaunchMonitorReading &reading)
{
    // The decision is decideStandalone()'s, not this function's. Everything here is
    // fact-GATHERING — see standalone_gate.h for why the two are separated.
    StandaloneFacts facts;
    facts.connectorConfigured = configured();
    facts.standaloneEnabled   = m_settings && m_settings->launchMonitorStandalone();
    facts.storeDeviceData     = m_settings && m_settings->saveLaunchMonitorData();
    facts.libraryConfigured   = m_settings && !m_settings->athleteLibraryPath().isEmpty();
    facts.athleteSelected     = m_athletes && m_athletes->hasCurrentAthlete();
    facts.sessionRunning      = m_session  && m_session->running();
    // Capture-active is the user's own statement, and no buffer state can stand in for
    // it: with no cameras and no IMUs there are no sources, so the buffer stays paused
    // however loudly they have said they are hitting balls.
    facts.captureActive       = m_cameras  && m_cameras->captureIntent();

    const StandaloneVerdict verdict = decideStandalone(facts);
    if (verdict != StandaloneVerdict::Record) {
        ppInfo() << "LaunchMonitor: reading" << reading.deviceShotId
                 << "not recorded as a shot of its own —"
                 << standaloneVerdictReason(verdict);
        return false;
    }

    const auto alloc = m_paths.allocateSwingDir(m_settings->athleteLibraryPath(),
                                                m_athletes->currentName(),
                                                m_athletes->currentUuid(),
                                                m_settings->sessionNamingPattern(),
                                                QString());
    if (alloc.swingDir.isEmpty()) {
        ppWarn() << "LaunchMonitor: standalone shot skipped — could not create the swing folder";
        return false;
    }

    return writeDeviceOnly(alloc.swingDir, reading, alloc.swingId, alloc.swingIndex,
                           alloc.sessionId, /*existingShotId*/ -1);
}

bool LaunchMonitorController::applyToSwing(const QString &swingDir, const LaunchMonitorReading &r)
{
    if (m_settings && !m_settings->saveLaunchMonitorData()) {
        ppInfo() << "LaunchMonitor: storing device data is switched off; reading not written";
        return false;
    }

    QString error;
    if (!pinpoint::SwingDocWriter::updateLaunchMonitor(swingDir, r, &error)) {
        ppWarn() << "LaunchMonitor: cannot write reading to" << swingDir << ":" << error;
        return false;
    }

    // Re-read the carousel row in place so the shot picks up its new metrics without
    // losing selection or ordering. The phase-grid sidecar is guarded on swing.json's
    // size and mtime, which the rewrite just moved, so the measures table regenerates
    // on its next read rather than serving a grid with no launch monitor rows in it.
    if (m_shotModel)
        m_shotModel->refreshShot(swingDir);

    QStringList bits;
    if (!r.deviceShotId.isEmpty()) bits << r.deviceShotId;
    if (!r.deviceClub.isEmpty())   bits << r.deviceClub;
    if (r.clubheadSpeed)           bits << tr("%1 mph").arg(*r.clubheadSpeed, 0, 'f', 1);
    if (r.carryDistance)           bits << tr("%1 yd").arg(*r.carryDistance, 0, 'f', 0);
    m_lastReading = bits.join(QStringLiteral(" · "));
    emit lastReadingChanged();

    emit readingApplied(swingDir);
    return true;
}
