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
// Four views behind one switcher, answering four different questions:
//   Characteristics — what does the library know?
//   Measures        — what can it actually grade, and against what?
//   Roadmap         — what does it need that we have not built?
//   Causes & health — what should a coach do, and what is wrong with the content?
// Detail and the authoring sheet overlay whichever view is active.
Item {
    id: root

    CharacteristicLibraryModel {
        id: library
        // The health list spans the norm set, and the corpus check grades against it — so this model
        // needs both the policy and the library path, seeded from the ONE global AppSettings exactly
        // as its two siblings below are.
        libraryRoot: appSettings.athleteLibraryPath
        Component.onCompleted: gradePolicy = appSettings.diagnosticsGradePolicy
    }
    CharacteristicEditorModel  { id: editor }

    NormModel {
        id: norms
        // Persisted in Settings, applied pack-wide. Seeded here rather than in the model so the
        // model itself stays free of the settings dependency and remains testable standalone.
        // `appSettings` is the root context property — the ONE global instance from main.cpp, per
        // the single-shared-instance rule; never construct a local AppSettings for a write.
        Component.onCompleted: {
            gradePolicy = appSettings.diagnosticsGradePolicy
            // Which norm-set layers take part. Seeded here for the same reason, and applied BEFORE
            // anything queries: setDisabledNormSets() resets the process-wide provider, so doing it
            // later would throw away an assembly every open view is already rendering from.
            setDisabledNormSets(appSettings.diagnosticsNormSetsOff)
        }
    }

    // The corridor editor's draft. Separate from NormModel exactly as CharacteristicEditorModel is
    // separate from CharacteristicLibraryModel: one reads the library, one holds an edit.
    NormEditorModel {
        id: normEditor
        libraryRoot: appSettings.athleteLibraryPath
        Component.onCompleted: setGradePolicy(appSettings.diagnosticsGradePolicy)
    }

    Connections {
        target: normEditor
        // A corridor was written. The façade caches its provider, so it has to re-take before any
        // binding re-reads — otherwise the edit is on disk and invisible until the next launch.
        function onNormsChanged() {
            norms.refresh()
            // The health list reads the norm set too, so it has to re-take the same assembly — a
            // corridor edit can create or clear a health row (a norm now exists; your override was
            // rebased) and the list would otherwise answer from the pack it was built with.
            library.refresh()
            root._revision++
        }
    }

    Connections {
        target: appSettings
        function onDiagnosticsGradePolicyChanged() {
            norms.gradePolicy = appSettings.diagnosticsGradePolicy
            normEditor.setGradePolicy(appSettings.diagnosticsGradePolicy)
            library.gradePolicy = appSettings.diagnosticsGradePolicy
        }
    }

    // Bumped whenever a save lands, so every query() binding re-evaluates. The façade is read-only
    // by design, so an explicit nudge is cheaper and clearer than making it observable.
    property int _revision: 0

    Connections {
        target: editor
        function onLibraryChanged() { root._revision++ }
    }

    // ── view state ────────────────────────────────────────────────────────────
    property string _groupFilter: ""    // "" = all groups
    property string _reachFilter: ""    // "" = all reaches
    property string _selectedId:  ""    // "" = directory (master)
    property string _selectedMeasureId: ""  // "" = measure catalogue (master)
    property bool   _editing:     false // the authoring sheet is open
    property bool   _corridor:    false // the corridor editor is open, over the measure detail
    property string _view:        "library"   // "library" | "measures" | "roadmap" | "health"

    function _openCorridor(measureId, contextId) {
        if (normEditor.begin(measureId, contextId)) root._corridor = true
    }

    // Settings-search hook (ScreenSettings.navigateToResult): return to the directory and report
    // success so the retry loop stops.
    function scrollToItem(itemId) {
        root._selectedId        = ""
        root._selectedMeasureId = ""
        return true
    }

    // Deep link to one measure. Same contract as showCharacteristic: an unknown id is ignored so a
    // stale link lands on the catalogue rather than a blank page.
    function showMeasure(measureId) {
        if (!measureId || measureId.length === 0) return false
        if (Object.keys(norms.measureDetail(measureId)).length === 0) return false
        root._view              = "measures"
        root._selectedMeasureId = measureId
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
        var _ = root._revision            // re-query after a save
        var f = { observableOnly: true }
        if (root._groupFilter.length > 0) f.group = root._groupFilter
        if (root._reachFilter.length > 0) f.reach = root._reachFilter
        return f
    }

    // Rows for one group block. The directory renders a block per group and each asks for its own
    // rows, so the active chip has to be honoured HERE — setting f.group below would otherwise
    // overwrite the chip's selection and every block would render regardless of it.
    function _rowsFor(group) {
        if (root._groupFilter.length > 0 && root._groupFilter !== group) return []

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

    // ══ View switcher — persistent, outside every view ════════════════════════
    // Deliberately not inside any of the four views: a control that lives in the view it
    // switches away from vanishes on first use and strands the user wherever they landed.
    Item {
        id: switcherBar
        anchors.top:   parent.top
        anchors.left:  parent.left
        anchors.right: parent.right
        // Measured from the Flow, not fixed: the chips WRAP at a narrow panel width, and a fixed
        // height would let the second row spill over the view below it rather than pushing it down.
        height:  visible ? switcherFlow.y + switcherFlow.implicitHeight + Theme.sp(10) : 0
        // Detail and the editor are full-page and carry their own way back, so the bar steps aside.
        visible: root._selectedId === "" && root._selectedMeasureId === "" && !root._editing
                 && !root._corridor

        Flow {
            id: switcherFlow
            x:       Theme.sp(32)
            y:       Theme.sp(24)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(6)

            Repeater {
                model: [{ name: "library",  label: qsTr("Characteristics") },
                        { name: "measures", label: qsTr("Measures & norms") },
                        { name: "roadmap",  label: qsTr("Roadmap") },
                        { name: "health",   label: qsTr("Causes & health") }]
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool active: root._view === modelData.name

                    implicitWidth:  vsText.implicitWidth + Theme.sp(22)
                    implicitHeight: Theme.sp(28)
                    radius: height / 2
                    color:  active ? Theme.colorAccent : Theme.colorBg2

                    Text {
                        id: vsText
                        anchors.centerIn: parent
                        text:           modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          parent.active ? Theme.colorBg : Theme.colorText2
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._view = modelData.name
                    }
                }
            }
        }
    }

    // ══ Directory (master) ════════════════════════════════════════════════════
    ScrollView {
        id: scrollView
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        contentWidth: availableWidth
        // `!_editing` matters on the NEW path and only there: opening the editor from a detail page
        // sets _selectedId, which already hides this, but "New characteristic" leaves it empty — so
        // the authoring sheet rendered straight over the still-visible list, both readable at once.
        // The measures, roadmap and health views have always carried this guard.
        visible: root._selectedId === "" && root._view === "library" && !root._editing

        ColumnLayout {
            x:       Theme.sp(32)
            y:       Theme.sp(12)
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

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(12)

                PpDisplayText { text: qsTr("Swing diagnostics") }
                Item { Layout.fillWidth: true }
                PpButton {
                    label: qsTr("New characteristic")
                    onClicked: { editor.beginNew(); root._editing = true }
                }
            }

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

    // ══ Measures & norms ══════════════════════════════════════════════════════
    MeasureCatalogue {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "measures" && root._selectedMeasureId === "" && !root._editing

        norms: norms

        onOpenMeasure: function(measureId) { root._selectedMeasureId = measureId }

        // A measure exists to be read by a characteristic: mintMeasure() writes into the editor's
        // DRAFT, so minting one with no draft open would silently discard it, and an orphan measure
        // is a validator warning the moment it lands. So "New measure" starts a characteristic and
        // opens the picker on it — the same picker, in the same mint mode, with somewhere to land.
        onNewMeasureRequested: {
            editor.beginNew()
            root._editing = true
            conditionEditor.openMeasurePicker()
        }
    }

    // ══ Roadmap ═══════════════════════════════════════════════════════════════
    RoadmapView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "roadmap" && root._selectedId === "" && !root._editing
        library: library
    }

    // ══ Causes & health ═══════════════════════════════════════════════════════
    HealthView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "health" && root._selectedId === "" && !root._editing
        library: library

        // A health row names a subject; following it opens that characteristic, which is the only
        // useful thing to do with the name.
        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }

        // A row about a CORRIDOR opens the measure instead. That page carries the norm rows, the
        // shipped-vs-yours markers and the reset — which is where "take theirs" actually lives, next
        // to the numbers you would be taking, rather than as a bare button in a list.
        onOpenMeasure: function(measureId) { root.showMeasure(measureId) }
    }

    // ══ Detail ════════════════════════════════════════════════════════════════
    CharacteristicDetail {
        anchors.fill: parent
        visible: root._selectedId !== "" && !root._editing
        detail:  (root._selectedId !== "" && root._revision >= 0)
                 ? library.detail(root._selectedId) : ({})

        library: library
        // The graph's one editing affordance writes a single edge through the editor model. It is
        // handed the same instance the authoring sheet uses so both see one user pack.
        editor:  editor

        onBack: root._selectedId = ""
        // Causes are conditions, so following one is just another detail page.
        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }
        // A measure node leaves the graph for the measure's own page, which is where its corridor,
        // its norms and its other users are.
        onOpenMeasure: function(measureId) {
            root._selectedId = ""
            root.showMeasure(measureId)
        }
        // An edge was added or removed from the graph: the census, the directory rows and this
        // page's own detail map are all now stale.
        onGraphChanged: root._revision++
        onEdit: {
            if (editor.beginEdit(root._selectedId)) root._editing = true
        }
    }

    // ══ Measure detail ════════════════════════════════════════════════════════
    MeasureDetail {
        anchors.fill: parent
        visible: root._selectedMeasureId !== "" && !root._editing && !root._corridor
        detail:  (root._selectedMeasureId !== "" && root._revision >= 0)
                 ? norms.measureDetail(root._selectedMeasureId) : ({})

        onBack: root._selectedMeasureId = ""
        // Following a user of this measure lands on that characteristic's page, which is the only
        // useful thing to do with the name.
        onOpenCondition: function(conditionId) {
            root._selectedMeasureId = ""
            root._view              = "library"
            root.showCharacteristic(conditionId)
        }
        onEditCorridor: function(measureId, contextId) { root._openCorridor(measureId, contextId) }
    }

    // ══ Corridor editor ═══════════════════════════════════════════════════════
    CorridorEditor {
        anchors.fill: parent
        visible: root._corridor
        editor:  normEditor

        onBack: {
            normEditor.cancel()
            root._corridor = false
        }
    }

    // ══ Editor ════════════════════════════════════════════════════════════════
    CharacteristicEditor {
        id: conditionEditor
        anchors.fill: parent
        visible: root._editing
        editor:  editor

        onClosed: root._editing = false
        onSaved: {
            root._revision++
            // A new characteristic lands on its own detail page rather than dumping the author
            // back at the top of the directory.
            if (editor.draft.id) root._selectedId = editor.draft.id
        }
    }
}
