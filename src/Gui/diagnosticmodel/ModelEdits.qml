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

// The Edits history. Every mutation in the session, newest at the bottom, and clicking any row
// winds the whole model to that point — which is what makes the stack navigable rather than a
// counter with two buttons.
//
// Undone entries stay in the list, greyed. A history that deleted what you stepped back past would
// make redo undiscoverable, and "nothing in this panel may be unrecoverable" includes the
// recoverability of the recovery.
Item {
    id: root

    property var edits: []          // [{ index, label, detail, undone, saved }]
    property bool sessionScoped: true

    signal windTo(int index)
    signal closeRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(18)
            Layout.rightMargin: Theme.sp(14)
            Layout.topMargin:   Theme.sp(14)
            Layout.bottomMargin: Theme.sp(8)
            spacing: Theme.sp(8)

            Text {
                Layout.fillWidth:    true
                text:                qsTr("EDITS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            Text {
                text: "×"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText3
                PpPressable { hoverScale: 1.0; onClicked: root.closeRequested() }
            }
        }

        // SAID, not assumed. A stack that silently empties between launches teaches an author not
        // to trust it, which is worse than not having one — so the scope is stated where the
        // history is read.
        Text {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(18)
            Layout.bottomMargin: Theme.sp(10)
            visible: root.sessionScoped
            text: qsTr("This history lasts until you close the app. Saved work is kept; the "
                       + "ability to step back through it is not.")
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            wrapMode:       Text.WordWrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        ListView {
            id: list
            Layout.fillWidth:  true
            Layout.fillHeight: true
            clip:  true
            model: root.edits
            // Newest is what you want to see, and it is at the end.
            onCountChanged: positionViewAtEnd()

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            Text {
                anchors.centerIn: parent
                visible: list.count === 0
                text:    qsTr("No edits yet")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText3
            }

            delegate: Item {
                id: editRow
                required property var modelData

                width:  list.width
                height: Theme.sp(38)

                Rectangle {
                    anchors.fill: parent
                    color: editHover.hovered ? Theme.colorBg2 : "transparent"
                }

                HoverHandler { id: editHover }

                // Saved work carries an accent tick down its edge, so the boundary between what is
                // on disk and what is not is visible per row rather than as one line that a forked
                // history would put in the wrong place.
                Rectangle {
                    anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                    width:   2
                    color:   Theme.colorAccent
                    visible: editRow.modelData.saved === true
                    opacity: editRow.modelData.undone ? 0.3 : 1.0
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin:  Theme.sp(18)
                    anchors.rightMargin: Theme.sp(14)
                    anchors.topMargin:    Theme.sp(6)
                    anchors.bottomMargin: Theme.sp(6)
                    spacing: Theme.sp(1)

                    Text {
                        Layout.fillWidth: true
                        text: editRow.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color: editRow.modelData.undone ? Theme.colorText3 : Theme.colorText
                        elide: Text.ElideRight
                    }

                    Text {
                        Layout.fillWidth: true
                        text:    editRow.modelData.detail
                        visible: text.length > 0
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                    }
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root.windTo(editRow.modelData.index)
                }
            }
        }
    }
}
