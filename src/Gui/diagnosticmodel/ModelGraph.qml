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
import QtQuick.Shapes
import QtQuick.Layouts
import QtQuick.Controls.Basic
import PinPointStudio

// The graph pane. It SWAPS the middle pane and keeps the rail and the inspector, so switching views
// keeps your place — selection is shared with the table.
//
// EVERY coordinate comes from ModelBrowser.graph(), which is dag_layout.h. QML positions nothing:
// rank assignment, ordering, routing and overlap are the only things about this surface that can be
// tested, and a layout computed inside delegate bindings is a layout nothing can assert. What this
// file does is draw, hit-test and drag.
//
// ── Editing: the gesture is the permission ──────────────────────────────────────────────────────
//
// There is NO editing mode and no editing toggle. A structural edit begins with a deliberate
// press-and-hold on the thing you mean, which opens the ring (RingMenu.qml). A mode would make
// permission a place you are standing rather than something you did: it has to be found, remembered
// and turned off, it is invisible at the moment of the accident it exists to prevent, and it forces
// affordance chrome onto every node for ever to advertise a state. 350 ms of pressure is a stronger
// consent signal than a switch flipped twenty minutes ago, and it cannot be left on.
//
// The price is that structural editing has no resting affordance here at all. It is paid three ways,
// and all three are load-bearing rather than nice: right-click opens the SAME ring (every desktop
// author tries it); the first hold of a session holds its labels a beat longer; and the inspector
// keeps every verb this ring has, worded identically, so an author who never discovers the ring
// loses no capability whatsoever.
//
// ── One input layer, not a handler per item ─────────────────────────────────────────────────────
//
// Every press on this canvas arrives at ONE MouseArea, which hit-tests the point itself. It cannot
// be a handler per node: the ring has to open on a link and on bare canvas too, and per-item
// handlers fight each other for the grab during exactly the held drag this whole surface is made
// of. So the nodes and the edge labels are drawn items with no input of their own, and everything
// below decides what a press meant.
Item {
    id: root

    property var    layoutData: ({})     // from ModelBrowser.graph()
    property string focusId:    ""
    property string selectedEdgeId: ""
    property bool   editable:   true

    // The marquee's selection, shared with the table's. Node ids and link row ids, kept apart
    // because §5.5's rule is that a marquee is one or the other and never a mixture.
    property var selectedNodeIds: []
    property var selectedEdgeIds: []

    // The node's own TYPE travels with it. Without it the receiver has to assume one, and the
    // measures lane is full of nodes that are not conditions — clicking one then asked for the
    // causal graph of a condition id that does not exist, and the graph went blank.
    signal nodeActivated(string nodeType, string id)
    signal edgeActivated(string rowId)
    signal linkRequested(string fromId, string toId)
    signal repointRequested(string edgeId, string end, string newId)
    signal createLinkedRequested(string objType, string name, string otherId, string end)
    signal selectionRequested(var nodeIds, var edgeIds)

    // Every ring spoke that is not this pane's own business. One signal rather than one per verb:
    // the ring is a menu, and a menu that needed a new signal per entry would be a menu the panel
    // had to be edited to extend.
    signal verbInvoked(string verb, var arg)

    // Asked ONCE, when a link drag arms — never per hover. { id: { reason, text } } for the refused.
    property var refusalsProbe: null
    // (type, id, field) -> [{ value, label, current }], at most three. The collar's contents.
    property var ringValuesProbe: null

    readonly property var _nodes: layoutData && layoutData.nodes ? layoutData.nodes : []
    readonly property var _edges: layoutData && layoutData.edges ? layoutData.edges : []

    // ── Zoom ──────────────────────────────────────────────────────────────────
    //
    // The graph opens FITTED, because a picture you have to hunt around before you can read is a
    // picture that has not answered anything yet. Ctrl+wheel then scales from there, so the fit is
    // a starting point rather than a cage.
    property real _userZoom: 1.0
    readonly property real _fitScale: {
        var gw = layoutData.width  || 0
        var gh = layoutData.height || 0
        if (gw <= 0 || gh <= 0 || canvas.width <= 0 || canvas.height <= 0) return 1
        // Never magnifies to fill: a four-node neighbourhood blown up to a full pane looks like a
        // diagram of something important, and it is not.
        return Math.min(1, canvas.width / gw, canvas.height / gh)
    }
    readonly property real _zoom: Math.max(0.2, Math.min(4.0, _fitScale * _userZoom))

    // A new layout is a new picture: it opens fitted again rather than inheriting a zoom that was
    // chosen for something else.
    onLayoutDataChanged: _userZoom = 1.0

    // ── Nudge: position is decoration, not data ───────────────────────────────
    //
    // Dragging a node BODY moves it for this session only. It is not written to the pack, not in
    // the unsaved list, not undoable, and gone on reload, radius change or focus change. That
    // preserves the rule the whole panel rests on — everything in the unsaved popover is content,
    // and everything content is in the unsaved popover — which a nudge that produced an undo entry
    // would break at both ends.
    //
    // If saved layouts are ever asked for, that is a VIEW feature (a named arrangement per user),
    // not a pack feature. It must not come in through this door.
    property var _nudges: ({})
    // Bumped on every change, and PASSED AS AN ARGUMENT to the lookup below. A binding that merely
    // mentioned a revision property would be dropped as a dead statement and would subscribe to
    // nothing; taking it as a parameter is what makes the dependency real.
    property int _nudgeRev: 0

    function _ndx(id, rev) { var n = root._nudges[id]; return n ? n.dx : 0 }
    function _ndy(id, rev) { var n = root._nudges[id]; return n ? n.dy : 0 }

    // How far an edge's LABEL moves when the nodes at its ends are nudged.
    //
    // The label is not on the line by accident — the layout puts it at the curve's midpoint,
    // `cubicAt(P0, C1, C2, P3, 0.5)` (dag_layout.cpp) — so it has to move by whatever that midpoint
    // moves by, or a dragged node leaves its strength word stranded over the gap the line used to
    // cross. Nudging the start moves P0 and C1; nudging the end moves C2 and P3. A cubic at t = 0.5
    // is (P0 + 3·C1 + 3·C2 + P3) / 8, so the midpoint shifts by (4·from + 4·to) / 8 — the MEAN of
    // the two nudges, not either one and not their sum.
    //
    // Gated by the same head/tail test the curve itself uses, so the two cannot disagree: only the
    // segment touching a node follows it, and a label sitting on a middle segment of a waypointed
    // edge is anchored to joints that did not move and correctly stays put. On a single-segment
    // edge that one segment is both ends, which is the ordinary case.
    function _edgeLabelDX(e, rev) {
        var f = ((e.segment || 0) === 0) ? root._ndx(e.from, rev) : 0
        var t = ((e.segment || 0) === Math.max(0, (e.segments || 1) - 1))
                    ? root._ndx(e.to, rev) : 0
        return (f + t) / 2
    }
    function _edgeLabelDY(e, rev) {
        var f = ((e.segment || 0) === 0) ? root._ndy(e.from, rev) : 0
        var t = ((e.segment || 0) === Math.max(0, (e.segments || 1) - 1))
                    ? root._ndy(e.to, rev) : 0
        return (f + t) / 2
    }

    function _clearNudges() {
        if (Object.keys(root._nudges).length === 0) return
        root._nudges = ({})
        root._nudgeRev++
    }
    // The three things §2.1 says drop a nudge, stated where they happen rather than left to the
    // reader to infer. None of them needs a warning, because nothing was ever at stake.
    //
    // Opening or closing a box drops them for the scope change's reason, and it is the same event
    // under a different name: a rank appears or goes, every column is re-ordered against its
    // neighbour, and an offset chosen against the old arrangement would hold a box away from a
    // position it no longer has.
    onFocusIdChanged:       root._clearNudges()
    onScopeChanged:         root._clearNudges()
    onExpandToggled:        root._clearNudges()
    onCollapseAllRequested: root._clearNudges()

    // Colour by TYPE, from the theme's own chart palette — so the graph reads as a legend of the
    // rail beside it, and no colour is invented here.
    function _typeColor(t) {
        switch (t) {
        case "characteristics": return Theme.chartSeriesColor(0)
        case "causes":          return Theme.chartSeriesColor(1)
        case "measures":        return Theme.chartSeriesColor(2)
        case "signals":         return Theme.chartSeriesColor(3)
        case "screens":         return Theme.chartSeriesColor(4)
        case "drills":          return Theme.chartSeriesColor(5)
        case "references":      return Theme.chartSeriesColor(6)
        case "corridors":       return Theme.chartSeriesColor(7)
        case "health":          return Theme.colorError
        }
        return Theme.colorBorderStrong
    }

    // ── Hit testing, in LAYOUT coordinates ────────────────────────────────────
    function _nodeAt(x, y) {
        for (var i = 0; i < _nodes.length; i++) {
            var n = _nodes[i]
            var nx = n.x + root._ndx(n.id, root._nudgeRev)
            var ny = n.y + root._ndy(n.id, root._nudgeRev)
            if (x >= nx && x <= nx + n.w && y >= ny && y <= ny + n.h) return n
        }
        return null
    }

    // A curve is unhittable at one pixel, so a link is picked up at its label point — which is the
    // part of the line a reader is already looking at.
    //
    // Nudged by the same amount the label is DRAWN by, from the same function. These are one
    // target: a label that moves with its line but is still picked up where the layout first put it
    // would be a link you select by clicking empty canvas, and cannot select by clicking the word.
    function _edgeAt(x, y) {
        for (var i = 0; i < _edges.length; i++) {
            var e = _edges[i]
            if (e.rowId === undefined) continue
            var lx = (e.labelX || 0) + root._edgeLabelDX(e, root._nudgeRev)
            var ly = (e.labelY || 0) + root._edgeLabelDY(e, root._nudgeRev)
            if (Math.abs(x - lx) <= Theme.sp(16) && Math.abs(y - ly) <= Theme.sp(9)) return e
        }
        return null
    }

    // ── Opening a box ─────────────────────────────────────────────────────────
    //
    // The hidden-neighbour count is drawn on the side of the node the layout would open towards,
    // and on that side it is a CONTROL rather than a caption. A number with no way to act on it is
    // a reader told what they cannot have.
    //
    // The AIMED way past the bound, next to `expand`, which is the broad one. They answer different
    // questions and neither replaces the other: `expand` is "show me more of everything", and this
    // is "show me what is behind THIS box" — which `expand` cannot do on its own, because the
    // per-rank cap spends the new slots on that rank's other parents before it reaches the twelve
    // you were pointing at.
    //
    // Hit-tested here, in layout coordinates, for the same reason every other press on this canvas
    // is: `input` accepts them all, so a PpPressable inside a node delegate would never see one.
    // The rects below and the badges in the node delegate are ONE target and must be read from the
    // same numbers — a control you can see but not click is worse than no control.
    readonly property real _togW: Theme.sp(20)
    readonly property real _togH: Theme.sp(15)

    // Which side a node opens towards: a cause opens further left, an effect further right, and
    // rank 0 — the focus and its partners — opens both ways, so it gets a control on each side.
    function _opensLeft(n)  { return (n.rank || 0) <= 0 }
    function _opensRight(n) { return (n.rank || 0) >= 0 }

    // "" when the point is on no control, else "<id>|L" or "<id>|R". A string rather than the node,
    // so a press and its release can be compared for the SAME control without relying on the
    // identity of an object the layout may have rebuilt in between.
    function _toggleAt(x, y) {
        for (var i = 0; i < _nodes.length; i++) {
            var n = _nodes[i]
            var nx = n.x + root._ndx(n.id, root._nudgeRev)
            var ny = n.y + root._ndy(n.id, root._nudgeRev)
            // Centred on the CONDITION's own row rather than on the box, which is taller than one
            // row whenever the box carries measures.
            var ty = ny + root._condRowH(n) / 2 - root._togH / 2
            if (y < ty || y > ty + root._togH) continue
            if (root._opensLeft(n) && (n.expanded === true || (n.hiddenCauses || 0) > 0)) {
                var lx = nx - Theme.sp(3) - root._togW
                if (x >= lx && x <= lx + root._togW) return n.id + "|L"
            }
            if (root._opensRight(n) && (n.expanded === true || (n.hiddenEffects || 0) > 0)) {
                var rx = nx + n.w + Theme.sp(3)
                if (x >= rx && x <= rx + root._togW) return n.id + "|R"
            }
        }
        return ""
    }

    // How tall the condition's OWN row is, whatever else the box carries. The delegate anchors the
    // name to this and the toggle centres on it; deriving it twice is how the two drift apart.
    //
    // Measured by SUBTRACTING what the inner rows took, rather than by reading nodeH back out of the
    // options — the box grew by exactly that much and this is the same arithmetic in reverse.
    function _condRowH(n) {
        var h = n.h
        var ms = n.measures || []
        for (var i = 0; i < ms.length; i++)   h -= (ms[i].h || 0)
        var rs = n.references || []
        for (var k = 0; k < rs.length; k++)   h -= (rs[k].h || 0)
        return h
    }

    // Which measure row is under this point, as "<conditionId>|<measureId>", or "". The rows carry
    // ABSOLUTE y from the layout, so this adds only the node's nudge — the same offset the delegate
    // applies to the box the rows are drawn in.
    function _measureAt(x, y) {
        for (var i = 0; i < _nodes.length; i++) {
            var n = _nodes[i]
            var ms = n.measures || []
            if (ms.length === 0) continue
            var nx = n.x + root._ndx(n.id, root._nudgeRev)
            if (x < nx || x > nx + n.w) continue
            var dy = root._ndy(n.id, root._nudgeRev)
            for (var k = 0; k < ms.length; k++) {
                var top = ms[k].y + dy
                if (y >= top && y <= top + ms[k].h) return n.id + "|" + ms[k].id
            }
        }
        return ""
    }
    property string _hotMeasure: ""

    // The same, for the citation row: "<conditionId>|<referenceId>", or "".
    //
    // A row whose citation resolved to NOTHING is skipped — it has no reference id, so there is no
    // page for a press to open, and a hover highlight on it would offer a click that does nothing.
    // The row is still drawn; it is simply not a control.
    function _referenceAt(x, y) {
        for (var i = 0; i < _nodes.length; i++) {
            var n = _nodes[i]
            var rs = n.references || []
            if (rs.length === 0) continue
            var nx = n.x + root._ndx(n.id, root._nudgeRev)
            if (x < nx || x > nx + n.w) continue
            var dy = root._ndy(n.id, root._nudgeRev)
            for (var k = 0; k < rs.length; k++) {
                if (!rs[k].id) continue
                var top = rs[k].y + dy
                if (y >= top && y <= top + rs[k].h) return n.id + "|" + rs[k].id
            }
        }
        return ""
    }
    property string _hotReference: ""

    // The control under the pointer, so it can look like one before it is pressed. Static text that
    // turns out to be clickable is a control nobody finds.
    property string _hotToggle: ""

    function _anyOpen() {
        for (var i = 0; i < _nodes.length; i++) if (_nodes[i].expanded === true) return true
        return false
    }

    function _nodeById(id) {
        for (var i = 0; i < _nodes.length; i++) if (_nodes[i].id === id) return _nodes[i]
        return null
    }
    function _edgeById(rowId) {
        for (var i = 0; i < _edges.length; i++) if (_edges[i].rowId === rowId) return _edges[i]
        return null
    }

    // Every node that could be an end of a causal link — the refusal set is computed over exactly
    // these, once, when a drag arms.
    function _conditionIds() {
        var out = []
        for (var i = 0; i < _nodes.length; i++)
            out.push(_nodes[i].id)
        return out
    }

    // ── The armed drag ────────────────────────────────────────────────────────
    //
    // `_armEnd` is which end of the proposed link the FIXED node occupies: "to" for `Add cause of
    // this…`, and either for a re-point, depending on which end is being moved.
    property string _armKind:   ""     // "" | "link" | "repoint"
    property string _armFixed:  ""     // the condition that is not moving
    property string _armEnd:    "to"
    property string _armEdgeId: ""     // the link being re-pointed
    property real   _dragX:     0
    property real   _dragY:     0
    property string _dragTarget: ""
    // A plain JS object for the life of the drag, cleared on release. Never re-queried per
    // positionChanged — that is the shape this replaces.
    property var    _refusals:  ({})

    readonly property bool _dragging: _armKind !== ""

    function _refusalOf(id) {
        var r = root._refusals ? root._refusals[id] : null
        return r ? r : null
    }

    property bool _firstHoldOfSession: true

    Flickable {
        id: canvas
        anchors.fill: parent
        contentWidth:  content.width
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // A drag on this canvas is a marquee, a nudge or a link — never a pan. The wheel and the
        // scrollbars are how it is panned, which is what leaves the drag free to mean something.
        interactive: false

        ScrollBar.vertical:   ScrollBar { policy: ScrollBar.AsNeeded; interactive: true }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded; interactive: true }

        // An explicit, SIZED item to hang everything on.
        //
        // Items declared inside a Flickable are reparented to its contentItem, and contentItem has
        // no geometry of its own — contentWidth/contentHeight describe the scrollable AREA, not that
        // item. So `anchors.fill: parent` in here resolves to 0x0, which silently collapsed every
        // Shape: the edges and the drag rubber-band were being drawn into nothing while the nodes,
        // which carry explicit x/y, kept rendering. A graph of boxes with no lines.
        // Ctrl+wheel zooms; a bare wheel still scrolls, which is what a wheel does everywhere else.
        WheelHandler {
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => {
                root._userZoom = Math.max(0.2, Math.min(4.0,
                                          root._userZoom * (event.angleDelta.y > 0 ? 1.12 : 1 / 1.12)))
                event.accepted = true
            }
        }
        WheelHandler {
            acceptedModifiers: Qt.NoModifier
            onWheel: (event) => {
                canvas.contentY = Math.max(0, Math.min(canvas.contentHeight - canvas.height,
                                                       canvas.contentY - event.angleDelta.y))
                canvas.contentX = Math.max(0, Math.min(canvas.contentWidth - canvas.width,
                                                       canvas.contentX - event.angleDelta.x))
                event.accepted = true
            }
        }

        // Sized to the SCALED graph, but never smaller than the pane — which is what centres a
        // small picture instead of pinning it to the top-left corner.
        Item {
            id: content
            width:  Math.max(canvas.width,  (root.layoutData.width  || 0) * root._zoom)
            height: Math.max(canvas.height, (root.layoutData.height || 0) * root._zoom)

            // The layout's own coordinate space, scaled as a whole. Everything inside keeps the
            // numbers dag_layout produced — no delegate multiplies anything by a zoom.
            Item {
                id: inner
                x: (content.width  - (root.layoutData.width  || 0) * root._zoom) / 2
                y: (content.height - (root.layoutData.height || 0) * root._zoom) / 2
                width:  root.layoutData.width  || 0
                height: root.layoutData.height || 0
                transformOrigin: Item.TopLeft
                scale: root._zoom

        // ── Rank headings ─────────────────────────────────────────────────────
        Repeater {
            model: root.layoutData.headings || []
            delegate: Text {
                required property var modelData
                x: modelData.x
                y: modelData.y
                width: modelData.w
                text:                modelData.label
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
                horizontalAlignment: Text.AlignHCenter
            }
        }

        // ── Edges ─────────────────────────────────────────────────────────────
        Repeater {
            model: root._edges
            delegate: Shape {
                id: edgeShape
                required property var modelData

                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                z: selected ? 3 : 1

                readonly property bool selected:
                    modelData.rowId !== undefined
                    && (modelData.rowId === root.selectedEdgeId
                        || root.selectedEdgeIds.indexOf(modelData.rowId) >= 0)
                // The selected edge must be identifiable ON THE CANVAS: drawn heavier, with the
                // rest muted. A selection that only shows in the inspector is a selection the
                // reader has to hold in their head.
                readonly property bool anySelected:
                    root.selectedEdgeId !== "" || root.selectedEdgeIds.length > 0
                readonly property real muting: anySelected && !selected ? 0.28 : 1.0

                // A nudged node drags its lines with it. Only the segment that actually touches the
                // node moves — a long edge routed through waypoints keeps every joint where the
                // layout put it, so the routing is never quietly re-invented here.
                readonly property bool headSeg: (modelData.segment || 0) === 0
                readonly property bool tailSeg:
                    (modelData.segment || 0) === Math.max(0, (modelData.segments || 1) - 1)
                readonly property real fdx: headSeg ? root._ndx(modelData.from, root._nudgeRev) : 0
                readonly property real fdy: headSeg ? root._ndy(modelData.from, root._nudgeRev) : 0
                readonly property real tdx: tailSeg ? root._ndx(modelData.to,   root._nudgeRev) : 0
                readonly property real tdy: tailSeg ? root._ndy(modelData.to,   root._nudgeRev) : 0

                ShapePath {
                    strokeColor: edgeShape.selected ? Theme.colorAccent : Theme.colorBorderStrong
                    strokeWidth: edgeShape.selected ? 2 : Math.max(1, edgeShape.modelData.weight || 1)
                    fillColor:   "transparent"
                    strokeStyle: ShapePath.SolidLine
                    capStyle:    ShapePath.RoundCap

                    startX: edgeShape.modelData.x1 + edgeShape.fdx
                    startY: edgeShape.modelData.y1 + edgeShape.fdy
                    PathCubic {
                        x:         edgeShape.modelData.x2  + edgeShape.tdx
                        y:         edgeShape.modelData.y2  + edgeShape.tdy
                        control1X: edgeShape.modelData.c1x + edgeShape.fdx
                        control1Y: edgeShape.modelData.c1y + edgeShape.fdy
                        control2X: edgeShape.modelData.c2x + edgeShape.tdx
                        control2Y: edgeShape.modelData.c2y + edgeShape.tdy
                    }
                }

                // The arrow head, its three points supplied by the layout — a head computed here
                // would point somewhere slightly different from the curve it caps.
                ShapePath {
                    fillColor:   edgeShape.selected ? Theme.colorAccent : Theme.colorBorderStrong
                    strokeWidth: 0
                    strokeColor: "transparent"
                    // Symmetric relations carry no direction, so they carry no head.
                    readonly property bool shown:
                        edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                    startX: shown ? edgeShape.modelData.tipAx + edgeShape.tdx : 0
                    startY: shown ? edgeShape.modelData.tipAy + edgeShape.tdy : 0
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipBx + edgeShape.tdx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipBy + edgeShape.tdy : 0
                    }
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipCx + edgeShape.tdx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipCy + edgeShape.tdy : 0
                    }
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipAx + edgeShape.tdx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipAy + edgeShape.tdy : 0
                    }
                }

                opacity: muting
                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
            }
        }

        // Edge labels. Drawn only — the press that picks one up is hit-tested by the input layer,
        // because the same press may be about to become a hold, and a handler here would take it.
        Repeater {
            model: root._edges
            delegate: Item {
                id: edgeLabel
                required property var modelData

                readonly property bool selected:
                    modelData.rowId === root.selectedEdgeId
                    || root.selectedEdgeIds.indexOf(modelData.rowId) >= 0

                visible: modelData.rowId !== undefined
                // Follows the line it names. `_nudgeRev` is passed as an ARGUMENT rather than read
                // as a bare statement — a mention would be compiled away and this would subscribe
                // to nothing, which is exactly the failure qml_reactivity_test exists to catch.
                x: (modelData.labelX || 0) + root._edgeLabelDX(modelData, root._nudgeRev)
                   - Theme.sp(16)
                y: (modelData.labelY || 0) + root._edgeLabelDY(modelData, root._nudgeRev)
                   - Theme.sp(9)
                width:  Theme.sp(32)
                height: Theme.sp(18)
                z: 4

                Rectangle {
                    anchors.fill: parent
                    radius:  Theme.radius
                    color:   Theme.colorBg
                    visible: edgeLabel.modelData.label !== "" || edgeLabel.selected
                    opacity: 0.9
                }

                Text {
                    anchors.centerIn: parent
                    text: edgeLabel.modelData.label || ""
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: edgeLabel.selected ? Theme.colorAccent : Theme.colorText3
                }
            }
        }

        // ── Nodes ─────────────────────────────────────────────────────────────
        Repeater {
            model: root._nodes
            delegate: Item {
                id: nodeItem
                required property var modelData

                x: modelData.x + root._ndx(modelData.id, root._nudgeRev)
                y: modelData.y + root._ndy(modelData.id, root._nudgeRev)
                width:  modelData.w
                height: modelData.h
                z: 2

                readonly property bool isFocus:   modelData.kind === "focus"
                readonly property bool inSelection: root.selectedNodeIds.indexOf(modelData.id) >= 0
                // Asked of the LAYOUT, not of the view's own list of opened ids: a box the reader
                // opened and then navigated away from is not open on a graph it is no longer in.
                readonly property bool opened: modelData.expanded === true
                // Read in two places — the frame draws it and the measure rows inset by it. The
                // last row reaches the bottom of the box, so a fill that ignored this painted over
                // the frame and the box lost its bottom edge under the pointer.
                readonly property int borderW: hotTarget ? 2 : isFocus ? 2 : 1

                readonly property var  refusal: root._dragging ? root._refusalOf(modelData.id) : null
                readonly property bool refused: refusal !== null
                readonly property bool hotTarget:
                    root._dragging && root._dragTarget === modelData.id && !refused

                readonly property color tint: root._typeColor(nodeItem.modelData.nodeType)

                // Refused nodes drop back and say why. Legality is settled INSIDE the gesture and
                // never after it: the validation strip is for problems that already exist in the
                // pack, and it must never be how an author discovers the UI let them make one.
                opacity: refused ? 0.35 : 1.0
                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    // Tinted by type, faintly — enough to tell a measure from a characteristic at a
                    // glance without the colour becoming the subject. Latent conditions stay an
                    // outline, so a reader can still tell what the app SAW from what it worked out.
                    color: nodeItem.hotTarget
                               ? Theme.colorAccentLight
                           : nodeItem.modelData.latent
                               ? "transparent"
                               : Qt.rgba(nodeItem.tint.r, nodeItem.tint.g, nodeItem.tint.b,
                                         nodeItem.isFocus ? 0.22 : 0.10)
                    border.width: nodeItem.borderW
                    border.color: nodeItem.hotTarget              ? Theme.colorAccent
                                : nodeItem.inSelection            ? Theme.colorAccent
                                : nodeItem.isFocus                ? nodeItem.tint
                                : nodeItem.modelData.dirty        ? Theme.colorAccent
                                : nodeItem.modelData.offeredOnly  ? Theme.colorBorderMid
                                                                  : nodeItem.tint
                    // An offered-only condition may never be concluded, so it has to look different
                    // everywhere it appears, including here.
                    opacity: nodeItem.modelData.available ? 1.0 : 0.55
                }

                // The 3px accent ring on the node a legal drop is hovering. Outside the box rather
                // than on its border, so it reads as an invitation rather than as a state the node
                // has acquired.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    visible: nodeItem.hotTarget
                    radius:  Theme.radius + 2
                    color:   "transparent"
                    border.width: 3
                    border.color: Theme.colorAccent
                    opacity: 0.5
                    z: -1
                }

                // The condition's own row, at the TOP of the box rather than filling it. With
                // measures on the box is taller than one row and the rest of it belongs to them;
                // anchors.fill would centre the name over the whole stack.
                RowLayout {
                    anchors.left:   parent.left
                    anchors.right:  parent.right
                    anchors.top:    parent.top
                    height:         root._condRowH(nodeItem.modelData)
                    anchors.leftMargin:  Theme.sp(7)
                    anchors.rightMargin: Theme.sp(7)
                    spacing: Theme.sp(6)

                    // What kind of thing this is, as a glyph. Colour alone cannot carry it — a
                    // reader with any colour deficiency gets the same information from the shape.
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text:           nodeItem.modelData.glyph || ""
                        visible:        text.length > 0
                        font.family:    Theme.fontSymbol
                        font.pixelSize: Theme.fontSzBody2
                        color:          nodeItem.tint
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Text {
                            Layout.fillWidth: true
                            text: nodeItem.modelData.label
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color: nodeItem.modelData.available ? Theme.colorText : Theme.colorText3
                            elide: Text.ElideRight
                        }

                        // The one line the node can say about itself — a measure's corridor, a
                        // relation's kind. During a drag it is given over to the answer the author
                        // needs at that instant instead: why this one is refused, or that it is the
                        // one about to be taken.
                        Text {
                            Layout.fillWidth: true
                            text: nodeItem.refused   ? nodeItem.refusal.text
                                : nodeItem.hotTarget ? root._dropHint()
                                                     : (nodeItem.modelData.note || "")
                            visible: text.length > 0
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color: nodeItem.hotTarget ? Theme.colorAccent
                                 : !nodeItem.refused  ? Theme.colorText3
                                 : nodeItem.refusal.reason === "cycle" ? Theme.colorWarn
                                                                       : Theme.colorText3
                            elide: Text.ElideRight
                        }
                    }

                    // Nearly everybody has this one. `∀` is the universal quantifier and says only
                    // "all" — a star, a flame or a warning would each have told the reader whether
                    // to be pleased about a base rate, and prominence is not a verdict (see
                    // characteristic.h: it is a PRIOR, and an editorial one). Anchored to the TOP of
                    // the condition row so it stays in the box's corner however many measure rows
                    // grew underneath, and last in the row so the label elides into it rather than
                    // under it. The word itself is in the hub's meta line when the node is held.
                    Text {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Theme.sp(3)
                        visible:        nodeItem.modelData.ubiquitous === true
                        text:           "∀"
                        font.family:    Theme.fontSymbol
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }

                // ── The measures, inside the box ──────────────────────────────
                //
                // Drawn from the layout's own y and h, mapped into the node by subtracting the
                // node's origin. The pane hit-tests the same numbers, so a row that is visible is a
                // row that opens its measure — see root._measureAt.
                //
                // A measure nothing can produce is drawn as the gap it is rather than left to look
                // live. That is the whole reason for showing detectors at all: a condition the app
                // cannot currently see is the thing this view is best placed to make obvious.
                Repeater {
                    model: nodeItem.modelData.measures || []
                    delegate: Item {
                        id: mrow
                        required property var modelData
                        x: 0
                        y: mrow.modelData.y - nodeItem.modelData.y
                        width:  nodeItem.width
                        height: mrow.modelData.h

                        required property int index
                        // Last in the BOX, not last in its own stack. With references on, a
                        // citation row sits below these, and a measure that rounded its corners
                        // there would cut a notch out of the middle of the frame.
                        readonly property bool last:
                            mrow.index === (nodeItem.modelData.measures || []).length - 1
                            && (nodeItem.modelData.references || []).length === 0
                        readonly property bool hot:
                            root._hotMeasure === nodeItem.modelData.id + "|" + mrow.modelData.id

                        // Inset by the frame on every side it touches, and rounded at the bottom on
                        // the LAST row — the box has a radius, and a square fill in a rounded corner
                        // shows outside it.
                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin:   nodeItem.borderW
                            anchors.rightMargin:  nodeItem.borderW
                            anchors.bottomMargin: mrow.last ? nodeItem.borderW : 0
                            bottomLeftRadius:  mrow.last ? Theme.radius : 0
                            bottomRightRadius: mrow.last ? Theme.radius : 0
                            color: mrow.hot ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        // A hairline above each row, so the stack reads as rows of one box rather
                        // than as text that happened to be placed under a name.
                        Rectangle {
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            anchors.leftMargin:  Theme.sp(6)
                            anchors.rightMargin: Theme.sp(6)
                            height: 1
                            color: Theme.colorBorderMid
                            opacity: Theme.borderOpacityNormal
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(9)
                            anchors.rightMargin: Theme.sp(7)
                            spacing: Theme.sp(5)

                            Text {
                                Layout.fillWidth: true
                                text: mrow.modelData.label
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color: mrow.modelData.available ? (mrow.hot ? Theme.colorText
                                                                            : Theme.colorText2)
                                                                : Theme.colorText3
                                elide: Text.ElideRight
                            }
                            Text {
                                text: mrow.modelData.available ? "" : mrow.modelData.statusLabel
                                visible: text.length > 0
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }
                    }
                }

                // ── The citation, under the measures ─────────────────────────
                //
                // Same geometry contract as the measure rows above and drawn the same way, because
                // it is the same kind of thing: a fact about the condition that is not a cause, and
                // that had nowhere to appear on this picture at all.
                //
                // A citation the bibliography has never heard of is drawn as the gap it is and is
                // NOT a control — see root._referenceAt. It stays visible because a dangling
                // citation and no citation must not look the same.
                Repeater {
                    model: nodeItem.modelData.references || []
                    delegate: Item {
                        id: rrow
                        required property var modelData
                        x: 0
                        y: rrow.modelData.y - nodeItem.modelData.y
                        width:  nodeItem.width
                        height: rrow.modelData.h

                        required property int index
                        // The citations are always the LAST rows in the box — the layout stacks them
                        // under the measures — so this one rounds its corners into the frame whether
                        // or not anything sits above it.
                        readonly property bool last:
                            rrow.index === (nodeItem.modelData.references || []).length - 1
                        readonly property bool hot:
                            rrow.modelData.id
                            && root._hotReference === nodeItem.modelData.id + "|" + rrow.modelData.id

                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin:   nodeItem.borderW
                            anchors.rightMargin:  nodeItem.borderW
                            anchors.bottomMargin: rrow.last ? nodeItem.borderW : 0
                            bottomLeftRadius:  rrow.last ? Theme.radius : 0
                            bottomRightRadius: rrow.last ? Theme.radius : 0
                            color: rrow.hot ? Theme.colorBg2 : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        }

                        Rectangle {
                            anchors { left: parent.left; right: parent.right; top: parent.top }
                            anchors.leftMargin:  Theme.sp(6)
                            anchors.rightMargin: Theme.sp(6)
                            height: 1
                            color: Theme.colorBorderMid
                            opacity: Theme.borderOpacityNormal
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin:  Theme.sp(9)
                            anchors.rightMargin: Theme.sp(7)
                            spacing: Theme.sp(5)

                            // The type glyph the rest of the app gives a reference. A row that is a
                            // paper rather than a measure has to say so without being read — the
                            // two stacks are otherwise the same size, in the same box, in the same
                            // small type.
                            Text {
                                Layout.alignment: Qt.AlignVCenter
                                text:           "§"
                                font.family:    Theme.fontSymbol
                                font.pixelSize: Theme.fontSzMicro
                                color:          rrow.modelData.resolved ? Theme.colorText3
                                                                        : Theme.colorWarn
                            }
                            Text {
                                Layout.fillWidth: true
                                text: rrow.modelData.label
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color: !rrow.modelData.resolved ? Theme.colorText3
                                     : rrow.hot                 ? Theme.colorText
                                                                : Theme.colorText2
                                elide: Text.ElideRight
                            }
                            Text {
                                text:    rrow.modelData.detailLabel || ""
                                visible: text.length > 0
                                font.family:    Theme.fontData
                                font.pixelSize: Theme.fontSzMicro
                                color: rrow.modelData.resolved ? Theme.colorText3 : Theme.colorWarn
                            }
                        }
                    }
                }

                // Whatever the depth bound cut off is COUNTED on the node it was cut from. A graph
                // that silently omitted half of what it knows is worse than one that drew nothing.
                //
                // On the side the node OPENS towards it is also the way past the bound: press it and
                // that box admits its own neighbours, however far the picture reaches on its own.
                // The sign is the verb and the number is what is behind it — `+3` is three not
                // drawn, `−` is opened with nothing left, `−2` is opened with two still past the
                // per-node budget. On the other side, where opening would fold the graph back on
                // itself, it stays a plain count.
                //
                // Geometry is read from root._togW / _togH and the same sp(3) margin the hit test
                // uses. These are one target; they must not be able to disagree about where it is.
                component Opener: Item {
                    id: op
                    required property bool live      // this side is the one the node opens towards
                    required property int  count
                    width:  root._togW
                    height: root._togH
                    anchors.verticalCenter: parent.verticalCenter
                    visible: op.live ? (nodeItem.opened || op.count > 0) : op.count > 0

                    property bool hot: false

                    Rectangle {
                        anchors.fill: parent
                        radius:  height / 2
                        visible: op.live
                        color:   op.hot ? Theme.colorAccentLight : "transparent"
                        border.width: 1
                        border.color: op.hot ? Theme.colorAccent : Theme.colorBorderMid
                        opacity: op.hot ? 1.0 : 0.7
                    }
                    Text {
                        anchors.centerIn: parent
                        text: (!op.live || !nodeItem.opened) ? "+" + op.count
                            : op.count > 0                   ? "−" + op.count
                                                             : "−"
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          op.hot ? Theme.colorAccent : Theme.colorText3
                    }
                }

                Opener {
                    anchors.right: parent.left
                    anchors.rightMargin: Theme.sp(3)
                    live:  root._opensLeft(nodeItem.modelData)
                    count: nodeItem.modelData.hiddenCauses || 0
                    hot:   root._opensLeft(nodeItem.modelData)
                           && root._hotToggle === nodeItem.modelData.id + "|L"
                }
                Opener {
                    anchors.left: parent.right
                    anchors.leftMargin: Theme.sp(3)
                    live:  root._opensRight(nodeItem.modelData)
                    count: nodeItem.modelData.hiddenEffects || 0
                    hot:   root._opensRight(nodeItem.modelData)
                           && root._hotToggle === nodeItem.modelData.id + "|R"
                }
            }
        }

        // ── The armed drag's own line ─────────────────────────────────────────
        //
        // The SAME curve the drawn edges use — control points half way along in x, exactly as
        // dag_layout computes them — so the thing being made reads as the same kind of object as
        // the things already there. Dashed, because it is not one yet.
        Shape {
            id: dragShape
            visible: root._dragging
            anchors.fill: parent
            z: 7
            preferredRendererType: Shape.CurveRenderer

            // A ShapePath is not an Item and has no `parent` to read these off, so they are held
            // here and addressed by id.
            readonly property real ax: root._armAnchor().x
            readonly property real ay: root._armAnchor().y
            // The line is always drawn cause → effect, whichever end the hand is holding, because
            // that is the direction the claim will read in once it exists.
            readonly property bool outward: root._armEnd === "from"
            readonly property real x1: outward ? ax : root._dragX
            readonly property real y1: outward ? ay : root._dragY
            readonly property real x2: outward ? root._dragX : ax
            readonly property real y2: outward ? root._dragY : ay

            ShapePath {
                strokeColor: Theme.colorAccent
                strokeWidth: 2
                fillColor:   "transparent"
                strokeStyle: ShapePath.DashLine
                dashPattern: [ 4, 3 ]
                capStyle:    ShapePath.RoundCap

                startX: dragShape.x1
                startY: dragShape.y1
                PathCubic {
                    x:         dragShape.x2
                    y:         dragShape.y2
                    control1X: dragShape.x1 + (dragShape.x2 - dragShape.x1) * 0.5
                    control1Y: dragShape.y1
                    control2X: dragShape.x2 - (dragShape.x2 - dragShape.x1) * 0.5
                    control2Y: dragShape.y2
                }
            }

            // The arrowhead is drawn AT THE HELD NODE from the first frame. It is the third of the
            // three places the direction is stated — the spoke's wording and the target's own line
            // being the other two — and it is the one that is true before anything is hovered.
            ShapePath {
                fillColor:   Theme.colorAccent
                strokeWidth: 0
                strokeColor: "transparent"

                startX: dragShape.x2
                startY: dragShape.y2
                PathLine {
                    x: dragShape.x2 - Theme.sp(7)
                    y: dragShape.y2 - Theme.sp(7) * 0.55
                }
                PathLine {
                    x: dragShape.x2 - Theme.sp(7)
                    y: dragShape.y2 + Theme.sp(7) * 0.55
                }
                PathLine { x: dragShape.x2; y: dragShape.y2 }
            }
        }

        // The ghost of a node that does not exist yet, at the point the drag was released over
        // empty canvas. It is dashed because it is a proposal, and it stays put while the popover
        // under it asks the only two questions that cannot be answered later.
        Rectangle {
            visible: cause.visible && cause.hasGhost
            x: cause.ghostX
            y: cause.ghostY
            width:  Theme.sp(160)
            height: Theme.sp(34)
            radius: Theme.radius
            color:  "transparent"
            border.width: 1
            border.color: Theme.colorAccent
            z: 8
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius
                color:  Theme.colorAccentLight
                opacity: 0.35
            }
        }

        // ── The marquee ───────────────────────────────────────────────────────
        Rectangle {
            visible: input.mode === "marquee"
            x: Math.min(input.pressLX, input.moveLX)
            y: Math.min(input.pressLY, input.moveLY)
            width:  Math.abs(input.moveLX - input.pressLX)
            height: Math.abs(input.moveLY - input.pressLY)
            color:  Theme.colorAccentLight
            opacity: 0.3
            border.width: 1
            border.color: Theme.colorAccent
            z: 9
        }
        }

        // ── The one input layer ───────────────────────────────────────────────
        //
        // Everything a press can mean is decided here, from the press point and how the hand moved
        // after it. Nothing else on this canvas accepts a press.
        //
        // A child of `content` rather than of the Flickable, and that is load-bearing: an item
        // declared straight inside a Flickable is reparented to its contentItem, which has no
        // geometry of its own, so `anchors.fill: parent` there resolves to 0×0 and this would
        // silently accept nothing. `content` is the explicitly sized item, and it also leaves the
        // scroll bars — which live outside it — clickable.
        MouseArea {
            id: input
            anchors.fill: parent
            z: 60
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: root._dragging             ? Qt.CrossCursor
                       : root._hotToggle !== ""     ? Qt.PointingHandCursor
                       : root._hotMeasure !== ""    ? Qt.PointingHandCursor
                       : root._hotReference !== ""  ? Qt.PointingHandCursor
                                                    : Qt.ArrowCursor

            // "idle" | "pending" | "nudge" | "marquee" | "ring" | "armed" | "toggle" | "measure"
            //        | "reference"
            property string mode: "idle"
            property real   pressLX: 0     // press point, in LAYOUT coordinates
            property real   pressLY: 0
            property real   moveLX:  0
            property real   moveLY:  0
            property var    pressedNode: null
            property var    pressedEdge: null
            property string pressedToggle: ""
            property string pressedMeasure: ""
            property string pressedReference: ""
            property var    nudgeBase:   ({})

            function toLayout(mx, my) { return inner.mapFromItem(input, mx, my) }

            onPressed: (mouse) => {
                var p = toLayout(mouse.x, mouse.y)

                // A drag armed from a LATCHED ring — or from the inspector — has no button held, so
                // the gesture ends with a click rather than a release. That click arrives here
                // first, and it is the drop; letting it fall through would re-enter hit-testing and
                // throw the armed drag away on the way to committing it.
                if (root._dragging) { root._dropDrag(p.x, p.y); mode = "idle"; return }

                pressLX = p.x; pressLY = p.y
                moveLX  = p.x; moveLY  = p.y

                // The open/close control sits OUTSIDE its node's box, so it is tested before the
                // node and before anything else this press could mean. It is neither a selection
                // nor a hold target: opening a box says nothing about what the author is acting on,
                // and letting the hold timer run would put a menu about the node over the control
                // that was actually pressed.
                pressedToggle = mouse.button === Qt.LeftButton ? root._toggleAt(p.x, p.y) : ""
                if (pressedToggle !== "") { mode = "toggle"; return }

                // Before the node, because a measure row is INSIDE its box: hit-testing the node
                // first would always win and no row would ever be reachable. Like the toggle it is
                // neither a selection nor a hold target — opening a measure is a navigation.
                pressedMeasure = mouse.button === Qt.LeftButton ? root._measureAt(p.x, p.y) : ""
                if (pressedMeasure !== "") { mode = "measure"; return }

                // And the citation row for the same reason, and on the same terms.
                pressedReference = mouse.button === Qt.LeftButton ? root._referenceAt(p.x, p.y) : ""
                if (pressedReference !== "") { mode = "reference"; return }

                pressedNode = root._nodeAt(p.x, p.y)
                pressedEdge = pressedNode ? null : root._edgeAt(p.x, p.y)

                // The scope of the ring is whatever is SELECTED at the press; pressing an unselected
                // node selects it first, so the menu is never about something the author cannot see
                // they had chosen.
                root._adoptPressSelection(pressedNode, pressedEdge)

                if (mouse.button === Qt.RightButton) {
                    // Every desktop author tries this, so it must not open a different menu. Latched
                    // — nothing is held — and a click commits.
                    mode = "idle"
                    root._openRing(mouse.x, mouse.y, true)
                    return
                }

                mode = "pending"
                holdTimer.restart()
            }

            onPositionChanged: (mouse) => {
                var p = toLayout(mouse.x, mouse.y)
                moveLX = p.x; moveLY = p.y

                // Only while nothing else is in flight — a control lighting up under a marquee or a
                // link drag would offer something this gesture cannot do.
                var quiet = (mode === "idle" || mode === "pending" || mode === "toggle"
                             || mode === "measure" || mode === "reference") && !root._dragging
                root._hotToggle  = quiet ? root._toggleAt(p.x, p.y) : ""
                root._hotMeasure = quiet && root._hotToggle === "" ? root._measureAt(p.x, p.y) : ""
                // Tested after the measures, though the two stacks cannot overlap — the layout gives
                // each row its own band of the box. Ordered anyway so that if they ever could, one
                // of them wins rather than both lighting up.
                root._hotReference = quiet && root._hotToggle === "" && root._hotMeasure === ""
                                         ? root._referenceAt(p.x, p.y) : ""

                if (mode === "measure" || mode === "reference") return

                // A press on a control that then travels off it is abandoned, exactly as a button
                // is. It never becomes a nudge: the control is not the node, and dragging it would
                // move a box the author was only trying to open.
                if (mode === "toggle") return

                if (mode === "ring") {
                    var q = root.mapFromItem(input, mouse.x, mouse.y)
                    ring.track(q.x, q.y)
                    return
                }
                if (mode === "armed" || root._dragging) { root._trackDrag(p.x, p.y); return }

                if (mode === "pending") {
                    // Movement before the hold matures is a nudge or a marquee, not a hold. The
                    // timer is cancelled rather than allowed to fire under the moving hand.
                    var far = Math.abs(p.x - pressLX) > Theme.sp(6)
                           || Math.abs(p.y - pressLY) > Theme.sp(6)
                    if (!far) return
                    holdTimer.stop()
                    if (pressedNode && root.editable) {
                        mode = "nudge"
                        nudgeBase = { dx: root._ndx(pressedNode.id, root._nudgeRev),
                                      dy: root._ndy(pressedNode.id, root._nudgeRev) }
                    } else if (!pressedNode && !pressedEdge) {
                        mode = "marquee"
                    } else {
                        mode = "idle"
                    }
                }

                if (mode === "nudge" && pressedNode) {
                    var n = root._nudges
                    n[pressedNode.id] = { dx: nudgeBase.dx + (p.x - pressLX),
                                          dy: nudgeBase.dy + (p.y - pressLY) }
                    root._nudges = n
                    root._nudgeRev++
                }
            }

            onReleased: (mouse) => {
                holdTimer.stop()
                var p = toLayout(mouse.x, mouse.y)

                if (mode === "measure") {
                    if (root._measureAt(p.x, p.y) === pressedMeasure) {
                        // The measure is its own object with its own page; opening it does NOT
                        // re-centre the graph on the condition the row was read from.
                        var bar = pressedMeasure.indexOf("|")
                        root.nodeActivated("measures", pressedMeasure.substring(bar + 1))
                    }
                    pressedMeasure = ""
                    mode = "idle"
                    return
                }
                if (mode === "reference") {
                    if (root._referenceAt(p.x, p.y) === pressedReference) {
                        // The paper is its own object with its own page, and opening it does not
                        // re-centre the graph either — a citation is not a place in the causal band.
                        var rbar = pressedReference.indexOf("|")
                        root.nodeActivated("references", pressedReference.substring(rbar + 1))
                    }
                    pressedReference = ""
                    mode = "idle"
                    return
                }
                if (mode === "toggle") {
                    // Only if the release is on the SAME control the press was on.
                    if (root._toggleAt(p.x, p.y) === pressedToggle)
                        root.expandToggled(pressedToggle.substring(0, pressedToggle.length - 2))
                    pressedToggle = ""
                    mode = "idle"
                    return
                }
                if (mode === "ring")  { ring.release(); mode = "idle"; return }
                if (root._dragging)   { root._dropDrag(p.x, p.y); mode = "idle"; return }
                if (mode === "marquee") { root._applyMarquee(); mode = "idle"; return }
                if (mode === "nudge")   { mode = "idle"; return }

                // A press that neither held nor travelled is a click.
                if (mode === "pending") {
                    if (pressedNode) root.nodeActivated(pressedNode.nodeType || "", pressedNode.id)
                    else if (pressedEdge) root.edgeActivated(pressedEdge.rowId)
                    else root.selectionRequested([], [])
                }
                mode = "idle"
            }

            onCanceled: {
                holdTimer.stop(); mode = "idle"
                pressedToggle = ""; pressedMeasure = ""; pressedReference = ""
            }
            onExited:   { root._hotToggle = ""; root._hotMeasure = ""; root._hotReference = "" }

            Timer {
                id: holdTimer
                interval: 350
                onTriggered: {
                    input.mode = "ring"
                    root._openRing(input.mouseX, input.mouseY, false)
                }
            }
        }
        }
    }

    // ── The ring ──────────────────────────────────────────────────────────────
    //
    // Parented to this PANE rather than to the window's overlay, so it cannot escape the pane the
    // way the old status bar did — and so its clamping has a viewport to clamp against.
    RingMenu {
        id: ring
        parent: root
        firstOfSession: root._firstHoldOfSession
        onCommitted: (slot, entry, value) => root._commitSpoke(entry, value)
    }

    // Keyboard equivalents live on the PANE, not inside the ring — the ring is one way to reach
    // these verbs and it must not be the only thing that knows about them. Escape abandons whatever
    // gesture is in flight, which is the one key an author will try without being told.
    Shortcut {
        sequences: [ StandardKey.Cancel ]
        enabled:   root.visible && root._dragging
        onActivated: root._disarm()
    }

    // Where a link drag actually lands, most of the time. The canvas draws a NEIGHBOURHOOD, so
    // nearly everything on it is already linked to the focus and therefore refused — which makes
    // dropping on open canvas the ordinary outcome rather than the escape hatch, and makes the list
    // of legal conditions OFF the canvas the thing an author is usually reaching for.
    ModelCausePicker {
        id: cause
        parent: root
        title: root._upstreamDrag ? qsTr("Add a cause") : qsTr("Add an effect")
        upstream: root._upstreamDrag
        candidateSource: (text) => root.causeCandidatesProbe
                                       ? root.causeCandidatesProbe(root._armFixed, text, root._armEnd)
                                       : []
        onPicked: (id) => {
            if (root._armEnd === "to") root.linkRequested(id, root._armFixed)
            else                       root.linkRequested(root._armFixed, id)
            root._disarm()
        }
        onCreated: (objType, name) => {
            root.createLinkedRequested(objType, name, root._armFixed, root._armEnd)
            root._disarm()
        }
        onRejected: root._disarm()
    }

    // (fixedId, searchText, end) -> legal existing conditions. Pre-filtered in C++, and over the
    // WHOLE library rather than the drawn nodes — which is the entire point: the ones worth linking
    // to are the ones this picture does not contain.
    property var causeCandidatesProbe: null

    // ── Building the ring's eight slots ───────────────────────────────────────
    //
    // Clockwise from north: look · make · set · unmake. A direction means the same thing for every
    // selection type, so the arrays below differ in what fills a slot and NEVER in which slot a
    // meaning lives in. `null` is a gap; there is no disabled state.
    // WHICH ring is decided by what was pressed, not by what happens to be selected: holding bare
    // canvas opens the canvas ring even with a node still picked up from a moment ago, because the
    // author pressed the canvas. What the SELECTION decides is the scope — pressing inside a
    // multi-selection keeps all of it, which is the only way the bulk ring is ever reached.
    function _ringModel() {
        if (input.pressedNode) {
            if (root.selectedNodeIds.length > 1) return root._bulkRing()
            return root._nodeRing(input.pressedNode.id)
        }
        if (input.pressedEdge) {
            if (root.selectedEdgeIds.length > 1) return root._bulkRing()
            return root._linkRing(input.pressedEdge.rowId)
        }
        return root._canvasRing()
    }

    function _spoke(icon, label, hint, verb, opts) {
        var e = { icon: icon, label: label, hint: hint, verb: verb,
                  kind: "action", enabled: true, danger: false, values: [] }
        if (opts) for (var k in opts) e[k] = opts[k]
        return e
    }

    function _nodeRing(id) {
        var n = root._nodeById(id)
        if (!n) return root._canvasRing()
        var vals = root.ringValuesProbe
                       ? root.ringValuesProbe(n.nodeType || "characteristics", id, "group") : []
        // `Revert to shipped` and `Move to trash` are the SAME call, and the object's source is
        // what decides which of the two it means. An object that ships and has been changed here
        // reverts; one authored here goes to the trash; one that ships untouched has neither, and
        // its slot is a gap rather than a button that would explain itself by refusing.
        var src   = n.source || "shipped"
        var mine  = src === "yours"
        var over  = src === "both"
        return [
            _spoke("◎", qsTr("Focus here"),  qsTr("Centre the graph on this"), "focus"),
            _spoke("⧉", qsTr("Duplicate"),   qsTr("A copy, pre-filled from this one"), "duplicate"),
            // TWO make-a-link spokes, not one, and the make column is doubled to pay for it.
            //
            // The brief argued for a single `Add cause of this…` with the direction carried by the
            // wording, the arrowhead and the hover text, and it costs the SE slot that `New cause
            // here` occupied. That was weighed and reversed: the DAG is read in both directions —
            // "what does this cause" is as ordinary a question as "what causes this" — and an
            // inversion the author has to know about (a held modifier, undiscoverable, invisible at
            // rest) is not the equal of a spoke they can see. `New cause here` is not lost with the
            // slot: the drag's own drop popover creates, and so does the inspector's `Add a cause`.
            _spoke("→", qsTr("Add cause…"),
                   qsTr("Drag to the condition that causes this"), "addCause", { kind: "drag" }),
            _spoke("←", qsTr("Add effect…"),
                   qsTr("Drag to the condition this one causes"), "addEffect", { kind: "drag" }),
            vals && vals.length > 0
                ? _spoke("▤", qsTr("Group"), qsTr("Which part of the swing this belongs to"),
                         "group", { kind: "value", values: vals })
                : null,
            over ? _spoke("↺", qsTr("Revert to shipped"),
                          qsTr("Drop your changes and take the shipped version"), "revert") : null,
            mine ? _spoke("␡", qsTr("Move to trash"), qsTr("Remove this condition"),
                          "trash", { danger: true })
                 : null,
            _spoke("▦", qsTr("Show in table"), qsTr("Find this row in the table"), "showInTable")
        ]
    }

    function _linkRing(rowId) {
        var e = root._edgeById(rowId)
        var vals = root.ringValuesProbe ? root.ringValuesProbe("links", rowId, "strength") : []
        var over = e && e.source === "both"
        return [
            _spoke("◎", qsTr("Open in inspector"), qsTr("This claim, in full"), "inspect"),
            null,
            _spoke("→", qsTr("Re-point from…"),
                   qsTr("Drag to the condition this should come from"),
                   "repointFrom", { kind: "drag" }),
            _spoke("→", qsTr("Re-point to…"),
                   qsTr("Drag to the condition this should point at"),
                   "repointTo", { kind: "drag" }),
            vals && vals.length > 0
                ? _spoke("▤", qsTr("Strength"), qsTr("How strongly this cause explains it"),
                         "strength", { kind: "value", values: vals })
                : null,
            over ? _spoke("↺", qsTr("Revert to shipped"),
                          qsTr("Drop your changes and take the shipped claim"), "revert") : null,
            // A link is a row that ceases to exist, so it is never "moved to trash" — there is no
            // trash it could be found in. Undo is the whole of its recoverability, and the wording
            // has to be honest about that.
            _spoke("␡", qsTr("Delete link"), qsTr("This claim stops existing"),
                   "deleteLink", { danger: true }),
            _spoke("▦", qsTr("Show in table"), qsTr("Find this row in the table"), "showInTable")
        ]
    }

    function _bulkRing() {
        var nodes = root.selectedNodeIds.length
        var edges = root.selectedEdgeIds.length
        var onNodes = nodes > 0
        var n = onNodes ? nodes : edges
        var vals = onNodes
            ? (root.ringValuesProbe
                   ? root.ringValuesProbe("characteristics", root.selectedNodeIds[0], "group") : [])
            : (root.ringValuesProbe
                   ? root.ringValuesProbe("links", root.selectedEdgeIds[0], "strength") : [])
        // Every live spoke is the bulk form with the count in the label, and each is ONE command
        // over a list — so one ⌘Z restores all of them rather than the last one.
        return [
            null,
            onNodes ? _spoke("⧉", qsTr("Duplicate %1").arg(n), qsTr("A copy of each"), "duplicateN")
                    : null,
            null,
            null,
            vals && vals.length > 0
                ? _spoke("▤", onNodes ? qsTr("Group") : qsTr("Strength"),
                         qsTr("Set it on all %1").arg(n),
                         onNodes ? "groupN" : "strengthN", { kind: "value", values: vals })
                : null,
            _spoke("↺", qsTr("Revert %1 to shipped").arg(n),
                   qsTr("Drop your changes to these"), "revertN"),
            _spoke("␡", onNodes ? qsTr("Trash %1 conditions").arg(n)
                                : qsTr("Delete %1 links").arg(n),
                   qsTr("All %1 at once, as one step").arg(n), "trashN", { danger: true }),
            _spoke("▦", qsTr("Show %1 in table").arg(n), qsTr("These rows, in the table"),
                   "showNInTable")
        ]
    }

    function _canvasRing() {
        // Scope is a VIEW setting, so its cells are built here rather than asked of the model —
        // there is nothing about it in the pack to ask about. Worded as the pill words it: the
        // spoke is a second way to the same control, not a second control.
        var vals = []
        var lo = Math.max(1, Math.min(root._scopeMax - 2, root.scope - 1))
        for (var i = 0; i < 3; i++) {
            var v = lo + i
            if (v > root._scopeMax) break
            vals.push({ value: String(v),
                        label: qsTr("%1 of %2").arg(v).arg(root._scopeMax),
                        current: v === root.scope })
        }
        return [
            _spoke("⤢", qsTr("Fit to window"), qsTr("Back to the fitted view"), "fit"),
            _spoke("≡", qsTr("Tidy layout"), qsTr("Re-run the layout and drop every nudge"), "tidy"),
            _spoke("✚", qsTr("New condition here"), qsTr("A new condition, unattached"), "newHere"),
            // Nothing on this canvas can be copied yet, so there is nothing to paste. The slot is a
            // gap rather than a button that would exist only to refuse.
            null,
            _spoke("▤", qsTr("Scope"), qsTr("How far around the focus to draw"),
                   "scope", { kind: "value", values: vals }),
            _spoke("↯", root.hideWeak ? qsTr("Show weak links") : qsTr("Hide weak links"),
                   qsTr("Weak claims, drawn or not"), "toggleWeak"),
            // The way back from a picture opened one box at a time. Absent rather than disabled
            // when nothing is open, which is this ring's rule for every other slot.
            root._anyOpen()
                ? _spoke("⊖", qsTr("Close every open box"),
                         qsTr("Back to the picture this opened from"), "collapseAll")
                : null,
            _spoke("▦", qsTr("Switch to table"), qsTr("The same content, as rows"), "toTable")
        ]
    }

    // ── Opening it ────────────────────────────────────────────────────────────
    function _openRing(mx, my, latched) {
        var p = root.mapFromItem(input, mx, my)
        ring.openAt(p.x, p.y, root._ringModel(), root._hubTitle(), root._hubMeta(), latched)
        root._firstHoldOfSession = false
    }

    // The hub says what the selection IS. Line two is replaced by the hot spoke's spoken label the
    // moment one is hot, which is the only place the ring explains itself.
    function _hubTitle() {
        if (input.pressedNode) {
            if (root.selectedNodeIds.length > 1)
                return qsTr("%1 conditions").arg(root.selectedNodeIds.length)
            var n = root._nodeById(input.pressedNode.id)
            return n ? n.label : ""
        }
        if (input.pressedEdge) {
            if (root.selectedEdgeIds.length > 1)
                return qsTr("%1 links").arg(root.selectedEdgeIds.length)
            var e = root._edgeById(input.pressedEdge.rowId)
            var f = e ? root._nodeById(e.from) : null
            var t = e ? root._nodeById(e.to)   : null
            return f && t ? (f.label + " → " + t.label) : qsTr("Causal link")
        }
        return qsTr("Canvas")
    }

    function _sourceWord(src) {
        return src === "both"  ? qsTr("yours over shipped")
             : src === "yours" ? qsTr("yours")
                               : qsTr("shipped")
    }

    function _hubMeta() {
        if (input.pressedNode) {
            if (root.selectedNodeIds.length > 1)
                return qsTr("%1 selected").arg(root.selectedNodeIds.length)
            var n = root._nodeById(input.pressedNode.id)
            if (!n) return ""
            // The ∀ in the node's corner, in words, for the one node the reader is holding — the
            // graph has no hover channel of its own (input is taken at the pane), so this is where
            // a glyph gets explained at all.
            return (n.latent ? qsTr("cause") : qsTr("condition")) + " · " + root._sourceWord(n.source)
                 + (n.ubiquitous === true ? " · " + qsTr("ubiquitous") : "")
        }
        if (input.pressedEdge) {
            if (root.selectedEdgeIds.length > 1)
                return qsTr("%1 selected").arg(root.selectedEdgeIds.length)
            var e = root._edgeById(input.pressedEdge.rowId)
            return qsTr("causal link") + " · " + root._sourceWord(e ? e.source : "")
        }
        // The honest census of what is on screen, which is also the answer to the question a
        // truncated graph raises.
        return qsTr("%1 of %2 drawn").arg(root._nodes.length).arg(root.totalConditions > 0
                                                                  ? root.totalConditions
                                                                  : root._nodes.length)
    }

    // How many conditions exist in all, for the hub's census line. Supplied by the panel because
    // this pane only ever holds a neighbourhood.
    property int totalConditions: 0

    // ── What a committed spoke does ───────────────────────────────────────────
    function _commitSpoke(entry, value) {
        if (!entry) return

        // A drag spoke does not do anything yet: it hands off to a drag with the button still down,
        // and the edit happens on release. One continuous gesture from hold to drop.
        if (entry.kind === "drag") { root._arm(entry.verb); return }

        switch (entry.verb) {
        // The pane's own business — a view state, so it never reaches the model or the undo stack.
        case "fit":        root._userZoom = 1.0; return
        case "tidy":       root._clearNudges(); root.verbInvoked("tidy", null); return
        case "scope":      root.scopeRequested(parseInt(value)); return
        case "toggleWeak": root.switchToggled("weak"); return
        case "collapseAll": root.collapseAllRequested(); return

        // Everything else is the panel's, because it is either a command or a navigation.
        case "focus":
        case "showInTable":
        case "inspect":
        case "duplicate":
        case "revert":
        case "trash":
        case "deleteLink":
        case "newHere":
        case "toTable":
        case "duplicateN":
        case "revertN":
        case "trashN":
        case "showNInTable":
            root.verbInvoked(entry.verb, root._verbArg())
            return
        case "group":
        case "strength":
        case "groupN":
        case "strengthN":
            root.verbInvoked(entry.verb, { ids: root._verbArg().ids, value: value })
            return
        }
    }

    function _verbArg() {
        var onEdges = root.selectedEdgeIds.length > 0 || root.selectedEdgeId !== ""
        var ids = root.selectedEdgeIds.length > 0 ? root.selectedEdgeIds
                : root.selectedEdgeId !== ""      ? [ root.selectedEdgeId ]
                : root.selectedNodeIds
        // The TYPE travels with the ids. A latent cause and a characteristic are different rows in
        // different lists, and a verb that assumed one of them would act on the wrong table the
        // first time it met the other.
        var t = "characteristics"
        if (onEdges) t = "links"
        else if (ids.length > 0) {
            var n = root._nodeById(ids[0])
            if (n && n.nodeType) t = n.nodeType
        }
        return {
            ids: ids,
            type: t,
            // Where a `new … here` lands, so the panel does not have to guess at the middle.
            x: input.pressLX,
            y: input.pressLY
        }
    }

    // ── Arming a drag ─────────────────────────────────────────────────────────
    //
    // The refusal set is computed HERE, once, at the moment the drag arms — not per hover. The
    // predicate is authoritative and it lives in C++: a node that is off this canvas is still in
    // the graph, and a reachability check written over the drawn nodes would happily draw a cycle
    // through one of them.
    function _arm(verb) {
        var fixed = "", end = "to", edgeId = ""
        var making = verb === "addCause" || verb === "addEffect"
        if (making) {
            if (root.selectedNodeIds.length !== 1) return
            fixed = root.selectedNodeIds[0]
            // Which END the held node occupies is the whole of the difference between the two make
            // spokes, and everything downstream of here reads it off this one value: the anchor
            // side of the box, which way the curve is drawn, where the arrowhead sits, which
            // direction the refusal traversal runs, and which end a dropped-on-canvas node takes.
            //
            // `Add cause…`  — the target becomes the cause, so the held node is the `to`.
            // `Add effect…` — the held node causes the target, so it is the `from`.
            end = verb === "addCause" ? "to" : "from"
        } else {
            edgeId = root.selectedEdgeId
            var e  = root._edgeById(edgeId)
            if (!e) return
            // The end that STAYS PUT is the one the refusal set is computed for.
            if (verb === "repointFrom") { fixed = e.to;   end = "to" }
            else                        { fixed = e.from; end = "from" }
        }

        root._armKind   = making ? "link" : "repoint"
        root._armFixed  = fixed
        root._armEnd    = end
        root._armEdgeId = edgeId
        root._refusals  = root.refusalsProbe
                              ? root.refusalsProbe(fixed, root._conditionIds(), end) : ({})
        var a = root._armAnchor()
        root._dragX = a.x
        root._dragY = a.y
        root._dragTarget = ""
        input.mode = "armed"
    }

    // The inspector's entrance to the identical drag. Returns false when this pane cannot serve it
    // — the link is not on screen, or its ends are not — so the caller can fall back to a picker
    // rather than arming a drag with nothing to aim at.
    function armRepoint(edgeId, end) {
        var e = root._edgeById(edgeId)
        if (!e) return false
        if (!root._nodeById(e.from) || !root._nodeById(e.to)) return false
        root.selectionRequested([], [ edgeId ])
        root._arm(end === "from" ? "repointFrom" : "repointTo")
        return root._dragging
    }

    function _disarm() {
        root._armKind = ""
        root._armFixed = ""
        root._armEdgeId = ""
        root._refusals = ({})
        root._dragTarget = ""
        input.mode = "idle"
    }

    // The point the drag line is pinned to: the fixed node's edge, on the side the claim will read
    // from, so the line leaves the box where a real one would.
    function _armAnchor() {
        var n = root._nodeById(root._armFixed)
        if (!n) return { x: 0, y: 0 }
        var nx = n.x + root._ndx(n.id, root._nudgeRev)
        var ny = n.y + root._ndy(n.id, root._nudgeRev)
        return { x: root._armEnd === "from" ? nx + n.w : nx, y: ny + n.h / 2 }
    }

    // Said on the node under the cursor, in a whole sentence, naming the other end. It is the third
    // statement of the direction — the spoke's label and the arrowhead being the other two — and it
    // is the only one that names both conditions, which is what makes it checkable by a reader who
    // has lost track of which way round they are.
    function _dropHint() {
        if (root._armKind === "repoint") return qsTr("drop to re-point here")
        var n = root._nodeById(root._armFixed)
        if (!n) return root._upstream() ? qsTr("drop to add cause") : qsTr("drop to add effect")
        return root._upstream() ? qsTr("drop to make this a cause of %1").arg(n.label)
                                : qsTr("drop to make this an effect of %1").arg(n.label)
    }

    // True while the drag is looking for a CAUSE of the held node — the held node is the `to` end.
    readonly property bool _upstreamDrag: root._armEnd === "to"
    function _upstream() { return root._armEnd === "to" }

    function _trackDrag(lx, ly) {
        root._dragX = lx
        root._dragY = ly
        var over = root._nodeAt(lx, ly)
        root._dragTarget = over ? over.id : ""
    }

    function _dropDrag(lx, ly) {
        var over = root._nodeAt(lx, ly)
        // Releasing over a refused node cancels. It was never a drop target, and saying so a second
        // time at the moment of release would be the refusal-after-the-fact the whole design avoids.
        if (over && !root._refusalOf(over.id)) {
            if (root._armKind === "link") {
                if (root._armEnd === "to") root.linkRequested(over.id, root._armFixed)
                else                       root.linkRequested(root._armFixed, over.id)
            } else {
                root.repointRequested(root._armEdgeId,
                                      root._armEnd === "to" ? "from" : "to", over.id)
            }
            root._disarm()
            return
        }
        if (over) { root._disarm(); return }

        // Empty canvas: ask which cause, over the whole library. This is the ordinary end of the
        // gesture rather than a fallback — see ModelCausePicker for why the drawn nodes are the
        // wrong place to look for one.
        //
        // A re-point stops here instead: it has an existing claim to move and nowhere out here to
        // move it to. Its own picker is the inspector's.
        if (root._armKind === "link") {
            var p = inner.mapToItem(root, lx, ly)
            cause.openAt(p.x, p.y, lx, ly)
            return
        }
        root._disarm()
    }

    // ── Selection ─────────────────────────────────────────────────────────────
    function _adoptPressSelection(node, edge) {
        if (node) {
            // Already in scope AND nothing else is — a press inside a multi-selection keeps it,
            // which is what makes the bulk ring reachable at all. The second half of the test is
            // load-bearing: a link left selected from a moment ago would otherwise decide which
            // ring opens over the node actually under the finger.
            if (root.selectedNodeIds.indexOf(node.id) >= 0 && root.selectedEdgeId === ""
                && root.selectedEdgeIds.length === 0) return
            root.selectionRequested([ node.id ], [])
            return
        }
        if (edge) {
            if (root.selectedEdgeIds.indexOf(edge.rowId) >= 0
                && root.selectedNodeIds.length === 0) return
            root.selectionRequested([], [ edge.rowId ])
            return
        }
        // Bare canvas keeps whatever is selected until the press turns out to be a click or a
        // marquee: a hold on empty space opens the canvas ring, and clearing here would make that
        // ring depend on how close to a node the author happened to press.
    }

    // A marquee touching only edges selects EDGES; if it touches any node at all it is a node
    // selection, and behaves exactly as the table's does. One rule, so the result of a sloppy drag
    // is predictable rather than a mixture nothing has a verb for.
    function _applyMarquee() {
        var x0 = Math.min(input.pressLX, input.moveLX), x1 = Math.max(input.pressLX, input.moveLX)
        var y0 = Math.min(input.pressLY, input.moveLY), y1 = Math.max(input.pressLY, input.moveLY)
        if (x1 - x0 < Theme.sp(4) && y1 - y0 < Theme.sp(4)) { root.selectionRequested([], []); return }

        var nodes = []
        for (var i = 0; i < root._nodes.length; i++) {
            var n = root._nodes[i]
            if (n.kind === "measure") continue
            var nx = n.x + root._ndx(n.id, root._nudgeRev)
            var ny = n.y + root._ndy(n.id, root._nudgeRev)
            if (nx + n.w >= x0 && nx <= x1 && ny + n.h >= y0 && ny <= y1) nodes.push(n.id)
        }
        if (nodes.length > 0) { root.selectionRequested(nodes, []); return }

        var edges = []
        for (var j = 0; j < root._edges.length; j++) {
            var e = root._edges[j]
            if (e.rowId === undefined) continue
            // Nudged, exactly as the nodes above already are and as the label is drawn. A marquee
            // that swept the layout's original label points would take links whose word is no
            // longer inside the band and miss the ones that are.
            var lx = (e.labelX || 0) + root._edgeLabelDX(e, root._nudgeRev)
            var ly = (e.labelY || 0) + root._edgeLabelDY(e, root._nudgeRev)
            if (lx >= x0 && lx <= x1 && ly >= y0 && ly <= y1 && edges.indexOf(e.rowId) < 0)
                edges.push(e.rowId)
        }
        root.selectionRequested([], edges)
    }

    // The reason, stated in the author's own terms, while the drag is still live. Only for the
    // node under the cursor — every other refusal is already written on its own node.
    Rectangle {
        readonly property var hovered: root._dragging && root._dragTarget !== ""
                                           ? root._refusalOf(root._dragTarget) : null
        visible: hovered !== null
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.sp(16)
        width:  Math.min(reasonText.implicitWidth + Theme.sp(24), root.width - Theme.sp(48))
        height: reasonText.implicitHeight + Theme.sp(16)
        radius: Theme.radius
        color:  Theme.colorErrorLight
        border.width: 1
        border.color: Theme.colorError
        z: 20

        Text {
            id: reasonText
            anchors.centerIn: parent
            width: parent.width - Theme.sp(24)
            text:  parent.hovered ? parent.hovered.text : ""
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorError
            wrapMode:       Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Where the gesture actually ends, said while it is still in flight.
    //
    // Nearly every node drawn here is already linked to the focus — that is WHY it is drawn — so
    // most of them refuse, and an author watching a canvas full of dimmed boxes has no way to know
    // that open space is the answer rather than a dead end. Stating it is the difference between a
    // gesture that looks broken and one that looks finished.
    Rectangle {
        visible: root._dragging && root._armKind === "link" && root._dragTarget === ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.sp(16)
        width:  hintText.implicitWidth + Theme.sp(24)
        height: hintText.implicitHeight + Theme.sp(14)
        radius: Theme.radius
        color:  Theme.colorAccentLight
        border.width: 1
        border.color: Theme.colorAccent
        z: 19

        Text {
            id: hintText
            anchors.centerIn: parent
            text: root._upstreamDrag
                      ? qsTr("Release on open canvas to choose or create a cause")
                      : qsTr("Release on open canvas to choose or create an effect")
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorAccent
        }
    }

    // ── Scope, and the switches ───────────────────────────────────────────────
    //
    // Overlaid on the picture rather than parked under it: they change what you are looking at, so
    // they belong where you are looking.
    //
    // EXPAND and REDUCE, in words, not `−` and `+` against a number. The number was never the thing
    // being chosen — nobody wants "scope 3", they want one more ring of explanation — and a pair of
    // arithmetic signs made the reader work out which direction was more. The level is still shown,
    // because "how far out am I, and is there further to go" is a fair question, but it reads as a
    // position rather than as the setting.
    property int  scope: 2
    // The same bound dag_layout.h clamps to. Stated once because the pill and the ring both offer
    // it, and a control that offers a step the layout ignores is a control that lies — which is
    // what the `+` did while the layout still stopped at 2.
    readonly property int _scopeMax: 4
    property bool includeMeasures: false
    // OFF for the same reason measures are: the causal band is the subject, and the sources behind
    // it are a detail hung under every box. Sparse, too — a fifth of the shipped conditions carry a
    // citation — so left on it would widen the picture for rows most boxes do not have.
    property bool includeReferences: false
    // ON by default, unlike measures. The screened causes are the far left of the causal band
    // rather than a detail hung under it, and a picture that opened without them would show a
    // golfer their swing and hide the reason for it.
    property bool includeScreened: true
    property bool hideWeak: false
    property bool hideProposed: false

    signal scopeRequested(int scope)
    signal switchToggled(string which)

    // Open or close ONE box. The pane holds no list of its own — which boxes are open is a view
    // state the panel owns, and it arrives back through layoutData.
    signal expandToggled(string id)
    signal collapseAllRequested()

    Rectangle {
        anchors.top:   parent.top
        anchors.right: parent.right
        anchors.margins: Theme.sp(10)
        visible: root._nodes.length > 0
        width:   switchRow.implicitWidth + Theme.sp(18)
        height:  Theme.sp(26)
        radius:  height / 2
        color:   Theme.colorSurface
        border.width: 1
        border.color: Theme.colorBorderMid
        z: 30

        RowLayout {
            id: switchRow
            anchors.centerIn: parent
            spacing: Theme.sp(8)

            component Switch: Text {
                id: sw
                required property string label
                required property bool   on
                required property string key
                text:           label
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          on ? Theme.colorAccent : Theme.colorText3
                PpPressable { hoverScale: 1.0; onClicked: root.switchToggled(sw.key) }
            }

            // A step out and a step in, said as verbs. Greyed at the ends rather than hidden: a
            // control that disappears at its limit takes the answer to "is there more?" with it.
            component Step: Text {
                id: st
                required property string label
                required property bool   can
                required property int    to
                text:           label
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color:          st.can ? Theme.colorText2 : Theme.colorText3
                opacity:        st.can ? 1.0 : 0.45
                PpPressable {
                    hoverScale: 1.0
                    enabled:    st.can
                    onClicked:  root.scopeRequested(st.to)
                }
            }

            Step {
                label: qsTr("reduce")
                can:   root.scope > 1
                to:    root.scope - 1
            }
            Text {
                text: qsTr("%1 of %2").arg(root.scope).arg(root._scopeMax)
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
            Step {
                label: qsTr("expand")
                can:   root.scope < root._scopeMax
                to:    root.scope + 1
            }

            Rectangle {
                Layout.preferredWidth:  1
                Layout.preferredHeight: Theme.sp(12)
                color: Theme.colorBorderMid
            }

            Switch { label: qsTr("measures"); on: root.includeMeasures;   key: "measures" }
            Switch { label: qsTr("sources");  on: root.includeReferences; key: "references" }
            Switch { label: qsTr("health");   on: root.includeScreened;   key: "screened" }
            Switch { label: qsTr("weak");     on: !root.hideWeak;       key: "weak" }
            Switch { label: qsTr("proposed"); on: !root.hideProposed;   key: "proposed" }
        }
    }

    // ── Zoom ──────────────────────────────────────────────────────────────────
    //
    // A pill as well as Ctrl+wheel: the wheel is the fast way and is undiscoverable, so the control
    // that teaches it has to be visible. The percentage is the EFFECTIVE scale rather than the
    // user's own multiplier — a graph that opens fitted at 60% should say 60%, not 100% — and
    // clicking it goes back to fitted, which is the only zoom level with a meaning of its own.
    Rectangle {
        anchors.right:  parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.sp(10)
        visible: root._nodes.length > 0
        width:   zoomRow.implicitWidth + Theme.sp(18)
        height:  Theme.sp(26)
        radius:  height / 2
        color:   Theme.colorSurface
        border.width: 1
        border.color: Theme.colorBorderMid
        z: 30

        RowLayout {
            id: zoomRow
            anchors.centerIn: parent
            spacing: Theme.sp(9)

            Text {
                text: "−"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzBody
                color: root._zoom > 0.21 ? Theme.colorText2 : Theme.colorText3
                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root._userZoom = Math.max(0.1, root._userZoom / 1.25)
                }
            }

            Text {
                text: Math.round(root._zoom * 100) + "%"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                // Accented once it is no longer the fitted view, so "you have zoomed" is visible
                // without having to remember what you did.
                color: Math.abs(root._userZoom - 1.0) < 0.001 ? Theme.colorText2 : Theme.colorAccent
                PpPressable { hoverScale: 1.0; onClicked: root._userZoom = 1.0 }
            }

            Text {
                text: "+"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzBody
                color: root._zoom < 3.99 ? Theme.colorText2 : Theme.colorText3
                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root._userZoom = Math.min(20.0, root._userZoom * 1.25)
                }
            }

            Rectangle {
                Layout.preferredWidth:  1
                Layout.preferredHeight: Theme.sp(12)
                color: Theme.colorBorderMid
            }

            Text {
                text: qsTr("fit")
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzMicro
                color: Math.abs(root._userZoom - 1.0) < 0.001 ? Theme.colorText3 : Theme.colorAccent
                PpPressable { hoverScale: 1.0; onClicked: root._userZoom = 1.0 }
            }
        }
    }

    // Never the whole graph unfiltered. What the bound cut off is counted on the nodes above; this
    // is the other half of the same honesty.
    Text {
        anchors.left:   parent.left
        anchors.bottom: parent.bottom
        anchors.margins: Theme.sp(10)
        visible: root.layoutData.truncated === true
        text:    qsTr("bounded — some neighbours are not drawn")
        font.family:    Theme.fontData
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorText3
    }

    Text {
        anchors.centerIn: parent
        width: parent.width - Theme.sp(64)
        visible: root._nodes.length === 0
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("Select a row to centre the graph on it")
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        color:          Theme.colorText3
    }
}
