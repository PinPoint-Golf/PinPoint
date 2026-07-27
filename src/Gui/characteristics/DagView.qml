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
import QtQuick.Shapes
import PinPointStudio

// The causal neighbourhood of one characteristic, as a graph you can walk.
//
// It REPLACES the two lists it used to sit beside, and that is the point: a list of causes and a
// list of effects tells you a condition has three of one and two of the other, and nothing about
// the shape it sits in. Tapping a node re-centres the graph on it, so following a chain back to its
// root is a sequence of taps rather than a sequence of page loads.
//
// ── This file holds NO layout ──────────────────────────────────────────────────────────────────
//
// Every coordinate comes from CharacteristicLibraryModel.dag() -> dag_layout.cpp: ranks, ordering,
// node boxes, the cubic control points, and every encoding flag the delegates read. What is here is
// what a view is for — colour, hover, taps and the menu. Theme metrics go IN as options so the
// C++ side sizes in the same units this renders in.
//
// ── The scope rule that stage 7 paid for ───────────────────────────────────────────────────────
//
// Inside a Repeater delegate the ONLY file-level id that resolves is `root`. `nodeMenu`, `toast`
// and `caption` are siblings of the graph and a delegate handler CANNOT see them — it throws
// ReferenceError the moment it is clicked, which no binding, no test and no screenshot will show.
// So every delegate handler below calls a method on `root`, and those methods touch the other ids
// from file scope. For the same reason no delegate here is a composite type: `PpPressable` and
// friends declare their own `id: root`, which shadows this one even where it would have resolved.
Item {
    id: root

    required property var    library     // CharacteristicLibraryModel
    required property string rootId      // the condition whose page this is
    // Optional. With no editor the graph is read-only and the menu offers navigation only — which
    // is the correct degradation, not a disabled button that cannot say why.
    property var             editor: null

    signal openCondition(string conditionId)   // leave the graph; open that condition's own page
    signal openMeasure(string measureId)
    signal graphChanged()                       // an edge was added or removed

    // ── Navigation state ──────────────────────────────────────────────────────
    // The breadcrumb IS the state: trail[0] is always the page's own characteristic, and back pops.
    property var    _trail:       []
    property var    _trailLabels: []
    property int    _depth:       1
    property int    _revision:    0     // bumped after an edit, to re-query

    readonly property string _focusId:
        root._trail.length > 0 ? root._trail[root._trail.length - 1] : root.rootId

    onRootIdChanged: root._reset()
    Component.onCompleted: root._reset()

    function _reset() {
        root._trail       = root.rootId.length > 0 ? [root.rootId] : []
        root._trailLabels = [""]
        root._depth       = 1
        root._zoom        = 1.0
        root._clearHover()
        nodeMenu.visible = false
    }

    // ── The layout, from C++ ──────────────────────────────────────────────────
    readonly property var _layout: {
        var _ = root._revision                      // re-query after an edit
        if (!root.library || root._focusId.length === 0) return ({ nodes: [], edges: [] })
        return root.library.dag(root._focusId, {
            nodeH:   Theme.sp(44),
            // Wide gutters, deliberately. The columns have to hold an arrowhead AND the strength
            // word on the line between them, and a graph packed to its minimum reads as a diagram
            // of a diagram — the boxes stop being separate things.
            gapX:    Theme.sp(120),
            gapY:    Theme.sp(34),
            laneGap: Theme.sp(72),
            padX:    Theme.sp(16),
            // The advance the delegate will actually render at. Passed in rather than assumed, so
            // the estimate tracks the theme's font scale instead of a number baked into the layout.
            charW:   Theme.fontSzBody2 * 0.60,
            minW:    Theme.sp(120),
            maxW:    Theme.sp(270),
            headerH: Theme.sp(26),
            depth:   root._depth
        })
    }

    readonly property var  _nodes:    root._layout.nodes    || []
    readonly property var  _edges:    root._layout.edges    || []
    readonly property var  _headings: root._layout.headings || []
    readonly property real _graphW:   root._layout.width    || 0
    readonly property real _graphH:   root._layout.height   || 0

    readonly property string _focusLabel: {
        for (var i = 0; i < root._nodes.length; ++i)
            if (root._nodes[i].kind === "focus") return root._nodes[i].label
        return ""
    }

    // Scale only when it must, and never far. Shrinking a little beats a graph you have to scroll
    // to read; below 0.75 the labels stop being legible, so past that it scrolls instead.
    readonly property real _fit: {
        var avail = plot.width - Theme.sp(36)
        if (root._graphW <= 0 || avail <= 0) return 1
        return Math.max(0.75, Math.min(1, avail / root._graphW))
    }

    // ── Zoom ──────────────────────────────────────────────────────────────────
    //
    // A multiplier ON TOP of the fit rather than a replacement for it, so 100 % always means "as it
    // fits in this panel" whatever the panel's width — the number stays meaningful when the window
    // is resized, which an absolute scale would not.
    //
    // The graph is drawn from coordinates C++ computed at one size; zooming SCALES that drawing and
    // never re-runs the layout. Re-running it with a different character width would reflow the
    // columns, which is a different picture rather than a closer look at the same one.
    readonly property real _zoomMin: 0.5
    readonly property real _zoomMax: 3.0
    property real _zoom: 1.0
    readonly property real _scale: root._fit * root._zoom

    function _setZoom(z) {
        root._zoom = Math.max(root._zoomMin, Math.min(root._zoomMax, z))
    }

    // A button press zooms about the CENTRE of the view — the only anchor it has, since the pointer
    // is over the button rather than over the graph. One press equals one wheel notch so the two
    // routes agree about what a step is.
    function _zoomStep(dir) {
        root._zoomAt(root._zoom * Math.pow(1.15, dir), flick.width / 2, flick.height / 2)
    }

    function _resetZoom() {
        root._zoom     = 1.0
        flick.contentX = 0
        flick.contentY = 0
    }

    // Zoom about a point, so what is under the pointer stays under it. Zooming about the origin
    // sends whatever you were looking at off the edge, which is what makes a zoom control feel
    // broken even when the scale is right.
    //
    // Only meaningful once the content overflows: while it fits, the canvas is CENTRED and
    // contentX/Y are pinned at 0, so there is nothing to preserve and the plain set is correct.
    function _zoomAt(z, px, py) {
        var before = root._scale
        root._setZoom(z)
        var after = root._scale
        if (after === before) return

        if (flick.contentWidth > flick.width)
            flick.contentX = Math.max(0, Math.min(flick.contentWidth - flick.width,
                                                  (flick.contentX + px) * after / before - px))
        if (flick.contentHeight > flick.height)
            flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height,
                                                  (flick.contentY + py) * after / before - py))
    }


    // ── Hover caption ─────────────────────────────────────────────────────────
    // Where an unavailable node says what is missing. A greyed box with no explanation is
    // indistinguishable from a rendering fault, and the reason is too long to sit inside the box.
    property string _captionText: ""

    function _clearHover() { root._captionText = "" }

    function _describe(n) {
        if (!n) return ""
        if (n.kind === "measure") {
            var mb = [n.label, n.statusLabel]
            if (n.metricKey) mb.push(n.metricKey)
            if (!n.available && n.unavailableReason) mb.push(n.unavailableReason)
            return mb.join(" · ")
        }
        var b = [n.label]
        if (n.reach && n.reach !== "measured") b.push(n.reachLabel)
        if (n.latent)      b.push(qsTr("inferred, not seen"))
        if (n.offeredOnly) b.push(qsTr("offered, never concluded"))
        if (n.coverage > 0) b.push(qsTr("explains %n other", "", n.coverage))
        if (!n.available && n.unavailableReason) b.push(n.unavailableReason)
        return b.join(" · ")
    }

    function _hover(n) { root._captionText = root._describe(n) }

    // ── Navigation ────────────────────────────────────────────────────────────
    function _tapNode(n) {
        if (!n) return
        // A measure is not a place in this graph — following one leaves for its own page rather
        // than re-centring on something with no causes and no effects.
        if (n.kind === "measure") { root.openMeasure(n.id); return }
        if (n.kind === "focus") return
        root._push(n.id, n.label)
    }

    function _push(id, label) {
        var t = root._trail.slice()
        var l = root._trailLabels.slice()
        t.push(id)
        l.push(label)
        root._trail       = t
        root._trailLabels = l
        root._depth       = 1          // a new centre starts local again
        // A zoom held across a re-centre leaves the new focus off screen, which reads as the tap
        // having done nothing at all.
        root._zoom        = 1.0
        nodeMenu.visible  = false
        root._clearHover()
    }

    function _back() {
        if (root._trail.length <= 1) return
        root._trail       = root._trail.slice(0, root._trail.length - 1)
        root._trailLabels = root._trailLabels.slice(0, root._trailLabels.length - 1)
        root._zoom        = 1.0
        nodeMenu.visible  = false
        root._clearHover()
    }

    function _jumpTo(index) {
        if (index < 0 || index >= root._trail.length - 1) return
        root._trail       = root._trail.slice(0, index + 1)
        root._trailLabels = root._trailLabels.slice(0, index + 1)
        root._zoom        = 1.0
        nodeMenu.visible  = false
        root._clearHover()
    }

    function _toggleDepth() {
        root._depth = root._depth === 1 ? 2 : 1
        nodeMenu.visible = false
    }

    // ── The long-press menu ───────────────────────────────────────────────────
    //
    // Tap navigates; long-press is where anything irreversible lives, so a mis-tap while walking a
    // chain can never edit the graph.
    function _longPress(n, sceneX, sceneY) {
        if (!n) return

        var items = []
        if (n.kind === "measure") {
            items.push({ action: "measure", label: qsTr("Open this measure"), destructive: false })
        } else {
            if (n.kind !== "focus")
                items.push({ action: "open", label: qsTr("Open its own page"), destructive: false })

            // Which way the link runs matters to the wording: "remove this link" has to say which
            // link, and the two directions are different claims about the swing.
            var linked = root._linkBetween(n.id)
            var rel    = root._relationBetween(n.id)
            if (n.kind !== "focus" && root.editor) {
                // A pair already related symmetrically is offered the SYMMETRIC actions and not the
                // causal ones. Offering "X causes Y" beside an existing corroboration would invite
                // an edit the validator refuses — the pair would then count twice in the ranking —
                // and a menu that offers a choice it will reject is worse than one that does not.
                if (rel !== "") {
                    items.push({ action: rel === "corroborates" ? "toExcludes" : "toCorroborates",
                                 destructive: false,
                                 label: rel === "corroborates"
                                        ? qsTr("They cannot both be true, in fact")
                                        : qsTr("They are the same thing seen twice, in fact") })
                    // Imperative, unlike the causal rows above it. "X no longer causes Y" carries
                    // its own signal of change in the "no longer"; "X and Y are not linked" has
                    // none, so it reads as a statement of fact rather than something to press.
                    items.push({ action: "unlinkRelation", destructive: true,
                                 label: qsTr("Unlink %1 and %2").arg(n.label).arg(root._focusLabel) })
                } else if (linked === "cause") {
                    items.push({ action: "unlinkCause", destructive: true,
                                 label: qsTr("%1 no longer causes %2").arg(n.label).arg(root._focusLabel) })
                } else if (linked === "effect") {
                    items.push({ action: "unlinkEffect", destructive: true,
                                 label: qsTr("%1 no longer causes %2").arg(root._focusLabel).arg(n.label) })
                } else {
                    items.push({ action: "link", destructive: false,
                                 label: qsTr("%1 causes %2").arg(n.label).arg(root._focusLabel) })
                    // Corroboration is refused wherever a causal path already exists, so it is only
                    // offered where it could actually be written.
                    if (!root._causallyReachable(n.id))
                        items.push({ action: "corroborate", destructive: false,
                                     label: qsTr("%1 is the same thing seen another way").arg(n.label) })
                    items.push({ action: "exclude", destructive: false,
                                 label: qsTr("%1 cannot also be true").arg(n.label) })
                }
            }
        }
        if (items.length === 0) return

        // Where it WANTS to be. The clamp to the view lives in the x/y bindings, because the menu's
        // own width is not final until its rows have been built from `items`.
        var p = root.mapFromItem(null, sceneX, sceneY)
        nodeMenu.nodeId  = n.id
        nodeMenu.items   = items
        nodeMenu.wantX   = p.x - Theme.sp(8)
        nodeMenu.wantY   = p.y
        nodeMenu.visible = true
    }

    // "cause"  — that node causes the focus
    // "effect" — the focus causes it
    // ""       — no drawn link between them
    function _linkBetween(id) {
        for (var i = 0; i < root._edges.length; ++i) {
            var e = root._edges[i]
            if (e.detects) continue
            // A symmetric relation is asked about separately (_relationBetween) — it is not a
            // direction, and answering "cause" or "effect" for one would put the causal wording in
            // front of a reader for a link that makes no causal claim.
            if (e.symmetric === true) continue
            if (e.from === id && e.to === root._focusId) return "cause"
            if (e.from === root._focusId && e.to === id) return "effect"
        }
        return ""
    }

    // Is there a causal path between this node and the focus, in either direction? Corroboration
    // over one is refused by the validator, so the menu must not offer it. Only the DRAWN edges are
    // consulted — a path outside the current depth is not visible, so the model still gets the last
    // word and the refusal it returns is the one a reader sees.
    function _causallyReachable(id) {
        return root._linkBetween(id) !== ""
    }

    // "" | "corroborates" | "excludes". Read from either end, because the edge means the same
    // whichever way round its author happened to type it.
    function _relationBetween(id) {
        for (var i = 0; i < root._edges.length; ++i) {
            var e = root._edges[i]
            if (e.symmetric !== true) continue
            if ((e.from === id && e.to === root._focusId)
                || (e.from === root._focusId && e.to === id)) return e.relation
        }
        return ""
    }

    function _menuAction(action) {
        var id = nodeMenu.nodeId
        nodeMenu.visible = false
        if (!id) return

        if (action === "open")    { root.openCondition(id); return }
        if (action === "measure") { root.openMeasure(id);   return }
        if (!root.editor) return

        var rel = root._relationBetween(id)

        var r = action === "link"           ? root.editor.linkCause(id, root._focusId, "moderate")
              : action === "unlinkCause"    ? root.editor.unlinkCause(id, root._focusId)
              : action === "unlinkEffect"   ? root.editor.unlinkCause(root._focusId, id)
              : action === "corroborate"    ? root.editor.linkRelation(id, root._focusId, "corroborates", "moderate")
              : action === "exclude"        ? root.editor.linkRelation(id, root._focusId, "excludes")
              : action === "unlinkRelation" ? root.editor.unlinkRelation(id, root._focusId, rel)
              : action === "toExcludes"     ? root.editor.editRelation(id, root._focusId, "corroborates", "excludes")
              : action === "toCorroborates" ? root.editor.editRelation(id, root._focusId, "excludes", "corroborates", "moderate")
              : ({ ok: false, message: "" })

        // Which undo the toast offers depends on which KIND of link was removed. Sharing one slot
        // would let "put the link back" restore the wrong kind — silently, since both are edges and
        // both would apply cleanly.
        root._lastRemovalWasRelation = (action === "unlinkRelation")

        toast.severity = r.ok ? "info" : "warn"
        // Ledger C31: a recoverable removal offers its undo in the same breath. Only when the model
        // says it can be undone — offering an action that can only fail is worse than not offering it.
        toast.showUndo = (r.ok === true && r.canUndo === true)
        toast.show(r.message || "")
        if (r.ok) {
            root._revision++
            root.graphChanged()
        }
    }

    // Put back the link that was just removed, WITH its strength — which re-linking by hand would
    // not restore, and which is the part a reader cannot reconstruct from memory.
    property bool _lastRemovalWasRelation: false

    function _undoUnlink() {
        if (!root.editor) return
        var r = root._lastRemovalWasRelation ? root.editor.undoUnlinkRelation()
                                             : root.editor.undoUnlinkCause()
        toast.showUndo = false                  // one level: the undo is consumed by using it
        toast.severity = r.ok ? "info" : "warn"
        toast.show(r.message || "")
        if (r.ok) {
            root._revision++
            root.graphChanged()
        }
    }

    // ══ Header ════════════════════════════════════════════════════════════════
    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        anchors.left:  parent.left
        anchors.right: parent.right
        anchors.top:   parent.top
        spacing: Theme.sp(8)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(10)

            Text {
                text:                qsTr("HOW IT CONNECTS")
                font.family:         Theme.fontBody
                font.pixelSize:      Theme.fontSzMicro
                font.letterSpacing:  Theme.trackingMicro
                font.capitalization: Font.AllUppercase
                color:               Theme.colorText3
            }

            // ── Breadcrumb ────────────────────────────────────────────────────
            // Only from the second step on: a trail of one is not a trail, and drawing it would put
            // a navigation control on every page that has nowhere to go back to.
            Text {
                visible:        root._trail.length > 1
                text:           "←"
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          backMa.containsMouse ? Theme.colorText : Theme.colorText3

                MouseArea {
                    id: backMa
                    anchors.fill:    parent
                    anchors.margins: -Theme.sp(6)
                    hoverEnabled:    true
                    cursorShape:     Qt.PointingHandCursor
                    onClicked:       root._back()
                }
            }

            Repeater {
                model: root._trail.length > 1 ? root._trailLabels : []
                delegate: Text {
                    id: crumb
                    required property var modelData
                    required property int index

                    readonly property bool last: crumb.index === root._trail.length - 1
                    readonly property string ownLabel:
                        crumb.index === 0 ? (root._trailLabels[0] || qsTr("This characteristic"))
                                          : (crumb.modelData || "")

                    text: crumb.ownLabel + (crumb.last ? "" : "  ›")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          crumb.last ? Theme.colorText2
                                  : crumbMa.containsMouse ? Theme.colorText : Theme.colorText3
                    elide:          Text.ElideRight
                    Layout.maximumWidth: Theme.sp(160)

                    MouseArea {
                        id: crumbMa
                        anchors.fill: parent
                        enabled:      !crumb.last
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root._jumpTo(crumb.index)
                    }
                }
            }

            Item { Layout.fillWidth: true }

            // ── Expand ────────────────────────────────────────────────────────
            // One step, and it stops there. The whole-library picture is the cause-coverage list;
            // this view exists because that one cannot be walked.
            Rectangle {
                implicitWidth:  expandText.implicitWidth + Theme.sp(20)
                implicitHeight: Theme.sp(24)
                radius: height / 2
                color:  root._depth === 2 ? Theme.colorBg3 : Theme.colorBg2
                border.width: 1
                border.color: expandMa.containsMouse ? Theme.colorBorderMid : "transparent"
                visible: root._nodes.length > 1

                Text {
                    id: expandText
                    anchors.centerIn: parent
                    text:           root._depth === 1 ? qsTr("Expand") : qsTr("Collapse")
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText2
                }
                MouseArea {
                    id: expandMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape:  Qt.PointingHandCursor
                    onClicked:    root._toggleDepth()
                }
            }
        }

        // ══ The graph ═════════════════════════════════════════════════════════
        Rectangle {
            id: plot
            Layout.fillWidth: true
            // A canvas, not a strip. The floor is deliberately well above what a two-node graph
            // needs: the page below this is mostly empty, and a graph packed into the smallest box
            // that will hold it reads as an afterthought however well spaced its own contents are.
            // The cap keeps a tall graph from pushing the provenance block off a page the reader
            // came here for; past it the canvas scrolls.
            implicitHeight: Math.min(Theme.sp(680),
                                     Math.max(Theme.sp(360), root._graphH * root._fit + Theme.sp(48)))
            radius: Theme.radius
            color:  Theme.colorBg2
            clip:   true

            // Nothing to draw. It is not an error state — a characteristic nothing explains yet is
            // ordinary, and it is the first thing a newly authored one looks like.
            Text {
                anchors.centerIn: parent
                width:            parent.width - Theme.sp(48)
                visible:          root._nodes.length === 0
                text:             qsTr("Nothing in the library connects to this yet.")
                horizontalAlignment: Text.AlignHCenter
                font.family:      Theme.fontBody
                font.pixelSize:   Theme.fontSzBody2
                color:            Theme.colorText3
                wrapMode:         Text.WordWrap
            }

            Flickable {
                id: flick
                anchors.fill:    parent
                anchors.margins: Theme.sp(18)
                contentWidth:    root._graphW * root._scale
                contentHeight:   root._graphH * root._scale
                boundsBehavior:  Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalAndVerticalFlick
                interactive:     contentWidth > width || contentHeight > height

                Item {
                    id: canvas
                    // Centred while it fits, pinned to the origin once it does not — a graph that
                    // drifts as you expand it costs the reader their bearings.
                    x: Math.max(0, (flick.width  - flick.contentWidth)  / 2)
                    y: Math.max(0, (flick.height - flick.contentHeight) / 2)
                    width:  root._graphW
                    height: root._graphH
                    scale:  root._scale
                    transformOrigin: Item.TopLeft

                    // ── Headings ──────────────────────────────────────────────
                    // Which way the arrows mean, in words. Left-to-right reading as "causes" is
                    // the whole convention of the picture, and leaving a reader to infer it from
                    // arrowheads is leaving them to guess.
                    Repeater {
                        model: root._headings
                        delegate: Text {
                            id: head
                            required property var modelData

                            x:     head.modelData.x
                            y:     head.modelData.y
                            width: head.modelData.w
                            horizontalAlignment: Text.AlignHCenter
                            text:                head.modelData.label
                            font.family:         Theme.fontBody
                            font.pixelSize:      Theme.fontSzMicro
                            font.letterSpacing:  Theme.trackingMicro
                            font.capitalization: Font.AllUppercase
                            color:               Theme.colorText3
                        }
                    }

                    // ── Edges, under the nodes ────────────────────────────────
                    Repeater {
                        model: root._edges
                        delegate: Shape {
                            id: link
                            required property var modelData
                            anchors.fill: parent
                            preferredRendererType: Shape.CurveRenderer

                            // WHAT KIND OF LINE IT IS, IN COLOUR — not in dashes.
                            //
                            // Dashes were doing this job for detection and offered links, and they
                            // read as noise at this line weight: a dashed cubic on a curved path
                            // looks like a rendering artefact rather than a distinction somebody
                            // chose. Colour separates the four kinds cleanly at any weight, and the
                            // legend below names them, which a dash pattern never could.
                            readonly property color ink:
                                link.modelData.detects              ? Theme.colorBorderMid
                              : link.modelData.relation === "excludes"     ? Theme.colorError
                              : link.modelData.relation === "corroborates" ? Theme.colorGood
                              : link.modelData.offeredOnly          ? Theme.colorAccentMid
                              :                                       Theme.colorBorderStrong

                            ShapePath {
                                // Strength is a WEIGHT, drawn as one. It is never a percentage and
                                // never a number on this surface. An exclusion has no strength —
                                // the pair is incompatible or it is not — so it draws thin.
                                strokeWidth: link.modelData.detects
                                             ? 1 : Math.max(1, link.modelData.weight)
                                strokeColor: link.ink
                                fillColor:   "transparent"
                                capStyle:    ShapePath.RoundCap
                                strokeStyle: ShapePath.SolidLine

                                startX: link.modelData.x1
                                startY: link.modelData.y1
                                PathCubic {
                                    control1X: link.modelData.c1x; control1Y: link.modelData.c1y
                                    control2X: link.modelData.c2x; control2Y: link.modelData.c2y
                                    x:         link.modelData.x2;  y:         link.modelData.y2
                                }
                            }

                            // The arrowhead, as three points computed in C++. Which end is the
                            // effect is the entire claim of a causal graph — it is not left to a
                            // rotation worked out in a delegate. A segment with no tip gets a
                            // degenerate triangle, which draws nothing.
                            ShapePath {
                                fillColor:   link.modelData.tip === true ? link.ink : "transparent"
                                strokeWidth: -1
                                startX: link.modelData.tip === true ? link.modelData.tipAx : 0
                                startY: link.modelData.tip === true ? link.modelData.tipAy : 0
                                PathLine {
                                    x: link.modelData.tip === true ? link.modelData.tipBx : 0
                                    y: link.modelData.tip === true ? link.modelData.tipBy : 0
                                }
                                PathLine {
                                    x: link.modelData.tip === true ? link.modelData.tipCx : 0
                                    y: link.modelData.tip === true ? link.modelData.tipCy : 0
                                }
                                PathLine {
                                    x: link.modelData.tip === true ? link.modelData.tipAx : 0
                                    y: link.modelData.tip === true ? link.modelData.tipAy : 0
                                }
                            }
                        }
                    }

                    // ── What each line claims ─────────────────────────────────
                    // "usually" / "often" / "sometimes" — the strength IN WORDS, on the line it
                    // belongs to. It is a ranking weight, so it is never a percentage, and it sits
                    // on the line rather than in a legend nobody reads.
                    Repeater {
                        model: root._edges
                        delegate: Rectangle {
                            id: edgeTag
                            required property var modelData

                            visible: (edgeTag.modelData.label || "").length > 0
                            width:   tagText.implicitWidth + Theme.sp(8)
                            height:  tagText.implicitHeight + Theme.sp(2)
                            x:       edgeTag.modelData.labelX - width / 2
                            y:       edgeTag.modelData.labelY - height / 2
                            // The plot's own fill, so the curve does not run through the word.
                            color:   Theme.colorBg2
                            radius:  Theme.sp(3)

                            Text {
                                id: tagText
                                anchors.centerIn: parent
                                text:           edgeTag.modelData.label || ""
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                        }
                    }

                    // ── Nodes ─────────────────────────────────────────────────
                    Repeater {
                        model: root._nodes
                        delegate: Rectangle {
                            id: box
                            required property var modelData

                            readonly property bool isFocus:   box.modelData.kind === "focus"
                            readonly property bool isMeasure: box.modelData.kind === "measure"
                            readonly property bool dim:       box.modelData.available === false

                            x:      box.modelData.x
                            y:      box.modelData.y
                            width:  box.modelData.w
                            height: box.modelData.h
                            radius: box.isMeasure ? height / 2 : Theme.radius

                            // A latent cause is OUTLINED. It cannot be seen in the swing; it was
                            // worked out from what it explains, and a reader must be able to tell
                            // the two apart without reading a word.
                            color: box.modelData.latent ? "transparent"
                                 : box.isFocus          ? Theme.colorBg3
                                 : box.isMeasure        ? Theme.colorBg
                                 :                        Theme.colorSurface
                            border.width: box.isFocus ? 2 : 1
                            border.color: box.isFocus            ? Theme.colorAccent
                                        : nodeMa.containsMouse   ? Theme.colorBorderStrong
                                        : box.modelData.latent   ? Theme.colorBorderMid
                                        :                          Theme.colorBorder
                            opacity: box.dim && !box.isFocus ? 0.6 : 1.0

                            Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                            RowLayout {
                                anchors.fill:        parent
                                anchors.leftMargin:  Theme.sp(10)
                                anchors.rightMargin: Theme.sp(10)
                                spacing: Theme.sp(6)

                                Text {
                                    Layout.fillWidth: true
                                    text:           box.modelData.label
                                    font.family:    Theme.fontBody
                                    font.pixelSize: box.isFocus ? Theme.fontSzBody : Theme.fontSzBody2
                                    // Asserted is OFFERED, never concluded, and looks different
                                    // everywhere it appears — including here.
                                    font.italic:    box.modelData.offeredOnly === true
                                    color:          box.dim               ? Theme.colorText3
                                                  : box.isFocus           ? Theme.colorText
                                                  : box.modelData.offeredOnly ? Theme.colorText3
                                                  :                         Theme.colorText2
                                    elide:          Text.ElideRight
                                    maximumLineCount: 1
                                }

                                // A measure says whether anything produces it, in one word.
                                Text {
                                    visible:        box.isMeasure && box.dim
                                    text:           "○"
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzMicro
                                    color:          Theme.colorText3
                                }
                            }

                            // ── What the bound cut off ────────────────────────
                            // On the node it hangs off, so a reader knows WHICH box has more behind
                            // it before tapping. A graph that omits half of what it knows without
                            // saying so is worse than one that draws nothing.
                            //
                            // Below the corner, not on the mid-line: the mid-line is exactly where
                            // the fanned edges leave the box, and a count sitting under a curve
                            // reads as a label ON that curve.
                            Text {
                                visible:        (box.modelData.hiddenCauses || 0) > 0
                                anchors.right:  parent.left
                                anchors.rightMargin: Theme.sp(5)
                                anchors.top:    parent.bottom
                                anchors.topMargin: Theme.sp(1)
                                text:           "+" + box.modelData.hiddenCauses
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }
                            Text {
                                visible:        (box.modelData.hiddenEffects || 0) > 0
                                anchors.left:   parent.right
                                anchors.leftMargin: Theme.sp(5)
                                anchors.top:    parent.bottom
                                anchors.topMargin: Theme.sp(1)
                                text:           "+" + box.modelData.hiddenEffects
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color:          Theme.colorText3
                            }

                            MouseArea {
                                id: nodeMa
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape:  box.isFocus ? Qt.ArrowCursor : Qt.PointingHandCursor
                                pressAndHoldInterval: 450

                                // Every handler here goes through `root`. `nodeMenu`, `toast` and
                                // `caption` are file-scope siblings and are NOT visible from inside
                                // a delegate — see the note at the top of this file.
                                onEntered:      root._hover(box.modelData)
                                onExited:       root._clearHover()
                                // Press, not only hover: on a touch screen there is no hover, and
                                // the caption is where a greyed node says which measure it is
                                // waiting on. Without this that reason would be unreachable.
                                onPressed:      root._hover(box.modelData)
                                onClicked:      root._tapNode(box.modelData)
                                onPressAndHold: function (mouse) {
                                    var p = box.mapToItem(null, mouse.x, mouse.y)
                                    root._longPress(box.modelData, p.x, p.y)
                                }
                            }
                        }
                    }
                }
            }

            // ══ Zoom controls ═════════════════════════════════════════════════
            //
            // Overlaid on the canvas rather than placed beside it: they belong to the picture, and a
            // row of controls under the graph would compete with the caption and the legend, which
            // are what a reader is actually meant to read.
            //
            // Muted until the pointer is over the graph. Chrome at full strength all the time reads
            // as part of the content, and this is a control for a picture that mostly needs none.
            Rectangle {
                id: zoomBar
                anchors.right:   parent.right
                anchors.bottom:  parent.bottom
                anchors.margins: Theme.sp(10)
                visible: root._nodes.length > 0

                implicitWidth:  zoomRow.implicitWidth + Theme.sp(10)
                implicitHeight: Theme.sp(26)
                radius: height / 2
                color:  Theme.colorBg
                border.width: 1
                border.color: Theme.colorBorderMid
                opacity: plotHover.hovered ? 1.0 : 0.35
                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

                Row {
                    id: zoomRow
                    anchors.centerIn: parent
                    spacing: Theme.sp(2)

                    // One press is one wheel notch, so the two routes agree about what a zoom step
                    // means. Both zoom about the CENTRE of the view, which is the only anchor a
                    // button press has — the pointer is over the button, not over the graph.
                    Rectangle {
                        width:  Theme.sp(22)
                        height: Theme.sp(22)
                        radius: height / 2
                        color:  outMa.containsMouse && root._zoom > root._zoomMin
                                ? Theme.colorBg2 : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text:           "\u2212"
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color: root._zoom > root._zoomMin ? Theme.colorText2 : Theme.colorText3
                        }
                        MouseArea {
                            id: outMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked:    root._zoomStep(-1)
                        }
                    }

                    // The percentage doubles as the reset. It is where a reader looks to find out
                    // what the zoom IS, so it is where they look to undo it.
                    Rectangle {
                        width:  Theme.sp(46)
                        height: Theme.sp(22)
                        radius: Theme.sp(4)
                        color:  resetMa.containsMouse ? Theme.colorBg2 : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text:           Math.round(root._zoom * 100) + "%"
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                        }
                        MouseArea {
                            id: resetMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked:    root._resetZoom()
                        }
                    }

                    Rectangle {
                        width:  Theme.sp(22)
                        height: Theme.sp(22)
                        radius: height / 2
                        color:  inMa.containsMouse && root._zoom < root._zoomMax
                                ? Theme.colorBg2 : "transparent"
                        Text {
                            anchors.centerIn: parent
                            text:           "+"
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color: root._zoom < root._zoomMax ? Theme.colorText2 : Theme.colorText3
                        }
                        MouseArea {
                            id: inMa
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape:  Qt.PointingHandCursor
                            onClicked:    root._zoomStep(+1)
                        }
                    }
                }
            }

            // Hover state for the whole plot, so the controls can fade in with the pointer without
            // any of them having to know about the others.
            HoverHandler { id: plotHover }

            // ══ Ctrl + wheel ══════════════════════════════════════════════════
            //
            // On an overlay that covers the whole plot, and LAST so it sits above everything.
            //
            // It started as a handler beside the Flickable and only fired in some of the panel:
            // Flickable handles the wheel itself, so whether the event reached a sibling depended
            // on what was under the pointer and on whether the content could scroll in that
            // direction. Which sibling wins is not something to guess at — putting the handler
            // above the competition removes the question.
            //
            // The Item accepts no other input. An Item with only a WheelHandler is transparent to
            // press and hover, so the nodes below still take their taps and long-presses, and the
            // zoom buttons still take their clicks. And because the handler declares
            // `acceptedModifiers`, a BARE wheel is not accepted here either — it falls through to
            // the Flickable to pan the graph, and past that to the page to scroll it.
            Item {
                anchors.fill: parent

                WheelHandler {
                    acceptedModifiers: Qt.ControlModifier
                    acceptedDevices:   PointerDevice.Mouse | PointerDevice.TouchPad
                    onWheel: function (ev) {
                        // A notch is 120 units; a touchpad sends much smaller ones. Scaling by the
                        // fraction of a notch stops a trackpad jumping three steps at a time.
                        var steps = ev.angleDelta.y / 120.0
                        if (steps === 0) return
                        // `ev.x` / `ev.y`, NOT `ev.position`. A QML WheelEvent carries the
                        // coordinates directly; `position` belongs to an EventPoint, and reading it
                        // threw a TypeError that swallowed the zoom while leaving the handler
                        // looking perfectly wired up.
                        var p = plot.mapToItem(flick, ev.x, ev.y)
                        root._zoomAt(root._zoom * Math.pow(1.15, steps), p.x, p.y)
                    }
                }
            }
        }

        // ══ Caption ═══════════════════════════════════════════════════════════
        // The hovered node in words — including, for a greyed one, the measure it is waiting on.
        Text {
            id: caption
            Layout.fillWidth: true
            // The distance has to be the one actually drawn. "One step away" under a two-step graph
            // is a caption that quietly stops being true the moment somebody presses Expand.
            text: root._captionText.length > 0 ? root._captionText
                : root._layout.truncated === true
                  ? qsTr("Showing what is within %n step(s). Some links are off this view — the "
                         + "small numbers count them. Tap a box to centre on it; press and hold "
                         + "for more.", "", root._depth)
                  : qsTr("Tap a box to centre the graph on it; press and hold for more.")
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            wrapMode:       Text.WordWrap
            visible:        root._nodes.length > 0
        }

        // ══ Legend ════════════════════════════════════════════════════════════
        //
        // Four kinds of line now say four different things, and colour without a key is decoration.
        // Only the kinds actually PRESENT are listed: a legend naming a relationship this graph does
        // not contain teaches the reader to look for something that is not there.
        Flow {
            Layout.fillWidth: true
            spacing: Theme.sp(14)
            visible: root._nodes.length > 0

            Repeater {
                model: {
                    var seen = { causes: false, corroborates: false, excludes: false, detects: false }
                    for (var i = 0; i < root._edges.length; ++i) {
                        var e = root._edges[i]
                        if (e.detects) seen.detects = true
                        else if (e.relation === "corroborates") seen.corroborates = true
                        else if (e.relation === "excludes") seen.excludes = true
                        else seen.causes = true
                    }
                    var out = []
                    if (seen.causes)
                        out.push({ c: Theme.colorBorderStrong, t: qsTr("causes — the arrow points at the effect") })
                    if (seen.corroborates)
                        out.push({ c: Theme.colorGood, t: qsTr("also seen as — the same event, read another way") })
                    if (seen.excludes)
                        out.push({ c: Theme.colorError, t: qsTr("cannot also be — one swing cannot be both") })
                    if (seen.detects)
                        out.push({ c: Theme.colorBorderMid, t: qsTr("measured by") })
                    return out
                }

                delegate: Row {
                    required property var modelData
                    spacing: Theme.sp(5)

                    Rectangle {
                        width:  Theme.sp(14)
                        height: 2
                        radius: 1
                        color:  parent.modelData.c
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text:           parent.modelData.t
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                    }
                }
            }
        }
    }

    // ══ Long-press menu ═══════════════════════════════════════════════════════
    MouseArea {
        anchors.fill: parent
        visible:      nodeMenu.visible
        z:            9
        onClicked:    nodeMenu.visible = false
    }

    Rectangle {
        id: nodeMenu
        visible: false
        z:       10

        property string nodeId: ""
        property var    items:  []
        property real   wantX:  0
        property real   wantY:  0

        implicitWidth:  Math.max(Theme.sp(160), menuCol.implicitWidth + Theme.sp(24))
        implicitHeight: menuCol.implicitHeight + Theme.sp(12)
        width:          implicitWidth
        height:         implicitHeight
        x: Math.max(0, Math.min(Math.max(0, root.width  - width),  nodeMenu.wantX))
        y: Math.max(0, Math.min(Math.max(0, root.height - height), nodeMenu.wantY))
        radius:         Theme.radius
        color:          Theme.colorBg3
        border.width:   1
        border.color:   Theme.colorBorderStrong

        // A ColumnLayout, NOT a Column. A Column sizes itself from its children's actual WIDTH, so
        // rows sized from the menu and a menu sized from the rows is a polish loop — Qt says
        // "Column called polish() inside updatePolish()" and the frame never settles. A layout
        // reads implicit sizes, which breaks the cycle: the rows state how wide they would like to
        // be, the menu takes the widest, and only then are they stretched to fill it.
        ColumnLayout {
            id: menuCol
            x: Theme.sp(6)
            y: Theme.sp(6)
            width:   nodeMenu.width - Theme.sp(12)
            spacing: 0

            Repeater {
                model: nodeMenu.items
                delegate: Rectangle {
                    id: menuRow
                    required property var modelData

                    Layout.fillWidth: true
                    implicitWidth:  menuRowText.implicitWidth + Theme.sp(20)
                    implicitHeight: Theme.sp(30)
                    radius: Theme.radius
                    color:  rowMa.containsMouse ? Theme.colorBg2 : "transparent"

                    Text {
                        id: menuRowText
                        anchors.left:           parent.left
                        anchors.leftMargin:     Theme.sp(10)
                        anchors.right:          parent.right
                        anchors.rightMargin:    Theme.sp(10)
                        anchors.verticalCenter: parent.verticalCenter
                        text:           menuRow.modelData.label
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        // Removing a link is a write to the user's pack and is styled as one.
                        color:          menuRow.modelData.destructive ? Theme.colorError
                                                                      : Theme.colorText2
                        elide:          Text.ElideRight
                    }

                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape:  Qt.PointingHandCursor
                        onClicked:    root._menuAction(menuRow.modelData.action)
                    }
                }
            }
        }
    }

    PpToast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom:           parent.bottom
        z: 11
        glyph: "◇"
        showUndo: false                        // set per notice by _runMenuAction
    }

    // The undo handler CANNOT be written inside the PpToast block above. PpToast declares its own
    // `id: root`, which shadows this file's — the same trap that threw ReferenceError at stage 7,
    // and one that only ever surfaces on a click, so no binding, test or screenshot would show it. A
    // Connections block declares no `root` of its own, so `root` here is this file's.
    Connections {
        target: toast
        function onUndoClicked() { root._undoUnlink() }
    }
}
