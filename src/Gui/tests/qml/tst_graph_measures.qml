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

// Measures drawn INSIDE the condition they detect, rather than in a lane with a line up to it.
//
// The rows are a hit target as well as a drawing, and both read the same absolute y off the layout.
// That is the property worth testing: a row the reader can see is a row that opens its measure, and
// a row that draws in one place and presses in another is the defect two coordinate systems invite.
Item {
    id: probe
    width: 900; height: 500

    // layout 700x300 in a 900x500 pane fits without scaling, so `inner` centres at (100, 100) and
    // layout coordinates map to pane coordinates by adding 100.
    readonly property real ox: 100
    readonly property real oy: 100

    property var opened: []

    ModelGraph {
        id: g
        anchors.fill: parent
        focusId: "slice"
        layoutData: ({
            width: 700, height: 300, focusX: 400, focusY: 150, truncated: false,
            headings: [], edges: [],
            nodes: [
                // 40 for the condition's own row + two 24pt measure rows = 88 high.
                { id: "slice", kind: "focus", label: "Slice", rank: 0,
                  x: 340, y: 100, w: 160, h: 88, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "characteristics",
                  measures: [
                    { id: "m_spinAxis", label: "Spin axis", statusLabel: "Needs a launch monitor",
                      metricKey: "spinAxis", available: false, y: 140, h: 24 },
                    { id: "m_faceToPath", label: "Face to path", statusLabel: "Live",
                      metricKey: "faceToPath", available: true, y: 164, h: 24 }
                  ] },
                // A condition nothing detects: one row high, no measures.
                { id: "over_the_top", kind: "cause", label: "Over the top", rank: -1,
                  x: 100, y: 120, w: 140, h: 40, available: true,
                  hiddenCauses: 3, hiddenEffects: 0, expanded: false, nodeType: "causes",
                  measures: [] }
            ]
        })
        onNodeActivated: (t, id) => probe.opened.push(t + ":" + id)
    }

    function rowCentre(nodeId, k) {
        var n = g._nodeById(nodeId)
        return { x: n.x + n.w / 2, y: n.measures[k].y + n.measures[k].h / 2 }
    }

    TestCase {
        name: "GraphMeasures"
        when: windowShown

        function init() { probe.opened = [] }

        function test_a_row_is_inside_the_box_it_belongs_to() {
            var n = g._nodeById("slice")
            compare(n.measures.length, 2)
            for (var k = 0; k < n.measures.length; k++) {
                var m = n.measures[k]
                verify(m.y >= n.y)
                verify(m.y + m.h <= n.y + n.h + 0.001)
                // …and below the condition's own row, never over the name.
                verify(m.y >= n.y + g._condRowH(n) - 0.001)
            }
        }

        // The condition's own row is the box minus its measure rows. The toggle centres on it and
        // the name anchors to it, so a wrong answer here misplaces both.
        function test_the_conditions_own_row_is_the_box_minus_its_rows() {
            compare(g._condRowH(g._nodeById("slice")), 40)
            compare(g._condRowH(g._nodeById("over_the_top")), 40)
        }

        function test_the_hit_test_finds_each_row_separately() {
            var a = rowCentre("slice", 0), b = rowCentre("slice", 1)
            compare(g._measureAt(a.x, a.y), "slice|m_spinAxis")
            compare(g._measureAt(b.x, b.y), "slice|m_faceToPath")
            // The condition's own row is NOT a measure row — pressing the name opens the condition.
            compare(g._measureAt(a.x, g._nodeById("slice").y + 10), "")
            // Nor is a box that carries none, nor bare canvas.
            compare(g._measureAt(170, 140), "")
            compare(g._measureAt(600, 40), "")
        }

        // The rows sit inside the box, so the node hit test would win every time if it ran first.
        function test_pressing_a_row_opens_that_measure_not_the_condition() {
            var a = rowCentre("slice", 0)
            mousePress(g,   probe.ox + a.x, probe.oy + a.y)
            mouseRelease(g, probe.ox + a.x, probe.oy + a.y)
            compare(probe.opened, [ "measures:m_spinAxis" ])
        }

        function test_pressing_the_name_still_opens_the_condition() {
            var n = g._nodeById("slice")
            var y = n.y + 10
            mousePress(g,   probe.ox + n.x + n.w / 2, probe.oy + y)
            mouseRelease(g, probe.ox + n.x + n.w / 2, probe.oy + y)
            compare(probe.opened, [ "characteristics:slice" ])
        }

        function test_a_press_that_travels_off_a_row_is_abandoned() {
            var a = rowCentre("slice", 0)
            mousePress(g,   probe.ox + a.x, probe.oy + a.y)
            mouseMove(g,    probe.ox + a.x + 300, probe.oy + a.y + 80)
            mouseRelease(g, probe.ox + a.x + 300, probe.oy + a.y + 80)
            compare(probe.opened.length, 0)
        }

        // The opener badge centres on the condition's row, not on the middle of a grown box.
        function test_the_opener_still_lands_on_a_box_with_rows() {
            var n = g._nodeById("over_the_top")
            var p = { x: n.x - Theme.sp(3) - g._togW / 2, y: n.y + g._condRowH(n) / 2 }
            compare(g._toggleAt(p.x, p.y), "over_the_top|L")
        }
    }
}
