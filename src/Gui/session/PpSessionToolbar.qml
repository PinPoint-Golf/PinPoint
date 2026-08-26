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

// Persistent session toolbar (Command-Bar direction): session clock + the one
// session-global Capture verb + two count-badged device pills. Each pill opens a
// Popup hosting the relevant device panel. Reusable across all four mode screens
// (Wrist first). Calibration is handled entirely INSIDE the panels — the toolbar
// no longer routes a calibrate request anywhere.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    implicitHeight: Theme.sp(60)

    // Session type of the hosting screen (SessionController::Type, 0–3).
    // Set by ScreenSessionMode / ScreenWrist; passed to sessionController.start()
    // so the session lock knows which type owns the running session.
    property int sessionType: -1

    // Mode-switch labels, indexed by SessionMode.mode (capture/review/analyse).
    readonly property var modeNames: [qsTr("Capture"), qsTr("Replay"), qsTr("Analyse")]

    // Live capture truth — mirrors the actual EventBuffer state (same source as
    // the Resource Monitor). cameraManager.bufferState notifies on every net
    // transition, including those caused by IMU register/deregister (forwarded
    // via applyCaptureIntent in main.cpp).
    readonly property bool captureLive: cameraManager.bufferState === "capturing"

    // ── Aggregate device state (drives pill badges + colours) ───────────────
    readonly property int  camTotal:     cameraManager.cameraList.length
    readonly property int  camConnected: cameraManager.instances.length
    // True when a connected camera is not "fixed in place" — i.e. it still needs
    // (stereo) calibration this session. Mirrors PpCameraPanel.needsCalibration;
    // stays lit until the camera is calibrated/fixed.
    readonly property bool camNeedsAttention: {
        var _dep  = cameraManager.instances
        var list  = cameraManager.cameraList
        var fixed = appSettings.cameraFixedInPlace
        for (var i = 0; i < list.length; ++i)
            if (list[i].selected && fixed[list[i].cameraKey] !== true) return true
        return false
    }

    readonly property int  imuTotal:     imuManager.imuDeviceList.length
    readonly property int  imuConnected: imuManager.imuCount   // connected instance count
    // True when at least one connected IMU is not yet *successfully* calibrated →
    // attention. A sensor counts as calibrated only when anatCalibrated AND the
    // mount check passes (same thresholds the calibration flow uses: deviation
    // ≤ 15°, gravity ≤ 25°). anatCalibrated alone flips true at the phase-1 arm-down
    // capture, before phase-2 mount validation — so a failed calibration must keep
    // the attention indicator lit, not clear it.
    function _imuCalibratedOk(inst) {
        return inst && inst.anatCalibrated
            && inst.mountDeviationDeg    <= 15.0
            && inst.mountGravityErrorDeg <= 25.0
    }
    readonly property bool imuNeedsAttention: {
        var _dep = imuManager.instances
        var list = imuManager.imuDeviceList
        for (var i = 0; i < list.length; ++i) {
            var inst = imuManager.instanceFor(list[i].id)
            if (inst && inst.imuConnected && !_imuCalibratedOk(inst)) return true
        }
        return false
    }

    // Lowest battery % across all connected IMUs (−1 when none report a level),
    // sourced from the manager's aggregate so it tracks live battery updates.
    // A reading below 50% raises a toolbar warning naming the level.
    readonly property int  imuLowestBattery: imuManager.lowBatteryPercent
    readonly property bool imuBatteryLow:    imuLowestBattery >= 0 && imuLowestBattery < 50

    // ── Phone (PPCP) health, folded into the Cameras pill ──────────────────
    // There is no separate Phones pill — a phone's only reason to be on this
    // bar is that it is carrying a camera, so its battery/thermal reads as a
    // CAMERAS-pill warning, the same slot camNeedsAttention already owns.
    // `ppcpHost` exists only in a `HAVE_PPCP_TRANSPORT` build (main.cpp), so
    // this guards the same way PhonesPanel.qml's `controller` does.
    readonly property bool   havePpcp:          typeof ppcpHost !== "undefined"
    readonly property int    phoneLowestBattery: root.havePpcp ? ppcpHost.phoneLowestBatteryPct : -1
    readonly property bool   phoneBatteryLow:    phoneLowestBattery >= 0 && phoneLowestBattery < 50
    readonly property string phoneWorstThermal:  root.havePpcp ? ppcpHost.phoneWorstThermal : ""
    readonly property bool   phoneThermalWarn:   phoneWorstThermal === "serious" || phoneWorstThermal === "critical"

    // Clock-sync uncertainty (6.1f's sigma), the same number PPC's own screen
    // shows while it is still converging. -1 means no relation has an
    // estimate yet — a fresh connection reads this way for its first ~2
    // minutes (the burst-then-maintenance cadence settling), which is
    // EXPECTED and not a fault, unlike the battery/thermal warnings above —
    // so it gets its own (lower) priority and always reads amber, never red.
    // 5ms is comfortably under the 50ms default coincidence window and well
    // under one frame period even at 200fps+, so it reads as "converged"
    // once under that, not merely "connected".
    readonly property real    phoneSyncSigmaMs: root.havePpcp ? ppcpHost.phoneWorstSyncSigmaMs : -1
    readonly property bool    phoneSyncWarn:    phoneSyncSigmaMs >= 0 && phoneSyncSigmaMs > 5.0

    // ── Motion pill label ────────────────────────────────────────────────────
    // "Off" wins outright (master switch dominates); otherwise the active
    // preset's label, or "Custom" once the user hand-edits an element away
    // from any catalogue preset (ViewLayout.motionPreset flips to "custom").
    readonly property string motionPillLabel: {
        if (!ViewLayout.motionOn(SessionMode.mode)) return qsTr("Off")
        var id = ViewLayout.motionPreset(SessionMode.mode)
        if (id === "custom") return qsTr("Custom")
        var cat = ViewLayout.presetCatalog()
        for (var i = 0; i < cat.length; ++i)
            if (cat[i].id === id) return cat[i].label
        return id
    }

    // ── Club pill ─────────────────────────────────────────────────────────────
    // The session's active club (SessionController.activeClub — the SoT read at
    // shot-join). Empty before a session starts → fall back to the athlete's
    // default so the pill always names a club. Taped = the club record carries
    // retro bands (non-empty bandCentersMm). Reactive to activeClub / athlete edits.
    readonly property string activeClub: {
        void athleteController.athletes
        return sessionController.activeClub !== ""
            ? sessionController.activeClub
            : athleteController.effectivePrimaryClub(athleteController.currentUuid)
    }
    readonly property bool activeClubTaped: {
        void athleteController.athletes
        var c = root.activeClub
        if (!c || !athleteController.hasCurrentAthlete) return false
        var rec = athleteController.clubsFor(athleteController.currentUuid)[c]
        return !!(rec && rec.bandCentersMm && rec.bandCentersMm.length > 0)
    }

    // Start a session: choose its on-disk folder, then start the clock + capture.
    // extend appends to today's most-recent folder (its prior swings load into the
    // carousel); otherwise a fresh "_NN" folder is created and the carousel clears.
    function _beginSession(extend) {
        shotProcessor.beginSessionFolder(root.sessionType, extend)
        shotModel.loadSessionDir(extend ? shotProcessor.activeSessionDir : "")
        sessionController.start(root.sessionType)
        cameraManager.startCapture()
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.aesthetic === "instrument" ? Theme.colorBg2 : Theme.colorSurface
    }
    Rectangle {  // bottom hairline
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1; color: Theme.colorBorderMid; opacity: Theme.borderOpacityNormal
    }

    RowLayout {
        anchors { fill: parent; leftMargin: Theme.sp(16); rightMargin: Theme.sp(14) }
        spacing: Theme.sp(12)

        // ── Review strip — replaces the whole capture cluster while a saved ──
        // session is loaded (device pills stay; see the !reviewActive gates on
        // captureBtn / clock / End / SHOT below).
        Rectangle {
            id: reviewStrip
            visible: sessionReviewController.reviewActive
            Layout.alignment: Qt.AlignVCenter
            implicitWidth:  reviewRow.implicitWidth + Theme.sp(28)
            implicitHeight: Theme.sp(40)
            radius: Theme.radius
            color: Theme.colorAccentLight
            border.width: 1; border.color: Theme.colorAccentMid

            Row {
                id: reviewRow
                anchors.centerIn: parent
                spacing: Theme.sp(10)
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("REVIEWING")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorAccent
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: sessionReviewController.activeDayLabel
                          + (sessionReviewController.activeTimeLabel
                                 ? " · " + sessionReviewController.activeTimeLabel : "")
                          + (sessionReviewController.activeClubMix
                                 ? " — " + sessionReviewController.activeClubMix : "")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                    color: Theme.colorText
                }
            }
        }

        // Returning to live is now the Capture mode button's job (it calls
        // resumeLive), so the old "Resume live session" button is gone — the
        // REVIEWING strip above just names the loaded session.

        // ── Capture (session-global) — far left, always first ───────────────
        Rectangle {
            id: captureBtn
            visible: !sessionReviewController.reviewActive
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: captureLbl.implicitWidth + Theme.sp(28)
            implicitHeight: Theme.sp(40)
            radius: Theme.radius
            readonly property color _baseColor: root.captureLive ? Theme.colorErrorLight : Theme.colorAccent
            color: capMa.containsMouse ? Qt.lighter(_baseColor, 1.08) : _baseColor
            border.width: root.captureLive ? 1 : 0
            border.color: Theme.colorError

            // Hover/press motion — the same language as the device pills, adapted
            // to a filled CTA: it brightens and grows a touch on hover, dips on
            // press. A filled button has no border to ease, so the brighten plays
            // the role bg2↔bg3 does on the pills. The colour Behavior also smooths
            // the Capture↔Stop swap. Theme.durationFast + OutCubic; reduceMotion
            // zeroes it.
            transformOrigin: Item.Center
            scale: capMa.pressed       ? 0.97
                 : capMa.containsMouse ? 1.02
                 :                       1.0
            Behavior on color { ColorAnimation  { duration: Theme.durationFast } }
            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }
            Row {
                id: captureLbl
                anchors.centerIn: parent
                spacing: Theme.sp(8)
                Rectangle {
                    width: Theme.sp(9); height: Theme.sp(9); radius: Theme.sp(4.5)
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.captureLive ? Theme.colorError
                                            : (Theme.dark ? Theme.colorBg : "#FFFFFF")
                }
                Text {
                    text: root.captureLive ? qsTr("Stop") : qsTr("Capture")
                    font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                    color: root.captureLive ? Theme.colorError
                                            : (Theme.dark ? Theme.colorBg : "#FFFFFF")
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: capMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.captureLive) {
                        cameraManager.stopCapture()
                    } else if (sessionController.running) {
                        // Session already running (resumed after Stop) — just re-arm
                        // capture; the folder was chosen when the session started.
                        cameraManager.startCapture()
                    } else if (shotProcessor.todaySessionDir(root.sessionType) !== "") {
                        startPrompt.open()        // ask: extend today's session, or new
                    } else {
                        root._beginSession(false) // no folder for today → new session
                    }
                }
            }

            // Extend-or-new prompt — shown when today already has a session folder.
            // Extend appends to it; New starts a fresh "_NN"; Cancel leaves capture off.
            Popup {
                id: startPrompt
                y: captureBtn.height + Theme.sp(10)
                x: 0
                padding: Theme.sp(14)
                margins: Theme.sp(8)
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                background: Rectangle {
                    color: Theme.colorSurface; radius: Theme.radiusLg
                    border.width: 1; border.color: Theme.colorBorderStrong
                }
                contentItem: Column {
                    spacing: Theme.sp(10)
                    Text {
                        text: qsTr("A session already exists for today.")
                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                        color: Theme.colorText
                    }
                    Row {
                        spacing: Theme.sp(8)
                        Rectangle {
                            width: extendLbl.implicitWidth + Theme.sp(20)
                            height: Theme.sp(30); radius: Theme.radius
                            color: extendMa.containsMouse ? Theme.colorAccentLight : "transparent"
                            border.width: 1; border.color: Theme.colorAccentMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: extendLbl
                                anchors.centerIn: parent; text: qsTr("Extend")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorAccent
                            }
                            PpPressable {
                                id: extendMa
                                onClicked: { startPrompt.close(); root._beginSession(true) }
                            }
                        }
                        Rectangle {
                            width: newLbl.implicitWidth + Theme.sp(20)
                            height: Theme.sp(30); radius: Theme.radius
                            color: newMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
                            border.width: 1; border.color: Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: newLbl
                                anchors.centerIn: parent; text: qsTr("New session")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorText2
                            }
                            PpPressable {
                                id: newMa
                                onClicked: { startPrompt.close(); root._beginSession(false) }
                            }
                        }
                        Rectangle {
                            width: cancelStartLbl.implicitWidth + Theme.sp(20)
                            height: Theme.sp(30); radius: Theme.radius
                            color: cancelStartMa.containsMouse ? Theme.colorBg3 : "transparent"
                            border.width: 1; border.color: Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: cancelStartLbl
                                anchors.centerIn: parent; text: qsTr("Cancel")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorText2
                            }
                            PpPressable {
                                id: cancelStartMa
                                onClicked: startPrompt.close()
                            }
                        }
                    }
                }
            }
        }

        // ── Session clock (alongside Capture, to its right) ─────────────────
        Column {
            visible: !sessionReviewController.reviewActive
            spacing: Theme.sp(2)
            Layout.alignment: Qt.AlignVCenter
            Text {
                text: qsTr("SESSION")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Row {
                spacing: Theme.sp(8)
                Rectangle {
                    width: Theme.sp(8); height: Theme.sp(8); radius: Theme.sp(4)
                    anchors.verticalCenter: parent.verticalCenter
                    color: root.captureLive ? Theme.colorError : Theme.colorText3
                    SequentialAnimation on opacity {
                        running: root.captureLive && !Theme.reduceMotion
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.3; duration: Theme.durationSlow }
                        NumberAnimation { to: 1.0; duration: Theme.durationSlow }
                    }
                }
                Text {
                    text: sessionController.elapsedLabel
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
                    color: Theme.colorText
                }
            }
        }

        // ── End Session (groups with the session controls) ──────────────────
        // Ghost button, only while a session is running. Confirmed via a small
        // anchored popup — the session clock can't be resumed once ended.
        Rectangle {
            id: endBtn
            visible: sessionController.running && !sessionReviewController.reviewActive
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: endLbl.implicitWidth + Theme.sp(24)
            implicitHeight: Theme.sp(40)
            radius: Theme.radius
            color: endMa.containsMouse ? Theme.colorBg2 : "transparent"
            border.width: 1
            border.color: Theme.colorBorderMid

            // Hover/press motion — matches the device pills: subtle scale up on
            // hover, held while the confirm popover is open, dip on press. The
            // ghost fill (transparent↔bg2) already carries the hover brighten, and
            // the border stays the quiet neutral — an End-session button shouldn't
            // pull the accent. Theme.durationFast + OutCubic; reduceMotion zeroes it.
            transformOrigin: Item.Center
            scale: endMa.pressed                            ? 0.97
                 : (endPopup.opened || endMa.containsMouse) ? 1.02
                 :                                            1.0
            Behavior on color { ColorAnimation  { duration: Theme.durationFast } }
            Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

            Text {
                id: endLbl
                anchors.centerIn: parent
                text: qsTr("End Session")
                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                color: Theme.colorText2
            }
            MouseArea {
                id: endMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: endPopup.opened ? endPopup.close() : endPopup.open()
            }

            Popup {
                id: endPopup
                y: endBtn.height + Theme.sp(10)
                x: 0
                padding: Theme.sp(14)
                margins: Theme.sp(8)
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                background: Rectangle {
                    color: Theme.colorSurface; radius: Theme.radiusLg
                    border.width: 1; border.color: Theme.colorBorderStrong
                }
                contentItem: Column {
                    spacing: Theme.sp(10)
                    Text {
                        text: qsTr("End session?")
                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                        color: Theme.colorText
                    }
                    Row {
                        spacing: Theme.sp(8)
                        Rectangle {
                            width: confirmLbl.implicitWidth + Theme.sp(20)
                            height: Theme.sp(30); radius: Theme.radius
                            color: confirmMa.containsMouse ? Theme.colorErrorLight : "transparent"
                            border.width: 1; border.color: Theme.colorError
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: confirmLbl
                                anchors.centerIn: parent; text: qsTr("End")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorError
                            }
                            PpPressable {
                                id: confirmMa
                                onClicked: {
                                    endPopup.close()
                                    cameraManager.stopCapture()
                                    sessionController.endSession()
                                    // Discard the session folder if it captured no
                                    // new swings (moved to the OS trash — recoverable),
                                    // then re-point the carousel at today's remaining
                                    // most-recent session, or empty it.
                                    shotProcessor.endSessionFolder()
                                    shotModel.loadSessionDir(
                                        shotProcessor.todaySessionDir(root.sessionType))
                                    // Ending the session releases the devices:
                                    // cameras stop + deselect, IMUs disconnect
                                    // (BLE battery). The next session's wizard
                                    // reconnects what it needs.
                                    cameraManager.disconnectAll()
                                    imuManager.disconnectAll()
                                }
                            }
                        }
                        Rectangle {
                            width: cancelLbl.implicitWidth + Theme.sp(20)
                            height: Theme.sp(30); radius: Theme.radius
                            color: cancelMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
                            border.width: 1; border.color: Theme.colorBorderMid
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Text {
                                id: cancelLbl
                                anchors.centerIn: parent; text: qsTr("Cancel")
                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorText2
                            }
                            PpPressable {
                                id: cancelMa
                                onClicked: endPopup.close()
                            }
                        }
                    }
                }
            }
        }

        // Centre stage moved to the title bar (SHOT trigger + ANALYSING badge);
        // this spacer now just pushes the device cluster to the right. The mode
        // switch is centred on the bar as an overlay sibling (see `modeSwitch`,
        // declared after the RowLayout) so it sits at the toolbar's true centre.
        Item { Layout.fillWidth: true }

        // ── View · subtle vertical divider sets the layout cluster apart ────
        // Always present alongside the View/Cameras/IMUs cluster (the device
        // pills are never gated on review state, and neither is View).
        PpDivider {
            orientation: Qt.Vertical
            Layout.preferredHeight: Theme.sp(28)
            Layout.alignment: Qt.AlignVCenter
        }

        // ── Club pill — the session's active club (a capture parameter, so ──
        // hidden while reviewing a loaded session). Shows the current club and a
        // taped-club marker dot; its popup lists the athlete's bag. Leads the pill
        // cluster as session context. Reuses PpToolPill (glyph + micro label + chevron).
        PpToolPill {
            id: clubPill
            visible: !sessionReviewController.reviewActive
            glyph: "⚑"
            microLabel: qsTr("CLUB")
            label: root.activeClub ? ClubFormat.display(root.activeClub) : qsTr("—")
            badge: root.activeClubTaped
            badgeColor: Theme.colorGood
            active: clubPopup.opened
            onClicked: {
                viewPopup.close(); motionPopup.close(); camPopup.close(); imuPopup.close()
                clubPopup.opened ? clubPopup.close() : clubPopup.open()
            }
        }

        // ── View pill — edits the CURRENT mode's saved layout ─────────────────
        PpToolPill {
            id: viewPill
            label: root.modeNames[SessionMode.mode]
            active: viewPopup.opened
            onClicked: {
                camPopup.close(); imuPopup.close(); motionPopup.close(); clubPopup.close()
                viewPopup.opened ? viewPopup.close() : viewPopup.open()
            }
        }

        // ── Motion pill — edits the CURRENT mode's motion overlay layout ────
        // Adjacent to View. Reuses PpToolPill (glyph / microLabel properties)
        // rather than duplicating its markup.
        PpToolPill {
            id: motionPill
            glyph: "∿"
            microLabel: qsTr("MOTION")
            label: root.motionPillLabel
            active: motionPopup.opened
            onClicked: {
                viewPopup.close(); camPopup.close(); imuPopup.close(); clubPopup.close()
                motionPopup.opened ? motionPopup.close() : motionPopup.open()
            }
        }

        // ── Cameras pill ────────────────────────────────────────────────────
        // Phone battery/thermal takes priority over the sync warning, which in
        // turn takes priority over the calibrate hint. Sync is ranked below
        // battery/thermal deliberately: an unconverged clock during the first
        // ~2 minutes after connecting is EXPECTED, not a fault, so it reads
        // amber and never displaces a genuine battery/thermal problem.
        DevicePill {
            id: camPill
            glyph: "◫"                 // ◫
            title: qsTr("CAMERAS")
            active: camPopup.opened
            count: root.camConnected
            valueText: root.camTotal === 0 ? qsTr("none")
                        : root.phoneBatteryLow ? qsTr("phone battery %1%").arg(root.phoneLowestBattery)
                        : root.phoneThermalWarn ? qsTr("phone %1").arg(root.phoneWorstThermal)
                        : root.phoneSyncWarn ? qsTr("sync ±%1ms").arg(root.phoneSyncSigmaMs.toFixed(1))
                        : root.camNeedsAttention ? qsTr("calibrate")
                        : qsTr("%1 of %2").arg(root.camConnected).arg(root.camTotal)
            ledColor: root.phoneBatteryLow ? (root.phoneLowestBattery <= 20 ? Theme.colorError : Theme.colorWarn)
                       : root.phoneThermalWarn ? (root.phoneWorstThermal === "critical" ? Theme.colorError : Theme.colorWarn)
                       : root.phoneSyncWarn ? Theme.colorWarn
                       : root.camNeedsAttention ? Theme.colorAttention
                       : root.camConnected > 0 ? Theme.colorGood : Theme.colorText3
            attention: root.camNeedsAttention && !root.phoneBatteryLow && !root.phoneThermalWarn && !root.phoneSyncWarn
            warn: root.phoneBatteryLow || root.phoneThermalWarn || root.phoneSyncWarn
            warnColor: root.phoneBatteryLow ? (root.phoneLowestBattery <= 20 ? Theme.colorError : Theme.colorWarn)
                        : root.phoneThermalWarn ? (root.phoneWorstThermal === "critical" ? Theme.colorError : Theme.colorWarn)
                        : Theme.colorWarn
            onClicked: {
                imuPopup.close(); viewPopup.close(); motionPopup.close(); clubPopup.close()
                camPopup.opened ? camPopup.close() : camPopup.open()
            }
        }

        // ── IMUs pill ───────────────────────────────────────────────────────
        // Low battery takes priority over the calibrate hint in the value line —
        // a dying sensor is time-critical, and the message names the level so the
        // user knows how low (e.g. "battery 32%"). Critical (≤20%) reads red.
        DevicePill {
            id: imuPill
            glyph: "⦿"                 // ⦿
            title: qsTr("IMUS")
            active: imuPopup.opened
            count: root.imuConnected
            valueText: root.imuTotal === 0 ? qsTr("none")
                        : root.imuBatteryLow ? qsTr("battery %1%").arg(root.imuLowestBattery)
                        : root.imuNeedsAttention ? qsTr("calibrate")
                        : qsTr("%1 connected").arg(root.imuConnected)
            ledColor: root.imuBatteryLow ? (root.imuLowestBattery <= 20 ? Theme.colorError : Theme.colorWarn)
                       : root.imuNeedsAttention ? Theme.colorAttention
                       : root.imuConnected > 0 ? Theme.colorGood : Theme.colorText3
            attention: root.imuNeedsAttention && !root.imuBatteryLow
            warn: root.imuBatteryLow
            warnColor: root.imuLowestBattery <= 20 ? Theme.colorError : Theme.colorWarn
            onClicked: {
                camPopup.close(); viewPopup.close(); motionPopup.close(); clubPopup.close()
                imuPopup.opened ? imuPopup.close() : imuPopup.open()
            }
        }
    }

    // ── Mode switch — primary layout control (Capture/Replay/Analyse) ─────────
    // Centred on the toolbar as an overlay sibling of the RowLayout (the bar's
    // centre is otherwise empty — SHOT/ANALYSING moved to the title bar), so it
    // sits at the TRUE centre regardless of the asymmetric left/right clusters.
    // The activity axis: Replay is never blocked (empty-state with no focused
    // swing); Replay/Analyse leave the data-source alone; Capture is the single
    // path back to the live current session (SessionReviewController.resumeLive).
    PpSegmentedControl {
        id: modeSwitch
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter:   parent.verticalCenter
        width: Theme.sp(220)
        solid: false
        options:  root.modeNames
        selected: root.modeNames[SessionMode.mode]
        onActivated: (value) => {
            var i = root.modeNames.indexOf(value)
            if (i === SessionMode.replay)       SessionMode.showReplay()
            else if (i === SessionMode.analyse) SessionMode.enterAnalyse()
            else                                SessionMode.enterCapture()
        }
    }

    // ── Popups host the reusable panels; positioned under their pills ───────
    // margins clamp the popup within the window; the panel's implicitHeight
    // drives the popup height so it grows when a panel enters calibrate mode.
    Popup {
        id: viewPopup
        parent: viewPill
        y: viewPill.height + Theme.sp(10)
        x: viewPill.width - width
        padding: 0
        margins: Theme.sp(8)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.colorSurface; radius: Theme.radiusLg
            border.width: 1; border.color: Theme.colorBorderStrong
        }
        contentItem: PpViewPanel { }
    }

    Popup {
        id: motionPopup
        parent: motionPill
        y: motionPill.height + Theme.sp(10)
        x: motionPill.width - width
        padding: 0
        margins: Theme.sp(8)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.colorSurface; radius: Theme.radiusLg
            border.width: 1; border.color: Theme.colorBorderStrong
        }
        contentItem: PpMotionPanel { }
    }

    Popup {
        id: camPopup
        parent: camPill
        y: camPill.height + Theme.sp(10)
        x: camPill.width - width
        padding: 0
        margins: Theme.sp(8)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.colorSurface; radius: Theme.radiusLg
            border.width: 1; border.color: Theme.colorBorderStrong
        }
        contentItem: PpCameraPanel {}
    }

    Popup {
        id: imuPopup
        parent: imuPill
        y: imuPill.height + Theme.sp(10)
        x: imuPill.width - width
        padding: 0
        margins: Theme.sp(8)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.colorSurface; radius: Theme.radiusLg
            border.width: 1; border.color: Theme.colorBorderStrong
        }
        contentItem: PpImuPanel {}
    }

    // Club popup — under its pill in the right cluster (right-aligned like its
    // siblings). PpClubPanel writes the pick to SessionController.activeClub and
    // asks to close.
    Popup {
        id: clubPopup
        parent: clubPill
        y: clubPill.height + Theme.sp(10)
        x: clubPill.width - width
        padding: 0
        margins: Theme.sp(8)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        background: Rectangle {
            color: Theme.colorSurface; radius: Theme.radiusLg
            border.width: 1; border.color: Theme.colorBorderStrong
        }
        contentItem: PpClubPanel {
            onRequestClose: clubPopup.close()
        }
    }

    // ── View pill — lighter sibling of DevicePill (no badge; shows the active
    // mode and a chevron; accent ring while its popup is open) ────────────────
    // The View/Motion/Club/Cameras/IMUs pill is the SHARED PpToolPill
    // (src/Gui/components/PpToolPill.qml), so any surface outside the toolbar
    // can present the same item instead of a lookalike that drifts.

    // ── Inline pill component ───────────────────────────────────────────────
    component DevicePill: Rectangle {
        property string glyph:     ""
        property string title:     ""
        property string valueText: ""
        property int    count:     0
        property color  ledColor:  Theme.colorText3
        property bool   active:    false   // tints the glyph accent while the pill's popup is open
        property bool   attention: false   // tints the value text amber (e.g. "calibrate")
        property bool   warn:      false   // overrides the value-text tint (e.g. low battery)
        property color  warnColor: Theme.colorWarn
        signal clicked()

        Layout.alignment: Qt.AlignVCenter
        implicitWidth: pillRow.implicitWidth + Theme.sp(24)
        implicitHeight: Theme.sp(44)
        radius: Theme.radius
        color: pillMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
        border.width: 1
        border.color: active              ? Theme.colorAccent
                    : pillMa.containsMouse ? Theme.colorAccentMid
                    :                        Theme.colorBorderMid

        // Same hover/press motion as PpToolPill (see note there): subtle scale up
        // on hover, held while the popup is open, dip on press. No lift — keeps
        // the pill anchored in the bar.
        transformOrigin: Item.Center
        scale: pillMa.pressed              ? 0.97
             : (active || pillMa.containsMouse) ? 1.02
             :                               1.0

        Behavior on color        { ColorAnimation  { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation  { duration: Theme.durationFast } }
        Behavior on scale        { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

        RowLayout {
            id: pillRow
            anchors { fill: parent; leftMargin: Theme.sp(11); rightMargin: Theme.sp(13) }
            spacing: Theme.sp(11)

            Item {  // glyph + count badge
                Layout.preferredWidth: Theme.sp(34); Layout.preferredHeight: Theme.sp(34)
                Layout.alignment: Qt.AlignVCenter
                Rectangle {
                    anchors.fill: parent; radius: Theme.radius; color: Theme.colorSurface
                    Text {
                        anchors.centerIn: parent; text: glyph
                        font.family: Theme.fontSymbol; font.pixelSize: Theme.sp(18)
                        color: active ? Theme.colorAccent : Theme.colorText2
                    }
                }
                Rectangle {
                    anchors { right: parent.right; top: parent.top
                              rightMargin: -Theme.sp(5); topMargin: -Theme.sp(5) }
                    width: Theme.sp(19); height: Theme.sp(19); radius: width / 2
                    color: ledColor
                    Text {
                        anchors.centerIn: parent; text: count
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        color: Theme.dark ? Theme.colorBg : "#FFFFFF"
                    }
                }
            }
            Column {
                Layout.alignment: Qt.AlignVCenter; spacing: Theme.sp(2)
                Text {
                    text: title; font.family: Theme.fontData
                    font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText3
                }
                Text {
                    text: valueText; font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color: warn ? warnColor
                         : attention ? Theme.colorAttention : Theme.colorText
                }
            }
        }
        MouseArea {
            id: pillMa; anchors.fill: parent; hoverEnabled: true
            cursorShape: Qt.PointingHandCursor; onClicked: parent.clicked()
        }
    }
}
