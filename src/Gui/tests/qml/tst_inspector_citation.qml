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

// The citation field is an editor AND a way through to the paper.
//
// The value has to stay the bare identifier — it is the join key the bibliography is searched on —
// so the title can only ever be a second thing on the row. What is worth pinning is that the two do
// not swallow each other: typing in the field must not navigate, clicking the title must not edit,
// and a citation that resolves to nothing must offer no link at all rather than a dead one.
//
// model_browser_test proves the façade hangs the link on the right rows. This proves the delegate
// renders it and that clicking it goes somewhere — the half a façade test cannot reach.
Item {
    id: probe
    width: 520; height: 420

    property var navigated: []

    function fieldSection(rows) {
        return { title: "Fields", kind: "fields", note: "", action: "", count: rows.length,
                 rows: rows }
    }

    function citationRow(value, linkId, linkLabel) {
        var r = { type: "", id: "citation", label: "Citation", detail: "DOI, PMID or ISBN",
                  tone: "", navigable: false, field: "citation", kind: "text",
                  value: value, options: [] }
        if (linkId !== "") {
            r.linkType  = "references"
            r.linkId    = linkId
            r.linkLabel = linkLabel
        }
        return r
    }

    // The pane with a citation that resolves. Rebuilt per test rather than bound once: the
    // unresolved case below writes `detail`, and QtTest gives no guarantee it runs last.
    function citedDetail() {
        return {
            found: true, type: "characteristics", id: "s_posture",
            label: "Poor posture", eyebrow: "Characteristic", subtitle: "s_posture",
            badges: [], source: "core", dirty: false,
            sections: [ probe.fieldSection([
                probe.citationRow("10.1177/0363546503261729", "ref.gluck2008",
                                  "The spine in golf: a review")
            ]) ]
        }
    }

    ModelInspector {
        id: ins
        anchors.fill: parent
        editable: true
        onNavigate: (t, id) => probe.navigated.push(t + ":" + id)
    }

    // The link, found by its text rather than by walking to a fixed child index — a delegate that
    // gains a sibling should not break this.
    function findText(item, wanted) {
        if (item.text !== undefined && String(item.text) === wanted) return item
        for (var i = 0; i < item.children.length; i++) {
            var hit = findText(item.children[i], wanted)
            if (hit) return hit
        }
        return null
    }

    TestCase {
        name: "InspectorCitation"
        when: windowShown

        function init() {
            probe.navigated = []
            ins.detail = probe.citedDetail()
        }

        function test_the_field_shows_the_title_beside_the_identifier() {
            var link = findText(ins, "The spine in golf: a review")
            verify(link !== null)
            verify(link.visible)
            // The FIELD still holds the identifier. If the title had replaced it, the author would
            // be editing a title into a join key the next time they touched the row.
            var editor = findChild(ins, "fieldEditor:citation:text")
            verify(editor !== null)
            compare(editor.text, "10.1177/0363546503261729")
            verify(editor.enabled)
        }

        function test_clicking_the_title_opens_the_reference() {
            var link = findText(ins, "The spine in golf: a review")
            verify(link !== null)
            var p = link.mapToItem(ins, link.width / 2, link.height / 2)
            mouseClick(ins, p.x, p.y)
            compare(probe.navigated, [ "references:ref.gluck2008" ])
        }

        // A dead link is worse than no link: the reader is told there is a paper and then shown an
        // empty pane. The façade hangs nothing on an unresolved citation, and the delegate has to
        // draw nothing for it.
        function test_an_unresolved_citation_offers_no_link() {
            ins.detail = ({
                found: true, type: "characteristics", id: "made_up",
                label: "Made up", eyebrow: "Characteristic", subtitle: "made_up",
                badges: [], source: "core", dirty: false,
                sections: [ probe.fieldSection([
                    probe.citationRow("99999999", "", "")
                ]) ]
            })
            compare(findText(ins, "The spine in golf: a review"), null)
            // …and the field is still there and still editable, which is the half that must survive.
            var editor = findChild(ins, "fieldEditor:citation:text")
            verify(editor !== null)
            compare(editor.text, "99999999")
        }
    }
}
