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

// Type-ahead for every link. Click, type three characters, Enter — never a picker dialog and never
// a scroll through 140 rows.
//
// The candidate list is PRE-FILTERED to legal targets in C++ (acyclicity, axis and
// corroborates-shadowing rules), so an illegal edit cannot be constructed. A control that offers a
// choice it will then reject is a worse control than one that does not offer it, and it also teaches
// the author that the rules are arbitrary.
Popup {
    id: root

    // A function (searchText) -> [{ id, label, detail, tone }], asked again on every keystroke so
    // the filtering stays in C++.
    property var  candidateSource: null
    property string title: ""

    signal picked(string id)

    width:   Theme.sp(360)
    padding: 0
    modal:   false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: { searchField.text = ""; searchField.forceActiveFocus(); listView.currentIndex = 0 }

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    readonly property var _candidates:
        candidateSource ? candidateSource(searchField.text) : []

    contentItem: ColumnLayout {
        spacing: 0

        Text {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(12)
            Layout.rightMargin: Theme.sp(12)
            Layout.topMargin:   Theme.sp(10)
            text:                root.title
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
        }

        PpTextField {
            id: searchField
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(10)
            Layout.rightMargin: Theme.sp(10)
            Layout.topMargin:   Theme.sp(6)
            placeholderText: qsTr("Type to narrow…")

            // Enter takes the highlighted candidate. With three characters typed that is usually
            // the only one left, which is the whole budget: click, three characters, Enter.
            Keys.onReturnPressed: root._take()
            Keys.onEnterPressed:  root._take()
            Keys.onDownPressed:   listView.currentIndex = Math.min(listView.currentIndex + 1,
                                                                   listView.count - 1)
            Keys.onUpPressed:     listView.currentIndex = Math.max(listView.currentIndex - 1, 0)
        }

        Text {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(12)
            Layout.topMargin:    Theme.sp(6)
            Layout.bottomMargin: Theme.sp(2)
            text: qsTr("%n legal target(s)", "", root._candidates.length)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }

        ListView {
            id: listView
            Layout.fillWidth:       true
            Layout.preferredHeight: Math.min(Theme.sp(240), Math.max(Theme.sp(32), count * Theme.sp(28)))
            Layout.bottomMargin:    Theme.sp(8)
            clip:  true
            model: root._candidates

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: candidate
                required property var modelData
                required property int index

                width:  listView.width
                height: Theme.sp(28)
                color: listView.currentIndex === index ? Theme.colorAccentLight
                     : candidateHover.hovered          ? Theme.colorBg2
                                                       : "transparent"

                HoverHandler { id: candidateHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin:  Theme.sp(12)
                    anchors.rightMargin: Theme.sp(12)
                    spacing: Theme.sp(8)

                    Text {
                        Layout.fillWidth: true
                        text: candidate.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color:          Theme.colorText
                        elide:          Text.ElideRight
                    }

                    Text {
                        text:    candidate.modelData.detail || ""
                        visible: text.length > 0
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                        Layout.maximumWidth: root.width * 0.4
                    }
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked: { root.picked(candidate.modelData.id); root.close() }
                }
            }
        }
    }

    function _take() {
        var list = root._candidates
        if (!list || list.length === 0) return
        var i = Math.max(0, Math.min(listView.currentIndex, list.length - 1))
        root.picked(list[i].id)
        root.close()
    }
}
