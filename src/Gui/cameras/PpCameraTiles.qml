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

// Camera stage-panel — its video source follows the session DATA SOURCE, not the
// mode. Capture is the only live-video mode: it shows one live tile per session-
// enabled camera (plus the degraded-shot ¼× auto-replay metric graph). Review AND
// Analyse both show the LOADED shot's video — one tile per stream of that swing,
// enumerated from the swing's own swing.json (shotReplay.streams), never the local
// camera rig, so a swing recorded on a different setup still plays and the playback
// follows the Review↔Analyse toggle. Hosted by PpModeStage as the "camera" panel.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root
    property int  sessionType: -1
    property bool showHittingArea: false

    // Analyse-only telestrator gutter: a reserved LHS margin that hosts the
    // annotation palette's open/collapse button, so the affordance lives beside
    // the footage rather than on top of it. Reserved whenever the palette can show
    // (regardless of open/closed) so the tiles never reflow as you toggle it.
    readonly property bool _annotate: SessionMode.mode === SessionMode.analyse
                                      && shotReplay.active && shotReplay.streams.length > 0
    property real _leftGutter: _annotate ? Theme.sp(54) : Theme.sp(12)
    Behavior on _leftGutter {
        enabled: !Theme.reduceMotion
        NumberAnimation { duration: Theme.durationNormal; easing.type: Easing.InOutQuad }
    }

    // Live cameras show ONLY in Capture; Review and Analyse both render the loaded
    // shot's disk streams. The fork is data-source-driven (not Review-specific), so
    // one loaded shotReplay carries the video across the Review↔Analyse mode toggle.
    readonly property bool _replay: SessionMode.mode !== SessionMode.capture

    // All session screens stay instantiated in the StackLayout, but a disk-replay
    // stream has ONE QMediaPlayer with ONE sink — so only the ACTIVE screen may
    // create replay tiles (else hidden screens steal the sink and the visible one
    // shows nothing). The session screen sits at StackLayout index sessionType + 1.
    readonly property bool _screenActive: navController.currentIndex === root.sessionType + 1
    readonly property bool _replayHere: _replay && _screenActive

    // Replay tiles are instantiated one event-loop turn AFTER _replayHere goes true.
    // Entering Replay can recreate this whole panel (Capture's "stage" arrangement →
    // Replay's "split" rebuilds PpModeStage's camera Loader), and the panel being torn
    // down would otherwise spawn a transient replay tile — which binds, then dies,
    // churning the disk-replay video sink. Loader destroys the dying panel
    // synchronously during the arrangement change, so deferring arming via Qt.callLater
    // means only the SURVIVING instance ever reaches _replayArmed and creates a tile.
    property bool _replayArmed: false
    function _syncReplayArmed() { root._replayArmed = root._replayHere }
    on_ReplayHereChanged: Qt.callLater(root._syncReplayArmed)
    Component.onCompleted:  Qt.callLater(root._syncReplayArmed)

    readonly property var _liveCameras:
        cameraManager.cameraList.filter(function(c) { return c.sessionEnabled })

    readonly property var _replaySeries:
        (shotProcessor.replayAnalysisDetail && shotProcessor.replayAnalysisDetail.series)
        ? shotProcessor.replayAnalysisDetail.series : []

    RowLayout {
        id: tilesRow
        anchors.fill: parent
        anchors.topMargin: Theme.sp(12)
        anchors.bottomMargin: Theme.sp(12)
        anchors.rightMargin: Theme.sp(12)
        anchors.leftMargin: root._leftGutter   // reserve the telestrator gutter
        spacing: Theme.sp(8)

        // ── Live camera tiles (Capture only) ───────────────────────────────
        Repeater {
            model: root._replay ? [] : root._liveCameras
            delegate: PpCameraFrame {
                required property var modelData
                Layout.fillHeight: true
                Layout.fillWidth: false
                Layout.preferredWidth: height * videoAspect
                // ⚠ READ FROM THE SETTING, NOT FROM THE LIST ROW, AND THAT IS
                // WHAT LETS A CROP EDIT LEAVE THE CAMERA LIST ALONE.
                // `cameraList()` carries crop-derived initialWidth/initialHeight,
                // so keeping a disconnected tile's aspect honest used to mean
                // re-emitting `cameraListChanged` on every crop write — which
                // rebuilt every delegate in every camera Repeater in the
                // application, including the one holding the mouse grab in the
                // crop editor (see CamerasPanel.qml's cropRoiChanged handler) and
                // the Settings tiles whose teardown closed their Streams.  This
                // binding depends on the setting itself, so the tile follows a
                // crop with no list refresh at all.  The row's own values remain
                // the fallback for a camera with no crop stored.
                placeholderAspect: {
                    const roi = appSettings.cameraRoi[modelData.cameraKey]
                    if (roi && roi.w > 0 && roi.h > 0
                        && modelData.maxWidth > 0 && modelData.maxHeight > 0)
                        return (modelData.maxWidth * roi.w) / (modelData.maxHeight * roi.h)
                    return (modelData.initialWidth > 0 && modelData.initialHeight > 0)
                           ? modelData.initialWidth / modelData.initialHeight : 16.0 / 9.0
                }
                instance: {
                    var insts = cameraManager.instances
                    for (var i = 0; i < insts.length; ++i)
                        if (insts[i].cameraKey === modelData.cameraKey) return insts[i]
                    return null
                }
                displayName: modelData.alias !== "" ? modelData.alias : modelData.description
                showHittingArea: root.showHittingArea
                showHittingAreaHint: cameraManager.livePoseEnabled && sessionController.running
                // Live ball circle rides the same live-pose gate as the ROI hint.
                showBallOverlay: cameraManager.livePoseEnabled && sessionController.running
                // Capture-mode ¼× post-shot auto-replay overlay follows the view's
                // Pose overlay toggle (Capture setting == current mode here). Chrome
                // bindings above are left EXACTLY as-is (capture chrome unchanged v1).
                showReplayOverlay: ViewLayout.overlaysOn(SessionMode.mode)
                // Per-element motion modes for that ¼× auto-replay overlay. The
                // Capture guard inside ViewLayout forces every body/shaft element to
                // "off" (ball only), so the live skeleton is untouched by this.
                motionOn:          ViewLayout.motionOn(SessionMode.mode)
                motionModes:       ViewLayout.motionFor(SessionMode.mode).modes
                motionTraceTarget: ViewLayout.motionTraceTarget(SessionMode.mode)
                leadIsLeft:        ViewLayout.leadIsLeft()
            }
        }

        // ── Replay stream tiles (Review / Analyse) — the loaded swing's OWN ─
        // streams. Only the active screen creates these (one sink per stream), and
        // only once arming has settled (see _replayArmed) so a torn-down panel during
        // the Capture→Replay transition never spawns a transient tile.
        Repeater {
            model: root._replayArmed ? shotReplay.streams : []
            delegate: PpCameraFrame {
                required property var modelData
                Layout.fillHeight: true
                Layout.fillWidth: false
                Layout.preferredWidth: height * videoAspect
                replayStreamIndex: modelData.index
                placeholderAspect: modelData.aspect > 0 ? modelData.aspect : 16.0 / 9.0
                displayName: qsTr("Replay")
                showHittingArea: false
                showHittingAreaHint: false
                showPoseOverlay: false        // live-pose canvas — replay uses replayOverlay
                showStatsOverlay: false
                showPerspectiveBadge: false
                // Analyzed pose+shaft+ball overlay follows the view's motion master
                // switch — covers both Replay and Analyse (this Repeater serves both).
                // showReplayOverlay gates the Canvas visibility; motionOn is the same
                // value (master off ⇒ nothing draws), with per-element modes below.
                showReplayOverlay: ViewLayout.motionOn(SessionMode.mode)
                motionOn:          ViewLayout.motionOn(SessionMode.mode)
                motionModes:       ViewLayout.motionFor(SessionMode.mode).modes
                motionTraceTarget: ViewLayout.motionTraceTarget(SessionMode.mode)
                leadIsLeft:        ViewLayout.leadIsLeft()
                // Telestrator is an Analyse-only affordance (not plain Replay).
                annotationsEnabled: SessionMode.mode === SessionMode.analyse
            }
        }

        // Trailing fill absorbs the dead space to the RIGHT of the left-aligned
        // replay tiles (their original layout). Its left edge marks the camera
        // cluster's right boundary, so the telestrator bar can centre over the
        // cluster without altering the tiles. (Capture uses the metric graph as
        // its fillWidth element instead.)
        Item { id: tilesEnd; Layout.fillWidth: true; Layout.fillHeight: true; visible: root._replay }

        // In-replay metric graph — the Capture-mode ¼× auto-replay transient
        // (shotProcessor; degraded shots only). Review/Analyse draw their curves
        // in the charts panel instead.
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            visible: !root._replay
            PpMetricChart {
                anchors.fill: parent; anchors.margins: Theme.sp(8)
                compact:    true        // plot only — no toolbar / brush / summary chrome
                visible:    shotProcessor.isReplaying && root._replaySeries.length > 0
                seriesList: root._replaySeries
                phases:     (shotProcessor.replayAnalysisDetail && shotProcessor.replayAnalysisDetail.phases)
                                ? shotProcessor.replayAnalysisDetail.phases : []
                startUs:    shotProcessor.replayStartUs
                endUs:      shotProcessor.replayEndUs
                impactUs:   shotProcessor.replayImpactUs
                playheadUs: shotProcessor.replayPositionUs
                showPlayhead: true
            }
        }
    }

    // Capture empty-state: no cameras enabled.
    Column {
        anchors.centerIn: parent
        spacing: Theme.sp(6)
        visible: !root._replay && root._liveCameras.length === 0
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("No cameras enabled")
            color: Theme.colorText2; font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Enable cameras in the toolbar's Cameras panel")
            color: Theme.colorText3; font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
        }
    }

    // Review / Analyse empty-state: no swing focused, or the swing has no video.
    Column {
        anchors.centerIn: parent
        spacing: Theme.sp(6)
        visible: root._replayHere && shotReplay.streams.length === 0
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: SessionMode.focusedShotId >= 0 ? qsTr("No video for this swing")
                                                 : qsTr("Select a swing to review")
            color: Theme.colorText2; font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Pick a shot from the filmstrip below")
            color: Theme.colorText3; font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
        }
    }

    // ── Telestrator open button (in the gutter, off the footage) ──────────────
    // A ">" chevron that opens the palette; shown only while collapsed (the
    // matching "<" collapse button lives on the palette itself). Docked top-left
    // in the reserved gutter so the affordance never sits on the video.
    Rectangle {
        id: annoOpen
        visible: root._annotate && !AnnotationTool.paletteOpen
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.leftMargin: Math.max(Theme.sp(4), (root._leftGutter - width) / 2)
        anchors.topMargin: Theme.sp(10)
        width: Theme.sp(32); height: Theme.sp(32)
        radius: Theme.radius
        z: 50
        color: annoOpenMa.containsMouse ? Theme.colorBg2 : "transparent"
        border.width: 1
        border.color: Theme.colorBorderMid
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        PpAnnotationIcon {
            anchors.centerIn: parent
            width: Theme.sp(17); height: Theme.sp(17)
            kind: "chevronRight"
            iconColor: Theme.colorText2
        }
        PpPressable {
            id: annoOpenMa
            onClicked: AnnotationTool.paletteOpen = true
        }
    }

    // ── Telestrator palette (Analyse only) ────────────────────────────────────
    // Centred over the camera cluster (gutter edge → tilesEnd's left edge) so its
    // position reads as "applies to all cameras" — without moving the left-aligned
    // tiles. Drives the AnnotationTool state every tile's PpAnnotationLayer reads.
    PpAnnotationToolbar {
        id: annoBar
        // cluster spans root-x [_leftGutter, _leftGutter + tilesEnd.x - spacing];
        // place this bar's centre at the cluster centre.
        x: root._leftGutter + (tilesEnd.x - tilesRow.spacing - width) / 2
        anchors.top: parent.top
        anchors.topMargin: Theme.sp(10)
        z: 50
        visible: root._annotate && AnnotationTool.paletteOpen
        opacity: visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
    }
}
