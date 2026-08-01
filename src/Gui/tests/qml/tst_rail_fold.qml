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

// The panel side of the rail fold: what the strip keeps, and the rule that a filter which is ON
// must remain visible however it came to be off screen. Three ways to lose the list — too narrow,
// rail folded, list folded — and every surface that compensates asks one _facetsVisible; a test on
// any single fold would pass while the other two leaked.
//
// WEAKER THAN THE OTHERS, and stated so rather than left to be discovered. DiagnosticModel needs a
// ModelBrowser on its `browser` context property and the whole shipped pack behind it, so this
// restates the panel's fold arithmetic here instead of loading it. It therefore asserts that the
// RULE is right, not that the panel is wired to it — if someone drops the `!_railFolded` term from
// _facetsVisible, this suite stays green. The other three files load the real component and are the
// pattern to follow; make this one do the same the day the panel is cheap enough to stand up.
Item {
    id: probe
    width: 1400; height: 700

    // Stands in for the panel's own bindings without needing a ModelBrowser behind it.
    property bool showFacets:   true
    property bool railFolded:   false
    property bool facetsFolded: false
    readonly property bool facetsVisible: showFacets && !railFolded && !facetsFolded

    property real railWidth: railFolded ? Theme.sp(26) : Theme.sp(214)

    TestCase {
        name: "RailFold"
        when: windowShown

        function init() {
            probe.showFacets = true
            probe.railFolded = false
            probe.facetsFolded = false
        }

        function test_the_folded_rail_keeps_a_strip_not_nothing() {
            compare(probe.railWidth, Theme.sp(214))
            probe.railFolded = true
            compare(probe.railWidth, Theme.sp(26))
            // Wide enough to hold the ‹‹ button, or the only route back is a shortcut nobody has
            // been told about.
            verify(probe.railWidth >= Theme.sp(22))
        }

        function test_a_hidden_filter_is_declared_however_it_was_hidden() {
            verify(probe.facetsVisible)                 // nothing to compensate for

            probe.railFolded = true                     // the pane, by hand
            verify(!probe.facetsVisible)
            probe.railFolded = false

            probe.facetsFolded = true                   // the list inside it, by hand
            verify(!probe.facetsVisible)
            probe.facetsFolded = false

            probe.showFacets = false                    // the window, not the author
            verify(!probe.facetsVisible)
        }

        function test_folds_compose_rather_than_cancel() {
            probe.railFolded = true
            probe.facetsFolded = true
            verify(!probe.facetsVisible)
            // Unfolding the OUTER one must not silently unfold the inner one — the author set both
            // and gets both back as they left them.
            probe.railFolded = false
            verify(!probe.facetsVisible)
            probe.facetsFolded = false
            verify(probe.facetsVisible)
        }
    }
}
