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

    // Open the editor straight onto the measure picker. Used by the measure catalogue's "New
    // measure": a mint writes into the DRAFT, so it needs a draft to land in, and a measure with
    // no characteristic reading it is a validator warning the moment it saves.
    function openMeasurePicker() { root._sheet = "measure" }

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
                visible: root.editor.shippedExists && !root.editor.hasUserOverride
                         && !root.editor.isNew
                text: qsTr("This is a shipped characteristic. Your changes are saved to your own "
                           + "library and override the shipped version — the original is never "
                           + "modified, so you can restore it at any time.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
            }

            // Already edited. Said in the accent because "is this still what we ship?" is the one
            // thing about a characteristic you cannot tell by reading it.
            Text {
                Layout.fillWidth: true
                visible: root.editor.hasUserOverride && !root.editor.isNew
                text: root.editor.shippedExists
                        ? qsTr("You have edited this shipped characteristic. The original is "
                               + "untouched and can be restored below.")
                        : qsTr("This characteristic is yours — nothing ships under this name.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorAccent
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
                        // Content-driven: the direction sentence wraps, and the missing-convention
                        // prompt adds a field, so a fixed 44 would clip both.
                        implicitHeight: sigRow.implicitHeight + Theme.sp(16)
                        radius: Theme.radius
                        color:  Theme.colorBg2

                        RowLayout {
                            id: sigRow
                            anchors.left:        parent.left
                            anchors.right:       parent.right
                            anchors.top:         parent.top
                            anchors.topMargin:   Theme.sp(8)
                            anchors.leftMargin:  Theme.sp(12)
                            anchors.rightMargin: Theme.sp(10)
                            spacing: Theme.sp(8)

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp(2)

                                Text {
                                    Layout.fillWidth: true
                                    text:           modelData.measureLabel || modelData.measureId
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText
                                    elide:          Text.ElideRight
                                }
                                // The tail in the MEASURE's own words — "flagged when there is more
                                // of it: further back, toward the trail foot" — rather than "too
                                // much", which is true of both tails of every measure ever written.
                                // Composed in C++; the delegate must not decide which sentence
                                // belongs to which direction.
                                Text {
                                    Layout.fillWidth: true
                                    text: (modelData.directionSentence || "")
                                          + (modelData.status === "notCapturable"
                                             ? " · " + qsTr("not measurable from capture")
                                             : modelData.status === "noProducer"
                                               ? " · " + qsTr("no producer yet") : "")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                    wrapMode:       Text.WordWrap
                                }

                                // A measure with no stated sign convention is the exact condition
                                // that let three signals ship inverted. Asked here, where the tail
                                // was chosen, rather than reported later as a health-view warning.
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: (modelData.highMeans || "").length === 0
                                    spacing: Theme.sp(4)

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("Nothing says what a HIGHER value of this "
                                                   + "measure means, so nothing can check that this "
                                                   + "tail is the right one.")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorRagWatch
                                        wrapMode:       Text.WordWrap
                                    }
                                    PpTextField {
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("A higher value means… e.g. “further "
                                                              + "back, toward the trail foot”")
                                        onEditingFinished:
                                            root.editor.setMeasureHighMeans(modelData.measureId, text)
                                    }
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

            PpDivider { Layout.fillWidth: true }

            // ── Where it applies ("…on this kind of shot") ────────────────────
            //
            // Every row is an EXCEPTION. A characteristic with nothing set here applies to every
            // kind of shot, which is what all 50 shipped ones do — so this list is almost always
            // read rather than written, and it has to make "nothing is set" look deliberate.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    text:                qsTr("WHERE IT APPLIES")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Text {
                    Layout.fillWidth: true
                    text: qsTr("It applies to every kind of shot unless you say otherwise, and a "
                               + "setting here carries to everything beneath it — switch off "
                               + "Partial swing and pitch and chip follow. Context never changes "
                               + "whether something is good or bad; it only says whether the "
                               + "question is worth asking.")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    wrapMode:       Text.WordWrap
                }

                Repeater {
                    model: root.editor.contexts
                    delegate: Item {
                        id: ctxRow
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: Theme.sp(30)

                        // Almost every row in this list is the default repeated, so the default is
                        // drawn quietly and only what somebody STATED is drawn firmly. A column of
                        // thirteen identical accent boxes would say "thirteen decisions were made
                        // here", which is the opposite of true.
                        HoverHandler { id: ctxHover }

                        RowLayout {
                            anchors.fill: parent
                            // Indent off the model's own depth — the view never walks parents.
                            anchors.leftMargin: Theme.sp(2) + modelData.depth * Theme.sp(18)
                            spacing: Theme.sp(10)

                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth:  Theme.sp(16)
                                implicitHeight: Theme.sp(16)
                                radius: Theme.sp(4)
                                color:  (modelData.applicable && modelData.own) ? Theme.colorAccent
                                                                                : "transparent"
                                border.width: 1
                                border.color: modelData.own ? Theme.colorAccent
                                                            : Theme.colorBorderStrong

                                Text {
                                    anchors.centerIn: parent
                                    visible:        modelData.applicable
                                    text:           "✓"
                                    font.family:    Theme.fontSymbol
                                    font.pixelSize: Theme.fontSzLabel
                                    color:          modelData.own
                                                      ? (Theme.dark ? Theme.colorBg : "#FFFFFF")
                                                      : Theme.colorText3
                                }

                                PpPressable {
                                    anchors.margins: -Theme.sp(8)   // 44pt-ish target
                                    onClicked: {
                                        var r = root.editor.setBinding(modelData.id,
                                                                       !modelData.applicable,
                                                                       modelData.material)
                                        if (r.ok && r.message) bindingToast.show(r.message)
                                        else if (!r.ok) root._show(r.message, false)
                                    }
                                }
                            }

                            Text {
                                text:           modelData.label
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          modelData.applicable ? Theme.colorText
                                                                     : Theme.colorText3
                            }

                            Text {
                                text: modelData.own
                                        ? qsTr("set here")
                                        : (modelData.inherited
                                           ? qsTr("from %1").arg(modelData.inheritedFromLabel) : "")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }

                            Item { Layout.fillWidth: true }

                            // Ranking weight, and never worded as a judgement about the shot.
                            // Hidden while it is the default AND untouched: it is a rarely-used
                            // control, and thirteen copies of "counts when ranking" would bury the
                            // one row where it does not.
                            Rectangle {
                                visible: modelData.applicable
                                         && (!modelData.material || ctxHover.hovered)
                                implicitWidth:  matText.implicitWidth + Theme.sp(14)
                                implicitHeight: Theme.sp(20)
                                radius: height / 2
                                color:  Theme.colorBg2

                                Text {
                                    id: matText
                                    anchors.centerIn: parent
                                    text: modelData.material ? qsTr("counts when ranking")
                                                             : qsTr("not counted when ranking")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          modelData.material ? Theme.colorText2
                                                                       : Theme.colorText3
                                }
                                PpPressable {
                                    onClicked: {
                                        var r = root.editor.setBinding(modelData.id, true,
                                                                       !modelData.material)
                                        if (r.ok && r.message) bindingToast.show(r.message)
                                    }
                                }
                            }

                            // Back to inheriting. Offered only where there is something of this
                            // characteristic's own to drop — an action that can only fail is worse
                            // than no action at all.
                            Text {
                                visible:        modelData.own
                                text:           "✕"
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorText3

                                PpPressable {
                                    anchors.margins: -Theme.sp(6)
                                    onClicked: {
                                        var r = root.editor.clearBinding(modelData.id)
                                        if (r.ok && r.message) bindingToast.show(r.message)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            PpDivider { Layout.fillWidth: true }

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

                // ONE operation, two very different outcomes — so two labels, and the destructive
                // one is styled as destructive. Offering "Restore shipped version" for a
                // characteristic that never shipped is how deleting somebody's work acquires a
                // reassuring name.
                PpButton {
                    visible:     root.editor.hasUserOverride && !root.editor.isNew
                    label:       root.editor.shippedExists ? qsTr("Restore shipped version")
                                                           : qsTr("Delete this characteristic")
                    destructive: !root.editor.shippedExists
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

    // ══ Binding toast ═════════════════════════════════════════════════════════
    //
    // Switching a context off can clear exception rows beneath it that the author never named, so
    // the change has to say what it did AND be reversible in the same breath. One level of undo,
    // held in the model: the toast is the affordance, not the mechanism.
    PpToast {
        id: bindingToast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom:           parent.bottom
        anchors.bottomMargin:     Theme.sp(24)
        z: 10
        glyph: "◇"
        onUndoClicked: root.editor.undoBindingChange()
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
