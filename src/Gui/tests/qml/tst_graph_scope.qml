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

// The scope control: what it says, what it offers at each end, and what pressing it asks for.
Item {
    id: probe
    width: 900; height: 500

    property var asked: []

    ModelGraph {
        id: g
        anchors.fill: parent
        focusId: "slice"
        scope: 2
        layoutData: ({
            width: 400, height: 200, focusX: 200, focusY: 100, truncated: true,
            headings: [], edges: [],
            nodes: [ { id: "slice", kind: "focus", label: "Slice", rank: 0,
                       x: 140, y: 80, w: 120, h: 34, available: true,
                       hiddenCauses: 0, hiddenEffects: 0, expanded: false,
                       nodeType: "characteristics" } ]
        })
        onScopeRequested: (v) => probe.asked.push(v)
    }

    // Every VISIBLE Text on the pane, in tree order, so the words are asserted as the reader sees
    // them rather than assumed from the source. Invisible ones are skipped deliberately: a node's
    // `+0` opener exists in the tree at all times and is shown only when it has something to offer.
    function words(item, out) {
        out = out || []
        for (var i = 0; i < item.children.length; i++) {
            var c = item.children[i]
            if (!c.visible) continue
            if (c.text !== undefined && String(c.text).length > 0) out.push(String(c.text))
            words(c, out)
        }
        return out
    }

    function runOf(w, first) {
        var at = w.indexOf(first)
        return at < 0 ? [] : w.slice(at, at + 3)
    }

    TestCase {
        name: "GraphScope"
        when: windowShown

        function test_it_is_worded_as_verbs() {
            var w = words(g)
            // The three cells in order, so `reduce` sits on the side that means less. A control
            // whose verbs are the right words in the wrong places is worse than the signs were.
            compare(runOf(w, "reduce"), [ "reduce", "2 of 4", "expand" ])
            // The old readout, and the arithmetic the reader had to decode to use it. The zoom pill
            // still uses \u2212 / + and is right to — a scale IS arithmetic — so this asks about the
            // scope group's own cells rather than about the pane as a whole.
            compare(w.indexOf("scope 2"), -1)
            verify(runOf(w, "reduce").indexOf("−") < 0)
            verify(runOf(w, "reduce").indexOf("+") < 0)
        }

        function test_it_asks_for_one_step_at_a_time() {
            probe.asked = []
            g.scopeRequested(3)                    // the signal the pill's `expand` emits
            compare(probe.asked, [ 3 ])
        }

        function test_the_ring_offers_the_same_control_by_the_same_name() {
            var ring = g._canvasRing()
            var spoke = null
            for (var i = 0; i < ring.length; i++)
                if (ring[i] && ring[i].verb === "scope") spoke = ring[i]
            verify(spoke !== null)
            verify(spoke.values.length > 0)
            // Never a step the layout would clamp away — that is what the old `+` did to 3 and 4.
            for (var k = 0; k < spoke.values.length; k++) {
                var v = parseInt(spoke.values[k].value)
                verify(v >= 1 && v <= g._scopeMax)
            }
        }

        // The switch row. `health` is the physical-screen layer and is the one switch that opens
        // ON — the screened causes are where the chain of technique faults bottoms out, so a
        // picture without them shows the swing and hides the reason for it.
        function test_the_switches_read_their_own_state() {
            var w = words(g)
            verify(w.indexOf("measures") >= 0)
            verify(w.indexOf("health") >= 0)
            verify(w.indexOf("weak") >= 0)
            verify(w.indexOf("proposed") >= 0)
            // Defaults, and they are deliberately not the same: measures opens off, health on.
            compare(g.includeMeasures, false)
            compare(g.includeScreened, true)
        }

        function test_every_switch_asks_by_its_own_key() {
            var asked = []
            function grab(k) { asked.push(k) }
            g.switchToggled.connect(grab)
            g.switchToggled("screened")
            g.switchToggled("measures")
            g.switchToggled.disconnect(grab)
            compare(asked, [ "screened", "measures" ])
        }

        function test_the_ends_are_shown_not_hidden() {
            g.scope = 1
            // Still drawn at the floor, just not pressable — a control that vanished at its limit
            // would take the answer to "is there further in?" with it.
            compare(runOf(words(g), "reduce"), [ "reduce", "1 of 4", "expand" ])
            g.scope = g._scopeMax
            compare(runOf(words(g), "reduce"), [ "reduce", "4 of 4", "expand" ])
            g.scope = 2
        }
    }
}
