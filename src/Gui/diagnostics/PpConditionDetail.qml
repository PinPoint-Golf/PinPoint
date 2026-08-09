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

// ONE CONDITION, DRILLED INTO: what might have caused it, and what it most likely leads to.
//
// DESIGN-REVIEW, AND SHIPPED MINIMAL ON PURPOSE. Brief §9 lists "tapping a chain node through" as
// not designed; this is a user request answered with the panel's EXISTING vocabulary and nothing
// invented — the same pattern card, the same node cards, the same graded link strokes, the same
// captions, in the same chrome. Every string on it is the model's, exactly as everywhere else on
// this panel. It wants a design pass before it is called finished; see
// docs/design/session_diagnostics_build_findings.md.
//
// IT IS A BODY SWAP, NOT A SCREEN. The composition behind it is hidden rather than torn down, so
// coming back is a visibility change and the panel the golfer left is the panel they return to —
// the watching row still expanded, the chain still open, every card still where it was. Opening a
// DIFFERENT condition from a cause or effect node re-targets this page in place; BACK still
// returns to the panel in one step. There is deliberately no navigation stack: one step back is
// the whole of the model, and a stack would be a second place for "where am I" to be wrong.
//
// WHAT IT DOES NOT DO. It runs no screen (brief §9), takes no cadence decision, and cannot move a
// ratchet or a tier — conditionDetail() is a pure read and this file is a pure draw of it.

import QtQuick
import PinPointStudio

Item {
    id: root

    // SessionDiagnosticsModel::detail — { id, name, header, causeHeadline, outcomeHeadline,
    // causes, effects, rivals, noCausesLine, noEffectsLine }.
    property var detail: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // 12c's reductions: the rails turn on their side, the captions thin out.
    property bool compact: false
    // Off on the auto-closing cast (PpSessionDiagnosticsWindow.interactive).
    property bool interactive: true

    // driver.screenConditionId / screenRef, passed through so a screened root on a cause rail
    // keeps the CTA it has on the panel's own rail.
    property string screenConditionId: ""
    property string screenRef: ""

    // A cause or effect node was tapped: RE-TARGET this page. The model decides whether the id is
    // one it can answer, exactly as it decides a focus declaration.
    signal conditionActivated(string conditionId)
    signal focusToggled(string conditionId, bool nowFocused)
    signal screenRequested(string screenRef, string conditionId)
    // Esc, and the header's BACK chip next door.
    signal closeRequested()

    objectName: "sdDetail"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2 * fit))

    readonly property var header:  detail ? detail.header  : null
    readonly property var causes:  detail ? (detail.causes  || []) : []
    readonly property var effects: detail ? (detail.effects || []) : []

    // Which non-primary path is open, per side. Panel-local, and the same shape as the body's
    // `expandedChain`: one open at a time, opening into THE SAME rail rather than another view.
    property int expandedCause: -1
    property int expandedEffect: -1

    // Esc closes. `where focus allows` — the panel is not modal and does not steal focus from the
    // stage, so this fires when the detail happens to hold it and the BACK chip is the affordance
    // that always works.
    focus: visible
    Keys.onEscapePressed: root.closeRequested()
    onVisibleChanged: if (visible) forceActiveFocus()
    // A page that re-targets must not keep the previous condition's expansions.
    onDetailChanged: { root.expandedCause = -1; root.expandedEffect = -1 }

    // The rail heights: the design's own node row wide, and whatever the vertical form asks for
    // in the 396 arrangement.
    readonly property int _railH: px(132)

    // The collapsed path's one line, and its marks. The arrow and the separator are the only
    // things composed here — every name and every mark is the model's, exactly as in the body's
    // _chainSummary().
    function _pathSummary(rail) {
        if (!rail || !rail.nodes) return ""
        const parts = []
        for (let i = 0; i < rail.nodes.length; ++i)
            parts.push(rail.nodes[i].name || "")
        return parts.join(" → ")
    }
    function _pathNote(rail) {
        if (!rail || !rail.nodes) return ""
        const parts = []
        for (let i = 0; i < rail.nodes.length; ++i)
            if (rail.nodes[i].mark)
                parts.push(rail.nodes[i].mark)
        return parts.join(" · ")
    }

    Flickable {
        id: flick
        objectName: "sdDetailFlick"
        anchors.fill: parent
        contentWidth: width
        contentHeight: col.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        Column {
            id: col
            width: flick.width
            spacing: root.px(8)

            // ── the condition itself ─────────────────────────────────────────
            // The panel's own pattern card, unchanged. It is the same claim in the same shape as
            // the card the golfer tapped to get here, which is what makes the drill-in read as
            // the same object opened rather than a second page about it.
            PpPatternCard {
                objectName: "sdDetailHeaderCard"
                width: col.width
                height: root.px(150)
                card: root.header
                fit: root.fit
                interactive: root.interactive
                onFocusToggled: (id, on) => root.focusToggled(id, on)
            }

            // The honesty mark, when this condition is not a live card — a ghost, a screened root
            // or an outcome. The pattern card has no slot for it and must not grow one: it is a
            // statement about what KIND of thing this is, which is the rail's vocabulary.
            Text {
                objectName: "sdDetailMark"
                width: col.width
                visible: text !== ""
                text: root.header ? (root.header.mark || "") : ""
                wrapMode: Text.WordWrap
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }

            // ── causes ───────────────────────────────────────────────────────
            Item {
                width: col.width
                height: causesLabel.implicitHeight

                Text {
                    id: causesLabel
                    anchors.left: parent.left
                    text: qsTr("WHAT MIGHT HAVE CAUSED THIS")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
            }

            Text {
                objectName: "sdDetailCauseHeadline"
                width: col.width
                visible: text !== ""
                text: root.detail ? (root.detail.causeHeadline || "") : ""
                wrapMode: Text.WordWrap
                lineHeight: 1.35
                font.family: Theme.fontBody
                font.pixelSize: root.tzLabel
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }

            Text {
                objectName: "sdDetailNoCauses"
                width: col.width
                visible: text !== ""
                text: root.detail ? (root.detail.noCausesLine || "") : ""
                wrapMode: Text.WordWrap
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }

            Repeater {
                id: causeRails
                model: root.causes

                Column {
                    id: causeRow
                    required property var modelData
                    required property int index
                    width: col.width
                    spacing: root.px(6)

                    readonly property bool primary: modelData && modelData.primary === true
                    readonly property bool open: primary || root.expandedCause === index

                    // The non-primary paths collapse to one line each — the same idiom 12c uses
                    // for chain B, and for the same reason: the summary must not hide what the
                    // path is mostly made of, so the marks ride under it.
                    PpDashedFrame {
                        id: causeFrame
                        objectName: "sdDetailPathFrame"
                        width: parent.width
                        height: causeSummary.implicitHeight + 2 * root.px(8)
                        visible: !causeRow.primary
                        frameRadius: Theme.radius
                        strokeColor: Theme.colorBorderMid
                        dashOn:  Math.max(1, root.px(3))
                        dashOff: Math.max(1, root.px(3))

                        Column {
                            id: causeSummary
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin:  root.px(9)
                            anchors.rightMargin: root.px(9)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: root.px(3)

                            Item {
                                width: parent.width
                                height: causePathLabel.implicitHeight

                                Text {
                                    id: causePathLabel
                                    anchors.left: parent.left
                                    text: qsTr("PATH %1").arg(causeRow.index + 1)
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    font.letterSpacing: Theme.trackingMicro
                                    color: Theme.colorText2
                                }
                                Text {
                                    objectName: "sdDetailPathToggle"
                                    anchors.right: parent.right
                                    anchors.baseline: causePathLabel.baseline
                                    text: causeRow.open ? qsTr("CLOSE ▴") : qsTr("OPEN ▸")
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    font.letterSpacing: Theme.trackingLabel
                                    color: Theme.colorAccent
                                }
                            }
                            Text {
                                objectName: "sdDetailPathSummary"
                                width: parent.width
                                text: root._pathSummary(causeRow.modelData)
                                wrapMode: Text.WordWrap
                                lineHeight: 1.35
                                font.family: Theme.fontBody
                                font.pixelSize: root.tzMicro
                                font.weight: Theme.fontBodyWeight
                                color: Theme.colorText
                            }
                            Text {
                                objectName: "sdDetailPathNote"
                                width: parent.width
                                visible: text !== ""
                                text: root._pathNote(causeRow.modelData)
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontData
                                font.pixelSize: root.tzCaption
                                color: Theme.colorText3
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.expandedCause =
                                causeRow.open ? -1 : causeRow.index
                        }
                    }

                    // Opening reveals THE SAME RAIL, in place — same nodes, same links, same
                    // grades (brief §8).
                    PpChainRail {
                        objectName: "sdDetailCauseRail"
                        width: parent.width
                        height: visible ? (root.compact ? implicitHeight : root._railH) : 0
                        visible: causeRow.open
                        chain: causeRow.modelData
                        fit: root.fit
                        vertical: root.compact
                        interactive: root.interactive
                        screenConditionId: root.screenConditionId
                        screenRef: root.screenRef
                        onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                        onFocusToggled: (id, on) => root.focusToggled(id, on)
                        onDetailRequested: (id) => root.conditionActivated(id)
                    }
                }
            }

            // The rival this session could not adjudicate between, named and explicitly not
            // ranked — the same disclosure the driver footer makes, on the same terms (§A4).
            Text {
                objectName: "sdDetailRival"
                width: col.width
                visible: text !== ""
                text: {
                    const rs = root.detail ? (root.detail.rivals || []) : []
                    return rs.length > 0 ? (rs[0].text || "") : ""
                }
                wrapMode: Text.WordWrap
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }

            // ── effects ──────────────────────────────────────────────────────
            Item {
                width: col.width
                height: effectsLabel.implicitHeight

                Text {
                    id: effectsLabel
                    anchors.left: parent.left
                    text: qsTr("WHAT THIS LEADS TO")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
            }

            Text {
                objectName: "sdDetailOutcomeHeadline"
                width: col.width
                visible: text !== ""
                text: root.detail ? (root.detail.outcomeHeadline || "") : ""
                wrapMode: Text.WordWrap
                lineHeight: 1.35
                font.family: Theme.fontBody
                font.pixelSize: root.tzLabel
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }

            Text {
                objectName: "sdDetailNoEffects"
                width: col.width
                visible: text !== ""
                text: root.detail ? (root.detail.noEffectsLine || "") : ""
                wrapMode: Text.WordWrap
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }

            Repeater {
                id: effectRails
                model: root.effects

                Column {
                    id: effectRow
                    required property var modelData
                    required property int index
                    width: col.width
                    spacing: root.px(6)

                    readonly property bool primary: modelData && modelData.primary === true
                    readonly property bool open: primary || root.expandedEffect === index

                    PpDashedFrame {
                        objectName: "sdDetailPathFrame"
                        width: parent.width
                        height: effectSummary.implicitHeight + 2 * root.px(8)
                        visible: !effectRow.primary
                        frameRadius: Theme.radius
                        strokeColor: Theme.colorBorderMid
                        dashOn:  Math.max(1, root.px(3))
                        dashOff: Math.max(1, root.px(3))

                        Column {
                            id: effectSummary
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.leftMargin:  root.px(9)
                            anchors.rightMargin: root.px(9)
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: root.px(3)

                            Item {
                                width: parent.width
                                height: effectPathLabel.implicitHeight

                                Text {
                                    id: effectPathLabel
                                    anchors.left: parent.left
                                    text: qsTr("PATH %1").arg(effectRow.index + 1)
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    font.letterSpacing: Theme.trackingMicro
                                    color: Theme.colorText2
                                }
                                Text {
                                    objectName: "sdDetailPathToggle"
                                    anchors.right: parent.right
                                    anchors.baseline: effectPathLabel.baseline
                                    text: effectRow.open ? qsTr("CLOSE ▴") : qsTr("OPEN ▸")
                                    font.family: Theme.fontData
                                    font.pixelSize: root.tzCaption
                                    font.letterSpacing: Theme.trackingLabel
                                    color: Theme.colorAccent
                                }
                            }
                            Text {
                                objectName: "sdDetailPathSummary"
                                width: parent.width
                                text: root._pathSummary(effectRow.modelData)
                                wrapMode: Text.WordWrap
                                lineHeight: 1.35
                                font.family: Theme.fontBody
                                font.pixelSize: root.tzMicro
                                font.weight: Theme.fontBodyWeight
                                color: Theme.colorText
                            }
                            Text {
                                objectName: "sdDetailPathNote"
                                width: parent.width
                                visible: text !== ""
                                text: root._pathNote(effectRow.modelData)
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontData
                                font.pixelSize: root.tzCaption
                                color: Theme.colorText3
                            }
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: root.expandedEffect =
                                effectRow.open ? -1 : effectRow.index
                        }
                    }

                    PpChainRail {
                        objectName: "sdDetailEffectRail"
                        width: parent.width
                        height: visible ? (root.compact ? implicitHeight : root._railH) : 0
                        visible: effectRow.open
                        chain: effectRow.modelData
                        fit: root.fit
                        vertical: root.compact
                        interactive: root.interactive
                        screenConditionId: root.screenConditionId
                        screenRef: root.screenRef
                        onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                        onFocusToggled: (id, on) => root.focusToggled(id, on)
                        onDetailRequested: (id) => root.conditionActivated(id)
                    }
                }
            }

            // A path count the walk had to stop at, COUNTED rather than dropped silently — the
            // same rule the card row and the chain tail follow.
            Text {
                objectName: "sdDetailCappedNote"
                width: col.width
                visible: text !== ""
                text: {
                    if (!root.detail) return ""
                    const up = root.detail.causesCapped === true
                    const dn = root.detail.effectsCapped === true
                    return (up || dn)
                        ? qsTr("more authored paths than this page draws — the best-evidenced are shown")
                        : ""
                }
                wrapMode: Text.WordWrap
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }
    }
}
