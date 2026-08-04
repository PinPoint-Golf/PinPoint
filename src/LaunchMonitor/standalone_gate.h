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

#include <QString>

namespace pinpoint::lm {

// Whether a reading that NOTHING ELSE SAW should become a shot of its own.
//
// Split out of LaunchMonitorController and made pure on purpose. The decision reads five
// pieces of state off four different controllers, and it has now behaved differently on a
// real machine than it did in my head TWICE — once because it did not require capture to
// be active at all, and once because the whole path sat behind a setting that was off. A
// decision with that history should be exhaustively testable without standing up a
// CameraManager, so the controller's remaining job is to GATHER facts, which is the part
// that is hard to get wrong.
//
// The rescue path — a shot the app detected but could not record — deliberately does NOT
// come through here. It needs no permission beyond storage being on: the app saw a shot
// and the device measured it, so there is nothing left to be cautious about.
struct StandaloneFacts {
    bool connectorConfigured = false;   // a launch monitor is selected at all
    bool standaloneEnabled   = false;   // launchmonitor/standaloneShots
    bool storeDeviceData     = false;   // storage/saveLaunchMonitorData
    bool libraryConfigured   = false;   // an athlete library path exists
    bool athleteSelected     = false;   // allocateSwingDir needs a name and a uuid
    bool sessionRunning      = false;   // a session folder to land in
    bool captureActive       = false;   // the toolbar Capture/Stop state
};

enum class StandaloneVerdict {
    Record,             // all of it holds — write the shot
    NotConfigured,
    Disabled,
    StorageOff,
    NoLibrary,
    NoAthlete,
    NoSession,
    CaptureInactive,
};

// ORDERED CONFIGURATION FIRST, THEN CURRENT STATE, and the order is the whole reason this
// returns a verdict rather than a bool. When several things are false the reason reported
// is the one the user is most likely able to act on: somebody still setting the monitor up
// hears about the setting before they hear about capture, and somebody already set up
// hears about capture rather than being told again that they have no library.
StandaloneVerdict decideStandalone(const StandaloneFacts &facts);

// The log line. Phrased as what is missing rather than as an error, because none of these
// is one — they are the ordinary states in which a reading is simply not ours to keep.
QString standaloneVerdictReason(StandaloneVerdict verdict);

} // namespace pinpoint::lm
