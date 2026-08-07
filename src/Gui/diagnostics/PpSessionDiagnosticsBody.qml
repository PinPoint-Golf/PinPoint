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

// EVERYTHING THE SESSION DIAGNOSTICS PANEL LOOKS LIKE. Chrome, header, the after-shot
// strip, and the body the stage machine chooses — one component that MATURES from Cold to
// Closing rather than four panels that replace each other, because the golfer is watching
// one object accumulate evidence and a panel that swapped itself out would break that.
//
// IT TAKES A `source` AND OWNS NOTHING. Every string, every count and every tick state on
// this panel is read off one object shaped like SessionDiagnosticsModel; the wiring that
// produces one — the session directory, the shot processor, review — lives next door in
// PpSessionDiagnosticsPanel. That split is the same one PpLaunchMonitorPanel makes with
// PpLmGraphicsBody and it buys the same thing: this file can be loaded offscreen and
// pressed with a fixture, so the states that matter most (Cold with no expectations, the
// quiet strip, a not-assessable tick) are asserted rather than eyeballed. It also means
// there is exactly one answer to "where does this number come from" — `source` — and no
// second place for a zone to compute one.
//
// ── HOW IT SIZES ITSELF ──────────────────────────────────────────────────────────────
//
// The design is quoted at 1168 × 560, and the panel's homes are that stage, a 396-wide
// split beside another panel, and a 1920 second screen. So every metric on it is one scale
// factor `k` away from the design's own proportions, and _fitFor() takes the largest k at
// which the WHOLE design still fits both axes. k never drops below 1: below the design size
// the panel does not shrink, it REDUCES — `compact` drops the captions the narrow
// arrangement drops (12c) and the card row takes what fits and counts the rest. A
// recurrence count too small to read is not a smaller panel, it is a useless one, and this
// is the launch monitor board's rule applied to a second surface for the same reason.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Rectangle {
    id: root

    // Anything with SessionDiagnosticsModel's read surface. Null draws the empty chrome.
    property var source: null

    // Panel-local, and per brief §8 the only state this panel owns beyond the carousel's
    // selection and the cadence setting.
    property bool watchingExpanded: false

    objectName: "sdBody"

    radius: Theme.radius
    color: Theme.colorBg2
    border.width: 1
    border.color: Theme.colorBorderMid
    clip: true

    // ── the design's own proportions, at k = 1 ───────────────────────────────
    readonly property int baseW: 1168
    readonly property int baseH: 560
    // 12b doubles the type at 1920; past ~2.4 the panel is being read from further away
    // than it was drawn for and more scale stops buying legibility.
    readonly property real kMax: 2.4
    // Below this the wide arrangement gives way to 12c's reductions.
    readonly property int baseCompactW: 640

    function _fitFor(w, h) {
        if (w <= 0 || h <= 0)
            return 1
        const kw = w / (baseW * Theme.fontScale)
        const kh = h / (baseH * Theme.fontScale)
        return Math.max(1, Math.min(kMax, Math.min(kw, kh)))
    }

    readonly property real k: _fitFor(width, height)
    readonly property bool compact: width < Math.round(baseCompactW * Theme.fontScale)

    // A design pixel, through the app's type scale and then the panel's fit. The one place
    // a number from the mock is allowed to appear.
    function px(n) { return Math.round(n * Theme.fontScale * root.k) }

    // The mock's 8–15 px type, on the Theme scale, at the panel's fit. fontSzMicro is the
    // smallest token there is, so the 8 px captions go through Theme.sp() — the same
    // fontScale, one step below the scale's floor.
    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * k))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro   * k))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel   * k))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2   * k))
    readonly property int tzData:    Math.max(1, Math.round(Theme.fontSzDataSm  * k))
    readonly property int tzHead:    Math.max(1, Math.round(Theme.fontSzHeading * k))

    // ── the source, read once ────────────────────────────────────────────────
    readonly property var header:       source ? source.headerInfo    : null
    readonly property string stage:     source ? (source.stage || "") : ""
    readonly property var cards:        source ? source.cards         : []
    readonly property var expectations: source ? source.expectations  : []
    readonly property var bookends:     source ? source.bookends      : []

    readonly property bool isCold:      stage === "cold"
    readonly property bool isEstablished: stage === "established"
    readonly property bool isClosing:   stage === "closing"

    // ── how many cards fit, and how many are left over ───────────────────────
    // The MODEL decides which cards come first (hystereticOrder), so taking a prefix is a
    // decision about space and never about importance. What does not fit is COUNTED rather
    // than dropped silently — a pattern that scrolled off the end still happened.
    readonly property int baseCardMinW: 300
    readonly property int _cardGap: px(8)
    readonly property int _cardsShown: {
        const n = cards ? cards.length : 0
        if (n <= 0 || width <= 0) return 0
        const avail = width - 2 * px(10)
        const perRow = Math.max(1, Math.floor((avail + _cardGap) / (px(baseCardMinW) + _cardGap)))
        return Math.min(n, perRow)
    }
    readonly property int _cardsHidden: (cards ? cards.length : 0) - _cardsShown

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin:   root.px(10)
        anchors.rightMargin:  root.px(10)
        anchors.bottomMargin: root.px(10)
        anchors.topMargin:    0
        spacing: root.px(8)

        // ── header ───────────────────────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: root.px(34)

            Row {
                anchors.left: parent.left
                anchors.leftMargin: root.px(2)
                // Anchored past the cadence note only while there IS one. Collapsing the
                // note's own width instead would make its implicitWidth depend on its width.
                anchors.right: cadenceNote.visible ? cadenceNote.left : parent.right
                anchors.rightMargin: root.px(9)
                anchors.verticalCenter: parent.verticalCenter
                spacing: root.px(9)

                Text {
                    objectName: "sdTitle"
                    anchors.verticalCenter: parent.verticalCenter
                    // 12c abbreviates rather than eliding: the panel's own name is the last
                    // thing that should be half a word.
                    text: root.compact ? qsTr("SESSION DIAG.") : qsTr("SESSION DIAGNOSTICS")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
                Text {
                    objectName: "sdShotLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.header ? (root.header.shotLabel || "") : ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    color: Theme.colorText3
                }
                Rectangle {
                    objectName: "sdStageChip"
                    anchors.verticalCenter: parent.verticalCenter
                    width:  stageText.implicitWidth + root.px(14)
                    height: stageText.implicitHeight + root.px(4)
                    radius: Math.max(1, root.px(3))
                    color: "transparent"
                    border.width: 1
                    border.color: Theme.colorBorderMid
                    visible: stageText.text !== ""

                    Text {
                        id: stageText
                        anchors.centerIn: parent
                        text: root.header ? (root.header.stageLabel || "") : ""
                        font.family: Theme.fontData
                        font.pixelSize: root.tzCaption
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText
                    }
                }
                Text {
                    objectName: "sdStageNote"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !root.compact && text !== ""
                    text: root.header ? (root.header.countLine || "") : ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    color: Theme.colorText2
                }
            }

            Text {
                id: cadenceNote
                objectName: "sdCadenceNote"
                anchors.right: parent.right
                anchors.rightMargin: root.px(2)
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.compact && text !== ""
                text: root.header ? (root.header.cadenceNote || "") : ""
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }

        // ── the after-shot strip, or the session's bookends ──────────────────
        // A CLOSED session has no after-shot moment, so the strip that reported one is
        // replaced rather than emptied.
        PpThisShotStrip {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: !root.isClosing
            chips:   root.source ? root.source.thisShot : []
            delta:   root.source ? root.source.afterShotDelta : null
            quiet:   root.source ? root.source.quiet === true : false
            fit:     root.k
            compact: root.compact
        }

        Rectangle {
            objectName: "sdBookends"
            Layout.fillWidth: true
            Layout.preferredHeight: root.px(56)
            visible: root.isClosing
            color: Theme.colorSurface
            radius: Theme.radius
            border.width: 1
            border.color: Theme.colorBorderMid
            clip: true

            Row {
                id: bookendsRow
                anchors.fill: parent
                anchors.leftMargin:   root.px(10)
                anchors.rightMargin:  root.px(10)
                anchors.topMargin:    root.px(7)
                anchors.bottomMargin: root.px(7)
                spacing: root.px(14)

                Text {
                    id: bookendsLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("SESSION\nBOOKENDS")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }

                Repeater {
                    model: root.bookends

                    Item {
                        required property var modelData
                        objectName: "sdBookend"

                        // Even shares of what is left after the label and the gaps. Named
                        // through the Row's id: a Repeater delegate's `parent` is the Row at
                        // run time but the Repeater to anything reading the file.
                        readonly property int _n: root.bookends ? root.bookends.length : 1
                        width: Math.max(0, (bookendsRow.width - bookendsLabel.width
                                            - bookendsRow.spacing * _n) / Math.max(1, _n))
                        height: bookendsRow.height

                        Rectangle {
                            anchors.left: parent.left
                            width: 1
                            height: parent.height
                            color: Theme.colorBorderMid
                        }

                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: root.px(12)
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: root.px(2)

                            Text {
                                width: parent.width
                                text: (modelData.name || "") + " ▸"
                                elide: Text.ElideRight
                                font.family: Theme.fontData
                                font.pixelSize: root.tzCaption
                                font.letterSpacing: Theme.trackingLabel
                                color: Theme.colorAccent
                            }
                            Text {
                                width: parent.width
                                text: modelData.representativeText || ""
                                elide: Text.ElideRight
                                font.family: Theme.fontData
                                font.pixelSize: root.tzMicro
                                color: Theme.colorText
                            }
                            Text {
                                width: parent.width
                                text: (modelData.worstText || "") + " · " + (modelData.bestText || "")
                                elide: Text.ElideRight
                                font.family: Theme.fontData
                                font.pixelSize: root.tzCaption
                                color: Theme.colorText3
                            }
                        }
                    }
                }
            }

        }

        // ── the body the stage machine chooses ───────────────────────────────
        Item {
            id: stageBody
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ── Cold ─────────────────────────────────────────────────────────
            Rectangle {
                objectName: "sdColdBody"
                anchors.fill: parent
                visible: root.isCold
                color: Theme.colorSurface
                radius: Theme.radius
                border.width: 1
                border.color: Theme.colorBorderMid
                clip: true

                Column {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin:  root.px(18)
                    anchors.rightMargin: root.px(18)
                    anchors.topMargin:   root.px(16)
                    spacing: root.px(14)

                    Column {
                        width: parent.width
                        spacing: root.px(5)

                        Text {
                            objectName: "sdColdHeadline"
                            width: parent.width
                            text: root.header ? (root.header.coldLine || "") : ""
                            wrapMode: Text.WordWrap
                            font.family: Theme.fontBody
                            font.pixelSize: root.tzHead
                            font.weight: Theme.fontBodyWeight
                            color: Theme.colorText
                        }
                        Text {
                            objectName: "sdColdSubline"
                            width: parent.width
                            text: root.header ? (root.header.countLine || "") : ""
                            elide: Text.ElideRight
                            font.family: Theme.fontData
                            font.pixelSize: root.tzMicro
                            color: Theme.colorText2
                        }
                    }

                    // USUALLY YOURS. Omitted ENTIRELY when the athlete has no fault profile
                    // yet — an empty "expectations" heading over nothing would read as the
                    // model having looked and found none, which is a different claim.
                    Column {
                        objectName: "sdExpectations"
                        width: parent.width
                        spacing: root.px(7)
                        visible: root.expectations && root.expectations.length > 0

                        Text {
                            text: qsTr("USUALLY YOURS · EXPECTATIONS TO TEST, NOT FINDINGS")
                            font.family: Theme.fontData
                            font.pixelSize: root.tzCaption
                            font.letterSpacing: Theme.trackingMicro
                            color: Theme.colorText3
                        }

                        Flow {
                            width: parent.width
                            spacing: root.px(8)

                            Repeater {
                                model: root.expectations

                                Item {
                                    required property var modelData
                                    objectName: "sdExpectationCard"

                                    // Design width, but never wider than the panel — in the
                                    // narrow arrangement the cards become one column.
                                    width: Math.min(root.px(250), parent.width)
                                    height: expectCol.implicitHeight + 2 * root.px(9)

                                    // Dashed, and that is the whole point: this is the one
                                    // thing on the panel that is not evidence from this
                                    // session (brief §3.3).
                                    PpDashedFrame {
                                        anchors.fill: parent
                                        frameRadius: Theme.radius
                                        strokeColor: Theme.colorBorderMid
                                        dashOn:  Math.max(1, root.px(3))
                                        dashOff: Math.max(1, root.px(3))
                                    }

                                    Column {
                                        id: expectCol
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.leftMargin:  root.px(11)
                                        anchors.rightMargin: root.px(11)
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: root.px(3)

                                        Text {
                                            width: parent.width
                                            text: modelData.name || ""
                                            elide: Text.ElideRight
                                            font.family: Theme.fontBody
                                            font.pixelSize: root.tzBody
                                            font.weight: Theme.fontBodyWeight
                                            color: Theme.colorText2
                                        }
                                        Text {
                                            width: parent.width
                                            text: modelData.text || ""
                                            elide: Text.ElideRight
                                            font.family: Theme.fontData
                                            font.pixelSize: root.tzMicro
                                            color: Theme.colorText3
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // The sentence that has to stay true in the implementation as well as on
                    // the screen (brief §3.3, §5.5). It is here because the profile is on
                    // screen; it is true because profileBias() has two call sites.
                    Text {
                        width: parent.width
                        visible: root.expectations && root.expectations.length > 0
                        text: qsTr("from your fault profile · biases ranking and presentation only, never firing or corridors")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontData
                        font.pixelSize: root.tzCaption
                        color: Theme.colorText3
                    }
                }
            }

            // ── Forming, and Closing until the rail lands ────────────────────
            Item {
                objectName: "sdCardsBody"
                anchors.fill: parent
                visible: !root.isCold && !root.isEstablished

                Item {
                    id: pictureHeader
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.leftMargin:  root.px(2)
                    anchors.rightMargin: root.px(2)
                    height: pictureLabel.implicitHeight

                    Text {
                        id: pictureLabel
                        anchors.left: parent.left
                        anchors.top: parent.top
                        text: qsTr("SESSION PICTURE")
                        font.family: Theme.fontData
                        font.pixelSize: root.tzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText2
                    }
                    Text {
                        objectName: "sdFormingNote"
                        anchors.left: pictureLabel.right
                        anchors.leftMargin: root.px(9)
                        anchors.right: moreTail.visible ? moreTail.left : parent.right
                        anchors.rightMargin: root.px(9)
                        anchors.baseline: pictureLabel.baseline
                        // "no chain drawn: the model authors no edge between these two" —
                        // the model did not FAIL to find a chain, and this is what stops
                        // that being read into the flat card row.
                        text: root.header ? (root.header.formingLine || root.header.closingLine || "") : ""
                        elide: Text.ElideRight
                        font.family: Theme.fontData
                        font.pixelSize: root.tzMicro
                        color: Theme.colorText3
                    }
                    Text {
                        id: moreTail
                        objectName: "sdMoreTail"
                        anchors.right: parent.right
                        anchors.baseline: pictureLabel.baseline
                        visible: root._cardsHidden > 0
                        text: qsTr("+%1 more").arg(root._cardsHidden)
                        font.family: Theme.fontData
                        font.pixelSize: root.tzMicro
                        color: Theme.colorText3
                    }
                }

                Row {
                    id: cardRow
                    objectName: "sdCardsRow"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: pictureHeader.bottom
                    anchors.topMargin: root.px(8)
                    height: Math.min(root.px(150), Math.max(0, parent.height - pictureHeader.height - root.px(8)))
                    spacing: root._cardGap

                    Repeater {
                        model: root._cardsShown

                        PpPatternCard {
                            required property int index
                            card: root.cards[index]
                            fit: root.k
                            width: Math.max(0, (cardRow.width - (root._cardsShown - 1) * cardRow.spacing)
                                               / Math.max(1, root._cardsShown))
                            height: cardRow.height
                        }
                    }
                }
            }

            // ── Established ──────────────────────────────────────────────────
            // TODO(phase 7): the chain rail — chain A's live spine, chain B's screened root
            // and ghosts, the graded link columns, and the LIKELY DRIVER strip below it.
            // Deliberately empty rather than placeholdered: an "Established" panel showing a
            // "coming soon" card would be claiming the model found nothing to chain.
            Item {
                objectName: "sdEstablishedBody"
                anchors.fill: parent
                visible: root.isEstablished
            }
        }

        // ── unchained pattern ────────────────────────────────────────────────
        // A pattern the model authors no edge for gets its OWN line — reported, never
        // forced onto a chain (brief §5.3).
        Rectangle {
            objectName: "sdUnchainedRow"
            Layout.fillWidth: true
            Layout.preferredHeight: root.px(26)
            visible: unchainedText.text !== ""
            color: "transparent"
            radius: Theme.radius
            border.width: 1
            border.color: Theme.colorBorderMid

            Row {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin:  root.px(10)
                anchors.rightMargin: root.px(10)
                anchors.verticalCenter: parent.verticalCenter
                spacing: root.px(9)

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("UNCHAINED PATTERN")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorAttention
                }
                Text {
                    id: unchainedText
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.source ? (root.source.unchainedLine || "") : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontBody
                    font.pixelSize: root.tzLabel
                    font.weight: Theme.fontBodyWeight
                    color: Theme.colorText
                }
            }
        }

        // ── watching, then coverage ──────────────────────────────────────────
        PpWatchingRow {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            items: root.source ? root.source.watching : []
            expanded: root.watchingExpanded
            fit: root.k
            onToggled: root.watchingExpanded = !root.watchingExpanded
        }

        PpCoverageLine {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.leftMargin: root.px(4)
            line: root.source ? (root.source.coverageLine || "") : ""
            fit: root.k
        }
    }
}
