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

// One control on the panel's two bars: a glyph, a word, or both, drawn quiet until hovered.
//
// PpButton is the app's button and stays the app's button — but it is a filled 34px control sized for
// a page's primary action, and the bar carries ten of them. Ten PpButtons in a row is the wall of
// equal-weight chrome ADDENDUM-02 exists to undo, so the bar's own items are drawn at bar weight and
// `Save` stays a PpButton precisely because it is the one that should not be.
//
// `implicitWidth` is always stated, because a RowLayout under pressure shrinks anything that does not
// state one to nothing — the lesson the Table/Graph control taught, generalised.
Rectangle {
    id: root

    property string glyph:   ""
    property string label:   ""
    // Rendered after the label in the dim colour: the ⌘F on the search box, the ▾ on a menu.
    property string hint:    ""
    property string tooltip: ""
    // Held open — a menu button stays lit while its menu is up, so the reader can see which of the
    // bar's items owns the popover under it.
    property bool   active:  false
    property color  tone:    Theme.colorText2
    property color  fill:    "transparent"

    signal clicked()

    implicitWidth:  content.implicitWidth + Theme.sp(16)
    implicitHeight: Theme.sp(28)
    radius:         Theme.radius
    opacity:        root.enabled ? 1.0 : 0.35

    color: root.active           ? Theme.colorBg3
         : barHover.containsMouse ? Theme.colorBg2
                                  : root.fill
    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

    Row {
        id: content
        anchors.centerIn: parent
        spacing: Theme.sp(6)

        Text {
            visible: root.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text:           root.glyph
            font.family:    Theme.fontSymbol
            font.pixelSize: Math.round(Theme.fontSzBody2 * Theme.symbolScale(root.glyph))
            color:          root.tone
        }
        Text {
            visible: root.label !== ""
            anchors.verticalCenter: parent.verticalCenter
            text:           root.label
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody2
            font.weight:    Theme.fontBodyWeight
            color:          root.tone
        }
        Text {
            visible: root.hint !== ""
            anchors.verticalCenter: parent.verticalCenter
            text:           root.hint
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }
    }

    // The tooltip is where the command's human label lives — `browser.undoLabel` says what ⌘Z would
    // actually undo, and until now nothing in the panel showed it anywhere.
    ToolTip.visible: root.tooltip !== "" && barHover.containsMouse
    ToolTip.text:    root.tooltip
    ToolTip.delay:   400

    PpPressable {
        id: barHover
        hoverScale: 1.0
        enabled:    root.enabled
        onClicked:  root.clicked()
    }
}
