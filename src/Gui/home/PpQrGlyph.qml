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


// The "Pair to my phone" icon: a QR code that is deliberately not one.
//
// ⚠ IT IS UNSCANNABLE ON PURPOSE, AND THAT IS THE WHOLE DESIGN.  It has the
// three finder squares a reader looks for and nothing else a reader needs —
// no timing pattern, no format-information stripe, no alignment block, no
// error correction, no data.  A scanner that locks onto the finders finds
// nothing behind them and gives up, which is the correct outcome: an icon that
// decoded to something would be an icon that could be photographed and acted
// on, and this one says "pairing lives here" and nothing more.
//
// ⚠ AND THE PATTERN IS DRAWN ONCE.  `Math.random()` picks a seed at creation
// and a tiny LCG expands it, so every launch gets a different arrangement but a
// given icon holds still.  Seeding per-paint would make it flicker on every
// hover, which reads as a rendering fault rather than as decoration.
//
// Not a glyph in Theme.fontSymbol: fontSymbol resolves to Apple Symbols on
// macOS and Segoe UI Symbol on Windows, and neither has a QR or a phone
// character.  A drawn one is identical on all three platforms and needs no
// entry in Theme.symbolScale.

import QtQuick
import PinPointStudio

Canvas {
    id: root

    // Module count per side.  11 keeps three 3×3 finders, a one-module
    // separator around each and a readable scatter between them; below 9 the
    // finders meet in the middle and it stops looking like a code.
    property int  modules: 11
    // Fraction of the free area that is dark.  Around 0.45 is what a real
    // symbol averages; much above it turns into a blob at icon size.
    property real density: 0.45
    property color color:  Theme.colorText

    implicitWidth:  Theme.sp(18)
    implicitHeight: Theme.sp(18)

    antialiasing: false

    // A 32-bit LCG (Numerical Recipes' constants).  Not for anything that
    // matters — this draws decoration — but it must be repeatable, and
    // Math.random() called per module is not.
    property int _seed: 1
    function _rand() {
        root._seed = (1664525 * root._seed + 1013904223) & 0x7fffffff
        return root._seed / 0x7fffffff
    }

    Component.onCompleted: {
        root._seed = 1 + Math.floor(Math.random() * 0x7ffffffe)
        root.requestPaint()
    }

    onColorChanged:   requestPaint()
    onModulesChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d")
        ctx.reset()

        var n = root.modules
        if (n < 9) return
        var s = Math.max(1, Math.floor(Math.min(width, height) / n))
        var pad = Math.floor((Math.min(width, height) - s * n) / 2)

        ctx.fillStyle = root.color

        // The three finders, top-left / top-right / bottom-left.  A real
        // symbol's fourth corner carries data, not a finder, and leaving it
        // empty is what makes this read as a QR rather than as a grid.
        var finders = [[0, 0], [n - 3, 0], [0, n - 3]]
        function inFinder(x, y) {
            for (var i = 0; i < finders.length; ++i) {
                // The finder itself plus one module of separator around it.
                if (x >= finders[i][0] - 1 && x <= finders[i][0] + 3
                 && y >= finders[i][1] - 1 && y <= finders[i][1] + 3) return true
            }
            return false
        }

        // Finder: a filled 3×3 with a hollow centre — the ring-and-dot a reader
        // looks for, at the smallest size that still reads at 18 px.
        for (var f = 0; f < finders.length; ++f) {
            var fx = finders[f][0], fy = finders[f][1]
            for (var dy = 0; dy < 3; ++dy)
                for (var dx = 0; dx < 3; ++dx)
                    if (dx !== 1 || dy !== 1)
                        ctx.fillRect(pad + (fx + dx) * s, pad + (fy + dy) * s, s, s)
        }

        // The scatter.  No data behind it and none intended.
        root._seed = root._seed || 1
        var seedAtStart = root._seed
        for (var y = 0; y < n; ++y) {
            for (var x = 0; x < n; ++x) {
                if (inFinder(x, y)) continue
                if (root._rand() < root.density)
                    ctx.fillRect(pad + x * s, pad + y * s, s, s)
            }
        }
        // Rewind, so a repaint redraws the SAME arrangement rather than
        // advancing the generator and shuffling the icon under the user.
        root._seed = seedAtStart
    }
}
