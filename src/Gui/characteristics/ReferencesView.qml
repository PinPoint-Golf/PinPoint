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

// The bibliography, and what each paper actually holds up.
//
// A reference list on its own is an appendix nobody opens. The question a reader has when they
// arrive here is "why does the app believe this?", so every entry pairs the paper with the causal
// claims resting on it, at the tier each one earned — and the list is ordered by how much of the
// library each carries, because that ordering IS the argument.
//
// Papers nothing cites are KEPT and marked. One of them contradicts two claims the pack does make,
// and a bibliography that quietly dropped it would be the most misleading version of this view we
// could ship.
Item {
    id: root

    required property var library     // CharacteristicLibraryModel

    signal openCondition(string id)

    property int _revision: 0
    readonly property var _refs: (root._revision >= 0) ? root.library.references() : []

    // healthChanged, not libraryChanged: `libraryChanged` belongs to CharacteristicLibrary.qml,
    // not to the model, and Connections cannot tell an unmatched handler from a typo at compile
    // time — it warns at INSTANTIATION, which for a lazily-loaded settings panel means the first
    // time somebody opens it. `refresh()` emits healthChanged, and both sibling views key off it.
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

    // Provenance, not verdict. Muted on purpose — a bright chip here would out-shout the claim it
    // is qualifying, and the tier is a caveat rather than a score.
    function _tierColor(tier) {
        switch (tier) {
        case "established":   return Theme.colorGood
        case "supported":     return Theme.colorGood
        case "indirect":      return Theme.colorAccent
        case "noSourceFound": return Theme.colorWarn
        }
        return Theme.colorText3          // practice, proposed
    }

    ScrollView {
        anchors.fill:    parent
        anchors.margins: Theme.sp(20)
        clip:            true

        ColumnLayout {
            width:   parent.width
            spacing: Theme.sp(16)

            SectionHead { text: qsTr("REFERENCES") }

            Text {
                Layout.fillWidth: true
                text: qsTr("Every source behind a citation in the library, with the claims resting "
                           + "on it and the tier each one earned. Ordered by how much each paper "
                           + "holds up. Tap a paper to open it at doi.org; tap a claim to go to it. "
                           + "Most of the library is coaching practice rather than published "
                           + "measurement — where that is so, no source is listed and the claim "
                           + "says as much.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            Repeater {
                model: root._refs

                delegate: ColumnLayout {
                    id: rrow
                    required property var modelData

                    Layout.fillWidth: true
                    spacing:          Theme.sp(4)

                    // ── The paper ──────────────────────────────────────────────
                    RowLayout {
                        Layout.fillWidth: true
                        spacing:          Theme.sp(8)

                        Text {
                            Layout.fillWidth: true
                            text:             rrow.modelData.title || rrow.modelData.doi
                            wrapMode:         Text.WordWrap
                            font.family:      Theme.fontBody
                            font.pixelSize:   Theme.fontSzBody
                            color:            paperMa.containsMouse ? Theme.colorAccent
                                                                    : Theme.colorText
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        // The count leads, because it is what the ordering is on: a reader
                        // scanning for what matters most should not have to count rows.
                        Text {
                            text: rrow.modelData.citeCount > 0
                                  ? qsTr("supports %1").arg(rrow.modelData.citeCount)
                                  : qsTr("cited by nothing")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            font.italic:    rrow.modelData.citeCount === 0
                            color:          Theme.colorText3
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        wrapMode:         Text.WordWrap
                        font.family:      Theme.fontBody
                        font.pixelSize:   Theme.fontSzMicro
                        color:            Theme.colorText2
                        text: {
                            var bits = []
                            if (rrow.modelData.authors) bits.push(rrow.modelData.authors)
                            if (rrow.modelData.journal) bits.push(rrow.modelData.journal)
                            if (rrow.modelData.year)    bits.push(String(rrow.modelData.year))
                            return bits.join("  ·  ")
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text:             rrow.modelData.doi
                        wrapMode:         Text.WrapAnywhere
                        font.family:      Theme.fontData
                        font.pixelSize:   Theme.fontSzMicro
                        color:            paperMa.containsMouse ? Theme.colorAccent
                                                                : Theme.colorText3
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                        MouseArea {
                            id:           paperMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked: if (rrow.modelData.url) Qt.openUrlExternally(rrow.modelData.url)
                        }
                    }

                    // What it establishes, in our words — the one thing a DOI cannot say, and what
                    // lets a reader tell an indirect citation from a supported one without opening
                    // the paper.
                    Text {
                        visible:          (rrow.modelData.establishes || "").length > 0
                        Layout.fillWidth: true
                        Layout.topMargin: Theme.sp(2)
                        text:             rrow.modelData.establishes || ""
                        wrapMode:         Text.WordWrap
                        font.family:      Theme.fontBody
                        font.pixelSize:   Theme.fontSzMicro
                        color:            Theme.colorText2
                    }

                    // ── What rests on it ───────────────────────────────────────
                    Repeater {
                        model: rrow.modelData.cites || []

                        delegate: RowLayout {
                            id: crow
                            required property var modelData

                            Layout.fillWidth: true
                            Layout.leftMargin: Theme.sp(12)
                            spacing:           Theme.sp(8)

                            Rectangle {
                                implicitWidth:  tierTxt.implicitWidth + Theme.sp(12)
                                implicitHeight: Theme.sp(16)
                                radius:         height / 2
                                color:          "transparent"
                                border.width:   1
                                border.color:   root._tierColor(crow.modelData.tier)

                                Text {
                                    id:               tierTxt
                                    anchors.centerIn: parent
                                    text:             crow.modelData.tierLabel || ""
                                    font.family:      Theme.fontBody
                                    font.pixelSize:   Theme.fontSzMicro
                                    color:            root._tierColor(crow.modelData.tier)
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                wrapMode:         Text.WordWrap
                                font.family:      Theme.fontBody
                                font.pixelSize:   Theme.fontSzMicro
                                color:            claimMa.containsMouse ? Theme.colorAccent
                                                                        : Theme.colorText2
                                text: crow.modelData.kind === "edge"
                                      ? crow.modelData.from + "  →  " + crow.modelData.to
                                      : crow.modelData.from
                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                MouseArea {
                                    id:           claimMa
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape:  Qt.PointingHandCursor
                                    // The cause end of an edge, the condition itself otherwise —
                                    // either way it opens something real.
                                    onClicked: root.openCondition(crow.modelData.fromId)
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth:  true
                        Layout.topMargin:  Theme.sp(8)
                        implicitHeight:    1
                        color:             Theme.colorBorderMid
                    }
                }
            }
        }
    }
}
