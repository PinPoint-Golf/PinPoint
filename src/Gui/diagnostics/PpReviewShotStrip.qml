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
// IT IS A BAND, NOT A PAGE. The mock's strip is two rows of nine cells at the top of the panel,
// and the panel underneath it is the point of the panel. A real capture answers thirty-odd
// conditions, most of them with the same two words twice — "not measurable" beside "clean all
// session" — and drawn in pack order they take five rows and four fifths of a 560 px frame to
// say that nothing happened. So the model SORTS by information (fired here, clean here, silent
// here but a pattern or a watch this session) and MARKS the silent tail, this file bounds the
// grid to the mock's two rows and scrolls inside it, and the tail collapses behind one dashed
// line that expands IN PLACE into the same cells (§8: nothing withheld, one tap away). Nothing
// is dropped and nothing is summarised away — the tail's line names both silences, and opening
// it is one press.
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
    // The mock's band, in rows. It is the strip's OWN ceiling rather than the panel's: a strip
    // sized as a fraction of the panel grows with the panel and with the measurable set, and
    // this one is meant to stay a band whatever is behind it.
    property int maxRows: 2

    // The tail — silent here AND silent all session — folded away by default and expanded in
    // place. Panel-local, and the only state this file owns; see brief §8, which allows exactly
    // two collapsed regions and requires both to open into the same content rather than a
    // different view.
    property bool tailExpanded: false

    objectName: "sdReviewStrip"

    // A NEW SHOT IS A NEW QUESTION. The tail is opened against one swing's read; carrying it
    // over to the next shot picked off the carousel would hand the reader a strip they never
    // asked to expand, sized for a set they have not looked at yet.
    onReadoutChanged: root.tailExpanded = false

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2 * fit))

    readonly property var allConditions: (readout && readout.conditions) ? readout.conditions : []
    // The model publishes the whole measurable set, in information order, with the tail marked.
    // Which of it is on screen is this file's decision and the ONLY one it makes about the
    // content: no reordering, no filtering by tier, no cadence.
    readonly property var conditions:
        (tailExpanded || tailCount === 0)
        ? allConditions
        : allConditions.filter(function (c) { return c.tail !== true })

    readonly property int tailCount: (readout && readout.tailCount) ? readout.tailCount : 0
    readonly property string tailSummary: readout ? (readout.tailSummary || "") : ""

    // The design's cell, at k = 1. Wide enough for a condition name beside its mark, and for
    // a reading beside the corridor it was tested against — the two facts the cell exists to
    // put next to each other.
    readonly property int baseCellW: 220
    readonly property int _gap: px(4)
    readonly property int _cellW: root.compact
                                  ? Math.max(0, frame.width - 2 * px(10))
                                  : Math.min(px(baseCellW), Math.max(0, frame.width - 2 * px(10)))

    // One cell's height, computed rather than measured off a delegate: the grid's ceiling has
    // to be known before the Flow has laid anything out, or the strip resizes itself on the
    // frame after it appears. These are the two lines inside a cell and the padding around
    // them, and the cell delegate below builds itself out of the same three numbers.
    TextMetrics {
        id: mName
        font.family: Theme.fontBody; font.pixelSize: root.tzLabel
        font.weight: Theme.fontBodyWeight
        text: "Ag"
    }
    TextMetrics {
        id: mValue
        font.family: Theme.fontData; font.pixelSize: root.tzMicro
        text: "Ag"
    }
    readonly property int _cellH:
        Math.round(mName.height + px(2) + mValue.height + 2 * px(5))
    // THE BAND. Two rows at k = 1, and a grid taller than that scrolls rather than pushing the
    // rail off the panel — which is the failure this bound exists to prevent, not a nicety.
    //
    // Opening the tail lifts the ceiling as well as adding the cells, because an expansion that
    // only lengthened a scroll region would look, at the moment of the press, like nothing
    // happened. It lifts it to a bound and not to the content: the strip is still a band, the
    // panel's own `maxHeight` is still above it, and the rest is still one flick away.
    function _rowsH(n) { return Math.max(_cellH, n * _cellH + (n - 1) * _gap) }
    readonly property int _gridMax: _rowsH(tailExpanded ? 2 * maxRows : maxRows)
    readonly property int _gridH: Math.min(flow.implicitHeight, _gridMax)

    readonly property int _tailH: tailRow.visible ? tailRow.implicitHeight + px(5) : 0
    readonly property int _wanted:
        2 * px(8) + headRow.implicitHeight + px(6) + _gridH + _tailH
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
            anchors.bottom: tailRow.visible ? tailRow.top : parent.bottom
            anchors.leftMargin:   root.px(10)
            anchors.rightMargin:  root.px(10)
            anchors.topMargin:    root.px(6)
            anchors.bottomMargin: tailRow.visible ? root.px(5) : root.px(8)
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

        // ── the silent tail, one line, opening in place ──────────────────────
        // DASHED, for the same reason the not-assessable cells are: what is behind this line is
        // a capture limit and a session-long silence, not a set of clean reads. It states both
        // in the model's own words and it counts them, so a reader who never opens it still
        // knows exactly what they are not looking at — which is the difference between folding
        // and hiding.
        Item {
            id: tailRow
            objectName: "sdReviewTailRow"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin:   root.px(10)
            anchors.rightMargin:  root.px(10)
            anchors.bottomMargin: root.px(8)
            visible: root.tailCount > 0
            implicitHeight: tailText.implicitHeight + 2 * root.px(4)
            height: implicitHeight

            PpDashedFrame {
                anchors.fill: parent
                frameRadius: Math.max(1, root.px(4))
                strokeColor: Theme.colorBorderMid
                dashOn:  Math.max(1, root.px(3))
                dashOff: Math.max(1, root.px(3))
            }

            Text {
                id: tailText
                objectName: "sdReviewTailSummary"
                anchors.left: parent.left
                anchors.right: tailToggle.left
                anchors.leftMargin:  root.px(8)
                anchors.rightMargin: root.px(8)
                anchors.verticalCenter: parent.verticalCenter
                text: root.tailSummary
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
            Text {
                id: tailToggle
                objectName: "sdReviewTailToggle"
                anchors.right: parent.right
                anchors.rightMargin: root.px(8)
                anchors.verticalCenter: parent.verticalCenter
                text: root.tailExpanded ? qsTr("HIDE ▾") : qsTr("SHOW ▸")
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingLabel
                color: Theme.colorAccent
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: root.tailExpanded = !root.tailExpanded
            }
        }
    }
}
