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
// WHICH LEAVES THE FIGURE FREE TO SAY ONE OTHER THING, and it says exactly one: that this
// reading is outside its corridor. Band identity is already on the label above it, so the
// two channels never compete for the same glyph — the words stay the metric's colour
// whatever the grade, and the number goes amber or red or stays as it was. Nothing here
// resolves a corridor; `grade` arrives from LmSessionModel with the reading it describes.
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
    // "" | "ideal" | "good" | "watch" | "action". Empty for a reading with no corridor, no
    // value, or a value the norm calls implausible — and drawn the same as Ideal and Good,
    // because this panel marks what is OUT and stays silent about everything else.
    property string grade: ""
    readonly property bool flagged: grade === "watch" || grade === "action"
    readonly property color flagColor: grade === "action" ? Theme.colorRagFault
                                                          : Theme.colorRagWatch

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
            // Named so the layout test can assert what colour a reading is printed in.
            // The corridor state is a fact a reader sees only as a hue, and a hue is
            // exactly the kind of thing that goes wrong silently.
            objectName: "readValue"
            id: readValue
            text: root.value
            // THE READING, at the size the headline strip prints its figures. It sat at
            // fontSzDataSm, one step down, which made every number on the board except the
            // six at the top quieter than the labels naming them deserved.
            font.family: Theme.fontData
            font.pixelSize: Theme.fontSzData
            color: root.flagged ? root.flagColor : Theme.colorText2
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
