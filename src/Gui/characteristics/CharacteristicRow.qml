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
import PinPointStudio

// One library row. Mirrors MetricRow.qml: chromeless until hovered, all shape supplied by the
// C++ façade so no graph walking happens here.
Item {
    id: root

    // A row map from CharacteristicLibraryModel.query().
    property var characteristic: ({})

    signal clicked()

    readonly property bool   _hovered:  rowMa.containsMouse
    readonly property bool   _proposed: characteristic.proposed === true
    readonly property string _reach:    characteristic.reach || "measured"

    // Resolvability drives the dot colour. A capture gap is deliberately NOT red: it is not a
    // failure, it is an honest statement that no sensor we have can see this.
    function _resolvabilityColor(r) {
        switch (r) {
        case "live":          return Theme.colorRagGood
        case "planned":       return Theme.colorRagWatch
        case "noProducer":    return Theme.colorRagFault
        case "notCapturable": return Theme.colorRagNone
        }
        return Theme.colorRagNone
    }

    implicitHeight: Theme.sp(52)

    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: root._hovered ? Theme.colorBg2 : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin:  Theme.sp(10)
        anchors.rightMargin: Theme.sp(12)
        spacing: Theme.sp(10)

        // ── Resolvability dot ────────────────────────────────────────────────
        Rectangle {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth:  Theme.sp(7)
            implicitHeight: Theme.sp(7)
            radius: width / 2
            color: root._resolvabilityColor(characteristic.resolvability)
        }

        // ── Label + secondary line ───────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(3)

            Text {
                Layout.fillWidth: true
                text:           characteristic.label || characteristic.id || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText
                elide:          Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                // Causes and effects both, because the same condition is routinely both.
                text: {
                    var bits = []
                    if (characteristic.measureCount > 0)
                        bits.push(qsTr("%n measure(s)", "", characteristic.measureCount))
                    if (characteristic.causeCount > 0)
                        bits.push(qsTr("%n cause(s)", "", characteristic.causeCount))
                    if (characteristic.effectCount > 0)
                        bits.push(qsTr("explains %n", "", characteristic.effectCount))
                    return bits.join(" · ")
                }
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                elide:          Text.ElideRight
                visible:        text.length > 0
            }
        }

        // ── Reach badge ──────────────────────────────────────────────────────
        // Physical / Behavioural causes can never be measured by this product. Saying so on every
        // row is what stops a reader assuming a producer is on the way.
        Rectangle {
            visible: root._reach !== "measured"
            Layout.alignment: Qt.AlignVCenter
            implicitWidth:  reachText.implicitWidth + Theme.sp(14)
            implicitHeight: Theme.sp(20)
            radius: height / 2
            color: "transparent"
            border.width: 1
            border.color: Theme.colorText3

            Text {
                id: reachText
                anchors.centerIn: parent
                text:           characteristic.reachLabel || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        // ── Proposed badge ───────────────────────────────────────────────────
        // No citation. Badged wherever it appears, per the pack's content rules.
        Rectangle {
            visible: root._proposed
            Layout.alignment: Qt.AlignVCenter
            implicitWidth:  proposedText.implicitWidth + Theme.sp(14)
            implicitHeight: Theme.sp(20)
            radius: height / 2
            color: Theme.colorBg2

            Text {
                id: proposedText
                anchors.centerIn: parent
                text:           qsTr("Proposed")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        Text {
            Layout.alignment: Qt.AlignVCenter
            text:           characteristic.resolvabilityLabel || ""
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }
    }

    MouseArea {
        id: rowMa
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
