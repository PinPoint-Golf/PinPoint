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
    // selection and the cadence setting. Two collapsed regions, both of which open into the
    // SAME content rather than a different view: Watching (n), and — in the 396 arrangement
    // only — every chain after the first.
    property bool watchingExpanded: false
    property int  expandedChain: -1

    // A screen was asked for, from the driver footer's CTA or from a screened root on the
    // rail. `screenRef` is the model's when it recommended that screen and empty when it did
    // not; `conditionId` always names what the screen would settle, so a host can route on
    // whichever it has. THIS PANEL RUNS NO SCREEN: the protocol UI is not designed yet
    // (brief §9) and inventing one here would be inventing a design.
    signal screenRequested(string screenRef, string conditionId)

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
    readonly property var chains:       source ? source.chains        : []
    readonly property var driver:       source ? source.driver        : null

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

    // ── how many chains the rail draws ───────────────────────────────────────
    // The model sorts most-evidenced first (buildChains()), so taking the first two is a
    // decision about a 560 px column and never about which chain matters. A session's
    // authored neighbourhood can yield eighteen rails; what does not fit is COUNTED, exactly
    // as the card row counts what it could not draw.
    readonly property int _chainsShown: root.compact
                                        ? (chains ? chains.length : 0)
                                        : Math.min(chains ? chains.length : 0, 2)
    readonly property int _chainsHidden: (chains ? chains.length : 0) - _chainsShown

    // driver.screenConditionId / screenRef — the only place the published surface carries a
    // screen ref for the screened-root node the rail draws.
    readonly property string _screenConditionId: driver ? (driver.screenConditionId || "") : ""
    readonly property string _screenRef:         driver ? (driver.screenRef || "") : ""

    // The footer exists in Established and Closing, which is where the mock has it.
    readonly property bool _hasDriverFooter: (isEstablished || isClosing) && !!driver
    // ...but it only carries the coverage line in the wide arrangement with a driver to sit
    // beside. A waiting footer and 12c's three-line footer both drop the right-hand column,
    // and the coverage line is never dropped with it (brief §1) — it goes back to the bottom.
    readonly property bool _footerCarriesCoverage:
        _hasDriverFooter && !compact && driver.eligible === true

    // 12c's collapsed chain, from the model's own node names — the arrow and the separator are
    // the only things composed here, the same contribution PpWatchingRow makes to its line.
    function _chainSummary(c) {
        if (!c || !c.nodes) return ""
        const parts = []
        for (let i = 0; i < c.nodes.length; ++i)
            parts.push(c.nodes[i].name || "")
        return parts.join(" → ")
    }
    // ...and the marks, which are what make a collapsed chain B worth opening: it is mostly
    // ghosts and a screened root, and the summary must not hide that.
    function _chainNote(c) {
        if (!c || !c.nodes) return ""
        const parts = []
        for (let i = 0; i < c.nodes.length; ++i)
            if (c.nodes[i].mark)
                parts.push(c.nodes[i].mark)
        return parts.join(" · ")
    }

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

            // ── Established: the chain rail ──────────────────────────────────
            Item {
                objectName: "sdEstablishedBody"
                anchors.fill: parent
                visible: root.isEstablished

                // ── wide: chains stacked as rows ─────────────────────────────
                Column {
                    id: wideChains
                    anchors.fill: parent
                    visible: !root.compact
                    spacing: root.px(8)

                    readonly property int _tailH: chainsTail.visible
                                                  ? chainsTail.implicitHeight + root.px(4) : 0
                    readonly property int _railH:
                        root._chainsShown > 0
                        ? Math.max(0, Math.floor((height - _tailH
                                                  - (root._chainsShown - 1) * spacing)
                                                 / root._chainsShown))
                        : 0

                    Repeater {
                        model: root.compact ? 0 : root._chainsShown

                        PpChainRail {
                            required property int index
                            width:  wideChains.width
                            height: wideChains._railH
                            chain:  root.chains[index]
                            fit:    root.k
                            screenConditionId: root._screenConditionId
                            screenRef:         root._screenRef
                            onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                        }
                    }

                    Text {
                        id: chainsTail
                        objectName: "sdChainsTail"
                        width: parent.width
                        visible: root._chainsHidden > 0
                        horizontalAlignment: Text.AlignRight
                        // The card row's own tail, verbatim: it counts and it does not decline
                        // a noun, so "+1 more" is right at every count.
                        text: qsTr("+%1 more").arg(root._chainsHidden)
                        font.family: Theme.fontData
                        font.pixelSize: root.tzCaption
                        color: Theme.colorText3
                    }
                }

                // ── 12c: the spine, turned on its side ───────────────────────
                // A 560 px column cannot hold a vertical rail AND the strips around it, and
                // the mock's answer is not pagination — it is that the rail scrolls and
                // everything below it stays put. Paging a chain would break the one thing the
                // rail is for, which is reading it as a single shape.
                Flickable {
                    id: narrowChains
                    objectName: "sdChainsFlick"
                    anchors.fill: parent
                    visible: root.compact
                    contentWidth: width
                    contentHeight: narrowCol.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: narrowCol
                        width: narrowChains.width
                        spacing: root.px(7)

                        // The first chain is open, because a panel whose every chain is
                        // collapsed has drawn no rail at all.
                        PpChainRail {
                            width:  narrowCol.width
                            height: implicitHeight
                            visible: root.compact && !!root.chains && root.chains.length > 0
                            chain:  (root.chains && root.chains.length > 0) ? root.chains[0] : null
                            fit:    root.k
                            vertical: true
                            screenConditionId: root._screenConditionId
                            screenRef:         root._screenRef
                            onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                        }

                        Repeater {
                            model: root.compact
                                   ? Math.max(0, (root.chains ? root.chains.length : 0) - 1)
                                   : 0

                            Column {
                                id: collapsedChain
                                required property int index
                                readonly property int chainIndex: index + 1
                                readonly property bool open: root.expandedChain === chainIndex
                                readonly property var chainData: root.chains[chainIndex]

                                width: narrowCol.width
                                spacing: root.px(7)

                                Item {
                                    objectName: "sdChainCollapsed"
                                    width: parent.width
                                    height: summaryCol.implicitHeight + 2 * root.px(8)

                                    // Dashed for the same reason the Cold expectations are:
                                    // this is a chain the panel has not drawn yet, not a
                                    // finding of its own.
                                    PpDashedFrame {
                                        anchors.fill: parent
                                        frameRadius: Theme.radius
                                        strokeColor: Theme.colorBorderMid
                                        dashOn:  Math.max(1, root.px(3))
                                        dashOff: Math.max(1, root.px(3))
                                    }

                                    Column {
                                        id: summaryCol
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.leftMargin:  root.px(9)
                                        anchors.rightMargin: root.px(9)
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: root.px(4)

                                        Item {
                                            width: parent.width
                                            height: chainLabel.implicitHeight

                                            Text {
                                                id: chainLabel
                                                objectName: "sdChainCollapsedLabel"
                                                anchors.left: parent.left
                                                text: qsTr("CHAIN %1").arg(collapsedChain.chainIndex + 1)
                                                font.family: Theme.fontData
                                                font.pixelSize: root.tzCaption
                                                font.letterSpacing: Theme.trackingMicro
                                                color: Theme.colorText2
                                            }
                                            Text {
                                                objectName: "sdChainCollapsedToggle"
                                                anchors.right: parent.right
                                                anchors.baseline: chainLabel.baseline
                                                text: collapsedChain.open ? qsTr("CLOSE ▴")
                                                                          : qsTr("OPEN ▸")
                                                font.family: Theme.fontData
                                                font.pixelSize: root.tzCaption
                                                font.letterSpacing: Theme.trackingLabel
                                                color: Theme.colorAccent
                                            }
                                        }

                                        Text {
                                            objectName: "sdChainCollapsedSummary"
                                            width: parent.width
                                            text: root._chainSummary(collapsedChain.chainData)
                                            wrapMode: Text.WordWrap
                                            lineHeight: 1.4
                                            font.family: Theme.fontBody
                                            font.pixelSize: root.tzMicro
                                            font.weight: Theme.fontBodyWeight
                                            color: Theme.colorText
                                        }
                                        Text {
                                            objectName: "sdChainCollapsedNote"
                                            width: parent.width
                                            visible: text !== ""
                                            text: root._chainNote(collapsedChain.chainData)
                                            wrapMode: Text.WordWrap
                                            lineHeight: 1.4
                                            font.family: Theme.fontData
                                            font.pixelSize: root.tzCaption
                                            color: Theme.colorText3
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: root.expandedChain =
                                            collapsedChain.open ? -1 : collapsedChain.chainIndex
                                    }
                                }

                                // Opening reveals THE SAME RAIL, in place — same nodes, same
                                // links, same grades (brief §8). Nothing about the chain was
                                // being withheld by the summary.
                                PpChainRail {
                                    width:  collapsedChain.width
                                    height: visible ? implicitHeight : 0
                                    visible: collapsedChain.open
                                    chain:  collapsedChain.chainData
                                    fit:    root.k
                                    vertical: true
                                    screenConditionId: root._screenConditionId
                                    screenRef:         root._screenRef
                                    onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                                }
                            }
                        }
                    }
                }
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

        // STATED EXACTLY ONCE. In Established and Closing the driver footer carries the
        // coverage line, because the mock puts it in the footer's right-hand column beside
        // the rival it could not adjudicate — the two are the same disclosure. Drawing it in
        // both places would not be twice as honest, it would read as two different numbers.
        PpCoverageLine {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            Layout.leftMargin: root.px(4)
            line: root._footerCarriesCoverage ? ""
                                              : (root.source ? (root.source.coverageLine || "") : "")
            fit: root.k
        }

        // ── likely driver ────────────────────────────────────────────────────
        PpDriverFooter {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? implicitHeight : 0
            visible: root._hasDriverFooter
            driver: root.driver
            coverageLine: root.source ? (root.source.coverageLine || "") : ""
            fit: root.k
            compact: root.compact
            onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
        }
    }
}
