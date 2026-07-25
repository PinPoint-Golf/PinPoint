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

// Detail page for one condition. Read-only in this pass; the sentence editor lands separately.
//
// Shows causes AND effects, because the same condition is routinely both — a page that showed only
// one direction would hide half the graph and make a chain look like a root.
Item {
    id: root

    property var    detail: ({})       // CharacteristicLibraryModel.detail()
    property string locale: "en"

    signal back()
    signal edit()
    signal openCondition(string conditionId)

    readonly property var _causes:   detail.causes   || []
    readonly property var _effects:  detail.effects  || []
    readonly property var _measures: detail.measures || []

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            // ── Back ─────────────────────────────────────────────────────────
            Text {
                text:           "← " + qsTr("All diagnostics")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          backMa.containsMouse ? Theme.colorText : Theme.colorText3

                MouseArea {
                    id: backMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    root.back()
                }
            }

            // ── Title ────────────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: root.detail.label || "" }
                Item { Layout.fillWidth: true }
                PpButton { label: qsTr("Edit"); onClicked: root.edit() }
            }

            RowLayout {
                spacing: Theme.sp(8)

                Repeater {
                    model: {
                        var tags = []
                        if (root.detail.groupLabel) tags.push(root.detail.groupLabel)
                        if (root.detail.reachLabel && root.detail.reach !== "measured")
                            tags.push(root.detail.reachLabel)
                        if (root.detail.proposed === true) tags.push(qsTr("Proposed"))
                        if (root.detail.resolvabilityLabel) tags.push(root.detail.resolvabilityLabel)
                        return tags
                    }
                    delegate: Rectangle {
                        required property var modelData
                        implicitWidth:  tagText.implicitWidth + Theme.sp(16)
                        implicitHeight: Theme.sp(22)
                        radius: height / 2
                        color:  Theme.colorBg2

                        Text {
                            id: tagText
                            anchors.centerIn: parent
                            text:           modelData
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                    }
                }
            }

            // ── What it costs the golfer ─────────────────────────────────────
            Text {
                Layout.fillWidth: true
                text:           root.detail.consequence || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody
                color:          Theme.colorText
                wrapMode:       Text.WordWrap
                visible:        text.length > 0
            }

            // ── Injury note — a separate axis from performance ───────────────
            Rectangle {
                Layout.fillWidth: true
                visible:        (root.detail.injuryNote || "").length > 0
                implicitHeight: injuryText.implicitHeight + Theme.sp(20)
                radius:         Theme.radius
                color:          Theme.colorBg2

                Text {
                    id: injuryText
                    x:              Theme.sp(12)
                    y:              Theme.sp(10)
                    width:          parent.width - Theme.sp(24)
                    text:           root.detail.injuryNote || ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    wrapMode:       Text.WordWrap
                }
            }

            // ── How it is detected ───────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)
                visible: root._measures.length > 0

                Text {
                    text:                qsTr("DETECTED FROM")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Repeater {
                    model: root._measures
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.sp(2)

                        Text {
                            Layout.fillWidth: true
                            text:           modelData.label || modelData.id
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            color:          Theme.colorText
                            wrapMode:       Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text: {
                                var bits = [modelData.statusLabel]
                                if (modelData.metricKey) bits.push(modelData.metricKey)
                                if (modelData.usedBy > 1)
                                    bits.push(qsTr("used by %n characteristics", "", modelData.usedBy))
                                return bits.join(" · ")
                            }
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }
                        // A capture gap says why, so nobody files it as pipeline work.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.gapReason || "").length > 0
                            text:           modelData.gapReason || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }
                    }
                }
            }

            // ── Causes and effects ───────────────────────────────────────────
            Repeater {
                model: [
                    { head: qsTr("USUALLY CAUSED BY"), rows: root._causes,
                      empty: qsTr("Nothing in the library explains this yet.") },
                    { head: qsTr("WHICH IN TURN CAUSES"), rows: root._effects, empty: "" }
                ]

                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.sp(8)
                    visible: modelData.rows.length > 0 || modelData.empty.length > 0

                    Text {
                        text:                modelData.head
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }

                    Text {
                        visible:        modelData.rows.length === 0
                        text:           modelData.empty
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorText3
                    }

                    Repeater {
                        model: modelData.rows
                        delegate: Item {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(38)

                            Rectangle {
                                anchors.fill: parent
                                radius: Theme.radius
                                color:  linkMa.containsMouse ? Theme.colorBg2 : "transparent"
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(10)
                                spacing: Theme.sp(8)

                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.label || modelData.id
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    // An Asserted cause is OFFERED, never concluded — it must look
                                    // different everywhere it appears.
                                    color:          modelData.offeredOnly === true ? Theme.colorText3
                                                                                   : Theme.colorText
                                    font.italic:    modelData.offeredOnly === true
                                    elide:          Text.ElideRight
                                }

                                // Strength as a WORD. It is a ranking weight, not a probability,
                                // and must never be rendered as a percentage.
                                Text {
                                    text:           modelData.strengthLabel || ""
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                Text {
                                    visible:        (modelData.reach || "measured") !== "measured"
                                    text:           modelData.reachLabel || ""
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            MouseArea {
                                id: linkMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked:    root.openCondition(modelData.id)
                            }
                        }
                    }
                }
            }

            // ── Provenance ───────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("PROVENANCE")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    text: (root.detail.citation && root.detail.citation.length > 0)
                          ? root.detail.citation
                          : qsTr("No citation. This entry states a direction and a phase reasoned "
                                 + "from mechanics, not taken from a source, and is badged Proposed "
                                 + "wherever it appears.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    visible:        (root.detail.screenRef || "").length > 0
                    text:           qsTr("Screen: %1").arg(root.detail.screenRef || "")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }
}
