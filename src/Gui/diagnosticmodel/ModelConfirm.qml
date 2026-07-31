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
import QtQuick.Controls.Basic
import PinPointStudio

// The house confirmation prompt, as the session-active close interception in Main.qml draws it:
// modal, centred, dimmed, Esc cancels, display-font title, right-aligned outlined primary followed by
// a neutral Cancel. Written once here because this panel raises two of them and two hand-drawn copies
// of one standard drift apart.
//
// `tone` is the ONE thing the two callers differ on, and the difference is meant. Theme.colorAttention
// is defined as a call-to-action frame — "draws the eye to a row/control that needs the user to act" —
// and the close prompt wears it because it interrupts something live, not because it destroys
// anything. A prompt that discards saved work wears the error family instead, which is how the app
// already draws a destructive action: removing something is a write to the user's pack, and is
// styled as one. Two different events must not read alike.
Popup {
    id: root

    property string title:       ""
    property string body:        ""
    property string confirmText: qsTr("Continue")
    property string cancelText:  qsTr("Cancel")
    // "attention" — a call to act, nothing is lost. "error" — this destroys saved work.
    property string tone:        "attention"

    signal confirmed()
    signal cancelled()

    readonly property color _strong: tone === "error" ? Theme.colorError : Theme.colorAttention
    readonly property color _light:  tone === "error" ? Theme.colorErrorLight
                                                      : Theme.colorAttentionLight

    parent: Overlay.overlay
    anchors.centerIn: parent
    modal: true
    dim:   true
    // Esc = cancel, the safe default. Deliberately NOT CloseOnPressOutside: a click beside a prompt
    // that is asking about saved work is not an answer.
    closePolicy: Popup.CloseOnEscape
    padding: Theme.sp(20)
    width: Math.min(Theme.sp(440), (parent ? parent.width : Theme.sp(600)) - Theme.sp(48))

    // Escape, a click on Cancel and a programmatic close all land here, so the cancel path is stated
    // once. `_answered` is what keeps confirming from also reporting a cancel on the way out.
    property bool _answered: false
    onOpened: root._answered = false
    onClosed: if (!root._answered) root.cancelled()

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radiusLg
        border.width: 1
        border.color: root._strong
    }

    contentItem: Column {
        spacing: Theme.sp(12)

        Text {
            width: parent.width
            text:  root.title
            font.family:    Theme.fontDisplay
            font.italic:    Theme.fontDisplayItalic
            font.weight:    Theme.fontDisplayWeight
            font.pixelSize: Math.min(Theme.sp(20), Theme.fontSzDisplay)
            color:          root._strong
            wrapMode:       Text.WordWrap
        }

        Text {
            width: parent.width
            text:  root.body
            font.family:    Theme.fontBody
            font.weight:    Theme.fontBodyWeight
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorText2
            wrapMode:       Text.WordWrap
            lineHeight:     1.5
        }

        Item { width: 1; height: Theme.sp(4) }

        Row {
            anchors.right: parent.right
            spacing: Theme.sp(8)

            // The primary, outlined in the tone rather than filled: it is the consequential answer,
            // and a filled button here would read as the recommended one.
            Rectangle {
                width:  confirmLbl.implicitWidth + Theme.sp(24)
                height: Theme.sp(32)
                radius: Theme.radius
                color:  confirmMa.containsMouse ? root._light : "transparent"
                border.width: 1
                border.color: root._strong
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: confirmLbl
                    anchors.centerIn: parent
                    text: root.confirmText
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          root._strong
                }
                PpPressable {
                    id: confirmMa
                    hoverScale: 1.0
                    onClicked: {
                        root._answered = true
                        root.close()
                        root.confirmed()
                    }
                }
            }

            Rectangle {
                width:  cancelLbl.implicitWidth + Theme.sp(24)
                height: Theme.sp(32)
                radius: Theme.radius
                color:  cancelMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
                border.width: 1
                border.color: Theme.colorBorderMid
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: cancelLbl
                    anchors.centerIn: parent
                    text: root.cancelText
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                }
                PpPressable { id: cancelMa; hoverScale: 1.0; onClicked: root.close() }
            }
        }
    }
}
