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
#include "launch_monitor_factory.h"
#include "pp_debug.h"
#include "shot_list_model.h"
#include "../../Export/swing_doc.h"

using namespace pinpoint::lm;

LaunchMonitorController::LaunchMonitorController(AppSettings *settings, ShotListModel *shotModel,
                                                 QObject *parent)
    : QObject(parent), m_settings(settings), m_shotModel(shotModel)
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
    m_pairing.noteShotDetected();
}

void LaunchMonitorController::onShotProcessed(int shotId, const QString &swingDir)
{
    Q_UNUSED(shotId)
    if (swingDir.isEmpty())
        return;
    if (const auto parked = m_pairing.noteSwingDir(swingDir))
        applyToSwing(swingDir, *parked);
}

void LaunchMonitorController::onShotFailed()
{
    if (m_pairing.hasParked())
        ppWarn() << "LaunchMonitor: shot failed before reaching disk; discarding its reading";
    m_pairing.noteShotFailed();
}

void LaunchMonitorController::onReadingAvailable(const LaunchMonitorReading &reading)
{
    const ShotPairing::Offer offer = m_pairing.offerReading(reading);
    switch (offer.disposition) {
    case ShotPairing::Disposition::Discarded:
        // Ordinary, not an error: a ball struck while we were not capturing, or a
        // second write with no shot in between.
        ppInfo() << "LaunchMonitor: reading" << reading.deviceShotId
                 << "arrived with no shot waiting; discarded";
        return;
    case ShotPairing::Disposition::Parked:
        // The usual case — analysis is still running, so there is nowhere to put it
        // yet. onShotProcessed() will flush it.
        return;
    case ShotPairing::Disposition::Claimed:
        applyToSwing(offer.swingDir, reading);
        return;
    }
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
