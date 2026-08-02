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

// The ring's collar, which is WIDER than the spoke that opens it.
//
// Driven through track()/release() the way the canvas drives it during a hold, rather than through
// synthesised mouse events: the gesture starts on the canvas and the button never comes up, so the
// press is already grabbed by the time the ring exists and there is no press for a test to send.
Item {
    id: probe
    width: 1200; height: 1200

    property var committedValue: null
    property int committedSlot:  -1

    function spoke(values) {
        return { icon: "▤", label: "Strength", hint: "How strongly", kind: "value", values: values }
    }
    function action() { return { icon: "◎", label: "Open", hint: "In full", kind: "action" } }

    readonly property var ladder: [
        { value: "veryWeak",   label: "rarely",    current: false },
        { value: "weak",       label: "sometimes", current: false },
        { value: "moderate",   label: "often",     current: true  },
        { value: "strong",     label: "usually",   current: false },
        { value: "veryStrong", label: "always",    current: false }
    ]

    RingMenu {
        id: ring
        parent: probe
        onCommitted: (slot, entry, value) => { probe.committedSlot = slot; probe.committedValue = value }
    }

    // A point at `deg` from the ring's centre, `r` out. Screen coordinates put +y downward and the
    // component measures the same way, so -90 is north in both.
    function at(deg, r) {
        const a = deg * Math.PI / 180
        return Qt.point(ring.centreX + r * Math.cos(a), ring.centreY + r * Math.sin(a))
    }
    function goTo(deg, r) { const p = probe.at(deg, r); ring.track(p.x, p.y) }

    // Mid-collar: past the rim, inside the outer edge.
    readonly property real rCollar: (ring._r1 + ring._outer) / 2

    // Slot 0 is north. Every other slot is a plain action, so nothing else can open a collar.
    function openOn(values) {
        const model = [ probe.spoke(values), probe.action(), probe.action(), probe.action(),
                        probe.action(), probe.action(), probe.action(), probe.action() ]
        ring.openAt(probe.width / 2, probe.height / 2, model, "A link", "", false)
    }

    TestCase {
        name: "RingCollar"
        when: windowShown

        function init() {
            probe.committedValue = null
            probe.committedSlot  = -1
            ring.close()
        }

        function test_the_collar_holds_the_whole_five_rung_ladder() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)                  // out through the north spoke
            verify(ring._inCollar)
            compare(ring._collarCells, 5)

            // Wider than the wedge that opened it, and sized per cell rather than fixed.
            verify(ring._collarSweep > ring._sweep)
            compare(ring._collarSweep, 5 * ring._cellDeg)
        }

        // The regression the width depends on. Cell 0's centre sits ~52° off the spoke's axis, which
        // by the raw angle rule belongs to the NW slot — so without the latch the entry would swap
        // underneath the gesture and a release would commit a value to whatever field slot 7 holds.
        function test_a_wide_travel_stays_on_the_spoke_that_opened() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)
            compare(ring._hotSlot, 0)

            const firstCell = ring._collarBase + ring._collarSweep / 10   // centre of cell 0
            verify(Math.abs(firstCell - -90) > 22.5)                      // genuinely off its wedge
            probe.goTo(firstCell, probe.rCollar)
            compare(ring._hotSlot, 0)
            compare(ring._hotCell, 0)
        }

        function test_every_rung_is_one_hold_away() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)

            const span = ring._collarSweep / ring._collarCells
            for (var i = 0; i < 5; i++) {
                probe.goTo(ring._collarBase + (i + 0.5) * span, probe.rCollar)
                compare(ring._hotCell, i)
                compare(ring._hotSlot, 0)
            }
        }

        // Past the end of the arc is NOTHING. Clamping to the nearest end cell would let a fling
        // across the ring commit `rarely` or `always` without the hand ever being over them.
        function test_beyond_the_arc_commits_nothing() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)
            probe.goTo(-90 + ring._collarSweep, probe.rCollar)   // a full half-sweep past the end
            compare(ring._hotCell, -1)

            ring.release()
            compare(probe.committedValue, null)
        }

        function test_a_release_commits_the_cell_under_the_hand() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)

            const span = ring._collarSweep / ring._collarCells
            probe.goTo(ring._collarBase + 4.5 * span, probe.rCollar)   // the last rung
            compare(ring._hotCell, 4)
            ring.release()
            compare(probe.committedSlot, 0)
            compare(probe.committedValue, "veryStrong")
        }

        // The gesture's safety property: a hold that travels to the value it already has changes
        // nothing. It is what lets an author open the collar just to read what is set.
        function test_releasing_on_the_current_value_is_a_no_op() {
            probe.openOn(probe.ladder)
            probe.goTo(-90, probe.rCollar)

            const span = ring._collarSweep / ring._collarCells
            probe.goTo(ring._collarBase + 2.5 * span, probe.rCollar)   // `often`, the current one
            compare(ring._hotCell, 2)
            ring.release()
            compare(probe.committedValue, null)
        }

        // Only as wide as it has to be — a two-cell collar stays compact rather than spanning the
        // arc five rungs would need.
        function test_the_arc_is_sized_per_cell() {
            probe.openOn(probe.ladder.slice(0, 2))
            probe.goTo(-90, probe.rCollar)
            compare(ring._collarCells, 2)
            compare(ring._collarSweep, 2 * ring._cellDeg)

            // And never narrower than the spoke itself, which would read as a rendering fault.
            probe.openOn(probe.ladder.slice(0, 1))
            probe.goTo(-90, probe.rCollar)
            verify(ring._collarSweep >= ring._sweep)
        }
    }
}
