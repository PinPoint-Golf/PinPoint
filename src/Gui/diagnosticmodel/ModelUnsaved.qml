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

// What Save would write, hung under the `n unsaved ▾` readout that counts it.
//
// The history used to take the inspector's slot behind a button called `Edits`, three controls away
// from the Save it describes, while `revert` was a 12px word in the status bar nowhere near either.
// One cluster now: the count, the history it counts, and the discard that throws it away.
//
// ModelEdits comes across whole rather than being re-drawn here — including the sentence stating that
// the history is session-scoped, which is the fact this popover most needs and least wants a second
// wording of.
Popup {
    id: root

    property var  edits: []
    property bool sessionScoped: true

    signal windTo(int index)
    signal revertAll()

    width:  Theme.sp(400)
    height: Theme.sp(420)
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

        ModelEdits {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            edits:         root.edits
            sessionScoped: root.sessionScoped
            onWindTo: (index) => root.windTo(index)
            onCloseRequested: root.close()
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        // At the foot of the history it would discard, which is the whole reason it moved here.
        // Warn-toned rather than error-toned: it throws away UNSAVED work only, and the command it
        // pushes is itself undoable — revert() puts itself on the stack for exactly that reason.
        RowLayout {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(14)
            Layout.rightMargin:  Theme.sp(14)
            Layout.topMargin:    Theme.sp(10)
            Layout.bottomMargin: Theme.sp(12)
            spacing: Theme.sp(10)

            Text {
                Layout.fillWidth: true
                text: qsTr("Discards every unsaved edit. ⌘Z brings them back.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Rectangle {
                implicitWidth:  revertLbl.implicitWidth + Theme.sp(20)
                implicitHeight: Theme.sp(28)
                radius: Theme.radius
                color:  revertMa.containsMouse ? Theme.colorWarnLight : "transparent"
                border.width: 1
                border.color: Theme.colorWarn
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: revertLbl
                    anchors.centerIn: parent
                    text: qsTr("Revert all")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorWarn
                }
                PpPressable {
                    id: revertMa
                    hoverScale: 1.0
                    onClicked: { root.revertAll(); root.close() }
                }
            }
        }
    }
}
