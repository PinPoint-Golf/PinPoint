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

// Settings → Reference → Diagnostics. Master (library) → detail, mirroring MetricLibrary.qml.
//
// Read-only in this pass. The authoring surface (MeasurePicker, the sentence editor) and the
// roadmap/health views land separately; the model already exposes their data.
Item {
    id: root

    CharacteristicLibraryModel { id: library }

    // ── view state ────────────────────────────────────────────────────────────
    property string _groupFilter: ""    // "" = all groups
    property string _reachFilter: ""    // "" = all reaches
    property string _selectedId:  ""    // "" = directory (master)

    // Settings-search hook (ScreenSettings.navigateToResult): return to the directory and report
    // success so the retry loop stops.
    function scrollToItem(itemId) {
        root._selectedId = ""
        return true
    }

    // Deep link from elsewhere in the app. An unknown id is ignored, so a stale link lands on the
    // directory rather than a blank page.
    function showCharacteristic(conditionId) {
        if (!conditionId || conditionId.length === 0) return false
        if (Object.keys(library.detail(conditionId)).length === 0) return false
        root._selectedId = conditionId
        return true
    }

    function _filters() {
        var f = { observableOnly: true }
        if (root._groupFilter.length > 0) f.group = root._groupFilter
        if (root._reachFilter.length > 0) f.reach = root._reachFilter
        return f
    }

    function _rowsFor(group) {
        var f = root._filters()
        f.group = group
        var rows = library.query(f)

        // Group the two tails of an axis together so the library reads as a list of axes rather
        // than a list of near-duplicate faults. The partner immediately follows its sibling.
        var seen = {}
        var out  = []
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (seen[r.id]) continue
            seen[r.id] = true
            out.push(r)
            if (r.axisPartner && r.axisPartner.length > 0) {
                for (var j = 0; j < rows.length; ++j) {
                    if (rows[j].id === r.axisPartner && !seen[rows[j].id]) {
                        seen[rows[j].id] = true
                        out.push(rows[j])
                    }
                }
            }
        }
        return out
    }

    readonly property int _totalCount: library.query(root._filters()).length

    // ══ Directory (master) ════════════════════════════════════════════════════
    ScrollView {
        id: scrollView
        anchors.fill: parent
        contentWidth: availableWidth
        visible: root._selectedId === ""

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(28)
            width:   scrollView.availableWidth - Theme.sp(64)
            spacing: Theme.sp(20)

            // ── Page header ────────────────────────────────────────────────────
            Text {
                text:                qsTr("REFERENCE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText { text: qsTr("Swing diagnostics") }

            Text {
                Layout.fillWidth: true
                text: qsTr("Named conditions detected from measures, each with what it costs the "
                           + "golfer and what usually causes it. Faults and causes are the same "
                           + "kind of thing — which one a condition is depends on the swing.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText2
                wrapMode:       Text.WordWrap
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("%1 characteristics · %2 causes · %3 causal links")
                        .arg(library.characteristicCount)
                        .arg(library.causeCount)
                        .arg(library.edgeCount)
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }

            // ── Group filter chips ─────────────────────────────────────────────
            Flow {
                Layout.fillWidth: true
                spacing: Theme.sp(8)

                Rectangle {
                    readonly property bool active: root._groupFilter === ""
                    implicitWidth:  allText.implicitWidth + Theme.sp(20)
                    implicitHeight: Theme.sp(26)
                    radius: height / 2
                    color:  active ? Theme.colorAccent : Theme.colorBg2

                    Text {
                        id: allText
                        anchors.centerIn: parent
                        text:           qsTr("All")
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          parent.active ? Theme.colorBg : Theme.colorText2
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._groupFilter = ""
                    }
                }

                Repeater {
                    model: library.groups
                    delegate: Rectangle {
                        required property var modelData
                        readonly property bool active: root._groupFilter === modelData.name

                        implicitWidth:  chipText.implicitWidth + Theme.sp(20)
                        implicitHeight: Theme.sp(26)
                        radius: height / 2
                        color:  active ? Theme.colorAccent : Theme.colorBg2

                        Text {
                            id: chipText
                            anchors.centerIn: parent
                            text:           modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          parent.active ? Theme.colorBg : Theme.colorText2
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root._groupFilter = parent.active ? "" : modelData.name
                        }
                    }
                }
            }

            // ── Empty state ────────────────────────────────────────────────────
            Text {
                Layout.fillWidth: true
                visible:        root._totalCount === 0
                text:           qsTr("Nothing matches these filters.")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText3
            }

            // ── Groups ─────────────────────────────────────────────────────────
            Repeater {
                model: library.groups
                delegate: ColumnLayout {
                    id: groupBlock
                    required property var modelData
                    readonly property var rows: root._rowsFor(modelData.name)

                    Layout.fillWidth: true
                    spacing: Theme.sp(4)
                    visible: rows.length > 0

                    Text {
                        text:                modelData.label
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color:               Theme.colorText3
                        bottomPadding:       Theme.sp(4)
                    }

                    Repeater {
                        model: groupBlock.rows
                        delegate: CharacteristicRow {
                            required property var modelData
                            Layout.fillWidth: true
                            characteristic: modelData
                            onClicked: root._selectedId = modelData.id
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true; implicitHeight: Theme.sp(24) }
        }
    }

    // ══ Detail ════════════════════════════════════════════════════════════════
    CharacteristicDetail {
        anchors.fill: parent
        visible: root._selectedId !== ""
        detail:  root._selectedId !== "" ? library.detail(root._selectedId) : ({})

        onBack: root._selectedId = ""
        // Causes are conditions, so following one is just another detail page.
        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }
    }
}
