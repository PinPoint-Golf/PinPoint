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

// One labelled row of chips. The options come from C++ already gated by the validity table, so an
// empty row means "choose the one above first" rather than "nothing is possible" — and it says so,
// because a row of nothing with no explanation is indistinguishable from a rendering fault.
ColumnLayout {
    id: root

    property string label:   ""
    property var    options: []      // [{ value, label }]
    property string chosen:  ""

    signal picked(string value)

    spacing: Theme.sp(4)

    Text {
        Layout.leftMargin:   Theme.sp(14)
        text:                root.label
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    Text {
        Layout.leftMargin: Theme.sp(14)
        visible: root.options.length === 0
        text:    qsTr("choose the one above first")
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorText3
    }

    Flow {
        Layout.fillWidth:   true
        Layout.leftMargin:  Theme.sp(14)
        Layout.rightMargin: Theme.sp(14)
        spacing: Theme.sp(6)

        Repeater {
            model: root.options
            delegate: Rectangle {
                id: chip
                required property var modelData

                readonly property bool active: root.chosen === modelData.value

                implicitWidth:  chipText.implicitWidth + Theme.sp(18)
                implicitHeight: Theme.sp(24)
                radius:         height / 2
                color:          active ? Theme.colorAccentLight : "transparent"
                border.width:   1
                border.color:   active ? Theme.colorAccent : Theme.colorBorderMid

                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: chip.modelData.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    font.weight:    Theme.fontBodyWeight
                    color: chip.active ? Theme.colorAccent : Theme.colorText2
                }

                PpPressable { hoverScale: 1.0; onClicked: root.picked(chip.modelData.value) }
            }
        }
    }
}
