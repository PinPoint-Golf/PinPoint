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

// The shot picker inside the filter menu — the selector that stands in for the film strip
// while the strip is folded away.
//
// THE THREE ASSERTIONS THAT EARN THIS FILE, because each is a rule that would fail silently
// and look like a design choice rather than a defect:
//
//   · The picker is ABSENT with the strip up. It exists because a folded strip leaves no way
//     to reach a swing; shown alongside the cards it is a second selector for the same job,
//     and the menu stops being "just the filter". A leaked section renders perfectly well.
//   · One click on the chip ALREADY on the stage still emits. The deselect is the host's
//     toggle (PpShotCarousel._toggleShot, shared with the cards), so a picker that quietly
//     swallowed the repeat click would leave a swing that can be put on the stage and never
//     taken off — and the panel would look correct in every screenshot.
//   · The picker's model IS the proxy, and it follows it live. That is the whole of "honours
//     filtering": there is no filter logic in the panel to get wrong, only the choice of
//     model, and the wrong choice shows a swing the strip beside it would not.
//
// THE PROXY'S OWN ACCEPT RULE IS NOT DRIVEN FROM HERE, and that is a limit worth stating
// rather than leaving to be discovered. ShotFilterProxyModel::filterAcceptsRow reads
// ShotListModel's role NUMBERS; a QML ListModel numbers its own roles, so a band set on the
// proxy over this fixture would exercise the fixture's numbering and not the rule. The rule
// belongs to the proxy and is unchanged by this work; what is new — and what is tested — is
// that the picker lists whatever the proxy leaves.
Item {
    id: probe
    width: 420; height: 700

    // Six shots, newest first — the shape ShotListModel publishes, with every role the
    // picker's delegate requires. Filled in init() so a test that mutates the set cannot
    // leak into the next one; uniform rows, because a ListModel takes its roles from the
    // first element and a row missing one reads as undefined everywhere.
    ListModel { id: shots }

    readonly property var fixture: [
        { shotId: 106, ordinal: 6, club: "7 iron", timestampLabel: "14:31",
          score: 82, swingDir: "/s/swing_0006" },
        { shotId: 105, ordinal: 5, club: "7 iron", timestampLabel: "14:29",
          score: 61, swingDir: "/s/swing_0005" },
        { shotId: 104, ordinal: 4, club: "Driver", timestampLabel: "14:26",
          score: 44, swingDir: "/s/swing_0004" },
        { shotId: 103, ordinal: 3, club: "Driver", timestampLabel: "14:24",
          score: 77, swingDir: "/s/swing_0003" },
        { shotId: 102, ordinal: 2, club: "Driver", timestampLabel: "14:21",
          score: 18, swingDir: "/s/swing_0002" },
        { shotId: 101, ordinal: 1, club: "Driver", timestampLabel: "14:18",
          score: 55, swingDir: "/s/swing_0001" }
    ]

    ShotFilterProxyModel {
        id: proxy
        sourceModel: shots
    }

    // What the panel ASKED FOR. The toggle itself lives in the carousel and is shared with
    // the film-strip cards, so the contract under test is the emission, not the outcome.
    property int    pickCount:    0
    property int    lastShotId:   -1
    property string lastSwingDir: ""

    PpShotFilter {
        id: panel
        anchors.top:  parent.top
        anchors.left: parent.left
        proxy: proxy
        shots: proxy
        onShotToggled: (shotId, swingDir) => {
            probe.pickCount++
            probe.lastShotId   = shotId
            probe.lastSwingDir = swingDir
        }
    }

    TestCase {
        name: "ShotPicker"
        when: windowShown

        function init() {
            shots.clear()
            for (var i = 0; i < probe.fixture.length; ++i) shots.append(probe.fixture[i])
            panel.showShots      = false
            panel.focusedShotId  = -1
            panel.focusedSummary = ({})
            probe.pickCount      = 0
            probe.lastShotId     = -1
            probe.lastSwingDir   = ""
        }

        function grid() { return findChild(panel, "shotPickerGrid") }

        // Folding the strip changes the Column's layout, and a Column re-lays-out on polish —
        // reading a height or clicking a chip in the same tick measures and presses the panel
        // as it was before the picker appeared.
        function reveal() {
            panel.showShots = true
            waitForRendering(panel)
        }
        function fold() {
            panel.showShots = false
            waitForRendering(panel)
        }

        // ── the strip is up: the cards are the selector, the menu is the filter ──
        function test_the_picker_is_absent_until_the_strip_is_folded() {
            fold()                                 // the tests run in name order, not this one's
            const filterOnlyHeight = panel.implicitHeight
            const g = grid()
            // The GridView object exists (it sits in the tree, not behind a Loader), but with
            // the strip up it is given no model and no height: nothing to see, nothing to click.
            verify(g !== null)
            compare(g.count, 0)
            verify(!g.visible)

            reveal()
            compare(g.count, 6)
            verify(g.visible)
            // ...and the panel grew to carry it, rather than overlaying the filters.
            verify(panel.implicitHeight > filterOnlyHeight + Theme.sp(40))
        }

        // ── one click names the swing it was clicked on ─────────────────────────
        function test_one_click_names_the_shot_it_was_clicked_on() {
            reveal()
            const cell = grid().itemAtIndex(2)     // third chip — ordinal 4, id 104
            verify(cell !== null)
            compare(cell.ordinal, 4)

            mouseClick(cell)
            compare(probe.pickCount, 1)
            compare(probe.lastShotId, 104)
            compare(probe.lastSwingDir, "/s/swing_0004")
        }

        // ── ...and one click on the swing already there takes it off ────────────
        function test_the_shot_on_the_stage_is_ringed_and_still_clickable() {
            reveal()
            panel.focusedShotId = 104

            const picked = grid().itemAtIndex(2)
            const other  = grid().itemAtIndex(0)
            verify(picked.picked)                  // ringed: this is the swing on the stage
            verify(!other.picked)

            // The repeat click MUST still reach the host — the deselect is the host's toggle.
            mouseClick(picked)
            compare(probe.pickCount, 1)
            compare(probe.lastShotId, 104)
        }

        // ── it lists the filtered set, because it lists the proxy — and live ─────
        function test_the_picker_lists_the_proxy_and_follows_it() {
            reveal()
            const g = grid()
            compare(g.model, proxy)                // not `shots`: the filter's output, not its input
            compare(g.count, 6)

            // Whatever removes a row from the proxy — a band chip above, a trashed shot —
            // reaches the picker as a row that is simply no longer there.
            shots.remove(2)                        // ordinal 4
            waitForRendering(g)
            compare(g.count, 5)
            compare(g.itemAtIndex(2).ordinal, 3)
        }

        // ── an empty set is stated, not left blank ──────────────────────────────
        function test_an_empty_set_keeps_its_band() {
            reveal()
            shots.clear()
            compare(grid().count, 0)
            // A zero-height picker reads as a section that failed to load rather than as a
            // filter that matched nothing, so the band stays open for the message.
            verify(grid().height > 0)
        }
    }
}
