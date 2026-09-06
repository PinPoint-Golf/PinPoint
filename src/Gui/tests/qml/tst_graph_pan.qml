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

import QtQuick
import QtTest
import PinPointStudio

// What a drag on the graph canvas means. The graph is read far more often than it is edited, so a
// bare drag moves the picture and the marquee moved behind Shift. The cases that matter are the
// three a hand can produce without being told any of that: a drag on nothing, a drag with Shift
// held, and a press that never travelled.
Item {
    id: probe
    width: 900; height: 500

    property var asked: []

    ModelGraph {
        id: g
        anchors.fill: parent
        focusId: "slice"
        layoutData: ({
            width: 700, height: 300, focusX: 500, focusY: 150, truncated: false,
            headings: [], edges: [],
            nodes: [
                { id: "slice", kind: "focus", label: "Slice", rank: 0,
                  x: 440, y: 130, w: 120, h: 34, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "characteristics" },
                { id: "over_the_top", kind: "cause", label: "Over the top", rank: -1,
                  x: 140, y: 130, w: 130, h: 34, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "characteristics" }
            ]
        })
        onSelectionRequested: (nodeIds, edgeIds) => probe.asked.push({ n: nodeIds, e: edgeIds })
    }

    TestCase {
        name: "GraphPan"
        when: windowShown

        // Zoomed to 2×, because a FITTED graph has nowhere to pan to — the picture is already
        // inside the pane. It is the zoomed reading case that needed a hand in the first place.
        // Layout 700×300 in a 900×500 pane fits at 1, so this makes the content 1400×600 and
        // leaves 500×100 of room. Pane coordinates are then twice the layout's, with no offset.
        function init() {
            // Back to fitted first: at 1× the content is the pane, so the Flickable has to return
            // whatever the last case panned to, and each test starts looking at the same place.
            g._userZoom = 1.0
            waitForRendering(g)
            g._userZoom = 2.0
            waitForRendering(g)
            compare(g._panX, 0)
            probe.asked = []
        }

        // A point in pane coordinates that no node is under: above every box, and clear of the
        // toggles that hang off their sides.
        readonly property real emptyX: 700
        readonly property real emptyY: 40

        function test_a_bare_drag_moves_the_picture() {
            verify(g._panX < 1)                       // opens at the left edge
            mousePress(g,   emptyX, emptyY)
            mouseMove(g,    emptyX - 200, emptyY, 0, Qt.LeftButton)
            compare(g._panX, 200)                     // the content came WITH the hand
            mouseRelease(g, emptyX - 200, emptyY)
            compare(g._panX, 200)                     // and stayed where it was put

            // A pan is a way of looking, not a way of choosing: it must not touch the selection.
            compare(probe.asked.length, 0)
        }

        // The pan is absolute from the press rather than accumulated, because this layer moves with
        // the content it is panning. A second move must land where the hand is, not twice as far.
        function test_the_pan_does_not_run_away() {
            mousePress(g,   emptyX, emptyY)
            mouseMove(g,    emptyX - 100, emptyY, 0, Qt.LeftButton)
            mouseMove(g,    emptyX - 150, emptyY, 0, Qt.LeftButton)
            compare(g._panX, 150)
            mouseRelease(g, emptyX - 150, emptyY)
        }

        // It stops at the edges rather than running off into white space.
        function test_it_stops_at_the_bound() {
            mousePress(g,   emptyX, emptyY)
            mouseMove(g,    emptyX - 4000, emptyY, 0, Qt.LeftButton)
            compare(g._panX, 500)                     // 1400 content − 900 pane
            mouseRelease(g, emptyX - 4000, emptyY)
        }

        // Shift keeps the marquee, and the marquee keeps its rule: a band that touches a node is a
        // node selection.
        function test_shift_still_marquees() {
            mousePress(g,   emptyX, emptyY, Qt.LeftButton, Qt.ShiftModifier)
            mouseMove(g,    340, 340, 0, Qt.LeftButton)
            mouseRelease(g, 340, 340, Qt.LeftButton, Qt.ShiftModifier)

            compare(g._panX, 0)                       // and it did NOT move the picture
            compare(probe.asked.length, 1)
            compare(probe.asked[0].n, [ "over_the_top" ])   // the box the band swept
            compare(probe.asked[0].e, [])
        }

        // A press that never travelled is still a click on empty canvas, which clears.
        function test_a_press_that_stays_put_still_clears() {
            mousePress(g,   emptyX, emptyY)
            mouseRelease(g, emptyX, emptyY)
            compare(g._panX, 0)
            compare(probe.asked.length, 1)
            compare(probe.asked[0].n, [])
            compare(probe.asked[0].e, [])
        }
    }
}
