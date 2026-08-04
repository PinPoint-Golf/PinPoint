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

// Settings → Launch Monitor (panelIndex 6).
//
// Connect a launch monitor and say where it writes. The first connector reads
// Foresight's LastShot.CSV out of a folder FSX2020 is pointed at.
//
// NO FILESYSTEM WORK ON COMPLETION. This panel is a direct StackLayout child, so it
// is built at launch whether or not anybody opens Settings — see the warning in
// StoragePanel.qml, written after a library walk on this thread cost 5-10 s of black
// window on an SMB mount. Everything shown here is a property of the controller,
// which its own poller maintains off this code path.

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import PinPointStudio

Item {
    id: root

    // ── Search-scroll contract (ScreenSettings → SettingsIndex) ───────────────

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

    // ── Folder dialog ─────────────────────────────────────────────────────────

    FolderDialog {
        id: folderDialog
        title: qsTr("Select the folder FSX2020 writes LastShot.CSV into")
        // urlToLocalFile, never a string strip: QML's url type has no toLocalFile()
        // and trimming "file://" leaves a stray slash before a Windows drive letter,
        // which is exactly the platform this folder usually lives on.
        onAccepted: appSettings.launchMonitorPath = appSettings.urlToLocalFile(selectedFolder)
    }

    // ── Inline component — reusable toggle pill ───────────────────────────────

    component TogglePill: Rectangle {
        id: pill
        property bool checked: false
        property bool enabledPill: true
        signal toggled(bool value)

        width:   Theme.sp(34)
        height:  Theme.sp(18)
        radius:  Theme.sp(9)
        opacity: pill.enabledPill ? 1.0 : 0.4
        color:   pill.checked ? Theme.colorAccent : Theme.colorBg3
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
            enabled:      pill.enabledPill
            cursorShape:  Qt.PointingHandCursor
            onClicked:    pill.toggled(!pill.checked)
        }
    }

    // ── Option model ──────────────────────────────────────────────────────────

    readonly property var deviceOptions: [
        { label: qsTr("None"),                        value: "none"   },
        { label: qsTr("Foresight GC Quad (FSX2020)"), value: "gcquad" }
    ]

    readonly property bool configured: appSettings.launchMonitorKind !== "none"

    readonly property color statusColor:
        launchMonitor.state === "ready"   ? Theme.colorGood
      : launchMonitor.state === "error"   ? Theme.colorError
      : launchMonitor.state === "waiting" ? Theme.colorAttention
      :                                     Theme.colorText3

    // ── Main scroll view ──────────────────────────────────────────────────────

    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        contentHeight: contentCol.y + contentCol.implicitHeight + Theme.sp(28)

        ColumnLayout {
            id: contentCol
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(20)

            // ── Page header ───────────────────────────────────────────────────

            Text {
                text:                qsTr("DEVICES")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText {
                text: qsTr("Launch Monitor")
            }

            Text {
                text: qsTr("A launch monitor measures ball and club data we cannot see optically — face angle, spin, strike location — and measures several we estimate ourselves. Both are kept: its readings are stored alongside our own, never in place of them, so the two can be compared shot by shot.")
                font.family:      Theme.fontBody
                font.pixelSize:   Theme.fontSzBody2
                font.weight:      Theme.fontBodyWeight
                color:            Theme.colorText3
                wrapMode:         Text.WordWrap
                Layout.fillWidth: true
            }

            // ── Group 1 — Connection ──────────────────────────────────────────

            Text {
                text:                qsTr("CONNECTION")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            // Device
            RowLayout {
                objectName: "setting_lmDevice"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Device")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("FSX2020 is Windows-only, but the folder it writes to can be a share — so this works from any machine that can see it")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                PpComboBox {
                    Layout.preferredWidth: Theme.sp(240)
                    model: root.deviceOptions.map(function (o) { return o.label })
                    currentIndex: {
                        for (var i = 0; i < root.deviceOptions.length; ++i)
                            if (root.deviceOptions[i].value === appSettings.launchMonitorKind)
                                return i
                        return 0
                    }
                    onActivated: (index) => appSettings.launchMonitorKind = root.deviceOptions[index].value
                }
            }

            // Folder
            RowLayout {
                objectName: "setting_lmPath"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                opacity: root.configured ? 1.0 : 0.45
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Shot data folder")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("The folder FSX2020 writes LastShot.CSV into — the folder, not the file")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight:   Theme.sp(28)
                        color:            Theme.colorBg2
                        radius:           Theme.radius
                        border.width:     1
                        border.color:     Theme.colorBorderMid
                        Text {
                            anchors.fill:            parent
                            anchors.leftMargin:      Theme.sp(8)
                            anchors.rightMargin:     Theme.sp(8)
                            verticalAlignment:       Text.AlignVCenter
                            elide:                   Text.ElideLeft
                            text:                    appSettings.launchMonitorPath !== ""
                                                     ? appSettings.launchMonitorPath
                                                     : qsTr("No folder selected")
                            font.family:             Theme.fontData
                            font.pixelSize:          Theme.fontSzMicro
                            color:                   appSettings.launchMonitorPath !== ""
                                                     ? Theme.colorText : Theme.colorText3
                        }
                    }
                }

                ColumnLayout {
                    spacing: Theme.sp(6)
                    PpButton {
                        label:     qsTr("Change…")
                        enabled:   root.configured
                        onClicked: folderDialog.open()
                    }
                    PpButton {
                        label:     qsTr("Open")
                        enabled:   root.configured && appSettings.launchMonitorPath !== ""
                        onClicked: Qt.openUrlExternally(appSettings.fileUrlFor(appSettings.launchMonitorPath))
                    }
                }
            }

            // Status — the whole point of this row is answering "did I get the path right"
            // without having to go and hit a ball.
            RowLayout {
                objectName: "setting_lmStatus"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                visible: root.configured
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                Rectangle {
                    width: Theme.sp(8); height: Theme.sp(8); radius: Theme.sp(4)
                    color: root.statusColor
                    Layout.alignment: Qt.AlignVCenter
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           launchMonitor.stateLabel
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text: launchMonitor.errorText !== "" ? launchMonitor.errorText
                            : launchMonitor.lastReading !== "" ? qsTr("Last reading — %1").arg(launchMonitor.lastReading)
                            : launchMonitor.sourceText !== "" ? launchMonitor.sourceText
                            : qsTr("Nothing read yet")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ── Group 2 — Behaviour ───────────────────────────────────────────

            Text {
                text:                qsTr("BEHAVIOUR")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            // Arrival chime
            RowLayout {
                objectName: "setting_lmChime"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Chime when a reading arrives")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("A short, quiet tone a few seconds after the shot chime, when the monitor's data has been folded into the swing. The fourth dot in the capture toolbar shows the same thing silently.")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                TogglePill {
                    checked:     appSettings.launchMonitorChimeEnabled
                    enabledPill: root.configured
                    onToggled:   (v) => appSettings.launchMonitorChimeEnabled = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            // Store device data — the key already existed, waiting for a source.
            RowLayout {
                objectName: "setting_lmStore"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Store launch monitor data with each swing")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Off means readings are read and discarded — nothing is written to the swing, and none of the measured metrics appear")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                        Layout.fillWidth: true
                    }
                }

                TogglePill {
                    checked:     appSettings.saveLaunchMonitorData
                    enabledPill: root.configured
                    onToggled:   (v) => appSettings.saveLaunchMonitorData = v
                    Layout.alignment: Qt.AlignVCenter
                }
            }

            // Poll interval
            RowLayout {
                objectName: "setting_lmPoll"
                Layout.fillWidth: true
                spacing: Theme.sp(16)
                opacity: root.configured ? 1.0 : 0.45
                property bool searchHighlight: false
                Rectangle { x: -Theme.sp(6); y: -Theme.sp(6); width: parent.width + Theme.sp(12); height: parent.height + Theme.sp(12); color: Theme.colorAccentLight; radius: Theme.radius; opacity: parent.searchHighlight ? 1.0 : 0.0; z: -1 }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        text:           qsTr("Check for new shots every")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText
                    }
                    Text {
                        text:           qsTr("Raise this only if the folder is on a slow or busy network share")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                PpComboBox {
                    Layout.preferredWidth: Theme.sp(140)
                    enabled: root.configured
                    readonly property var msValues: [100, 250, 500, 1000, 2000]
                    model: [qsTr("0.1 s"), qsTr("0.25 s"), qsTr("0.5 s"), qsTr("1 s"), qsTr("2 s")]
                    currentIndex: Math.max(0, msValues.indexOf(appSettings.launchMonitorPollMs))
                    onActivated: (index) => appSettings.launchMonitorPollMs = msValues[index]
                }
            }
        }
    }
}
