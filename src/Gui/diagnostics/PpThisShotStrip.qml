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

// The after-shot moment, and its silence.
//
// TWO STATES, ONE HEIGHT. The strip either says what this swing did — the delta headline
// over a row of chips — or it says BANDWIDTH · QUIET. They are the same size and sit in the
// same place because the quiet state is a STATEMENT, not an absence: the ledger accumulated
// at full rate and cadence gated only the surfacing (brief §3.4, §5.6). A strip that
// collapsed when quiet would leave the golfer reading "nothing was recorded" off a gap,
// which is the one thing bandwidth mode must never be mistaken for. The caption under it
// says so in words, and it is not optional.
//
// GHOSTED PATTERN CHIPS. A condition that fired here AND is already one of the session's
// patterns is drawn dashed at reduced opacity with a ↓ arrow — its card downstairs is where
// it is read, and a full-strength chip would make the same finding twice at two different
// weights. The chips that stay at full strength are the ones the card row does not carry.

import QtQuick
import PinPointStudio

Item {
    id: root

    // SessionDiagnosticsModel::thisShot() — [{ id, name, kind, tier, focus }]
    property var chips: []
    // SessionDiagnosticsModel::afterShotDelta()
    property var delta: null
    // SessionDiagnosticsModel::quiet()
    property bool quiet: false
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // Below the wide arrangement's floor the strip drops its right-hand caption and lets
    // the headline wrap instead of eliding — 12c's treatment.
    property bool compact: false

    objectName: "sdThisShotStrip"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2 * fit))

    implicitHeight: px(56)

    // ── surfaced ─────────────────────────────────────────────────────────────
    Rectangle {
        objectName: "sdChipState"
        anchors.fill: parent
        visible: !root.quiet
        color: Theme.colorSurface
        radius: Theme.radius
        border.width: 1
        border.color: Theme.colorBorderMid
        clip: true

        Column {
            anchors.fill: parent
            anchors.leftMargin:   root.px(10)
            anchors.rightMargin:  root.px(10)
            anchors.topMargin:    root.px(7)
            anchors.bottomMargin: root.px(7)
            spacing: root.px(5)

            Item {
                width: parent.width
                height: headline.implicitHeight

                Text {
                    objectName: "sdStripLabel"
                    id: stripLabel
                    anchors.left: parent.left
                    anchors.baseline: headline.baseline
                    text: qsTr("THIS SHOT")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
                Text {
                    id: headline
                    objectName: "sdDeltaHeadline"
                    anchors.left: stripLabel.right
                    anchors.leftMargin: root.px(9)
                    anchors.right: chipHint.visible ? chipHint.left : parent.right
                    anchors.rightMargin: chipHint.visible ? root.px(9) : 0
                    anchors.top: parent.top
                    text: root.delta ? (root.delta.headline || "") : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontBody
                    font.pixelSize: root.tzBody
                    font.weight: Theme.fontBodyWeight
                    color: Theme.colorText
                }
                Text {
                    id: chipHint
                    anchors.right: parent.right
                    anchors.baseline: headline.baseline
                    visible: !root.compact
                    text: qsTr("dashed chip = one of your patterns · read it on its card")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    color: Theme.colorText3
                }
            }

            // The chips wrap rather than scroll: a condition that fired is not allowed to
            // be off-screen because the row ran out of room.
            Flow {
                width: parent.width
                spacing: root.px(4)

                Repeater {
                    model: root.chips

                    Rectangle {
                        id: chip
                        required property var modelData

                        objectName: "sdChip"

                        readonly property bool fired: modelData.kind === "fired"
                        readonly property bool clean: modelData.kind === "clean"
                        // A pattern chip is ghosted: the card is where it is read.
                        readonly property bool ghosted: fired && modelData.tier === "pattern"
                        readonly property color stateColor: fired ? Theme.colorError
                                                          : clean ? Theme.colorGood
                                                                  : Theme.colorText3

                        width:  chipRow.implicitWidth + root.px(14)
                        height: chipRow.implicitHeight + root.px(4)
                        radius: Math.max(1, root.px(3))
                        opacity: ghosted ? 0.62 : 1.0

                        // Fired fills: the error token at its Light (~10%) alpha, framed at
                        // ~35% of the same hue. Derived from the token, so both Studio
                        // themes and every other aesthetic follow it.
                        color: fired ? Theme.colorErrorLight : "transparent"
                        border.width: ghosted ? 0 : 1
                        border.color: fired ? Qt.rgba(Theme.colorError.r, Theme.colorError.g,
                                                      Theme.colorError.b, 0.35)
                                            : Theme.colorBorder

                        PpDashedFrame {
                            objectName: "sdChipDash"
                            anchors.fill: parent
                            visible: chip.ghosted
                            frameRadius: chip.radius
                            strokeColor: Qt.rgba(Theme.colorError.r, Theme.colorError.g,
                                                 Theme.colorError.b, 0.35)
                            dashOn:  Math.max(1, root.px(3))
                            dashOff: Math.max(1, root.px(3))
                        }

                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: root.px(5)

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.modelData.name || ""
                                font.family: Theme.fontBody
                                font.pixelSize: root.tzLabel
                                font.weight: Theme.fontBodyWeight
                                color: Theme.colorText
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                // The CLEAN and NOT-ASSESSABLE entries carry their whole
                                // meaning in `name` ("clean on every measurable condition",
                                // "2 not assessable on this capture"), so a state label
                                // beside them would only repeat it.
                                visible: text !== ""
                                text: chip.fired
                                      ? (qsTr("FIRED") + (chip.ghosted ? " ↓" : ""))
                                      : ""
                                font.family: Theme.fontData
                                font.pixelSize: root.tzCaption
                                font.letterSpacing: Theme.trackingLabel
                                color: chip.stateColor
                            }
                        }
                    }
                }
            }
        }
    }

    // ── quiet ────────────────────────────────────────────────────────────────
    Item {
        objectName: "sdQuietState"
        anchors.fill: parent
        visible: root.quiet

        PpDashedFrame {
            anchors.fill: parent
            frameRadius: Theme.radius
            strokeColor: Theme.colorBorderMid
            dashOn:  Math.max(1, root.px(4))
            dashOff: Math.max(1, root.px(4))
        }

        Column {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin:  root.px(10)
            anchors.rightMargin: root.px(10)
            anchors.verticalCenter: parent.verticalCenter
            spacing: root.px(4)

            Item {
                width: parent.width
                height: quietLine.implicitHeight

                Text {
                    id: quietLabel
                    objectName: "sdQuietLabel"
                    anchors.left: parent.left
                    anchors.baseline: quietLine.baseline
                    text: qsTr("BANDWIDTH · QUIET")
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorGood
                }
                Text {
                    id: quietLine
                    objectName: "sdQuietLine"
                    anchors.left: quietLabel.right
                    anchors.leftMargin: root.px(9)
                    anchors.right: parent.right
                    anchors.top: parent.top
                    text: root.delta ? (root.delta.headline || "") : ""
                    elide: Text.ElideRight
                    font.family: Theme.fontBody
                    font.pixelSize: root.tzBody
                    font.weight: Theme.fontBodyWeight
                    color: Theme.colorText2
                }
            }

            // Not decoration. See the header comment.
            Text {
                width: parent.width
                text: qsTr("the ledger kept accumulating at full rate · cadence gates surfacing, never the engine")
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }
    }
}
