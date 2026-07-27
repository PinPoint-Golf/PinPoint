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
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// The glossary. Every characteristic, the words coaches actually use for it, what it means in plain
// language, and what commonly causes it.
//
// There is NO glossary dataset. Every line here is the rule set read out: the label and aliases from
// the condition, the meaning from its `consequence` (the field that already had to answer "what does
// this cost the golfer"), and "commonly caused by" straight off the causal edges. That is what makes
// it free to maintain — an entry cannot go stale relative to the library, because it IS the library.
//
// Alphabetical rather than grouped, because a glossary is consulted rather than browsed: the common
// use is "what does X mean", and grouping would make that the slow path.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    signal openCondition(string conditionId)

    property string _search: ""

    // Re-read when the library changes underneath — an alias edited in the editor must show here.
    property int _revision: 0
    readonly property var _entries: (root._revision >= 0) ? root.library.glossary(root._search) : []

    Connections {
        target: root.library
        function onHealthChanged() { root._revision++ }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.sp(20)
        spacing: Theme.sp(14)

        PpTextField {
            Layout.fillWidth: true
            placeholderText: qsTr("Search a term — try “flip”, “OTT”, “standing up”")
            onTextChanged: root._search = text
        }

        Text {
            Layout.fillWidth: true
            text: root._entries.length === 0
                  ? qsTr("Nothing matches that term.")
                  : qsTr("%n entr(y)(ies)", "", root._entries.length)
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }

        ScrollView {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            clip: true

            ColumnLayout {
                width: parent.width
                spacing: Theme.sp(18)

                Repeater {
                    model: root._entries

                    delegate: ColumnLayout {
                        id: entry
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(4)

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(8)

                            Text {
                                text:           entry.modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody
                                color:          Theme.colorAccent

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root.openCondition(entry.modelData.id)
                                }
                            }

                            Text {
                                text:           entry.modelData.groupLabel
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }

                            // Uncited content must be badged wherever it appears, and a glossary is
                            // exactly where somebody would otherwise read it as settled fact.
                            Text {
                                visible:        entry.modelData.proposed === true
                                text:           qsTr("proposed")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }

                            Item { Layout.fillWidth: true }
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: (entry.modelData.aliases || []).length > 0
                            text: qsTr("also called ") + (entry.modelData.aliases || []).join(", ")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            font.italic:    true
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }

                        Text {
                            Layout.fillWidth: true
                            text:           entry.modelData.meaning || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText2
                            wrapMode:       Text.WordWrap
                        }

                        // Straight off the causal edges — tappable, so the glossary is a way INTO
                        // the graph rather than a dead end beside it.
                        Flow {
                            Layout.fillWidth: true
                            spacing: Theme.sp(6)
                            visible: (entry.modelData.causedBy || []).length > 0

                            Text {
                                text:           qsTr("commonly caused by")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }

                            Repeater {
                                model: entry.modelData.causedBy || []
                                delegate: Text {
                                    required property var modelData
                                    text:           modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorAccent

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape:  Qt.PointingHandCursor
                                        // A handler in a Repeater delegate can only see the
                                        // component root, so it goes through `entry` rather than
                                        // reaching for a file-level id.
                                        onClicked:    root.openCondition(parent.modelData.id)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
