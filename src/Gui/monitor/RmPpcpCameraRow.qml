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

// One PPCP camera (one PPCP `Source`) inside a phone's card on the
// PINPOINTCAPTURE tab of the resource monitor.
//
// ⛔ KEYED ON THE SOURCE, NEVER ON `serialNumber`.  A PPCP camera's
// `serialNumber` IS the owning peer's id, so both cameras of a two-camera phone
// collapse onto one identity wherever that is used as a key — a known open
// defect in the folded device stats, and CR-02 CB5 forbids repeating it here.
// `camData` is grouped by the wire's `source_id` and carries the full device id
// ("ppcp:<peer_id>/<source_id>") that VideoInputPpcp::deviceIdFor() builds, so a
// phone offering two Sources always shows two rows.
Rectangle {
    id: root

    property var  camData
    property bool isAlternate: false

    readonly property bool haveData: camData !== null && camData !== undefined
    readonly property string errText: haveData && camData.lastStreamError ? camData.lastStreamError : ""

    height: Theme.sp(38) + (errText !== "" ? errLbl.implicitHeight + Theme.sp(4) : 0)
    color: isAlternate ? Theme.colorBg : Theme.colorSurface

    // One counter, label above value — the same shape the STATS cells use.
    component Cell: Column {
        id: cell
        property string label: ""
        property string value: "—"
        property color  tint: Theme.colorText2
        width: Theme.sp(48)
        spacing: 1
        Text {
            text: cell.label
            font.family: Theme.fontData; font.pixelSize: Theme.sp(8)
            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
        }
        Text {
            text: cell.value
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzDataSm
            color: cell.tint
        }
    }

    // ── Identity ────────────────────────────────────────────────────────────
    Column {
        id: ident
        anchors { left: parent.left; leftMargin: Theme.sp(10); top: parent.top; topMargin: Theme.sp(5) }
        width: Math.max(Theme.sp(40), parent.width - Theme.sp(10) - Theme.sp(360))
        spacing: 1
        visible: root.haveData

        Row {
            spacing: Theme.sp(6)

            Rectangle {
                width: Theme.sp(5)
                height: Theme.sp(5)
                radius: Theme.sp(3)
                anchors.verticalCenter: parent.verticalCenter
                color: root.haveData && root.camData.attached ? Theme.colorGood : Theme.colorBorderStrong
            }

            Text {
                text: root.haveData ? root.camData.sourceId : ""
                font.family: Theme.fontData
                font.pixelSize: Theme.fontSzDataSm
                color: Theme.colorText2
                elide: Text.ElideRight
                anchors.verticalCenter: parent.verticalCenter
            }

            // What kind of consumer is reading it — "is anything reading this
            // camera" and "is the SESSION reading it" are different questions.
            Text {
                text: !root.haveData ? ""
                      : root.camData.previewOnly ? qsTr("preview only") : qsTr("tile")
                font.family: Theme.fontData
                font.pixelSize: Theme.sp(9)
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        // The full device id — the identity that keeps two cameras apart.
        Text {
            text: root.haveData ? root.camData.deviceId : ""
            font.family: Theme.fontData
            font.pixelSize: Theme.sp(9)
            color: Theme.colorText3
            elide: Text.ElideRight
            width: parent.width
        }
    }

    // ── Counters ────────────────────────────────────────────────────────────
    Row {
        anchors { right: parent.right; rightMargin: Theme.sp(10); top: parent.top; topMargin: Theme.sp(6) }
        spacing: Theme.sp(8)
        visible: root.haveData

        Cell { label: qsTr("FRAMES");  value: root.haveData ? String(root.camData.previewFrames) : "—" }
        Cell { label: qsTr("OPENED");  value: root.haveData ? String(root.camData.streamsOpened) : "—" }
        Cell {
            label: qsTr("REFUSED")
            value: root.haveData ? String(root.camData.streamsRefused) : "—"
            tint: root.haveData && root.camData.streamsRefused > 0 ? Theme.colorWarn : Theme.colorText2
        }
        Cell {
            label: qsTr("DECODE")
            value: root.haveData ? String(root.camData.decodeFailures) : "—"
            tint: root.haveData && root.camData.decodeFailures > 0 ? Theme.colorError : Theme.colorText2
        }
        Cell {
            label: qsTr("ABSENT")
            value: root.haveData ? String(root.camData.absentSegments) : "—"
            tint: root.haveData && root.camData.absentSegments > 0 ? Theme.colorWarn : Theme.colorText2
        }

        // ── MSG 5.5 / CORE 5.20 — "can this Source be used right now" ───────
        //
        // ⚠ THREE ANSWERS, NOT TWO.  An em dash means NOTHING HAS BEEN SAID:
        // 5.5a is push-on-change, so a Source that has never been unavailable
        // has never sent a reading, and that is a different answer from
        // `available: true`.  Showing a green "yes" there would be inventing a
        // measurement, which is what 5.20a restates 5.15a for this entity to
        // stop.  Where it IS unavailable the device's own `reason` is shown
        // verbatim (`in_use`, `disconnected`, …) rather than mapped onto a word
        // this host already knows (10.3a / I13).
        Cell {
            label: qsTr("AVAIL")
            width: Theme.sp(64)
            value: {
                if (!root.haveData || !root.camData.avail) return "—"
                if (root.camData.avail === "yes") return qsTr("yes")
                return root.camData.availReason ? root.camData.availReason : qsTr("no")
            }
            tint: !root.haveData || !root.camData.avail ? Theme.colorText3
                : root.camData.avail === "yes"          ? Theme.colorGood
                                                        : Theme.colorError
        }
    }

    // The one string that says why a Stream did not open.
    Text {
        id: errLbl
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                  leftMargin: Theme.sp(22); rightMargin: Theme.sp(10); bottomMargin: Theme.sp(3) }
        visible: root.errText !== ""
        text: root.errText
        font.family: Theme.fontData
        font.pixelSize: Theme.sp(9)
        color: Theme.colorWarn
        wrapMode: Text.WordWrap
    }
}
