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

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtMultimedia
import PinPointStudio

Item {
    id: root

    // Index of the camera row whose ROI panel is currently open (-1 = none).
    property int openRoiIndex: -1
    // Index of the camera whose ball-detection (hitting area + calibration)
    // panel is expanded; -1 = none. Same pattern as openRoiIndex.
    property int openBallIndex: -1
    // True when opening the ball panel connected the camera itself — closing
    // the panel then restores the disconnected state (settings should not
    // silently change connection state).
    property bool ballAutoConnected: false

    function closeBallPanel() {
        var idx = openBallIndex
        if (idx === -1) return
        openBallIndex = -1
        if (ballAutoConnected) {
            ballAutoConnected = false
            cameraManager.setSelected(idx, false)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — reusable toggle pill
    // ─────────────────────────────────────────────────────────────────────────
    component TogglePill: Rectangle {
        id: pill
        property bool checked: false
        signal toggled(bool value)

        width:  Theme.sp(34)
        height: Theme.sp(18)
        radius: Theme.sp(9)
        color:  pill.checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            width:  Theme.sp(12)
            height: Theme.sp(12)
            radius: Theme.sp(6)
            color:  "white"
            anchors.verticalCenter: parent.verticalCenter
            x: pill.checked ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            Behavior on x { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    pill.toggled(!pill.checked)
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Inline component — camera device row
    // ─────────────────────────────────────────────────────────────────────────
    component CameraDeviceRow: Item {
        id: camRow

        property var camData: ({})  // one entry from cameraManager.cameraList

        // The selected CameraInstance for this camera (null when not selected).
        readonly property var realInstance: {
            var instances = cameraManager.instances
            for (var i = 0; i < instances.length; ++i) {
                if (instances[i].cameraKey === camData.cameraKey)
                    return instances[i]
            }
            return null
        }

        // The preview-only instance behind BOTH the row tile and the crop
        // editor.  ⚠ One instance, not two: they name the same PPCP Source, and
        // two would collide on the deterministic capture Stream id and stop
        // each other (VideoInputPpcp::reclaimStream).  syncPreview() moves the
        // sink between the two VideoOutputs instead.
        property var localPreviewInstance: null

        // Is the row tile actually showing something?
        readonly property bool rowPreviewLive:
            localPreviewInstance !== null
            && localPreviewInstance.lastPreviewError === ""
            && localPreviewInstance.cameraFps > 0

        // Effective instance. While the crop editor is open the camera is
        // guaranteed disconnected (the open flow stops capture and deselects
        // it), so the editor always binds to the full-sensor preview-only
        // instance — a camera reconnected elsewhere can't hijack the editor.
        readonly property var instance: roiOpen ? localPreviewInstance
                                                : (realInstance !== null ? realInstance
                                                                         : localPreviewInstance)

        readonly property bool roiOpen: root.openRoiIndex === camData.index
        readonly property bool ballOpen: root.openBallIndex === camData.index

        // Effective fps for storage calculations:
        // priority: user-set target → live measured configuredFps → capability maxFps → 30
        readonly property double currentFps: {
            var stored = appSettings.cameraTargetFps[camData.cameraKey]
            if (stored !== undefined && stored > 0) return stored
            if (camRow.instance && camRow.instance.configuredFps > 0)
                return camRow.instance.configuredFps
            return camData.maxFps > 0 ? camData.maxFps : 30
        }

        implicitHeight: camRow.roiOpen
                            ? headerRow.height + bodyRow.height + roiPanel.height
                            : headerRow.height + (camData.enabled ? bodyRow.height : excludedNote.height)
                              + (camRow.ballOpen ? ballPanel.height : 0)

        Behavior on implicitHeight { NumberAnimation { duration: Theme.durationFast } }

        opacity: camData.enabled ? 1.0 : 0.5
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

        // Background fill — drawn first, behind all content
        Rectangle {
            anchors.fill: parent
            color:        Theme.colorSurface
            radius:       Theme.radius
        }

        clip: true

        // Border overlay — drawn last via z:100 so content never occludes it
        Rectangle {
            anchors.fill: parent
            color:        "transparent"
            border.width: 1
            border.color: (camRow.roiOpen || camRow.ballOpen)
                            ? Theme.colorAccent
                            : (camData.enabled ? Theme.colorBorderStrong : Theme.colorBorderMid)
            radius:       Theme.radius
            z:            100
            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }
        }

        // ── Header row ───────────────────────────────────────────────────────
        RowLayout {
            id: headerRow
            anchors.top:   parent.top
            anchors.left:  parent.left
            anchors.right: parent.right
            anchors.margins: Theme.sp(14)
            height: Theme.sp(54)
            spacing: Theme.sp(10)

            // Status dot
            Rectangle {
                width:  Theme.sp(6)
                height: Theme.sp(6)
                radius: Theme.sp(3)
                color: camRow.realInstance && camRow.realInstance.isRecording
                            ? Theme.colorGood
                            : (camData.enabled ? Theme.colorWarn : Theme.colorText3)
                Layout.alignment: Qt.AlignVCenter
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }

            // ── The live tile, for a phone, from the moment it connects ──────
            //
            // CORE 5.11.2 calls setup and framing preview's MAIN use, and a
            // preview that only appears once someone opens the crop editor is
            // useless for it — by then the framing decision has been made
            // blind.  We ask the phone for preview at `declare` and
            // PpcpHostService holds a consumer per Source from then on, so by
            // the time this row exists the pictures are already arriving.
            //
            // ⚠ PPCP ONLY (`camData.isPpcp`).  An industrial camera is not
            // started to fill a thumbnail — see camera_manager.cpp beside the
            // flag.
            Rectangle {
                id: rowPreviewTile
                visible: camData.isPpcp
                Layout.preferredWidth:  Theme.sp(64)
                Layout.preferredHeight: Theme.sp(36)
                Layout.alignment: Qt.AlignVCenter
                color:        Theme.colorBg
                radius:       Theme.sp(3)
                border.width: 1
                border.color: Theme.colorBorder
                clip:         true

                VideoOutput {
                    id: rowVideoOutput
                    anchors.fill:    parent
                    anchors.margins: 1
                    fillMode:        VideoOutput.PreserveAspectFit
                }

                // ⚠ Says WHY there is no picture rather than showing black.
                // "no peer attached" is the ordinary state for this backend
                // when the phone has gone, not a fault.
                Text {
                    anchors.centerIn: parent
                    width:            parent.width - Theme.sp(4)
                    visible:          !camRow.rowPreviewLive
                    text:             camRow.localPreviewInstance
                                      && camRow.localPreviewInstance.lastPreviewError !== ""
                                          ? qsTr("no signal") : qsTr("waiting…")
                    font.family:      Theme.fontData
                    font.pixelSize:   Theme.fontSzMicro
                    color:            Theme.colorText3
                    horizontalAlignment: Text.AlignHCenter
                    elide:            Text.ElideRight
                }
            }

            // Alias (editable) + meta
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(2)

                PpTextField {
                    Layout.fillWidth: true
                    placeholderText:  qsTr("Device alias…")
                    text:             camData.alias
                    onEditingFinished: cameraManager.setCameraAlias(camData.cameraKey, text)
                }

                Row {
                    spacing: Theme.sp(10)
                    Text {
                        text:           camData.description || ""
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                    Text {
                        text:           camData.interface || ""
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                    Text {
                        text:           camData.serialNumber || ""
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }
            }

            // Enable toggle
            Row {
                spacing: Theme.sp(6)
                Layout.alignment: Qt.AlignVCenter

                Text {
                    text:           qsTr("Enable")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    anchors.verticalCenter: parent.verticalCenter
                }

                TogglePill {
                    checked: camData.enabled
                    onToggled: (v) => cameraManager.setExcluded(camData.index, !v)
                }
            }
        }

        // ── Excluded note (shown when disabled) ──────────────────────────────
        Item {
            id: excludedNote
            anchors.top:   headerRow.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height: camData.enabled ? 0 : implicitContentHeight + Theme.sp(14)
            visible: !camData.enabled
            clip: true

            readonly property real implicitContentHeight: excText.implicitHeight + Theme.sp(14)

            Text {
                id: excText
                anchors {
                    left:        parent.left
                    right:       parent.right
                    top:         parent.top
                    leftMargin:  Theme.sp(14)
                    rightMargin: Theme.sp(14)
                    topMargin:   Theme.sp(4)
                }
                text:           qsTr("Excluded — will not appear in view assignment or consume capture resources.")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                font.italic:    true
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }
        }

        // ── Body row (visible when enabled) ──────────────────────────────────
        RowLayout {
            id: bodyRow
            anchors.top:    headerRow.bottom
            anchors.left:   parent.left
            anchors.right:  parent.right
            anchors.leftMargin:   Theme.sp(14)
            anchors.rightMargin:  Theme.sp(14)
            anchors.bottomMargin: Theme.sp(28)
            height: camData.enabled ? implicitHeight + Theme.sp(28) : 0
            visible: camData.enabled
            spacing: Theme.sp(16)
            clip: true

            // View selector ──────────────────────────────────────────────────
            ColumnLayout {
                spacing: Theme.sp(4)
                Layout.alignment: Qt.AlignTop

                Text {
                    text:           qsTr("VIEW")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:          Theme.colorText3
                }

                PpComboBox {
                    id: viewCombo
                    implicitWidth: Theme.sp(168)

                    // Any number of cameras may share a perspective (e.g. two
                    // face-on cameras in one session).
                    readonly property var viewOptions: [
                        { label: qsTr("— Unassigned —"), perspective: CameraInstance.None },
                        { label: qsTr("Face-on"),         perspective: CameraInstance.FaceOn },
                        { label: qsTr("Down-the-line"),   perspective: CameraInstance.DownTheLine },
                        { label: qsTr("Other"),           perspective: CameraInstance.Other }
                    ]

                    model: viewOptions.map(function(o) { return o.label })

                    Component.onCompleted: {
                        var p = appSettings.cameraPerspective[camData.cameraKey] || 0
                        for (var i = 0; i < viewOptions.length; i++) {
                            if (viewOptions[i].perspective === p) { currentIndex = i; break }
                        }
                    }

                    Connections {
                        target: appSettings
                        function onCameraPerspectiveChanged() {
                            var p = appSettings.cameraPerspective[camData.cameraKey] || 0
                            for (var i = 0; i < viewCombo.viewOptions.length; i++) {
                                if (viewCombo.viewOptions[i].perspective === p) {
                                    viewCombo.currentIndex = i; break
                                }
                            }
                        }
                    }

                    onActivated: (idx) => {
                        var p = viewOptions[idx].perspective
                        var map = appSettings.cameraPerspective
                        if (p !== CameraInstance.None) map[camData.cameraKey] = p
                        else delete map[camData.cameraKey]
                        appSettings.cameraPerspective = map
                        if (camRow.realInstance)
                            cameraManager.setPerspective(camRow.realInstance, p)
                    }
                }
            }

            // Mirrored toggle ─────────────────────────────────────────────────
            ColumnLayout {
                spacing: Theme.sp(4)
                Layout.alignment: Qt.AlignTop

                Text {
                    text:           qsTr("IMAGE")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:          Theme.colorText3
                }

                Rectangle {
                    id: mirrorChip
                    width:  mirrorLabel.implicitWidth + Theme.sp(16)
                    height: Theme.sp(24)
                    radius: Theme.radius

                    readonly property bool isMirrored: (appSettings.cameraIsMirrored[camData.cameraKey] === true)

                    color:        isMirrored ? Theme.colorAccent
                                : mirrorPress.containsMouse
                                    ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.6)
                                    : Theme.colorSurface
                    border.width: 1
                    border.color: isMirrored ? Theme.colorAccent
                                : mirrorPress.containsMouse ? Theme.colorAccentMid
                                : Theme.colorBorderMid
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        id: mirrorLabel
                        anchors.centerIn: parent
                        text:           qsTr("Mirrored")
                        color:          mirrorChip.isMirrored ? Theme.colorBg : Theme.colorText2
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Font.Normal
                    }

                    PpPressable {
                        id: mirrorPress
                        onClicked: {
                            var map = appSettings.cameraIsMirrored
                            if (mirrorChip.isMirrored)
                                delete map[camData.cameraKey]
                            else
                                map[camData.cameraKey] = true
                            appSettings.cameraIsMirrored = map
                            if (camRow.realInstance)
                                cameraManager.setIsMirrored(camRow.realInstance, !mirrorChip.isMirrored)
                        }
                    }
                }
            }

            // Frame rate chips ────────────────────────────────────────────────
            ColumnLayout {
                spacing: Theme.sp(4)
                Layout.alignment: Qt.AlignTop

                Text {
                    text:           qsTr("FRAME RATE")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:          Theme.colorText3
                }

                Row {
                    spacing: Theme.sp(4)

                    readonly property var fpsOptions: {
                        var opts = []
                        var max = camData.maxFps > 0 ? camData.maxFps : 60
                        if (max >= 30) opts.push(30)
                        if (max >= 60 && max > 30) opts.push(60)
                        if (max >= 120 && max > 60) opts.push(120)
                        if (max > 0 && opts.indexOf(max) < 0) opts.push(max)
                        return opts
                    }

                    readonly property double selectedFps: {
                        var v = appSettings.cameraTargetFps[camData.cameraKey]
                        return (v !== undefined && v > 0) ? v : (camData.maxFps > 0 ? camData.maxFps : 60)
                    }

                    Repeater {
                        model: parent.fpsOptions

                        delegate: Rectangle {
                            id: fpsChip
                            required property var modelData

                            readonly property bool isSelected: Math.abs(parent.selectedFps - modelData) < 0.5

                            width:  fpsLabel.implicitWidth + Theme.sp(20)
                            height: Theme.sp(24)
                            radius: Theme.radius
                            color:  isSelected ? Theme.colorAccentLight
                                  : fpsArea.containsMouse
                                      ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                      : "transparent"
                            border.width: 1
                            border.color: isSelected ? Theme.colorAccent
                                        : fpsArea.containsMouse ? Theme.colorAccentMid
                                        : Theme.colorBorderStrong
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                            Text {
                                id: fpsLabel
                                anchors.centerIn: parent
                                text:           (Math.round(modelData * 10) / 10) + qsTr(" fps")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          fpsChip.isSelected ? Theme.colorAccent : Theme.colorText2
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            MouseArea {
                                id: fpsArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked: {
                                    var map = appSettings.cameraTargetFps
                                    map[camData.cameraKey] = modelData
                                    appSettings.cameraTargetFps = map
                                    cameraManager.setTargetFps(camData.index, modelData)
                                }
                            }
                        }
                    }
                }
            }

            // Trigger chips ───────────────────────────────────────────────────
            ColumnLayout {
                spacing: Theme.sp(4)
                Layout.alignment: Qt.AlignTop

                Text {
                    text:           qsTr("TRIGGER")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:          Theme.colorText3
                }

                Row {
                    spacing: Theme.sp(4)

                    readonly property string selectedMode: {
                        var v = appSettings.cameraTriggerMode[camData.cameraKey]
                        return v !== undefined ? v : "freerun"
                    }

                    Repeater {
                        model: camData.hwTrigger
                                ? [{ label: qsTr("Free-run"), mode: "freerun" },
                                   { label: qsTr("HW sync"),  mode: "hwsync"  }]
                                : [{ label: qsTr("Free-run"), mode: "freerun" }]

                        delegate: Rectangle {
                            id: trigChip
                            required property var modelData

                            readonly property bool isSelected: parent.selectedMode === modelData.mode

                            width:  trigLabel.implicitWidth + Theme.sp(20)
                            height: Theme.sp(24)
                            radius: Theme.radius
                            color:  isSelected ? Theme.colorAccentLight
                                  : trigArea.containsMouse
                                      ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                      : "transparent"
                            border.width: 1
                            border.color: isSelected ? Theme.colorAccent
                                        : trigArea.containsMouse ? Theme.colorAccentMid
                                        : Theme.colorBorderStrong
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                            Text {
                                id: trigLabel
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          trigChip.isSelected ? Theme.colorAccent : Theme.colorText2
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            MouseArea {
                                id: trigArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked: {
                                    var map = appSettings.cameraTriggerMode
                                    map[camData.cameraKey] = modelData.mode
                                    appSettings.cameraTriggerMode = map
                                    cameraManager.setTriggerMode(camData.index, modelData.mode)
                                }
                            }
                        }
                    }
                }
            }

            // ── Fixed in place toggle ────────────────────────────────────────
            ColumnLayout {
                spacing: Theme.sp(4)
                Layout.alignment: Qt.AlignTop

                Text {
                    text:                qsTr("FIXED IN PLACE")
                    font.family:         Theme.fontData
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                RowLayout {
                    spacing: Theme.sp(6)

                    TogglePill {
                        readonly property bool fixedVal: {
                            var v = appSettings.cameraFixedInPlace[camData.cameraKey]
                            return v !== undefined ? !!v : false
                        }
                        checked: fixedVal
                        onToggled: (v) => {
                            var map = appSettings.cameraFixedInPlace
                            map[camData.cameraKey] = v
                            appSettings.cameraFixedInPlace = map
                        }
                    }

                    Text {
                        text: {
                            var v = appSettings.cameraFixedInPlace[camData.cameraKey]
                            return (v !== undefined && !!v) ? qsTr("Yes") : qsTr("No")
                        }
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText2
                        Layout.alignment: Qt.AlignVCenter
                    }
                }

                Text {
                    text:           qsTr("Camera is attached to the wall or immovable.")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.italic:    true
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                    Layout.preferredWidth: Theme.sp(160)
                }
            }

            Item { Layout.fillWidth: true }

            // Action buttons ─────────────────────────────────────────────────
            Row {
                spacing:  Theme.sp(6)
                Layout.alignment: Qt.AlignVCenter | Qt.AlignRight

                // Set crop — always available (setRoi() is a software crop on every camera)
                Rectangle {
                    id: cropBtn
                    visible: true
                    width:  cropLabel.implicitWidth + Theme.sp(24)
                    height: Theme.sp(26)
                    radius: Theme.radius
                    color:  camRow.roiOpen ? Theme.colorAccentLight
                          : cropPress.containsMouse
                              ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                              : "transparent"
                    border.width: 1
                    border.color: camRow.roiOpen ? Theme.colorAccent
                                : cropPress.containsMouse ? Theme.colorAccentMid
                                : Theme.colorBorderStrong
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        id: cropLabel
                        anchors.centerIn: parent
                        text:           qsTr("Set crop")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          camRow.roiOpen ? Theme.colorAccent : Theme.colorText2
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    PpPressable {
                        id: cropPress
                        onClicked: {
                            if (camRow.roiOpen) {
                                root.openRoiIndex = -1
                                return
                            }
                            // The crop editor must preview the FULL sensor,
                            // and the crop only applies at connect — so stop
                            // any capture and disconnect this camera before
                            // opening. NOTE: setSelected() rebuilds the
                            // Repeater delegates SYNCHRONOUSLY, destroying
                            // this delegate's QML context mid-handler — after
                            // that, every unqualified name lookup throws a
                            // ReferenceError, even for objects that outlive
                            // the row. Snapshot everything into JS locals
                            // FIRST; locals survive context destruction.
                            var mgr         = cameraManager
                            var toast       = cropToast
                            var panelRoot   = root
                            var idx         = camData.index
                            var wasSelected = camData.selected
                            var wasActive   = mgr.isRecording || wasSelected
                            var notice      = qsTr("Capture stopped — crop editing previews the full sensor")
                            if (mgr.isRecording)
                                mgr.stopAll()
                            if (wasSelected)
                                mgr.setSelected(idx, false)
                            if (wasActive)
                                toast.show(notice)
                            panelRoot.closeBallPanel()
                            panelRoot.openRoiIndex = idx
                        }
                    }
                }

                // Ball detection — hitting area + in-situ calibration
                // (design §8.1). Face-on cameras only; the profile only
                // persists for a fixed-in-place camera, which the panel
                // explains inline.
                Rectangle {
                    id: ballBtn
                    visible: camData.perspective === CameraInstance.FaceOn
                    width:  ballLabel.implicitWidth + Theme.sp(24)
                    height: Theme.sp(26)
                    radius: Theme.radius
                    color:  camRow.ballOpen ? Theme.colorAccentLight
                          : ballPress.containsMouse
                              ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                              : "transparent"
                    border.width: 1
                    border.color: camRow.ballOpen ? Theme.colorAccent
                                : ballPress.containsMouse ? Theme.colorAccentMid
                                : Theme.colorBorderStrong
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        id: ballLabel
                        anchors.centerIn: parent
                        text:           qsTr("Ball detection")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          camRow.ballOpen ? Theme.colorAccent : Theme.colorText2
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    PpPressable {
                        id: ballPress
                        onClicked: {
                            if (camRow.ballOpen) {
                                root.closeBallPanel()
                                return
                            }
                            // Calibration needs the LIVE pipeline (the ball
                            // detector only exists on buffer-backed
                            // instances), so opening connects the camera via
                            // the normal path — the wizard's Connect ditto.
                            // setSelected rebuilds the Repeater delegates
                            // SYNCHRONOUSLY (see the crop button above):
                            // snapshot locals first.
                            var mgr         = cameraManager
                            var panelRoot   = root
                            var idx         = camData.index
                            var wasSelected = camData.selected
                            panelRoot.closeBallPanel()   // restore any other row first
                            panelRoot.openRoiIndex = -1
                            panelRoot.ballAutoConnected = !wasSelected
                            if (!wasSelected) {
                                mgr.setSelected(idx, true)
                                if (!mgr.isRecording && mgr.anySelected)
                                    mgr.startAll()
                            }
                            panelRoot.openBallIndex = idx
                        }
                    }
                }

            }
        }

        // ── ROI panel ────────────────────────────────────────────────────────
        Item {
            id: roiPanel
            anchors.top:   bodyRow.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height: camRow.roiOpen ? roiPanelContent.implicitHeight : 0
            clip: true
            visible: camRow.roiOpen

            Behavior on height { NumberAnimation { duration: Theme.durationFast } }

            ColumnLayout {
                id: roiPanelContent
                anchors.left:  parent.left
                anchors.right: parent.right
                spacing: 0

                // Top border separator
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color:  Theme.colorBorderMid
                    opacity: Theme.borderOpacityNormal
                }

                // ── Camera capabilities strip ────────────────────────────────
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Repeater {
                        model: [
                            { key: qsTr("Sensor"),         val: (camData.vendorName || "") + " " + (camData.modelName || "") },
                            { key: qsTr("Max resolution"),  val: (camData.maxWidth || 0) + " × " + (camData.maxHeight || 0) + " px" },
                            { key: qsTr("Pixel format"),    val: camData.pixelFormat || "—" },
                            { key: qsTr("Bit depth"),       val: (camData.bitsPerPixel || 0) + " bit" },
                            { key: qsTr("Bytes / px"),      val: ((camData.bitsPerPixel || 0) / 8.0).toFixed(2) }
                        ]

                        delegate: Rectangle {
                            required property var modelData
                            required property int index

                            Layout.fillWidth: true
                            implicitHeight: capCol.implicitHeight + Theme.sp(18)
                            color: Theme.colorBg2

                            // Vertical separator (except last)
                            Rectangle {
                                anchors.right:  parent.right
                                anchors.top:    parent.top
                                anchors.bottom: parent.bottom
                                width: 1
                                color: Theme.colorBorderMid
                                opacity: Theme.borderOpacityNormal
                                visible: index < 4
                            }

                            ColumnLayout {
                                id: capCol
                                anchors {
                                    left:   parent.left
                                    right:  parent.right
                                    top:    parent.top
                                    leftMargin:  Theme.sp(14)
                                    rightMargin: Theme.sp(14)
                                    topMargin:   Theme.sp(9)
                                }
                                spacing: Theme.sp(3)

                                Text {
                                    text:            modelData.key
                                    font.family:     Theme.fontData
                                    font.pixelSize:  Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingMicro
                                    font.capitalization: Font.AllUppercase
                                    color:           Theme.colorText3
                                }
                                Text {
                                    text:            modelData.val
                                    font.family:     Theme.fontData
                                    font.pixelSize:  Theme.fontSzBody2
                                    color:           Theme.colorText
                                    elide:           Text.ElideRight
                                    Layout.fillWidth: true
                                }
                            }
                        }
                    }
                }

                // ── Preview + controls ───────────────────────────────────────
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin:  Theme.sp(16)
                    Layout.rightMargin: Theme.sp(16)
                    Layout.topMargin:   Theme.sp(16)
                    Layout.bottomMargin: Theme.sp(28)
                    spacing: Theme.sp(20)

                    // Video preview + draggable ROI
                    ColumnLayout {
                        spacing: 0
                        Layout.alignment: Qt.AlignTop

                        // Preview area
                        Rectangle {
                            id: previewRect
                            width:  Theme.sp(510)
                            height: Theme.sp(288)
                            color:  "#080a0c"
                            border.width: 1
                            border.color: Theme.colorBorderMid
                            radius: 2
                            clip: true

                            // ── ROI box pixel positions (bound to instance.cropRoi) ──
                            readonly property real rX: camRow.instance ? camRow.instance.cropRoi.x      * previewRect.width  : 0
                            readonly property real rY: camRow.instance ? camRow.instance.cropRoi.y      * previewRect.height : 0
                            readonly property real rW: camRow.instance && camRow.instance.cropRoi.width  > 0
                                                           ? camRow.instance.cropRoi.width  * previewRect.width  : previewRect.width
                            readonly property real rH: camRow.instance && camRow.instance.cropRoi.height > 0
                                                           ? camRow.instance.cropRoi.height * previewRect.height : previewRect.height

                            // ── Unified drag state ────────────────────────────
                            property string roiDragMode: "none" // none|move|tl|tr|bl|br
                            property real   roiDragStartX: 0
                            property real   roiDragStartY: 0
                            property real   roiOrigX: 0
                            property real   roiOrigY: 0
                            property real   roiOrigW: 0
                            property real   roiOrigH: 0

                            // ── Live video feed ───────────────────────────────
                            VideoOutput {
                                id: settingsVideoOutput
                                anchors.fill: parent
                            }

                            // A failed start is otherwise silent — the tile just stays
                            // black forever.  Most common cause today: a PPCP camera
                            // whose phone isn't connected right now ("PPCP camera: no
                            // peer attached"), which is an ordinary state for that
                            // backend, not a fault — same visual language as
                            // ImusPanel.qml's "Not connected" placeholder.
                            Rectangle {
                                anchors.fill: parent
                                color:        Theme.colorBg
                                visible:      camRow.localPreviewInstance !== null
                                              && camRow.localPreviewInstance.lastPreviewError !== ""

                                Text {
                                    anchors.centerIn: parent
                                    anchors.margins:  Theme.sp(16)
                                    width:             parent.width - Theme.sp(32)
                                    text:              camRow.localPreviewInstance
                                                           ? camRow.localPreviewInstance.lastPreviewError
                                                           : ""
                                    font.family:       Theme.fontData
                                    font.pixelSize:    Theme.fontSzMicro
                                    color:              Theme.colorText3
                                    wrapMode:           Text.WordWrap
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }

                            // Persist ROI changes directly to AppSettings whenever the
                            // instance's cropRoi changes (drag, numeric field, preset).
                            //
                            // ⚠ GUARDED AGAINST A DESTROYED CONTEXT, NOT JUST A NULL
                            // TARGET. `cropRoiChanged` can fire off the C++ side as
                            // `destroyPreviewInstance()` (below) tears the instance
                            // down — the same "setSelected() rebuilds every Repeater
                            // delegate synchronously" hazard the crop button's own
                            // onClicked works around by snapshotting locals first
                            // (see its comment). This handler has no "before" to
                            // snapshot from — it only runs REACTIVELY.
                            //
                            // ⚠ `typeof appSettings === "undefined"` is NOT a safe
                            // guard here, and was tried first — it only protects
                            // against an identifier that was never DECLARED. Once
                            // the enclosing QML context itself is mid-destruction
                            // (this row's Repeater delegate torn down while its
                            // preview instance's last cropRoiChanged is still in
                            // flight — see reclaimStream()'s note in
                            // VideoInputPpcp.cpp for how that race arises), even a
                            // `typeof` lookup throws, because the scope-chain
                            // resolution itself is what's broken, not the identifier.
                            // try/catch is the one thing that catches an exception
                            // regardless of why the lookup failed — confirmed
                            // necessary 25 Aug 2026: the typeof guard alone still
                            // logged "ReferenceError: appSettings is not defined"
                            // here on a real PPCP camera's crop editor.
                            function persistRoi() {
                                try {
                                    if (!camData.cameraKey) return
                                    var roi = camRow.instance.cropRoi
                                    var map = appSettings.cameraRoi
                                    if (roi.width > 0 && roi.height > 0)
                                        map[camData.cameraKey] = { x: roi.x, y: roi.y, w: roi.width, h: roi.height }
                                    else
                                        delete map[camData.cameraKey]
                                    appSettings.cameraRoi = map
                                } catch (e) {
                                    // The context this handler needed is gone —
                                    // the row is being torn down and there is
                                    // nothing left here to persist ROI for.
                                }
                            }

                            Connections {
                                target: camRow.instance
                                function onCropRoiChanged() {
                                    // ⛔ NOT WHILE A DRAG IS IN FLIGHT, AND THIS IS WHY
                                    // RESIZING THE CROP BOX WAS ALMOST IMPOSSIBLE.
                                    //
                                    // Writing `appSettings.cameraRoi` emits
                                    // `cameraRoiChanged`, and CameraManager wires that
                                    // straight to `cameraListChanged` (camera_manager.cpp,
                                    // "a crop edit must refresh the list") because
                                    // `cameraList()` derives initialWidth/initialHeight
                                    // from the persisted crop.  That rebuilds every
                                    // delegate in this Repeater — including THIS
                                    // `previewRect` and the MouseArea holding the mouse
                                    // grab — so the drag died on the first pixel and the
                                    // operator had to re-press for each one.
                                    //
                                    // ⚠ AND IT IS WHY MOVING WORKED WHILE RESIZING DID
                                    // NOT.  Qt leaves a Repeater's delegates standing when
                                    // a row's CONTENTS have not changed: a move alters x/y
                                    // only, which no list field carries, so the rebuild was
                                    // a no-op.  A resize changes the crop-derived
                                    // initialWidth/initialHeight, the row genuinely
                                    // differs, and the delegates go.
                                    //
                                    // ⚠ AND IT TOOK THE PREVIEW WITH IT.  The teardown ran
                                    // `Component.onDestruction` → `destroyPreviewInstance()`
                                    // → `stopCapture()`, which is where the four
                                    // "the device closed the preview stream (not_needed)"
                                    // lines in Mark's 31 Aug log came from.
                                    //
                                    // The gesture is persisted once, on release.  A
                                    // numeric field, a preset or a clear still lands here
                                    // and is written immediately.
                                    if (previewRect.roiDragMode !== "none") return
                                    previewRect.persistRoi()
                                }
                            }

                            // Wire/clear the settings sink and start/stop the full-sensor
                            // preview-only instance (no event buffer, no pose pipeline, no
                            // crop) with the panel. A plain function — not just Connections —
                            // because opening the panel deselects the camera, which rebuilds
                            // every Repeater delegate: the rebuilt row is created with
                            // roiOpen already true, so onRoiOpenChanged never fires and
                            // Component.onCompleted must run the same wiring.
                            // ⚠ ONE INSTANCE, TWO PLACES TO DRAW IT.  A phone's
                            // row keeps a preview alive whenever the panel is
                            // open, so the tile in the header is never blank
                            // waiting for a round trip; opening the crop editor
                            // moves the SAME instance's sink to the big
                            // VideoOutput rather than making a second one,
                            // which would collide on the Stream id and stop the
                            // first.  An industrial camera keeps the old
                            // behaviour exactly: nothing runs until the editor
                            // opens.
                            function syncPreview() {
                                var want = camRow.roiOpen || camData.isPpcp
                                if (want) {
                                    if (!camRow.localPreviewInstance)
                                        camRow.localPreviewInstance = cameraManager.createPreviewInstance(camData.index)
                                    var inst = camRow.localPreviewInstance
                                    if (!inst) return
                                    if (camRow.roiOpen) {
                                        // Seed a default crop the first time the editor opens
                                        var r = inst.cropRoi
                                        if (r.width <= 0 || r.height <= 0)
                                            inst.setCropRoi(Qt.rect(0.3, 0.0, 0.4, 1.0))
                                        inst.setSettingsSink(settingsVideoOutput.videoSink)
                                    } else {
                                        inst.setSettingsSink(rowVideoOutput.videoSink)
                                    }
                                    inst.startPreview()
                                } else if (camRow.localPreviewInstance) {
                                    camRow.localPreviewInstance.setSettingsSink(null)
                                    camRow.localPreviewInstance.stopPreview()
                                    cameraManager.destroyPreviewInstance(camRow.localPreviewInstance)
                                    camRow.localPreviewInstance = null
                                }
                            }

                            Component.onCompleted: syncPreview()
                            Component.onDestruction: {
                                if (camRow.localPreviewInstance) {
                                    camRow.localPreviewInstance.setSettingsSink(null)
                                    camRow.localPreviewInstance.stopPreview()
                                    cameraManager.destroyPreviewInstance(camRow.localPreviewInstance)
                                    camRow.localPreviewInstance = null
                                }
                            }

                            Connections {
                                target: camRow
                                function onRoiOpenChanged() { previewRect.syncPreview() }
                            }

                            // ── Overlay shades around the ROI ─────────────────
                            readonly property bool hasRoi: camRow.instance
                                                        && camRow.instance.cropRoi.width > 0

                            Rectangle {   // left
                                x: 0; y: 0
                                width:  previewRect.rX
                                height: previewRect.height
                                color:  "black"; opacity: 0.52
                                visible: previewRect.hasRoi
                            }
                            Rectangle {   // right
                                x: previewRect.rX + previewRect.rW; y: 0
                                width:  previewRect.width - (previewRect.rX + previewRect.rW)
                                height: previewRect.height
                                color:  "black"; opacity: 0.52
                                visible: previewRect.hasRoi
                            }
                            Rectangle {   // top
                                x: previewRect.rX; y: 0
                                width:  previewRect.rW
                                height: previewRect.rY
                                color:  "black"; opacity: 0.52
                                visible: previewRect.hasRoi
                            }
                            Rectangle {   // bottom
                                x: previewRect.rX
                                y: previewRect.rY + previewRect.rH
                                width:  previewRect.rW
                                height: previewRect.height - (previewRect.rY + previewRect.rH)
                                color:  "black"; opacity: 0.52
                                visible: previewRect.hasRoi
                            }

                            // ── ROI outline + corner handles (visual only) ────
                            Item {
                                id: roiVisual
                                x:      previewRect.rX
                                y:      previewRect.rY
                                width:  previewRect.rW
                                height: previewRect.rH
                                visible: previewRect.hasRoi
                                z: 5

                                Rectangle {
                                    anchors.fill: parent
                                    color:        Theme.colorAccent
                                    opacity:      0.05
                                }
                                Rectangle {
                                    anchors.fill:  parent
                                    color:        "transparent"
                                    border.width:  1.5
                                    border.color:  Theme.colorAccent
                                }

                                // Corner handle squares (visual only — drag handled by the MouseArea below)
                                readonly property int hs: Theme.sp(10)
                                Rectangle { width: parent.hs; height: parent.hs; x: -parent.hs/2;              y: -parent.hs/2;              color: Theme.colorAccent; border.width:1; border.color:"black" }
                                Rectangle { width: parent.hs; height: parent.hs; x: parent.width-parent.hs/2;  y: -parent.hs/2;              color: Theme.colorAccent; border.width:1; border.color:"black" }
                                Rectangle { width: parent.hs; height: parent.hs; x: -parent.hs/2;              y: parent.height-parent.hs/2; color: Theme.colorAccent; border.width:1; border.color:"black" }
                                Rectangle { width: parent.hs; height: parent.hs; x: parent.width-parent.hs/2;  y: parent.height-parent.hs/2; color: Theme.colorAccent; border.width:1; border.color:"black" }
                            }

                            // ── Single unified drag MouseArea ─────────────────
                            // Covers the entire preview so the mouse is never "outside"
                            // its active area during a fast drag.
                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                preventStealing: true
                                z: 20

                                // Handle hit radius (larger than the 10px visual square)
                                readonly property real hr: Theme.sp(16)

                                function hitZone(mx, my) {
                                    if (!previewRect.hasRoi) return "none"
                                    var rx = previewRect.rX, ry = previewRect.rY
                                    var rw = previewRect.rW, rh = previewRect.rH
                                    var h  = hr
                                    if (Math.abs(mx - rx)      < h && Math.abs(my - ry)      < h) return "tl"
                                    if (Math.abs(mx - (rx+rw)) < h && Math.abs(my - ry)      < h) return "tr"
                                    if (Math.abs(mx - rx)      < h && Math.abs(my - (ry+rh)) < h) return "bl"
                                    if (Math.abs(mx - (rx+rw)) < h && Math.abs(my - (ry+rh)) < h) return "br"
                                    if (mx > rx && mx < rx+rw && my > ry && my < ry+rh)           return "move"
                                    return "none"
                                }

                                cursorShape: {
                                    switch (hitZone(mouseX, mouseY)) {
                                    case "tl": case "br": return Qt.SizeFDiagCursor
                                    case "tr": case "bl": return Qt.SizeBDiagCursor
                                    case "move":          return Qt.SizeAllCursor
                                    default:              return Qt.CrossCursor
                                    }
                                }

                                onPressed: (mouse) => {
                                    previewRect.roiDragMode   = hitZone(mouse.x, mouse.y)
                                    previewRect.roiDragStartX = mouse.x
                                    previewRect.roiDragStartY = mouse.y
                                    if (camRow.instance && previewRect.hasRoi) {
                                        var r = camRow.instance.cropRoi
                                        previewRect.roiOrigX = r.x
                                        previewRect.roiOrigY = r.y
                                        previewRect.roiOrigW = r.width
                                        previewRect.roiOrigH = r.height
                                    }
                                }

                                onPositionChanged: (mouse) => {
                                    if (previewRect.roiDragMode === "none") return
                                    if (!camRow.instance) return
                                    // All deltas are computed from press-start in normalised coords
                                    var dx = (mouse.x - previewRect.roiDragStartX) / previewRect.width
                                    var dy = (mouse.y - previewRect.roiDragStartY) / previewRect.height
                                    var ox = previewRect.roiOrigX, oy = previewRect.roiOrigY
                                    var ow = previewRect.roiOrigW, oh = previewRect.roiOrigH
                                    var nx, ny, nw, nh
                                    switch (previewRect.roiDragMode) {
                                    case "move":
                                        nx = Math.max(0, Math.min(1.0 - ow, ox + dx))
                                        ny = Math.max(0, Math.min(1.0 - oh, oy + dy))
                                        camRow.instance.setCropRoi(Qt.rect(nx, ny, ow, oh))
                                        break
                                    case "tl":
                                        nx = Math.max(0,   Math.min(ox + ow - 0.02, ox + dx))
                                        ny = Math.max(0,   Math.min(oy + oh - 0.02, oy + dy))
                                        nw = Math.max(0.02, ow - (nx - ox))
                                        nh = Math.max(0.02, oh - (ny - oy))
                                        camRow.instance.setCropRoi(Qt.rect(nx, ny, nw, nh))
                                        break
                                    case "tr":
                                        ny = Math.max(0,   Math.min(oy + oh - 0.02, oy + dy))
                                        nw = Math.max(0.02, Math.min(1.0 - ox, ow + dx))
                                        nh = Math.max(0.02, oh - (ny - oy))
                                        camRow.instance.setCropRoi(Qt.rect(ox, ny, nw, nh))
                                        break
                                    case "bl":
                                        nx = Math.max(0,   Math.min(ox + ow - 0.02, ox + dx))
                                        nw = Math.max(0.02, ow - (nx - ox))
                                        nh = Math.max(0.02, Math.min(1.0 - oy, oh + dy))
                                        camRow.instance.setCropRoi(Qt.rect(nx, oy, nw, nh))
                                        break
                                    case "br":
                                        nw = Math.max(0.02, Math.min(1.0 - ox, ow + dx))
                                        nh = Math.max(0.02, Math.min(1.0 - oy, oh + dy))
                                        camRow.instance.setCropRoi(Qt.rect(ox, oy, nw, nh))
                                        break
                                    }
                                }

                                onReleased: {
                                    // The one write for the whole gesture — see the
                                    // cropRoiChanged handler above for why it is not
                                    // per-pixel.  ⚠ AFTER clearing the mode, so the
                                    // handler that fires on the way through does not
                                    // skip it.
                                    const wasDragging = previewRect.roiDragMode !== "none"
                                    previewRect.roiDragMode = "none"
                                    if (wasDragging) previewRect.persistRoi()
                                }
                            }

                            // Live badge
                            Rectangle {
                                anchors.top:        parent.top
                                anchors.left:       parent.left
                                anchors.topMargin:  Theme.sp(8)
                                anchors.leftMargin: Theme.sp(8)
                                implicitWidth:  liveBadgeRow.implicitWidth + Theme.sp(16)
                                implicitHeight: Theme.sp(18)
                                color:  Qt.rgba(26/255, 74/255, 46/255, 0.9)
                                border.width: 1
                                border.color: Theme.colorGood
                                radius: 2
                                z: 25

                                Row {
                                    id: liveBadgeRow
                                    anchors.centerIn: parent
                                    spacing: Theme.sp(4)

                                    Rectangle {
                                        width:  Theme.sp(5); height: Theme.sp(5); radius: Theme.sp(3)
                                        color:  Theme.colorGood
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        text:           qsTr("Live")
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorGood
                                    }
                                }
                            }

                            // FPS badge — hidden when fps is unknown (0)
                            Text {
                                anchors.top:         parent.top
                                anchors.right:       parent.right
                                anchors.topMargin:   Theme.sp(8)
                                anchors.rightMargin: Theme.sp(8)
                                readonly property double displayFps: {
                                    if (camRow.instance && camRow.instance.configuredFps > 0)
                                        return camRow.instance.configuredFps
                                    return camData.maxFps > 0 ? camData.maxFps : 0
                                }
                                visible: displayFps > 0
                                text:    (Math.round(displayFps * 10) / 10).toFixed(1) + qsTr(" fps")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Qt.rgba(1, 1, 1, 0.3)
                                z: 25
                            }
                        }

                        // Footer below preview
                        Rectangle {
                            width:  Theme.sp(340)
                            height: Theme.sp(26)
                            color:  Theme.colorSurface
                            border.width: 1
                            border.color: Theme.colorBorderMid
                            radius: 0

                            RowLayout {
                                anchors {
                                    fill:        parent
                                    leftMargin:  Theme.sp(10)
                                    rightMargin: Theme.sp(10)
                                }

                                Text {
                                    text: {
                                        var w = camData.maxWidth  || 0
                                        var h = camData.maxHeight || 0
                                        return w + " × " + h + " px (full)"
                                    }
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: {
                                        if (!camRow.instance || camRow.instance.cropRoi.width <= 0)
                                            return ""
                                        var r  = camRow.instance.cropRoi
                                        var rw = Math.round(r.width  * (camData.maxWidth  || 0))
                                        var rh = Math.round(r.height * (camData.maxHeight || 0))
                                        return rw + " × " + rh + " px (ROI)"
                                    }
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorAccent
                                }
                            }
                        }
                    }

                    // ── Numeric inputs + storage ─────────────────────────────
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(10)
                        Layout.alignment: Qt.AlignTop

                        // Origin inputs
                        ColumnLayout {
                            spacing: Theme.sp(4)

                            Text {
                                text:           qsTr("ORIGIN (px)")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:          Theme.colorText3
                            }

                            RowLayout {
                                spacing: Theme.sp(6)

                                Text {
                                    text:  "X"
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: Theme.colorText3
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Rectangle {
                                    implicitWidth:  Theme.sp(70)
                                    implicitHeight: Theme.sp(24)
                                    color:  Theme.colorSurface
                                    border.width: 1
                                    border.color: xField.activeFocus ? Theme.colorAccent : Theme.colorBorderStrong
                                    radius: Theme.radius
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    TextInput {
                                        id: xField
                                        anchors { fill: parent; leftMargin: Theme.sp(8); rightMargin: Theme.sp(8) }
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                        text: camRow.instance ? Math.round(camRow.instance.cropRoi.x * (camData.maxWidth || 0)).toString() : "0"
                                        onEditingFinished: {
                                            if (!camRow.instance) return
                                            var val = parseInt(text) || 0
                                            var r   = camRow.instance.cropRoi
                                            var nx  = Math.max(0, Math.min(1.0 - r.width, val / (camData.maxWidth || 1)))
                                            camRow.instance.setCropRoi(Qt.rect(nx, r.y, r.width, r.height))
                                        }
                                    }
                                }

                                Text {
                                    text:  "Y"
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: Theme.colorText3
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Rectangle {
                                    implicitWidth:  Theme.sp(70)
                                    implicitHeight: Theme.sp(24)
                                    color:  Theme.colorSurface
                                    border.width: 1
                                    border.color: yField.activeFocus ? Theme.colorAccent : Theme.colorBorderStrong
                                    radius: Theme.radius
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    TextInput {
                                        id: yField
                                        anchors { fill: parent; leftMargin: Theme.sp(8); rightMargin: Theme.sp(8) }
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                        text: camRow.instance ? Math.round(camRow.instance.cropRoi.y * (camData.maxHeight || 0)).toString() : "0"
                                        onEditingFinished: {
                                            if (!camRow.instance) return
                                            var val = parseInt(text) || 0
                                            var r   = camRow.instance.cropRoi
                                            var ny  = Math.max(0, Math.min(1.0 - r.height, val / (camData.maxHeight || 1)))
                                            camRow.instance.setCropRoi(Qt.rect(r.x, ny, r.width, r.height))
                                        }
                                    }
                                }
                            }
                        }

                        // Size inputs
                        ColumnLayout {
                            spacing: Theme.sp(4)

                            Text {
                                text:           qsTr("SIZE (px)")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:          Theme.colorText3
                            }

                            RowLayout {
                                spacing: Theme.sp(6)

                                Text {
                                    text:  "W"
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: Theme.colorText3
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Rectangle {
                                    implicitWidth:  Theme.sp(70)
                                    implicitHeight: Theme.sp(24)
                                    color:  Theme.colorSurface
                                    border.width: 1
                                    border.color: wField.activeFocus ? Theme.colorAccent : Theme.colorBorderStrong
                                    radius: Theme.radius
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    TextInput {
                                        id: wField
                                        anchors { fill: parent; leftMargin: Theme.sp(8); rightMargin: Theme.sp(8) }
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                        text: camRow.instance ? Math.round(camRow.instance.cropRoi.width  * (camData.maxWidth  || 0)).toString() : (camData.maxWidth  || 0).toString()
                                        onEditingFinished: {
                                            if (!camRow.instance) return
                                            var val = parseInt(text) || (camData.maxWidth || 0)
                                            var r   = camRow.instance.cropRoi
                                            var nw  = Math.max(0.01, Math.min(1.0 - r.x, val / (camData.maxWidth || 1)))
                                            camRow.instance.setCropRoi(Qt.rect(r.x, r.y, nw, r.height))
                                        }
                                    }
                                }

                                Text {
                                    text:  "H"
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: Theme.colorText3
                                    Layout.alignment: Qt.AlignVCenter
                                }

                                Rectangle {
                                    implicitWidth:  Theme.sp(70)
                                    implicitHeight: Theme.sp(24)
                                    color:  Theme.colorSurface
                                    border.width: 1
                                    border.color: hField.activeFocus ? Theme.colorAccent : Theme.colorBorderStrong
                                    radius: Theme.radius
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    TextInput {
                                        id: hField
                                        anchors { fill: parent; leftMargin: Theme.sp(8); rightMargin: Theme.sp(8) }
                                        verticalAlignment: TextInput.AlignVCenter
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText
                                        text: camRow.instance ? Math.round(camRow.instance.cropRoi.height * (camData.maxHeight || 0)).toString() : (camData.maxHeight || 0).toString()
                                        onEditingFinished: {
                                            if (!camRow.instance) return
                                            var val = parseInt(text) || (camData.maxHeight || 0)
                                            var r   = camRow.instance.cropRoi
                                            var nh  = Math.max(0.01, Math.min(1.0 - r.y, val / (camData.maxHeight || 1)))
                                            camRow.instance.setCropRoi(Qt.rect(r.x, r.y, r.width, nh))
                                        }
                                    }
                                }
                            }
                        }

                        // Preset buttons
                        ColumnLayout {
                            spacing: Theme.sp(4)

                            Text {
                                text:           qsTr("PRESETS")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:          Theme.colorText3
                            }

                            Row {
                                spacing: Theme.sp(4)

                                Repeater {
                                    model: [
                                        { label: qsTr("Full frame"),    action: "full"    },
                                        { label: qsTr("Default crop"),  action: "default" },
                                        { label: qsTr("16:9"),          action: "16:9"    }
                                    ]

                                    delegate: Rectangle {
                                        id: presetChip
                                        required property var modelData

                                        width:  presetLabel.implicitWidth + Theme.sp(20)
                                        height: Theme.sp(24)
                                        radius: Theme.radius
                                        color:  presetArea.containsMouse
                                                    ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                                    : "transparent"
                                        border.width: 1
                                        border.color: presetArea.containsMouse ? Theme.colorAccentMid : Theme.colorBorderStrong
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                        Text {
                                            id: presetLabel
                                            anchors.centerIn: parent
                                            text:           modelData.label
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzBody2
                                            font.weight:    Theme.fontBodyWeight
                                            color:          Theme.colorText2
                                        }

                                        MouseArea {
                                            id: presetArea
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape:  Qt.PointingHandCursor
                                            onClicked: {
                                                if (!camRow.instance) return
                                                switch (modelData.action) {
                                                case "full":
                                                    camRow.instance.setCropRoi(Qt.rect(0, 0, 1, 1))
                                                    break
                                                case "default":
                                                    camRow.instance.setCropRoi(Qt.rect(0.3, 0, 0.4, 1.0))
                                                    break
                                                case "16:9": {
                                                    var mw = camData.maxWidth  || 1
                                                    var mh = camData.maxHeight || 1
                                                    var h  = mw * 9.0 / 16.0 / mh
                                                    var y  = (1.0 - h) / 2.0
                                                    camRow.instance.setCropRoi(Qt.rect(0, y, 1.0, h))
                                                    break
                                                }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // Storage block
                        Rectangle {
                            id: storageRect
                            Layout.fillWidth: true
                            implicitHeight:   storageCol.implicitHeight + Theme.sp(24)
                            color:  Theme.colorBg2
                            border.width: 1
                            border.color: Theme.colorBorderStrong
                            radius: Theme.radius

                            // Slot sizing matches CameraInstance's ring buffer allocation exactly.
                            // slotBytesPerPixel = worst-case BPP (min 2), slotWidth/Height = max resolution.
                            readonly property int  slotBpp:   camData.slotBytesPerPixel || 2
                            readonly property int  slotW:     camData.slotWidth  || camData.maxWidth  || 0
                            readonly property int  slotH:     camData.slotHeight || camData.maxHeight || 0
                            readonly property real slotBytes: slotW * slotH * slotBpp

                            // Full-frame display values use the default (actual) format for the caption.
                            readonly property real bpp:       (camData.bitsPerPixel > 0 ? camData.bitsPerPixel : 8) / 8.0
                            readonly property real fullBytes: slotBytes  // slot = full frame at worst-case BPP

                            // ROI crop dimensions (normalised → pixels using default resolution for display).
                            readonly property real roiCropW:  camRow.instance && camRow.instance.cropRoi.width  > 0
                                                                ? camRow.instance.cropRoi.width  * (camData.maxWidth  || 0)
                                                                : (camData.maxWidth  || 0)
                            readonly property real roiCropH:  camRow.instance && camRow.instance.cropRoi.height > 0
                                                                ? camRow.instance.cropRoi.height * (camData.maxHeight || 0)
                                                                : (camData.maxHeight || 0)
                            // ROI bytes uses the slot BPP so the saving is relative to the allocated slot.
                            readonly property real roiBytes:  roiCropW * roiCropH * slotBpp
                            readonly property int  framePct:  fullBytes > 0 ? Math.round(roiBytes / fullBytes * 100) : 100

                            // Slot count mirrors SourceRing: nextPow2(ceil(fps × 5)).
                            readonly property double slotFps: (camData.slotFps > 0 ? camData.slotFps : camRow.currentFps) || 60
                            readonly property int    slotCount: {
                                var n = Math.ceil(slotFps * 5.0)
                                if (n <= 0) return 1
                                --n
                                n |= n >> 1; n |= n >> 2; n |= n >> 4; n |= n >> 8; n |= n >> 16
                                return n + 1
                            }
                            readonly property real   ringBytes:     roiBytes  * slotCount
                            readonly property real   ringFullBytes: fullBytes * slotCount
                            readonly property int    ringPct:       ringFullBytes > 0 ? Math.round(ringBytes / ringFullBytes * 100) : 100

                            ColumnLayout {
                                id: storageCol
                                anchors {
                                    fill:        parent
                                    margins:     Theme.sp(12)
                                }
                                spacing: Theme.sp(10)

                                // Frame storage bar
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.sp(4)

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text:           qsTr("FRAME STORAGE (ROI vs FULL FRAME)")
                                            font.family:    Theme.fontData
                                            font.pixelSize: Theme.fontSzMicro
                                            font.letterSpacing: Theme.trackingMicro
                                            font.capitalization: Font.AllUppercase
                                            color:          Theme.colorText3
                                            Layout.fillWidth: true
                                        }
                                        Text {
                                            text:           storageRect.framePct + "%"
                                            font.family:    Theme.fontData
                                            font.pixelSize: Theme.fontSzMicro
                                            color:          Theme.colorAccent
                                        }
                                    }

                                    Text {
                                        text: {
                                            var roiMb  = (storageRect.roiBytes  / 1048576).toFixed(2)
                                            var fullMb = (storageRect.fullBytes / 1048576).toFixed(2)
                                            return qsTr("ROI: ") + roiMb + qsTr(" MB/frame  (full frame: ") + fullMb + qsTr(" MB)")
                                        }
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText2
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: Theme.sp(4)
                                        radius: Theme.sp(2)
                                        color:  Theme.colorBg3

                                        Rectangle {
                                            width:  parent.width * Math.min(1, storageRect.framePct / 100.0)
                                            height: parent.height
                                            radius: parent.radius
                                            color:  Theme.colorAccent
                                            Behavior on width { NumberAnimation { duration: Theme.durationFast } }
                                        }
                                    }
                                }

                                // Ring buffer bar
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.sp(4)

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text:           qsTr("RING BUFFER  ")
                                                            + storageRect.slotCount
                                                            + qsTr(" SLOTS  (")
                                                            + Math.round(storageRect.slotFps)
                                                            + qsTr(" fps × 5 s → nextPow2)")
                                            font.family:    Theme.fontData
                                            font.pixelSize: Theme.fontSzMicro
                                            font.letterSpacing: Theme.trackingMicro
                                            font.capitalization: Font.AllUppercase
                                            color:          Theme.colorText3
                                            Layout.fillWidth: true
                                        }
                                        Text {
                                            text:           storageRect.ringPct + "%"
                                            font.family:    Theme.fontData
                                            font.pixelSize: Theme.fontSzMicro
                                            color:          Theme.colorGood
                                        }
                                    }

                                    Text {
                                        text: {
                                            var frames   = storageRect.slotCount
                                            var roiMb    = (storageRect.roiBytes     / 1048576).toFixed(2)
                                            var ringMb   = (storageRect.ringBytes    / 1048576).toFixed(0)
                                            var ringFull = (storageRect.ringFullBytes / 1048576).toFixed(0)
                                            return frames + qsTr(" slots × ") + roiMb + qsTr(" MB = ") + ringMb + qsTr(" MB  (full: ") + ringFull + qsTr(" MB)")
                                        }
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText2
                                    }

                                    Rectangle {
                                        Layout.fillWidth: true
                                        height: Theme.sp(4)
                                        radius: Theme.sp(2)
                                        color:  Theme.colorBg3

                                        Rectangle {
                                            width:  parent.width * Math.min(1, storageRect.ringPct / 100.0)
                                            height: parent.height
                                            radius: parent.radius
                                            color:  Theme.colorGood
                                            Behavior on width { NumberAnimation { duration: Theme.durationFast } }
                                        }
                                    }
                                }
                            }
                        }

                        // How the crop is applied
                        Text {
                            text: qsTr("Crop is applied when the camera connects — in hardware where the sensor supports it, otherwise on arriving frames. The preview always shows the full sensor; changes take effect on the next connect.")
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            font.italic:    true
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                            Layout.fillWidth: true
                        }

                        // Reset / done buttons
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(6)

                            Rectangle {
                                id: resetBtn
                                width:  resetLabel.implicitWidth + Theme.sp(20)
                                height: Theme.sp(26)
                                radius: Theme.radius
                                color:  resetPress.containsMouse
                                            ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                            : "transparent"
                                border.width: 1
                                border.color: resetPress.containsMouse ? Theme.colorAccentMid : Theme.colorBorderStrong
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                Text {
                                    id: resetLabel
                                    anchors.centerIn: parent
                                    text:           qsTr("Reset to full frame")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    font.weight:    Theme.fontBodyWeight
                                    color:          Theme.colorText2
                                }

                                PpPressable {
                                    id: resetPress
                                    onClicked: {
                                        if (camRow.instance)
                                            camRow.instance.setCropRoi(Qt.rect(0, 0, 1, 1))
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true }

                            Rectangle {
                                id: roiDoneBtn
                                width:  doneLabel.implicitWidth + Theme.sp(24)
                                height: Theme.sp(26)
                                radius: Theme.radius
                                color:  roiDonePress.containsMouse ? Qt.lighter(Theme.colorAccentLight, 1.08) : Theme.colorAccentLight
                                border.width: 1
                                border.color: Theme.colorAccent
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                Text {
                                    id: doneLabel
                                    anchors.centerIn: parent
                                    text:           qsTr("Done")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    font.weight:    Font.Normal
                                    color:          Theme.colorAccent
                                }

                                PpPressable {
                                    id: roiDonePress
                                    onClicked:    root.openRoiIndex = -1
                                }
                            }
                        }
                    }
                }
            }
        }

        // ── Ball detection panel — hitting area + calibration (design §8.1) ──
        Item {
            id: ballPanel
            anchors.top:   roiPanel.bottom
            anchors.left:  parent.left
            anchors.right: parent.right
            height: camRow.ballOpen ? ballPanelContent.implicitHeight : 0
            clip: true
            visible: camRow.ballOpen

            Behavior on height { NumberAnimation { duration: Theme.durationFast } }

            readonly property bool fixedInPlace:
                appSettings.cameraFixedInPlace[camData.cameraKey] === true

            ColumnLayout {
                id: ballPanelContent
                anchors.left:  parent.left
                anchors.right: parent.right
                spacing: Theme.sp(12)

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 1
                    color:  Theme.colorBorderMid
                    opacity: Theme.borderOpacityNormal
                }

                // Camera not connected — everything here needs live frames.
                Text {
                    visible: camRow.realInstance === null
                    Layout.fillWidth: true
                    Layout.margins: Theme.sp(16)
                    text: qsTr("Connecting the camera\u2026 If this persists, check the camera is not in use elsewhere.")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color: Theme.colorText2
                }

                RowLayout {
                    visible: camRow.realInstance !== null
                    Layout.fillWidth: true
                    Layout.margins: Theme.sp(16)
                    Layout.topMargin: Theme.sp(4)
                    Layout.bottomMargin: Theme.sp(28)
                    spacing: Theme.sp(16)

                    // Live view with the hitting-area overlay; drag to draw.
                    // Loader-gated so a collapsed panel never subscribes to
                    // the video stream.
                    Loader {
                        id: ballFrameLoader
                        Layout.preferredWidth: Theme.sp(450)
                        Layout.preferredHeight: Theme.sp(285)
                        Layout.alignment: Qt.AlignTop
                        active: camRow.ballOpen && camRow.realInstance !== null
                        // Seed a default hitting area the first time the
                        // editor opens, so a manipulable rect is there
                        // immediately (crop-editor pattern). Never touches a
                        // calibrated camera (those always have an area).
                        onLoaded: {
                            var inst = camRow.realInstance
                            if (inst && inst.roi.width <= 0)
                                cameraManager.setBallRoi(inst, Qt.rect(0.40, 0.55, 0.20, 0.30))
                        }
                        sourceComponent: PpCameraFrame {
                            instance: camRow.realInstance
                            displayName: camData.alias || camData.description
                            showPoseOverlay:      false
                            showHittingArea:      true
                            roiEditable:          true
                            showPerspectiveBadge: false
                            showStatsOverlay:     false
                            showReplayOverlay:    false
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.sp(8)

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Place the hitting area over where the ball sits at address: drag the rectangle to move it, drag a corner to resize, or drag outside it to draw a new one. Then empty the mat and press Learn.")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorText2
                        }

                        Text {
                            visible: !ballPanel.fixedInPlace
                            Layout.fillWidth: true
                            text: qsTr("This camera isn't marked fixed in place — the hitting area will not be restored next session.")
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            font.italic: true
                            color: Theme.colorWarn
                        }

                        // v2 temporal detector (Option A): learn the empty-mat
                        // baseline, then a placed ball is detected live in the
                        // hitting-area overlay above. Empty the mat, click, then
                        // place a ball. (The start-session wizard has the guided
                        // version of this flow.)
                        Text {
                            visible: camRow.realInstance !== null
                            text: qsTr("→ Learn hitting area (empty the mat, then place a ball)")
                            font.family: Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color: Theme.colorAccent
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: cameraManager.relearnBallBaseline(camRow.realInstance)
                            }
                        }
                    }
                }
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // ── Search scroll-to support ──────────────────────────────────────────────

    property string lastHighlightId: ""

    function findChild(parent, name) {
        for (var i = 0; i < parent.children.length; i++) {
            var child = parent.children[i]
            if (child.objectName === name) return child
            var found = findChild(child, name)
            if (found) return found
        }
        return null
    }

    function scrollToItem(itemId) {
        if (!itemId) return true
        var target = findChild(contentCol, itemId)
        if (!target) return false
        var mapped = target.mapToItem(contentCol, 0, 0)
        scrollView.contentItem.contentY = Math.max(0, Math.min(
            mapped.y - Theme.sp(24),
            scrollView.contentItem.contentHeight - scrollView.height
        ))
        target.searchHighlight = true
        lastHighlightId = itemId
        highlightTimer.restart()
        return true
    }

    Timer {
        id: highlightTimer
        interval: 1800
        onTriggered: {
            var target = findChild(contentCol, lastHighlightId)
            if (target) target.searchHighlight = false
        }
    }

    // Informational notice shown when opening the crop editor stops an
    // active capture / disconnects the camera. Lives on the panel root so it
    // survives the Repeater delegate rebuild triggered by setSelected().
    PpToast {
        id: cropToast
        showUndo: false
        glyph: "⚠"
        z: 1000
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.sp(24)
    }

    // Main scroll view
    // ─────────────────────────────────────────────────────────────────────────
    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: contentCol.y + contentCol.implicitHeight + Theme.sp(28)

        ColumnLayout {
            id: contentCol
            x: Theme.sp(32)
            y: Theme.sp(28)
            width: parent.width - Theme.sp(64)
            spacing: Theme.sp(16)

            // ── Page header ────────────────────────────────────────────────
            Text {
                text: qsTr("HARDWARE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color: Theme.colorText3
            }

            PpDisplayText {
                text: qsTr("Cameras")
            }

            Text {
                text: qsTr("All detected cameras are listed below. Enable each device, assign it to a view, configure capture parameters, and optionally define a crop region to reduce frame storage and ring buffer size.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Detected cameras section ───────────────────────────────────
            RowLayout {
                Layout.fillWidth: true

                Text {
                    text:           qsTr("DETECTED CAMERAS")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:          Theme.colorText3
                    Layout.fillWidth: true
                }

                PpButton {
                    label: qsTr("Refresh")
                    onClicked: cameraManager.enumerate()
                }
            }

            // Camera rows
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.sp(26)
                spacing: Theme.sp(8)

                Repeater {
                    model: cameraManager.cameraList

                    delegate: CameraDeviceRow {
                        required property var modelData
                        camData: modelData
                        Layout.fillWidth: true
                    }
                }

                // Empty state
                Text {
                    visible: cameraManager.cameraList.length === 0
                    text:    qsTr("No cameras detected. Connect a camera and click Refresh.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    font.italic:    true
                    color:          Theme.colorText3
                    Layout.fillWidth: true
                }
            }

            PpDivider { orientation: Qt.Horizontal; Layout.fillWidth: true }

            // ── Global capture section ─────────────────────────────────────
            Text {
                text:           qsTr("GLOBAL CAPTURE")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:          Theme.colorText3
            }

            // Pre-roll buffer row
            RowLayout {
                objectName: "setting_preroll"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)

                    Text {
                        text:           qsTr("Pre-roll buffer")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Seconds of frames held before swing trigger — directly sets ring buffer size above")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                // Chip group for preroll values
                Row {
                    spacing: Theme.sp(4)
                    Layout.alignment: Qt.AlignVCenter

                    readonly property var prerollOptions: [
                        { label: qsTr("0.5 s"), value: 0.5 },
                        { label: qsTr("1.0 s"), value: 1.0 },
                        { label: qsTr("2.0 s"), value: 2.0 }
                    ]

                    Repeater {
                        model: parent.prerollOptions

                        delegate: Rectangle {
                            id: prerollChip
                            required property var modelData

                            readonly property bool isSelected: Math.abs(appSettings.cameraPreroll - modelData.value) < 0.01

                            width:  prerollLabel.implicitWidth + Theme.sp(20)
                            height: Theme.sp(24)
                            radius: Theme.radius
                            color:  isSelected ? Theme.colorAccentLight
                                  : prerollPress.containsMouse
                                      ? Qt.rgba(Theme.colorAccentLight.r, Theme.colorAccentLight.g, Theme.colorAccentLight.b, 0.4)
                                      : "transparent"
                            border.width: 1
                            border.color: isSelected ? Theme.colorAccent
                                        : prerollPress.containsMouse ? Theme.colorAccentMid
                                        : Theme.colorBorderStrong
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                            Text {
                                id: prerollLabel
                                anchors.centerIn: parent
                                text:           modelData.label
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          prerollChip.isSelected ? Theme.colorAccent : Theme.colorText2
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            PpPressable {
                                id: prerollPress
                                onClicked:    appSettings.cameraPreroll = modelData.value
                            }
                        }
                    }
                }
            }

            // Synchronise cameras row
            RowLayout {
                objectName: "setting_camSync"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)

                    Text {
                        text:           qsTr("Synchronise cameras")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Lock frame timing across all enabled cameras")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                TogglePill {
                    checked: appSettings.cameraSyncEnabled
                    onToggled: (v) => appSettings.cameraSyncEnabled = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }
        }
    }
}
