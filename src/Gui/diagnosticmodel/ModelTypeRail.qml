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

    signal typePicked(string key)
    signal facetToggled(string key, string value)
    signal facetsCleared()

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.sp(18)
            Layout.topMargin:  Theme.sp(12)
            Layout.bottomMargin: Theme.sp(6)
            text:                qsTr("CONTENT")
            font.family:         Theme.fontBody
            font.pixelSize:      Theme.fontSzMicro
            font.weight:         Theme.fontBodyWeight
            font.letterSpacing:  Theme.trackingMicro
            font.capitalization: Font.AllUppercase
            color:               Theme.colorText3
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

        Item { Layout.fillHeight: true }

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

            Text {
                Layout.fillWidth:    true
                text:                qsTr("FILTERS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.weight:         Theme.fontBodyWeight
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            Text {
                text:    qsTr("clear")
                visible: Object.keys(root.activeFacets).length > 0
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorAccent
                PpPressable { hoverScale: 1.0; onClicked: root.facetsCleared() }
            }
        }

        // Scrolled, because a facet list over a nine-value vocabulary is taller than the rail once
        // three of them are stacked.
        ScrollView {
            Layout.fillWidth:  true
            Layout.maximumHeight: Theme.sp(300)
            Layout.bottomMargin:  Theme.sp(12)
            Layout.preferredHeight: facetColumn.implicitHeight
            clip: true
            visible: root.facets.length > 0

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
