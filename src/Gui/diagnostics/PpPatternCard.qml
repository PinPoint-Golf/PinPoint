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

    objectName: "sdPatternCard"

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

    color: Theme.colorSurface
    radius: Theme.radius
    border.width: 1
    border.color: Theme.colorBorderMid
    clip: true

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
        Text {
            objectName: "sdCardDirection"
            width: col.width
            text: root.card ? (root.card.directionText || "") : ""
            visible: text !== ""
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.family: Theme.fontBody
            font.pixelSize: root.tzLabel
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText2
        }

        // ── the run ──────────────────────────────────────────────────────────
        PpTickRun {
            objectName: "sdTickRun"
            width: col.width
            ticks: root.card ? root.card.ticks : []
            fit: root.fit
        }

        // ── trend + recency ──────────────────────────────────────────────────
        Row {
            width: col.width
            spacing: root.px(8)

            Text {
                objectName: "sdCardTrend"
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
                text: root.card ? (root.card.firingsAfterText || root.card.recencyText || "") : ""
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                color: Theme.colorText3
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
            visible: height >= root.tzMicro * lineHeight
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
}
