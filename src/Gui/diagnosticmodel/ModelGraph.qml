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
// EVERY coordinate comes from ModelBrowser.dag(), which is dag_layout.h. QML positions nothing: rank
// assignment, ordering, routing and overlap are the only things about this surface that can be
// tested, and a layout computed inside delegate bindings is a layout nothing can assert. What this
// file does is draw, hit-test and drag.
Item {
    id: root

    property var    layoutData: ({})     // from ModelBrowser.dag()
    property string focusId:    ""
    property string selectedEdgeId: ""
    property bool   editable:   true

    // The node's own TYPE travels with it. Without it the receiver has to assume one, and the
    // measures lane is full of nodes that are not conditions — clicking one then asked for the
    // causal graph of a condition id that does not exist, and the graph went blank.
    signal nodeActivated(string nodeType, string id)
    signal edgeActivated(string rowId)
    signal linkRequested(string fromId, string toId)
    // Asked DURING a drag, not on release: { ok, reason }. A refusal has to be visible while the
    // pointer is still moving, or the author has already committed to the gesture.
    property var legalityProbe: null

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

    // The layout centres itself on the focus rather than on the origin. layoutDag() reports where
    // the focus landed for exactly this reason: a graph wider than its pane that opens at 0,0 shows
    // whatever happens to be top-left, which is rarely the condition you asked about.
    // ── Drag state ────────────────────────────────────────────────────────────
    property string _dragFrom:   ""
    property real   _dragX:      0
    property real   _dragY:      0
    property string _dragTarget: ""
    property bool   _dragLegal:  false
    property string _dragReason: ""

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

    function _nodeAt(x, y) {
        for (var i = 0; i < _nodes.length; i++) {
            var n = _nodes[i]
            if (n.kind === "measure") continue
            if (x >= n.x && x <= n.x + n.w && y >= n.y && y <= n.y + n.h) return n
        }
        return null
    }

    Flickable {
        id: canvas
        anchors.fill: parent
        contentWidth:  content.width
        contentHeight: content.height
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical:   ScrollBar { policy: ScrollBar.AsNeeded }
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

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
                    modelData.rowId !== undefined && modelData.rowId === root.selectedEdgeId
                // The selected edge must be identifiable ON THE CANVAS: drawn heavier, with the
                // rest muted. A selection that only shows in the inspector is a selection the
                // reader has to hold in their head.
                readonly property real muting: root.selectedEdgeId !== "" && !selected ? 0.28 : 1.0

                ShapePath {
                    strokeColor: edgeShape.modelData.detects
                                     ? Theme.colorText3
                                     : (edgeShape.selected ? Theme.colorAccent : Theme.colorBorderStrong)
                    strokeWidth: edgeShape.selected ? 2 : Math.max(1, edgeShape.modelData.weight || 1)
                    fillColor:   "transparent"
                    // A detection line is not a causal claim, so it must not look like one.
                    strokeStyle: edgeShape.modelData.detects ? ShapePath.DashLine : ShapePath.SolidLine
                    dashPattern: [ 3, 3 ]
                    capStyle:    ShapePath.RoundCap

                    startX: edgeShape.modelData.x1
                    startY: edgeShape.modelData.y1
                    PathCubic {
                        x:         edgeShape.modelData.x2
                        y:         edgeShape.modelData.y2
                        control1X: edgeShape.modelData.c1x
                        control1Y: edgeShape.modelData.c1y
                        control2X: edgeShape.modelData.c2x
                        control2Y: edgeShape.modelData.c2y
                    }
                }

                // The arrow head, its three points supplied by the layout — a head computed here
                // would point somewhere slightly different from the curve it caps.
                ShapePath {
                    fillColor:   edgeShape.selected ? Theme.colorAccent : Theme.colorBorderStrong
                    strokeWidth: 0
                    strokeColor: "transparent"
                    // Symmetric relations carry no direction, so they carry no head.
                    startX: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                                ? edgeShape.modelData.tipAx : 0
                    startY: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                                ? edgeShape.modelData.tipAy : 0
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipBx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipBy : 0
                    }
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipCx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipCy : 0
                    }
                    PathLine {
                        x: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipAx : 0
                        y: edgeShape.modelData.tip && !edgeShape.modelData.symmetric
                               ? edgeShape.modelData.tipAy : 0
                    }
                }

                opacity: muting
                Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
            }
        }

        // Edge hit targets. A curve is unclickable at 1px, so each one gets a small box at its
        // label point — which is the part of the line a reader is already looking at.
        Repeater {
            model: root._edges
            delegate: Item {
                id: edgeHit
                required property var modelData

                visible: modelData.rowId !== undefined
                x: (modelData.labelX || 0) - Theme.sp(16)
                y: (modelData.labelY || 0) - Theme.sp(9)
                width:  Theme.sp(32)
                height: Theme.sp(18)
                z: 4

                Rectangle {
                    anchors.fill: parent
                    radius:  Theme.radius
                    color:   Theme.colorBg
                    visible: edgeHit.modelData.label !== "" || edgeHover.hovered
                    opacity: 0.9
                }

                Text {
                    anchors.centerIn: parent
                    text: edgeHit.modelData.label || ""
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: edgeHit.modelData.rowId === root.selectedEdgeId
                               ? Theme.colorAccent : Theme.colorText3
                }

                HoverHandler { id: edgeHover }
                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root.edgeActivated(edgeHit.modelData.rowId)
                }
            }
        }

        // ── Nodes ─────────────────────────────────────────────────────────────
        Repeater {
            model: root._nodes
            delegate: Item {
                id: nodeItem
                required property var modelData

                x: modelData.x
                y: modelData.y
                width:  modelData.w
                height: modelData.h
                z: 2

                readonly property bool isFocus:   modelData.kind === "focus"
                readonly property bool isMeasure: modelData.kind === "measure"

                readonly property color tint: root._typeColor(nodeItem.modelData.nodeType)

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius
                    // Tinted by type, faintly — enough to tell a measure from a characteristic at a
                    // glance without the colour becoming the subject. Latent conditions stay an
                    // outline, so a reader can still tell what the app SAW from what it worked out.
                    color: nodeItem.modelData.latent
                               ? "transparent"
                               : Qt.rgba(nodeItem.tint.r, nodeItem.tint.g, nodeItem.tint.b,
                                         nodeItem.isFocus ? 0.22 : 0.10)
                    border.width: nodeItem.isFocus ? 2 : 1
                    border.color: nodeItem.isFocus               ? nodeItem.tint
                                : nodeItem.modelData.dirty       ? Theme.colorAccent
                                : nodeItem.modelData.offeredOnly ? Theme.colorBorderMid
                                                                 : nodeItem.tint
                    // An offered-only condition may never be concluded, so it has to look different
                    // everywhere it appears, including here.
                    opacity: nodeItem.modelData.available ? 1.0 : 0.55
                }

                RowLayout {
                    anchors.fill: parent
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
                            font.pixelSize: nodeItem.isMeasure ? Theme.fontSzMicro
                                                               : Theme.fontSzBody2
                            font.weight:    Theme.fontBodyWeight
                            color: nodeItem.modelData.available ? Theme.colorText : Theme.colorText3
                            elide: Text.ElideRight
                        }

                        // The one line the node can say about itself — a measure's corridor, a
                        // relation's kind. Empty for anything with nothing to add.
                        Text {
                            Layout.fillWidth: true
                            text:    nodeItem.modelData.note || nodeItem.modelData.statusLabel || ""
                            visible: text.length > 0
                            font.family:    Theme.fontData
                            font.pixelSize: Theme.fontSzMicro
                            color:          Theme.colorText3
                            elide:          Text.ElideRight
                        }
                    }
                }

                // Whatever the depth bound cut off is COUNTED on the node it was cut from. A graph
                // that silently omitted half of what it knows is worse than one that drew nothing.
                Text {
                    anchors.right: parent.left
                    anchors.rightMargin: Theme.sp(3)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: (nodeItem.modelData.hiddenCauses || 0) > 0
                    text:    "+" + nodeItem.modelData.hiddenCauses
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }
                Text {
                    anchors.left: parent.right
                    anchors.leftMargin: Theme.sp(3)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: (nodeItem.modelData.hiddenEffects || 0) > 0
                    text:    "+" + nodeItem.modelData.hiddenEffects
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }

                PpPressable {
                    hoverScale: 1.0
                    onClicked:  root.nodeActivated(nodeItem.modelData.nodeType || "",
                                                   nodeItem.modelData.id)
                }

                // ── The drag handle ───────────────────────────────────────────
                // Drag from a node's TRAILING edge onto another to add a cause. On the trailing
                // edge because that is the direction the graph reads: left-to-right is "causes".
                Rectangle {
                    id: handle
                    visible: root.editable && !nodeItem.isMeasure
                             && (nodeHover.hovered || root._dragFrom === nodeItem.modelData.id)
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.right: parent.right
                    anchors.rightMargin: -Theme.sp(4)
                    width:  Theme.sp(9)
                    height: Theme.sp(9)
                    radius: width / 2
                    color:  Theme.colorAccent
                    z: 5

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -Theme.sp(4)
                        cursorShape: Qt.CrossCursor
                        onPressed: {
                            root._dragFrom = nodeItem.modelData.id
                            root._dragX = nodeItem.x + nodeItem.width
                            root._dragY = nodeItem.y + nodeItem.height / 2
                        }
                        onPositionChanged: (mouse) => {
                            var p = mapToItem(inner, mouse.x, mouse.y)
                            root._dragX = p.x
                            root._dragY = p.y
                            var over = root._nodeAt(p.x, p.y)
                            var id   = over && over.id !== root._dragFrom ? over.id : ""
                            if (id !== root._dragTarget) {
                                root._dragTarget = id
                                // The rules are asked of C++ while the pointer is still moving, so
                                // an illegal target refuses DURING the drag with the reason stated
                                // — not on release, by which point the gesture is already made.
                                if (id !== "" && root.legalityProbe) {
                                    var v = root.legalityProbe(root._dragFrom, id)
                                    root._dragLegal  = v.ok === true
                                    root._dragReason = v.reason || ""
                                } else {
                                    root._dragLegal  = false
                                    root._dragReason = ""
                                }
                            }
                        }
                        onReleased: {
                            if (root._dragTarget !== "" && root._dragLegal)
                                root.linkRequested(root._dragFrom, root._dragTarget)
                            root._dragFrom = ""
                            root._dragTarget = ""
                            root._dragReason = ""
                        }
                    }
                }

                HoverHandler { id: nodeHover }

                // The drop target reads as legal or not while the pointer is over it.
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    visible: root._dragTarget === nodeItem.modelData.id
                    radius: Theme.radius
                    color:  "transparent"
                    border.width: 2
                    border.color: root._dragLegal ? Theme.colorGood : Theme.colorError
                    z: 6
                }
            }
        }

        // The rubber-band line, and the refusal beside it.
        Shape {
            visible: root._dragFrom !== ""
            anchors.fill: parent
            z: 7
            ShapePath {
                strokeColor: root._dragTarget === "" ? Theme.colorAccent
                           : root._dragLegal         ? Theme.colorGood
                                                     : Theme.colorError
                strokeWidth: 2
                fillColor:   "transparent"
                strokeStyle: ShapePath.DashLine
                dashPattern: [ 4, 3 ]
                startX: {
                    for (var i = 0; i < root._nodes.length; i++)
                        if (root._nodes[i].id === root._dragFrom)
                            return root._nodes[i].x + root._nodes[i].w
                    return 0
                }
                startY: {
                    for (var i = 0; i < root._nodes.length; i++)
                        if (root._nodes[i].id === root._dragFrom)
                            return root._nodes[i].y + root._nodes[i].h / 2
                    return 0
                }
                PathLine { x: root._dragX; y: root._dragY }
            }
        }
        }
        }
    }

    // The reason, stated in the author's own terms, while the drag is still live.
    Rectangle {
        visible: root._dragFrom !== "" && root._dragTarget !== "" && !root._dragLegal
                 && root._dragReason !== ""
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
            text:  root._dragReason
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorError
            wrapMode:       Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // ── Scope ─────────────────────────────────────────────────────────────────
    //
    // Overlaid on the picture rather than parked under it: it changes what you are looking at, so it
    // belongs where you are looking.
    property int  scope: 2
    property bool includeMeasures: false
    property bool hideWeak: false
    property bool hideProposed: false

    signal scopeRequested(int scope)
    signal switchToggled(string which)

    Rectangle {
        anchors.top:   parent.top
        anchors.right: parent.right
        anchors.margins: Theme.sp(10)
        visible: root._nodes.length > 0
        width:   scopeRow.implicitWidth + Theme.sp(18)
        height:  Theme.sp(26)
        radius:  height / 2
        color:   Theme.colorSurface
        border.width: 1
        border.color: Theme.colorBorderMid
        z: 30

        RowLayout {
            id: scopeRow
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

            Text {
                text: "−"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzBody
                color: root.scope > 1 ? Theme.colorText2 : Theme.colorText3
                PpPressable {
                    hoverScale: 1.0
                    enabled:    root.scope > 1
                    onClicked:  root.scopeRequested(root.scope - 1)
                }
            }
            Text {
                text: qsTr("scope %1").arg(root.scope)
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText2
            }
            Text {
                text: "+"
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzBody
                color: root.scope < 4 ? Theme.colorText2 : Theme.colorText3
                PpPressable {
                    hoverScale: 1.0
                    enabled:    root.scope < 4
                    onClicked:  root.scopeRequested(root.scope + 1)
                }
            }

            Rectangle {
                Layout.preferredWidth:  1
                Layout.preferredHeight: Theme.sp(12)
                color: Theme.colorBorderMid
            }

            Switch { label: qsTr("measures"); on: root.includeMeasures; key: "measures" }
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
