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
// Controls.Basic for the attached ToolTip on the inspector's unfold strip — the only Controls type
// this file uses directly.
import QtQuick.Controls.Basic
import PinPointStudio

// Settings → Reference → Diagnostic Model. One shell, three panes: type rail → table → relationship
// inspector, with a trail of the chain you walked.
//
// It REPLACED an earlier Diagnostics panel, which is now deleted. That one read the pack as saved
// and could not show an unsaved edit, which is why this exists rather than growing out of it.
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
    // This is nudged by modelChanged and every list binding depends on it.
    //
    // ⚠ EVERY BINDING THAT READS THIS MUST *USE* THE VALUE. Naming it on a line of its own —
    //
    //     readonly property var _rows: {
    //         root._revision            // ← dead code. Dropped, and the dependency with it.
    //         return browser.rows(…)
    //     }
    //
    // is a statement whose result is discarded, so the compiler removes it and the binding is left
    // subscribed to nothing. It then answers once and never again.
    //
    // This is not hypothetical and it is not obvious from reading: it shipped, and the symptom was
    // an inspector that kept showing three causes after a fourth was added — while the graph beside
    // it showed four, because switching views re-evaluated that binding for an unrelated reason.
    // Fourteen bindings in this file had it. Every one now reads the value into a guard
    // (`if (root._revision < 0) return …`), which is unreachable, cheap, and impossible to optimise
    // away — the read has to happen for the comparison to be made.
    //
    // If you add a binding that calls a Q_INVOKABLE on `browser`, copy the guard. If you are ever
    // debugging "the model changed but the screen did not", start here.
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
    // The chain walked to get here. Its terminal item is ALWAYS the current selection — that is
    // what makes it a trail rather than a history.
    property var    _trail:      []

    // A search runs in one of two SCOPES, and the difference is the whole reason the rail felt dead.
    //
    // Typing searches EVERYTHING — one flat cross-type result list, which is what `searchAll()`
    // returns and what `_searching` gates. Picking a type in the rail while that is up does not
    // throw the search away; it NARROWS it, keeping the text and applying it within that type. The
    // list then gets that type's own columns and its own editable cells, which is what the author
    // was reaching for: "find the causal links about attack angle, then re-rate one of them".
    //
    // It used to do neither. `_type` changed, `_search` did not, and `_searching` kept the table on
    // the cross-type results — so the rail highlighted a type and the screen did not move. The
    // result list also has search's columns (no Strength) and is deliberately not editable, so the
    // panel looked read-only into the bargain.
    property bool _searchAllTypes: true
    readonly property bool _searching: _search.trim().length > 0 && _searchAllTypes
    // Screens, drills, references and health have their own registries and their own write paths;
    // this panel does not write them, and a cell that looked live and did nothing would be worse
    // than one that plainly is not.
    // The types this panel writes. Corridors belong here as much as anything else does — leaving
    // them out made every corridor cell inert AND hid the plot's drag handles, so the whole editing
    // surface was there and dead. The list that is short is the read-only one below.
    readonly property bool _typeEditable:
        _type === "characteristics" || _type === "causes" || _type === "measures"
        || _type === "signals" || _type === "links" || _type === "corridors"
        || _type === "screens" || _type === "drills"

    // What ONE of the current type is called, for the button that names what it would make. The
    // string comes from C++ with every other type-naming rule rather than being a second table here.
    readonly property string _typeOne: {
        if (root._revision < 0) return ""
        var t = browser.types
        for (var i = 0; i < t.length; i++) if (t[i].key === root._type) return t[i].one
        return ""
    }

    // Creation is not a thing for every editable type: a link is drawn between two objects, a signal
    // is minted by attaching a measure, a corridor is added at a context. Nor while searching — a
    // result list is not a place you author into.
    readonly property bool _canCreate:
        _typeEditable && !_searching && _type !== "links" && _type !== "signals"
        && _type !== "corridors"

    // The grade policy as a WORD, not the stored key. The readout is the state every corridor on
    // screen is drawn under, so it has to read like the thing the policy list offered.
    readonly property string _policyLabel: {
        if (root._revision < 0) return ""
        var ps = browser.gradePolicies()
        for (var i = 0; i < ps.length; i++)
            if (ps[i].name === appSettings.diagnosticsGradePolicy) return ps[i].label
        return appSettings.diagnosticsGradePolicy
    }

    // The active filters, as chips, for when the facet rail is collapsed. Labels come from the same
    // facet spec the rail renders, so a chip cannot say something the rail would not.
    readonly property var _facetChips: {
        if (root._revision < 0) return []
        var out = []
        var spec = browser.facets(root._type)
        for (var k in root._facets) {
            var values = root._facets[k]
            for (var i = 0; i < values.length; i++) {
                var label = values[i]
                for (var s = 0; s < spec.length; s++) {
                    if (spec[s].key !== k) continue
                    for (var o = 0; o < spec[s].options.length; o++)
                        if (spec[s].options[o].value === values[i])
                            label = spec[s].label + " · " + spec[s].options[o].label
                }
                out.push({ key: k, value: values[i], label: label })
            }
        }
        return out
    }

    // ── Responsive ────────────────────────────────────────────────────────────
    // The three panes plus the sidenav add up fast, and the name column is what gets starved.
    // The inspector is the pane that actually holds prose, a corridor plot and a list of links, so
    // it is the one that gets the room. Widened by half over the mockup's 352, which was drawn
    // before it carried a plot at all — and the threshold for dropping it moves with it, or the
    // panel would keep a pane it can no longer fit.
    //
    // Both thresholds are stated as THIS PANEL'S width. They used to be written `1500 - 275` and
    // `1326 - 275`, subtracting a settings sidenav that is now foldable — a constant baked into a
    // measurement of something else. The panel is handed whatever the sidenav does not take, so its
    // own width is the only figure either question needs, and folding the sidenav now buys the facet
    // rail and the inspector real room instead of quietly changing what the arithmetic meant.
    //
    // Order of concession under pressure: fold the sidenav → collapse the facet rail → drop the
    // inspector. The first step is the author's to take, which is why it is a control and not a
    // threshold.
    readonly property int  _inspectorWidth: Theme.sp(528)

    // Folded away by hand, and it STAYS folded — persisted in AppSettings for the same reason the
    // settings sidenav's fold is: one that resets on every visit is one you re-do forever.
    //
    // Distinct from _showInspector below, which is the panel running out of room. That one is the
    // app's decision and reverses itself when the window grows; this one is the author's and does
    // not. When the panel drops the pane for width, it takes the fold's strip with it — a control
    // offering to restore something the layout will not give back would be a lie.
    readonly property bool _inspectorFolded: appSettings.diagnosticsInspectorCollapsed
    readonly property bool _showFacets:     width > Theme.sp(1225)
    readonly property bool _showInspector:  width > Theme.sp(1051)

    // ── Data, all derived in C++ ──────────────────────────────────────────────
    readonly property var _columns: {
        if (root._revision < 0) return []
        return root._searching ? browser.columns("search") : browser.columns(root._type)
    }
    readonly property var _rows: {
        if (root._revision < 0) return []
        if (root._searching) return browser.searchAll(root._search)
        // `search` travels into rows() so a search narrowed to one type still filters it. The
        // façade has always taken this filter; the panel simply never passed it.
        return browser.rows(root._type, { sort: root._sort, descending: root._descending,
                                          facets: root._facets, search: root._search })
    }
    readonly property int _totalForType: {
        if (root._revision < 0) return 0
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
        if (root._revision < 0 || root._scanRevision < 0) return function () { return ({}) }
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
        if (root._revision < 0 || root._selectedId === "") return ({})
        return browser.inspect(root._selectedType, root._selectedId)
    }

    // ── Navigation ────────────────────────────────────────────────────────────

    function _typeOfRow(row) {
        // A search result knows its own type; an ordinary row's type is the table's.
        return row && row.resultType ? row.resultType : (row && row.type ? row.type : root._type)
    }

    function _trailStep(type, id) {
        return { type: type, id: id, label: browser.inspect(type, id).label || id }
    }

    // WALKED to — the trail grows. Following a relationship is the only thing that makes a chain a
    // chain: a metric, the measure that reads it, the corridor that judges it, the characteristic
    // that fires. Every caller of this is a link somebody followed.
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
        t.push(_trailStep(type, id))
        if (t.length > 8) t = t.slice(t.length - 8)
        root._trail = t
    }

    // ARRIVED at — the trail STARTS here, one item long.
    //
    // Picking a row out of the table is not a step in a chain; it is the decision to start a
    // different one. Keeping the old walk in front of it would make the breadcrumb claim a route
    // nobody took — and worse, offer to "go back" to something the author had already left. So the
    // trail is exactly what it says it is: what you walked SINCE you last chose where to stand.
    //
    // The test for which of the two applies is whether a RELATIONSHIP was followed. Table rows, deep
    // links from the dashboard, a health finding's subject and a freshly created object all put you
    // somewhere without one, so all of them begin a trail rather than extending one.
    function selectFresh(type, id) {
        root._selectedType = type
        root._selectedId   = id
        root._selectedEdgeId = ""
        root._trail = [ _trailStep(type, id) ]
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

    // What the graph has picked up, which is NOT the same as where it is standing. A press or a
    // marquee on the canvas sets the scope of the next ring; the focus only moves when somebody
    // asks it to. Kept apart from `_selection` for exactly that reason — the table's selection
    // means "these rows", and this one means "the next gesture is about these".
    property var _graphNodeSel: []
    property var _graphEdgeSel: []

    // For the ring's hub census — "11 of 108 drawn". Taken from the type rail's own counts, so it
    // cannot disagree with the number beside the type in the list.
    readonly property int _conditionCount: {
        if (root._revision < 0) return 0
        var t = browser.types
        var n = 0
        for (var i = 0; i < t.length; i++)
            if (t[i].key === "characteristics" || t[i].key === "causes") n += t[i].count
        return n
    }

    // Where the GRAPH stands, which is not always where the table's selection is.
    //
    // Selecting an edge sets the selection to the LINK, and a link has a neighbourhood of its own —
    // two nodes and the line between them. Drawing that in place of the causal picture replaces the
    // very drawing the reader picked the line out of, so the heavier stroke and the muting of
    // everything else could never be seen. A claim centres on its cause and stays drawn.
    //
    // Decided in C++ — see graphFocus() — because "a link is not a place to stand" is a fact about
    // links, and a rule written in a binding is a rule nothing can test.
    readonly property var _graphFocus:
        root._revision < 0 ? ({}) : browser.graphFocus(root._selectedType, root._selectedId)

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
                // A finding is not a step in a chain — it is a place to start looking.
                root.selectFresh(order[t], subject)
                return
            }
        }
        // A subject that resolves nowhere is itself the finding — say so rather than doing nothing.
        toast.severity = "warn"
        toast.show(qsTr("Nothing in the model has the id %1").arg(subject))
    }

    // One place, because two surfaces set it: the facet rail and the chips the context bar draws
    // when that rail is collapsed. Toggling from either has to mean the same thing.
    function toggleFacet(key, value) {
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

    // Open a picker where the click was.
    //
    // Every one of these used to compute `parent.width / 2 - popup.width / 2` — but `parent` there
    // was the inspector (528 wide) while `x` is in the PANEL's coordinates, so the sum landed the
    // popup at x≈84: hard against the left edge, however far right the affordance that opened it
    // was. On a full-screen window that is the whole display to cross to answer a question you just
    // asked on the other side.
    //
    // `origin` is in `source`'s coordinates; it is mapped here and then clamped so the popup stays
    // on the panel whichever edge it was opened near.
    function _openPickerNear(popup, source, origin) {
        var p = source.mapToItem(root, origin.x, origin.y)
        var h = popup.height > 0 ? popup.height : Theme.sp(320)
        popup.x = Math.max(Theme.sp(8),
                           Math.min(p.x, root.width - popup.width - Theme.sp(8)))
        popup.y = Math.max(Theme.sp(8),
                           Math.min(p.y + Theme.sp(4), root.height - h - Theme.sp(8)))
        popup.open()
    }

    // Hang a popover under the bar item that owns it, right edges aligned. The item's own `x` is
    // relative to the bar, not to the panel, so it has to be mapped or every popover sits one bar
    // margin off — visible as a popover that does not quite line up with the thing it belongs to.
    function _dropUnder(popup, item) {
        var right = item.mapToItem(root, item.width, item.height)
        popup.x = Math.max(Theme.sp(8), Math.min(right.x - popup.width,
                                                 root.width - popup.width - Theme.sp(8)))
        popup.y = right.y + Theme.sp(2)
        popup.open()
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

    // Save is PACK-WIDE: one button, everything dirty, which is why the `n unsaved` count beside it
    // is exactly what it would write.
    //
    // The first save that writes over SHIPPED content interrupts, once per install. Every other edit
    // in this panel is undoable and local; this one changes what the whole app grades against, and an
    // author should meet that fact deliberately once rather than never. Purely authored content does
    // not trigger it — adding a characteristic of your own is not deviating from the standard model,
    // and a prompt that cried wolf on the first save of anything would be dismissed unread.
    function doSave() {
        if (!appSettings.diagnosticsBaseModelWarningAck && browser.overriddenCount > 0) {
            saveConfirm.open()
            return
        }
        _report(browser.save())
    }
    function doUndo()      { _report(browser.undo()) }
    function doRedo()      { _report(browser.redo()) }
    function doRevert()    { _report(browser.revert()) }

    // The way back from that warning. Everything local goes — overrides AND objects authored here,
    // because a new characteristic is screened and graded like any other, so an install carrying one
    // is not running the standard model either. Recoverable twice over: it is a command, so ⌘Z holds
    // it for the session, and the packs are copied aside before they are replaced.
    function doResetToStandard() {
        var r = browser.resetToStandard()
        _report(r)
        // Back on the standard model is back to not having been warned about leaving it.
        if (r.ok === true) appSettings.diagnosticsBaseModelWarningAck = false
    }

    // `type` is optional and defaults to the LIST's type, which is right for the table. The
    // inspector passes the SELECTED object's type explicitly, because what is selected need not be
    // of the type the list is showing — following a cause out of a characteristic's pane leaves the
    // list on characteristics and the selection on a link.
    function doDuplicate(id, type) {
        var r = browser.duplicate(type || root._type, id)
        _report(r)
        // The copy appears in the table and is selected there. Same rule as picking it by hand: it
        // begins a trail rather than hanging off whatever chain the original was the end of.
        if (r.ok === true) selectFresh(r.type, r.id)
    }

    // ── What the graph's ring asks for ────────────────────────────────────────
    //
    // ONE entry point for every spoke that is not the pane's own view state. The ring is a menu of
    // verbs this panel already has — that is the rule §7 rests on, and it is what makes shipping
    // the gesture safe: an author who never discovers the hold loses no capability, because the
    // inspector and the table reach every one of these by another route.
    //
    // Nothing here is new behaviour. `Revert to shipped` and `Move to trash` are the SAME call,
    // because dropping your copy of a shipped object restores it and dropping your own deletes it —
    // the model decides which of the two happened and words the message accordingly.
    function doGraphVerb(verb, arg) {
        var ids  = arg && arg.ids  ? arg.ids  : []
        var type = arg && arg.type ? arg.type : root._type
        var one  = ids.length > 0 ? ids[0] : ""

        switch (verb) {
        case "focus":
            root._graphNodeSel = []
            root.navigateTo(type, one)
            return
        case "inspect":
            root._selectedEdgeId = one
            root.select("links", one, false)
            return

        case "showInTable":
        case "showNInTable":
            root._view = "table"
            root._search = ""
            searchField.text = ""
            root._type = type
            root._selection = ids
            if (one !== "") root.selectFresh(type, one)
            return
        case "toTable":
            root._view = "table"
            return

        case "duplicate":
            root.doDuplicate(one, type)
            return
        case "duplicateN":
            // Not collapsed into one command, and deliberately not claimed to be: §5.5 promises a
            // single step for bulk STRENGTH and bulk DELETE, which are the two the author asked
            // for. n copies is n creations, and each is separately worth taking back.
            for (var i = 0; i < ids.length; i++) root._report(browser.duplicate(type, ids[i]))
            return

        case "revert":
        case "trash":
            root._report(browser.removeObject(type, one))
            return
        case "deleteLink":
            root._report(browser.removeObject("links", one))
            root._selectedEdgeId = ""
            root._graphEdgeSel = []
            return
        case "revertN":
        case "trashN":
            root._report(browser.removeObjects(type, ids))
            root._graphNodeSel = []
            root._graphEdgeSel = []
            root._selectedEdgeId = ""
            return

        case "group":
        case "groupN":
            root._report(browser.setFieldOnAll(type, ids, "group", arg.value))
            return
        case "strength":
        case "strengthN":
            root._report(browser.setCauseStrength(ids, arg.value))
            return

        // `New cause here` is gone from the node ring: the SE slot pays for `Add effect…`. The
        // capability is not gone with it — both make-drags create from their drop popover, and the
        // inspector's `Add a cause` creates too — so §7's rule that no verb is ring-only still holds.
        case "newHere":
            var r = browser.createObject("characteristics")
            root._report(r)
            if (r.ok === true) root.selectFresh(r.type, r.id)
            return

        case "tidy":
            // The nudges are already gone — the pane dropped them. This re-runs the layout, which
            // is the other half of what the spoke says it does.
            root._revision++
            return
        }
    }

    // Create an object at the end of a drag and attach it in the one way its type attaches, as ONE
    // undo step. A measure is the exception and routes to the mint, because a measure IS its facets
    // and a name cannot stand in for them.
    function doCreateAttached(objType, name, otherId, end) {
        if (objType === "measures") {
            root._pendingMeasureHost = otherId
            mintPopup.open()
            return
        }
        var r = browser.createAndAttach(objType, name, otherId, end)
        root._report(r)
        if (r.ok === true) root.selectFresh(r.type, r.id)
    }

    // Which condition a measure minted from the canvas should be attached to, remembered across the
    // mint because the mint itself has no idea it was opened from a graph drag.
    property string _pendingMeasureHost: ""

    // ── Re-pointing from the inspector ────────────────────────────────────────
    //
    // The same edit the ring's `Re-point from…` / `Re-point to…` spokes make, reached by anybody
    // who did not think to hold the link. One gesture, two entrances — not two features.
    //
    // On the graph it arms the identical drag, because there is a picture to drag across. In the
    // table there is not, so it falls back to the type-ahead every other link edit in this panel
    // uses. Both are fed the SAME pre-filtered legal set, so neither can offer something the write
    // would then refuse.
    property string _repointEdgeId: ""
    property string _repointEnd:    ""

    function doRepoint(edgeId, end) {
        if (edgeId === "") return
        if (root._view === "graph" && graph.armRepoint(edgeId, end)) return

        root._repointEdgeId = edgeId
        root._repointEnd    = end
        repointPopup.title  = end === "from" ? qsTr("Re-point from…") : qsTr("Re-point to…")
        root._openPickerNear(repointPopup, inspector, inspector.actionOrigin)
    }

    function doRemove(id, type) {
        // removeObject() unpacks a link id itself now, so there is one call for every type. It used
        // to hunt the visible rows for the link's parts, which only worked while the link happened
        // to be in the list on screen — never true when the delete came from the inspector.
        _report(browser.removeObject(type || root._type, id))
    }

    // ── Deep links ────────────────────────────────────────────────────────────
    //
    // The dashboard's metric tiles route here through MetricRoute. They used to land in Diagnostics;
    // that panel is hidden now, so a link into it would open a page with no row selected in the
    // sidenav — which reads as the app losing its place.
    //
    // Each of these is "select this object", because that is all navigation in this panel IS: the
    // type rail follows the selection and the inspector follows it too.
    //
    // They BEGIN a trail. Arriving from a dashboard tile is the start of a piece of work, and the
    // chain somebody walked in this panel an hour ago is not the route they took to get here.
    function showMetric(key) {
        root._search = ""
        root._view   = "table"
        root._type   = "metrics"
        root.selectFresh("metrics", key)
    }

    function showCharacteristic(conditionId) {
        root._search = ""
        root._view   = "table"
        root._type   = "characteristics"
        root.selectFresh("characteristics", conditionId)
    }

    function showMeasure(measureId) {
        root._search = ""
        root._view   = "table"
        root._type   = "measures"
        root.selectFresh("measures", measureId)
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

        // ── The global bar ────────────────────────────────────────────────────
        //
        // ONE bar where there were four bands: a page header, a nine-control toolbar, a validation
        // strip and a status bar that repeated the unsaved count. The rule that decides what belongs
        // here (ADDENDUM-02, A1): the top bar belongs to the PANEL and never changes. Anything that
        // depends on what you are looking at is on the context bar inside the middle pane, where it
        // can act on the thing it names. A bar that never moves can be learned; a bar whose contents
        // shuffle per type cannot.
        //
        // The page header is gone entirely. `REFERENCE` was the sidenav section the reader was
        // standing in, and repeating it is chrome describing chrome.
        RowLayout {
            id: globalBar
            Layout.fillWidth:       true
            Layout.leftMargin:      Theme.sp(24)
            Layout.rightMargin:     Theme.sp(24)
            Layout.preferredHeight: Theme.sp(42)
            spacing: Theme.sp(10)

            // Below this the title and the pack label drop out before anything else does. They are
            // the only two items on the bar that say something the reader already knows.
            readonly property bool roomy: root.width > Theme.sp(1100)

            PpDisplayText {
                text:    qsTr("Diagnostic Model")
                visible: globalBar.roomy
                Layout.fillWidth: false
            }

            Text {
                text:           browser.packLabel
                visible:        globalBar.roomy
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
                Layout.fillWidth: false
            }

            PpTextField {
                id: searchField
                Layout.fillWidth:    true
                Layout.maximumWidth: Theme.sp(420)
                Layout.minimumWidth: Theme.sp(180)
                Layout.preferredHeight: Theme.sp(30)
                rightPadding: findHint.implicitWidth + Theme.sp(18)
                placeholderText: qsTr("Search every content type")
                onTextChanged: {
                    table.endEdit()
                    root._search = text
                    // Typing is always a fresh question, asked of everything. Narrowing to a type is
                    // a decision taken AFTER the answer comes back.
                    root._searchAllTypes = true
                    // A search answers across every type at once, which is a LIST — so asking for
                    // one puts you in the list, visibly, rather than silently swapping the pane out
                    // from under a control that still says otherwise.
                    if (text.trim().length > 0) root._view = "table"
                }
                Keys.onEscapePressed: { text = ""; root._search = "" }

                // The shortcut exists either way; saying so is what makes it reachable by anybody
                // who did not read the code.
                Text {
                    id: findHint
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right:          parent.right
                    anchors.rightMargin:    Theme.sp(9)
                    visible: !searchField.activeFocus && searchField.text === ""
                    text:    Qt.platform.os === "osx" ? "⌘F" : "Ctrl+F"
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
            }

            Item { Layout.fillWidth: true }

            // ── Grading, as a live readout ────────────────────────────────────
            // Every band edge in every corridor on screen is drawn under this policy, so the answer
            // to "why is this corridor red" must not be behind a button called Tools. Promoted out
            // of the drawer to a value you can see and change in one click.
            ModelBarButton {
                id: gradingButton
                Layout.fillWidth: false
                label:   qsTr("Grading %1").arg(root._policyLabel)
                hint:    "▾"
                tooltip: qsTr("How every corridor on screen is graded")
                active:  policyPopup.opened
                onClicked: root._dropUnder(policyPopup, gradingButton)
            }

            ModelBarButton {
                id: drawerButton
                Layout.fillWidth: false
                glyph:   "⋯"
                tooltip: qsTr("Views, exports and norm sets")
                active:  toolsPopup.opened
                onClicked: root._dropUnder(toolsPopup, drawerButton)
            }

            Rectangle {
                Layout.fillWidth: false
                Layout.preferredWidth:  1
                Layout.preferredHeight: Theme.sp(18)
                color:   Theme.colorBorderMid
                opacity: Theme.borderOpacityNormal
            }

            // ── The validation chip ───────────────────────────────────────────
            // Was a band of its own. A clean draft now costs zero pixels rather than a conditional
            // strip, and the one thing that made the strip worth having survives: clicking it is a
            // WAY IN to the findings, not a description of a problem the reader then has to go and
            // locate.
            Rectangle {
                id: validationChip
                readonly property int errors:   browser.validationErrorCount
                readonly property int warnings: browser.validationWarningCount

                Layout.fillWidth: false
                visible: errors + warnings > 0
                implicitWidth:  chipText.implicitWidth + Theme.sp(18)
                implicitHeight: Theme.sp(26)
                radius: Theme.radius
                color:  errors > 0 ? Theme.colorErrorLight : Theme.colorWarnLight

                Text {
                    id: chipText
                    anchors.centerIn: parent
                    text: {
                        if (root._revision < 0) return ""
                        var bits = []
                        if (validationChip.errors > 0)
                            bits.push(qsTr("%n error(s)", "", validationChip.errors))
                        if (validationChip.warnings > 0)
                            bits.push(qsTr("%n warning(s)", "", validationChip.warnings))
                        return "⚠ " + bits.join(" · ")
                    }
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    font.weight:    Theme.fontBodyWeight
                    color: validationChip.errors > 0 ? Theme.colorError : Theme.colorWarn
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

            // ── The commit cluster ────────────────────────────────────────────
            // Three items, always adjacent, always here. This is the only part of the chrome an
            // author touches every few seconds, and it used to be spread across three bands with
            // `revert` orphaned in the status bar and redo owning no button at all.
            ModelBarButton {
                Layout.fillWidth: false
                glyph:   "↶"
                enabled: browser.canUndo
                // The commands already carry a human label and nothing showed it anywhere.
                tooltip: browser.canUndo ? qsTr("Undo %1").arg(browser.undoLabel) : qsTr("Nothing to undo")
                onClicked: root.doUndo()
            }

            ModelBarButton {
                Layout.fillWidth: false
                glyph:   "↷"
                enabled: browser.canRedo
                tooltip: browser.canRedo ? qsTr("Redo %1").arg(browser.redoLabel) : qsTr("Nothing to redo")
                onClicked: root.doRedo()
            }

            ModelBarButton {
                id: unsavedButton
                Layout.fillWidth: false
                visible: browser.dirty
                label:   qsTr("%n unsaved", "", browser.unsavedCount)
                hint:    "▾"
                tone:    Theme.colorAccent
                tooltip: qsTr("What Save would write")
                active:  unsavedPopup.opened
                onClicked: root._dropUnder(unsavedPopup, unsavedButton)
            }

            PpButton {
                Layout.fillWidth: false
                Layout.preferredHeight: Theme.sp(30)
                label:   qsTr("Save")
                primary: true
                enabled: browser.dirty
                onClicked: root.doSave()
            }
        }

        // ── The context bar ───────────────────────────────────────────────────
        //
        // Everything that depends on WHAT YOU ARE LOOKING AT (ADDENDUM-02, A2), on its own line
        // directly under the global bar. It replaces the old middle-pane header row rather than
        // adding to it: the trail IS the heading, so the breadcrumb sits above the list it walked
        // and a band disappears.
        //
        // A2 originally put this INSIDE the middle pane, starting where the table starts, on the
        // argument that a bar belonging to the list should look like it. That argument was right
        // about meaning and wrong about arithmetic: the middle pane is what is left after a 214px
        // rail and a 528px inspector, so eight controls plus an eliding breadcrumb were fighting
        // over roughly a third of the window — cramped at FULL SCREEN, not just under pressure.
        // A row that cannot hold its contents does not communicate what it belongs to either.
        //
        // So it spans the panel and takes the same margins as the global bar. Two bands, both
        // full width: one that never changes and one that always does.
        RowLayout {
            id: contextBar
            Layout.fillWidth:       true
            Layout.leftMargin:      Theme.sp(24)
            Layout.rightMargin:     Theme.sp(24)
            Layout.bottomMargin:    Theme.sp(6)
            Layout.preferredHeight: Theme.sp(38)
            spacing: Theme.sp(10)

            ModelTrail {
                Layout.fillWidth:    true
                Layout.minimumWidth: Theme.sp(60)
                trail: root._searching ? [] : root._trail
                fallbackLabel: {
                    if (root._revision < 0) return ""
                    if (root._searching) return qsTr("Results")
                    var t = browser.types
                    for (var i = 0; i < t.length; i++)
                        if (t[i].key === root._type) return t[i].label
                    return ""
                }
                onStepPicked: (type, id) => root.navigateTo(type, id)
            }

            Text {
                Layout.fillWidth: false
                Layout.maximumWidth: Theme.sp(260)
                text: {
                    if (root._revision < 0) return ""
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

            // A collapsed facet rail hides the fact that a filter is ON, which would leave
            // the reader looking at a short list with no way to see why. The chips are the
            // filter, said where the list is.
            Repeater {
                model: root._showFacets ? [] : root._facetChips
                delegate: Rectangle {
                    id: facetChip
                    required property var modelData

                    Layout.fillWidth: false
                    implicitWidth:  facetChipText.implicitWidth + Theme.sp(22)
                    implicitHeight: Theme.sp(22)
                    radius: height / 2
                    color:  Theme.colorAccentLight
                    border.width: 1
                    border.color: Theme.colorAccent

                    Text {
                        id: facetChipText
                        anchors.centerIn: parent
                        text: facetChip.modelData.label + "  ✕"
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorAccent
                    }

                    PpPressable {
                        hoverScale: 1.0
                        onClicked: root.toggleFacet(facetChip.modelData.key,
                                                    facetChip.modelData.value)
                    }
                }
            }

            // Bulk-set is offered only when there is a selection to bulk-set, and it says
            // how many rows it would touch.
            Text {
                Layout.fillWidth: false
                visible: root._selection.length > 1
                text: qsTr("%n selected", "", root._selection.length)
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorAccent
            }

            // minimumWidth, not just preferredWidth: PpSegmentedControl sets an
            // implicitHeight and no implicitWidth, so a RowLayout under pressure will
            // happily shrink it to nothing — and the Table/Graph toggle would vanish rather
            // than merely get narrow. It is the only way into the graph pane besides the G
            // key, so it must not be the thing that gives.
            //
            // Hidden for health, where a finding has no neighbourhood to draw.
            PpSegmentedControl {
                Layout.fillWidth: false
                Layout.preferredWidth: Theme.sp(140)
                Layout.minimumWidth:   Theme.sp(120)
                visible: root._type !== "health"
                options: [ qsTr("Table"), qsTr("Graph") ]
                selected: root._view === "table" ? qsTr("Table") : qsTr("Graph")
                onActivated: (value) => root._view = (value === qsTr("Table") ? "table" : "graph")
            }

            // ── The three row actions ─────────────────────────────────────────
            //
            // ONE control type for all three. They were a PpButton and two bar glyphs, which read as
            // three different kinds of thing sitting together: a filled 34px page button beside two
            // quiet 28px icons, for three actions of equal standing that all operate on the same
            // list.
            //
            // The rule for both bars, now stated: everything is a ModelBarButton, and `Save` is the
            // sole PpButton because it is the sole action that leaves the draft. Weight among the
            // three is carried by TONE, not by shape — accent for the one that makes something,
            // plain for the ones that copy and remove it.
            //
            // The label still NAMES the type. "New" answered a question the reader had to hold in
            // their head — new what? — while being the one control on the old toolbar that knew
            // what was selected at all.
            ModelBarButton {
                id: newButton
                Layout.fillWidth: false
                glyph:   "+"
                label:   qsTr("New %1").arg(root._typeOne.toLowerCase())
                tone:    Theme.colorAccent
                tooltip: qsTr("Create a %1").arg(root._typeOne.toLowerCase())
                visible: root._canCreate
                onClicked: {
                    if (root._type === "measures") {
                        root._dropUnder(mintPopup, newButton)
                    } else {
                        var r = browser.createObject(root._type)
                        root._report(r)
                        // A new object is not somewhere you walked to, so it begins a trail.
                        if (r.ok === true) root.selectFresh(r.type, r.id)
                    }
                }
            }

            // Copy and Delete used to sit here as ⧉ and 🗑. They act on the SELECTED OBJECT, not on
            // the list, so they moved to the foot of the pane that shows that object — where they
            // are words rather than glyphs, and where a destructive action sits beside the thing it
            // would remove instead of in the chrome. `+ New` stays: it acts on the list.
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
                    if (root._revision < 0) return []
                    return root._showFacets ? browser.facets(root._type) : []
                }
                activeFacets: root._facets
                selectedType: root._type
                totalObjects: browser.totalObjects
                onTypePicked: (key) => {
                    // Close any open editor BEFORE the rows change under it — see
                    // ModelTable.endEdit()'s comment for what a stale editor looks like.
                    table.endEdit()
                    // NARROWS the search rather than ending it: the text stays, the scope becomes
                    // this one type. See `_searchAllTypes`.
                    root._searchAllTypes = false
                    root._type = key
                    root._sort = ""
                    root._facets = ({})
                    root._selection = []
                }
                // A facet narrows ONE type's rows, so it cannot apply to a cross-type result list —
                // touching one scopes the search to the type being looked at, exactly as picking the
                // type would.
                onFacetToggled: (key, value) => {
                    root._searchAllTypes = false
                    root.toggleFacet(key, value)
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

                // No header row of its own any more — the context bar above the panes is the
                // heading, and the table starts at the top of the pane it fills.
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
                        // Picking a row is choosing where to STAND, not a step taken from where you
                        // were — so the trail starts here rather than growing by one.
                        root.selectFresh(root._typeOfRow(row), id)
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
                    focusId:  root._graphFocus.id || ""
                    selectedEdgeId: root._selectedEdgeId
                    // ANY selected row, not only a characteristic. A condition gets the causal
                    // DAG because it has ranks; everything else gets its neighbourhood, which is
                    // the honest shape for a relation that is one hop and has no direction.
                    layoutData: {
                        if (root._revision < 0 || root._view !== "graph"
                            || root._graphFocus.id === undefined
                            || root._graphFocus.id === "") return ({})
                        // The theme's own metrics travel INTO the layout — the layout does the
                        // positioning, but it has to be told what a row is worth in this aesthetic.
                        return browser.graph(root._graphFocus.type, root._graphFocus.id, {
                            nodeH: Theme.sp(34), gapX: Theme.sp(52), gapY: Theme.sp(14),
                            laneGap: Theme.sp(36), padX: Theme.sp(12), charW: Theme.sp(6.4),
                            minW: Theme.sp(110), maxW: Theme.sp(210),
                            depth: middlePane.graphDepth, maxPerRank: 8,
                            includeMeasures: middlePane.graphMeasures,
                            hideWeak: middlePane.graphHideWeak,
                            hideProposed: middlePane.graphHideProposed
                        })
                    }
                    // Asked ONCE per drag, not per hover, and answered in C++ — a node that is off
                    // this canvas is still in the graph, and a reachability check written over the
                    // drawn nodes would happily draw a cycle through one of them.
                    refusalsProbe:   (fixed, ids, end) => browser.linkRefusals(fixed, ids, end)
                    ringValuesProbe: (t, id, field)    => browser.ringValues(t, id, field)
                    // Over the whole library, not the drawn nodes — the conditions worth linking to
                    // are precisely the ones this neighbourhood does not already contain.
                    causeCandidatesProbe: (fixed, text, end) => {
                        if (root._revision < 0) return []
                        return browser.linkCandidates("causes", fixed, text, end)
                    }

                    selectedNodeIds: root._graphNodeSel
                    selectedEdgeIds: root._graphEdgeSel
                    totalConditions: root._conditionCount

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
                        root._graphEdgeSel = []
                        // The node says what it is. A measure in the detection lane is a measure,
                        // and centring the graph on it is the whole point of it being drawn.
                        root.navigateTo(nodeType !== "" ? nodeType : "characteristics", id)
                    }
                    onEdgeActivated: (rowId) => {
                        root._selectedEdgeId = rowId
                        root._graphNodeSel = []
                        root._graphEdgeSel = [ rowId ]
                        root.select("links", rowId, false)
                    }

                    // A press or a marquee sets the SCOPE of the next gesture. It deliberately does
                    // not move the graph's focus: `Focus here` is its own spoke precisely because
                    // choosing what to act on and choosing what to look at are different decisions,
                    // and a canvas that re-centred every time you picked something up would throw
                    // away the picture the pick was made from.
                    onSelectionRequested: (nodeIds, edgeIds) => {
                        root._graphNodeSel = nodeIds
                        root._graphEdgeSel = edgeIds
                        root._selectedEdgeId = edgeIds.length === 1 ? edgeIds[0] : ""
                        root._selection = edgeIds.length > 0 ? edgeIds : nodeIds
                    }

                    onLinkRequested: (from, to) => {
                        var r = browser.addLink(from, to, "causes")
                        root._report(r)
                        // The inspector opens on the new claim, where strength is one click away —
                        // which is the whole reason the drag does not stop to ask for it.
                        if (r.ok === true && r.edgeId) {
                            root._selectedEdgeId = r.edgeId
                            root._graphEdgeSel = [ r.edgeId ]
                            root.select("links", r.edgeId, false)
                        }
                    }
                    onRepointRequested: (edgeId, end, newId) => {
                        var r = browser.repointCause(edgeId, end, newId)
                        root._report(r)
                        if (r.ok === true && r.edgeId) {
                            root._selectedEdgeId = r.edgeId
                            root._graphEdgeSel = [ r.edgeId ]
                            root.select("links", r.edgeId, false)
                        }
                    }
                    onCreateLinkedRequested: (objType, name, otherId, end) =>
                        root.doCreateAttached(objType, name, otherId, end)

                    onVerbInvoked: (verb, arg) => root.doGraphVerb(verb, arg)
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

                        // Only what is genuinely status. The unsaved count and `revert` both left:
                        // the count was drawn twice and `revert` was a 12px word nowhere near the
                        // Save it undoes. Both now live in the commit cluster, together.
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

            // ── Inspector ─────────────────────────────────────────────────────
            // The Edits history used to share this slot behind an `Edits` button. It moved into the
            // popover under the count that describes it, which is where an author looking at "12
            // unsaved" actually wants it — so the inspector is now just the inspector.
            Item {
                // Folded, the pane keeps a strip just wide enough to hold the way back. Zero width
                // would be tidier and would strand the author: the only route back would be a
                // shortcut nobody has been told about.
                Layout.preferredWidth: root._inspectorFolded ? Theme.sp(26) : root._inspectorWidth
                Layout.fillHeight:     true
                visible: root._showInspector

                Behavior on Layout.preferredWidth {
                    NumberAnimation { duration: Theme.durationFast; easing.type: Easing.OutCubic }
                }

                // The way back, in the strip the fold leaves behind. Points left because that is
                // where the pane comes back from.
                Rectangle {
                    id: unfoldButton
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.top:              parent.top
                    anchors.topMargin:        Theme.sp(12)
                    visible: root._inspectorFolded
                    width:   Theme.sp(22)
                    height:  Theme.sp(20)
                    radius:  Theme.radius
                    color:   unfoldMa.containsMouse ? Theme.colorBg2 : "transparent"
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                    Text {
                        anchors.centerIn: parent
                        text: "‹‹"
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzBody2
                        color: unfoldMa.containsMouse ? Theme.colorText2 : Theme.colorText3
                    }

                    ToolTip.visible: unfoldMa.containsMouse
                    ToolTip.text: qsTr("Show the inspector")
                                  + (Qt.platform.os === "osx" ? "  ⌘⇧\\" : "  Ctrl+Shift+\\")
                    ToolTip.delay: 400

                    PpPressable {
                        id: unfoldMa
                        hoverScale: 1.0
                        onClicked: appSettings.diagnosticsInspectorCollapsed = false
                    }
                }

                ModelInspector {
                    id: inspector
                    anchors.fill: parent
                    visible: !root._inspectorFolded
                    onCollapseRequested: appSettings.diagnosticsInspectorCollapsed = true
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
                        root._openPickerNear(causePicker, inspector, inspector.actionOrigin)
                    }
                    onAddMeasureRequested: {
                        root._openPickerNear(measurePicker, inspector, inspector.actionOrigin)
                    }
                    onAddCorridorRequested: {
                        root._openPickerNear(corridorPicker, inspector, inspector.actionOrigin)
                    }
                    onBindingCycled: (contextId, applicable, material, clear) => {
                        if (clear) root._report(browser.clearBinding(root._selectedId, contextId))
                        else       root._report(browser.setBinding(root._selectedId, contextId,
                                                                   applicable, material))
                    }
                    // The reference pane's two buttons. Copying a citation is not an edit and does
                    // not touch the undo stack; opening a DOI leaves the app entirely.
                    onRowActionRequested: (action, label) => {
                        if (action === "copyCitation") {
                            var csl = browser.referenceCsl(root._selectedId)
                            if (csl === "") { root._report({ ok: false,
                                                             message: qsTr("Nothing to copy.") })
                                              return }
                            clipboard.setText(csl)
                            toast.severity = "info"
                            toast.show(qsTr("CSL-JSON copied"))
                        } else if (action === "openDoi") {
                            var url = browser.referenceDoiUrl(root._selectedId)
                            if (url !== "") Qt.openUrlExternally(url)
                        } else if (action === "repointFrom" || action === "repointTo") {
                            root.doRepoint(root._selectedId,
                                           action === "repointFrom" ? "from" : "to")
                        }
                    }
                    // The one live control on an otherwise imported page.
                    onClaimStrengthChanged: (linkId, strength) => root._report(
                        browser.setField("links", linkId, "strength", strength))
                    // One write path: the inspector's editors go through the same setField() the
                    // table's inline editor does, so a field cannot behave differently depending on
                    // which surface it was typed into.
                    onFieldCommitted: (field, value) =>
                        root._report(browser.setField(root._selectedType, root._selectedId,
                                                      field, value))
                    onDuplicateRequested: root.doDuplicate(root._selectedId, root._selectedType)
                    onRemoveRequested:    root.doRemove(root._selectedId, root._selectedType)
                    onAddRowRequested: (action) => {
                        settlesPicker.mode = action
                        root._openPickerNear(settlesPicker, inspector, inspector.actionOrigin)
                    }
                    onRemoveRowRequested: (kind, id) => {
                        if (kind === "measure")
                            root._report(browser.removeMeasureFrom(root._selectedId, id))
                        else if (kind === "settles")
                            root._report(browser.removeScreenSettles(root._selectedId, id))
                        else if (kind === "answers")
                            root._report(browser.removeDrillAnswers(root._selectedId, id))
                        else
                            root._report(browser.removeLink(id, root._selectedId, "causes"))
                    }
                }
            }
        }
    }

    // ── Type-ahead pickers ────────────────────────────────────────────────────
    // The inspector's entrance to the same question the canvas asks, with the same control and the
    // same words: which condition causes this one, existing or new.
    //
    // The candidate list is computed for the end that MOVES. It used to be computed for the other
    // one — `linkCandidates("causes", selectedId)` answers "what could this cause", while the write
    // underneath is `picked → selected`, which is the opposite question — so it offered targets
    // addLink() then refused as cycles. Two different directions, one of them silently wrong; the
    // `end` argument is what makes them the same question now.
    ModelCausePicker {
        id: causePicker
        parent: root
        title: qsTr("Add a cause")
        candidateSource: (text) => {
            if (root._revision < 0 || root._selectedId === "") return []
            return browser.linkCandidates("causes", root._selectedId, text, "to")
        }
        onPicked: (id) => root._report(browser.addLink(id, root._selectedId, "causes"))
        onCreated: (objType, name) =>
            root.doCreateAttached(objType, name, root._selectedId, "to")
    }

    // The table-side entrance to a re-point. The candidate list is pre-filtered for the end that is
    // MOVING, against the one that is staying put — so the picker cannot offer a target the write
    // would refuse, and the two entrances agree about what is legal because they ask the same layer.
    ModelPicker {
        id: repointPopup
        parent: root
        candidateSource: (text) => {
            if (root._revision < 0 || root._repointEdgeId === "") return []
            var row = browser.rows("links", { ids: [ root._repointEdgeId ] })
            if (row.length === 0) return []
            // The end that STAYS is the opposite of the one being moved, and it is what the legal
            // set is computed against.
            var staying = root._repointEnd === "from" ? row[0].toId : row[0].fromId
            return browser.linkCandidates("causes", staying, text,
                                          root._repointEnd === "from" ? "to" : "from")
        }
        onPicked: (id) => {
            var r = browser.repointCause(root._repointEdgeId, root._repointEnd, id)
            root._report(r)
            if (r.ok === true && r.edgeId) {
                root._selectedEdgeId = r.edgeId
                root._graphEdgeSel = [ r.edgeId ]
                root.select("links", r.edgeId, false)
            }
            root._repointEdgeId = ""
        }
    }

    ModelPicker {
        id: corridorPicker
        parent: root
        title: qsTr("Add corridor at")
        candidateSource: (text) => {
            if (root._revision < 0) return []
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

    // One picker for both relationships, because it is one gesture — pick the characteristic this
    // screen settles, or the one this drill answers. `mode` says which, and the candidate list comes
    // pre-filtered from C++ either way, so an illegal pairing cannot be constructed.
    ModelPicker {
        id: settlesPicker
        parent: root
        property string mode: "settles"
        title: mode === "settles" ? qsTr("Settles which characteristic?")
                                  : qsTr("Answers which characteristic?")
        candidateSource: (text) => {
            if (root._revision < 0) return []
            return settlesPicker.mode === "settles"
                       ? browser.screenCandidates(root._selectedId, text)
                       : browser.drillCandidates(root._selectedId, text)
        }
        onPicked: (id) => root._report(settlesPicker.mode === "settles"
                                           ? browser.addScreenSettles(root._selectedId, id)
                                           : browser.addDrillAnswers(root._selectedId, id))
    }

    ModelMint {
        id: mintPopup
        parent:  root
        browser: browser
        onMinted: (id) => {
            // Minted from a graph drag: attach it to the condition the drag came from, which is
            // what the author was in the middle of doing. A measure DETECTS a characteristic — it
            // is not a cause of one — so the attachment is the detection join, not an edge.
            if (root._pendingMeasureHost !== "") {
                root._report(browser.addMeasureTo(root._pendingMeasureHost, id))
                root._pendingMeasureHost = ""
            }
            root._type = "measures"
            root.selectFresh("measures", id)
        }
        onClosed: root._pendingMeasureHost = ""
    }

    ModelPolicyPicker {
        id: policyPopup
        parent:  root
        browser: browser
        gradePolicy: appSettings.diagnosticsGradePolicy
        // Written straight to the ONE global AppSettings, per the single-shared-instance rule. It is
        // not an edit to the library, so it deliberately does not touch the undo stack.
        onPicked: (name) => { appSettings.diagnosticsGradePolicy = name
                              toast.severity = "info"
                              toast.show(qsTr("Grading as %1").arg(name)) }
    }

    ModelUnsaved {
        id: unsavedPopup
        parent: root
        edits:  browser.edits
        sessionScoped: browser.undoIsSessionScoped
        onWindTo:   (index) => root._report(browser.undoTo(index))
        onRevertAll: root.doRevert()
    }

    ModelTools {
        id: toolsPopup
        parent:  root
        browser: browser
        revision: root._revision
        // Counted over the DRAFT, not the file: these are what a reset would take away, and an edit
        // made a minute ago is as much a loss as one saved last week.
        hasLocalContent: browser.overriddenCount + browser.authoredCount > 0
        changedCount:    browser.overriddenCount
        yoursCount:      browser.authoredCount
        onResetRequested: resetConfirm.open()
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

    // ── The two prompts ───────────────────────────────────────────────────────
    //
    // Same structure, deliberately different colour. One is a call to act and loses nothing; the
    // other destroys saved work. See ModelConfirm.qml for why that distinction is drawn rather than
    // assumed.

    ModelConfirm {
        id: saveConfirm
        parent: root
        tone:   "attention"
        title:  qsTr("Your diagnostics will differ from the standard")
        body:   qsTr("Saving writes your edits over the diagnostic model that ships with PinPoint "
                     + "Studio. From here on this install screens, grades and explains against your "
                     + "version, so its results are no longer directly comparable with an unmodified "
                     + "install.\n\nNothing is lost — shipped items keep their original underneath "
                     + "and the Source column shows which is which.")
        confirmText: qsTr("Save changes")
        onConfirmed: {
            // Confirming IS the acknowledgement; there is no checkbox to forget to tick.
            appSettings.diagnosticsBaseModelWarningAck = true
            root._report(browser.save())
        }
    }

    ModelConfirm {
        id: resetConfirm
        parent: root
        tone:   "error"
        title:  qsTr("Reset to the standard model")
        body: {
            var changed = browser.overriddenCount, mine = browser.authoredCount
            var what = []
            if (changed > 0) what.push(qsTr("%n change(s) to shipped items", "", changed))
            if (mine > 0)    what.push(qsTr("%n item(s) you created", "", mine))
            return qsTr("This removes %1, and puts this install back on the diagnostic model that "
                        + "ships with PinPoint Studio.\n\nYour current model is copied to a dated "
                        + "backup file first, and ⌘Z undoes this until you close the app.")
                     .arg(what.join(qsTr(" and ")))
        }
        confirmText: qsTr("Reset")
        onConfirmed: root.doResetToStandard()
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
    // ⌘⇧\ folds the inspector, beside ⌘\ for the settings sidenav — the same gesture for the same
    // kind of decision, one modifier apart, so learning either teaches the other.
    Shortcut {
        sequence: "Ctrl+Shift+\\"
        onActivated: appSettings.diagnosticsInspectorCollapsed =
                         !appSettings.diagnosticsInspectorCollapsed
    }
    Shortcut {
        sequence: "G"
        // Window-wide shortcuts outrank a focused text field, so an ungated "G" would make the
        // letter untypeable in the search box and in every inline editor. It is a view toggle, not
        // a text key, and it has to stand down while something is being typed into.
        enabled: !searchField.activeFocus && !table.editing
                 && !causePicker.opened && !measurePicker.opened && !settlesPicker.opened
        onActivated: root._view = root._view === "graph" ? "table" : "graph"
    }

    Component.onCompleted: table.forceActiveFocus()
}
