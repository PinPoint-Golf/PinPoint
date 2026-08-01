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

// The table. ONE delegate for every content type: the shell does not change, only the columns do,
// and both the columns and the cells inside them are handed over by ModelBrowser. Nothing here
// knows what a Measure is.
//
// Editing lives here rather than in a sheet: there is no edit mode, no Edit button and no modal. A
// cell whose value is scalar edits in place — text inline, enums as a one-click list — and the list
// stays on screen through every edit.
//
// The one departure from the brief is WHEN an editor opens. The brief says one click should select
// the row and open the editor together; in use that meant clicking a name to read the inspector
// dropped a caret into it, so navigating paid for editing. So: a click selects, and a click on a
// row that is ALREADY selected edits. See the handler at the foot of the cell delegate for the
// full reasoning and what it costs.
Item {
    id: root

    // Column specs and row maps, straight from the façade.
    property var columns: []
    property var rows:    []

    property string selectedId: ""
    // Multi-select, for bulk-set. Plain array of ids; the toolbar reads its length.
    property var    selection:  []

    property string sortKey:    ""
    property bool   descending: false

    // Set false for the types this panel does not write (screens, drills, references, health), so a
    // cell never looks live when nothing is listening.
    property bool editable: true

    signal activated(string id)                       // row clicked / arrowed onto
    signal commit(string id, string field, var value) // a cell was edited
    signal duplicateRequested(string id)
    signal removeRequested(string id)
    signal sortRequested(string key)
    // Draw this row's graph. Emitted AFTER activated(), so whoever is listening has already been
    // told what to centre on before it is asked to show the picture.
    signal graphRequested(string id)

    readonly property int rowHeight: Theme.sp(32)     // INCLUSIVE of its 1px separator

    // The one column that takes the slack. Everything else is fixed, so the name column is what
    // gets starved as panes are added — hence the floor, which the brief sets at sp(240).
    readonly property int _fixedWidth: {
        var w = 0
        for (var i = 0; i < columns.length; i++)
            if (!columns[i].flex) w += Theme.sp(columns[i].width)
        return w
    }
    readonly property int _flexWidth:
        Math.max(Theme.sp(240), listView.width - _fixedWidth - Theme.sp(36))

    function _columnWidth(col) { return col.flex ? _flexWidth : Theme.sp(col.width) }

    // The x of the FLEX column's right edge — where the row's Graph chip sits.
    //
    // The flex column, not column zero. It is the one holding the label and taking the slack, which
    // is the column a reader is actually scanning; on a cross-type search result that is `name` at
    // index 1, with `Type` ahead of it, and hard-coding zero would park the chip over the type
    // badge. Exactly one column flexes — the row-shape test asserts it — so this cannot be ambiguous.
    readonly property int _flexColumnRight: {
        var x = Theme.sp(18)                       // the Row's own left margin
        for (var i = 0; i < columns.length; i++) {
            var w = _columnWidth(columns[i])
            if (columns[i].flex) return x + w
            x += w
        }
        return x
    }

    function _toneColor(tone) {
        switch (tone) {
        case "good":   return Theme.colorRagGood
        case "watch":  return Theme.colorRagWatch
        case "warn":   return Theme.colorWarn
        case "fault":  return Theme.colorRagFault
        case "error":  return Theme.colorError
        case "accent": return Theme.colorAccent
        case "dim":    return Theme.colorText3
        case "none":   return Theme.colorRagNone
        }
        return Theme.colorText
    }

    function indexOfId(id) {
        for (var i = 0; i < rows.length; i++) if (rows[i].id === id) return i
        return -1
    }

    function selectRow(index) {
        if (index < 0 || index >= rows.length) return
        endEdit()
        root.selectedId = rows[index].id
        root.selection  = [ rows[index].id ]
        listView.currentIndex = index
        listView.positionViewAtIndex(index, ListView.Contain)
        root.activated(rows[index].id)
    }

    // ── Editing state ─────────────────────────────────────────────────────────
    // One cell at a time, addressed by row id + column index so it survives the list re-sorting
    // under it after a commit.
    property string _editRowId: ""
    property int    _editCol:   -1

    // True while a cell has the keyboard. Read by the panel's single-letter shortcuts, which must
    // stand down rather than eat the character being typed.
    readonly property bool editing: _editRowId !== ""

    function beginEdit(rowId, col) {
        if (!root.editable) return
        var r = rows[indexOfId(rowId)]
        if (!r) return
        var c = r.cells[col]
        if (!c || !c.editable) return
        root._editRowId = rowId
        root._editCol   = col
    }

    function endEdit() { root._editRowId = ""; root._editCol = -1 }

    // Anything that rebuilds the rows underneath an open editor closes it first. Sorting is handled
    // here because the table owns it; the panel calls endEdit() for the changes it owns (type,
    // facets, search). Without this the editor stays bound to a row id that may no longer be on
    // screen, and reappears — stale — the moment that row comes back.
    onSortKeyChanged:    endEdit()
    onDescendingChanged: endEdit()
    onColumnsChanged:    endEdit()

    // Tab commits and moves RIGHT, to the next cell that can actually be edited — skipping the
    // inert ones rather than parking the caret on a cell that ignores typing.
    function editNext(rowId, col, step) {
        var r = rows[indexOfId(rowId)]
        if (!r) { endEdit(); return }
        var i = col + step
        while (i >= 0 && i < r.cells.length) {
            if (r.cells[i].editable) { beginEdit(rowId, i); return }
            i += step
        }
        endEdit()
    }

    // ── Header ────────────────────────────────────────────────────────────────
    Rectangle {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(28)
        color:  "transparent"

        Row {
            anchors.fill: parent
            anchors.leftMargin:  Theme.sp(18)
            anchors.rightMargin: Theme.sp(18)

            Repeater {
                model: root.columns
                delegate: Item {
                    id: headerCell
                    required property var  modelData
                    required property int  index

                    width:  root._columnWidth(modelData)
                    height: header.height

                    readonly property bool active:     root.sortKey === modelData.key
                    readonly property bool alignRight: modelData.align === "right"

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left:  headerCell.alignRight ? undefined : parent.left
                        anchors.right: headerCell.alignRight ? parent.right : undefined
                        anchors.rightMargin: headerCell.alignRight ? Theme.sp(10) : 0
                        width: parent.width - Theme.sp(10)
                        horizontalAlignment: headerCell.alignRight ? Text.AlignRight : Text.AlignLeft
                        // Emphasis is colour and case, never weight — the sorted column is accented
                        // and carries the caret, and everything else stays at the same weight.
                        text: headerCell.modelData.title
                              + (headerCell.active ? (root.descending ? "  ▾" : "  ▴") : "")
                        font.family:         Theme.fontBody
                        font.pixelSize:      Theme.fontSzMicro
                        font.weight:         Theme.fontBodyWeight
                        font.letterSpacing:  Theme.trackingMicro
                        font.capitalization: Font.AllUppercase
                        color: headerCell.active ? Theme.colorAccent : Theme.colorText3
                        elide: Text.ElideRight
                    }

                    PpPressable {
                        hoverScale: 1.0
                        enabled:    headerCell.modelData.sortable
                        onClicked:  root.sortRequested(headerCell.modelData.key)
                    }
                }
            }
        }

        Rectangle {
            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
            height: 1
            color:   Theme.colorBorderMid
            opacity: Theme.borderOpacityNormal
        }
    }

    // ── Rows ──────────────────────────────────────────────────────────────────
    ListView {
        id: listView
        anchors { left: parent.left; right: parent.right; top: header.bottom; bottom: parent.bottom }
        clip:   true
        model:  root.rows
        // A pixel-scrolled list, not row-snapped: at sp(32) a snapped list jumps a whole row per
        // wheel notch, which reads as stuttering rather than as scrolling.
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Text {
            anchors.centerIn: parent
            visible: listView.count === 0
            text:    qsTr("Nothing here")
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorText3
        }

        delegate: Item {
            id: rowItem
            required property var modelData
            required property int index

            width:  listView.width
            height: root.rowHeight

            readonly property bool isSelected: root.selection.indexOf(modelData.id) >= 0
            readonly property bool isCurrent:  root.selectedId === modelData.id
            readonly property bool hovered:    rowHover.hovered

            // Whether this row is a thing the graph can draw. A search result knows its own type;
            // an ordinary row carries the type it came from. Health is the one exclusion, and the
            // façade agrees — graph() returns nothing for it, because a finding is a statement
            // ABOUT an object rather than an object with a neighbourhood of its own.
            readonly property bool canGraph: {
                var t = modelData.resultType || modelData.type || ""
                return t !== "" && t !== "health"
            }

            Rectangle {
                anchors.fill: parent
                color: rowItem.isCurrent  ? Theme.colorAccentLight
                     : rowItem.isSelected ? Theme.colorBg3
                     : rowItem.hovered    ? Theme.colorBg2
                                          : "transparent"
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }
            }

            // The dirty gutter: 2px colorAccent down the left edge of a row with unsaved changes.
            Rectangle {
                anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
                width:   2
                color:   Theme.colorAccent
                visible: rowItem.modelData.dirty === true
            }

            HoverHandler { id: rowHover }

            Row {
                anchors.fill: parent
                anchors.leftMargin:  Theme.sp(18)
                anchors.rightMargin: Theme.sp(18)

                Repeater {
                    model: rowItem.modelData.cells
                    delegate: Item {
                        id: cellItem
                        required property var modelData
                        required property int index

                        readonly property var  col:     root.columns[index]
                        readonly property bool editing: root._editRowId === rowItem.modelData.id
                                                        && root._editCol === index

                        width:  col ? root._columnWidth(col) : 0
                        height: rowItem.height

                        // The status dot rides in the FIRST cell, so it lines up under the name
                        // column whatever the type is.
                        Rectangle {
                            id: dot
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            width:  Theme.sp(7)
                            height: Theme.sp(7)
                            radius: width / 2
                            visible: cellItem.index === 0 && rowItem.modelData.dot !== undefined
                                     && rowItem.modelData.dot !== ""
                            color: root._toneColor(rowItem.modelData.dot)
                        }

                        // The label. `fillWidth` + elide with trailing padding, because a nowrap
                        // sibling beside it would otherwise collapse it to zero width — which is
                        // exactly how the name column disappears at narrow widths.
                        Text {
                            id: cellText
                            visible: !cellItem.editing
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left:       parent.left
                            anchors.leftMargin: dot.visible ? Theme.sp(7) + Theme.sp(9) : 0
                            anchors.right:      parent.right
                            // The flex column yields to the Graph chip rather than being covered by
                            // it. The chip is opaque and could simply occlude, but this is the
                            // column holding the NAME: what it would hide is the end of the very
                            // word the reader is scanning for, and an elision they can see is
                            // honest where a covered tail is not.
                            anchors.rightMargin: (cellItem.col && cellItem.col.flex
                                                  && graphChip.visible)
                                                     ? graphChip.width + Theme.sp(14)
                                                     : Theme.sp(10)
                            horizontalAlignment: cellItem.col && cellItem.col.align === "right"
                                                     ? Text.AlignRight : Text.AlignLeft
                            text: cellItem.modelData.text
                            font.family: (cellItem.col && cellItem.col.mono) || cellItem.modelData.mono
                                             ? Theme.fontData : Theme.fontBody
                            font.pixelSize: cellItem.index === 0 ? Theme.fontSzBody : Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color: root._toneColor(cellItem.modelData.tone)
                            elide: Text.ElideRight
                        }

                        // Editable cells announce themselves on hover, but only on the SELECTED
                        // row — which is exactly where a click edits rather than selects. Without
                        // this, select-then-edit is a rule with no visible trace, and a numeric
                        // cell in particular reads as a printed figure rather than a control.
                        Rectangle {
                            visible: root.editable && cellItem.modelData.editable
                                     && rowItem.isSelected && cellPress.containsMouse
                                     && !cellItem.editing
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            anchors.rightMargin:  Theme.sp(8)
                            anchors.bottomMargin: Theme.sp(4)
                            height:  1
                            color:   Theme.colorAccent
                            opacity: 0.5
                        }

                        // An enum opens a list rather than a caret, so it says so.
                        Text {
                            visible: root.editable && cellItem.modelData.kind === "enum"
                                     && rowItem.isSelected && cellPress.containsMouse
                                     && !cellItem.editing
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.sp(2)
                            text:           "▾"
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorAccent
                        }

                        // "Yours" marker on a field the user has overridden — per-field, so an
                        // author can tell which part of a shipped object they changed.
                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.sp(3)
                            width:  Theme.sp(3)
                            height: Theme.sp(3)
                            radius: width / 2
                            color:  Theme.colorAccent
                            visible: cellItem.modelData.own === true && !cellItem.editing
                        }

                        // ── Inline text editor ────────────────────────────────
                        TextInput {
                            id: inlineEdit
                            visible: cellItem.editing
                                     && (cellItem.modelData.kind === "text"
                                         || cellItem.modelData.kind === "number")
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left:  parent.left
                            anchors.right: parent.right
                            anchors.rightMargin: Theme.sp(8)
                            horizontalAlignment: cellItem.modelData.kind === "number"
                                                     ? TextInput.AlignRight : TextInput.AlignLeft
                            // A corridor figure is a number, and the keyboard says so before the
                            // facade has to refuse it. An empty string is allowed through because
                            // clearing a plausibility bound is a real edit — "no bound" is not zero.
                            validator: cellItem.modelData.kind === "number"
                                           ? numberValidator : null
                            font.family:    cellItem.modelData.kind === "number"
                                                ? Theme.fontData : Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color:          Theme.colorText
                            selectByMouse:  true

                            DoubleValidator { id: numberValidator; notation: DoubleValidator.StandardNotation }

                            // What the cell held when the editor opened. Kept so a commit can be
                            // skipped when nothing changed — an Enter on an untouched cell should
                            // not put an entry on the undo stack — and so focus loss can tell a
                            // real edit from a delegate being torn down underneath it.
                            property string _original: ""
                            property bool   _seeded:   false

                            function seed() {
                                if (!visible || _seeded) return
                                _original = cellItem.modelData.value !== undefined
                                                ? cellItem.modelData.value : cellItem.modelData.text
                                text      = _original
                                _seeded   = true
                                forceActiveFocus()
                                selectAll()
                            }

                            // BOTH, and that is the bug this pair fixes. onVisibleChanged does not
                            // fire for a delegate that is CREATED already visible — QML emits no
                            // change signal for an initial value — and switching facets destroys
                            // and rebuilds delegates. A row still marked as editing therefore came
                            // back with an empty TextInput sitting on top of its hidden label, so
                            // the name looked like it had been wiped.
                            Component.onCompleted: seed()
                            onVisibleChanged: {
                                if (visible) seed()
                                else         _seeded = false
                            }

                            function commitIfChanged() {
                                if (!_seeded || text === _original) return
                                root.commit(rowItem.modelData.id, cellItem.modelData.field, text)
                            }

                            // Clicking away keeps what you typed. That is this project's existing
                            // convention — PpTextField commits on focus-out for the same reason —
                            // and it is guarded twice: nothing is written unless the editor was
                            // properly seeded AND the text actually changed, so a delegate losing
                            // focus because it is being destroyed cannot commit an empty string
                            // over a name.
                            onActiveFocusChanged: {
                                if (activeFocus || !cellItem.editing) return
                                commitIfChanged()
                                root.endEdit()
                            }

                            // Enter commits and STAYS, Tab commits and moves right, Esc cancels.
                            Keys.onReturnPressed: { commitIfChanged(); root.endEdit() }
                            Keys.onEnterPressed:  { commitIfChanged(); root.endEdit() }
                            Keys.onEscapePressed: {
                                // Cancel means cancel: put the seed back before the editor closes,
                                // so the focus-out handler above has nothing left to commit.
                                text = _original
                                root.endEdit()
                            }
                            Keys.onTabPressed: {
                                commitIfChanged()
                                root.editNext(rowItem.modelData.id, cellItem.index, 1)
                            }
                            Keys.onBacktabPressed: {
                                commitIfChanged()
                                root.editNext(rowItem.modelData.id, cellItem.index, -1)
                            }
                        }

                        Rectangle {
                            visible: cellItem.editing
                                     && (cellItem.modelData.kind === "text"
                                         || cellItem.modelData.kind === "number")
                            anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                            anchors.rightMargin:  Theme.sp(8)
                            anchors.bottomMargin: Theme.sp(4)
                            height: 1
                            color:  Theme.colorAccent
                        }

                        // ── Enum popup ────────────────────────────────────────
                        // One click opens, one click picks: the budget for group / reach / tier /
                        // strength is exactly two clicks and this is both of them.
                        Popup {
                            id: enumPopup
                            visible: cellItem.editing && cellItem.modelData.kind === "enum"
                            y: cellItem.height
                            padding: Theme.sp(4)
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                            onClosed: if (cellItem.editing) root.endEdit()

                            background: Rectangle {
                                color:        Theme.colorSurface
                                radius:       Theme.radius
                                border.width: 1
                                border.color: Theme.colorBorderStrong
                            }

                            contentItem: Column {
                                spacing: 0
                                Repeater {
                                    model: cellItem.modelData.options || []
                                    delegate: Rectangle {
                                        id: enumOption
                                        required property var modelData
                                        readonly property bool chosen:
                                            modelData.value === cellItem.modelData.value

                                        width:  Math.max(Theme.sp(150), optionText.implicitWidth + Theme.sp(24))
                                        height: Theme.sp(28)
                                        radius: Theme.radius
                                        color:  optionHover.hovered ? Theme.colorBg2 : "transparent"

                                        HoverHandler { id: optionHover }

                                        Text {
                                            id: optionText
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.left: parent.left
                                            anchors.leftMargin: Theme.sp(10)
                                            text: enumOption.modelData.label
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzBody2
                                            font.weight:    Theme.fontBodyWeight
                                            color: enumOption.chosen ? Theme.colorAccent
                                                                     : Theme.colorText
                                        }

                                        PpPressable {
                                            hoverScale: 1.0
                                            onClicked: {
                                                root.commit(rowItem.modelData.id,
                                                            cellItem.modelData.field,
                                                            enumOption.modelData.value)
                                                root.endEdit()
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // ── Select first, then edit ───────────────────────────
                        //
                        // A click on a row that is NOT already selected only selects it. A click on
                        // a cell of a row that IS already selected opens that cell's editor.
                        //
                        // The brief asks for one click to do both ("selecting a row IS opening its
                        // editor", budget: change a strength in 1 click). That was built, used, and
                        // rejected: clicking a name to read the inspector or re-centre the graph
                        // dropped a caret into it every time, so the common gesture — navigating —
                        // paid for the rare one. Mark's call, 2026-07-30, and the cost is stated
                        // rather than hidden: the FIRST edit on a row is now two clicks. Every edit
                        // after it, down that same row, is still one.
                        //
                        // The mode this introduces is the SELECTION, which is already drawn on
                        // screen — that is what makes it different from the toolbar toggle the
                        // brief bans, where the state is invisible until you get it wrong.
                        PpPressable {
                            id: cellPress
                            hoverScale: 1.0
                            enabled:    !cellItem.editing
                            onClicked: (mouse) => {
                                var id = rowItem.modelData.id
                                if (mouse && (mouse.modifiers & Qt.ControlModifier
                                              || mouse.modifiers & Qt.MetaModifier)) {
                                    var next = root.selection.slice()
                                    var at   = next.indexOf(id)
                                    if (at >= 0) next.splice(at, 1)
                                    else         next.push(id)
                                    root.selection = next
                                    root.selectedId = id
                                    root.activated(id)
                                    return
                                }
                                if (mouse && (mouse.modifiers & Qt.ShiftModifier)
                                    && root.selectedId !== "") {
                                    var from = root.indexOfId(root.selectedId)
                                    var to   = rowItem.index
                                    var lo   = Math.min(from, to), hi = Math.max(from, to)
                                    var range = []
                                    for (var i = lo; i <= hi; i++) range.push(root.rows[i].id)
                                    root.selection = range
                                    root.activated(id)
                                    return
                                }

                                // Already in the selection, so this click is the edit. The
                                // selection is left ALONE rather than collapsed to this one row:
                                // collapsing it would make bulk-set unreachable, because the click
                                // that starts the edit would throw away the other eleven rows it
                                // was meant to apply to.
                                if (root.selection.indexOf(id) >= 0) {
                                    root.selectedId = id
                                    listView.currentIndex = rowItem.index
                                    root.activated(id)
                                    root.beginEdit(id, cellItem.index)
                                    return
                                }

                                root.endEdit()
                                root.selectedId = id
                                root.selection  = [ id ]
                                listView.currentIndex = rowItem.index
                                root.activated(id)
                            }
                        }

                    }
                }
            }

            // ── Draw this row ─────────────────────────────────────────────────
            //
            // The graph already answers for any selected row — `G` toggles the middle pane — but
            // that is a keyboard fact, and nothing on a row said the picture was a click away. The
            // reach and commonality columns made that worse rather than better: they rank a row by
            // what it connects to, which invites exactly the question ("connected to WHAT?") that
            // only the graph answers.
            //
            // It sits at the right edge of the FLEX column — the one holding the name — for the
            // same reason the status dot rides in that column: it is where the eye already is. At
            // the row's trailing edge it was a cursor journey across every numeric column to reach
            // a control about the row you were already reading, and on a wide window that journey
            // is most of the panel.
            //
            // Shown on HOVER as well as on the selected row, so it costs one click from a row you
            // have not visited rather than two. It never appears on a row that is neither: 95 live
            // buttons down a list is the wall of equal-weight chrome this panel keeps refusing.
            //
            // Declared after the Row and NOT inside the name cell, on purpose. Inside the delegate
            // it would be built once per cell — eight columns' worth of hidden chips per row, for
            // one that can ever show — and "the name" is not reliably cell zero anyway: a
            // cross-type search result puts `Type` there and the name second.
            Rectangle {
                id: graphChip

                // Never over an open editor. The editor owns the row while it is up, and a control
                // floating on a caret is one misclick from committing something.
                visible: rowItem.canGraph
                         && (rowItem.hovered || rowItem.isCurrent)
                         && root._editRowId !== rowItem.modelData.id

                anchors.verticalCenter: parent.verticalCenter
                x: root._flexColumnRight - width - Theme.sp(10)

                width:  graphChipText.implicitWidth + Theme.sp(14)
                height: Theme.sp(20)
                radius: Theme.radius
                color:  graphPress.containsMouse ? Theme.colorAccentLight : Theme.colorBg3
                border.width: 1
                border.color: graphPress.containsMouse ? Theme.colorAccent : Theme.colorBorderMid
                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                // The word the Table/Graph toggle already uses, not a glyph — there is nothing to
                // learn, and no font-coverage gamble on a symbol for "network". The shortcut rides
                // in the tooltip, so the mouse path teaches the keyboard one.
                Text {
                    id: graphChipText
                    anchors.centerIn: parent
                    text:                qsTr("Graph")
                    font.family:         Theme.fontBody
                    font.pixelSize:      Theme.fontSzMicro
                    font.weight:         Theme.fontBodyWeight
                    font.letterSpacing:  Theme.trackingMicro
                    font.capitalization: Font.AllUppercase
                    color: graphPress.containsMouse ? Theme.colorAccent : Theme.colorText2
                }

                ToolTip.visible: graphPress.containsMouse
                ToolTip.text:    qsTr("Show in graph  ·  G")
                ToolTip.delay:   400

                PpPressable {
                    id: graphPress
                    hoverScale: 1.0
                    enabled:    graphChip.visible
                    onClicked: {
                        var id = rowItem.modelData.id
                        // Selects exactly as a plain click on the row would, THEN asks for the
                        // picture. Two steps rather than one because the graph centres on the
                        // selection: asking without selecting first would draw whatever was
                        // selected before, which on a hovered row is another row entirely — the
                        // pointer over one thing and the canvas showing another.
                        root.endEdit()
                        root.selectedId = id
                        root.selection  = [ id ]
                        listView.currentIndex = rowItem.index
                        root.activated(id)
                        root.graphRequested(id)
                    }
                }
            }

            Rectangle {
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                height:  1
                color:   Theme.colorBorder
                opacity: Theme.borderOpacityNormal
            }
        }
    }

    // ── Keyboard ──────────────────────────────────────────────────────────────
    // A power author should be able to work a dozen rows without touching the mouse.
    focus: true
    Keys.onUpPressed:   root.selectRow(root.indexOfId(root.selectedId) - 1)
    Keys.onDownPressed: root.selectRow(root.indexOfId(root.selectedId) + 1)
    Keys.onPressed: (event) => {
        if (root._editRowId !== "") return   // the editor owns the keyboard while it is open
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            // Enter on a selected row opens its first editable cell. With select-then-edit this is
            // the keyboard's SECOND click — arrow onto a row, Enter to start editing it — so the
            // keyboard path costs exactly what the mouse path does.
            root.editNext(root.selectedId, -1, 1)
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            if (root.selectedId !== "") { root.removeRequested(root.selectedId); event.accepted = true }
        } else if (event.key === Qt.Key_D && (event.modifiers & Qt.ControlModifier
                                              || event.modifiers & Qt.MetaModifier)) {
            if (root.selectedId !== "") { root.duplicateRequested(root.selectedId); event.accepted = true }
        }
    }
}
