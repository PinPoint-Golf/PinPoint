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

// The reviewed shot, read INSIDE the finished ledger (design 13a, brief §6).
//
// IT REPLACES THE AFTER-SHOT STRIP AND THE BOOKENDS, AND IT SHOWS EVERY CONDITION. The live
// strip reports a moment and is subject to cadence; this one reports a shot the golfer went
// looking for, so nothing is withheld and no cadence gate is consulted — a reader who picked
// shot 9 is owed all nine reads, not the two the panel would have volunteered at the time.
// That is why the cells WRAP rather than being counted-and-truncated the way the pattern card
// row is: a condition the reader asked about is not allowed to be a "+3 more".
//
// THE POPULATION IS ONE. The headline counts fired against fired+clean ("7 of 9 conditions
// fired on this swing") and the not-assessable measures are stated SEPARATELY underneath,
// because folding them into the denominator would quietly turn "we did not look" into
// "it was fine".
//
// NEITHER SLOT IS EVER BLANK ON A NOT-ASSESSABLE CELL. The value reads "not measurable" and
// the corridor slot carries THE REASON — "shaft occluded at P6", "ball not tracked". A blank
// there reads as a bug in the app rather than as a limit of the capture, and the dashed border
// is the same distinction drawn a second way. Both strings come from the model; this file
// positions and paints.

import QtQuick
import PinPointStudio

Item {
    id: root

    // SessionDiagnosticsModel::shotReadout(shotId) — { headline, note, conditions: [...] }.
    // Null, or a map with no conditions, means no shot is selected and this strip is not the
    // one on screen (see PpSessionDiagnosticsBody).
    property var readout: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // Below the wide arrangement's floor the right-hand caption goes, exactly as the live
    // strip drops its own — and the cells stack one per row rather than shrinking.
    property bool compact: false
    // The most vertical space the panel can spare. The grid SCROLLS past it rather than
    // clipping cells away: at 1168 the nine cells are two rows and fit outright, at 396 they
    // stack and do not. 0 = take whatever is wanted.
    property real maxHeight: 0

    objectName: "sdReviewStrip"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2 * fit))

    readonly property var conditions: (readout && readout.conditions) ? readout.conditions : []

    // The design's cell, at k = 1. Wide enough for a condition name beside its mark, and for
    // a reading beside the corridor it was tested against — the two facts the cell exists to
    // put next to each other.
    readonly property int baseCellW: 220
    readonly property int _gap: px(4)
    readonly property int _cellW: root.compact
                                  ? Math.max(0, frame.width - 2 * px(10))
                                  : Math.min(px(baseCellW), Math.max(0, frame.width - 2 * px(10)))

    readonly property int _wanted: 2 * px(8) + headRow.implicitHeight + px(6) + flow.implicitHeight
    implicitHeight: maxHeight > 0 ? Math.min(_wanted, maxHeight) : _wanted

    Rectangle {
        id: frame
        anchors.fill: parent
        color: Theme.colorSurface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.colorBorderMid
        clip: true

        // ── the one-population headline ──────────────────────────────────────
        Item {
            id: headRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin:  root.px(10)
            anchors.rightMargin: root.px(10)
            anchors.topMargin:   root.px(8)
            height: headline.implicitHeight

            Text {
                id: stripLabel
                objectName: "sdReviewStripLabel"
                anchors.left: parent.left
                anchors.baseline: headline.baseline
                text: qsTr("THIS SHOT")
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                font.letterSpacing: Theme.trackingMicro
                // Accent, not the live strip's grey: the tense has changed and the header
                // badge says so in the same colour.
                color: Theme.colorAccent
            }
            Text {
                id: headline
                objectName: "sdReviewHeadline"
                anchors.left: stripLabel.right
                anchors.leftMargin: root.px(10)
                anchors.top: parent.top
                text: root.readout ? (root.readout.headline || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontBody
                font.pixelSize: root.tzBody
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }
            Text {
                id: subline
                objectName: "sdReviewSubline"
                anchors.left: headline.right
                anchors.leftMargin: root.px(10)
                anchors.right: cellHint.visible ? cellHint.left : parent.right
                anchors.rightMargin: root.px(9)
                anchors.baseline: headline.baseline
                visible: text !== ""
                text: root.readout ? (root.readout.note || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                color: Theme.colorText2
            }
            Text {
                id: cellHint
                anchors.right: parent.right
                anchors.baseline: headline.baseline
                visible: !root.compact
                text: qsTr("every condition shown · IN / OUT is this swing against its corridor")
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }

        // ── every condition, wrapping ────────────────────────────────────────
        // Clipped and flickable so that a narrow arrangement (or a session with more than the
        // usual handful of measurable conditions) SCROLLS. Dropping a cell here would be the
        // one thing this strip exists to not do.
        Flickable {
            id: grid
            objectName: "sdReviewGrid"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: headRow.bottom
            anchors.bottom: parent.bottom
            anchors.leftMargin:   root.px(10)
            anchors.rightMargin:  root.px(10)
            anchors.topMargin:    root.px(6)
            anchors.bottomMargin: root.px(8)
            contentWidth: width
            contentHeight: flow.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Flow {
                id: flow
                width: grid.width
                spacing: root._gap

                Repeater {
                    model: root.conditions

                    Item {
                        id: cell
                        required property var modelData

                        objectName: "sdReviewCell"

                        readonly property bool fired: modelData.stateKind === "fired"
                        readonly property bool clean: modelData.stateKind === "clean"
                        readonly property bool notAssessable: !fired && !clean

                        readonly property color stateColor: fired ? Theme.colorError
                                                          : clean ? Theme.colorGood
                                                                  : Theme.colorText3
                        // pattern / watching / clean all session — the SESSION tier, beside
                        // this shot's read, which is the whole reason the cell is worth
                        // drawing in review: was this swing typical of the session or not.
                        readonly property color tierColor:
                            modelData.tierTag === "pattern"  ? Theme.colorError
                          : modelData.tierTag === "watching" ? Theme.colorAttention
                                                             : Theme.colorText3

                        width:  root._cellW
                        height: cellCol.implicitHeight + 2 * root.px(5)

                        Rectangle {
                            objectName: "sdReviewCellFill"
                            anchors.fill: parent
                            radius: Math.max(1, root.px(4))
                            // Fired fills at the error token's own Light (~10%) alpha and is
                            // framed at ~35% of the same hue; clean is framed only. Derived
                            // from the tokens, so every aesthetic follows.
                            color: cell.fired ? Theme.colorErrorLight : "transparent"
                            border.width: cell.notAssessable ? 0 : 1
                            border.color: cell.fired
                                ? Qt.rgba(Theme.colorError.r, Theme.colorError.g,
                                          Theme.colorError.b, 0.35)
                                : Qt.rgba(Theme.colorGood.r, Theme.colorGood.g,
                                          Theme.colorGood.b, 0.28)
                        }

                        // The dash is the honesty device, not decoration: a measure this
                        // capture could not answer is not a clean one drawn faintly.
                        PpDashedFrame {
                            objectName: "sdReviewCellDash"
                            anchors.fill: parent
                            visible: cell.notAssessable
                            frameRadius: Math.max(1, root.px(4))
                            strokeColor: Theme.colorBorderMid
                            dashOn:  Math.max(1, root.px(3))
                            dashOff: Math.max(1, root.px(3))
                        }

                        Column {
                            id: cellCol
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin:  root.px(8)
                            anchors.rightMargin: root.px(8)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: root.px(2)

                            Item {
                                width: parent.width
                                height: cellName.implicitHeight

                                Text {
                                    id: cellName
                                    objectName: "sdReviewCellName"
                                    anchors.left: parent.left
                                    anchors.right: cellMark.left
                                    anchors.rightMargin: root.px(6)
                                    anchors.top: parent.top
                                    text: cell.modelData.name || ""
                                    elide: Text.ElideRight
                                    font.family: Theme.fontBody
                                    font.pixelSize: root.tzLabel
                                    font.weight: Theme.fontBodyWeight
                                    color: Theme.colorText
                                }
                                Text {
                                    id: cellMark
                                    objectName: "sdReviewCellMark"
                                    anchors.right: parent.right
                                    anchors.baseline: cellName.baseline
                                    // OUT / IN / — . The em dash is the third state and it is
                                    // not a blank: "we did not look" is a fact the golfer is
                                    // owed, in the same slot as the other two answers.
                                    text: cell.modelData.state || ""
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    font.letterSpacing: Theme.trackingLabel
                                    color: cell.stateColor
                                }
                            }

                            Item {
                                width: parent.width
                                height: cellValue.implicitHeight

                                Text {
                                    id: cellValue
                                    objectName: "sdReviewCellValue"
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    // The model formats the number, including the true minus
                                    // (U+2212) that makes "−4.4°" line up with the corridor
                                    // beside it. Nothing here reformats it.
                                    text: cell.modelData.valueText || ""
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzMicro
                                    color: cell.stateColor
                                }
                                Text {
                                    id: cellBand
                                    objectName: "sdReviewCellCorridor"
                                    anchors.left: cellValue.right
                                    anchors.leftMargin: root.px(6)
                                    anchors.right: cellTier.left
                                    anchors.rightMargin: root.px(6)
                                    anchors.baseline: cellValue.baseline
                                    // The corridor it was tested against — or, on a cell the
                                    // capture could not answer, WHY. Same slot either way.
                                    text: cell.modelData.corridorText || ""
                                    elide: Text.ElideRight
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    color: Theme.colorText3
                                }
                                Text {
                                    id: cellTier
                                    objectName: "sdReviewCellTier"
                                    anchors.right: parent.right
                                    anchors.baseline: cellValue.baseline
                                    text: cell.modelData.tierTag || ""
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    color: cell.tierColor
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
