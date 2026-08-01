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

// The type rail: every content type with its live count, and the selected type's facets beneath.
// Counts come from the façade, which derives them by asking the row list — never a stored figure.
Item {
    id: root

    property var    types:        []      // [{ key, label, count, hint }]
    property var    facets:       []      // [{ key, label, options:[{value,label,count}] }]
    property var    activeFacets: ({})    // { key: [values…] }
    property string selectedType: ""
    property int    totalObjects: 0

    // Folded away by hand. The HEADING stays when it is — a filter list that vanished completely
    // would have nothing left to press to bring it back, and the count of what is still filtering
    // has to be readable from the fold or the rail is quietly lying about the rows.
    property bool   facetsFolded: false

    signal typePicked(string key)
    signal facetToggled(string key, string value)
    signal facetsCleared()
    signal facetsFoldToggled()
    signal collapseRequested()

    readonly property int _activeFacetCount: {
        var n = 0
        for (var k in root.activeFacets) n += (root.activeFacets[k] || []).length
        return n
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The heading and the way out of the pane, on one line. Mirrors the inspector's fold at the
        // other edge of the panel — same glyph pair, same tooltip shape, pointing the way the pane
        // goes — so learning either teaches the other.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(10)
            Layout.topMargin:    Theme.sp(12)
            Layout.bottomMargin: Theme.sp(6)

            Text {
                Layout.fillWidth:    true
                text:                qsTr("CONTENT")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.weight:         Theme.fontBodyWeight
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            Rectangle {
                id: foldButton
                Layout.preferredWidth:  Theme.sp(24)
                Layout.preferredHeight: Theme.sp(20)
                radius: Theme.radius
                color:  foldMa.containsMouse ? Theme.colorBg2 : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    anchors.centerIn: parent
                    text: "‹‹"
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzBody2
                    color: foldMa.containsMouse ? Theme.colorText2 : Theme.colorText3
                }

                ToolTip.visible: foldMa.containsMouse
                ToolTip.text: qsTr("Hide the content rail")
                              + (Qt.platform.os === "osx" ? "  ⌥\\" : "  Alt+\\")
                ToolTip.delay: 400

                PpPressable { id: foldMa; hoverScale: 1.0; onClicked: root.collapseRequested() }
            }
        }

        Repeater {
            model: root.types
            delegate: Item {
                id: typeRow
                required property var modelData

                Layout.fillWidth: true
                Layout.preferredHeight: Theme.sp(28)

                readonly property bool active: root.selectedType === modelData.key

                Rectangle {
                    anchors.fill: parent
                    color: typeRow.active    ? Theme.colorAccentLight
                         : typeHover.hovered ? Theme.colorBg2
                                             : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }

                HoverHandler { id: typeHover }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin:  Theme.sp(18)
                    anchors.rightMargin: Theme.sp(18)
                    spacing: Theme.sp(9)

                    Text {
                        Layout.fillWidth: true
                        text: typeRow.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        font.weight:    Theme.fontBodyWeight
                        color: typeRow.active ? Theme.colorAccent : Theme.colorText
                        elide: Text.ElideRight
                    }

                    Text {
                        text: typeRow.modelData.count
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color: typeRow.active ? Theme.colorAccent : Theme.colorText3
                    }
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root.typePicked(typeRow.modelData.key)
                }
            }
        }

        // The census. Summed from the rail above rather than stated, so it cannot disagree with it.
        Text {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(18)
            Layout.topMargin:    Theme.sp(8)
            text: qsTr("%n object(s) in all", "", root.totalObjects)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            elide:          Text.ElideRight
        }

        // Pushes the filters to the bottom of the rail — but it YIELDS to them, which is the whole
        // point of the minimum. It used to be the only greedy child here, so it took every spare
        // pixel and left the facet list on a fixed cap it routinely overflowed.
        Item { Layout.fillHeight: true; Layout.minimumHeight: Theme.sp(8) }

        // ── Facets ────────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
            visible: root.facets.length > 0
        }

        RowLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(18)
            Layout.rightMargin: Theme.sp(18)
            Layout.topMargin:   Theme.sp(12)
            visible: root.facets.length > 0

            // The heading is the fold's own control, so there is no separate strip to explain: the
            // thing you press to put the filters away is the thing that names them. Folded, it
            // carries the count of what is still narrowing the rows — the rail owes the reader that
            // whether or not the list itself is on screen.
            Text {
                Layout.fillWidth:    true
                text: root.facetsFolded && root._activeFacetCount > 0
                          ? qsTr("FILTERS (%1)").arg(root._activeFacetCount)
                          : qsTr("FILTERS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.weight:         Theme.fontBodyWeight
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color: root.facetsFolded && root._activeFacetCount > 0 ? Theme.colorAccent
                                                                       : Theme.colorText3
                PpPressable { hoverScale: 1.0; onClicked: root.facetsFoldToggled() }
            }

            Text {
                text:    qsTr("clear")
                // Offered while folded too. The count beside FILTERS says something is on; taking
                // away the one control that turns it off would make that a complaint rather than a
                // fact the reader can act on.
                visible: root._activeFacetCount > 0
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorAccent
                PpPressable { hoverScale: 1.0; onClicked: root.facetsCleared() }
            }

            // A word, not a chevron. `hide` and `show` say which way the press goes; a caret only
            // says which way something points and leaves the reader to work out the rest.
            Text {
                text:    root.facetsFolded ? qsTr("show") : qsTr("hide")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          facetFoldMa.containsMouse ? Theme.colorText2 : Theme.colorText3

                ToolTip.visible: facetFoldMa.containsMouse
                ToolTip.text: (root.facetsFolded ? qsTr("Show the filters")
                                                 : qsTr("Hide the filters"))
                              + (Qt.platform.os === "osx" ? "  ⌥⇧\\" : "  Alt+Shift+\\")
                ToolTip.delay: 400

                PpPressable {
                    id: facetFoldMa
                    hoverScale: 1.0
                    onClicked: root.facetsFoldToggled()
                }
            }
        }

        // Scrolled, because a facet list over a nine-value vocabulary is taller than the rail once
        // three of them are stacked.
        //
        // Takes the height it is ASKED for, up to what it needs, rather than a fixed cap. The cap
        // was Theme.sp(300), which fitted a nine-value vocabulary and one more — so the third facet
        // was already below the fold before the ranking facets were added, and those landed under
        // it. A scroll area with nothing to say it continues reads as a list that has ended, so the
        // rows past the cap were not merely awkward to reach: nobody knew they were there.
        //
        // The floor keeps it honest the other way. On a short window the list still scrolls rather
        // than collapsing to a sliver that looks like a rendering fault.
        ScrollView {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            Layout.preferredHeight: facetColumn.implicitHeight
            Layout.maximumHeight:   facetColumn.implicitHeight
            Layout.minimumHeight:   Theme.sp(140)
            Layout.bottomMargin:    Theme.sp(12)
            clip: true
            // A layout excludes an invisible child outright, so the floor above goes with it and a
            // folded list keeps none of the rail.
            visible: root.facets.length > 0 && !root.facetsFolded

            Column {
                id: facetColumn
                width: root.width
                spacing: 0

                Repeater {
                    model: root.facets
                    delegate: Column {
                        id: facetGroup
                        required property var modelData

                        width: facetColumn.width
                        spacing: 0

                        Text {
                            x: Theme.sp(18)
                            topPadding:    Theme.sp(8)
                            bottomPadding: Theme.sp(3)
                            text:           facetGroup.modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }

                        Repeater {
                            model: facetGroup.modelData.options
                            delegate: Item {
                                id: facetOption
                                required property var modelData

                                // Read off the group by id rather than by walking `parent.parent`:
                                // a layout change would silently re-point that chain at the wrong
                                // object, and the symptom would be a filter that ticks the wrong box.
                                readonly property string facetKey: facetGroup.modelData.key
                                readonly property bool   on: {
                                    var v = root.activeFacets[facetKey]
                                    return v !== undefined && v.indexOf(modelData.value) >= 0
                                }

                                width:  facetColumn.width
                                height: Theme.sp(22)

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin:  Theme.sp(18)
                                    anchors.rightMargin: Theme.sp(18)
                                    spacing: Theme.sp(8)

                                    Rectangle {
                                        Layout.preferredWidth:  Theme.sp(12)
                                        Layout.preferredHeight: Theme.sp(12)
                                        radius:       Theme.radius > 0 ? 2 : 0
                                        color:        facetOption.on ? Theme.colorAccent : "transparent"
                                        border.width: 1
                                        border.color: facetOption.on ? Theme.colorAccent
                                                                     : Theme.colorBorderStrong
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: facetOption.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color: facetOption.on ? Theme.colorText : Theme.colorText2
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: facetOption.modelData.count
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                    }
                                }

                                PpPressable {
                                    hoverScale: 1.0
                                    onClicked:  root.facetToggled(facetOption.facetKey,
                                                                  facetOption.modelData.value)
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
