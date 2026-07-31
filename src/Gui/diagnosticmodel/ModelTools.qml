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

// The things that are not content: how the library is GRADED, and the artefacts that leave the app.
//
// Kept apart from the table on purpose. A grade policy is not an edit to the library — it is how the
// library is being read — so it does not belong on the undo stack and must not look like it does.
// The two exports are here for the same reason: they produce a file, they change nothing.
Popup {
    id: root

    property var browser: null
    // Bound to AppSettings by the panel, so this component keeps no settings dependency — the same
    // seam every other holder of this setting uses.
    property string gradePolicy: ""

    signal gradePolicyPicked(string name)
    signal exportRoadmapRequested()
    signal exportReferencesRequested()
    signal roadmapRequested()
    signal glossaryRequested()

    width:   Theme.sp(340)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    component Heading: Text {
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    component Action: Item {
        id: action
        property string label: ""
        property string hint:  ""
        signal triggered()

        implicitHeight: Theme.sp(30)

        Rectangle {
            anchors.fill: parent
            color: actionHover.hovered ? Theme.colorBg2 : "transparent"
        }
        HoverHandler { id: actionHover }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin:  Theme.sp(14)
            anchors.rightMargin: Theme.sp(14)
            spacing: Theme.sp(8)

            Text {
                Layout.fillWidth: true
                text: action.label
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText
                elide:          Text.ElideRight
            }
            Text {
                text:    action.hint
                visible: text.length > 0
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        PpPressable { hoverScale: 1.0; onClicked: action.triggered() }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Heading {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(4)
            text: qsTr("How it grades")
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
                    color: policyRow.active   ? Theme.colorAccentLight
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
                    onClicked:  root.gradePolicyPicked(policyRow.modelData.name)
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp(8)
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        Heading {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(2)
            text: qsTr("Norm sets")
        }

        Repeater {
            model: root.browser ? root.browser.normSets() : []
            delegate: RowLayout {
                id: setRow
                required property var modelData

                Layout.fillWidth:   true
                Layout.leftMargin:  Theme.sp(14)
                Layout.rightMargin: Theme.sp(14)
                Layout.bottomMargin: Theme.sp(3)
                spacing: Theme.sp(8)

                Text {
                    Layout.fillWidth: true
                    // "merged" is an implementation word and must never reach a reader: they need
                    // the shipped set and their own as separate things, because that is what the
                    // override relationship between them means.
                    text: setRow.modelData.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    elide:          Text.ElideRight
                }
                Text {
                    text: setRow.modelData.normCount
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp(8)
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        Heading {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(2)
            text: qsTr("Views and artefacts")
        }

        Action {
            Layout.fillWidth: true
            label: qsTr("Roadmap")
            hint:  qsTr("what is not built yet")
            onTriggered: { root.roadmapRequested(); root.close() }
        }
        Action {
            Layout.fillWidth: true
            label: qsTr("Glossary")
            hint:  qsTr("what a term means")
            onTriggered: { root.glossaryRequested(); root.close() }
        }
        Action {
            Layout.fillWidth: true
            label: qsTr("Export roadmap")
            hint:  qsTr("markdown")
            onTriggered: { root.exportRoadmapRequested(); root.close() }
        }
        Action {
            Layout.fillWidth:    true
            Layout.bottomMargin: Theme.sp(8)
            label: qsTr("Export references")
            hint:  qsTr("CSL-JSON")
            onTriggered: { root.exportReferencesRequested(); root.close() }
        }
    }
}
