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
import QtQuick.Controls.Basic
import PinPointStudio

// How the library is GRADED, hung off the bar's live readout rather than buried in a drawer.
//
// Promoted out of ModelTools because it is not an artefact and not a view — it is the state every
// corridor on screen is drawn under, and the answer to "why is this corridor red" should not be
// behind a button called Tools. It is still not an edit to the library, so it still does not touch
// the undo stack: the readout writes straight to the one global AppSettings.
Popup {
    id: root

    property var    browser:     null
    property string gradePolicy: ""

    signal picked(string name)

    width:   Theme.sp(320)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    contentItem: ColumnLayout {
        spacing: 0

        Text {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(4)
            text:                qsTr("How it grades")
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
        }

        // The policy is one comparable thing across athletes and shared packs, which is why it is
        // stored and shown by NAME rather than as three z numbers.
        Repeater {
            model: root.browser ? root.browser.gradePolicies() : []
            delegate: Item {
                id: policyRow
                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: Theme.sp(34)

                readonly property bool active: root.gradePolicy === modelData.name

                Rectangle {
                    anchors.fill: parent
                    color: policyRow.active    ? Theme.colorAccentLight
                         : policyHover.hovered ? Theme.colorBg2
                                               : "transparent"
                }
                HoverHandler { id: policyHover }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin:  Theme.sp(14)
                    anchors.rightMargin: Theme.sp(14)
                    spacing: 0

                    Text {
                        text: policyRow.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color: policyRow.active ? Theme.colorAccent : Theme.colorText
                    }
                    Text {
                        Layout.fillWidth: true
                        text: policyRow.modelData.hint
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                    }
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked:  { root.picked(policyRow.modelData.name); root.close() }
                }
            }
        }

        Item { Layout.preferredHeight: Theme.sp(8) }
    }
}
