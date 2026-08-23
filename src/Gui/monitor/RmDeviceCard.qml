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
import PinPointStudio

Rectangle {
    id: root

    property var deviceData

    signal openSettingsRequested()

    radius: Theme.radiusLg
    border.width: 1
    border.color: Theme.colorBorderMid
    color: Theme.colorSurface
    clip: true
    visible: root.deviceData !== null && root.deviceData !== undefined
    implicitHeight: header.height + body.implicitHeight + body.topPadding + body.bottomPadding

    // ── Header strip ─────────────────────────────────────────────────────────
    Rectangle {
        id: header
        height: Theme.sp(40)
        anchors { top: parent.top; left: parent.left; right: parent.right }
        color: Theme.colorBg2
        radius: Theme.radiusLg

        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 1
            color: Theme.colorBorderMid
        }

        // Bottom corners square (to overlap radius)
        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: Theme.radiusLg
            color: Theme.colorBg2
        }

        Row {
            anchors { fill: parent; leftMargin: Theme.sp(12); rightMargin: Theme.sp(10) }
            spacing: Theme.sp(8)

            // Status dot
            Rectangle {
                width: Theme.sp(7)
                height: Theme.sp(7)
                radius: Theme.sp(4)
                anchors.verticalCenter: parent.verticalCenter
                color: {
                    var s = root.deviceData.status
                    if (s === "streaming" || s === "connected") return Theme.colorGood
                    if (s === "stalled" || root.deviceData.hasWarning) return Theme.colorWarn
                    return Theme.colorBorderStrong
                }
            }

            // Name + model/backend
            Column {
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                width: parent.width - Theme.sp(7) - Theme.sp(8) - pillItem.implicitWidth - Theme.sp(8)

                Text {
                    text: root.deviceData.name
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzBody
                    color: Theme.colorText
                    elide: Text.ElideRight
                    width: parent.width
                }

                Text {
                    text: root.deviceData.model !== root.deviceData.name
                          ? root.deviceData.model + " · " + root.deviceData.backend
                          : root.deviceData.backend
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(9)
                    color: Theme.colorText3
                    elide: Text.ElideRight
                    width: parent.width
                }
            }

            // Status pill
            Item {
                id: pillItem
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: pill.implicitWidth + Theme.sp(14)
                implicitHeight: pill.implicitHeight + Theme.sp(6)

                Rectangle {
                    anchors.fill: parent
                    radius: height / 2
                    color: {
                        var s = root.deviceData.status
                        if (s === "streaming" || s === "connected") return Theme.colorGoodLight
                        if (s === "stalled" || root.deviceData.hasWarning) return Theme.colorWarnLight
                        return Theme.colorBg3
                    }
                    border.width: 1
                    border.color: {
                        var s = root.deviceData.status
                        if (s === "streaming" || s === "connected")
                            return Qt.rgba(Theme.colorGood.r, Theme.colorGood.g, Theme.colorGood.b, 0.3)
                        if (s === "stalled" || root.deviceData.hasWarning)
                            return Qt.rgba(Theme.colorWarn.r, Theme.colorWarn.g, Theme.colorWarn.b, 0.3)
                        return Theme.colorBorderMid
                    }
                }

                Text {
                    id: pill
                    anchors.centerIn: parent
                    text: {
                        var s = root.deviceData.status
                        if (s === "connected" && root.deviceData.dataRateHz > 0)
                            return root.deviceData.dataRateHz.toFixed(0) + " Hz"
                        return s.toUpperCase()
                    }
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(9)
                    font.letterSpacing: Theme.trackingMicro
                    color: {
                        var s = root.deviceData.status
                        if (s === "streaming" || s === "connected") return Theme.colorGood
                        if (s === "stalled" || root.deviceData.hasWarning) return Theme.colorWarn
                        return Theme.colorText3
                    }
                }
            }
        }
    }

    // ── Body ─────────────────────────────────────────────────────────────────
    Column {
        id: body
        anchors { top: header.bottom; left: parent.left; right: parent.right }
        topPadding: Theme.sp(12)
        bottomPadding: Theme.sp(12)
        leftPadding: Theme.sp(14)
        rightPadding: Theme.sp(14)
        spacing: Theme.sp(7)

        property var rows: {
            var d = root.deviceData
            if (d.kind === "Camera") {
                var camRows = []
                if (d.serialNumber)
                    camRows.push({ key: qsTr("Identifier"), value: d.serialNumber, cls: "neutral" })
                camRows.push(
                    { key: qsTr("Resolution"),    value: d.resolutionStr,       cls: "neutral" },
                    { key: qsTr("Crop size"),     value: d.cropStr,             cls: "neutral" },
                    { key: qsTr("Frame rate"),    value: d.dataRateStr,         cls: d.dataRateHz > 0 ? "good" : "neutral" },
                    { key: qsTr("Events written"),value: d.eventsWrittenStr,    cls: "neutral" },
                    { key: qsTr("Bytes written"), value: d.bytesWrittenStr,     cls: "neutral" },
                    { key: qsTr("Ring wraps"),    value: d.eventsOverwrittenStr,cls: "neutral" },
                    { key: qsTr("Ring size"),     value: d.ringCapacityStr,     cls: "neutral" }
                )
                return camRows
            } else if (d.kind === "Phone") {
                // ⚠ ITS OWN ARM, AND IT NEEDED ONE.  This was a two-way branch
                // — Camera or else-IMU — so a phone fell into the IMU side and
                // was rendered with a battery gauge, a gimbal-drop count and a
                // ring size, none of which it has.  A phone carries no bytes of
                // its own: its CAMERAS are separate rows in this same list and
                // they are where the rate and the ring live.
                var phoneRows = []
                if (d.identifier)
                    phoneRows.push({ key: qsTr("Pairing"), value: d.identifier, cls: "neutral" })
                phoneRows.push(
                    { key: qsTr("Connection"),
                      value: d.status === "connected"    ? qsTr("Connected")
                           : d.status === "revoked"      ? qsTr("Revoked")
                                                         : qsTr("Not connected"),
                      cls:   d.status === "connected" ? "good" : "neutral" },
                    { key: qsTr("Remembered"),
                      value: d.persisted ? qsTr("Yes") : qsTr("No"),
                      cls:   "neutral" },
                    { key: qsTr("Transport"), value: d.backend, cls: "neutral" }
                )
                return phoneRows
            } else {
                var imuRows = []
                if (d.identifier)
                    imuRows.push({ key: qsTr("Identifier"), value: d.identifier, cls: "neutral" })
                imuRows.push(
                    { key: qsTr("Data rate"),
                      value: d.dataRateStr,
                      cls: d.dataRateHz > 0 ? "good" : "neutral" },
                    { key: qsTr("Battery"),
                      value: d.batteryStr,
                      cls: d.batteryPct >= 0 && d.batteryPct < 20 ? "warn"
                         : d.batteryPct > 60 ? "good" : "neutral" },
                    { key: qsTr("Gimbal drops"),
                      value: d.gimbalDropCountStr,
                      cls: d.gimbalDropCount > 0 ? "warn" : "neutral" },
                    { key: qsTr("Events written"),
                      value: d.eventsWrittenStr,
                      cls: "neutral" },
                    { key: qsTr("Ring wraps"),
                      value: d.eventsOverwrittenStr,
                      cls: "neutral" },
                    { key: qsTr("Ring size"),
                      value: d.ringCapacityStr,
                      cls: "neutral" }
                )
                return imuRows
            }
        }

        Repeater {
            model: body.rows

            Row {
                width: body.width - body.leftPadding - body.rightPadding
                spacing: Theme.sp(8)

                Text {
                    width: parent.width * 0.52
                    text: modelData.key
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.sp(11)
                    font.weight: Theme.fontBodyWeight
                    color: Theme.colorText3
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    width: parent.width - parent.width * 0.52 - Theme.sp(8)
                    text: modelData.value
                    font.family: Theme.fontData
                    font.pixelSize: Theme.sp(11)
                    horizontalAlignment: Text.AlignRight
                    color: modelData.cls === "good" ? Theme.colorGood
                         : modelData.cls === "warn" ? Theme.colorWarn
                         : Theme.colorText2
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // Ring fill bar
        Rectangle {
            width: body.width - body.leftPadding - body.rightPadding
            height: Theme.sp(3)
            radius: Theme.sp(2)
            color: Theme.colorBg3

            Rectangle {
                height: Theme.sp(3)
                radius: Theme.sp(2)
                width: {
                    var fill = root.deviceData.ringFill
                    return parent.width * Math.min(1.0, fill)
                }
                color: root.deviceData.ringFill > 0.85 ? Theme.colorWarn
                     : root.deviceData.kind === "IMU" ? Theme.colorAccent
                     : Theme.colorGood

                Behavior on width {
                    NumberAnimation { duration: 600; easing.type: Easing.OutCubic }
                }
            }
        }

        // Settings link
        Item {
            width: body.width - body.leftPadding - body.rightPadding
            height: Theme.sp(18)

            Text {
                id: settingsLink
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                text: qsTr("Settings →")
                font.family: Theme.fontBody
                font.pixelSize: Theme.sp(10)
                color: settingsLinkHover.hovered ? Qt.lighter(Theme.colorAccent, 1.08) : Theme.colorAccent
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                transformOrigin: Item.Right
                scale: settingsLinkTap.pressed ? 0.97 : settingsLinkHover.hovered ? 1.02 : 1.0
                Behavior on scale { NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic } }

                HoverHandler { id: settingsLinkHover; cursorShape: Qt.PointingHandCursor }
                TapHandler  { id: settingsLinkTap; onTapped: root.openSettingsRequested() }
            }
        }
    }
}
