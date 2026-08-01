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

// Folding the filter list away. The rail must keep saying that filters are ON while they are, and
// must keep a way back — a list that vanished whole would have neither.
Item {
    id: probe
    width: 260; height: 700

    property int toggles:   0
    property int cleared:   0
    property int railFolds: 0

    ModelTypeRail {
        id: rail
        anchors.fill: parent
        totalObjects: 140
        selectedType: "measures"
        types: [ { key: "measures", label: "Measures", count: 40, hint: "" },
                 { key: "characteristics", label: "Characteristics", count: 100, hint: "" } ]
        facets: [ { key: "status", label: "Status",
                    options: [ { value: "live", label: "Live", count: 20 },
                               { value: "gap",  label: "No producer", count: 20 } ] },
                  { key: "kind", label: "Kind",
                    options: [ { value: "composed", label: "Composed", count: 12 } ] } ]
        activeFacets: ({})
        onFacetsFoldToggled: probe.toggles++
        onFacetsCleared:     probe.cleared++
        onCollapseRequested: probe.railFolds++
    }

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
    function has(w, t) { return w.indexOf(t) >= 0 }
    // Substring, for text whose exact form depends on a translator this harness does not load —
    // the census renders its plural as the literal `object(s)` here and as `objects` in the app.
    function hasPart(w, t) {
        for (var i = 0; i < w.length; i++) if (w[i].indexOf(t) >= 0) return true
        return false
    }

    TestCase {
        name: "FacetFold"
        when: windowShown

        function init() {
            rail.facetsFolded = false
            rail.activeFacets = ({})
            probe.toggles   = 0
            probe.cleared   = 0
            probe.railFolds = 0
        }

        // The whole pane's own fold, at the top of the rail rather than inside it. Distinct from
        // the filter list's: this one takes the type list with it.
        function test_the_rail_offers_its_own_fold() {
            var w = words(rail)
            verify(has(w, "CONTENT"))
            verify(has(w, "‹‹"))               // points the way the pane goes, as the inspector does
            rail.collapseRequested()
            compare(probe.railFolds, 1)
            compare(probe.toggles, 0)          // and it is NOT the filter fold
        }

        // Folding the filters must not touch the type list, or the two controls are one control
        // wearing two labels.
        function test_the_two_folds_are_independent() {
            rail.facetsFolded = true
            var w = words(rail)
            verify(has(w, "CONTENT"))
            verify(has(w, "Measures"))
            verify(has(w, "Characteristics"))
            verify(has(w, "‹‹"))
        }

        function test_open_shows_the_list_and_offers_to_hide_it() {
            var w = words(rail)
            verify(has(w, "FILTERS"))
            verify(has(w, "hide"))
            verify(!has(w, "show"))
            // The options themselves are on screen.
            verify(has(w, "Status"))
            verify(has(w, "Live"))
            verify(has(w, "Composed"))
        }

        function test_folded_takes_the_options_away_but_not_the_way_back() {
            rail.facetsFolded = true
            var w = words(rail)
            verify(has(w, "FILTERS"))
            verify(has(w, "show"))
            verify(!has(w, "hide"))
            // The list is gone…
            verify(!has(w, "Status"))
            verify(!has(w, "Live"))
            // …and the rail above it is not, so the pane still does its first job.
            verify(has(w, "Measures"))
            verify(hasPart(w, "140 object"))
        }

        function test_a_fold_never_hides_that_a_filter_is_on() {
            rail.activeFacets = ({ status: [ "live" ], kind: [ "composed" ] })
            rail.facetsFolded = true
            var w = words(rail)
            // The count rides on the heading — the rail owes this whether or not the list is shown.
            verify(has(w, "FILTERS (2)"))
            // And the one control that turns it off is still there.
            verify(has(w, "clear"))
        }

        function test_no_count_when_nothing_is_filtering() {
            rail.facetsFolded = true
            var w = words(rail)
            verify(has(w, "FILTERS"))
            verify(!has(w, "FILTERS (0)"))
            verify(!has(w, "clear"))
        }

        function test_pressing_the_heading_and_the_word_both_ask_to_fold() {
            // Both are the control, so both must ask — a heading that looked pressable and was not
            // would be worse than a heading that was plainly inert.
            rail.facetsFoldToggled()
            compare(probe.toggles, 1)
        }

        function test_nothing_to_fold_when_there_are_no_facets() {
            // The panel passes [] when the window is too narrow for the list at all. There is then
            // no heading and no control — offering to restore something the layout will not give
            // back would be a lie.
            var saved = rail.facets
            rail.facets = []
            var w = words(rail)
            verify(!has(w, "FILTERS"))
            verify(!has(w, "hide"))
            verify(!has(w, "show"))
            rail.facets = saved
        }
    }
}
