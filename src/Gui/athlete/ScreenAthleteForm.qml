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

Item {
    id: root

    signal cancelled()
    signal saved(string uuid)
    signal savedAndStarted(string uuid)

    // ── Form state ────────────────────────────────────────────────────────────
    property bool   nameError:      false
    property string handedness:     "Right"
    property string heightUnit:     "ft"
    property string weightUnit:     "lb"
    property string primaryClub:    "DRIVER"

    // "" = create mode; a uuid = edit that athlete.
    property string editUuid: ""

    // Preferred-club options come from the athlete's actual bag (canonical ids,
    // vocabulary order). Re-evaluates on every club commit via athletesChanged.
    // Create mode (no uuid yet) and an empty bag fall back to the full vocabulary.
    readonly property var clubModel: {
        void athleteController.athletes
        var vocab = athleteController.clubOptions()
        if (root.editUuid === "") return vocab
        var bag = athleteController.clubsFor(root.editUuid)
        var ids = Object.keys(bag)
        if (ids.length === 0) return vocab
        ids.sort(function (a, b) { return vocab.indexOf(a) - vocab.indexOf(b) })
        return ids
    }

    onEditUuidChanged: loadForEdit()
    Component.onCompleted: loadForEdit()

    function resetForm() {
        nameField.text     = ""
        handedness         = "Right"
        heightField.text   = ""
        heightUnit         = "ft"
        weightField.text   = ""
        weightUnit         = "lb"
        handicapField.text = ""
        primaryClub        = "DRIVER"
        speedField.text    = ""
        notesField.text    = ""
        nameError          = false
    }

    function loadForEdit() {
        if (editUuid === "") { resetForm(); return }

        const a = athleteController.athletes.find(function(x) { return x.uuid === editUuid })
        if (!a) { resetForm(); return }

        nameField.text     = a.name || ""
        handedness         = a.handedness  || "Right"
        // Resolve to a real bag club (handles legacy/absent values).
        primaryClub        = athleteController.effectivePrimaryClub(editUuid) || "DRIVER"
        handicapField.text = (a.handicap !== undefined && a.handicap > -900) ? String(a.handicap) : ""
        speedField.text    = (a.speedTarget && a.speedTarget > 0) ? String(a.speedTarget) : ""
        notesField.text    = a.notes || ""

        // Height/weight are STORED in base units (ft / lb) but the athlete carries a
        // saved display-unit preference. Convert the stored value back into that unit
        // so the number and the toggle agree.
        heightUnit = (a.heightUnit === "cm") ? "cm" : "ft"
        heightField.text = (a.heightValue && a.heightValue > 0)
            ? (heightUnit === "cm" ? (a.heightValue * 30.48).toFixed(1)
                                   : a.heightValue.toFixed(2))
            : ""

        weightUnit = (a.weightUnit === "kg") ? "kg" : "lb"
        weightField.text = (a.weightValue && a.weightValue > 0)
            ? (weightUnit === "kg" ? (a.weightValue / 2.20462).toFixed(1)
                                   : a.weightValue.toFixed(1))
            : ""

        nameError = false
    }

    function validate(): bool {
        nameError = nameField.text.trim() === ""
        return !nameError
    }

    function doSave(): string {
        const hv = parseFloat(heightField.text) || 0.0
        const wv = parseFloat(weightField.text) || 0.0
        const hcp = parseFloat(handicapField.text)
        const handicap = isNaN(hcp) ? -999.0 : hcp
        const speed = parseFloat(speedField.text) || 0.0

        return athleteController.saveAthlete(
            root.editUuid,            // "" creates, uuid updates
            nameField.text.trim(),
            handedness,
            hv, heightUnit,
            wv, weightUnit,
            handicap,
            primaryClub,
            speed,
            notesField.text.trim()
        )
    }

    ScrollView {
        anchors.fill:    parent
        contentWidth:    width
        clip:            true

        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            width:   Theme.contentWidth(parent.width)
            spacing: 0
            y:       32

            // ── Header ───────────────────────────────────────────────────────
            Text {
                text:               qsTr("ATHLETE PROFILE")
                font.family:        Theme.fontData
                font.pixelSize:     Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color:              Theme.colorText3
                bottomPadding:      10
            }
            PpDisplayText {
                text: root.editUuid === "" ? qsTr("New athlete") : qsTr("Edit athlete")
            }
            Item { width: 1; height: 6 }
            Text {
                width:          parent.width
                text:           qsTr("Only the first two fields are required. The rest sharpen analysis and personalise coaching output.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText3
                wrapMode:       Text.WordWrap
                lineHeight:     1.5
                bottomPadding:  24
            }

            // ── Required group ───────────────────────────────────────────────
            Rectangle {
                width:        parent.width
                height:       reqCol.implicitHeight
                radius:       Theme.radiusLg
                border.width: 1
                border.color: Theme.colorBorderMid
                clip:         true
                color:        "transparent"

                Column {
                    id: reqCol
                    width: parent.width
                    spacing: 0

                    // Header strip
                    Rectangle {
                        width:  parent.width
                        height: Theme.sp(36)
                        color:  Theme.colorBg2

                        RowLayout {
                            anchors { fill: parent; leftMargin: Theme.sp(14); rightMargin: Theme.sp(14) }
                            Text {
                                text:           qsTr("Required")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                color:          Theme.colorText2
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text:           qsTr("Name and handedness")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }

                        // Bottom border
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width:  parent.width
                            height: 1
                            color:  Theme.colorBorderMid
                        }
                    }

                    // Body
                    Rectangle {
                        width:  parent.width
                        height: reqBody.implicitHeight + 36
                        color:  Theme.colorSurface

                        Column {
                            id: reqBody
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(18) }
                            spacing: Theme.sp(12)

                            // Name
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("NAME")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                PpTextField {
                                    id: nameField
                                    width:           parent.width
                                    placeholderText: qsTr("e.g. Mark Carter")
                                    hasError:        root.nameError
                                    onTextChanged:   if (root.nameError && text.trim() !== "") root.nameError = false
                                }
                                Text {
                                    visible:        root.nameError
                                    height:         root.nameError ? implicitHeight : 0
                                    text:           qsTr("Name is required")
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorWarn
                                }
                            }

                            // Handedness
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("HANDEDNESS")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                PpChipGroup {
                                    options:  ["Right", "Left"]
                                    selected: root.handedness
                                    onSelectionChanged: function(v) { root.handedness = v }
                                }
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: Theme.sp(16) }

            // ── Recommended group ────────────────────────────────────────────
            Rectangle {
                width:        parent.width
                height:       recCol.implicitHeight
                radius:       Theme.radiusLg
                border.width: 1
                border.color: Theme.colorBorderMid
                clip:         true
                color:        "transparent"

                Column {
                    id: recCol
                    width: parent.width
                    spacing: 0

                    // Header strip
                    Rectangle {
                        width:  parent.width
                        height: Theme.sp(36)
                        color:  Theme.colorBg2

                        RowLayout {
                            anchors { fill: parent; leftMargin: Theme.sp(14); rightMargin: Theme.sp(14) }
                            Text {
                                text:           qsTr("Recommended")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                color:          Theme.colorText2
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text:           qsTr("Improves analysis accuracy")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width:  parent.width
                            height: 1
                            color:  Theme.colorBorderMid
                        }
                    }

                    // Body
                    Rectangle {
                        width:  parent.width
                        height: recBody.implicitHeight + 36
                        color:  Theme.colorSurface

                        Column {
                            id: recBody
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(18) }
                            spacing: Theme.sp(12)

                            // Height
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("HEIGHT")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                Row {
                                    width:   parent.width
                                    spacing: 0
                                    PpTextField {
                                        id: heightField
                                        width:           parent.width - heightUnitToggle.width
                                        placeholderText: root.heightUnit === "ft" ? "e.g. 5'11\"" : "e.g. 180"
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    }
                                    PpUnitToggle {
                                        id: heightUnitToggle
                                        units:    ["ft", "cm"]
                                        selected: root.heightUnit
                                        onSelectionChanged: function(unit) {
                                            const val = parseFloat(heightField.text)
                                            if (!isNaN(val) && val > 0) {
                                                if (unit === "cm" && root.heightUnit === "ft")
                                                    heightField.text = (val * 30.48).toFixed(1)
                                                else if (unit === "ft" && root.heightUnit === "cm")
                                                    heightField.text = (val / 30.48).toFixed(2)
                                            }
                                            root.heightUnit = unit
                                        }
                                    }
                                }
                            }

                            // Weight
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("WEIGHT")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                Row {
                                    width:   parent.width
                                    spacing: 0
                                    PpTextField {
                                        id: weightField
                                        width:           parent.width - weightUnitToggle.width
                                        placeholderText: root.weightUnit === "lb" ? "e.g. 178" : "e.g. 80"
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    }
                                    PpUnitToggle {
                                        id: weightUnitToggle
                                        units:    ["lb", "kg"]
                                        selected: root.weightUnit
                                        onSelectionChanged: function(unit) {
                                            const val = parseFloat(weightField.text)
                                            if (!isNaN(val) && val > 0) {
                                                if (unit === "kg" && root.weightUnit === "lb")
                                                    weightField.text = (val / 2.20462).toFixed(1)
                                                else if (unit === "lb" && root.weightUnit === "kg")
                                                    weightField.text = (val * 2.20462).toFixed(1)
                                            }
                                            root.weightUnit = unit
                                        }
                                    }
                                }
                            }

                            // Handicap
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("HANDICAP")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                PpTextField {
                                    id: handicapField
                                    width:            Theme.sp(100)
                                    placeholderText:  "e.g. 5"
                                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                                }
                            }

                            // Primary club
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("PRIMARY CLUB")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                // Sourced from the athlete's bag (canonical ids). Sync
                                // currentIndex imperatively — not via a binding — so
                                // picking an item doesn't sever the link on reload.
                                PpComboBox {
                                    id: primaryClubCombo
                                    implicitWidth: Theme.sp(220)
                                    model: root.clubModel
                                    displayFn: ClubFormat.display
                                    onActivated: function (i) { root.primaryClub = root.clubModel[i] }
                                    function syncFromValue() {
                                        var idx = root.clubModel.indexOf(root.primaryClub)
                                        if (idx < 0 && root.clubModel.length > 0) {
                                            root.primaryClub = root.clubModel[0]
                                            idx = 0
                                        }
                                        currentIndex = idx >= 0 ? idx : 0
                                    }
                                    Component.onCompleted: syncFromValue()
                                    onModelChanged: syncFromValue()
                                    Connections {
                                        target: root
                                        function onPrimaryClubChanged() { primaryClubCombo.syncFromValue() }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: Theme.sp(16) }

            // ── Optional group ───────────────────────────────────────────────
            Rectangle {
                width:        parent.width
                height:       optCol.implicitHeight
                radius:       Theme.radiusLg
                border.width: 1
                border.color: Theme.colorBorderMid
                clip:         true
                color:        "transparent"

                Column {
                    id: optCol
                    width: parent.width
                    spacing: 0

                    // Header strip
                    Rectangle {
                        width:  parent.width
                        height: Theme.sp(36)
                        color:  Theme.colorBg2

                        RowLayout {
                            anchors { fill: parent; leftMargin: Theme.sp(14); rightMargin: Theme.sp(14) }
                            Text {
                                text:           qsTr("Optional")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                font.letterSpacing: Theme.trackingMicro
                                color:          Theme.colorText2
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text:           qsTr("Personalises coaching output")
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width:  parent.width
                            height: 1
                            color:  Theme.colorBorderMid
                        }
                    }

                    // Body fields
                    Rectangle {
                        width:  parent.width
                        height: optBody.implicitHeight + 36
                        color:  Theme.colorSurface

                        Column {
                            id: optBody
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(18) }
                            spacing: Theme.sp(12)

                            // Driver speed target
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("DRIVER SPEED TARGET")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                Row {
                                    width:   parent.width
                                    spacing: 0
                                    PpTextField {
                                        id: speedField
                                        width:            parent.width - speedUnit.width
                                        placeholderText:  "e.g. 105"
                                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                                    }
                                    Rectangle {
                                        id: speedUnit
                                        width:  Theme.sp(36)
                                        height: Theme.sp(34)
                                        color:  Theme.colorBg2
                                        border.width: 1
                                        border.color: Theme.colorBorderStrong
                                        radius: Theme.radius
                                        Text {
                                            anchors.centerIn: parent
                                            text:           "mph"
                                            font.family:    Theme.fontData
                                            font.pixelSize: Theme.fontSzMicro
                                            color:          Theme.colorText3
                                        }
                                    }
                                }
                            }

                            // Notes
                            Column {
                                width:   parent.width
                                spacing: Theme.sp(4)
                                Text {
                                    text:               qsTr("NOTES / TAGS")
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingLabel
                                    color:              Theme.colorText3
                                }
                                PpTextField {
                                    id: notesField
                                    width:           parent.width
                                    placeholderText: "e.g. junior, left-low, early ext"
                                }
                            }
                        }
                    }

                    // Note strip
                    Rectangle {
                        width:  parent.width
                        height: noteText.implicitHeight + Theme.sp(20)
                        color:  Theme.colorSurface
                        border.width: 0

                        Rectangle {
                            anchors.top:  parent.top
                            width:        parent.width
                            height:       1
                            color:        Theme.colorBorderMid
                        }

                        Text {
                            id: noteText
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(18); topMargin: Theme.sp(10) }
                            text:           qsTr("Pinpoint builds baselines automatically from early sessions. You don't need to know your driver speed to get started.")
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzDataSm
                            font.weight:    Font.Light
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                            lineHeight:     1.4
                        }
                    }
                }
            }

            // ── Clubs (edit mode only — records commit immediately, unlike the
            //    buffered scalar fields; a new athlete must be saved first) ─────
            Item { visible: root.editUuid !== ""; width: 1; height: Theme.sp(16) }
            AthleteClubsSection {
                visible:     root.editUuid !== ""
                athleteUuid: root.editUuid
                width:       parent.width
            }

            // ── Action row ───────────────────────────────────────────────────
            Item { width: 1; height: Theme.sp(24) }
            Row {
                anchors.right: parent.right
                spacing: Theme.sp(8)
                bottomPadding: 32

                PpButton {
                    label: qsTr("Cancel")
                    onClicked: root.cancelled()
                }
                PpButton {
                    label:   root.editUuid === "" ? qsTr("Save") : qsTr("Save changes")
                    primary: root.editUuid !== ""        // primary in edit mode (no Save-and-start there)
                    onClicked: {
                        if (!validate()) return
                        const uuid = doSave()
                        if (uuid !== "") root.saved(uuid)
                    }
                }
                PpButton {
                    visible: root.editUuid === ""        // create flow only
                    label:   qsTr("Save and start ↗")
                    primary: true
                    onClicked: {
                        if (!validate()) return
                        const uuid = doSave()
                        if (uuid !== "") root.savedAndStarted(uuid)
                    }
                }
            }
            Item { width: 1; height: Theme.sp(32) }
        }
    }
}
