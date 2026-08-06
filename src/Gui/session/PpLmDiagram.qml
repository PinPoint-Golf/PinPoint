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
// function call on every anchor, and it would resample every glyph on the card, turning
// the few reference labels that live in here into smudges. Multiplying the coordinates
// keeps text rendering at its native size.
//
// GEOMETRY SCALES; TYPE DOES NOT. There is no `f()` any more. A label in here is sized
// from a Theme token like every other label in the app, so a small card does not get small
// writing — it gets a smaller drawing under normal-sized writing. Only a handful of labels
// belong in here at all now (TARGET LINE, TOE/HEEL, the flight card's L and R): they name
// the picture rather than report a number, and every reading has moved out to the card's
// strip, where a laid-out row cannot collide with a drawing that changed size.
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

    // Whether this drawing is big enough to carry its own readings at the design anchors.
    // Set by the card that owns it (see PpLmCard.hosted for the test and why it is the
    // drawing's size that decides, not the layout's). The readings inside bind their
    // `visible` to it; when it is false they are drawn in the card's strip instead.
    property bool hosted: true

    // The uniform scale from design pixels to real ones. Falls back to 1 before the
    // first layout pass so a child's binding never divides by zero.
    readonly property real s: (designW > 0 && designH > 0 && width > 0 && height > 0)
                              ? Math.min(width / designW, height / designH) : 1

    // A design coordinate or length, in real pixels.
    function d(v) { return v * root.s }

    // ── the spare height, and what may use it ────────────────────────────────
    // These drawings are wide — 672 × 182 for the plan view — and a card is not. The scale
    // is uniform and therefore set by the WIDTH, so a tall card leaves a deep band of
    // nothing above and below the geometry while the labels stay clamped to the drawing's
    // own edges. It reads as a strip of content floating in an empty box.
    //
    // The drawing itself cannot take the room: stretching it vertically would draw an
    // attack angle steeper than it was measured, which is a wrong reading rather than a
    // stylistic liberty (see the header). The LABELS can, and should — they are captions,
    // not measurements, and nothing about their position claims anything.
    readonly property real slack: Math.max(0, (height - frame.height) / 2)
    // Not all of it. A reading pinned to the very edge of a tall card has stopped being an
    // annotation on a drawing and become a caption underneath one.
    readonly property real labelSpread: slack * 0.75

    // A LABEL's y, in the frame's coordinate space. Geometry uses d(); anything that names
    // rather than measures uses this, which pushes it away from the drawing's centre line
    // in proportion to how far out it already sits. A label near the middle barely moves;
    // one at the top or bottom edge takes most of the spare height. The proportion is what
    // keeps a stack of labels reading as a stack rather than as two clumps.
    function ly(v) {
        const c = root.designH / 2
        return root.d(v) + ((v - c) / c) * root.labelSpread
    }

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
