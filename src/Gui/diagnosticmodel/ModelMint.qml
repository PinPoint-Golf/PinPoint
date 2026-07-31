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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// Minting a measure from facets.
//
// A measure is a SERIES (what · quantity · relative to what) sampled by a REDUCER (at which phase),
// and it is built by tapping chips rather than typing a name — the name is generated from the
// facets, deterministically, so two authors describing one measure cannot produce two of them.
//
// Every chip row is gated by the validity table in C++, so the picker never offers something the
// loader will then reject. And the near-duplicate check fires HERE, at creation, because after the
// fact nobody merges them — that is the whole defence against a library filling with almost
// identical measures inside a month.
Popup {
    id: root

    property var browser: null

    signal minted(string id)

    width:   Theme.sp(460)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    property string _what:      ""
    property string _quantity:  ""
    property string _reference: ""
    property string _anchor:    "p1"

    onOpened: { _what = ""; _quantity = ""; _reference = ""; _anchor = "p1"; phrase.text = "" }

    readonly property var _facets: ({ what: _what, quantity: _quantity,
                                      reference: _reference, anchor: _anchor })
    readonly property var _preview: browser ? browser.previewMeasure(_facets) : ({})

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    contentItem: ColumnLayout {
        spacing: Theme.sp(8)

        Text {
            Layout.fillWidth:  true
            Layout.margins:    Theme.sp(14)
            Layout.bottomMargin: 0
            text:                qsTr("NEW MEASURE")
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
        }

        // A typed phrase SEEDS the chips; it is not a query. Wrong guesses are corrected by tapping,
        // which is far easier than rephrasing a search that returned nothing.
        PpTextField {
            id: phrase
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(14)
            Layout.rightMargin: Theme.sp(14)
            placeholderText: qsTr("Describe it — e.g. “lead knee angle to ground”")
            onEditingFinished: {
                if (!root.browser || text.length === 0) return
                var seed = root.browser.seedFacetsFromPhrase(text)
                if (seed.what)      root._what      = seed.what
                if (seed.quantity)  root._quantity  = seed.quantity
                if (seed.reference) root._reference = seed.reference
            }
        }

        ModelChipRow {
            Layout.fillWidth: true
            label:   qsTr("What")
            options: root.browser ? root.browser.anatomyRoles() : []
            chosen:  root._what
            onPicked: (v) => { root._what = v; root._quantity = ""; root._reference = "" }
        }

        ModelChipRow {
            Layout.fillWidth: true
            label:   qsTr("Quantity")
            options: root.browser && root._what ? root.browser.quantitiesFor(root._what) : []
            chosen:  root._quantity
            onPicked: (v) => { root._quantity = v; root._reference = "" }
        }

        ModelChipRow {
            Layout.fillWidth: true
            label:   qsTr("Relative to")
            options: root.browser && root._what && root._quantity
                         ? root.browser.referencesFor(root._what, root._quantity) : []
            chosen:  root._reference
            onPicked: (v) => root._reference = v
        }

        ModelChipRow {
            Layout.fillWidth: true
            label:   qsTr("Read at")
            options: root.browser ? root.browser.phases() : []
            chosen:  root._anchor
            onPicked: (v) => root._anchor = v
        }

        // The generated name, live. The name is never typed — identity is the facet tuple, and the
        // string is for humans only.
        Rectangle {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(14)
            Layout.rightMargin: Theme.sp(14)
            Layout.preferredHeight: previewText.implicitHeight + Theme.sp(16)
            radius: Theme.radius
            color:  Theme.colorBg2

            Text {
                id: previewText
                anchors.fill: parent
                anchors.margins: Theme.sp(8)
                text: root._preview.valid === true
                          ? root._preview.label
                          : (root._preview.reason || qsTr("Pick what is being measured"))
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color: root._preview.valid === true ? Theme.colorText : Theme.colorText3
                wrapMode: Text.WordWrap
            }
        }

        // Reuse beats creation. An exact match means the measure already exists and this would be a
        // second name for one number; a near-duplicate is one facet away and is usually a mistake.
        Text {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(14)
            Layout.rightMargin: Theme.sp(14)
            visible: root._preview.exactMatch !== undefined
            text: qsTr("This already exists as “%1” — use that one.")
                      .arg(root._preview.exactMatch ? root._preview.exactMatch.label : "")
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorWarn
            wrapMode:       Text.WordWrap
        }

        Text {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(14)
            Layout.rightMargin: Theme.sp(14)
            visible: root._preview.nearDuplicates !== undefined
                     && root._preview.nearDuplicates.length > 0
            text: {
                var l = root._preview.nearDuplicates || []
                var names = []
                for (var i = 0; i < l.length && i < 3; i++) names.push(l[i].label)
                return qsTr("One facet away from: %1").arg(names.join(" · "))
            }
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorWarn
            wrapMode:       Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth:    true
            Layout.margins:      Theme.sp(14)
            Layout.topMargin:    0
            spacing: Theme.sp(8)

            Item { Layout.fillWidth: true }

            PpButton { label: qsTr("Cancel"); onClicked: root.close() }

            PpButton {
                label:   qsTr("Create")
                primary: true
                // A measure with no producer is a legitimate outcome — it is the roadmap's input —
                // so nothing here blocks on that. Only an invalid or duplicate series blocks.
                enabled: root._preview.valid === true && root._preview.exactMatch === undefined
                onClicked: {
                    var r = root.browser.mintMeasure(root._facets)
                    if (r.ok === true) root.minted(r.id)
                    root.close()
                }
            }
        }
    }
}
