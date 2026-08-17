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
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick3D.Helpers
import PinPointStudio

Item {
    id: capturePage

    // The first (and in practice only) active IMU instance.
    // ImuVizView and ArmVizView are persistent; only this binding changes.
    property QtObject firstInst: imuManager.instances.length > 0 ? imuManager.instances[0] : null

    // ⚠ WHAT THE CUBE IS BOUND TO IS NOT THE INSTANCE FOR EVERY DEVICE KIND.
    // ImuVizView resolves quat*/accel* by NAME through the metaobject, and an
    // HmInstance has none of them — the wG3's two units each carry their own set
    // (hm_instance.h), because one peripheral holds two sensors that are supposed
    // to disagree. Binding the instance itself produced three "Unable to assign
    // [undefined] to double" warnings per connect, and this page is inside the
    // shell's StackLayout, so its bindings evaluate whether or not anyone ever
    // navigates here. The lower arm is the arbitrary-but-fixed choice for a
    // single cube; ImusPanel is where both units are shown side by side.
    // `unitLowerArm` is undefined on a Witmotion, so that falls through unchanged.
    property QtObject vizController: capturePage.firstInst
                                     && capturePage.firstInst.unitLowerArm
                                        ? capturePage.firstInst.unitLowerArm
                                        : capturePage.firstInst

    // Clear log and backfill any entries already emitted before Connections wired.
    onFirstInstChanged: {
        imuLogModel.clear()
        if (firstInst) {
            var entries = firstInst.logEntries
            for (var i = 0; i < entries.length; ++i)
                imuLogModel.append({ "entry": entries[i] })
            imuLogView.positionViewAtEnd()
        }
    }

    // Wire log entries from the active instance.
    Connections {
        target: capturePage.firstInst
        function onLogEntryAdded(entry) {
            imuLogModel.append({ "entry": entry })
            imuLogView.positionViewAtEnd()
        }
    }

    ListModel { id: imuLogModel }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp(16)
        spacing: Theme.sp(12)

        // ── Header row: title + IMU selector chips ────────────────────────────
        RowLayout {
            spacing: Theme.sp(12)

            Label {
                text: qsTr("IMU")
                color: Theme.colorText
                font.family: Theme.fontBody
                font.pixelSize: Theme.fontSzHeading
                font.weight: Font.Normal
            }

            Item { Layout.fillWidth: true }

            // One toggle chip per discovered IMU — same pattern as VideoPage camera chips.
            Repeater {
                model: imuManager.imuList
                delegate: Rectangle {
                    readonly property bool isConnecting: modelData.connecting
                    readonly property bool isConnected:  modelData.connected

                    // Drop devices absent from the latest scan (powered off / out
                    // of range). A still-selected device stays present, so a
                    // connected IMU that stopped advertising is never hidden.
                    visible: modelData.present
                    width:  imuChipLabel.implicitWidth + Theme.sp(16)
                    height: Theme.sp(24)
                    radius: Theme.radius

                    color: modelData.selected
                           ? (isConnected ? Theme.colorAccent : Theme.colorAccentLight)
                           : Theme.colorSurface
                    border.width: 1
                    border.color: modelData.selected
                                  ? Theme.colorAccent
                                  : Theme.colorBorderMid

                    Behavior on color       { ColorAnimation { duration: Theme.durationFast } }
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    scale: imuChipTap.pressed ? 0.97 : imuChipHover.hovered ? 1.02 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                    Text {
                        id: imuChipLabel
                        anchors.centerIn: parent
                        text: modelData.alias || modelData.description
                        color: (modelData.selected && isConnected)
                               ? Theme.colorBg
                               : modelData.selected
                                 ? Theme.colorAccent
                                 : Theme.colorText2
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Font.Normal
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                    }

                    // Pulsing ring while connecting
                    Rectangle {
                        anchors.fill: parent
                        radius: parent.radius
                        color:  "transparent"
                        border.width: 2
                        border.color: Theme.colorAccent
                        visible: isConnecting
                        SequentialAnimation on opacity {
                            running: isConnecting
                            loops: Animation.Infinite
                            NumberAnimation { to: 0.2; duration: 600; easing.type: Easing.InOutSine }
                            NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutSine }
                        }
                    }

                    TapHandler {
                        id: imuChipTap
                        onTapped: imuManager.setSelected(modelData.index, !modelData.selected)
                    }
                    HoverHandler { id: imuChipHover; cursorShape: Qt.PointingHandCursor }
                }
            }

            // Scan button
            Rectangle {
                id: scanChip
                property bool scanning: false

                width:  scanMeasure.implicitWidth + Theme.sp(20)
                height: Theme.sp(24)
                radius: Theme.radius
                color:  scanning ? Theme.colorAccentLight : "transparent"
                border.width: 1
                border.color: scanning ? Theme.colorAccent : Theme.colorBorderStrong
                Behavior on color       { ColorAnimation { duration: Theme.durationFast } }
                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                scale: scanTap.pressed ? 0.97 : scanHover.hovered ? 1.02 : 1.0
                Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                Text {
                    id: scanMeasure
                    visible: false
                    text: qsTr("Scanning…")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                }

                Text {
                    anchors.centerIn: parent
                    text:           scanChip.scanning ? qsTr("Scanning…") : qsTr("Scan")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color:          scanChip.scanning ? Theme.colorAccent : Theme.colorText2
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }

                Timer {
                    id: scanTimer
                    interval: 30000
                    onTriggered: scanChip.scanning = false
                }

                Connections {
                    target: imuManager
                    function onImuEnumeratedCountChanged() { scanTimer.stop(); scanChip.scanning = false }
                }

                TapHandler {
                    id: scanTap
                    onTapped: {
                        scanChip.scanning = true
                        imuManager.rescanImu()
                        scanTimer.restart()
                    }
                }
                HoverHandler { id: scanHover; cursorShape: Qt.PointingHandCursor }
            }
        }

        // ── Main content area ─────────────────────────────────────────────────
        //
        // ImuVizView (View3D) and ArmVizView (View3D) live here as persistent
        // items whose `controller` property is rebound when the selection changes.
        // They are NEVER destroyed on deselect — only the binding changes to null.
        // This prevents the Qt Quick 3D render-thread heap corruption that occurred
        // when these were inside the Repeater delegate (destroyed on every deselect).
        //
        Item {
            Layout.fillWidth:  true
            Layout.fillHeight: true

            // Persistent view area — visible whenever at least one IMU is selected.
            ColumnLayout {
                anchors.fill: parent
                visible: imuManager.instances.length > 0
                spacing: Theme.sp(6)

                // ── Tab bar ───────────────────────────────────────────────────
                TabBar {
                    id: imuTabBar
                    Layout.fillWidth: true
                    background: Rectangle {
                        color: Theme.colorSurface
                        radius: Theme.radius
                        border.width: 1
                        border.color: Theme.colorBorderMid
                    }

                    TabButton {
                        text: qsTr("Viz")
                        contentItem: Text {
                            text: parent.text
                            color: imuTabBar.currentIndex === 0 ? Theme.colorText : Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            font.weight:    Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment:   Text.AlignVCenter
                        }
                        background: Rectangle {
                            color:  imuTabBar.currentIndex === 0 ? Theme.colorBg3 : "transparent"
                            radius: Theme.radius - 1
                        }
                    }

                    TabButton {
                        text: qsTr("Log")
                        contentItem: Text {
                            text: parent.text
                            color: imuTabBar.currentIndex === 1 ? Theme.colorText : Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            font.weight:    Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment:   Text.AlignVCenter
                        }
                        background: Rectangle {
                            color:  imuTabBar.currentIndex === 1 ? Theme.colorBg3 : "transparent"
                            radius: Theme.radius - 1
                        }
                    }

                    TabButton {
                        text: qsTr("Arm")
                        contentItem: Text {
                            text: parent.text
                            color: imuTabBar.currentIndex === 2 ? Theme.colorText : Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            font.weight:    Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment:   Text.AlignVCenter
                        }
                        background: Rectangle {
                            color:  imuTabBar.currentIndex === 2 ? Theme.colorBg3 : "transparent"
                            radius: Theme.radius - 1
                        }
                    }

                    TabButton {
                        text: qsTr("Body")
                        contentItem: Text {
                            text: parent.text
                            color: imuTabBar.currentIndex === 3 ? Theme.colorText : Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            font.weight:    Font.Normal
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment:   Text.AlignVCenter
                        }
                        background: Rectangle {
                            color:  imuTabBar.currentIndex === 3 ? Theme.colorBg3 : "transparent"
                            radius: Theme.radius - 1
                        }
                    }
                }

                // ── Persistent 3D views ───────────────────────────────────────
                StackLayout {
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    currentIndex: imuTabBar.currentIndex

                    // Viz tab — persistent View3D; controller rebinds, view never destroyed.
                    ImuVizView {
                        controller: capturePage.vizController
                    }

                    // Log tab
                    Rectangle {
                        color:        Theme.colorSurface
                        radius:       Theme.radius
                        border.width: 1
                        border.color: Theme.colorBorderMid

                        ListView {
                            id: imuLogView
                            anchors.fill:    parent
                            anchors.margins: Theme.sp(8)
                            clip:  true
                            model: imuLogModel
                            spacing: 1

                            delegate: Text {
                                required property string entry
                                width:      imuLogView.width
                                text:       entry
                                color:      Theme.colorText
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzLabel
                                wrapMode:   Text.NoWrap
                            }
                        }
                    }

                    // Arm viz tab — persistent View3D; reads imuManager.instances directly.
                    ArmVizView {}

                    // Body viz tab — persistent View3D; Y Bot full-body model.
                    // poseSource wired later when pose adapter is plumbed here.
                    BodyVizView {}
                }

                // ── Per-instance controls (Repeater, no View3D) ───────────────
                // One row of controls per selected IMU. The Repeater only creates
                // lightweight Items — no View3D, no QSG resources at stake.
                Repeater {
                    model: imuManager.instances
                    delegate: RowLayout {
                        property QtObject ctrl: modelData

                        Layout.fillWidth: true
                        spacing: Theme.sp(8)

                        // Status dot
                        Rectangle {
                            width:  Theme.sp(6)
                            height: Theme.sp(6)
                            radius: Theme.sp(3)
                            color: ctrl && ctrl.imuConnected ? Theme.colorGood
                                 : ctrl && ctrl.busy         ? Theme.colorAccent
                                 :                              Theme.colorText3
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        Label {
                            text: {
                                if (!ctrl) return ""
                                if (ctrl.imuConnected && ctrl.dataRateHz > 0)
                                    return ctrl.stateLabel + " · " + ctrl.dataRateHz.toFixed(1) + " Hz"
                                return ctrl.stateLabel
                            }
                            color: ctrl && ctrl.imuConnected ? Theme.colorGood
                                 : ctrl && ctrl.busy         ? Theme.colorWarn
                                 :                              Theme.colorText3
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        Item { Layout.fillWidth: true }

                        // Battery badge
                        Rectangle {
                            visible: ctrl && ctrl.imuConnected && ctrl.batteryPercent >= 0
                            radius: Theme.radius
                            color: ctrl && ctrl.batteryPercent > 60 ? Theme.colorGoodLight
                                 : Theme.colorWarnLight
                            border.width: 1
                            border.color: ctrl && ctrl.batteryPercent > 60
                                          ? Qt.rgba(Theme.colorGood.r, Theme.colorGood.g, Theme.colorGood.b, 0.25)
                                          : Qt.rgba(Theme.colorWarn.r, Theme.colorWarn.g, Theme.colorWarn.b, 0.25)
                            implicitWidth:  batLabel.implicitWidth + Theme.sp(10)
                            implicitHeight: batLabel.implicitHeight + Theme.sp(4)

                            Label {
                                id: batLabel
                                anchors.centerIn: parent
                                text: ctrl ? ("BAT: " + ctrl.batteryPercent + "%") : ""
                                color: ctrl && ctrl.batteryPercent > 60 ? Theme.colorGood : Theme.colorWarn
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                            }
                        }

                        // Rate selector
                        PpComboBox {
                            id: rateCombo
                            enabled: ctrl && ctrl.imuConnected
                            model: [10, 50, 100, 200]
                            displaySuffix: " Hz"
                            currentIndex: {
                                if (!ctrl) return 2
                                var idx = model.indexOf(ctrl.outputRateHz)
                                return idx >= 0 ? idx : 2
                            }
                            onActivated: if (ctrl) ctrl.setOutputRateHz(model[currentIndex])
                            implicitWidth: Theme.sp(90)
                        }

                        // Zero button
                        Button {
                            id: zeroBtn
                            text: qsTr("Zero")
                            enabled: ctrl && ctrl.imuConnected
                            onClicked: if (ctrl) ctrl.zeroOrientation()
                            contentItem: Text {
                                text:  zeroBtn.text
                                color: zeroBtn.enabled ? Theme.colorText2 : Theme.colorText3
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody
                                font.weight:    Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment:   Text.AlignVCenter
                            }
                            background: Rectangle {
                                color:        "transparent"
                                border.width: 1
                                border.color: zeroBtn.enabled ? Theme.colorBorderStrong : Theme.colorBorderMid
                                radius: Theme.radius
                            }
                        }

                        // Save Log button
                        Button {
                            id: saveLogBtn
                            text: qsTr("Save Log")
                            onClicked: if (ctrl) ctrl.saveLog()
                            contentItem: Text {
                                text:  saveLogBtn.text
                                color: Theme.colorText2
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody
                                font.weight:    Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment:   Text.AlignVCenter
                            }
                            background: Rectangle {
                                color:        "transparent"
                                border.width: 1
                                border.color: Theme.colorBorderStrong
                                radius: Theme.radius
                            }
                        }
                    }
                }
            }

            // Placeholder when no IMU is selected.
            Rectangle {
                anchors.fill: parent
                visible: imuManager.instances.length === 0
                color:        Theme.colorBg2
                radius:       Theme.radius
                border.width: 1
                border.color: Theme.colorBorderMid

                Label {
                    anchors.centerIn: parent
                    text:  qsTr("Select an IMU above")
                    color: Theme.colorText3
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                }
            }
        }

    }
}
