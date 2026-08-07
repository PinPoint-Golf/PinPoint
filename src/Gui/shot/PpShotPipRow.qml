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

// A shot card's diagnostic read, at carousel size: one pip per tracked condition, plus the
// count of the ones that fired (design 13a, brief §6.1).
//
// SAME COLOUR RULE AS THE TICK RUN, AND THE SAME THIRD STATE. Fired fills in the error token,
// clean in the good one, and a condition this capture could not answer is drawn SHORTER AND
// OUTLINED rather than left out — the carousel is where a golfer decides which swing to open,
// and a row that silently dropped the unmeasured conditions would make two different shots
// look like the same read. The panel's PpTickRun makes this argument at the other end of the
// session; this is the same fact at 3 px.
//
// EXTRACTED FROM PpShotCard SO IT CAN BE PRESSED. The card requires a dozen model roles and a
// proxy row to exist at all; the pip row requires a list of states. Keeping it separate is
// what lets the offscreen QML test assert the third state is drawn rather than eyeball it,
// and it costs the card one extra item.

import QtQuick
import PinPointStudio

Item {
    id: root

    // SessionDiagnosticsModel::pipsFor(shotId) — [{ id, state }] where state is
    // "fired" | "clean" | "notAssessable". Empty means this shot is not in the ledger.
    property var pips: []
    // SessionDiagnosticsModel::firedCountFor(shotId).
    property int firedCount: 0

    objectName: "sdPipRow"

    readonly property int count: pips ? pips.length : 0

    // The mock's pips are 8 px tall in a 78 px cell; the real cell is Theme.sp(139) wide with
    // three overlays already on it, so the row is drawn at the smallest size that still
    // separates a filled pip from an outlined one — 4 px, and 3 px for the outlined one.
    readonly property int _tallH:  Math.max(2, Theme.sp(4))
    readonly property int _shortH: Math.max(1, Theme.sp(3))
    readonly property real _gap:   Math.max(1, Theme.sp(1))

    // The count reads as a severity, in the same three colours the rest of the app grades in.
    readonly property color countColor: firedCount >= 4 ? Theme.colorError
                                      : firedCount >= 2 ? Theme.colorAttention
                                                        : Theme.colorGood

    implicitHeight: Math.max(_tallH, countText.implicitHeight)

    Row {
        id: pipRow
        anchors.left: parent.left
        anchors.right: countText.left
        anchors.rightMargin: Theme.sp(5)
        anchors.verticalCenter: parent.verticalCenter
        spacing: root._gap

        Repeater {
            model: root.pips

            Rectangle {
                required property var modelData

                objectName: "sdPip"

                readonly property bool notAssessable: modelData.state === "notAssessable"
                readonly property bool fired:         modelData.state === "fired"

                // Even shares of the row, so a nine-condition read and a four-condition one
                // both span the card and can be compared across cells at a glance.
                width: Math.max(1, (pipRow.width - (root.count - 1) * root._gap)
                                   / Math.max(1, root.count))
                height: notAssessable ? root._shortH : root._tallH
                anchors.verticalCenter: parent.verticalCenter
                radius: 1

                color: notAssessable ? "transparent"
                                     : (fired ? Theme.colorError : Theme.colorGood)
                border.width: notAssessable ? 1 : 0
                border.color: Theme.colorText3
            }
        }
    }

    Text {
        id: countText
        objectName: "sdPipCount"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        text: qsTr("%1 fired").arg(root.firedCount)
        font.family: Theme.fontData
        font.pixelSize: Theme.fontSzMicro
        color: root.countColor
    }
}
