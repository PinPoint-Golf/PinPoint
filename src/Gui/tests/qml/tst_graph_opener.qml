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

// The open/close control on a graph node: where it is, which side of which node carries one, and
// what a press on it does. Driven through the same MouseArea a hand would use — the control is
// hit-tested rather than being an Item of its own, so "drawn here" and "clickable here" are two
// numbers that must not be allowed to disagree.
Item {
    id: probe
    width: 900; height: 500

    // Chosen so the graph fits without scaling: _fitScale is min(1, 900/700, 500/300) = 1, and
    // `inner` is then centred at (100, 100). Layout coordinates therefore map to pane coordinates
    // by adding 100 — which is what lets a press be aimed at a layout position.
    readonly property real ox: 100
    readonly property real oy: 100

    property var fired: []

    ModelGraph {
        id: g
        anchors.fill: parent
        focusId: "slice"
        totalConditions: 140
        layoutData: ({
            width: 700, height: 300, focusX: 500, focusY: 150, truncated: true,
            headings: [], edges: [],
            nodes: [
                { id: "slice", kind: "focus", label: "Slice", rank: 0,
                  x: 440, y: 130, w: 120, h: 34, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "characteristics" },
                { id: "out_to_in_path", kind: "cause", label: "Path out-to-in", rank: -1,
                  x: 240, y: 130, w: 150, h: 34, available: true,
                  hiddenCauses: 0, hiddenEffects: 1, expanded: false, nodeType: "characteristics" },
                { id: "over_the_top", kind: "cause", label: "Over the top", rank: -2,
                  x: 40, y: 130, w: 130, h: 34, available: true,
                  hiddenCauses: 12, hiddenEffects: 2, expanded: false, nodeType: "characteristics" },
                { id: "chicken_wing", kind: "effect", label: "Chicken wing", rank: 1,
                  x: 620, y: 60, w: 130, h: 34, available: true,
                  hiddenCauses: 3, hiddenEffects: 4, expanded: false, nodeType: "characteristics" }
            ]
        })
        onExpandToggled: (id) => probe.fired.push(id)
        onCollapseAllRequested: probe.fired.push("*")
    }

    function leftOf(id)  { var n = g._nodeById(id)
                           return { x: n.x - Theme.sp(3) - g._togW / 2, y: n.y + n.h / 2 } }
    function rightOf(id) { var n = g._nodeById(id)
                           return { x: n.x + n.w + Theme.sp(3) + g._togW / 2, y: n.y + n.h / 2 } }

    property var pristine: null

    TestCase {
        name: "GraphOpener"
        when: windowShown

        // Every case starts from the same picture. A test that mutates layoutData must not decide
        // what the next one alphabetically is looking at.
        function init() {
            if (!probe.pristine) probe.pristine = JSON.stringify(g.layoutData)
            g.layoutData = JSON.parse(probe.pristine)
            probe.fired = []
        }

        function test_which_side_carries_one() {
            // A cause opens towards ITS causes, so the control is on the left and only there.
            compare(g._toggleAt(leftOf("over_the_top").x, leftOf("over_the_top").y),
                    "over_the_top|L")
            compare(g._toggleAt(rightOf("over_the_top").x, rightOf("over_the_top").y), "")

            // An effect opens the other way.
            compare(g._toggleAt(rightOf("chicken_wing").x, rightOf("chicken_wing").y),
                    "chicken_wing|R")
            compare(g._toggleAt(leftOf("chicken_wing").x, leftOf("chicken_wing").y), "")

            // Nothing hidden, nothing to press.
            compare(g._toggleAt(leftOf("slice").x, leftOf("slice").y), "")
            compare(g._toggleAt(leftOf("out_to_in_path").x, leftOf("out_to_in_path").y), "")

            // And bare canvas is bare canvas.
            compare(g._toggleAt(400, 20), "")
        }

        function test_a_press_opens_that_box() {
            probe.fired = []
            var p = leftOf("over_the_top")
            mousePress(g,   probe.ox + p.x, probe.oy + p.y)
            mouseRelease(g, probe.ox + p.x, probe.oy + p.y)
            compare(probe.fired.length, 1)
            compare(probe.fired[0], "over_the_top")
        }

        function test_it_is_not_the_node_behind_it() {
            // Pressing the control must not select, activate or nudge the node it hangs off.
            probe.fired = []
            var before = JSON.stringify(g._nudges)
            var p = leftOf("chicken_wing")   // no control on this side
            compare(g._toggleAt(p.x, p.y), "")

            p = rightOf("chicken_wing")
            mousePress(g,   probe.ox + p.x, probe.oy + p.y)
            mouseRelease(g, probe.ox + p.x, probe.oy + p.y)
            compare(probe.fired, [ "chicken_wing" ])
            compare(JSON.stringify(g._nudges), before)
        }

        function test_a_press_that_travels_off_is_abandoned() {
            probe.fired = []
            var p = leftOf("over_the_top")
            mousePress(g,   probe.ox + p.x, probe.oy + p.y)
            mouseMove(g,    probe.ox + p.x + 250, probe.oy + p.y + 60)
            mouseRelease(g, probe.ox + p.x + 250, probe.oy + p.y + 60)
            compare(probe.fired.length, 0)
        }


        // An OPEN box keeps its control so it can be closed again, even with nothing left hidden —
        // and the canvas ring grows the way back.
        function test_an_open_box_can_be_closed() {
            var d = JSON.parse(JSON.stringify(g.layoutData))
            for (var i = 0; i < d.nodes.length; i++)
                if (d.nodes[i].id === "over_the_top") {
                    d.nodes[i].expanded = true
                    d.nodes[i].hiddenCauses = 0     // the open box has nothing left behind it
                }
            g.layoutData = d

            verify(g._anyOpen())
            var p = leftOf("over_the_top")
            compare(g._toggleAt(p.x, p.y), "over_the_top|L")

            probe.fired = []
            mousePress(g,   probe.ox + p.x, probe.oy + p.y)
            mouseRelease(g, probe.ox + p.x, probe.oy + p.y)
            compare(probe.fired, [ "over_the_top" ])

            var ring = g._canvasRing()
            var found = false
            for (var k = 0; k < ring.length; k++)
                if (ring[k] && ring[k].verb === "collapseAll") found = true
            verify(found)
        }

        function test_collapse_all_is_offered_only_when_something_is_open() {
            verify(!g._anyOpen())
            var ring = g._canvasRing()
            var found = false
            for (var i = 0; i < ring.length; i++)
                if (ring[i] && ring[i].verb === "collapseAll") found = true
            verify(!found)
        }
    }
}
