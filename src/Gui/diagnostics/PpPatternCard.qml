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

// One pattern, as the session has it so far. Brief §4.2, top to bottom: name, the fresh
// tag, the state pill for THIS shot, the recurrence count, direction-or-dispersion, the
// run of ticks, trend + recency, one line of evidence prose.
//
// EVERY STRING IS THE MODEL'S. `recurrence` is a count over assessable shots and never a
// percentage; `directionText` is either the direction claim or the sentence saying why the
// claim is withheld, and this file cannot tell which and does not need to. The one thing
// decided here is which of the two lines gets dropped when the card is short — the evidence
// prose, per 12c ("cards lose their evidence sentence") — because that is a fact about the
// space, not about the evidence.

import QtQuick
import PinPointStudio

Rectangle {
    id: root

    // One entry of SessionDiagnosticsModel::cards().
    property var card: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0

    // Off on the auto-closing cast, where arming a tap on something about to vanish under the
    // pointer is a trap (PpSessionDiagnosticsWindow.interactive).
    property bool interactive: true

    // ── the focus contract (design §A6) ──────────────────────────────────────
    //
    // DESIGN-REVIEW: the affordance is MINIMAL by intent — a micro-label and a tap on the
    // whole card. What has not been designed is the moment AFTER declaring: the split point
    // is invisible on the run (the ticks before and after the declaration look identical),
    // and there is no way to see what the contract has bought yet beyond a link quietly
    // becoming Moved-together further down the rail. Both want a design pass before this is
    // called finished.
    //
    // THE CARD ASKS AND THE MODEL DECIDES. Tapping publishes the request; declareFocus() /
    // clearFocus() live on SessionDiagnosticsModel and this file cannot reach one. `focused`
    // is read back off the model's own card map, so a tap that the model declined to honour
    // leaves the card exactly as it was — which is the only arrangement in which the accent
    // bar is a statement about the ledger rather than about a click.
    //
    // WHAT DECLARING FOCUS DOES, so the affordance is not mistaken for a filter: it records a
    // split at the current shot count, which is what puts the Moved-together link grade on
    // offer at all. It shapes ATTENTION and EXPERIMENT STRUCTURE. It is not an input to
    // detection, to a corridor or to a tier, and the model is what enforces that.
    readonly property bool focused: card ? card.focused === true : false
    signal focusToggled(string conditionId, bool nowFocused)

    // ── the after-shot pulse (§B3, §B8) ──────────────────────────────────────
    //
    // A CUE, not a channel: { token, ids, focusId }, republished by the body on every
    // shotIngested. One property so the ids can never arrive after the token that is supposed
    // to describe them. A card whose id is not in `ids` did not change on this swing and does
    // not move.
    property var pulseCue: null
    // The animation's own answer, so a test can assert that reduceMotion left nothing running
    // rather than assert on a duration constant.
    readonly property bool pulsing: pulseSeq.running
    property real _pulseT: 0.0

    // Design pixels through the app's type scale and the panel's fit — the panel's own
    // px(), repeated here so the card is usable on its own.
    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro  * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel  * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2  * fit))
    readonly property int tzData:    Math.max(1, Math.round(Theme.fontSzDataSm * fit))

    readonly property string _pillState: card ? (card.thisShot || "") : ""
    readonly property color _pillColor: _pillState === "fired" ? Theme.colorError
                                      : _pillState === "clean" ? Theme.colorGood
                                                               : Theme.colorText3
    readonly property color _pillFill: _pillState === "fired" ? Theme.colorErrorLight
                                     : _pillState === "clean" ? Theme.colorGoodLight
                                                              : "transparent"
    readonly property color _trendColor: !card ? Theme.colorText3
                                       : card.trend === "worsening" ? Theme.colorError
                                       : card.trend === "improving" ? Theme.colorGood
                                                                    : Theme.colorText3

    objectName: "sdPatternCard"

    color: Theme.colorSurface
    radius: Theme.radius
    border.width: 1
    border.color: Theme.colorBorderMid
    clip: true

    // The accent border, drawn OVER the resting one rather than replacing it: a Rectangle has
    // exactly one border and this card needs two states of it that cross-fade. Focused holds
    // the accent; the pulse lifts it on a shot that fired this condition. The two share one
    // channel on purpose — a focused card that fired is the headline of the moment (§B3) and
    // should not need two marks to say so.
    Rectangle {
        objectName: "sdCardAccentBorder"
        anchors.fill: parent
        color: "transparent"
        radius: parent.radius
        border.width: 1
        border.color: Theme.colorAccent
        opacity: root.focused ? 0.45 + 0.55 * root._pulseT : root._pulseT
        visible: opacity > 0
    }

    // ── the focus bar ────────────────────────────────────────────────────────
    // A left edge, because it is a mark ON the card rather than a thing in it — and because
    // the card's own content is already the full width of what it has to say.
    Rectangle {
        objectName: "sdCardFocusBar"
        visible: root.focused
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        anchors.margins: 1
        width: Math.max(1, root.px(3))
        radius: width / 2
        color: Theme.colorAccent
        opacity: 0.7 + 0.3 * root._pulseT
    }

    // ── the pulse ────────────────────────────────────────────────────────────
    //
    // ONE PASS, NOTHING MOVES. Opacity and border alpha only: no size, no position, and
    // emphatically no reordering — the display order is hysteretic and model-side, and a card
    // that slid to a new seat while it flashed would be reporting a rank change the ledger
    // did not make.
    //
    // reduceMotion IS NOT A SHORTER PULSE, it is no pulse: the stagger goes too, so nothing
    // is waiting to happen either. The card still redraws with its new pill, its new tick and
    // its NEW tag — the state change was never carried by the animation.
    SequentialAnimation {
        id: pulseSeq
        PauseAnimation { id: pulseWait; duration: 0 }
        NumberAnimation { target: root; property: "_pulseT"; to: 1.0
                          duration: Theme.durationFast; easing.type: Easing.OutQuad }
        NumberAnimation { target: root; property: "_pulseT"; to: 0.0
                          duration: Theme.durationSlow; easing.type: Easing.InQuad }
    }

    // A CARD CREATED AFTER THE SHOT MUST NOT REPLAY IT. The cue is a property, so a delegate
    // built later — a resize that changed how many fit, a stage that swapped the body — binds
    // to whatever cue is current and would flash for a swing it was not there for.
    property bool _ready: false
    Component.onCompleted: root._ready = true

    onPulseCueChanged: {
        if (!root._ready) return
        // STOPPED FIRST, ALWAYS. A new shot has landed, so whatever this card was saying about
        // the last one is over — including when this card is not in the new cue at all. A card
        // left flashing from the previous swing would be pointing at the wrong ball.
        pulseSeq.stop()
        root._pulseT = 0.0
        const cue = root.pulseCue
        const id  = root.card ? (root.card.id || "") : ""
        if (!cue || !cue.ids || id === "") return
        const slot = cue.ids.indexOf(id)
        if (slot < 0) return          // this condition did not change on this swing
        if (Theme.reduceMotion) return
        // Stagger in CARD ORDER, which is the model's order — the eye reads the change down
        // the row the way the swing runs rather than all at once.
        pulseWait.duration = slot * 70
        pulseSeq.start()
    }

    Column {
        id: col
        anchors.fill: parent
        anchors.leftMargin:   root.px(11)
        anchors.rightMargin:  root.px(11)
        anchors.topMargin:    root.px(9)
        anchors.bottomMargin: root.px(9)
        spacing: root.px(5)

        // ── name · NEW · state pill ──────────────────────────────────────────
        Item {
            width: col.width
            height: nameText.implicitHeight

            Text {
                id: nameText
                objectName: "sdCardName"
                anchors.left: parent.left
                // Past the NEW tag only while there is one — collapsing the tag's own width
                // instead would make its implicitWidth depend on its width.
                anchors.right: freshTag.visible ? freshTag.left : pill.left
                anchors.rightMargin: root.px(7)
                anchors.verticalCenter: parent.verticalCenter
                text: root.card ? root.card.name : ""
                elide: Text.ElideRight
                font.family: Theme.fontBody
                font.pixelSize: root.tzBody
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }
            Text {
                id: freshTag
                objectName: "sdCardFresh"
                anchors.right: pill.left
                anchors.rightMargin: root.px(7)
                anchors.verticalCenter: parent.verticalCenter
                visible: root.card ? root.card.fresh === true : false
                text: qsTr("NEW")
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorAccent
            }
            Rectangle {
                id: pill
                objectName: "sdStatePill"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width:  pillText.implicitWidth + root.px(12)
                height: pillText.implicitHeight + root.px(2)
                radius: Math.max(1, root.px(2))
                color: root._pillFill
                border.width: root._pillState === "notAssessable" ? 1 : 0
                border.color: Theme.colorBorder

                Text {
                    id: pillText
                    anchors.centerIn: parent
                    text: root.card ? (root.card.statePill || "") : ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingLabel
                    color: root._pillColor
                }
            }
        }

        // ── recurrence: a count over assessable shots, and the largest thing on the card
        //    because it is the thing being asserted (12b's note, which holds at every size).
        Text {
            objectName: "sdCardRecurrence"
            width: col.width
            text: root.card ? (root.card.recurrence || "") : ""
            elide: Text.ElideRight
            font.family: Theme.fontData
            font.pixelSize: root.tzData
            color: Theme.colorText
        }

        // ── direction, or the sentence saying the direction claim is withheld ─
        //
        // THE FIRST THING TO GO WHEN THE CARD IS SHORT, and it goes before the run does. What
        // is under this line is the ledger — the run, and what happened after the reviewed
        // shot — and prose must not push evidence off the bottom of its own card. Dropped
        // whole, never half-drawn: a tick run cut through the middle reads as a run of short
        // ticks, which is a claim about the session that the ledger never made.
        Text {
            id: directionTxt
            objectName: "sdCardDirection"
            width: col.width
            text: root.card ? (root.card.directionText || "") : ""
            visible: text !== ""
                     && col.height - y - height
                        >= run.height + trendRow.height + 2 * col.spacing
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.family: Theme.fontBody
            font.pixelSize: root.tzLabel
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText2
        }

        // ── the run ──────────────────────────────────────────────────────────
        // Last to go, and only when the card cannot hold it whole.
        PpTickRun {
            id: run
            objectName: "sdTickRun"
            width: col.width
            visible: col.height - y >= height
            ticks: root.card ? root.card.ticks : []
            fit: root.fit
        }

        // ── trend + recency, and the focus affordance ────────────────────────
        Item {
            id: trendRow
            width: col.width
            // Never above the run it annotates: a trend arrow with no run under it is a claim
            // with its evidence removed.
            visible: run.visible && col.height - y >= height
            height: Math.max(trendTxt.implicitHeight, focusTag.implicitHeight)

            Text {
                id: trendTxt
                objectName: "sdCardTrend"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.card ? ((root.card.trendArrow || "") + (root.card.trendText || "")) : ""
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                color: root._trendColor
            }
            // IN REVIEW THE RECENCY SLOT CHANGES TENSE, not position. "last fired 2 measurable
            // shots ago" is a statement about now and means nothing on a finished session;
            // what the reader is asking is where the swing they picked sits in the run, which
            // is "3 more firings after this shot". The model publishes firingsAfterText only
            // while reviewing or closed, so its presence IS the tense.
            Text {
                objectName: "sdCardRecency"
                anchors.left: trendTxt.right
                anchors.leftMargin: root.px(8)
                anchors.right: focusTag.visible ? focusTag.left : parent.right
                anchors.rightMargin: focusTag.visible ? root.px(8) : 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.card ? (root.card.firingsAfterText || root.card.recencyText || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
            // The affordance, in the house micro-label style — a caret invites, a middot
            // reports. It is here rather than as a hover-only reveal because a contract the
            // golfer cannot see is a contract they will never declare, and nothing else on
            // this panel is discoverable by hovering.
            Text {
                id: focusTag
                objectName: "sdCardFocusTag"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.interactive && !!root.card
                text: root.focused ? qsTr("FOCUSED ·") : qsTr("FOCUS ▸")
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingLabel
                color: root.focused ? Theme.colorAccent
                                    : (focusHover.hovered ? Theme.colorAccent : Theme.colorText3)
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }
        }

        // ── one line of evidence prose ───────────────────────────────────────
        // Dropped, not shrunk, when the card is too short to hold it: an evidence sentence
        // clipped mid-clause reads as a different claim than the one the model made.
        Text {
            id: evidence
            objectName: "sdCardEvidence"
            width: col.width
            height: Math.max(0, col.height - y)
            visible: trendRow.visible && height >= root.tzMicro * lineHeight
            text: root.card ? (root.card.evidence || "") : ""
            wrapMode: Text.WordWrap
            elide: Text.ElideRight
            lineHeight: 1.4
            font.family: Theme.fontBody
            font.pixelSize: root.tzMicro
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText2
        }
    }

    // THE WHOLE CARD IS THE TARGET, not the six characters of the tag: the tag says the card
    // is tappable and the card is what the thumb lands on. Declared last so it sits over the
    // content; nothing else on a pattern card takes a click.
    HoverHandler { id: focusHover; enabled: root.interactive; cursorShape: Qt.PointingHandCursor }
    TapHandler {
        enabled: root.interactive && !!root.card
        onTapped: root.focusToggled(root.card.id || "", !root.focused)
    }
}
