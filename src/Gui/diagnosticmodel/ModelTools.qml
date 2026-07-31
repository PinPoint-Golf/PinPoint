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

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// The `⋯` drawer: the things that are not content and are not the draft — two saved views, two
// exports, the norm-set inventory, and the way back to the shipped model.
//
// This was `Tools`, and Tools was a grab-bag whose own header comment described "the things that are
// not content" as if that were one thing. It is two. The grade policy left for a live readout on the
// bar (ModelPolicyPicker.qml) because it is state you need to SEE, not an action you go and find;
// what stayed is a junk drawer, which is the right shape for the rest of it.
//
// Nothing in here is on the undo stack except the reset, which is a command like every other write.
Popup {
    id: root

    property var browser: null
    // Bumped by the panel on every model change. `normSets()` is a Q_INVOKABLE, so a binding calling
    // it is subscribed to nothing — and this Popup's content is built once and kept, which means the
    // inventory would freeze at whatever it said the first time the drawer was opened. Read as a
    // VALUE below, never as a bare statement: see the note on `_revision` in DiagnosticModel.qml.
    property int revision: 0
    // Whether the install carries local content at all — the reset section does not exist without it,
    // which is how the drawer stays a drawer rather than a danger zone.
    property bool hasLocalContent: false
    property int  changedCount:    0     // shipped objects overridden
    property int  yoursCount:      0     // objects authored here

    signal exportRoadmapRequested()
    signal exportReferencesRequested()
    signal roadmapRequested()
    signal glossaryRequested()
    signal resetRequested()

    width:   Theme.sp(340)
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color:        Theme.colorSurface
        radius:       Theme.radius
        border.width: 1
        border.color: Theme.colorBorderStrong
    }

    component Heading: Text {
        font.family:         Theme.fontBody
        font.pixelSize:      Theme.fontSzMicro
        font.letterSpacing:  Theme.trackingMicro
        font.capitalization: Font.AllUppercase
        color:               Theme.colorText3
    }

    component Action: Item {
        id: action
        property string label: ""
        property string hint:  ""
        // Styled as what it does. The app already draws a destructive row this way — see the per-row
        // menu in DagView.qml, "Removing a link is a write to the user's pack and is styled as one".
        property bool   destructive: false
        signal triggered()

        implicitHeight: Theme.sp(30)

        Rectangle {
            anchors.fill: parent
            color: actionHover.hovered ? Theme.colorBg2 : "transparent"
        }
        HoverHandler { id: actionHover }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin:  Theme.sp(14)
            anchors.rightMargin: Theme.sp(14)
            spacing: Theme.sp(8)

            Text {
                Layout.fillWidth: true
                text: action.label
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          action.destructive ? Theme.colorError : Theme.colorText
                elide:          Text.ElideRight
            }
            Text {
                text:    action.hint
                visible: text.length > 0
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          action.destructive ? Theme.colorError : Theme.colorText3
                opacity:        action.destructive ? 0.75 : 1.0
            }
        }

        PpPressable { hoverScale: 1.0; onClicked: action.triggered() }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Heading {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(2)
            text: qsTr("Views and artefacts")
        }

        Action {
            Layout.fillWidth: true
            label: qsTr("Roadmap")
            hint:  qsTr("what is not built yet")
            onTriggered: { root.roadmapRequested(); root.close() }
        }
        Action {
            Layout.fillWidth: true
            label: qsTr("Glossary")
            hint:  qsTr("what a term means")
            onTriggered: { root.glossaryRequested(); root.close() }
        }
        Action {
            Layout.fillWidth: true
            label: qsTr("Export roadmap")
            hint:  qsTr("markdown")
            onTriggered: { root.exportRoadmapRequested(); root.close() }
        }
        Action {
            Layout.fillWidth: true
            label: qsTr("Export references")
            hint:  qsTr("CSL-JSON")
            onTriggered: { root.exportReferencesRequested(); root.close() }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp(8)
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        Heading {
            Layout.margins:      Theme.sp(14)
            Layout.bottomMargin: Theme.sp(2)
            text: qsTr("Norm sets")
        }

        Repeater {
            model: (root.browser && root.revision >= 0) ? root.browser.normSets() : []
            delegate: RowLayout {
                id: setRow
                required property var modelData

                Layout.fillWidth:   true
                Layout.leftMargin:  Theme.sp(14)
                Layout.rightMargin: Theme.sp(14)
                Layout.bottomMargin: Theme.sp(3)
                spacing: Theme.sp(8)

                Text {
                    Layout.fillWidth: true
                    // "merged" is an implementation word and must never reach a reader: they need
                    // the shipped set and their own as separate things, because that is what the
                    // override relationship between them means.
                    text: setRow.modelData.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                    elide:          Text.ElideRight
                }
                Text {
                    text: setRow.modelData.normCount
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }
        }

        // ── The way back ──────────────────────────────────────────────────────
        //
        // Absent on an untouched install: there is nothing to reset, and an entry that could only
        // ever refuse is worse than no entry. The counts are two different losses and are said as
        // two — a shipped screen you edited and a characteristic you wrote are not the same thing
        // to lose.
        //
        // NOT the same as `Revert all` in the unsaved popover, and named so it cannot be mistaken
        // for it: that one discards this session's UNSAVED edits back to the file, this one discards
        // what is already saved back to what shipped.
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp(8)
            Layout.preferredHeight: 1
            visible: root.hasLocalContent
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        Action {
            Layout.fillWidth: true
            visible: root.hasLocalContent
            destructive: true
            label: qsTr("Reset to the standard model…")
            hint: {
                var bits = []
                if (root.changedCount > 0) bits.push(qsTr("%n changed", "", root.changedCount))
                if (root.yoursCount > 0)   bits.push(qsTr("%n yours", "", root.yoursCount))
                return bits.join(" · ")
            }
            onTriggered: { root.resetRequested(); root.close() }
        }

        Item { Layout.preferredHeight: Theme.sp(8) }
    }
}
