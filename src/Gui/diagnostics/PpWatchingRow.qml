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

// Watching (n) — seen, not yet evidence, and it never headlines.
//
// COLLAPSED BY DEFAULT AND TRUNCATING, which is the design's judgement rather than a space
// saving: a condition below the pattern gate has fired at least once, and giving it the
// same weight as a pattern would be the panel claiming a recurrence it cannot support
// (brief §3.2). One tracked line says the model is watching without asserting anything.
//
// Expanding opens THE SAME CONTENT larger, not a different view (brief §8) — the same rows,
// one per line, so nothing is revealed by expanding that the collapsed row was hiding.

import QtQuick
import PinPointStudio

Item {
    id: root

    // SessionDiagnosticsModel::watching() — [{ id, name, recurrence }]
    property var items: []
    property bool expanded: false
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0

    signal toggled()

    objectName: "sdWatchingRow"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))
    readonly property int tzMicro:   Math.max(1, Math.round(Theme.fontSzMicro * fit))
    readonly property int count: items ? items.length : 0

    visible: count > 0
    implicitHeight: visible ? (expanded ? body.implicitHeight : px(22)) : 0

    // The joined one-liner. The separator is the only thing this file contributes — every
    // name and every recurrence count is the model's wording.
    readonly property string _line: {
        if (!items) return ""
        const parts = []
        for (let i = 0; i < items.length; ++i)
            parts.push(items[i].name + " " + items[i].recurrence)
        return parts.join(" · ")
    }

    Column {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.leftMargin:  root.px(4)
        anchors.rightMargin: root.px(4)
        spacing: root.px(4)

        Item {
            width: parent.width
            height: root.px(22)

            Text {
                id: label
                objectName: "sdWatchingLabel"
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: (root.expanded ? "▴ " : "▾ ") + qsTr("WATCHING (%1)").arg(root.count)
                font.family: Theme.fontData
                font.pixelSize: root.tzCaption
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
            Text {
                objectName: "sdWatchingLine"
                anchors.left: label.right
                anchors.leftMargin: root.px(9)
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.expanded
                text: root._line
                elide: Text.ElideRight
                font.family: Theme.fontData
                font.pixelSize: root.tzMicro
                color: Theme.colorText2
            }
        }

        Repeater {
            model: root.expanded ? root.items : []

            Item {
                required property var modelData
                objectName: "sdWatchingItem"
                width: body.width
                height: root.px(16)

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: root.px(14)
                    anchors.right: recurrence.left
                    anchors.rightMargin: root.px(9)
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.name || ""
                    elide: Text.ElideRight
                    font.family: Theme.fontBody
                    font.pixelSize: root.tzMicro
                    font.weight: Theme.fontBodyWeight
                    color: Theme.colorText2
                }
                Text {
                    id: recurrence
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.recurrence || ""
                    font.family: Theme.fontData
                    font.pixelSize: root.tzMicro
                    color: Theme.colorText3
                }
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.toggled()
    }
}
