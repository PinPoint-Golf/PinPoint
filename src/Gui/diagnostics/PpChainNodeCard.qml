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

// ONE NODE ON THE CHAIN RAIL, in whichever of the four things a node can be.
//
// WHY THIS IS NOT PpPatternCard IN A COMPACT MODE. It was written that way first and the
// two cards turned out to share four fields out of nine. A chain node publishes `mark`,
// `phase` and `measure` and publishes NO `fresh`, NO `directionText`, NO `recencyText` and
// NO `trendText`; a Forming card publishes exactly the opposite set. Reusing the Forming
// card would have meant a phase·measure line it has no data for, three tags bound to
// undefined, and — the part that decided it — a `kind` switch inside a file whose whole job
// is to draw the ONE kind the Forming row has. Three of the four kinds here are not cards at
// all: a ghost is an outline with a name, a screened root is a call to action, an outcome is
// the miss the golfer declared. They belong together because they are alternatives to each
// other, and they do not belong inside the card that has none of them.
//
// THE FOUR MARKS ARE THE HONESTY DEVICE and they are the model's words, never composed here
// (brief §4, §5.4). A screened root is drawn as a REQUEST — attention-tinted, with the screen
// that would settle it — and never as a finding, because until the screen is entered it is
// not one. A ghost is dashed and dimmed for the same reason the Cold expectations are: it is
// not evidence from this session. Neither ever borrows the live card's surface.

import QtQuick
import PinPointStudio

Item {
    id: root

    // One entry of a SessionDiagnosticsModel::chains() entry's `nodes`.
    property var node: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // 12c's one-line collapse: mark dot, name, recurrence, ledger. Everything else goes.
    property bool slim: false

    // The screened root's CTA. The ref is the model's when it recommended this screen and
    // empty when it did not; the condition id is always the thing the screen would settle.
    signal screenRequested(string screenRef, string conditionId)

    // Set by the panel from driver.screenConditionId/screenRef — the only place a screen ref
    // for this node exists in the published surface.
    property string screenRef: ""

    // Off on the auto-closing cast (PpSessionDiagnosticsWindow.interactive).
    property bool interactive: true

    // ── the focus contract (design §A6) ──────────────────────────────────────
    // Offered on LIVE nodes only, and that is not a layout decision: declaring focus splits
    // the session at the current shot so a link can earn Moved-together, and a node this
    // capture cannot measure has nothing to split. A ghost, a screened root and the declared
    // outcome are all things the panel is reporting ABOUT rather than things a session can be
    // run at. The read-back is the model's `focused`, exactly as on the pattern card.
    readonly property bool focused: node ? node.focused === true : false
    readonly property bool focusable: interactive && isLive && !!node
    signal focusToggled(string conditionId, bool nowFocused)

    // ── the after-shot pulse (§B3, §B8) ──────────────────────────────────────
    // { token, ids, focusId }, republished by the body on every shotIngested. See the twin
    // block on PpPatternCard — one mechanism, because the two cards are the same claim drawn
    // in two arrangements and a rail that pulsed differently would read as a second event.
    property var pulseCue: null
    readonly property bool pulsing: pulseSeq.running
    property real _pulseT: 0.0

    objectName: "sdChainNode"

    // The mock's node is `overflow:hidden` and so is this: a rail row shorter than the design
    // cuts the tail of the evidence prose rather than pushing a node past its neighbour.
    clip: true

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro  * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel  * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2  * fit))
    readonly property int tzHead:    Math.max(1, Math.round(Theme.fontSzHeading * fit))

    readonly property string kind:  node ? (node.kind || "live") : "live"
    readonly property bool isLive:   kind === "live"
    readonly property bool isGhost:  kind === "ghost"
    readonly property bool isScreen: kind === "screenedRoot"
    readonly property bool isOutcome: kind === "outcome"

    readonly property string _state: node ? (node.state || "") : ""
    readonly property color _stateColor: _state === "fired" ? Theme.colorError
                                       : _state === "clean" ? Theme.colorGood
                                                            : Theme.colorText3
    readonly property color _pillFill: _state === "fired" ? Theme.colorErrorLight
                                     : _state === "clean" ? Theme.colorGoodLight
                                                          : "transparent"
    readonly property color _trendColor: !node ? Theme.colorText3
                                       : node.trend === "worsening" ? Theme.colorError
                                       : node.trend === "improving" ? Theme.colorGood
                                                                    : Theme.colorText3

    // The dot that carries the node's kind through 12c's collapse: the state colour on a live
    // card, and the kind's own colour where there is no state to report.
    readonly property color markColor: isScreen ? Theme.colorAttention
                                     : isOutcome ? Theme.colorError
                                     : isGhost ? Theme.colorText3
                                               : _stateColor

    // The mock's tints are .06 fill / .35 border on the screened root and .06 / .30 on the
    // outcome. The Light tokens are the fills; the borders are the same hue at the mock's
    // alpha, which no token carries because no other surface asks for one.
    readonly property color _fill: isScreen ? Theme.colorAttentionLight
                                 : isOutcome ? Theme.colorErrorLight
                                 : isGhost ? "transparent"
                                           : Theme.colorSurface
    readonly property color _stroke: isScreen
                                     ? Qt.rgba(Theme.colorAttention.r, Theme.colorAttention.g,
                                               Theme.colorAttention.b, 0.35)
                                     : isOutcome
                                       ? Qt.rgba(Theme.colorError.r, Theme.colorError.g,
                                                 Theme.colorError.b, 0.30)
                                       : Theme.colorBorderMid

    // A ghost is not evidence from this session, and the dash plus the dimming is the whole
    // of how that is said (brief §4, and the same device as the Cold expectations).
    opacity: isGhost ? 0.62 : 1.0

    // What the node needs when it is allowed to size itself — the wide rail centres a ghost
    // rather than stretching it, because a stretched outline reads as a card with nothing in
    // it rather than a node nobody measured.
    readonly property int contentHeight: slim ? px(36)
                                              : Math.round(wide.implicitHeight + 2 * px(9))
    implicitHeight: contentHeight

    // ── the pulse ────────────────────────────────────────────────────────────
    // One pass, opacity and border alpha only. reduceMotion is no pulse and no stagger, not a
    // faster one — see PpPatternCard.
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
        const id  = root.node ? (root.node.id || "") : ""
        if (!cue || !cue.ids || id === "") return
        const slot = cue.ids.indexOf(id)
        if (slot < 0) return          // this condition did not change on this swing
        if (Theme.reduceMotion) return
        pulseWait.duration = slot * 70
        pulseSeq.start()
    }

    // ── the frame ────────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        visible: !root.isGhost
        color: root._fill
        radius: Theme.radius
        border.width: 1
        // The focused node's marker is the headline of the moment (§B3): the accent replaces
        // the resting stroke while focus is declared, and the pulse lifts it on a shot that
        // fired here. Unfocused nodes fade the accent in over their own stroke and back out.
        border.color: root.focused
                      ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g, Theme.colorAccent.b,
                                0.45 + 0.55 * root._pulseT)
                      : (root._pulseT > 0
                         ? Qt.rgba(Theme.colorAccent.r, Theme.colorAccent.g,
                                   Theme.colorAccent.b, root._pulseT)
                         : root._stroke)
    }

    // ── the focus bar ────────────────────────────────────────────────────────
    Rectangle {
        objectName: "sdChainNodeFocusBar"
        visible: root.focused
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        anchors.margins: 1
        width: Math.max(1, root.px(3))
        radius: width / 2
        color: Theme.colorAccent
        opacity: 0.7 + 0.3 * root._pulseT
    }
    PpDashedFrame {
        objectName: "sdChainGhostFrame"
        anchors.fill: parent
        visible: root.isGhost
        frameRadius: Theme.radius
        strokeColor: Theme.colorBorderMid
        dashOn:  Math.max(1, root.px(3))
        dashOff: Math.max(1, root.px(3))
    }

    // ── the wide rail's node ─────────────────────────────────────────────────
    Column {
        id: wide
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin:  root.px(10)
        anchors.rightMargin: root.px(10)
        // A live card fills top-down; the three that are one thought are centred, as the
        // mock centres them.
        anchors.top: root.isLive ? parent.top : undefined
        anchors.topMargin: root.isLive ? root.px(9) : 0
        anchors.verticalCenter: root.isLive ? undefined : parent.verticalCenter
        visible: !root.slim
        spacing: root.px(4)

        // ── name · state pill ────────────────────────────────────────────────
        Item {
            width: wide.width
            height: nodeName.implicitHeight

            Text {
                id: nodeName
                objectName: "sdChainNodeName"
                anchors.left: parent.left
                anchors.right: pill.visible ? pill.left : parent.right
                anchors.rightMargin: pill.visible ? root.px(7) : 0
                anchors.verticalCenter: parent.verticalCenter
                text: root.node ? (root.node.name || "") : ""
                // The outcome node is the miss the golfer declared and is the largest thing
                // on the rail because it is what the whole chain is about.
                wrapMode: root.isLive ? Text.NoWrap : Text.WordWrap
                elide: root.isLive ? Text.ElideRight : Text.ElideNone
                font.family: Theme.fontBody
                font.pixelSize: root.isOutcome ? root.tzHead : root.tzBody
                font.weight: Theme.fontBodyWeight
                color: root.isGhost ? Theme.colorText2 : Theme.colorText
            }
            Rectangle {
                id: pill
                objectName: "sdChainNodePill"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.isLive && pillText.text !== ""
                width:  pillText.implicitWidth + root.px(12)
                height: pillText.implicitHeight + root.px(2)
                radius: Math.max(1, root.px(2))
                color: root._pillFill
                border.width: root._state === "notAssessable" ? 1 : 0
                border.color: Theme.colorBorder

                Text {
                    id: pillText
                    anchors.centerIn: parent
                    text: root.node ? (root.node.statePill || "") : ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingLabel
                    color: root._stateColor
                }
            }
        }

        // ── the mark: what KIND of node this is, in the model's words ────────
        Text {
            objectName: "sdChainNodeMark"
            width: wide.width
            visible: !root.isLive && text !== ""
            text: root.node ? (root.node.mark || "") : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: root.isScreen ? Theme.colorAttention
                 : root.isOutcome ? Theme.colorText2
                                  : Theme.colorText3
        }

        // ── phase · measure: what the reading was, and where in the swing ────
        Text {
            objectName: "sdChainNodePhase"
            width: wide.width
            visible: root.isLive && text !== ""
            text: {
                if (!root.node) return ""
                const p = root.node.phase || "", m = root.node.measure || ""
                return (p !== "" && m !== "") ? (p + " · " + m) : (p + m)
            }
            elide: Text.ElideRight
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            font.letterSpacing: Theme.trackingLabel
            color: Theme.colorText3
        }

        // ── recurrence: a count over assessable shots, never a percentage ────
        Text {
            objectName: "sdChainNodeRecurrence"
            width: wide.width
            visible: !root.isGhost && text !== ""
            text: root.node ? (root.node.recurrence || "") : ""
            elide: Text.ElideRight
            font.family: Theme.fontData
            font.pixelSize: root.tzLabel
            color: root.isOutcome ? Theme.colorError : Theme.colorText
        }

        // ── the run ──────────────────────────────────────────────────────────
        PpTickRun {
            objectName: "sdChainNodeRun"
            width: wide.width
            visible: root.node && root.node.ticks && root.node.ticks.length > 0
            ticks: root.node ? root.node.ticks : []
            fit: root.fit
        }

        // ── trend, and — in review — where the shot sits in the run ──────────
        // The recency line is REVIEW-ONLY on a node (the model publishes firingsAfterText
        // while reviewing or closed and not otherwise), and it is the sentence that makes the
        // wide tick above it mean something: "3 more firings after this shot" is the answer to
        // "was the swing I picked the end of it, or the middle".
        Item {
            width: wide.width
            height: root.isLive
                    ? Math.max(trendText.implicitHeight, focusTag.implicitHeight) : 0
            visible: root.isLive

            Text {
                id: trendText
                objectName: "sdChainNodeTrend"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                text: root.node ? ((root.node.trendArrow || "") + (root.node.trend || "")) : ""
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                color: root._trendColor
            }
            Text {
                id: firingsAfter
                objectName: "sdChainNodeRecency"
                anchors.left: trendText.right
                anchors.leftMargin: trendText.visible ? root.px(7) : 0
                anchors.right: focusTag.visible ? focusTag.left : parent.right
                anchors.rightMargin: focusTag.visible ? root.px(7) : 0
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                text: root.node ? (root.node.firingsAfterText || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
            // Same micro-label as the pattern card's, in the same words: the two surfaces are
            // two arrangements of one contract and a reader must not have to learn it twice.
            Text {
                id: focusTag
                objectName: "sdChainNodeFocusTag"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: root.focusable
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
        // Dropped, not shrunk, when there is no room for it: an evidence sentence clipped
        // mid-clause reads as a different claim than the one the model made.
        Text {
            id: evidence
            objectName: "sdChainNodeEvidence"
            width: wide.width
            height: root.isLive
                    ? Math.max(0, root.height - root.px(9) - y - (cta.visible ? cta.height + wide.spacing : 0))
                    : implicitHeight
            visible: !root.isGhost && text !== "" && height >= root.tzMicro
            text: root.node ? (root.node.evidence || "") : ""
            wrapMode: Text.WordWrap
            elide: Text.ElideRight
            lineHeight: 1.45
            font.family: Theme.fontBody
            font.pixelSize: root.tzMicro
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText2
        }

        // ── the screened root's call to action ───────────────────────────────
        // The highest-value output of the whole model and the only one that costs no
        // hardware. It is on the NODE as well as in the footer because this is where the
        // golfer is looking when they ask what the dashed half of the rail is for.
        Text {
            id: cta
            objectName: "sdChainScreenCta"
            width: wide.width
            visible: root.isScreen
            text: qsTr("RUN THE SCREEN ▸")
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            font.letterSpacing: Theme.trackingLabel
            color: Theme.colorAttention
        }
    }

    // ── 12c: one line ────────────────────────────────────────────────────────
    // Declared after the wide form on purpose: the two carry the same objectNames, so tree
    // order decides which one a reader finds first, and the wide form is the one every size
    // above 640 px is drawing.
    Row {
        anchors.fill: parent
        anchors.leftMargin:  root.px(9)
        anchors.rightMargin: root.px(9)
        visible: root.slim
        spacing: root.px(7)

        Rectangle {
            objectName: "sdChainNodeDot"
            anchors.verticalCenter: parent.verticalCenter
            width:  root.px(5)
            height: root.px(5)
            radius: width / 2
            color: root.markColor
        }
        Text {
            objectName: "sdChainNodeName"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, parent.width - root.px(5) - slimRecur.width - slimRun.width
                               - 3 * parent.spacing)
            text: root.node ? (root.node.name || "") : ""
            elide: Text.ElideRight
            font.family: Theme.fontBody
            font.pixelSize: root.tzLabel
            font.weight: Theme.fontBodyWeight
            color: root.isGhost ? Theme.colorText2 : Theme.colorText
        }
        Text {
            id: slimRecur
            objectName: "sdChainNodeRecurrence"
            anchors.verticalCenter: parent.verticalCenter
            // The screened root has no recurrence to report and its CTA is the point of it
            // being on the rail at all, so that is what takes the slot.
            text: root.isScreen ? qsTr("RUN THE SCREEN ▸")
                                : (root.node ? (root.node.recurrence || "") : "")
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: root.isScreen ? Theme.colorAttention : Theme.colorText2
        }
        // A fixed slot, not the run's own implicitWidth — PpTickRun derives its pitch FROM its
        // width, so binding the width back to the implicit one is a loop. It squeezes the
        // pitch to fit rather than dropping ticks, which is the behaviour wanted here.
        PpTickRun {
            id: slimRun
            objectName: "sdChainNodeRun"
            anchors.verticalCenter: parent.verticalCenter
            width: visible ? root.px(56) : 0
            visible: root.node && root.node.ticks && root.node.ticks.length > 0
            ticks: root.node ? root.node.ticks : []
            fit: root.fit * 0.8
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.isScreen
        onClicked: root.screenRequested(root.screenRef,
                                        root.node ? (root.node.id || "") : "")
    }

    // The other kind of tap this node takes, and the two can never collide: a screened root
    // is not live and a live node is not screened.
    HoverHandler { id: focusHover; enabled: root.focusable; cursorShape: Qt.PointingHandCursor }
    TapHandler {
        enabled: root.focusable
        onTapped: root.focusToggled(root.node.id || "", !root.focused)
    }
}
