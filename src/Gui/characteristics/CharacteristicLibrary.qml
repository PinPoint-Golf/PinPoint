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
        // EVERY write through the editor lands here — `reload()` is what save(), linkCause,
        // unlinkCause and the three relation calls all finish with — so this is the one place the
        // reader's façade has to re-take its provider.
        //
        // Bumping the revision alone was not enough and looked like it was: the bindings re-ran, but
        // against the pack `library` was BUILT with, since the façade caches its provider and the
        // editor writes through one of its own. A new causal link was on disk, in the editor's view
        // of the world, and invisible on the page that had just asked for it until the next launch.
        function onLibraryChanged() {
            library.refresh()
            root._revision++
        }
    }

    // Running from a build tree rather than a shipped artefact. This is the app's EXISTING
    // dev-build signal — the one the updater already computes per platform ($APPIMAGE unset
    // on Linux, App-Translocation/bundle checks on macOS, the unins000.exe probe on Windows)
    // and reports to Settings → General. Reused rather than adding a compile-time flag: a
    // second definition of "is this a developer build" is a second thing to get wrong.
    // (A macOS/Windows build made without its Sparkle reports "unsupported" instead, and so
    // reads as shipped here — which is the safe way round.)
    readonly property bool _developerBuild: updateController.state === "devbuild"

    // ── view state ────────────────────────────────────────────────────────────
    property string _groupFilter: ""    // "" = all groups
    property string _reachFilter: ""    // "" = all reaches
    property string _search:      ""    // free text over the directory
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

    // Deep link to one METRIC's detail page — the dashboard tile click-through, routed via
    // MetricRoute. It used to land on a settings panel of its own; the panel became a view here, so
    // the entry point moved with it rather than being dropped.
    function showMetric(key) {
        if (!key || key.length === 0) return false
        root._view = "metrics"
        metricLibraryView.showMetric(key)
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
        if (root._groupFilter.length > 0) f.group  = root._groupFilter
        if (root._reachFilter.length > 0) f.reach  = root._reachFilter
        if (root._search.length      > 0) f.search = root._search
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
    //
    // Three content views on the left, and the ROADMAP held apart on the right. The roadmap
    // is not content — it is a work queue for whoever is building the library, and sitting it
    // in the same run of chips claimed it was another way to read the same material. It is
    // developer-only, and drawn as an outline rather than a filled chip so it reads as an
    // aside to the row rather than a fourth peer of it.
    Item {
        id: switcherBar
        anchors.top:   parent.top
        anchors.left:  parent.left
        anchors.right: parent.right
        // Measured from the row, not fixed: the chips WRAP at a narrow panel width, and a fixed
        // height would let the second row spill over the view below it rather than pushing it down.
        height:  visible ? switcherRow.y + switcherRow.implicitHeight + Theme.sp(10) : 0
        // Detail and the editor are full-page and carry their own way back, so the bar steps aside.
        visible: root._selectedId === "" && root._selectedMeasureId === "" && !root._editing
                 && !root._corridor

        RowLayout {
            id: switcherRow
            x:       Theme.sp(32)
            y:       Theme.sp(24)
            width:   parent.width - Theme.sp(64)
            spacing: Theme.sp(12)

            Flow {
                id: switcherFlow
                Layout.fillWidth: true
                spacing: Theme.sp(6)

                Repeater {
                    // The order is the order somebody works in: what the library CLAIMS, the
                    // readings behind it, the metrics those readings come from, what is wrong with
                    // the lot, what to do about it, and finally what the words mean. Metrics used
                    // to be its own settings panel; it sits here because a metric, a measure and a
                    // corridor are one chain and reading them meant leaving the page.
                    model: [{ name: "library",  label: qsTr("Characteristics") },
                            { name: "measures", label: qsTr("Measures & norms") },
                            { name: "metrics",  label: qsTr("Metrics") },
                            { name: "health",   label: qsTr("Causes & health") },
                            { name: "screens",  label: qsTr("Drills") },
                            { name: "glossary", label: qsTr("Glossary") }]
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

            // ── References — always shown ──────────────────────────────────────
            // Same outline treatment as Roadmap, and to its LEFT so that in a shipping build (where
            // Roadmap is hidden) this is the far-right chip, while a developer build keeps Roadmap
            // beyond it. Unlike Roadmap it is never hidden: the sources behind a claim are the
            // user's business, not a maintainer's, and a library that cannot show its working is
            // asking to be taken on faith.
            Rectangle {
                id: refsChip
                Layout.alignment: Qt.AlignRight | Qt.AlignTop

                readonly property bool active: root._view === "references"

                implicitWidth:  refText.implicitWidth + Theme.sp(22)
                implicitHeight: Theme.sp(28)
                radius: height / 2
                color:  "transparent"
                border.width: 1
                border.color: active ? Theme.colorAccent
                                     : refMa.containsMouse ? Theme.colorBorderStrong
                                                           : Theme.colorBorderMid
                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: refText
                    anchors.centerIn: parent
                    text:           qsTr("References")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          refsChip.active ? Theme.colorAccent
                                                    : refMa.containsMouse ? Theme.colorText2
                                                                          : Theme.colorText3
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }
                MouseArea {
                    id: refMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root._view = "references"
                }
            }

            // ── Roadmap — developer builds only ────────────────────────────────
            // Outline, never a fill: quieter than the content chips at every state, but it
            // keeps a full-strength border and lifts to the accent when selected, so it is
            // still legibly a control and still legibly the one you are on.
            // Top-aligned so it stays beside the FIRST row when the chips wrap.
            Rectangle {
                id: roadmapChip
                visible: root._developerBuild
                Layout.alignment: Qt.AlignRight | Qt.AlignTop

                readonly property bool active: root._view === "roadmap"

                implicitWidth:  rmText.implicitWidth + Theme.sp(22)
                implicitHeight: Theme.sp(28)
                radius: height / 2
                color:  "transparent"
                border.width: 1
                border.color: active ? Theme.colorAccent
                                     : rmMa.containsMouse ? Theme.colorBorderStrong
                                                          : Theme.colorBorderMid
                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: rmText
                    anchors.centerIn: parent
                    text:           qsTr("Roadmap")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          roadmapChip.active ? Theme.colorAccent
                                                       : rmMa.containsMouse ? Theme.colorText2
                                                                            : Theme.colorText3
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }
                MouseArea {
                    id: rmMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root._view = "roadmap"
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

            // ── Search ─────────────────────────────────────────────────────────
            // Above the chips and the same width as the catalogue's, so the two directories
            // filter the same way in the same place.
            PpTextField {
                Layout.fillWidth: true
                Layout.maximumWidth: Theme.sp(380)
                placeholderText: qsTr("Search characteristics")
                text: root._search
                onTextChanged: root._search = text
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
    // The developer gate is on the VIEW as well as its chip: it belongs to the feature, not
    // to one way in, so a future deep link cannot route a shipped build here.
    RoadmapView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._developerBuild && root._view === "roadmap"
                 && root._selectedId === "" && !root._editing
        library: library
    }

    // ══ Metrics ═══════════════════════════════════════════════════════════════
    // Was Settings → Metrics, a panel of its own. A metric, the measures that read it and the
    // corridors that judge them are one chain, and following it meant leaving the page — so the
    // panel became a view, and `openNorm` now crosses to a sibling view instead of a sibling panel.
    MetricLibrary {
        id: metricLibraryView
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "metrics" && root._selectedId === "" && !root._editing

        onOpenNorm: function(measureId) { root.showMeasure(measureId) }
    }

    // ══ Glossary ══════════════════════════════════════════════════════════════
    // The rule set read out as plain language. No second dataset — see GlossaryView.qml.
    GlossaryView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "glossary" && root._selectedId === "" && !root._editing
        library: library

        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }
    }

    // ══ Screens & drills ══════════════════════════════════════════════════════
    ScreensView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "screens" && root._selectedId === "" && !root._editing
        library: library

        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }
    }

    // ══ References ════════════════════════════════════════════════════════════
    ReferencesView {
        anchors.top:    switcherBar.bottom
        anchors.left:   parent.left
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        visible: root._view === "references" && root._selectedId === "" && !root._editing
        library: library

        onOpenCondition: function(conditionId) { root.showCharacteristic(conditionId) }
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
        //
        // The façade's own re-take happens in the `onLibraryChanged` handler above, which every
        // write reaches; this is only the nudge for the bindings on THIS page.
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
