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

    // SessionDiagnosticsModel::shotReadout(selectedShotId), for the shot the carousel has
    // focused — null when nothing is selected. THE ONE THING THIS BODY TAKES THAT IS NOT A
    // PROPERTY OF `source`, and it is because shotReadout() is an INVOKABLE: it cannot be
    // bound, so somebody has to call it and re-call it when the selection or the surface
    // changes. That somebody is the panel, which is where the model's signals are (see
    // PpSessionDiagnosticsPanel). A test hands a fixture map in directly, exactly as it hands
    // `source` in — the body still computes nothing.
    property var readout: null

    // Panel-local, and per brief §8 the only state this panel owns beyond the carousel's
    // selection and the cadence setting. Two collapsed regions, both of which open into the
    // SAME content rather than a different view: Watching (n), and — in the 396 arrangement
    // only — every chain after the first. (The miss picker below is a third, and it is a
    // transient rather than a state: it is open only while it is being answered.)
    property bool watchingExpanded: false
    property int  expandedChain: -1

    // Off on the auto-closing cast, where every affordance on the panel is a trap: the
    // surface is a glance and it is about to vanish under the pointer
    // (PpSessionDiagnosticsWindow.interactive). Nothing about what the panel SAYS changes.
    property bool interactive: true

    // ── the two declarations the golfer can make (design §A6) ────────────────
    //
    // BOTH GO STRAIGHT TO THE MODEL AND NEITHER IS MIRRORED HERE. declareFocus, clearFocus and
    // declareMiss are the model's, the read-back is the model's (`focused` on each card,
    // `declaredMiss` on the surface), and this file holds no copy that could disagree with it.
    // That is what makes a declined declaration — the model is reviewing, the id is not in the
    // pack — leave the panel exactly as it was rather than showing a state the ledger is not in.
    //
    // GUARDED ON THE FUNCTION, not on the source being non-null: the offscreen test presses
    // this body with a plain fixture object, and a fixture that wants to assert the tap
    // arrived supplies a spy of the same name. A fixture that does not gets a no-op instead of
    // a TypeError that would fail an unrelated assertion three tests later.
    function _declareFocus(conditionId, on) {
        if (!source) return
        if (on && typeof source.declareFocus === "function")
            source.declareFocus(conditionId)
        else if (!on && typeof source.clearFocus === "function")
            source.clearFocus()
    }
    function _declareMiss(missId) {
        if (source && typeof source.declareMiss === "function")
            source.declareMiss(missId)
        missPicker.close()
    }
    function _missCandidates() {
        if (source && typeof source.missCandidates === "function")
            return source.missCandidates() || []
        return []
    }

    // ── the after-shot pulse (§B3, §B8) ──────────────────────────────────────
    //
    // ONE CUE OBJECT, PUBLISHED DOWNWARD: { token, ids, focusId }. Single-property so a card
    // can never see ids that belong to a different shot, and a token so the same set of ids
    // firing twice in a row is still two events.
    //
    // CALLED, NOT BOUND. shotIngested is a SIGNAL on SessionDiagnosticsModel and this file has
    // no model — the panel next door does, and it calls this when one lands, exactly as it
    // calls shotReadout() and hands the result down. It is also the seam the offscreen test
    // uses: a synthetic after-shot moment is a fixture plus this call, with no event loop and
    // no ingest in it.
    property var pulseCue: null
    property int _pulseToken: 0

    function pulseAfterShot() {
        const d = source ? source.afterShotDelta : null
        const fired = (d && d.fired) ? d.fired : []
        if (fired.length === 0) { root.pulseCue = null; return }

        const wanted = {}
        for (let i = 0; i < fired.length; ++i) wanted[fired[i]] = true

        // ORDER IS THE PANEL'S DRAWING ORDER, never the delta's. `fired` comes out in the
        // shot's own row order; what the eye is following is the sweep down the arrangement in
        // front of it, and a stagger that ran in some third order would read as a shuffle.
        // So the body on screen decides: the rail's nodes when the rail is the body, the card
        // row's cards when it is not, and whatever fired that the chosen body does not draw
        // brings up the rear rather than being dropped — it still happened.
        const ids = []
        function take(id) {
            if (id && wanted[id] && ids.indexOf(id) < 0) ids.push(id)
        }
        function takeCards() {
            const cs = root.cards || []
            for (let i = 0; i < cs.length; ++i) take(cs[i].id)
        }
        function takeRail() {
            const chs = root.chains || []
            for (let i = 0; i < chs.length; ++i) {
                const ns = chs[i].nodes || []
                for (let j = 0; j < ns.length; ++j) take(ns[j].id)
            }
        }
        if (root._railBody) { takeRail(); takeCards() } else { takeCards(); takeRail() }
        for (let i = 0; i < fired.length; ++i) take(fired[i])

        // THE FOCUSED NODE LEADS. Under a focus contract its marker is the headline of the
        // moment (§B3), and leading the stagger is how a sweep says so without a second mark:
        // it is the one that moves first and the rest follow it.
        const focusId = d.focusId || ""
        const at = ids.indexOf(focusId)
        if (at > 0) { ids.splice(at, 1); ids.unshift(focusId) }

        root._pulseToken += 1
        root.pulseCue = { token: root._pulseToken, ids: ids, focusId: focusId }
    }

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
    readonly property bool isForming:   stage === "forming"
    readonly property bool isEstablished: stage === "established"
    readonly property bool isClosing:   stage === "closing"

    // ── the declared miss ────────────────────────────────────────────────────
    //
    // DESIGN-REVIEW: minimal by intent. The chip asks and the picker answers, and neither
    // says anything about what the declaration DID — `missChain` is published (everything
    // authored upstream of the outcome, pre-armed) and nothing on this panel draws it yet,
    // so the golfer declares a miss and sees the chip change and nothing else. There is also
    // no verification tense: the model checks the declaration against launch-monitor data
    // where the session has any, and the panel does not report the answer. Both want a
    // design pass.
    //
    // The session's opening question — "what is the bad shot?" — and the panel ASKS it only
    // while the answer can still shape the session: Cold and Forming, when there is not yet a
    // picture to read. Past that the invitation stands down and the chip reports whatever was
    // declared, because a declaration is a fact about the session and does not expire with the
    // stage that took it.
    readonly property string declaredMiss:     source ? (source.declaredMiss || "")     : ""
    readonly property string declaredMissName: source ? (source.declaredMissName || "") : ""
    readonly property bool _missInvited:
        interactive && declaredMiss === "" && (isCold || isForming) && !reviewing

    // ── review (13a, brief §6) ───────────────────────────────────────────────
    // READ OFF THE PUBLISHED SURFACE, never off a controller: the model is the one that
    // decides the panel is in review (it also freezes the stage at Closing when it is), and a
    // body that asked something else could disagree with the counts it is drawing. The badge
    // string is non-empty exactly when the model is reviewing a ledger with shots in it.
    readonly property bool reviewing:
        !!header && (header.reviewBadge || "") !== ""
    // ...and a SHOT IS BEING READ only once the carousel has focused one. Review without a
    // selection is the finished session's own summary — bookends and all — because the panel
    // holds the final state and selection is what enters shot-reading (brief §6).
    readonly property bool reviewingShot:
        reviewing && !!readout && !!readout.conditions && readout.conditions.length > 0

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

    // DID THE SESSION EVER ESTABLISH? Published by the model beside the displayed stage,
    // because `stage` is frozen at Closing for anything closed or under review and cannot
    // answer it. See buildHeader().
    readonly property bool reachedEstablished:
        isEstablished || (!!header && header.reachedEstablished === true)

    // THE RAIL SURVIVES THE CLOSE — AND ONLY FOR A SESSION THAT EARNED ONE. A session that
    // established still has its chain once it is finished, and 13a draws the reviewed shot
    // against that rail; losing it at the close would mean the one arrangement review is FOR
    // could never be reached.
    //
    // But `chains.length > 0` is NOT the test for having earned one. extractChains() publishes
    // the authored neighbourhood around every pattern, so a Forming session with two unchained
    // patterns still gets rails — ghosts, screened roots and unanchored links scaffolded around
    // nodes that share no edge. Drawing those at the close replaces two full pattern cards,
    // with the tick runs and the review pills that are the whole point of reading a shot inside
    // the finished ledger, with slim node rows for a chain the model explicitly says it did not
    // author (the UNCHAINED line, one row below, says so in words). Design 12a is unambiguous:
    // Forming is "flat cards, no chain because these two patterns share no authored edge", and
    // closing a Forming session does not turn it into an Established one.
    //
    // So the composition asks the RATCHET, which is the record of what the session achieved,
    // and the chains stay published either way — unread in this arrangement, not withdrawn.
    readonly property bool _railBody:
        reachedEstablished && (isEstablished
                               || (isClosing && !!chains && chains.length > 0))

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
                // ── the tense ────────────────────────────────────────────────
                // REVIEWING · shot 9 of 14, framed in the accent at ~35% and lettered in it.
                // It sits beside the stage chip rather than replacing it because they say
                // different things — the stage is what the ledger matured to, the badge is
                // which tense the panel is being read in.
                Rectangle {
                    objectName: "sdReviewBadge"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: reviewBadgeText.text !== ""
                    width:  reviewBadgeText.implicitWidth + root.px(14)
                    height: reviewBadgeText.implicitHeight + root.px(4)
                    radius: Math.max(1, root.px(3))
                    color: "transparent"
                    border.width: 1
                    border.color: Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g,
                                          Theme.colorAccent.b, 0.35)

                    Text {
                        id: reviewBadgeText
                        anchors.centerIn: parent
                        text: root.header ? (root.header.reviewBadge || "") : ""
                        font.family: Theme.fontData
                        font.pixelSize: root.tzCaption
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorAccent
                    }
                }
                // ── the declared miss ────────────────────────────────────────
                // A chip, in the header, beside the stage — because it is a statement about
                // the SESSION and not about a pattern, and because it is the one thing on
                // this panel the golfer supplies rather than the model. It is deliberately
                // NOT tinted like a finding: intent shapes attention and pre-arms chains, and
                // it is never evidence (§A6). The accent is the app's "you can act here"
                // colour, which is exactly what it is.
                Rectangle {
                    id: missChip
                    objectName: "sdMissChip"
                    anchors.verticalCenter: parent.verticalCenter
                    visible: root._missInvited || root.declaredMiss !== ""
                    width:  missText.implicitWidth + root.px(14)
                    height: missText.implicitHeight + root.px(4)
                    radius: Math.max(1, root.px(3))
                    color: "transparent"
                    border.width: 1
                    border.color: Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g,
                                          Theme.colorAccent.b,
                                          missHover.hovered ? 0.55 : 0.30)
                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        id: missText
                        anchors.centerIn: parent
                        // The name once there is one, and the invitation until then. The
                        // caret says it opens something; the middot says it is settled and
                        // can still be changed.
                        text: root.declaredMiss !== ""
                              ? qsTr("MISS · %1").arg(root.declaredMissName || root.declaredMiss)
                                + (root.interactive ? qsTr(" ▸") : "")
                              : qsTr("DECLARE MISS ▸")
                        font.family: Theme.fontData
                        font.pixelSize: root.tzCaption
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorAccent
                    }

                    HoverHandler { id: missHover; enabled: root.interactive
                                   cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: root.interactive
                        onTapped: missPicker.openBelow(missChip)
                    }
                }

                Text {
                    objectName: "sdReviewNote"
                    anchors.verticalCenter: parent.verticalCenter
                    // "final session state · this shot read inside the finished ledger" — the
                    // sentence that stops the counts beside it being read as this shot's.
                    visible: !root.compact && text !== ""
                    text: root.header ? (root.header.reviewNote || "") : ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    color: Theme.colorText2
                }
                Text {
                    objectName: "sdStageNote"
                    anchors.verticalCenter: parent.verticalCenter
                    // In review the count line moves to the right-hand end (13a), where it
                    // reads as the session's total rather than as a note on the stage.
                    visible: !root.compact && !root.reviewing && text !== ""
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
                // The right-hand end carries ONE of them, and never both: cadence is a live
                // statement (there is no cadence gating in review, brief §6) and the count
                // line is the reviewed session's total. So the slot changes tense with the
                // panel instead of stacking two captions nobody asked to compare.
                visible: !root.compact && text !== ""
                text: root.header
                      ? (root.reviewing ? (root.header.countLine || "")
                                        : (root.header.cadenceNote || ""))
                      : ""
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }

        // ── the after-shot strip, the bookends, or the reviewed shot ─────────
        // ONE SLOT, THREE TENSES. A live session reports the moment after the swing; a closed
        // one has no after-shot moment, so the strip that reported one is REPLACED rather than
        // emptied; and a closed one with a shot picked off the carousel reports that shot,
        // read inside the finished ledger. They are the same slot because they are the same
        // question — "what does this panel have to say about a swing" — asked in three tenses.
        PpThisShotStrip {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: !root.isClosing && !root.reviewingShot
            chips:   root.source ? root.source.thisShot : []
            delta:   root.source ? root.source.afterShotDelta : null
            quiet:   root.source ? root.source.quiet === true : false
            fit:     root.k
            compact: root.compact
        }

        PpReviewShotStrip {
            Layout.fillWidth: true
            Layout.preferredHeight: implicitHeight
            visible: root.reviewingShot
            readout: root.readout
            fit:     root.k
            compact: root.compact
            // The strip bounds ITSELF to the mock's two-row band and scrolls inside it, so this
            // is a backstop rather than the working limit: whatever the strip asks for — a
            // taller cell at a large font scale, the tail opened on a thirty-condition set — it
            // never takes half the panel away from the body it is the preface to.
            maxHeight: Math.round(root.height * 0.45)
        }

        Rectangle {
            objectName: "sdBookends"
            Layout.fillWidth: true
            Layout.preferredHeight: root.px(56)
            visible: root.isClosing && !root.reviewingShot
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

            // ── Forming, and a Closing session the model drew no chain for ───
            Item {
                objectName: "sdCardsBody"
                anchors.fill: parent
                visible: !root.isCold && !root._railBody

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
                        // ...and in review the closing sentence is stated ONCE, along the
                        // bottom, where 13a puts it — saying it here as well would be the
                        // same disclosure at two weights.
                        text: root.header
                              ? (root.header.formingLine
                                 || (root.reviewing ? "" : (root.header.closingLine || "")))
                              : ""
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
                            interactive: root.interactive
                            pulseCue: root.pulseCue
                            onFocusToggled: (id, on) => root._declareFocus(id, on)
                            width: Math.max(0, (cardRow.width - (root._cardsShown - 1) * cardRow.spacing)
                                               / Math.max(1, root._cardsShown))
                            height: cardRow.height
                        }
                    }
                }
            }

            // ── Established, and Closing when there is a rail to draw ────────
            Item {
                objectName: "sdEstablishedBody"
                anchors.fill: parent
                visible: root._railBody

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
                            interactive:       root.interactive
                            pulseCue:          root.pulseCue
                            screenConditionId: root._screenConditionId
                            screenRef:         root._screenRef
                            onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                            onFocusToggled:    (id, on) => root._declareFocus(id, on)
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
                            interactive:       root.interactive
                            pulseCue:          root.pulseCue
                            screenConditionId: root._screenConditionId
                            screenRef:         root._screenRef
                            onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                            onFocusToggled:    (id, on) => root._declareFocus(id, on)
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
                                    interactive:       root.interactive
                                    pulseCue:          root.pulseCue
                                    screenConditionId: root._screenConditionId
                                    screenRef:         root._screenRef
                                    onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                                    onFocusToggled:    (id, on) => root._declareFocus(id, on)
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

        // ── the tense, stated in words ───────────────────────────────────────
        // 13a's footer, and it is not a caption. Everything above it in review is a SESSION
        // total with one wide tick in it, and that is only unambiguous to a reader who has
        // been told the panel does not rewind to what it knew at the selected shot. It is
        // model copy (headerInfo.reviewFootLine), because it names the shot count.
        Text {
            objectName: "sdTenseFooter"
            Layout.fillWidth: true
            Layout.leftMargin: root.px(4)
            Layout.preferredHeight: visible ? implicitHeight : 0
            visible: text !== ""
            text: root.header ? (root.header.reviewFootLine || "") : ""
            elide: Text.ElideRight
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: Theme.colorText3
        }
    }

    // ── the miss picker ──────────────────────────────────────────────────────
    //
    // A LOCAL, PLAIN-QtQuick POPUP and deliberately not the model browser's ModelCausePicker,
    // whose idiom this borrows: that one lives in a different module, carries a search field,
    // a create-a-new-condition row and a legality contract, and importing it here would drag
    // the whole authoring surface onto a panel that is not an editor. What is needed is a list
    // of the outcomes the pack already authors, which is a list.
    //
    // THE CANDIDATES ARE THE MODEL'S — SessionDiagnosticsModel::missCandidates(), which asks
    // the pack's ConditionKind and not this file's idea of what a miss looks like. A body that
    // kept its own list of "slice, hook, thin, fat" would be authoring content in the QML, and
    // it would go stale the first time the pack gained an outcome.
    Item {
        id: missPicker
        objectName: "sdMissPicker"
        anchors.fill: parent
        visible: _open
        z: 100

        property bool _open: false
        property var  _rows: []
        property real _anchorX: 0
        property real _anchorY: 0

        function openBelow(item) {
            _rows = root._missCandidates()
            const p = item.mapToItem(missPicker, 0, item.height)
            _anchorX = p.x
            _anchorY = p.y + root.px(6)
            _open = true
        }
        function close() { _open = false }

        // Anywhere else is "not now". No modal veil: the panel behind it is still the answer
        // to a different question and dimming it would say the picker had taken the surface
        // over, which it has not.
        //
        // A MouseArea rather than a TapHandler, here and on the sheet below, and the reason is
        // the dismiss: TapHandler's default gesture policy takes a PASSIVE grab, so a scrim
        // handler and a row handler both fire on the same press and every click inside the
        // sheet would also be a click outside it. MouseArea accepts the event and stops it.
        MouseArea { anchors.fill: parent; onClicked: missPicker.close() }

        Rectangle {
            objectName: "sdMissPickerSheet"
            // Kept inside the panel on both axes — the header chip can sit near the right-hand
            // edge in the narrow arrangement, and a sheet half off the panel is clipped away
            // by root's own `clip` rather than drawn outside it.
            x: Math.max(root.px(6), Math.min(missPicker._anchorX,
                                             missPicker.width - width - root.px(6)))
            y: Math.max(root.px(6), Math.min(missPicker._anchorY,
                                             missPicker.height - height - root.px(6)))
            width: root.px(260)
            // SIZED FROM THE ROW COUNT, not from the ListView's contentHeight: the list's own
            // height comes from this one through the anchors, so reading contentHeight back
            // would be a loop the engine breaks by leaving the sheet at whatever it had — a
            // three-row list showing one row.
            readonly property int _rowH: root.px(24)
            height: Math.min(root.px(300),
                             head.height
                             + Math.max(1, missPicker._rows.length) * _rowH
                             + (clearRow.visible ? clearRow.height : 0)
                             + 2 * root.px(6))
            color: Theme.colorSurface
            radius: Theme.radius
            border.width: 1
            border.color: Theme.colorAccent
            clip: true

            // The sheet eats its own clicks, so a tap on its background is not also a tap on
            // the scrim. FIRST child, so the rows above it get theirs first.
            MouseArea { anchors.fill: parent }

            Text {
                id: head
                objectName: "sdMissPickerLabel"
                anchors { left: parent.left; right: parent.right; top: parent.top }
                anchors.margins: root.px(9)
                height: implicitHeight + root.px(6)
                // WHAT THE DECLARATION IS FOR, said where it is made. It pre-arms the chains
                // upstream of the outcome; it is not a filter and it is not evidence (§A6).
                text: qsTr("WHAT IS THE BAD SHOT?")
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }

            ListView {
                id: list
                objectName: "sdMissPickerList"
                anchors { left: parent.left; right: parent.right; top: head.bottom
                          bottom: clearRow.visible ? clearRow.top : parent.bottom }
                anchors.leftMargin:  root.px(4)
                anchors.rightMargin: root.px(4)
                anchors.bottomMargin: root.px(4)
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: missPicker._rows

                delegate: Rectangle {
                    required property var modelData
                    objectName: "sdMissCandidate"
                    width: list.width
                    height: list.parent._rowH
                    radius: Theme.radius
                    readonly property bool _isCurrent: modelData.id === root.declaredMiss
                    color: _isCurrent ? Theme.colorAccentLight
                                      : (rowMouse.containsMouse ? Theme.colorBg3 : "transparent")

                    Text {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.leftMargin:  root.px(7)
                        anchors.rightMargin: root.px(7)
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.name || modelData.id || ""
                        elide: Text.ElideRight
                        font.family: Theme.fontBody
                        font.pixelSize: root.tzLabel
                        font.weight: Theme.fontBodyWeight
                        color: parent._isCurrent ? Theme.colorAccent : Theme.colorText
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._declareMiss(parent.modelData.id || "")
                    }
                }
            }

            // Withdrawing is declaring nothing, and it is the same call with an empty id — the
            // model treats "" as a clear, so there is no second verb to keep in step.
            Item {
                id: clearRow
                objectName: "sdMissClear"
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height: visible ? root.px(26) : 0
                visible: root.declaredMiss !== ""

                Rectangle {
                    anchors { left: parent.left; right: parent.right; top: parent.top }
                    anchors.leftMargin:  root.px(9)
                    anchors.rightMargin: root.px(9)
                    height: 1
                    color: Theme.colorBorderMid
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: root.px(11)
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("NO DECLARED MISS")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingLabel
                    color: clearMouse.containsMouse ? Theme.colorText : Theme.colorText3
                }
                MouseArea {
                    id: clearMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root._declareMiss("")
                }
            }
        }
    }
}
