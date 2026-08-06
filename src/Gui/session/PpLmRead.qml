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

// ONE READING on a launch monitor schematic: a micro label in the metric's band hue over
// the figure in colorText2.
//
// ONE COMPONENT, TWO HOMES. The same block is used at a design anchor inside a diagram
// (where there is room to put it beside the line it describes) and in the card's readings
// strip (where there is not). It has to be one file: two would drift, and the two homes
// are two positions for the same thing, not two things.
//
// THE HUE IS ON THE LABEL, not the figure. It ties the words to the line they describe
// while keeping every number at the contrast the tiles board settled on, after a
// mid-chroma hue on colorSurface failed review once.
//
// FIXED TYPE. Nothing here answers to the diagram's scale — that is the whole point of
// the graphics view's type rule, and it is what makes this block safe to drop into a
// laid-out strip as well as onto a scaled drawing.

import QtQuick
import PinPointStudio

Column {
    id: root
    objectName: "anno"

    property string label: ""
    property string value: "—"
    property string unit: ""
    property color hue: Theme.colorText2
    // The metric this block names. Setting it makes the block the HOVER TARGET for that
    // metric's shaded region — a real-sized thing to point at, where the rotated 1 px line
    // it describes is not.
    property string metricKey: ""
    // A qualifier printed beside the label — "· PPS EST." where a figure is PinPoint's own
    // estimate rather than the device's reading, "· LOFT LESS ATTACK" where it is derived
    // from two others on the same card. Never silent: this panel's premise is that you can
    // tell what produced a number.
    property string note: ""

    signal hovered(string key, bool on)

    spacing: Theme.sp(2)

    HoverHandler {
        enabled: root.metricKey !== ""
        onHoveredChanged: root.hovered(root.metricKey, hovered)
    }

    Row {
        spacing: Theme.sp(4)
        Text {
            text: root.label
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color: root.hue
        }
        Text {
            text: root.note
            visible: root.note !== ""
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color: Theme.colorText3
        }
    }
    Row {
        spacing: Theme.sp(3)
        Text {
            id: readValue
            text: root.value
            // THE READING, at the size the headline strip prints its figures. It sat at
            // fontSzDataSm, one step down, which made every number on the board except the
            // six at the top quieter than the labels naming them deserved.
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzData
            color: Theme.colorText2
        }
        Text {
            anchors.baseline: readValue.baseline
            text: root.unit
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText2
            visible: root.unit !== ""
        }
    }
}
