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

// Settings → Reference → Diagnostic Model. One shell, three panes: type rail → table → relationship
// inspector, with a trail of the chain you walked.
//
// It sits BESIDE the existing Diagnostics panel (CharacteristicLibrary.qml) and touches nothing in
// it. The intent is that this replaces it once its UX is better in every respect; until then both
// ship and the old one is the fallback.
//
// The organising rule: EVERY element of the model is editable in the fewest possible clicks. No
// modals, no edit mode, no Edit button. A click selects a row; a click on a row that is already
// selected edits the cell under it — see ModelTable.qml for why that differs from the brief, which
// asked for both in one click. Everything that changes the library goes through ModelBrowser, which
// holds an unsaved working copy, so one Save writes the lot and ⌘Z steps back through all of it.
Item {
    id: root

    ModelBrowser {
        id: browser
        // Seeded from the ONE global AppSettings, per the single-shared-instance rule. The policy
        // matters here beyond bookkeeping: every band edge in the corridor picture is
        // policy-derived, so drawing under the default while the user has chosen Strict would paint
        // a corridor the app does not grade against.
        libraryRoot: appSettings.athleteLibraryPath
        gradePolicy: appSettings.diagnosticsGradePolicy
    }

    // The façade is read through Q_INVOKABLEs, which are not properties and so cannot be bound to.
    // This is nudged by modelChanged and every list binding depends on it — the same shape
    // CharacteristicLibrary.qml uses, and for the same reason.
    property int _revision: 0

    Connections {
        target: browser
        function onModelChanged() { root._revision++ }
        // A save landed. Whoever else is holding a provider has to re-take it or the edit is on
        // disk and invisible until relaunch.
        function onLibraryChanged() { root._revision++ }
        // A reading landing has to redraw the picture without re-querying every table in the panel.
        function onCorridorSamplesChanged() { root._scanRevision++ }
    }

    // ── View state ────────────────────────────────────────────────────────────
    property string _type:       "characteristics"
    property string _view:       "table"      // "table" | "graph"
    property string _search:     ""
    property string _sort:       ""
    property bool   _descending: false
    property var    _facets:     ({})
    property string _selectedId:   ""
    property string _selectedType: ""
    property var    _selection:  []
    property bool   _editsOpen:  false
    // The chain walked to get here. Its terminal item is ALWAYS the current selection — that is
    // what makes it a trail rather than a history.
    property var    _trail:      []

    readonly property bool _searching: _search.trim().length > 0
    // Screens, drills, references and health have their own registries and their own write paths;
    // this panel does not write them, and a cell that looked live and did nothing would be worse
    // than one that plainly is not.
    // The types this panel writes. Corridors belong here as much as anything else does — leaving
    // them out made every corridor cell inert AND hid the plot's drag handles, so the whole editing
    // surface was there and dead. The list that is short is the read-only one below.
    readonly property bool _typeEditable:
        _type === "characteristics" || _type === "causes" || _type === "measures"
        || _type === "signals" || _type === "links" || _type === "corridors"

    // ── Responsive ────────────────────────────────────────────────────────────
    // The three panes plus the sidenav add up fast, and the name column is what gets starved.
    // The inspector is the pane that actually holds prose, a corridor plot and a list of links, so
    // it is the one that gets the room. Widened by half over the mockup's 352, which was drawn
    // before it carried a plot at all — and the threshold for dropping it moves with it, or the
    // panel would keep a pane it can no longer fit.
    readonly property int  _inspectorWidth: Theme.sp(528)
    readonly property bool _showFacets:     width > Theme.sp(1500 - 275)
    readonly property bool _showInspector:  width > Theme.sp(1326 - 275)

    // ── Data, all derived in C++ ──────────────────────────────────────────────
    readonly property var _columns: {
        root._revision
        return root._searching ? browser.columns("search") : browser.columns(root._type)
    }
    readonly property var _rows: {
        root._revision
        if (root._searching) return browser.searchAll(root._search)
        return browser.rows(root._type, { sort: root._sort, descending: root._descending,
                                          facets: root._facets })
    }
    readonly property int _totalForType: {
        root._revision
        var t = browser.types
        for (var i = 0; i < t.length; i++) if (t[i].key === root._type) return t[i].count
        return 0
    }
    // The corridor picture, as a FUNCTION of the options the plot asks with — its own canvas size,
    // and any uncommitted drag values. One source for the resting picture and every preview, so the
    // two cannot diverge, and the size is the plot's to state because it is the only thing that
    // knows it.
    //
    // Reads _revision and _scanRevision so a redraw follows an edit or a reading landing; the plot
    // re-invokes it on its own for a drag.
    readonly property var _corridorPlotSource: {
        root._revision
        root._scanRevision
        var type = root._selectedType
        var id   = root._selectedId
        return function (opts) {
            if (type !== "corridors" || id === "") return ({})
            var body = id.substring(5)
            var at   = body.lastIndexOf("@")
            if (at <= 0) return ({})
            return browser.corridorPlot(body.substring(0, at), body.substring(at + 1), opts || ({}))
        }
    }

    property int _scanRevision: 0

    readonly property var _inspectorDetail: {
        root._revision
        if (root._selectedId === "") return ({})
        return browser.inspect(root._selectedType, root._selectedId)
    }

    // ── Navigation ────────────────────────────────────────────────────────────

    function _typeOfRow(row) {
        // A search result knows its own type; an ordinary row's type is the table's.
        return row && row.resultType ? row.resultType : (row && row.type ? row.type : root._type)
    }

    function select(type, id, pushTrail) {
        root._selectedType = type
        root._selectedId   = id
        if (pushTrail === false) return

        // The terminal item is always the current selection. Re-selecting something already on the
        // trail truncates back to it rather than appending a second copy — otherwise walking a
        // cycle would grow the trail forever.
        var t = root._trail.slice()
        for (var i = 0; i < t.length; i++)
            if (t[i].id === id) { t = t.slice(0, i); break }
        t.push({ type: type, id: id, label: browser.inspect(type, id).label || id })
        if (t.length > 8) t = t.slice(t.length - 8)
        root._trail = t
    }

    // Navigating from the inspector may cross types — a measure's blast radius lands on a
    // characteristic — so the table follows the selection rather than the other way round.
    function navigateTo(type, id) {
        if (type === "" || id === "") return

        // Every navigable row carries a real TYPE KEY — the same strings the rail uses. The guard
        // here once excluded "cause" and "measure", singular, which were never type keys at all:
        // leftovers from the inspector's remove actions that could only ever be dead. A type that
        // does not match the rail silently leaves the table on the wrong list.
        if (type !== root._type) {
            root._type   = type
            root._search = ""
            searchField.text = ""
        }
        select(type, id, true)
        // Following a node clears any selected edge: the edge belonged to the picture you just
        // navigated away from.
        root._selectedEdgeId = ""
    }

    property string _selectedEdgeId: ""

    // When a CORRIDOR is selected, the measure it grades. Corridor actions are addressed by measure
    // plus context, and the row id carries both — this unpacks the half the pickers need.
    readonly property string _corridorMeasureId: {
        if (root._selectedType !== "corridors" || root._selectedId === "") return ""
        var body = root._selectedId.substring(5)
        var at   = body.lastIndexOf("@")
        return at > 0 ? body.substring(0, at) : ""
    }

    // Find whichever content type holds `subject` and select it there. A health row names an id and
    // nothing else — it does not know, and must not have to know, which registry that id lives in.
    function openSubject(subject) {
        if (!subject || subject === "") return
        var order = [ "characteristics", "causes", "measures", "signals", "links",
                      "screens", "drills", "references" ]
        for (var t = 0; t < order.length; t++) {
            var rows = browser.rows(order[t], { ids: [ subject ] })
            if (rows.length > 0) {
                root._type = order[t]
                root.select(order[t], subject, true)
                return
            }
        }
        // A subject that resolves nowhere is itself the finding — say so rather than doing nothing.
        toast.severity = "warn"
        toast.show(qsTr("Nothing in the model has the id %1").arg(subject))
    }

    function _report(result) {
        if (!result) return
        toast.severity = result.ok === true ? "info" : "warn"
        toast.show(result.message || "")
    }

    // ── Actions ───────────────────────────────────────────────────────────────

    function doCommit(id, field, value) {
        var type = root._searching ? root._selectedType : root._type
        // A multi-row selection with the edited row inside it is a BULK edit: the author selected
        // twelve rows for a reason, and applying the change to one of them would be the surprise.
        if (root._selection.length > 1 && root._selection.indexOf(id) >= 0)
            _report(browser.setFieldOnAll(type, root._selection, field, value))
        else
            _report(browser.setField(type, id, field, value))
    }

    function doSave()      { _report(browser.save()) }
    function doUndo()      { _report(browser.undo()) }
    function doRedo()      { _report(browser.redo()) }
    function doRevert()    { _report(browser.revert()) }

    function doDuplicate(id) {
        var r = browser.duplicate(root._type, id)
        _report(r)
        if (r.ok === true) select(r.type, r.id, true)
    }

    function doRemove(id) {
        if (root._type === "links") {
            var row = null
            for (var i = 0; i < root._rows.length; i++)
                if (root._rows[i].id === id) row = root._rows[i]
            if (!row) return
            var rel = ""
            for (var c = 0; c < row.cells.length; c++)
                if (row.cells[c].field === "relation") rel = row.cells[c].value
            _report(browser.removeLink(row.fromId, row.toId, rel))
        } else {
            _report(browser.removeObject(root._type, id))
        }
    }

    // ── Deep links ────────────────────────────────────────────────────────────
    //
    // The dashboard's metric tiles route here through MetricRoute. They used to land in Diagnostics;
    // that panel is hidden now, so a link into it would open a page with no row selected in the
    // sidenav — which reads as the app losing its place.
    //
    // Each of these is "select this object", because that is all navigation in this panel IS: the
    // type rail follows the selection and the inspector follows it too.
    function showMetric(key) {
        root._search = ""
        root._view   = "table"
        root._type   = "metrics"
        root.select("metrics", key, true)
    }

    function showCharacteristic(conditionId) {
        root._search = ""
        root._view   = "table"
        root._type   = "characteristics"
        root.select("characteristics", conditionId, true)
    }

    function showMeasure(measureId) {
        root._search = ""
        root._view   = "table"
        root._type   = "measures"
        root.select("measures", measureId, true)
    }

    function scrollToItem(itemId) {
        // Settings-search hook (ScreenSettings.navigateToResult). It has to answer, or the retry
        // loop in scrollWithRetry() spins three times and gives up silently.
        return true
    }

    Rectangle { anchors.fill: parent; color: Theme.colorBg }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Page header ───────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth:   true
            Layout.leftMargin:  Theme.sp(24)
            Layout.rightMargin: Theme.sp(24)
            Layout.topMargin:   Theme.sp(12)
            spacing: Theme.sp(12)

            Text {
                text:                qsTr("REFERENCE")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            PpDisplayText { text: qsTr("Diagnostic Model") }

            Item { Layout.fillWidth: true }

            Text {
                text:           browser.packLabel
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        // ── Toolbar ───────────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth:    true
            Layout.leftMargin:   Theme.sp(24)
            Layout.rightMargin:  Theme.sp(24)
            Layout.topMargin:    Theme.sp(12)
            Layout.bottomMargin: Theme.sp(10)
            spacing: Theme.sp(10)

            PpTextField {
                id: searchField
                Layout.preferredWidth: Theme.sp(320)
                placeholderText: qsTr("Search every content type")
                onTextChanged: {
                    table.endEdit()
                    root._search = text
                    // A search answers across every type at once, which is a LIST — so asking for
                    // one puts you in the list, visibly, rather than silently swapping the pane out
                    // from under a control that still says otherwise.
                    if (text.trim().length > 0) root._view = "table"
                }
                Keys.onEscapePressed: { text = ""; root._search = "" }
            }

            ModelTrail {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.maximumWidth: Theme.sp(520)
                trail: root._trail
                visible: root._trail.length > 0 && !root._searching
                onStepPicked: (type, id) => root.navigateTo(type, id)
            }

            Item { Layout.fillWidth: true }

            // Unsaved state, said plainly. The count is OBJECTS that would be written, not
            // keystrokes — three edits to one label is one unsaved object.
            Text {
                text: qsTr("%n unsaved", "", browser.unsavedCount)
                visible: browser.dirty
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorAccent
            }

            // minimumWidth, not just preferredWidth: PpSegmentedControl sets an implicitHeight and
            // no implicitWidth, so a RowLayout under pressure will happily shrink it to nothing —
            // and the Table/Graph toggle would vanish rather than merely get narrow. It is the only
            // way into the graph pane besides the G key, so it must not be the thing that gives.
            PpSegmentedControl {
                Layout.preferredWidth: Theme.sp(150)
                Layout.minimumWidth:   Theme.sp(120)
                options: [ qsTr("Table"), qsTr("Graph") ]
                selected: root._view === "table" ? qsTr("Table") : qsTr("Graph")
                onActivated: (value) => root._view = (value === qsTr("Table") ? "table" : "graph")
            }

            // What "new" means depends on what you are looking at: a measure is built from facets,
            // a characteristic starts blank. One button, because "New" is one intent.
            PpButton {
                label:   qsTr("New")
                visible: root._typeEditable && root._type !== "links" && root._type !== "signals"
                         && root._type !== "corridors"
                onClicked: {
                    if (root._type === "measures") {
                        mintPopup.x = root.width / 2 - mintPopup.width / 2
                        mintPopup.y = Theme.sp(80)
                        mintPopup.open()
                    } else {
                        var r = browser.createObject(root._type)
                        root._report(r)
                        if (r.ok === true) root.select(r.type, r.id, true)
                    }
                }
            }

            PpButton {
                label: qsTr("Tools")
                onClicked: {
                    toolsPopup.x = root.width - toolsPopup.width - Theme.sp(24)
                    toolsPopup.y = Theme.sp(80)
                    toolsPopup.open()
                }
            }

            PpButton {
                label:   qsTr("Edits")
                enabled: true
                onClicked: root._editsOpen = !root._editsOpen
            }

            PpButton {
                label:   qsTr("Undo")
                enabled: browser.canUndo
                onClicked: root.doUndo()
            }

            PpButton {
                label:   qsTr("Save")
                primary: true
                enabled: browser.dirty
                onClicked: root.doSave()
            }
        }

        // ── Validation strip ──────────────────────────────────────────────────
        // What is wrong with the DRAFT. Clicking it filters the table to the offending rows, so the
        // strip is a way in rather than a description of a problem the reader then has to find.
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight:
                (browser.validationErrorCount + browser.validationWarningCount) > 0
                    ? Theme.sp(30) : 0
            visible: Layout.preferredHeight > 0
            color: browser.validationErrorCount > 0
                       ? Theme.colorErrorLight : Theme.colorWarnLight

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin:  Theme.sp(24)
                anchors.rightMargin: Theme.sp(24)
                spacing: Theme.sp(10)

                Text {
                    Layout.fillWidth: true
                    text: {
                        root._revision
                        var e = browser.validationErrorCount, w = browser.validationWarningCount
                        var bits = []
                        if (e > 0) bits.push(qsTr("%n error(s)", "", e))
                        if (w > 0) bits.push(qsTr("%n warning(s)", "", w))
                        return bits.join(" · ")
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color: browser.validationErrorCount > 0
                               ? Theme.colorError : Theme.colorWarn
                    elide: Text.ElideRight
                }

                Text {
                    text: qsTr("show →")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color: browser.validationErrorCount > 0
                               ? Theme.colorError : Theme.colorWarn
                }
            }

            PpPressable {
                hoverScale: 1.0
                onClicked: {
                    root._type   = "health"
                    root._search = ""
                    searchField.text = ""
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color:   Theme.colorBorderMid
            opacity: Theme.borderOpacityNormal
        }

        // ── The three panes ───────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            spacing: 0

            ModelTypeRail {
                Layout.preferredWidth: Theme.sp(214)
                Layout.fillHeight:     true
                types:        browser.types
                facets: {
                    root._revision
                    return root._showFacets ? browser.facets(root._type) : []
                }
                activeFacets: root._facets
                selectedType: root._type
                totalObjects: browser.totalObjects
                onTypePicked: (key) => {
                    // Close any open editor BEFORE the rows change under it — see
                    // ModelTable.endEdit()'s comment for what a stale editor looks like.
                    table.endEdit()
                    root._type = key
                    root._sort = ""
                    root._facets = ({})
                    root._selection = []
                }
                onFacetToggled: (key, value) => {
                    table.endEdit()
                    var f = JSON.parse(JSON.stringify(root._facets))
                    var v = f[key] || []
                    var at = v.indexOf(value)
                    if (at >= 0) v.splice(at, 1)
                    else         v.push(value)
                    if (v.length === 0) delete f[key]
                    else                f[key] = v
                    root._facets = f
                }
                onFacetsCleared: { table.endEdit(); root._facets = ({}) }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight:     true
                color:   Theme.colorBorder
                opacity: Theme.borderOpacityNormal
            }

            // ── Middle pane ───────────────────────────────────────────────────
            ColumnLayout {
                id: middlePane
                Layout.fillWidth:  true
                Layout.fillHeight: true
                Layout.minimumWidth: Theme.sp(400)
                spacing: 0

                RowLayout {
                    Layout.fillWidth:    true
                    Layout.leftMargin:   Theme.sp(18)
                    Layout.rightMargin:  Theme.sp(18)
                    Layout.topMargin:    Theme.sp(14)
                    Layout.bottomMargin: Theme.sp(10)
                    spacing: Theme.sp(10)

                    Text {
                        text: {
                            root._revision
                            if (root._searching) return qsTr("Results")
                            var t = browser.types
                            for (var i = 0; i < t.length; i++)
                                if (t[i].key === root._type) return t[i].label
                            return ""
                        }
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzHeading
                        font.weight:    Theme.fontBodyWeight
                        color:          Theme.colorText
                    }

                    Text {
                        Layout.fillWidth: true
                        text: {
                            root._revision
                            if (root._searching)
                                return qsTr("%n match(es) across every type", "", root._rows.length)
                            var t = browser.types
                            for (var i = 0; i < t.length; i++)
                                if (t[i].key === root._type)
                                    return t[i].count + " · " + t[i].hint
                            return ""
                        }
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        elide:          Text.ElideRight
                    }

                    // Bulk-set is offered only when there is a selection to bulk-set, and it says
                    // how many rows it would touch.
                    Text {
                        visible: root._selection.length > 1
                        text: qsTr("%n selected", "", root._selection.length)
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorAccent
                    }
                }

                ModelTable {
                    id: table
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    // `_view` alone. It used to be `_view === "table" || _searching`, with the
                    // graph carrying the complement — two properties deciding one thing, while the
                    // Table/Graph control bound to only one of them. Typing a single character
                    // hid the graph with the toggle still reading "Graph", which is the same bug
                    // three times over. Searching now SETS the view, so the control cannot
                    // disagree with what is on screen.
                    visible: root._view === "table"
                    columns: root._columns
                    rows:    root._rows
                    editable: root._typeEditable && !root._searching
                    selectedId: root._selectedId
                    selection:  root._selection
                    sortKey:    root._sort
                    descending: root._descending

                    onActivated: (id) => {
                        var row = null
                        for (var i = 0; i < root._rows.length; i++)
                            if (root._rows[i].id === id) row = root._rows[i]
                        root._selection = table.selection
                        // A health row is a FINDING, not an object. Selecting one jumps to the
                        // object it names — that is what makes the validation strip a way in rather
                        // than a description of a problem the reader then has to go and find.
                        if (row && row.type === "health") {
                            root.openSubject(row.subject)
                            return
                        }
                        root.select(root._typeOfRow(row), id, true)
                    }
                    onCommit: (id, field, value) => root.doCommit(id, field, value)
                    onDuplicateRequested: (id) => root.doDuplicate(id)
                    onRemoveRequested:    (id) => root.doRemove(id)
                    onSortRequested: (key) => {
                        if (root._sort === key) root._descending = !root._descending
                        else                  { root._sort = key; root._descending = false }
                    }
                }

                ModelGraph {
                    id: graph
                    Layout.fillWidth:  true
                    Layout.fillHeight: true
                    visible: root._view === "graph"
                    editable: root._typeEditable
                    focusId:  root._selectedId
                    selectedEdgeId: root._selectedEdgeId
                    // ANY selected row, not only a characteristic. A condition gets the causal
                    // DAG because it has ranks; everything else gets its neighbourhood, which is
                    // the honest shape for a relation that is one hop and has no direction.
                    layoutData: {
                        root._revision
                        if (root._view !== "graph" || root._selectedId === "") return ({})
                        // The theme's own metrics travel INTO the layout — the layout does the
                        // positioning, but it has to be told what a row is worth in this aesthetic.
                        return browser.graph(root._selectedType, root._selectedId, {
                            nodeH: Theme.sp(34), gapX: Theme.sp(52), gapY: Theme.sp(14),
                            laneGap: Theme.sp(36), padX: Theme.sp(12), charW: Theme.sp(6.4),
                            minW: Theme.sp(110), maxW: Theme.sp(210),
                            depth: middlePane.graphDepth, maxPerRank: 8,
                            includeMeasures: middlePane.graphMeasures,
                            hideWeak: middlePane.graphHideWeak,
                            hideProposed: middlePane.graphHideProposed
                        })
                    }
                    legalityProbe: (from, to) => browser.linkLegality(from, to, "causes")

                    scope:           middlePane.graphDepth
                    includeMeasures: middlePane.graphMeasures
                    hideWeak:        middlePane.graphHideWeak
                    hideProposed:    middlePane.graphHideProposed
                    onScopeRequested: (v) => middlePane.graphDepth = v
                    onSwitchToggled: (which) => {
                        if (which === "measures")      middlePane.graphMeasures     = !middlePane.graphMeasures
                        else if (which === "weak")     middlePane.graphHideWeak     = !middlePane.graphHideWeak
                        else                           middlePane.graphHideProposed = !middlePane.graphHideProposed
                    }

                    onNodeActivated: (nodeType, id) => {
                        root._selectedEdgeId = ""
                        // The node says what it is. A measure in the detection lane is a measure,
                        // and centring the graph on it is the whole point of it being drawn.
                        root.navigateTo(nodeType !== "" ? nodeType : "characteristics", id)
                    }
                    onEdgeActivated: (rowId) => {
                        root._selectedEdgeId = rowId
                        root.select("links", rowId, false)
                    }
                    onLinkRequested: (from, to) => root._report(browser.addLink(from, to, "causes"))
                }

                // Graph view options. The controls that set them are overlaid on the graph —
                // they change what you are looking at, so they belong where you are looking.
                property int  graphDepth: 2
                property bool graphMeasures: false
                property bool graphHideWeak: false
                property bool graphHideProposed: false

                // ── Status bar ────────────────────────────────────────────────
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.sp(28)
                    color: "transparent"

                    Rectangle {
                        anchors { left: parent.left; right: parent.right; top: parent.top }
                        height:  1
                        color:   Theme.colorBorderMid
                        opacity: Theme.borderOpacityNormal
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin:  Theme.sp(18)
                        anchors.rightMargin: Theme.sp(18)
                        spacing: Theme.sp(16)

                        // Both figures are derived from the arrays that feed the rows, never stated.
                        Text {
                            text: root._searching
                                      ? qsTr("%1 matches").arg(root._rows.length)
                                      : qsTr("%1 of %2 shown").arg(root._rows.length)
                                                              .arg(root._totalForType)
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }

                        Text {
                            Layout.fillWidth: true
                            visible: !root._searching
                            text: root._sort === "" ? qsTr("default order")
                                                    : qsTr("sorted by %1").arg(root._sort)
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            elide:          Text.ElideRight
                        }

                        Text {
                            text: qsTr("%n unsaved", "", browser.unsavedCount)
                            visible: browser.dirty
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorAccent
                        }

                        Text {
                            text:    qsTr("revert")
                            visible: browser.dirty
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            PpPressable { hoverScale: 1.0; onClicked: root.doRevert() }
                        }
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: 1
                Layout.fillHeight:     true
                color:   Theme.colorBorder
                opacity: Theme.borderOpacityNormal
                visible: root._showInspector
            }

            // ── Inspector / Edits ─────────────────────────────────────────────
            // The Edits history takes the inspector's slot rather than floating over the table:
            // it is a place you go, and stealing the list's width to show it would be the modal
            // this panel does not have.
            Item {
                Layout.preferredWidth: root._inspectorWidth
                Layout.fillHeight:     true
                visible: root._showInspector

                ModelInspector {
                    anchors.fill: parent
                    visible: !root._editsOpen
                    detail:  root._inspectorDetail
                    corridorPlotSource: root._corridorPlotSource
                    editable: root._typeEditable
                    onCorridorHandleCommitted: (handle, v) => root._report(
                        browser.setField("corridors", root._selectedId, handle, v))
                    // The text goes through verbatim: an empty one clears a plausibility bound,
                    // which is a state no number can stand for.
                    onCorridorFieldCommitted: (f, t) => root._report(
                        browser.setField("corridors", root._selectedId, f, t))

                    onCorridorScanRequested: browser.scanCorridor(root._corridorMeasureId)
                    onNavigate: (type, id) => root.navigateTo(type, id)
                    onAddCauseRequested: {
                        causePicker.x = parent.width / 2 - causePicker.width / 2
                        causePicker.y = Theme.sp(80)
                        causePicker.open()
                    }
                    onAddMeasureRequested: {
                        measurePicker.x = parent.width / 2 - measurePicker.width / 2
                        measurePicker.y = Theme.sp(80)
                        measurePicker.open()
                    }
                    onAddCorridorRequested: {
                        corridorPicker.x = parent.width / 2 - corridorPicker.width / 2
                        corridorPicker.y = Theme.sp(80)
                        corridorPicker.open()
                    }
                    onBindingCycled: (contextId, applicable, material, clear) => {
                        if (clear) root._report(browser.clearBinding(root._selectedId, contextId))
                        else       root._report(browser.setBinding(root._selectedId, contextId,
                                                                   applicable, material))
                    }
                    onRemoveRowRequested: (kind, id) => {
                        if (kind === "measure")
                            root._report(browser.removeMeasureFrom(root._selectedId, id))
                        else
                            root._report(browser.removeLink(id, root._selectedId, "causes"))
                    }
                }

                ModelEdits {
                    anchors.fill: parent
                    visible: root._editsOpen
                    edits:   browser.edits
                    sessionScoped: browser.undoIsSessionScoped
                    onWindTo: (index) => root._report(browser.undoTo(index))
                    onCloseRequested: root._editsOpen = false
                }
            }
        }
    }

    // ── Type-ahead pickers ────────────────────────────────────────────────────
    ModelPicker {
        id: causePicker
        parent: root
        title: qsTr("Add cause")
        candidateSource: (text) => { root._revision
                                     return browser.linkCandidates("causes", root._selectedId, text) }
        onPicked: (id) => root._report(browser.addLink(id, root._selectedId, "causes"))
    }

    ModelPicker {
        id: corridorPicker
        parent: root
        title: qsTr("Add corridor at")
        candidateSource: (text) => {
            root._revision
            // Only contexts WITHOUT a row of their own: adding a second corridor at one context is
            // not a thing, and offering it would be a choice the facade then refuses.
            var all = browser.corridorContexts(root._selectedType === "corridors"
                                                   ? root._corridorMeasureId : root._selectedId)
            var out = []
            for (var i = 0; i < all.length; i++) {
                if (all[i].own) continue
                if (text && all[i].label.toLowerCase().indexOf(text.toLowerCase()) < 0) continue
                out.push({ id: all[i].id, label: all[i].label,
                           detail: all[i].found ? qsTr("inherits μ %1").arg(all[i].mu)
                                                : qsTr("nothing resolves here") })
            }
            return out
        }
        onPicked: (contextId) => root._report(
                      browser.addCorridor(root._selectedType === "corridors"
                                              ? root._corridorMeasureId : root._selectedId,
                                          contextId))
    }

    ModelPicker {
        id: measurePicker
        parent: root
        title: qsTr("Add measure")
        candidateSource: (text) => { root._revision
                                     return browser.measureCandidates(root._selectedId, text) }
        onPicked: (id) => root._report(browser.addMeasureTo(root._selectedId, id, "high"))
    }

    ModelMint {
        id: mintPopup
        parent:  root
        browser: browser
        onMinted: (id) => { root._type = "measures"; root.select("measures", id, true) }
    }

    ModelTools {
        id: toolsPopup
        parent:  root
        browser: browser
        gradePolicy: appSettings.diagnosticsGradePolicy
        // Written straight to the ONE global AppSettings, per the single-shared-instance rule. It is
        // not an edit to the library, so it deliberately does not touch the undo stack.
        onGradePolicyPicked: (name) => { appSettings.diagnosticsGradePolicy = name
                                         toast.severity = "info"
                                         toast.show(qsTr("Grading as %1").arg(name)) }
        onExportRoadmapRequested:    root._report(browser.exportRoadmap())
        onExportReferencesRequested: root._report(browser.exportReferences())
        onRoadmapRequested:  { root._type = "measures"
                               root._sort = "readBy"
                               root._descending = true
                               toast.show(qsTr("Measures, most-blocking first")) }
        onGlossaryRequested: { root._type = "characteristics"
                               searchField.forceActiveFocus()
                               toast.show(qsTr("Search by the word you were taught")) }
    }

    PpToast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.sp(44)
        showUndo: false
    }

    // ── Keyboard ──────────────────────────────────────────────────────────────
    // Keyboard-first: a power author should be able to re-tier a dozen edges without the mouse.
    Shortcut { sequences: [ StandardKey.Save ]; onActivated: root.doSave() }
    Shortcut { sequences: [ StandardKey.Undo ]; onActivated: root.doUndo() }
    Shortcut { sequences: [ StandardKey.Redo ]; onActivated: root.doRedo() }
    Shortcut { sequence: "Ctrl+D";         onActivated: if (root._selectedId !== "")
                                                            root.doDuplicate(root._selectedId) }
    Shortcut { sequences: [ StandardKey.Find ]; onActivated: searchField.forceActiveFocus() }
    Shortcut {
        sequence: "G"
        // Window-wide shortcuts outrank a focused text field, so an ungated "G" would make the
        // letter untypeable in the search box and in every inline editor. It is a view toggle, not
        // a text key, and it has to stand down while something is being typed into.
        enabled: !searchField.activeFocus && !table.editing
                 && !causePicker.opened && !measurePicker.opened
        onActivated: root._view = root._view === "graph" ? "table" : "graph"
    }

    Component.onCompleted: table.forceActiveFocus()
}
