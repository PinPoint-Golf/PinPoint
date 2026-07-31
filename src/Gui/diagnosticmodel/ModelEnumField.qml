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

// A one-of-many field: the value, in a box that looks like every other field, opened by clicking it.
//
// It replaces a PpSegmentedControl, which is the right control for two or three choices on a wide
// bar and the wrong one in a 480px inspector: Evidence has SIX tiers, and six segments in that width
// rendered as a row of overlapping, unreadable pills.
//
// The rule this restores is worth stating, because the segmented control broke it: every field on a
// details pane looks the same and is changed the same way — you click it — whatever kind it holds.
// A reader should not have to learn a different gesture per field type, and a control that changes
// shape with the number of options is one that renders differently on every object.
//
// One click opens, one click picks. That is the same two-click budget the table's enum cells keep.
Rectangle {
    id: root

    // [{ value, label }] — the same shape the façade hands the table.
    property var    options: []
    property var    value:   ""
    // Rendered below the caret when the list is open; harmless when nothing is open.
    readonly property bool opened: menu.opened

    signal picked(var value)

    implicitHeight: Theme.sp(30)
    radius:         Theme.radius
    color:          Theme.colorSurface
    border.width:   1
    border.color:   menu.opened ? Theme.colorAccent : Theme.colorBorderStrong
    opacity:        root.enabled ? 1.0 : 0.5

    readonly property string currentLabel: {
        var o = root.options || []
        for (var i = 0; i < o.length; i++)
            if (o[i].value === root.value) return o[i].label
        return ""
    }

    Text {
        anchors.left:           parent.left
        anchors.right:          caret.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin:     Theme.sp(10)
        anchors.rightMargin:    Theme.sp(6)
        text:  root.currentLabel
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody
        font.weight:    Theme.fontBodyWeight
        color:          Theme.colorText
        elide:          Text.ElideRight
    }

    // Says it opens something, in the app's own combo glyph (PpComboBox uses the same).
    Text {
        id: caret
        anchors.right:          parent.right
        anchors.rightMargin:    Theme.sp(10)
        anchors.verticalCenter: parent.verticalCenter
        text: "⌄"
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody
        color: boxMa.containsMouse ? Theme.colorText2 : Theme.colorText3
    }

    PpPressable {
        id: boxMa
        hoverScale: 1.0
        enabled:    root.enabled
        onClicked:  menu.open()
    }

    Popup {
        id: menu
        y: root.height + Theme.sp(2)
        width: root.width
        padding: Theme.sp(4)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            color:        Theme.colorSurface
            radius:       Theme.radius
            border.width: 1
            border.color: Theme.colorBorderStrong
        }

        contentItem: Column {
            spacing: 0
            Repeater {
                model: root.options || []
                delegate: Rectangle {
                    id: option
                    required property var modelData
                    readonly property bool chosen: modelData.value === root.value

                    width:  menu.availableWidth
                    height: Theme.sp(28)
                    radius: Theme.radius
                    color:  optionHover.hovered ? Theme.colorBg2 : "transparent"

                    HoverHandler { id: optionHover }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left:        parent.left
                        anchors.right:       parent.right
                        anchors.leftMargin:  Theme.sp(10)
                        anchors.rightMargin: Theme.sp(10)
                        text: option.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color: option.chosen ? Theme.colorAccent : Theme.colorText
                        elide: Text.ElideRight
                    }

                    PpPressable {
                        hoverScale: 1.0
                        onClicked: { root.picked(option.modelData.value); menu.close() }
                    }
                }
            }
        }
    }
}
