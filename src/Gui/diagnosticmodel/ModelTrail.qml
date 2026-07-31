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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import PinPointStudio

// The trail: the chain you actually walked to get here. Its terminal item is ALWAYS the current
// selection, which is what makes it a trail rather than a history — a metric, the measure that
// reads it, the corridor that judges it, the characteristic that fires, read left to right.
//
// Clicking a step goes back to it and truncates everything after, so the trail never carries a
// future you have left.
Rectangle {
    id: root

    property var trail: []      // [{ type, id, label }]

    signal stepPicked(string type, string id)

    implicitHeight: Theme.sp(28)
    radius:       height / 2
    color:        Theme.colorSurface
    border.width: 1
    border.color: Theme.colorBorderMid

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin:  Theme.sp(10)
        anchors.rightMargin: Theme.sp(10)
        spacing: Theme.sp(6)

        Text {
            text:                qsTr("TRAIL")
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
        }

        Repeater {
            model: root.trail
            delegate: RowLayout {
                id: step
                required property var modelData
                required property int index

                readonly property bool terminal: index === root.trail.length - 1

                spacing: Theme.sp(6)
                // Earlier steps give up their width first: what you are looking at NOW is the part
                // that must stay readable, so the terminal item keeps its room and the rest elide.
                Layout.maximumWidth: terminal ? Theme.sp(180) : Theme.sp(110)

                Text {
                    Layout.fillWidth: true
                    text: step.modelData.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color: step.terminal ? Theme.colorText : Theme.colorText3
                    elide: Text.ElideRight

                    PpPressable {
                        hoverScale: 1.0
                        enabled:    !step.terminal
                        onClicked:  root.stepPicked(step.modelData.type, step.modelData.id)
                    }
                }

                Text {
                    visible: !step.terminal
                    text:    "→"
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }
        }

        Item { Layout.fillWidth: true }
    }
}
