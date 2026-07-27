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

// Detail page for one condition.
//
// Causes and effects are ONE graph, not two lists. They used to be rendered as a pair of stacked
// lists here, which said how many of each there were and nothing about the shape they sat in — and
// following a chain back to its root meant a page load per step. DagView replaces both: it is a
// navigation surface, and it is where the same-condition-is-both-a-cause-and-an-effect structure
// this pack is built on finally shows.
Item {
    id: root

    property var    detail: ({})       // CharacteristicLibraryModel.detail()
    property string locale: "en"

    // The graph asks the model for its own layout, so the detail map is not enough.
    required property var library      // CharacteristicLibraryModel
    property var          editor: null // CharacteristicEditorModel — omit for a read-only graph

    signal back()
    signal edit()
    signal openCondition(string conditionId)
    signal openMeasure(string measureId)
    signal graphChanged()

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

            // ── The other names for it ───────────────────────────────────────
            // Above the consequence, not below: a reader who arrived by searching a coach term needs
            // to see their own word confirmed before they read anything else, or they cannot tell
            // whether they landed on the right page.
            Text {
                Layout.fillWidth: true
                text: (root.detail.aliases && root.detail.aliases.length > 0)
                      ? qsTr("Also called ") + root.detail.aliases.join(qsTr(", "))
                      : ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                font.italic:    true
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                visible:        text.length > 0
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

            // ── Where it applies ─────────────────────────────────────────────
            //
            // Shown ONLY when somebody has narrowed it. A characteristic that applies everywhere —
            // which is all 50 shipped ones — says nothing here, because a line reading "applies to
            // every kind of shot" on every page would train the reader to skip the one page where
            // it does not.
            Text {
                Layout.fillWidth: true
                visible:        (root.detail.appliesSummary || "").length > 0
                text:           root.detail.appliesSummary || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
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
                        // WHICH SIDE FIRES. The first thing a reader has to check before deciding
                        // a characteristic is right, and until now it could only be seen by opening
                        // the editor. Composed in C++ by the same rule as the control that sets it.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.directionSentence || "").length > 0
                            text:           modelData.directionSentence || ""
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText2
                            wrapMode:       Text.WordWrap
                        }
                        // No stated convention means nothing can check that tail is the right one.
                        Text {
                            Layout.fillWidth: true
                            visible:        (modelData.direction || "").length > 0
                                            && (modelData.highMeans || "").length === 0
                            text: qsTr("Nothing says what a higher value of this measure means, so "
                                       + "which side fires cannot be checked.")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorRagWatch
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

            // ── How it connects ──────────────────────────────────────────────
            // The whole causes/effects picture, walkable. Replaces the two lists that used to sit
            // here; every coordinate in it comes from dag_layout.cpp.
            DagView {
                Layout.fillWidth: true
                library: root.library
                editor:  root.editor
                rootId:  root.detail.id || ""

                onOpenCondition: function (conditionId) { root.openCondition(conditionId) }
                onOpenMeasure:   function (measureId)   { root.openMeasure(measureId) }
                onGraphChanged:  root.graphChanged()
            }

            // ── Relations that are not causal ────────────────────────────────
            //
            // Below the graph rather than in it. The DAG ranks by signed causal distance, and a
            // symmetric relation has no direction to rank by — drawing one in that flow would state
            // a claim the pack does not hold. These sit beneath it as what they are: two readings of
            // one event, and pairs that cannot both be true of one swing.
            Repeater {
                model: [{ rows: root.detail.corroboratedBy || [],
                          head: qsTr("ALSO SEEN AS"),
                          hint: qsTr("The same event, read another way. Independent confirmation — no causal claim either direction.") },
                        { rows: root.detail.excludes || [],
                          head: qsTr("CANNOT ALSO BE"),
                          hint: qsTr("One swing cannot be both. If both fire, the explanation keeps the more confident reading and says which it dropped.") }]

                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.sp(6)
                    visible: modelData.rows.length > 0

                    Text {
                        text:                modelData.head
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                    }

                    Repeater {
                        model: modelData.rows
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            text:           modelData.label + " · " + modelData.groupLabel
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color:          Theme.colorAccent
                            elide:          Text.ElideRight

                            MouseArea {
                                anchors.fill: parent
                                cursorShape:  Qt.PointingHandCursor
                                onClicked:    root.openCondition(parent.modelData.id)
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:           modelData.hint
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:       Text.WordWrap
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
