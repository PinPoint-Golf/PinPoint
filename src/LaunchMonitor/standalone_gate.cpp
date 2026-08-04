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

#include "standalone_gate.h"

#include <QCoreApplication>

namespace pinpoint::lm {

StandaloneVerdict decideStandalone(const StandaloneFacts &f)
{
    if (!f.connectorConfigured) return StandaloneVerdict::NotConfigured;
    if (!f.standaloneEnabled)   return StandaloneVerdict::Disabled;
    if (!f.storeDeviceData)     return StandaloneVerdict::StorageOff;
    if (!f.libraryConfigured)   return StandaloneVerdict::NoLibrary;
    if (!f.athleteSelected)     return StandaloneVerdict::NoAthlete;
    if (!f.sessionRunning)      return StandaloneVerdict::NoSession;
    if (!f.captureActive)       return StandaloneVerdict::CaptureInactive;
    return StandaloneVerdict::Record;
}

QString standaloneVerdictReason(StandaloneVerdict v)
{
    const auto tr = [](const char *s) {
        return QCoreApplication::translate("LaunchMonitor", s);
    };
    switch (v) {
    case StandaloneVerdict::Record:
        return QString();
    case StandaloneVerdict::NotConfigured:
        return tr("no launch monitor is configured");
    case StandaloneVerdict::Disabled:
        return tr("recording shots the monitor sees on its own is switched off");
    case StandaloneVerdict::StorageOff:
        return tr("storing launch monitor data is switched off");
    case StandaloneVerdict::NoLibrary:
        return tr("no athlete library is configured");
    case StandaloneVerdict::NoAthlete:
        return tr("no athlete is selected");
    case StandaloneVerdict::NoSession:
        return tr("no session is running");
    case StandaloneVerdict::CaptureInactive:
        return tr("capture is not active");
    }
    return QString();
}

} // namespace pinpoint::lm
