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

// LIKELY DRIVER — the panel's one conclusion, and the three things that keep it honest
// (brief §4.3): why it is the driver, the screen that would settle it, and the rival parent
// that this session could not adjudicate.
//
// THREE COLUMNS, NOT ONE HEADLINE, AND THAT IS THE DESIGN. A driver on its own is a verdict.
// A driver beside the test that would confirm it and the alternative that was not ruled out
// is a hypothesis with its own falsifier printed next to it, which is what the model is
// actually offering. Dropping either of the right-hand columns for space would turn one into
// the other, so at 396 px the footer keeps the driver and the SCREEN and drops the rest —
// the screen is the thing the golfer can act on and the mock's 12c keeps exactly that.
//
// WAITING IS A STATE, NOT AN EMPTY FOOTER. Until the pattern set has held still the driver is
// debounced away (§B8), and the footer says so in the model's words rather than showing a
// blank strip or, worse, the last driver it happened to have.

import QtQuick
import PinPointStudio

Rectangle {
    id: root

    // SessionDiagnosticsModel::driver()
    property var driver: null
    // SessionDiagnosticsModel::coverageLine() — it lives here in the wide arrangement, which
    // is where the mock puts it; the panel drops its own copy so it is stated exactly once.
    property string coverageLine: ""
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // 12c's reductions.
    property bool compact: false

    signal screenRequested(string screenRef, string conditionId)

    objectName: "sdDriverFooter"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int tzLabel:   Math.max(1, Math.round(Theme.fontSzLabel * fit))
    readonly property int tzBody:    Math.max(1, Math.round(Theme.fontSzBody2 * fit))

    readonly property bool eligible: driver ? driver.eligible === true : false
    // THE FOOTER HAS TWO WAYS OF HAVING NO DRIVER, AND ONLY ONE OF THEM IS A WAIT. A live
    // session is waiting for the pattern set to hold still; a finished one is not waiting for
    // anything, and the model hands down a definitive sentence instead. Which of the two is on
    // screen is the model's call — this reads its answer and never infers one.
    readonly property bool finalNoDriver: !eligible && !!driver && driver.final === true
    readonly property var  rival:    driver ? driver.rival : null
    readonly property string screenRef: driver ? (driver.screenRef || "") : ""
    readonly property string screenConditionId: driver ? (driver.screenConditionId || "") : ""
    readonly property bool hasScreen: screenCtaText.text !== ""

    color: Theme.colorSurface
    radius: Theme.radius
    border.width: 1
    border.color: Theme.colorBorderMid
    clip: true

    implicitHeight: compact ? px(56) : px(74)

    // ── waiting for the pattern set to hold still ────────────────────────────
    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin:  root.px(12)
        anchors.rightMargin: root.px(12)
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.eligible
        spacing: root.px(3)

        Text {
            objectName: "sdDriverLabel"
            text: qsTr("LIKELY DRIVER")
            font.family: Theme.fontData
            font.pixelSize: root.tzMicro
            font.letterSpacing: Theme.trackingMicro
            color: Theme.colorText2
        }
        // The live tense: what the footer is waiting for. Absent on a finished session, where
        // there is nothing left to wait for.
        Text {
            objectName: "sdDriverWaiting"
            width: parent.width
            visible: !root.finalNoDriver
            text: root.driver ? (root.driver.waitingText || "") : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: Theme.colorText3
        }
        // ...and the finished tense: the model's own final sentence, in the body weight the
        // driver's name would have had. It is an answer, not a placeholder, and it is set at
        // the same size as one.
        Text {
            objectName: "sdDriverFinal"
            width: parent.width
            visible: root.finalNoDriver
            text: root.driver ? (root.driver.finalText || "") : ""
            wrapMode: Text.WordWrap
            font.family: Theme.fontBody
            font.pixelSize: root.compact ? root.tzMicro : root.tzLabel
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText2
        }
    }

    // ── the driver, its screen, and what it did not settle ───────────────────
    Row {
        id: wide
        anchors.fill: parent
        anchors.leftMargin:   root.px(12)
        anchors.rightMargin:  root.px(12)
        anchors.topMargin:    root.px(9)
        anchors.bottomMargin: root.px(9)
        visible: root.eligible
        spacing: root.px(16)

        Column {
            width: Math.max(0, wide.width - (screenCol.visible ? screenCol.width + wide.spacing : 0)
                               - (asideCol.visible ? asideCol.width + wide.spacing : 0))
            anchors.verticalCenter: parent.verticalCenter
            spacing: root.px(3)

            Text {
                objectName: "sdDriverLabel"
                text: qsTr("LIKELY DRIVER")
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText2
            }
            Text {
                objectName: "sdDriverName"
                width: parent.width
                text: root.driver ? (root.driver.rootName || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontBody
                font.pixelSize: root.compact ? root.tzBody : root.tzLabel
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText
            }
            // 12c keeps the driver and the screen; the sentence that ranks it is the first
            // thing to go, because the golfer can act on the screen and not on the ranking.
            Text {
                objectName: "sdDriverWhy"
                width: parent.width
                visible: !root.compact && text !== ""
                text: root.driver ? (root.driver.whyText || "") : ""
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                lineHeight: 1.4
                font.family: Theme.fontBody
                font.pixelSize: root.tzMicro
                font.weight: Theme.fontBodyWeight
                color: Theme.colorText2
            }
            // ...and takes the CTA under the name instead of beside it.
            Text {
                objectName: "sdScreenCtaCompact"
                width: parent.width
                visible: root.compact && root.hasScreen
                text: screenCtaText.text
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingLabel
                color: Theme.colorAttention
            }
        }

        // ── the screen that would anchor it ──────────────────────────────────
        Column {
            id: screenCol
            width: root.px(300)
            anchors.verticalCenter: parent.verticalCenter
            visible: !root.compact && root.hasScreen
            spacing: root.px(4)

            Rectangle {
                objectName: "sdScreenCta"
                width: parent.width
                height: screenCtaText.implicitHeight + root.px(10)
                radius: Math.max(1, root.px(3))
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(Theme.colorAttention.r, Theme.colorAttention.g,
                                      Theme.colorAttention.b, 0.35)

                Text {
                    id: screenCtaText
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin:  root.px(9)
                    anchors.rightMargin: root.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        const c = root.driver ? (root.driver.screenCta || "") : ""
                        return c === "" ? "" : c + " ▸"
                    }
                    elide: Text.ElideRight
                    font.family: Theme.fontData
                    font.pixelSize: root.tzCaption
                    font.letterSpacing: Theme.trackingLabel
                    color: Theme.colorAttention
                }
            }
            Text {
                objectName: "sdScreenWhy"
                width: parent.width
                text: root.driver ? (root.driver.screenReason || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }

        // ── the rival it did not adjudicate, and the coverage it never had ───
        Column {
            id: asideCol
            width: root.px(276)
            anchors.verticalCenter: parent.verticalCenter
            // 12c drops this column and the panel puts the coverage line back at the bottom;
            // see PpSessionDiagnosticsBody._footerCarriesCoverage.
            visible: !root.compact
            spacing: root.px(4)

            Text {
                objectName: "sdDriverRival"
                width: parent.width
                visible: text !== ""
                text: root.rival ? (root.rival.text || "") : ""
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
                lineHeight: 1.4
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
            Text {
                objectName: "sdDriverCoverage"
                width: parent.width
                visible: text !== ""
                text: root.coverageLine
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                lineHeight: 1.4
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.eligible && root.hasScreen
        onClicked: root.screenRequested(root.screenRef, root.screenConditionId)
    }
}
