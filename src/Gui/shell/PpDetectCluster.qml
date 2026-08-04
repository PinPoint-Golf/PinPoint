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

// Shot-detection indicator cluster — the "DETECT" label plus one dot per
// detection modality (IMU impact + acoustic onset today; Ball/vision reserved).
// Hosted centred in the title bar (PpHeader) during a live Capture session.
//
// This cluster is ALSO the manual shot trigger — it REPLACES the old toolbar
// SHOT button. A frame fades in on hover (armed only) and a click fires
// shotController.triggerShot() with the same brief accent border-flash the SHOT
// button used. `armed` tracks shotController.armed (buffer capturing + processor
// idle), so a disarmed click is a no-op and the hover frame / pointer cursor do
// not appear. The placement site still owns the OUTER visibility gate (session
// screen + Capture mode). Reads the global context props imuManager /
// cameraManager / shotController / controller / appSettings, plus the Theme and
// SessionMode singletons.
//
// Dot tiers: even-more-dim (auto-detect off, or the modality is unavailable —
// e.g. the Ball placeholder / no IMU connected), dim (armed pipeline idle), glow
// (armed & listening, with a breathing halo). A detector firing flashes its dot
// green and decays back to the base tier over 2 s.
//
// The FOURTH dot is not a detector. It reports that a launch monitor reading was
// brought in for the shot on screen, and it LATCHES rather than flashing — see
// lmDot below for why a flash would have been invisible in exactly the case it
// exists to report.

import QtQuick
import PinPointStudio

Item {
    id: root

    readonly property bool armed:   shotController.armed
    readonly property bool hovered: hoverMa.containsMouse && armed

    implicitWidth:  cluster.implicitWidth + Theme.sp(28)
    implicitHeight: Theme.sp(40)

    // Hover frame — fades in when armed & hovered (the click affordance).
    Rectangle {
        id: frame
        anchors.fill: parent
        radius: Theme.radius
        color:        root.hovered ? Theme.colorBg3 : "transparent"
        border.width: 1
        border.color: root.hovered ? Theme.colorBorderMid : "transparent"
        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
    }

    // Accent flash on fire — a dedicated overlay so the animation never fights
    // the hover binding on `frame.border.color`. Mirrors the SHOT button's brief
    // accent border flash (fast in, slow out).
    Rectangle {
        id: flashFrame
        anchors.fill: parent
        radius: Theme.radius
        color: "transparent"
        border.width: 1
        border.color: Theme.colorAccent
        opacity: 0
        SequentialAnimation {
            id: shotFlash
            NumberAnimation { target: flashFrame; property: "opacity"
                              to: 1; duration: Theme.durationFast }
            NumberAnimation { target: flashFrame; property: "opacity"
                              to: 0; duration: Theme.durationSlow }
        }
    }

    Row {
        id: cluster
        anchors.centerIn: parent
        spacing: Theme.sp(8)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("DETECT")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
        }
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.sp(10)
            DetectDot { id: imuDot;  available: imuManager.imuCount > 0 }
            DetectDot { id: acDot;   available: true }
            // Ball/vision dot (design §8.3): steady green core while the ball is
            // present at the hitting area (v2 temporal detector). The shot flash
            // joins when the Source::Ball candidate feed lands.
            DetectDot {
                id: ballDot
                readonly property QtObject foInst: {
                    var insts = cameraManager.instances
                    for (var i = 0; i < insts.length; ++i)
                        if (insts[i].perspective === CameraInstance.FaceOn)
                            return insts[i]
                    return null
                }
                available: foInst !== null && foInst.ballEnabled
                presence:  foInst !== null && foInst.ballPresent
            }
            // Launch monitor: this shot's device reading has been read and written.
            //
            // NOT A DETECTOR, and it differs from its three neighbours twice over.
            // It does not follow auto-detect (that setting governs how a SHOT is
            // spotted; the monitor is not spotting anything), and it LATCHES instead
            // of flashing. The latch is the important one: a reading lands while
            // analysis is still running, when shotController.armed is false and the
            // whole cluster has yielded its slot to the ANALYSING badge — so a 2 s
            // flash would fire against a hidden widget every single time. Held
            // instead, the dot is still green when the cluster comes back, which is
            // also the more useful statement: "this shot has monitor data".
            DetectDot {
                id: lmDot
                available: launchMonitor.configured
                isDetector: false
                presence: root.lmReceived
            }
        }
    }

    // Click = manual shot (replaces the toolbar SHOT button). The central
    // shotController.triggerShot() gate matches `armed`, so a disarmed click is a
    // no-op even if the binding lags; `enabled` also suppresses hover/pointer.
    PpPressable {
        id: hoverMa
        enabled: root.armed
        onClicked: {
            shotController.triggerShot()
            if (!Theme.reduceMotion) shotFlash.restart()
        }
    }

    // ── Launch monitor arrival ─────────────────────────────────────────────
    // Latched for the shot on screen, cleared when the next one is committed.
    property bool lmReceived: false

    // A short, low ting when a reading is folded into the shot. Deliberately two
    // octaves below the C8 shot chime it follows by a few seconds, and quieter — it
    // is a confirmation, not an event, and it must not compete with the shot itself.
    // Gated on a monitor being configured AND on the user leaving it switched on.
    TingPlayer { id: lmTing; frequency: 1046.5; volume: 0.35 }

    Connections {
        target: launchMonitor
        function onReadingApplied(swingDir) {
            root.lmReceived = true
            if (launchMonitor.configured && appSettings.launchMonitorChimeEnabled)
                lmTing.play()
        }
    }
    Connections {
        target: shotController
        function onShotDetected(source, timestampUs, sessionType) { root.lmReceived = false }
    }

    // ── Detection-dot flashes ──────────────────────────────────────────────
    // Each detector firing flashes its dot green; DetectDot.triggerFlash()
    // self-gates on the dot being armed (capturing + auto-detect on), so a
    // detector firing while disarmed leaves the dot at its dim tier. Acoustic
    // onsets are emitted on the audio thread — the queued QML connection
    // marshals them onto the GUI thread.
    Connections {
        target: imuManager
        function onImpactDetected(estImpactUs, confidence) { imuDot.triggerFlash() }
    }
    Connections {
        target: controller   // TranscriptionController owns the acoustic detector
        function onImpactDetected(estImpactUs, confidence) { acDot.triggerFlash() }
    }

    // ── Shot-detection indicator dot ────────────────────────────────────────
    // One per detection modality. Brightness tiers: even-more-dim (auto-detect
    // off, or unavailable — e.g. the Ball placeholder / no IMU connected), dim
    // (armed pipeline idle), glow (armed & listening, with a breathing halo). A
    // detection flashes the dot green and decays to the base tier over 2 s. Call
    // triggerFlash() to fire it.
    component DetectDot: Item {
        id: dd
        property bool   available: true
        // Whether this dot reports a SHOT DETECTOR. True for the three that do, and
        // it ties them to the capture pipeline twice: they go dim when auto-detect is
        // off (that setting is exactly what they do), and they are only "armed" while
        // the pipeline is idle and listening.
        //
        // False for the launch monitor, and both halves matter. It keeps reading its
        // file however shots are being triggered, and — the load-bearing one — it must
        // stay lit while the processor is busy, because that is precisely when a
        // reading arrives.
        property bool   isDetector: true
        property real   flash:     0     // 1 just after a detection → 0
        // Core/halo colour tier — Theme.colorAttention marks a modality that
        // is running but needs attention (e.g. ball detection uncalibrated).
        property color  baseColor: Theme.colorAccent
        // Steady-good core while the modality's subject is present (ball seen
        // at the hitting area). Armed tier only.
        property bool   presence:  false

        readonly property bool _live:  available && (!dd.isDetector || appSettings.autoDetectSwing)
        readonly property bool armed:  _live && (!dd.isDetector || shotController.armed)
        readonly property real _alpha: armed ? 1.0 : (_live ? 0.38 : 0.14)

        implicitWidth: Theme.sp(8); implicitHeight: Theme.sp(8)

        function triggerFlash() {
            if (!armed) return
            _decay.stop(); _rmHold.stop()
            flash = 1
            if (Theme.reduceMotion) _rmHold.restart(); else _decay.restart()
        }

        // Breathing halo — armed only.
        Rectangle {
            anchors.centerIn: parent
            width: Theme.sp(15); height: width; radius: width / 2
            visible: dd.armed
            color: Qt.rgba(dd.baseColor.r, dd.baseColor.g, dd.baseColor.b, 0.22)
            SequentialAnimation on scale {
                running: dd.armed && !Theme.reduceMotion
                loops: Animation.Infinite
                NumberAnimation { to: 1.3; duration: Theme.durationSlow }
                NumberAnimation { to: 1.0; duration: Theme.durationSlow }
            }
        }
        // Core dot — colour tier fades between states.
        Rectangle {
            anchors.fill: parent; radius: width / 2
            color: dd.presence && dd.armed
                   ? Qt.rgba(Theme.colorGood.r, Theme.colorGood.g, Theme.colorGood.b, 0.95)
                   : Qt.rgba(dd.baseColor.r, dd.baseColor.g, dd.baseColor.b, dd._alpha)
            Behavior on color { ColorAnimation { duration: Theme.durationNormal } }
        }
        // Green detection flash, decays to reveal the core tier beneath.
        Rectangle {
            anchors.fill: parent; radius: width / 2
            color: Theme.colorGood
            opacity: dd.flash
        }
        NumberAnimation { id: _decay; target: dd; property: "flash"; to: 0; duration: 2000 }
        Timer { id: _rmHold; interval: 2000; onTriggered: dd.flash = 0 }   // reduce-motion: hold then clear
    }
}
