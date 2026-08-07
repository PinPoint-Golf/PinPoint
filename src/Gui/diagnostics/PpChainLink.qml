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

// ONE EDGE OF THE CHAIN, drawn as the strongest claim its evidence supports.
//
// THE STROKE IS THE CLAIM, NOT DECORATION (brief §4.1). Two things arrive at a heavier line
// and an arrowhead — a pair that MOVED TOGETHER across a declared change, and a pair the
// Fisher test found dependent — and everything weaker gets a line that visibly cannot carry
// an arrow: dashed for coherent (nothing varied, so nothing could be tested), dotted for
// present-together (neither endpoint was measurable), dotted at half opacity for unanchored
// (the root has not been screened). That is why the 396 arrangement can throw away the note
// and keep the meaning: the grade rides on the stroke, so it survives the collapse.
//
// THE WORD AND THE STROKE COME FROM THE SAME PLACE. `word`, `stroke`, `width`, `arrow` and
// `opacity` are all published together by SessionDiagnosticsModel::buildChains() off one
// table, so this file cannot draw "Coherent" with an arrowhead however it is used. It maps
// the stroke NAME to a theme colour and a dash pattern and paints; it decides nothing.
//
// Canvas rather than Shape, and rather than a stack of Rectangles: dotted, dashed and solid
// at three different widths plus a triangle is one path each way, and QtQuick.Shapes is a
// plugin the offscreen QML test target does not link. Same reasoning as PpDashedFrame.

import QtQuick
import PinPointStudio

Item {
    id: root

    // One entry of a SessionDiagnosticsModel::chains() entry's `links`.
    property var link: null
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0
    // 12c turns the rail on its side: the stroke becomes a short vertical bar.
    property bool vertical: false

    objectName: "sdChainLink"

    // The node beside it is overflow:hidden and so is this. A link column is centred on its
    // row, so when the row is shorter than the word-stroke-note stack the stack hangs out of
    // BOTH ends and lands on the node cards either side — the loose "Unanchored / not
    // established / screen would confirm" fragments floating over a squeezed rail were this and
    // nothing else. Clipped, the same overflow can only ever cost the caption.
    clip: true

    function px(n) { return Math.round(n * Theme.fontScale * root.fit) }

    readonly property int tzCaption: Math.max(1, Math.round(Theme.sp(8) * fit))

    readonly property string grade:  link ? (link.grade || "") : ""
    readonly property string word:   link ? (link.word || "")  : ""
    readonly property string note:   link ? (link.note || "")  : ""
    readonly property string stroke: link ? (link.stroke || "dotted") : "dotted"
    readonly property bool hasArrow: link ? link.arrow === true : false

    // Exposed so the geometry is assertable: a Canvas paints, and what it painted cannot be
    // read back out of it. These five ARE the §4.1 table's right-hand column.
    readonly property color strokeColor: stroke === "solidAccent" ? Theme.colorAccent
                                       : stroke === "solid"       ? Theme.colorText2
                                       : stroke === "dashed"      ? Theme.colorText2
                                                                  : Theme.colorText3
    readonly property string strokeStyle: (stroke === "solidAccent" || stroke === "solid")
                                          ? "solid"
                                          : (stroke === "dashed" ? "dashed" : "dotted")
    readonly property real strokeWidth: Math.max(1, (link ? (link.width || 1) : 1)
                                                    * Theme.fontScale * root.fit)
    // The stroke's own row height in the wide column — the one thing that is never dropped.
    readonly property int _strokeH: Math.round(Math.max(strokeWidth, px(7)))

    // The half-opacity of an unanchored link is the model's, not a style choice here: a link
    // whose root has never been screened is drawn as barely there because that is what the
    // evidence is.
    opacity: link ? (link.opacity !== undefined ? link.opacity : 1.0) : 1.0

    // A Canvas repaints on resize on its own; a change of GRADE is a change of paint with no
    // change of geometry, so it has to be asked for.
    function _repaint() { horizStroke.requestPaint(); vertStroke.requestPaint() }
    onStrokeColorChanged: _repaint()
    onStrokeStyleChanged: _repaint()
    onStrokeWidthChanged: _repaint()
    onHasArrowChanged:    _repaint()

    // ── the wide rail's link column ──────────────────────────────────────────
    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin:  root.px(5)
        anchors.rightMargin: root.px(5)
        anchors.verticalCenter: parent.verticalCenter
        visible: !root.vertical
        spacing: root.px(5)

        // THE STROKE IS THE GRADE; the word and the note are the grade said twice more. When
        // the row is too short for all three, the captions go and the stroke stays — dropped
        // whole rather than clipped mid-word, for the same reason the node card drops its
        // evidence sentence rather than shrinking it. Measured off the Texts' own implicit
        // heights, which do not depend on whether they are shown, so this cannot chase itself.
        Text {
            id: linkWord
            objectName: "sdChainLinkWord"
            width: parent.width
            visible: root.height >= implicitHeight + root._strokeH + root.px(5)
            text: root.word
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.3
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            font.letterSpacing: Theme.trackingLabel
            color: root.strokeColor
        }

        Canvas {
            id: horizStroke
            objectName: "sdChainLinkStroke"
            width: parent.width
            height: root._strokeH

            onWidthChanged:  requestPaint()
            onHeightChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (width <= 0 || height <= 0)
                    return
                const y = Math.round(height / 2)
                const headW = root.hasArrow ? root.px(5) : 0
                const headH = root.px(4)

                ctx.strokeStyle = root.strokeColor
                ctx.lineWidth   = root.strokeWidth
                ctx.setLineDash(root.strokeStyle === "dashed"
                                ? [root.px(4), root.px(3)]
                                : root.strokeStyle === "dotted"
                                  ? [Math.max(1, root.px(2)), root.px(3)]
                                  : [])
                ctx.beginPath()
                ctx.moveTo(0, y)
                ctx.lineTo(Math.max(0, width - headW), y)
                ctx.stroke()

                if (headW <= 0)
                    return
                ctx.setLineDash([])
                ctx.fillStyle = root.strokeColor
                ctx.beginPath()
                ctx.moveTo(width, y)
                ctx.lineTo(width - headW, y - headH)
                ctx.lineTo(width - headW, y + headH)
                ctx.closePath()
                ctx.fill()
            }
        }

        Text {
            id: linkNote
            objectName: "sdChainLinkNote"
            width: parent.width
            visible: text !== ""
                     && root.height >= linkWord.implicitHeight + implicitHeight
                                       + root._strokeH + 2 * root.px(5)
            text: root.note
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            lineHeight: 1.35
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: Theme.colorText3
        }
    }

    // ── 12c: the same edge on its side ───────────────────────────────────────
    // The bar keeps the grade because the grade is the stroke. The word and the note ride
    // beside it on one line rather than under it, so a node-link-node sequence still reads
    // top to bottom in a 396 px column.
    Row {
        anchors.fill: parent
        anchors.leftMargin: root.px(14)
        visible: root.vertical
        spacing: root.px(7)

        Canvas {
            id: vertStroke
            objectName: "sdChainLinkStroke"
            width: Math.max(root.strokeWidth, root.px(6))
            height: parent.height

            onWidthChanged:  requestPaint()
            onHeightChanged: requestPaint()

            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                if (width <= 0 || height <= 0)
                    return
                const x = Math.round(width / 2)
                const headH = root.hasArrow ? root.px(5) : 0
                const headW = root.px(4)

                ctx.strokeStyle = root.strokeColor
                ctx.lineWidth   = root.strokeWidth
                ctx.setLineDash(root.strokeStyle === "dashed"
                                ? [root.px(4), root.px(3)]
                                : root.strokeStyle === "dotted"
                                  ? [Math.max(1, root.px(2)), root.px(3)]
                                  : [])
                ctx.beginPath()
                ctx.moveTo(x, 0)
                ctx.lineTo(x, Math.max(0, height - headH))
                ctx.stroke()

                if (headH <= 0)
                    return
                ctx.setLineDash([])
                ctx.fillStyle = root.strokeColor
                ctx.beginPath()
                ctx.moveTo(x, height)
                ctx.lineTo(x - headW, height - headH)
                ctx.lineTo(x + headW, height - headH)
                ctx.closePath()
                ctx.fill()
            }
        }

        Text {
            id: vertWord
            objectName: "sdChainLinkWord"
            anchors.verticalCenter: parent.verticalCenter
            text: root.word
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            font.letterSpacing: Theme.trackingLabel
            color: root.strokeColor
        }
        Text {
            objectName: "sdChainLinkNote"
            anchors.verticalCenter: parent.verticalCenter
            // Measured off the siblings, never off this item's own x — a Row sets x from the
            // widths it is given, so reading x back into a width is a loop.
            width: Math.max(0, parent.width - vertStroke.width - vertWord.width
                               - 2 * parent.spacing)
            visible: text !== ""
            text: root.note
            elide: Text.ElideRight
            font.family: Theme.fontData
            font.pixelSize: root.tzCaption
            color: Theme.colorText3
        }
    }
}
