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

// ONE CHAIN, LAID OUT. Nodes and the graded columns between them, left to right on the wide
// stage and top to bottom in the 396 arrangement.
//
// IT DRAWS WHAT THE MODEL AUTHORED AND NOTHING ELSE (brief §5.3). There is no branch in here
// that joins two nodes because they happen to be adjacent: an edge is drawn only where
// buildChains() published a link whose `from` is this node, so a chain of four cards with
// three links draws three, and a chain the model gave two links draws two and leaves the
// third pair apart. The links list is keyed by id rather than by position for exactly that
// reason — it is shorter than the node list whenever a node has no outgoing edge, and
// walking it by index would silently shift every edge one node to the left.
//
// THE VERTICAL FORM IS THE SAME RAIL, NOT A SUMMARY OF IT (brief §8). Same nodes, same
// links, same order, same grades — each node collapsed to one line and each link to a short
// bar. That is what lets chain B's collapsed row in the narrow arrangement expand INTO this
// rather than into some other view.

import QtQuick
import PinPointStudio

Item {
    id: root

    // One entry of SessionDiagnosticsModel::chains().
    property var chain: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // 12c: one line per node, short vertical link bars.
    property bool vertical: false
    // driver.screenConditionId / driver.screenRef — the only place the published surface
    // carries a screen ref for a screened-root node.
    property string screenConditionId: ""
    property string screenRef: ""

    // Off on the auto-closing cast (PpSessionDiagnosticsWindow.interactive).
    property bool interactive: true
    // The body's after-shot cue, passed straight through. See PpChainNodeCard.
    property var pulseCue: null

    signal screenRequested(string screenRef, string conditionId)
    signal focusToggled(string conditionId, bool nowFocused)

    objectName: "sdChainRail"

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property var nodes: chain ? (chain.nodes || []) : []
    readonly property var links: chain ? (chain.links || []) : []

    // The edge LEAVING this node, or null. See the header: by id, never by index.
    function linkFrom(id) {
        for (let i = 0; i < links.length; ++i)
            if (links[i].from === id)
                return links[i]
        return null
    }

    readonly property int nodeCount: nodes.length
    readonly property int linkCount: {
        let n = 0
        for (let i = 0; i < nodeCount; ++i)
            if (linkFrom(nodes[i].id))
                ++n
        return n
    }

    // ── wide: cards and link columns interleaved ─────────────────────────────
    // The link columns are a fixed share and the nodes divide what is left, so a five-node
    // chain draws narrower cards rather than three cards and a cliff.
    readonly property int _linkW: px(84)
    readonly property int _nodeW: nodeCount > 0
                                  ? Math.max(px(60),
                                             Math.floor((width - linkCount * _linkW) / nodeCount))
                                  : 0

    readonly property int _slimNodeH: px(36)
    readonly property int _slimLinkH: px(20)
    // What the vertical form needs; the panel gives it a Flickable when the panel has less.
    implicitHeight: vertical
                    ? nodeCount * _slimNodeH + linkCount * _slimLinkH
                    : px(150)

    Row {
        id: wideRow
        anchors.fill: parent
        visible: !root.vertical
        spacing: 0

        Repeater {
            model: root.vertical ? [] : root.nodes

            Row {
                id: wideNode
                required property var modelData
                spacing: 0
                height: wideRow.height

                readonly property var _link: root.linkFrom(modelData.id)

                Item {
                    width: root._nodeW
                    height: wideRow.height

                    PpChainNodeCard {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        // A ghost is CENTRED at its own height rather than stretched: an
                        // outline the full height of a live card reads as a card with
                        // nothing in it, which is a different claim from "nobody measured
                        // this".
                        height: node && node.kind === "ghost"
                                ? Math.min(contentHeight, parent.height)
                                : parent.height
                        node: wideNode.modelData
                        fit:  root.fit
                        interactive: root.interactive
                        pulseCue: root.pulseCue
                        screenRef: (wideNode.modelData.id === root.screenConditionId)
                                   ? root.screenRef : ""
                        onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                        onFocusToggled: (id, on) => root.focusToggled(id, on)
                    }
                }

                PpChainLink {
                    width:  wideNode._link ? root._linkW : 0
                    height: wideRow.height
                    visible: wideNode._link !== null
                    link: wideNode._link
                    fit:  root.fit
                }
            }
        }
    }

    // ── 12c: one line per node ───────────────────────────────────────────────
    Column {
        id: slimCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: root.vertical
        spacing: 0

        Repeater {
            model: root.vertical ? root.nodes : []

            Column {
                id: slimNode
                required property var modelData
                width: slimCol.width
                spacing: 0

                readonly property var _link: root.linkFrom(modelData.id)

                PpChainNodeCard {
                    width:  slimNode.width
                    height: root._slimNodeH
                    slim: true
                    node: slimNode.modelData
                    fit:  root.fit
                    interactive: root.interactive
                    pulseCue: root.pulseCue
                    screenRef: (slimNode.modelData.id === root.screenConditionId)
                               ? root.screenRef : ""
                    onScreenRequested: (ref, cond) => root.screenRequested(ref, cond)
                    onFocusToggled: (id, on) => root.focusToggled(id, on)
                }

                PpChainLink {
                    width:  slimNode.width
                    height: slimNode._link ? root._slimLinkH : 0
                    visible: slimNode._link !== null
                    vertical: true
                    link: slimNode._link
                    fit:  root.fit
                }
            }
        }
    }
}
