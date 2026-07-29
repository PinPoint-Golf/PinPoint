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

// What the library needs and does not yet have, ranked by how much each unblocks.
//
// The reader has to be able to take this list at face value as a WORK QUEUE. That is why capture
// gaps sit in their own section below rather than as rows here: one entry nobody could ever pick up
// would quietly change how every other row reads.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    readonly property var _rows: root.library.roadmap()
    readonly property var _gaps: root.library.captureGaps()

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(12)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            Text {
                text:                qsTr("WHAT THIS LIBRARY NEEDS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: qsTr("Measure roadmap") }
                Item { Layout.fillWidth: true }

                PpButton {
                    label: qsTr("Export")
                    onClicked: {
                        var r = root.library.exportRoadmap()
                        // The message is built in C++ and already names the path, so it is not
                        // re-composed here — one tr() string, in one place, for both outcomes.
                        exportToast.severity = r.ok ? "info" : "error"
                        exportToast.copyText = r.ok ? r.path : ""   // empty hides the copy action
                        exportToast.show(r.message)
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Every row is work that could be picked up: a measure some characteristic "
                           + "needs and nothing yet produces. Ranked by how many characteristics it "
                           + "unblocks, because one producer often serves several.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            // ── The queue ─────────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible:        root._rows.length === 0
                text:           qsTr("Nothing outstanding — every measure has a producer.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText3
            }

            Repeater {
                model: root._rows
                delegate: Rectangle {
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: Theme.sp(52)
                    radius: Theme.radius
                    color:  rowMa.containsMouse ? Theme.colorBg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin:  Theme.sp(12)
                        anchors.rightMargin: Theme.sp(12)
                        spacing: Theme.sp(12)

                        // How much it unblocks — the whole basis of the ranking, so it leads.
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            implicitWidth:  Theme.sp(28)
                            implicitHeight: Theme.sp(28)
                            radius: width / 2
                            color:  Theme.colorBg2

                            Text {
                                anchors.centerIn: parent
                                text:           modelData.blocks
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(2)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                                elide:          Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: {
                                    var bits = [modelData.statusLabel]
                                    if (modelData.metricKey) bits.push(modelData.metricKey)
                                    if (modelData.viewNeeded === "faceOn")
                                        bits.push(qsTr("face-on"))
                                    else if (modelData.viewNeeded === "downTheLine")
                                        bits.push(qsTr("down-the-line"))
                                    return bits.join(" · ")
                                }
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                elide:          Text.ElideRight
                            }
                        }

                        Text {
                            text:           qsTr("unblocks %n", "", modelData.blocks)
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }

                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                    }
                }
            }

            // ── Capture gaps — deliberately NOT roadmap rows ───────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)
                visible: root._gaps.length > 0

                PpDivider { Layout.fillWidth: true }

                Text {
                    text:                qsTr("NOT RESOLVABLE FROM CURRENT CAPTURE")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("These are not roadmap items and no amount of pipeline work will "
                               + "close them. They need a different sensor or a different view.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }

                Repeater {
                    model: root._gaps
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(2)

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.sp(8)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText
                                elide:          Text.ElideRight
                            }
                            Text {
                                text:           qsTr("blocks %n", "", modelData.blocks)
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            text:           modelData.reason
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                            bottomPadding:  Theme.sp(6)
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }

    // Same shape as the References export and as the log export in ScreenResourceMonitor: no UNDO
    // (writing a file is not undoable), and the path offered to the clipboard because the next thing
    // anyone does with an exported file is paste its location somewhere.
    PpToast {
        id: exportToast
        showUndo: false
        glyph:    "⤓"
        z:        10
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom:           parent.bottom
        anchors.bottomMargin:     Theme.sp(24)
    }
}
