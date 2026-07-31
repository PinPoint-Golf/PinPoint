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

// The inspector — a RELATIONSHIP HUB, not a property sheet.
//
// Its job is that every related object is one click away. That is the whole complaint the panel
// exists to answer: a metric, the measure that reads it, the corridor that judges it and the
// characteristic that fires are one chain, and following it used to mean leaving the page. Every row
// in here is navigable, and navigating pushes onto the trail rather than replacing where you were.
//
// It is always live: there is no read-only state and no Edit button, because selecting a row IS
// opening its editor.
Item {
    id: root

    // The map from ModelBrowser.inspect().
    property var detail: ({})
    // The map from ModelBrowser.corridorPlot(), when a corridor is selected. Empty otherwise.
    // Asks for the corridor picture at whatever size the plot decides it needs —
    // see ModelCorridorPlot.plotSource.
    property var corridorPlotSource: null
    // Add affordances are offered only where the panel can actually write.
    property bool editable: true

    signal navigate(string type, string id)
    signal addCauseRequested()
    signal addMeasureRequested()
    signal addCorridorRequested()
    // Sections added since: the add is the same gesture every time, so it travels as the section's
    // own action KEY rather than growing a signal per relationship.
    signal addRowRequested(string action)
    signal removeRowRequested(string type, string id)
    // A row that is a BUTTON rather than a link — "Copy CSL-JSON", "Open DOI ↗". The row carries the
    // action key in its id, so the view stays ignorant of what any of them do.
    signal rowActionRequested(string action, string id)
    // A claim's strength, set from the reference pane. The citation is imported and read-only; what
    // rests on it is ours, so this one control is live on an otherwise inert page.
    signal claimStrengthChanged(string linkId, string strength)
    // Fold this pane away. The control belongs to the pane, not to the panel — the same reasoning
    // that puts the settings sidenav's ‹‹ at the sidenav's own edge rather than on the page.
    signal collapseRequested()

    // Where the affordance that just fired sits, in THIS pane's coordinates. The panel maps it and
    // opens the picker there, so a type-ahead triggered from a section at the bottom right of the
    // window does not appear at the top left of it — which on a large screen is a mouse journey
    // across the whole display to answer a question you asked over here.
    property point actionOrigin: Qt.point(0, 0)

    // A field of the selected object was committed. Routed to the same setField() the table uses, so
    // there is one write path and one set of rules whichever surface the author typed into.
    signal fieldCommitted(string field, var value)
    // Copy / Delete for the whole object. They live here rather than on the context bar because they
    // act on the thing this pane is showing, and a destructive action belongs beside the thing it
    // would destroy.
    signal duplicateRequested()
    signal removeRequested()
    // A binding cycles rather than opening a control: applies → not counted → does not apply →
    // inherits. Four states, one click each, and the row says which it is in — a tri-state that
    // needed a popup would cost more clicks than the thing it is setting is worth.
    signal bindingCycled(string contextId, bool applicable, bool material, bool clear)
    signal corridorHandleCommitted(string handle, real value)
    signal corridorFieldCommitted(string field, string text)
    signal corridorScanRequested()

    readonly property bool _found: detail && detail.found === true

    function _toneColor(tone) {
        switch (tone) {
        case "good":   return Theme.colorRagGood
        case "watch":  return Theme.colorRagWatch
        case "warn":   return Theme.colorWarn
        case "fault":  return Theme.colorRagFault
        case "error":  return Theme.colorError
        case "dim":    return Theme.colorText3
        case "none":   return Theme.colorRagNone
        }
        return Theme.colorText
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - Theme.sp(48)
        visible: !root._found
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("Select a row to see what it connects to")
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        color:          Theme.colorText3
    }

    // The fold, at this pane's own leading edge — it collapses rightwards, out of the way of the
    // table, so it points the way it goes. Anchored to the PANE, not placed in the header: the
    // header comes and goes with the selection and this must not.
    Rectangle {
        id: foldButton
        anchors.left:       parent.left
        anchors.top:        parent.top
        anchors.leftMargin: Theme.sp(14)
        anchors.topMargin:  Theme.sp(12)
        z: 2
        width:  Theme.sp(24)
        height: Theme.sp(20)
        radius: Theme.radius
        color:  foldMa.containsMouse ? Theme.colorBg2 : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Text {
            anchors.centerIn: parent
            text: "››"
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzBody2
            color: foldMa.containsMouse ? Theme.colorText2 : Theme.colorText3
        }

        ToolTip.visible: foldMa.containsMouse
        ToolTip.text: qsTr("Hide the inspector")
                      + (Qt.platform.os === "osx" ? "  ⌘⇧\\" : "  Ctrl+Shift+\\")
        ToolTip.delay: 400

        PpPressable { id: foldMa; hoverScale: 1.0; onClicked: root.collapseRequested() }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: root._found

        // ── Header ────────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(18)
            // Room at the top for the fold control, which floats over this pane rather than sitting
            // in the header — the header only exists when something is selected, and a pane you can
            // only fold while it has content is a pane that traps you on an empty one.
            Layout.topMargin:    Theme.sp(38)
            Layout.bottomMargin: Theme.sp(12)
            spacing: Theme.sp(5)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Text {
                    text:                root.detail.eyebrow || ""
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color:               Theme.colorText3
                }

                Item { Layout.fillWidth: true }

                // Whose content this is. An author has to know before they change it, not after.
                Text {
                    text: root.detail.source === "yours"       ? qsTr("Yours")
                        : root.detail.source === "both"        ? qsTr("Yours, over shipped")
                                                               : qsTr("Shipped")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: root.detail.source === "shipped" ? Theme.colorText3 : Theme.colorAccent
                }

                Text {
                    text:    qsTr("edited")
                    visible: root.detail.dirty === true
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorAccent
                }
            }

            Text {
                Layout.fillWidth: true
                text:           root.detail.label || ""
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzHeading
                font.weight:    Theme.fontBodyWeight
                color:          Theme.colorText
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text:           root.detail.subtitle || ""
                visible:        text.length > 0
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                elide:          Text.ElideRight
            }

            Flow {
                Layout.fillWidth: true
                Layout.topMargin: Theme.sp(5)
                spacing: Theme.sp(6)

                Repeater {
                    model: root.detail.badges || []
                    delegate: Rectangle {
                        id: badge
                        required property var modelData

                        implicitWidth:  badgeText.implicitWidth + Theme.sp(16)
                        implicitHeight: Theme.sp(20)
                        radius:         height / 2
                        color:          "transparent"
                        border.width:   1
                        border.color:   modelData.tone ? root._toneColor(modelData.tone)
                                                       : Theme.colorBorderStrong

                        Text {
                            id: badgeText
                            anchors.centerIn: parent
                            text: badge.modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color: badge.modelData.tone ? root._toneColor(badge.modelData.tone)
                                                        : Theme.colorText2
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        // The corridor picture, above the sections — for a corridor it IS the page, and reading it
        // is the reason anybody opened this row.
        ModelCorridorPlot {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(14)
            Layout.rightMargin: Theme.sp(14)
            Layout.topMargin:   Theme.sp(10)
            visible:  root.detail && root.detail.type === "corridors"
            plotSource: root.corridorPlotSource
            editable:   root.editable
            onHandleCommitted: (handle, v) => root.corridorHandleCommitted(handle, v)
            onFieldCommitted:  (f, t) => root.corridorFieldCommitted(f, t)
            onScanRequested:   root.corridorScanRequested()
        }

        // ── Sections ──────────────────────────────────────────────────────────
        ScrollView {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            clip: true
            contentWidth: availableWidth

            Column {
                width: parent.width
                spacing: 0

                Repeater {
                    model: root.detail.sections || []
                    delegate: Column {
                        id: sectionItem
                        required property var modelData

                        width: parent.width
                        spacing: Theme.sp(6)
                        bottomPadding: Theme.sp(12)
                        topPadding:    Theme.sp(12)

                        RowLayout {
                            width: sectionItem.width - Theme.sp(36)
                            x:     Theme.sp(18)
                            spacing: Theme.sp(8)

                            Text {
                                Layout.fillWidth:    true
                                text:                sectionItem.modelData.title
                                font.family:         Theme.fontBody
                                font.pixelSize:      Theme.fontSzMicro
                                font.letterSpacing:  Theme.trackingMicro
                                font.capitalization: Font.AllUppercase
                                color:               Theme.colorText3
                                elide:               Text.ElideRight
                            }

                            Text {
                                visible: sectionItem.modelData.kind === "list"
                                text:    sectionItem.modelData.count
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }

                        // The note explains an EMPTY section rather than leaving a blank: "no norm
                        // resolves for this measure" is a finding, and a gap with no explanation is
                        // indistinguishable from a rendering fault.
                        Text {
                            visible: sectionItem.modelData.note !== ""
                            x:     Theme.sp(18)
                            width: sectionItem.width - Theme.sp(36)
                            text:  sectionItem.modelData.note
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            wrapMode:       Text.WordWrap
                        }

                        Repeater {
                            model: sectionItem.modelData.rows
                            delegate: Item {
                                id: hubRow
                                required property var modelData

                                readonly property string kind: sectionItem.modelData.kind

                                width:  sectionItem.width
                                height: kind === "fields" ? fieldEditor.implicitHeight + Theme.sp(10)
                                      : kind === "prose" ? proseText.implicitHeight + Theme.sp(4)
                                      : kind === "quote" ? quoteBlock.implicitHeight + Theme.sp(10)
                                      : kind === "claims" ? Math.max(Theme.sp(34),
                                                                     rowLabel.implicitHeight + Theme.sp(10))
                                      : Math.max(Theme.sp(24), rowLabel.implicitHeight + Theme.sp(8))

                                Rectangle {
                                    anchors.fill: parent
                                    color: hubHover.hovered && hubRow.modelData.navigable
                                               ? Theme.colorBg2 : "transparent"
                                }

                                HoverHandler { id: hubHover }

                                // A quoted record — the citation as it reads, set apart so it is
                                // legible as one thing rather than as a paragraph of prose.
                                Rectangle {
                                    id: quoteBlock
                                    visible: hubRow.kind === "quote"
                                    x:      Theme.sp(18)
                                    width:  sectionItem.width - Theme.sp(36)
                                    implicitHeight: quoteText.implicitHeight + Theme.sp(20)
                                    height: implicitHeight
                                    radius: Theme.radius
                                    color:  Theme.colorBg2

                                    Text {
                                        id: quoteText
                                        anchors.left:   parent.left
                                        anchors.right:  parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin:  Theme.sp(12)
                                        anchors.rightMargin: Theme.sp(12)
                                        text: hubRow.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        color:          Theme.colorText2
                                        wrapMode:       Text.WordWrap
                                        lineHeight:     1.35
                                    }
                                }

                                Text {
                                    id: proseText
                                    visible: sectionItem.modelData.kind === "prose"
                                    x:     Theme.sp(18)
                                    width: sectionItem.width - Theme.sp(36)
                                    text:  hubRow.modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    font.weight:    Theme.fontBodyWeight
                                    color: hubRow.modelData.tone
                                               ? root._toneColor(hubRow.modelData.tone)
                                               : Theme.colorText2
                                    wrapMode:  Text.WordWrap
                                    lineHeight: 1.35
                                }

                                // A binding row: indented to its depth in the context tree, because
                                // "beneath partial swing" is the whole meaning of the row, and
                                // clicking cycles its state.
                                RowLayout {
                                    visible: sectionItem.modelData.kind === "bindings"
                                    anchors.fill: parent
                                    anchors.leftMargin:  Theme.sp(18)
                                        + Theme.sp(12) * (hubRow.modelData.depth || 0)
                                    anchors.rightMargin: Theme.sp(18)
                                    spacing: Theme.sp(9)

                                    Text {
                                        Layout.fillWidth: true
                                        text: hubRow.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        // Emphasis is colour, never weight: a row of the author's
                                        // own reads as stated, an inherited one as repeated.
                                        color: hubRow.modelData.own ? Theme.colorText
                                                                    : Theme.colorText3
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: hubRow.modelData.detail
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color: hubRow.modelData.tone === "warn" ? Theme.colorWarn
                                                                                : Theme.colorText3
                                        elide: Text.ElideRight
                                        Layout.maximumWidth: sectionItem.width * 0.5
                                    }

                                }

                                // OUTSIDE the RowLayout, deliberately. PpPressable fills its parent
                                // by anchor, so as a direct child of a layout it is both managed and
                                // anchored — Qt warns, and the click target ends up being one slot
                                // of the row rather than the whole row.
                                PpPressable {
                                    hoverScale: 1.0
                                    enabled:    root.editable
                                                && sectionItem.modelData.kind === "bindings"
                                    onClicked: {
                                        var a   = hubRow.modelData.applicable
                                        var m   = hubRow.modelData.material
                                        var own = hubRow.modelData.own
                                        // applies → not counted → does not apply → inherits
                                        if (!own)        root.bindingCycled(hubRow.modelData.id, true,  false, false)
                                        else if (a && m) root.bindingCycled(hubRow.modelData.id, true,  false, false)
                                        else if (a)      root.bindingCycled(hubRow.modelData.id, false, false, false)
                                        else             root.bindingCycled(hubRow.modelData.id, true,  true,  true)
                                    }
                                }

                                // A FIELD row: label above, editor below. Every writable field of
                                // every type comes through here — the inspector is where an author
                                // expects to see and change everything an object holds, and the
                                // table is the fast path for the few that fit in a column.
                                ColumnLayout {
                                    id: fieldEditor
                                    objectName: "fieldBox"
                                    visible: hubRow.kind === "fields"
                                    x:     Theme.sp(18)
                                    y:     Theme.sp(5)
                                    width: sectionItem.width - Theme.sp(36)
                                    // A Layout that is not itself inside a Layout gets NO size from
                                    // anywhere — `width` was set and `height` was not, so this box
                                    // was 0 high and laid its children out into nothing: the label
                                    // painted, the control under it did not, and the field read as
                                    // present but dead. Both dimensions have to be stated.
                                    height: implicitHeight
                                    spacing: Theme.sp(3)

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.sp(6)

                                        Text {
                                            text: hubRow.modelData.label
                                            font.family:         Theme.fontBody
                                            font.pixelSize:      Theme.fontSzMicro
                                            font.letterSpacing:  Theme.trackingMicro
                                            font.capitalization: Font.AllUppercase
                                            color:               Theme.colorText3
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text:    hubRow.modelData.detail || ""
                                            visible: text.length > 0
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzMicro
                                            color:          Theme.colorText3
                                            opacity: 0.8
                                            elide:   Text.ElideRight
                                        }
                                    }

                                    // text · number — one line, commits on Enter or focus-out, the
                                    // same contract the table's inline editor keeps.
                                    PpTextField {
                                        objectName: "fieldEditor:" + hubRow.modelData.field + ":text"
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Theme.sp(30)
                                        visible: hubRow.modelData.kind === "text"
                                                 || hubRow.modelData.kind === "number"
                                        enabled: root.editable
                                        text: hubRow.modelData.value !== undefined
                                                  ? String(hubRow.modelData.value) : ""
                                        horizontalAlignment: hubRow.modelData.kind === "number"
                                                                 ? TextInput.AlignRight
                                                                 : TextInput.AlignLeft
                                        font.family: hubRow.modelData.kind === "number"
                                                         ? Theme.fontData : Theme.fontBody
                                        onEditingFinished: {
                                            var was = hubRow.modelData.value !== undefined
                                                          ? String(hubRow.modelData.value) : ""
                                            // Nothing is written unless something changed. A pane
                                            // that pushed a command every time focus moved would
                                            // fill the undo stack with edits nobody made.
                                            if (text !== was)
                                                root.fieldCommitted(hubRow.modelData.field, text)
                                        }
                                    }

                                    // prose — the reason this kind exists. A paragraph has never
                                    // belonged in a table cell, which is why these fields had no
                                    // control at all until now.
                                    Rectangle {
                                        objectName: "fieldEditor:" + hubRow.modelData.field + ":prose"
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: Math.max(Theme.sp(56),
                                                                         proseEdit.implicitHeight
                                                                         + Theme.sp(12))
                                        visible: hubRow.modelData.kind === "prose"
                                        radius:  Theme.radius
                                        color:   Theme.colorSurface
                                        border.width: 1
                                        border.color: proseEdit.activeFocus ? Theme.colorAccent
                                                                            : Theme.colorBorderStrong

                                        TextEdit {
                                            id: proseEdit
                                            anchors.fill: parent
                                            anchors.margins: Theme.sp(8)
                                            enabled: root.editable
                                            text: hubRow.modelData.value !== undefined
                                                      ? String(hubRow.modelData.value) : ""
                                            wrapMode: TextEdit.Wrap
                                            selectByMouse: true
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzBody2
                                            color:          Theme.colorText
                                            // Focus-out, never per-keystroke: a paragraph typed a
                                            // character at a time would be a hundred commands.
                                            onActiveFocusChanged: {
                                                if (activeFocus) return
                                                var was = hubRow.modelData.value !== undefined
                                                              ? String(hubRow.modelData.value) : ""
                                                if (text !== was)
                                                    root.fieldCommitted(hubRow.modelData.field, text)
                                            }
                                        }
                                    }

                                    // enum — the same box a text field gets, opened by clicking
                                    // it. See ModelEnumField.qml for why this is not a segmented
                                    // control any more.
                                    ModelEnumField {
                                        objectName: "fieldEditor:" + hubRow.modelData.field + ":enum"
                                        Layout.fillWidth: true
                                        visible: hubRow.modelData.kind === "enum"
                                        enabled: root.editable
                                        options: hubRow.modelData.options || []
                                        value:   hubRow.modelData.value
                                        onPicked: (v) => root.fieldCommitted(hubRow.modelData.field, v)
                                    }
                                }

                                // An ACTION row: a button, not a link. Drawn as one so nobody expects
                                // it to navigate.
                                RowLayout {
                                    visible: hubRow.kind === "actions"
                                    anchors.fill: parent
                                    anchors.leftMargin:  Theme.sp(18)
                                    anchors.rightMargin: Theme.sp(18)
                                    spacing: Theme.sp(9)

                                    Rectangle {
                                        implicitWidth:  actionLbl.implicitWidth + Theme.sp(18)
                                        implicitHeight: Theme.sp(24)
                                        radius: Theme.radius
                                        color:  actionMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
                                        border.width: 1
                                        border.color: Theme.colorBorderMid
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                        Text {
                                            id: actionLbl
                                            anchors.centerIn: parent
                                            text: hubRow.modelData.label
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzMicro
                                            color:          Theme.colorText2
                                        }
                                        PpPressable {
                                            id: actionMa
                                            hoverScale: 1.0
                                            onClicked: root.rowActionRequested(hubRow.modelData.id,
                                                                               hubRow.modelData.label)
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: hubRow.modelData.detail
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }

                                // A CLAIM row: navigable like any relationship row, with the one
                                // control on this page that is live. The reference is imported; the
                                // claim resting on it is ours.
                                RowLayout {
                                    visible: hubRow.kind === "claims"
                                    anchors.fill: parent
                                    anchors.leftMargin:  Theme.sp(18)
                                    anchors.rightMargin: Theme.sp(18)
                                    spacing: Theme.sp(9)

                                    Text {
                                        Layout.fillWidth: true
                                        text: hubRow.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color:          Theme.colorText
                                        elide:          Text.ElideRight

                                        PpPressable {
                                            hoverScale: 1.0
                                            enabled:    hubRow.modelData.id !== undefined
                                            onClicked:  root.navigate(hubRow.modelData.type,
                                                                      hubRow.modelData.id)
                                        }
                                    }

                                    // Only an EDGE carries a strength. A condition's own provenance
                                    // is in the same list because it rests on the same paper, and it
                                    // is left alone rather than given a control that would write
                                    // nowhere.
                                    ModelEnumField {
                                        id: strengthControl
                                        visible: root.editable
                                                 && hubRow.modelData.options !== undefined
                                        Layout.preferredWidth: Theme.sp(168)
                                        Layout.minimumWidth:   Theme.sp(140)
                                        options: hubRow.modelData.options || []
                                        value:   hubRow.modelData.strength
                                        onPicked: (v) => root.claimStrengthChanged(hubRow.modelData.id, v)
                                    }

                                    Text {
                                        visible: !strengthControl.visible
                                        text:    hubRow.modelData.detail || ""
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }
                                }

                                RowLayout {
                                    visible: hubRow.kind !== "prose" && hubRow.kind !== "bindings"
                                             && hubRow.kind !== "quote" && hubRow.kind !== "actions"
                                             && hubRow.kind !== "claims" && hubRow.kind !== "fields"
                                    anchors.fill: parent
                                    anchors.leftMargin:  Theme.sp(18)
                                    anchors.rightMargin: Theme.sp(18)
                                    spacing: Theme.sp(9)

                                    Rectangle {
                                        Layout.alignment: Qt.AlignVCenter
                                        implicitWidth:  Theme.sp(6)
                                        implicitHeight: Theme.sp(6)
                                        radius:  width / 2
                                        visible: hubRow.modelData.tone !== ""
                                        color:   root._toneColor(hubRow.modelData.tone)
                                    }

                                    // fillWidth + elide, with the detail as a nowrap sibling. The
                                    // label MUST be the filling child or the sibling collapses it
                                    // to nothing at narrow widths.
                                    Text {
                                        id: rowLabel
                                        Layout.fillWidth: true
                                        text: hubRow.modelData.label
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color: hubRow.modelData.navigable ? Theme.colorText
                                                                          : Theme.colorText2
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        Layout.maximumWidth: sectionItem.width * 0.42
                                        text:    hubRow.modelData.detail
                                        visible: text.length > 0
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzMicro
                                        color:          Theme.colorText3
                                        elide:          Text.ElideRight
                                    }

                                    // Remove is inline and undoable — never a confirm dialog, and
                                    // never unrecoverable. Offered only where the section says it
                                    // can be, by its action KEY rather than by its title.
                                    Text {
                                        text:    "×"
                                        visible: root.editable && hubHover.hovered
                                                 && (sectionItem.modelData.action === "cause"
                                                     || sectionItem.modelData.action === "measure"
                                                     || sectionItem.modelData.action === "settles"
                                                     || sectionItem.modelData.action === "answers")
                                        font.family:    Theme.fontData
                                        font.pixelSize: Theme.fontSzBody
                                        color:          Theme.colorText3
                                        PpPressable {
                                            hoverScale: 1.0
                                            onClicked: root.removeRowRequested(
                                                           sectionItem.modelData.action,
                                                           hubRow.modelData.id)
                                        }
                                    }
                                }

                                PpPressable {
                                    hoverScale: 1.0
                                    // A claims row draws its own click target on the label, so the
                                    // whole-row one would sit over the strength control.
                                    enabled:    hubRow.modelData.navigable
                                                && hubRow.kind !== "claims"
                                    onClicked:  root.navigate(hubRow.modelData.type,
                                                              hubRow.modelData.id)
                                }
                            }
                        }

                        // "+ add" lives at the foot of the section it adds to, so the affordance is
                        // where the thing goes. Click, type three characters, Enter.
                        Text {
                            id: addAffordance
                            visible: root.editable && sectionItem.modelData.action !== ""
                                     && sectionItem.modelData.action !== "binding"
                            x: Theme.sp(18)
                            topPadding: Theme.sp(4)
                            text: sectionItem.modelData.action === "cause"    ? qsTr("+ add cause")
                                : sectionItem.modelData.action === "corridor" ? qsTr("+ add corridor")
                                : sectionItem.modelData.action === "settles"  ? qsTr("+ settles a characteristic")
                                : sectionItem.modelData.action === "answers"  ? qsTr("+ answers a characteristic")
                                                                              : qsTr("+ add measure")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorAccent
                            PpPressable {
                                hoverScale: 1.0
                                onClicked: {
                                    // Recorded before the signal, so whoever handles it knows where
                                    // the click was. Bottom-left of the affordance: the picker hangs
                                    // off it the way a menu hangs off the thing that opened it.
                                    root.actionOrigin = addAffordance.mapToItem(
                                        root, 0, addAffordance.height)
                                    if (sectionItem.modelData.action === "cause")
                                        root.addCauseRequested()
                                    else if (sectionItem.modelData.action === "corridor")
                                        root.addCorridorRequested()
                                    else if (sectionItem.modelData.action === "settles"
                                             || sectionItem.modelData.action === "answers")
                                        root.addRowRequested(sectionItem.modelData.action)
                                    else
                                        root.addMeasureRequested()
                                }
                            }
                        }

                        Rectangle {
                            width:   sectionItem.width
                            height:  1
                            color:   Theme.colorBorder
                            opacity: Theme.borderOpacityNormal
                        }
                    }
                }
            }
        }

        // ── Copy · Delete ─────────────────────────────────────────────────────
        //
        // Words, not glyphs, and at the foot of the pane that shows the object they act on. They
        // were two icons on the context bar, which put a destructive action in the chrome next to
        // controls that only change what you are LOOKING at — and asked the reader to know what ⧉
        // meant. Delete wears colorError because that is how this app draws a write that removes
        // something (DagView's destructive menu row), and because "Move to trash" being recoverable
        // is a reason to allow it, not a reason to hide it.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            visible: root.editable && root._found
            color:   Theme.colorBorder
            opacity: Theme.borderOpacityNormal
        }

        RowLayout {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(18)
            Layout.topMargin:    Theme.sp(10)
            Layout.bottomMargin: Theme.sp(12)
            visible: root.editable && root._found
            spacing: Theme.sp(8)

            Item { Layout.fillWidth: true }

            Rectangle {
                implicitWidth:  copyLbl.implicitWidth + Theme.sp(22)
                implicitHeight: Theme.sp(28)
                radius: Theme.radius
                color:  copyMa.containsMouse ? Theme.colorBg3 : Theme.colorBg2
                border.width: 1
                border.color: Theme.colorBorderMid
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: copyLbl
                    anchors.centerIn: parent
                    text: qsTr("Copy")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText2
                }
                ToolTip.visible: copyMa.containsMouse
                ToolTip.text: Qt.platform.os === "osx" ? qsTr("Duplicate this  ⌘D")
                                                       : qsTr("Duplicate this  Ctrl+D")
                ToolTip.delay: 400
                PpPressable { id: copyMa; hoverScale: 1.0; onClicked: root.duplicateRequested() }
            }

            Rectangle {
                implicitWidth:  delLbl.implicitWidth + Theme.sp(22)
                implicitHeight: Theme.sp(28)
                radius: Theme.radius
                color:  delMa.containsMouse ? Theme.colorErrorLight : "transparent"
                border.width: 1
                border.color: Theme.colorError
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: delLbl
                    anchors.centerIn: parent
                    text: qsTr("Delete")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorError
                }
                ToolTip.visible: delMa.containsMouse
                ToolTip.text: qsTr("Move to trash — ⌘Z brings it back")
                ToolTip.delay: 400
                PpPressable { id: delMa; hoverScale: 1.0; onClicked: root.removeRequested() }
            }
        }
    }
}
