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

// The session diagnostics stage panel: what keeps happening this session, and what the
// model thinks is causing it. The post-shot dashboard answers "what happened on that
// swing"; this one is per session and cumulative, and most of its design is the restraint
// that keeps it from ever saying more than the ledger supports.
//
// THIS FILE IS THE WIRING AND NOTHING ELSE. Everything the panel LOOKS like is
// PpSessionDiagnosticsBody, which takes a `source` and owns none of it. The split is the
// one PpLaunchMonitorPanel makes with PpLmGraphicsBody, and it buys the same thing twice
// over: the body can be loaded offscreen with a fixture, so the states nobody can produce
// on demand — Cold with an empty fault profile, the bandwidth-quiet strip, a
// not-assessable tick — are asserted rather than eyeballed; and there is exactly one place
// where this panel touches the app's singletons, which is here.
//
// THE MODEL IS INSTANTIATED, NOT REACHED FOR. SessionDiagnosticsModel owns a characteristic
// pack, a norm provider and a worker thread that re-runs detection on every shot, and this
// panel is OFF by default — the user turns it on in View. A golfer who never opens it
// should not be paying for any of that, which is why the model is not a context property in
// main.cpp. See the class comment on SessionDiagnosticsModel.
//
// CADENCE GATES SURFACING, NEVER THE ENGINE. ingestShot() below is wired straight to
// ShotProcessor::shotProcessed with no cadence check anywhere near it, and there must never
// be one: the ledger accumulates at full rate whatever the panel is showing (brief §3.4,
// §5.6). ingestShot() is idempotent per shot id, which is what makes it safe to wire this
// way and to call again from activateSession()'s back-fill.

import QtQuick
import PinPointStudio

Item {
    id: root

    // Set by PpModeStage only on the muted placeholder path; the panel titles itself.
    property string title: qsTr("Session diagnostics")

    // THE ONE INDIRECTION THE BODY READS THROUGH. Left at diagModel in the app; a test or a
    // second host can point the body at a different object of the same shape without this
    // file having to know.
    property alias source: body.source

    SessionDiagnosticsModel {
        id: diagModel

        // Both passed in rather than reached for, so the model stays constructible in a
        // test. gradePolicy must be the SAME policy the dashboard and the launch monitor
        // board grade against, or one reading would sit outside its corridor on one panel
        // and inside it on another.
        cadence:     appSettings.sessionDiagnosticsCadence
        gradePolicy: appSettings.diagnosticsGradePolicy

        // Review FREEZES the ledger at Closing: a finished session's panel is its summary
        // and must not re-open because it was looked at. Consuming the selection — the
        // carousel's focused shot, the review this-shot strip — is a later phase; the
        // property is set here so the model is already in the right tense when it lands.
        reviewing:   sessionReviewController.reviewActive
    }

    // The live session's directory. activateSession() covers three situations that are the
    // same problem — opening a finished session, resuming after a crash, and turning this
    // panel on half way through a session — by reconciling diagnostics.json against the
    // swing_* directories beside it and back-filling the difference.
    readonly property string sessionDir: shotProcessor.activeSessionDir

    onSessionDirChanged: if (sessionDir !== "") diagModel.activateSession(sessionDir)
    Component.onCompleted: if (sessionDir !== "") diagModel.activateSession(sessionDir)

    Connections {
        target: shotProcessor
        function onShotProcessed(shotId, swingDir) {
            diagModel.ingestShot(shotId, swingDir)
        }
    }

    PpSessionDiagnosticsBody {
        id: body
        anchors.fill: parent
        source: diagModel
    }
}
