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

// The chrome around one launch monitor schematic: a titled surface with a diagram
// container under it. Shared by the wide two-row composition and the narrow reflowed
// column, so a card looks the same and is padded the same in both.
//
// THE CAPTION MUST NOT WRAP, and that is a layout constraint rather than a preference.
// Every annotation inside the diagram is positioned from the container's TOP, so a
// caption that took a second line would push the whole schematic down by 12 design px
// and the lowest labels straight out of the card. It elides instead.

import QtQuick
import PinPointStudio

Item {
    id: root
    objectName: "lmCard"

    // Design-space scale, shared with the diagram inside so chrome and annotations grow
    // together.
    property real s: 1
    // The band this card is named for — the TITLE's hue only. Annotations inside are
    // coloured by their own metric's band, which is what lets one colour meaning carry
    // between the tiles board and these schematics.
    property color hue: Theme.colorText
    property string title: ""
    property string caption: ""
    // The schematic. Instantiated by a Loader so only the active layout builds one.
    property Component diagram: null

    // The brief's card padding: sp(9) top, sp(12) sides, sp(10) bottom, over a 19 px
    // title row. Everything below the title is the diagram's.
    readonly property real padTop: 9 * s
    readonly property real padX: 12 * s
    readonly property real padBottom: 10 * s
    readonly property real titleH: 19 * s

    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: Theme.colorSurface
        border.width: 1
        border.color: Theme.colorBorderMid
    }

    Row {
        id: titleRow
        anchors {
            top: parent.top; topMargin: root.padTop
            left: parent.left; leftMargin: root.padX
            right: parent.right; rightMargin: root.padX
        }
        height: root.titleH
        spacing: Math.round(8 * root.s)

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            font.family: Theme.fontData
            font.pixelSize: Math.max(1, Math.round(Theme.fontSzMicro * root.s))
            font.letterSpacing: Theme.trackingMicro
            color: root.hue
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            // Chrome, so colorText3 — the one place on this panel that token is correct.
            // Every NUMBER and every evidence line is colorText2.
            width: Math.max(0, parent.width - x)
            text: root.caption
            font.family: Theme.fontBody
            font.pixelSize: Math.max(1, Math.round(Theme.fontSzMicro * root.s))
            color: Theme.colorText3
            elide: Text.ElideRight
            maximumLineCount: 1
        }
    }

    Loader {
        objectName: "diagramSlot"
        anchors {
            top: titleRow.bottom
            left: parent.left; leftMargin: root.padX
            right: parent.right; rightMargin: root.padX
            bottom: parent.bottom; bottomMargin: root.padBottom
        }
        sourceComponent: root.diagram
    }
}
