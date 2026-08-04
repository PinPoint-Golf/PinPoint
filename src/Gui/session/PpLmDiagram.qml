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

// A DESIGN-COORDINATE CONTAINER for one launch monitor schematic.
//
// The design brief specifies every annotation anchor as a pixel inside a fixed container
// — "target line x 20 to 652 at y 96", "smash chip at (314, 106)" — and calls that table
// the layout contract. This item is what lets those numbers stay literal: children
// position with `x: d(20)` and size type with `f(Theme.fontSzMicro)`, and the whole
// diagram scales to whatever room the card actually has.
//
// SCALED BY COORDINATE, NOT BY `scale:`. A transform would be one line instead of a
// function call on every anchor, and it would resample every glyph on the card — at the
// 8.5 px floor these annotations live at, that is the difference between a number and a
// smudge. Multiplying the coordinates keeps text rendering at its native size.
//
// The scale is uniform: one factor for both axes, so a diagram is never stretched. The
// spare pixels on the other axis go into centring rather than into distortion, because
// these are geometric drawings — an attack angle drawn 3° steeper than it was measured
// because the card was short is a wrong reading, not a stylistic liberty.

import QtQuick
import PinPointStudio

Item {
    id: root

    // The container size the brief's anchors are quoted in.
    property real designW: 672
    property real designH: 182

    // The uniform scale from design pixels to real ones. Falls back to 1 before the
    // first layout pass so a child's binding never divides by zero.
    readonly property real s: (designW > 0 && designH > 0 && width > 0 && height > 0)
                              ? Math.min(width / designW, height / designH) : 1

    // A design coordinate or length, in real pixels.
    function d(v) { return v * root.s }

    // A design font size, in real pixels. Rounded because a fractional pixel size makes
    // Qt hint the glyphs differently between two cards showing the same label, and
    // floored at 1 so a very small card degrades to unreadable rather than to invisible.
    function f(v) { return Math.max(1, Math.round(v * root.s)) }

    // Where the scaled design frame actually sits inside this item — the drawing is
    // centred in whatever it was given. Children anchor to `frame`, not to the item, so
    // the brief's origin really is the top-left of the design rectangle.
    Item {
        id: frame
        objectName: "diagramFrame"
        width: root.d(root.designW)
        height: root.d(root.designH)
        anchors.centerIn: parent
    }

    // Children are reparented into the centred frame, so `x: d(20)` means 20 design
    // pixels from the design origin and not from the card's padding box.
    default property alias content: frame.data
}
