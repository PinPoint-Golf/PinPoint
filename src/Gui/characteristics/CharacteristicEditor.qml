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

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// The condition as an editable sentence:
//
//   Flag {name} when {signal} and {signal}. Usually caused by {cause} or {cause}.
//   It costs the golfer {consequence}.
//
// Tapping a signal pill opens the MeasurePicker. Tapping a cause pill opens the condition library —
// causes ARE conditions, so that is a reuse picker rather than a second vocabulary.
//
// Edits are written to the user's own pack and override the shipped entry of the same id. The
// shipped pack is never modified, so any edit can be undone by removing the override.
Item {
    id: root

    required property var editor    // CharacteristicEditorModel

    signal closed()
    signal saved()

    property string _statusMessage: ""
    property bool   _statusOk:      true

    // Which sub-picker is open, if any.
    property string _sheet: ""      // "" | "measure" | "cause"

    readonly property var _draft: root.editor.draft

    function _show(message, ok) {
        root._statusMessage = message
        root._statusOk      = ok
        statusTimer.restart()
    }

    Timer {
        id: statusTimer
        interval: 4000
        onTriggered: root._statusMessage = ""
    }

    // ══ The sentence ══════════════════════════════════════════════════════════
    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth
        visible: root._sheet === ""

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(18)

            Text {
                text:           "← " + qsTr("Back")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          cancelMa.containsMouse ? Theme.colorText : Theme.colorText3

                MouseArea {
                    id: cancelMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked: { root.editor.discard(); root.closed() }
                }
            }

            PpDisplayText {
                text: root.editor.isNew ? qsTr("New characteristic") : qsTr("Edit characteristic")
            }

            // Editing a shipped entry writes an override, and says so — the user should know their
            // change lives in their own file and can be undone.
            Text {
                Layout.fillWidth: true
                visible: root.editor.overridesCore && !root.editor.isNew
                text: qsTr("This is a shipped characteristic. Your changes are saved to your own "
                           + "library and override the shipped version — the original is never "
                           + "modified, so you can restore it at any time.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            // ── Name ──────────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("FLAG IT AS")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                PpTextField {
                    Layout.fillWidth: true
                    text: root._draft.label || ""
                    placeholderText: qsTr("What a coach would call it")
                    onEditingFinished: root.editor.setLabel(text)
                }
            }

            // ── Group ─────────────────────────────────────────────────────────
            Flow {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Repeater {
                    model: root.editor.conditionGroups
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool active: root._draft.group === modelData.name

                        implicitWidth:  gText.implicitWidth + Theme.sp(18)
                        implicitHeight: Theme.sp(24)
                        radius: height / 2
                        color:  active ? Theme.colorAccent : Theme.colorBg2

                        Text {
                            id: gText
                            anchors.centerIn: parent
                            text:           modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          parent.active ? Theme.colorBg : Theme.colorText2
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.editor.setGroup(modelData.name)
                        }
                    }
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ── Signals ("…when…") ────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    text:                qsTr("WHEN")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Repeater {
                    model: root._draft.signals || []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Theme.sp(44)
                        radius: Theme.radius
                        color:  Theme.colorBg2

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(12)
                            anchors.rightMargin: Theme.sp(10)
                            spacing: Theme.sp(8)

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.measureLabel || modelData.measureId
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText
                                    elide:          Text.ElideRight
                                }
                                Text {
                                    text: (modelData.direction === "high" ? qsTr("too much")
                                                                          : qsTr("too little"))
                                          + (modelData.status === "notCapturable"
                                             ? " · " + qsTr("not measurable from capture")
                                             : modelData.status === "noProducer"
                                               ? " · " + qsTr("no producer yet") : "")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }

                                // Blast radius, shown BEFORE the edit rather than explained after.
                                Text {
                                    visible:        (modelData.sharedWith || 0) > 0
                                    text: qsTr("shared with %n other characteristic(s) — changing "
                                               + "this measure changes them too", "",
                                               modelData.sharedWith || 0)
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorRagWatch
                                    wrapMode:       Text.WordWrap
                                    Layout.fillWidth: true
                                }
                            }

                            Text {
                                text:           "✕"
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          rmMa.containsMouse ? Theme.colorText : Theme.colorText3

                                MouseArea {
                                    id: rmMa
                                    anchors.fill: parent
                                    anchors.margins: -Theme.sp(6)
                                    hoverEnabled: true
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: root.editor.detachSignal(modelData.id)
                                }
                            }
                        }
                    }
                }

                PpButton {
                    label: qsTr("Add a measure")
                    onClicked: root._sheet = "measure"
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ── Causes ("…usually caused by…") ────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    text:                qsTr("USUALLY CAUSED BY")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Repeater {
                    model: root._draft.causes || []
                    delegate: Rectangle {
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Theme.sp(40)
                        radius: Theme.radius
                        color:  Theme.colorBg2

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(12)
                            anchors.rightMargin: Theme.sp(10)
                            spacing: Theme.sp(8)

                            Text {
                                Layout.fillWidth: true
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                // Behavioural causes are OFFERED, never concluded — visually
                                // distinct everywhere they appear.
                                color:          modelData.offeredOnly ? Theme.colorText3 : Theme.colorText
                                font.italic:    modelData.offeredOnly === true
                                elide:          Text.ElideRight
                            }

                            // Strength cycles through three words. Never a percentage: it is a
                            // ranking weight, not a probability.
                            Rectangle {
                                implicitWidth:  strText.implicitWidth + Theme.sp(16)
                                implicitHeight: Theme.sp(22)
                                radius: height / 2
                                color:  Theme.colorBg

                                Text {
                                    id: strText
                                    anchors.centerIn: parent
                                    text:           modelData.strengthLabel || ""
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText2
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var next = modelData.strength === "weak" ? "moderate"
                                                 : modelData.strength === "moderate" ? "strong" : "weak"
                                        root.editor.addCause(modelData.id, next)
                                    }
                                }
                            }

                            Text {
                                text:           "✕"
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          rmCauseMa.containsMouse ? Theme.colorText
                                                                        : Theme.colorText3

                                MouseArea {
                                    id: rmCauseMa
                                    anchors.fill: parent
                                    anchors.margins: -Theme.sp(6)
                                    hoverEnabled: true
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked: root.editor.removeCause(modelData.id)
                                }
                            }
                        }
                    }
                }

                PpButton {
                    label: qsTr("Add a cause")
                    onClicked: root._sheet = "cause"
                }
            }

            PpDivider { Layout.fillWidth: true }

            // ── Consequence ("…it costs the golfer…") ─────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("IT COSTS THE GOLFER")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                PpTextField {
                    Layout.fillWidth: true
                    text: root._draft.consequence || ""
                    placeholderText: qsTr("The mechanical outcome, in plain English")
                    onEditingFinished: root.editor.setConsequence(text)
                }
            }

            // ── Injury note — a separate axis from performance ────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("INJURY NOTE (OPTIONAL)")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                PpTextField {
                    Layout.fillWidth: true
                    text: root._draft.injuryNote || ""
                    placeholderText: qsTr("Worded conservatively — this is not a diagnosis")
                    onEditingFinished: root.editor.setInjuryNote(text)
                }
            }

            // ── Provenance ────────────────────────────────────────────────────
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Text {
                    text:                qsTr("CITATION (OPTIONAL)")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                PpTextField {
                    Layout.fillWidth: true
                    text: root._draft.citation || ""
                    placeholderText: qsTr("DOI or PMID")
                    onEditingFinished: root.editor.setCitation(text)
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("Without a citation this is badged Proposed wherever it appears. "
                               + "Do not cite a commercial organisation, product or certification "
                               + "body — the ideas are common domain, the brands are not.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }
            }

            // ── Actions ───────────────────────────────────────────────────────
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(10)

                PpButton {
                    visible: root.editor.overridesCore === false && !root.editor.isNew
                    label:   qsTr("Restore shipped version")
                    onClicked: {
                        var r = root.editor.revertToShipped()
                        root._show(r.message, r.ok)
                        if (r.ok) { root.saved(); root.closed() }
                    }
                }

                Item { Layout.fillWidth: true }

                Text {
                    visible:        root._statusMessage.length > 0
                    text:           root._statusMessage
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          root._statusOk ? Theme.colorText3 : Theme.colorRagFault
                }

                PpButton {
                    // Says what it will actually do: with unsaved edits, cancelling throws them
                    // away, and the label should not pretend otherwise.
                    label: root.editor.dirty ? qsTr("Discard changes") : qsTr("Cancel")
                    onClicked: { root.editor.discard(); root.closed() }
                }

                PpButton {
                    label:   qsTr("Save")
                    primary: true
                    onClicked: {
                        var r = root.editor.save()
                        root._show(r.message, r.ok)
                        if (r.ok) { root.saved(); root.closed() }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }

    // ══ Measure picker sheet ══════════════════════════════════════════════════
    MeasurePicker {
        anchors.fill: parent
        anchors.margins: Theme.sp(28)
        visible: root._sheet === "measure"
        editor:  root.editor

        onCancelled: root._sheet = ""
        onMeasureChosen: function(measureId, direction) {
            root.editor.attachMeasure(measureId, direction)
            root._sheet = ""
        }
    }

    // ══ Cause picker sheet ════════════════════════════════════════════════════
    // Causes are conditions, so this is a reuse picker over the same library. Ranking by how much
    // each already explains is the defence against a graph where every characteristic invents its
    // own private cause.
    Item {
        anchors.fill: parent
        visible: root._sheet === "cause"

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.sp(28)
            spacing: Theme.sp(12)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(10)

                PpDisplayText { text: qsTr("Add a cause") }
                Item { Layout.fillWidth: true }
                PpButton { label: qsTr("Done"); onClicked: root._sheet = "" }
            }

            PpTextField {
                id: causeSearch
                Layout.fillWidth: true
                placeholderText: qsTr("Search the library")
            }

            ScrollView {
                Layout.fillWidth:  true
                Layout.fillHeight: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.sp(2)

                    Repeater {
                        model: root.editor.candidateCauses(causeSearch.text)
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: Theme.sp(42)
                            radius: Theme.radius
                            color: modelData.selected ? Theme.colorBg2
                                                      : (causeMa.containsMouse ? Theme.colorBg2
                                                                               : "transparent")

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin:  Theme.sp(12)
                                anchors.rightMargin: Theme.sp(12)
                                spacing: Theme.sp(8)

                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText
                                    elide:          Text.ElideRight
                                }
                                Text {
                                    visible:        modelData.reach !== "measured"
                                    text:           modelData.reachLabel
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                                Text {
                                    visible:        modelData.explains > 0
                                    text:           qsTr("explains %n", "", modelData.explains)
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            MouseArea {
                                id: causeMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  Qt.PointingHandCursor
                                onClicked: {
                                    if (modelData.selected) root.editor.removeCause(modelData.id)
                                    else                    root.editor.addCause(modelData.id, "moderate")
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
