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

// The two reference registries: the physical screens a characteristic can be settled by, and the
// drills that answer one.
//
// Ordered by how much each one SETTLES, not alphabetically. That order is the argument the whole
// model makes: a handful of physical tests, none of which needs any capture hardware, explain most
// of what the library can detect. A list sorted by name would bury that.
//
// A screen with nothing pointing at it is still listed. The library is being written, and a row
// nobody uses yet is not a row that is wrong — the count says which is which.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    signal openCondition(string conditionId)

    property int _revision: 0
    readonly property var _screens: (root._revision >= 0) ? root.library.screens() : []
    readonly property var _drills:  (root._revision >= 0) ? root.library.drills()  : []

    Connections {
        target: root.library
        function onHealthChanged() { root._revision++ }
    }

    component SectionHead : Text {
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    ScrollView {
        anchors.fill: parent
        anchors.margins: Theme.sp(20)
        clip: true

        ColumnLayout {
            width: parent.width
            spacing: Theme.sp(16)

            SectionHead { text: qsTr("PHYSICAL SCREENS") }

            Text {
                Layout.fillWidth: true
                text: qsTr("What a characteristic marked Physical can be settled by. None of these "
                           + "needs any capture hardware, which is why a handful of them explain "
                           + "most of what the library detects. A screen says what the body can do "
                           + "on a table; whether that explains what the swing did is the "
                           + "explanation's job, and whether it needs treating is a clinician's.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Repeater {
                model: root._screens

                delegate: ColumnLayout {
                    id: srow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.sp(4)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(8)

                        Text {
                            text:           srow.modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText
                        }
                        Text {
                            text:           srow.modelData.bodyRegion || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: srow.modelData.settlesCount > 0
                                  ? qsTr("would settle %1").arg(srow.modelData.settlesCount)
                                  : qsTr("nothing points at it yet")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:           srow.modelData.protocol || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText2
                        wrapMode:       Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        // The words always; the number only when one exists. Several screens are
                        // qualitative on purpose, and printing "0 °" for those would be a lie the
                        // reader could not see through.
                        text: qsTr("Passes: ") + (srow.modelData.passCriterion || "")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText2
                        wrapMode:       Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible:        (srow.modelData.note || "").length > 0
                        text:           srow.modelData.note || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.italic:    true
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: Theme.sp(6)
                        visible: (srow.modelData.settles || []).length > 0

                        Repeater {
                            model: srow.modelData.settles || []
                            delegate: Text {
                                required property var modelData
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorAccent

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root.openCondition(parent.modelData.id)
                                }
                            }
                        }
                    }
                }
            }

            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.colorBorderMid }

            SectionHead { text: qsTr("DRILLS") }

            Text {
                Layout.fillWidth: true
                text: qsTr("What a golfer does about a characteristic. Each says what to do and "
                           + "what it is trying to change — as intent, never as a promise: nothing "
                           + "here has measured an effect on anybody.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Repeater {
                model: root._drills

                delegate: ColumnLayout {
                    id: drow
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.sp(4)

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.sp(8)
                        Text {
                            text:           drow.modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorText
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: drow.modelData.answersCount > 0
                                  ? qsTr("answers %1").arg(drow.modelData.answersCount)
                                  : qsTr("not attached to anything yet")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:           drow.modelData.instruction || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody
                        color:          Theme.colorText2
                        wrapMode:       Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text:           drow.modelData.targets || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible:        (drow.modelData.note || "").length > 0
                        text:           drow.modelData.note || ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.italic:    true
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
                    }
                }
            }
        }
    }
}
