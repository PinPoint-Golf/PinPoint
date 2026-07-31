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
    signal removeRowRequested(string type, string id)
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

    ColumnLayout {
        anchors.fill: parent
        spacing: 0
        visible: root._found

        // ── Header ────────────────────────────────────────────────────────────
        ColumnLayout {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(18)
            Layout.rightMargin:  Theme.sp(18)
            Layout.topMargin:    Theme.sp(14)
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

                                width:  sectionItem.width
                                height: sectionItem.modelData.kind === "prose"
                                            ? proseText.implicitHeight + Theme.sp(4)
                                            : Math.max(Theme.sp(24), rowLabel.implicitHeight + Theme.sp(8))

                                Rectangle {
                                    anchors.fill: parent
                                    color: hubHover.hovered && hubRow.modelData.navigable
                                               ? Theme.colorBg2 : "transparent"
                                }

                                HoverHandler { id: hubHover }

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

                                RowLayout {
                                    visible: sectionItem.modelData.kind !== "prose"
                                             && sectionItem.modelData.kind !== "bindings"
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
                                                     || sectionItem.modelData.action === "measure")
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
                                    enabled:    hubRow.modelData.navigable
                                    onClicked:  root.navigate(hubRow.modelData.type,
                                                              hubRow.modelData.id)
                                }
                            }
                        }

                        // "+ add" lives at the foot of the section it adds to, so the affordance is
                        // where the thing goes. Click, type three characters, Enter.
                        Text {
                            visible: root.editable && sectionItem.modelData.action !== ""
                                     && sectionItem.modelData.action !== "binding"
                            x: Theme.sp(18)
                            topPadding: Theme.sp(4)
                            text: sectionItem.modelData.action === "cause"    ? qsTr("+ add cause")
                                : sectionItem.modelData.action === "corridor" ? qsTr("+ add corridor")
                                                                              : qsTr("+ add measure")
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorAccent
                            PpPressable {
                                hoverScale: 1.0
                                onClicked: {
                                    if (sectionItem.modelData.action === "cause")
                                        root.addCauseRequested()
                                    else if (sectionItem.modelData.action === "corridor")
                                        root.addCorridorRequested()
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
    }
}
