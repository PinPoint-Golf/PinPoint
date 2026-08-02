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

// The citation drawn INSIDE the condition that rests on it, under the measures.
//
// Same contract as the measure rows next door — one absolute y for the drawing and the hit test —
// and two things beyond it that only exist here:
//
//   1. A citation the bibliography has never heard of is drawn and is NOT a control. There is no
//      page behind it, so a press that opened one would open nothing, and a hover highlight would
//      offer a click that does not exist.
//   2. The two stacks share a box. `_condRowH` has to subtract BOTH or the name and the opener drift
//      down into the rows, and the citation has to start exactly where the measures stopped.
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
                // 40 for the condition's own row + one 24pt measure + one 24pt citation = 88.
                { id: "slice", kind: "focus", label: "Slice", rank: 0,
                  x: 340, y: 100, w: 160, h: 88, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "characteristics",
                  measures: [
                    { id: "m_faceToPath", label: "Face to path", statusLabel: "Live",
                      metricKey: "faceToPath", available: true, y: 140, h: 24 }
                  ],
                  references: [
                    { id: "ref.hume2005", citation: "10.2165/00007256-200535050-00003",
                      label: "The role of biomechanics in maximising distance and accuracy",
                      detailLabel: "2005", resolved: true, y: 164, h: 24 }
                  ] },
                // A citation the registry does not hold: drawn, marked, and not a control.
                { id: "over_the_top", kind: "cause", label: "Over the top", rank: -1,
                  x: 100, y: 110, w: 140, h: 64, available: true,
                  hiddenCauses: 3, hiddenEffects: 0, expanded: false, nodeType: "causes",
                  measures: [],
                  references: [
                    { id: "", citation: "99999999", label: "PMID 99999999",
                      detailLabel: "not in the bibliography", resolved: false, y: 150, h: 24 }
                  ] },
                // Neither kind of row. The unchanged case, one row high.
                { id: "early_extension", kind: "cause", label: "Early extension", rank: -1,
                  x: 100, y: 200, w: 140, h: 40, available: true,
                  hiddenCauses: 0, hiddenEffects: 0, expanded: false, nodeType: "causes",
                  measures: [], references: [] }
            ]
        })
        onNodeActivated: (t, id) => probe.opened.push(t + ":" + id)
    }

    function refCentre(nodeId, k) {
        var n = g._nodeById(nodeId)
        return { x: n.x + n.w / 2, y: n.references[k].y + n.references[k].h / 2 }
    }

    TestCase {
        name: "GraphReferences"
        when: windowShown

        function init() { probe.opened = [] }

        function test_the_citation_sits_inside_its_box_below_the_measures() {
            var n = g._nodeById("slice")
            compare(n.references.length, 1)
            var r = n.references[0]
            verify(r.y >= n.y)
            verify(r.y + r.h <= n.y + n.h + 0.001)
            // Below the measures, never over them — the order the layout fixes.
            var m = n.measures[0]
            compare(r.y, m.y + m.h)
            // …and it is the last row, so it reaches the bottom of the box.
            compare(r.y + r.h, n.y + n.h)
        }

        // The condition's own row is the box minus BOTH stacks. The toggle centres on it and the
        // name anchors to it, so counting only the measures misplaces both.
        function test_the_conditions_own_row_is_the_box_minus_every_inner_row() {
            compare(g._condRowH(g._nodeById("slice")), 40)
            compare(g._condRowH(g._nodeById("over_the_top")), 40)
            compare(g._condRowH(g._nodeById("early_extension")), 40)
        }

        function test_the_hit_test_finds_the_citation_row() {
            var a = refCentre("slice", 0)
            compare(g._referenceAt(a.x, a.y), "slice|ref.hume2005")
            // The measure row above it is not a citation row, and vice versa.
            var n = g._nodeById("slice")
            var m = { x: n.x + n.w / 2, y: n.measures[0].y + n.measures[0].h / 2 }
            compare(g._referenceAt(m.x, m.y), "")
            compare(g._measureAt(a.x, a.y), "")
            // Nor is the name, a box with no rows, or bare canvas.
            compare(g._referenceAt(a.x, n.y + 10), "")
            compare(g._referenceAt(170, 220), "")
            compare(g._referenceAt(600, 40), "")
        }

        // The whole point of `resolved`. The row is visible — a dangling citation must not look like
        // no citation — but there is no reference id, so there is nothing for a press to open.
        function test_an_unresolved_citation_is_drawn_but_is_not_a_control() {
            var a = refCentre("over_the_top", 0)
            compare(g._referenceAt(a.x, a.y), "")
            mousePress(g,   probe.ox + a.x, probe.oy + a.y)
            mouseRelease(g, probe.ox + a.x, probe.oy + a.y)
            // It falls through to the node, which is what any other part of the box does.
            compare(probe.opened, [ "causes:over_the_top" ])
        }

        // The rows sit inside the box, so the node hit test would win every time if it ran first.
        function test_pressing_the_citation_opens_the_paper_not_the_condition() {
            var a = refCentre("slice", 0)
            mousePress(g,   probe.ox + a.x, probe.oy + a.y)
            mouseRelease(g, probe.ox + a.x, probe.oy + a.y)
            compare(probe.opened, [ "references:ref.hume2005" ])
        }

        function test_pressing_the_name_still_opens_the_condition() {
            var n = g._nodeById("slice")
            var y = n.y + 10
            mousePress(g,   probe.ox + n.x + n.w / 2, probe.oy + y)
            mouseRelease(g, probe.ox + n.x + n.w / 2, probe.oy + y)
            compare(probe.opened, [ "characteristics:slice" ])
        }

        function test_a_press_that_travels_off_the_citation_is_abandoned() {
            var a = refCentre("slice", 0)
            mousePress(g,   probe.ox + a.x, probe.oy + a.y)
            mouseMove(g,    probe.ox + a.x + 300, probe.oy + a.y + 80)
            mouseRelease(g, probe.ox + a.x + 300, probe.oy + a.y + 80)
            compare(probe.opened.length, 0)
        }

        // The opener badge centres on the condition's row, not on the middle of a grown box.
        function test_the_opener_still_lands_on_a_box_grown_by_a_citation() {
            var n = g._nodeById("over_the_top")
            var p = { x: n.x - Theme.sp(3) - g._togW / 2, y: n.y + g._condRowH(n) / 2 }
            compare(g._toggleAt(p.x, p.y), "over_the_top|L")
        }
    }
}
