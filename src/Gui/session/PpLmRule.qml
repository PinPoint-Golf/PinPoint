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

// ONE STRAIGHT ANNOTATION LINE on a launch monitor schematic, and the LINE GRAMMAR that
// says what its style means.
//
// THE GRAMMAR IS THE RULE THE WHOLE VIEW DEPENDS ON, so it lives in one file rather than
// at forty call sites:
//
//   kBall      solid, in the metric's band hue — the BALL's own line: start direction,
//              the launch ray, the trajectory, the roll.
//   kClub      dashed, in the metric's band hue — a CLUB or SPIN AXIS: club path, face
//              angle, attack angle, dynamic loft, lie angle, spin axis.
//   kReference dashed, neutral — the target line, the ground, the face crosshair.
//   kBracket   solid, neutral and fainter, with end ticks — a ruler.
//
// A HUE-DASHED LINE IS A CLUB AXIS; A GREY-DASHED LINE IS A REFERENCE. A reader who
// blurs those two is reading a face angle as a target line, so `kind` is required and
// there is no way to set a colour and a dash pattern independently from outside.
//
// NO QtQuick.Shapes. Every line on these five cards except the two flight curves is a
// straight segment, and a rotated Item with a Repeater of Rectangles draws one with
// nothing but QtQuick — which is what lets the whole body load in the offscreen QML test
// harness, where the Shapes plugin is not linked.

import QtQuick
import PinPointStudio

Item {
    id: root

    // A string rather than a QML enum: enum scoping in QML differs between the attached
    // and unscoped forms across Qt versions, and a line that silently defaulted to
    // "reference" because the enum did not resolve would be a club axis drawn as a
    // target line — the one confusion the grammar exists to prevent.
    // One of "ball", "club", "reference", "bracket".
    property string kind: "reference"
    // The band hue for kBall/kClub. Ignored by the neutral kinds, which take their
    // colour from the grammar and must not be tintable by a caller.
    property color hue: Theme.colorText
    // Design-space length and the scale it is drawn at — the same `s` its PpLmDiagram
    // parent exposes, so a rule and the anchors around it scale together.
    property real len: 100
    property real s: 1
    property real thickness: 1
    // Degrees, clockwise-positive on screen, about `transformOrigin`.
    property real angle: 0
    // Where the rotation pivots: "left" (default), "right" or "center" of the line.
    //
    // PIVOT MOVES THE ROTATION, NOT THE POSITION. `x`/`y` are this item's TOP-LEFT in
    // every case, as they are for any QML item — so a line that should pass THROUGH a
    // point must be placed to put that point under its pivot: `x: cx - len/2` for
    // "center", `x: cx - len` for "right". Getting this wrong draws the mark beside the
    // thing it describes rather than on it, which is exactly how the club face ended up
    // 36 px right of the ball it was meant to be struck with. Call sites write the
    // subtraction out for that reason.
    property string pivot: "left"

    readonly property bool _neutral: kind === "reference" || kind === "bracket"
    readonly property bool _dashed:  kind === "club" || kind === "reference"
    // The two neutral alphas the brief specifies: a reference is readable, a ruler
    // recedes behind the thing it measures.
    readonly property color _color: _neutral
        ? Qt.rgba(Theme.colorText.r, Theme.colorText.g, Theme.colorText.b,
                  kind === "bracket" ? 0.18 : 0.22)
        : hue

    width: Math.max(1, len * s)
    height: Math.max(1, thickness * s)
    transformOrigin: pivot === "right" ? Item.Right
                   : pivot === "center" ? Item.Center : Item.Left
    rotation: angle

    // Solid: one rectangle, which is also the only case the ruler and the ball lines use.
    Rectangle {
        anchors.fill: parent
        visible: !root._dashed
        color: root._color
    }

    // Dashed: a run of equal segments. The pattern is sized in DESIGN pixels and scaled
    // like everything else, so a dashed line keeps its texture when the card grows —
    // a fixed 4 px dash on a bay TV reads as a solid line.
    Row {
        anchors.fill: parent
        visible: root._dashed
        spacing: Math.max(1, Math.round(3 * root.s))

        Repeater {
            model: {
                const dash = Math.max(1, Math.round(4 * root.s))
                const gap  = Math.max(1, Math.round(3 * root.s))
                return Math.max(1, Math.floor((root.width + gap) / (dash + gap)))
            }
            Rectangle {
                required property int index
                width: Math.max(1, Math.round(4 * root.s))
                height: root.height
                color: root._color
            }
        }
    }

    // A ruler's end ticks — 9 design px, the brief's figure, centred on the line.
    Repeater {
        model: root.kind === "bracket" ? 2 : 0
        Rectangle {
            required property int index
            width: Math.max(1, root.thickness * root.s)
            height: Math.max(2, 9 * root.s)
            x: index === 0 ? 0 : root.width - width
            y: (root.height - height) / 2
            color: root._color
        }
    }
}
