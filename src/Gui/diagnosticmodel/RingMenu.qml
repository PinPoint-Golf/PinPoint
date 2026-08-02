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
// Controls.Basic for Popup — the ring is a popup like every other transient surface in this panel,
// so it closes on Escape and on a press outside by the same rules as the pickers.
import QtQuick.Controls.Basic
import PinPointStudio

// The press-and-hold ring. ONE component for every selection type — a node, a link, a multi-select
// and bare canvas differ only by the array passed in, never by a subclass, because the whole promise
// of the shape is that the silhouette is identical every time it opens.
//
// ── The eight slots are fixed by MEANING ────────────────────────────────────────────────────────
//
// Clockwise from north the ring reads look · make · set · unmake, and a direction means the same
// thing whatever is selected. `model` is therefore EXACTLY EIGHT entries, index 0 = N and rising
// clockwise, with `null` for a slot this selection does not fill. A gap is a hairline, never a
// greyed-out button and never a live spoke moved up to close it: muscle memory has to survive a
// change of selection, so the only variable between one opening and the next is how many wedges are
// lit. `enabled: false` is treated as a gap for the same reason — there is no disabled state here.
//
// Each entry: { icon, label, hint, kind, enabled, danger, values[] }
//   kind    "action"  commits on release
//           "drag"    commits by handing off to a drag with the button still down (§5)
//           "value"   carries `▸` and opens the collar
//   hint    the spoken label — what the hub says while this spoke is hot, and the ONLY place the
//           ring explains itself. It is why the wedge itself can stay icon-plus-short-label.
//   values  [{ value, label, current }] for a `value` spoke. Three or fewer, always.
//
// ── This component reads no input of its own while the button is held ───────────────────────────
//
// The gesture starts on the canvas and the button never comes up, so the press is already grabbed by
// the time the ring exists. The canvas feeds track()/release() instead. A latched ring — the
// right-click entrance, where no button is down — puts up its own tracking area, which is the one
// case where it can.
Popup {
    id: root

    // Index 0 = N, rising clockwise. Length is asserted rather than tolerated.
    property var model: []

    // The hub's two lines. `hubMeta` is replaced by the hot spoke's `hint` while one is hot.
    property string hubTitle: ""
    property string hubMeta:  ""

    // Right-click: the ring stays up with nothing held, and a click commits. Everything else about
    // it is identical, which is the point — §7's first discoverability measure is worthless if the
    // two entrances open different menus.
    property bool latched: false

    // Once per session, not once per ring: the first hold shows every spoke's spoken label for a
    // beat before settling. Long enough to read the ring, rare enough not to become a delay.
    property bool firstOfSession: false

    // Where the ring is centred, in the PARENT's coordinates. Set through openAt(), which clamps it.
    property real centreX: 0
    property real centreY: 0

    // `value` is the collar cell chosen, or "" for an action or drag spoke.
    signal committed(int slot, var entry, string value)
    signal cancelled()

    // ── Geometry ──────────────────────────────────────────────────────────────
    //
    // The mockup's proportions, not its pixels — every one through Theme.sp(), so the ring grows
    // with the rest of the type. hub 52 · rim 126 · collar +46 · 3° between wedges.
    readonly property real _r0:     Theme.sp(52)
    readonly property real _r1:     Theme.sp(126)
    readonly property real _collar: Theme.sp(46)
    readonly property real _outer:  _r1 + _collar
    readonly property real _gapDeg: 3
    readonly property real _sweep:  45 - _gapDeg     // 42° of wedge, 3° of air

    // ── Where the cursor is, in ring-centred coordinates ──────────────────────
    property real _dx: 0
    property real _dy: 0
    // Set once the cursor has been past the rim on a value spoke. Sticky for the life of that
    // spoke's collar: crossing back inside the rim must not snap the collar shut mid-choice, or a
    // hand that wobbles loses the menu it is halfway through reading.
    property bool _inCollar: false
    property bool _verbose:  false

    readonly property real _dist: Math.sqrt(_dx * _dx + _dy * _dy)
    // Screen coordinates put +y downward, so north is -90° and the slots rise clockwise from it.
    readonly property real _angDeg: Math.atan2(_dy, _dx) * 180 / Math.PI

    // Which slot the cursor points at — angle arithmetic snapped to 45°, with a dead zone inside the
    // hub. Not a HoverHandler per wedge: those fight for the grab during a held drag, which is the
    // only way this menu is ever driven.
    readonly property int _hotSlot: {
        if (_dist < _r0) return -1                        // the hub: nothing is hot
        if (_dist > _outer) return -1                     // flung past everything
        if (_dist > _r1 && !_inCollar) return -1          // past the rim on a spoke with no collar
        var s = Math.round((_angDeg + 90) / 45)
        s = ((s % 8) + 8) % 8
        return root._entryAt(s) ? s : -1
    }

    readonly property var _hotEntry: _hotSlot >= 0 ? root._entryAt(_hotSlot) : null

    // Which of the collar's three cells, or -1 when the collar is not open. The cells divide the
    // spoke's own arc, so the choice is made in the direction the hand is already travelling.
    readonly property int _hotCell: {
        if (!_inCollar || _hotSlot < 0) return -1
        var e = root._hotEntry
        if (!e || !e.values || e.values.length === 0) return -1
        if (_dist <= _r1 || _dist > _outer) return -1
        var centre = -90 + _hotSlot * 45
        var off    = root._norm(_angDeg - centre) + _sweep / 2      // 0 … _sweep across the wedge
        var n      = Math.min(3, e.values.length)
        var c      = Math.floor(off / (_sweep / n))
        return Math.max(0, Math.min(n - 1, c))
    }

    // ── Popup plumbing ────────────────────────────────────────────────────────
    x: centreX - _outer
    y: centreY - _outer
    width:   _outer * 2
    height:  _outer * 2
    padding: 0
    modal:   false
    // Never the whole app's overlay: parented to the canvas by its caller, so it cannot escape the
    // pane the way the old status bar did.
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    background: null

    onClosed: { _inCollar = false; _verbose = false }
    // Esc reaches here through closePolicy. Whichever way it closed without a commit is a cancel,
    // and a cancelled gesture is not an error — nothing is said about it anywhere.
    onAboutToHide: if (!_committing) root.cancelled()
    property bool _committing: false

    // Open centred on the press point, CLAMPED so the rim and the collar stay on the canvas. The
    // centre moves rather than the ring being cropped: a ring with a slot off the edge would have a
    // different silhouette, which is the one thing it may never have.
    function openAt(px, py, entries, title, meta, isLatched) {
        root.model    = entries
        root.hubTitle = title
        root.hubMeta  = meta
        root.latched  = isLatched === true

        var lim = root._outer
        var pw  = root.parent ? root.parent.width  : 0
        var ph  = root.parent ? root.parent.height : 0
        root.centreX = pw > lim * 2 ? Math.max(lim, Math.min(pw - lim, px)) : pw / 2
        root.centreY = ph > lim * 2 ? Math.max(lim, Math.min(ph - lim, py)) : ph / 2

        root._dx = 0
        root._dy = 0
        root._inCollar  = false
        root._committing = false
        root._verbose   = root.firstOfSession
        if (root.firstOfSession) settleTimer.restart()
        root.open()
    }

    // Fed by the canvas while the button is held. Coordinates are the PARENT's, the same space the
    // centre is in, so the caller never has to know where the popup put itself.
    function track(px, py) {
        root._dx = px - root.centreX
        root._dy = py - root.centreY
        // Latches on once, and only for a spoke that has a collar to open.
        if (!root._inCollar && root._dist > root._r1) {
            var s = ((Math.round((root._angDeg + 90) / 45) % 8) + 8) % 8
            var e = root._entryAt(s)
            if (e && e.values && e.values.length > 0) root._inCollar = true
        }
    }

    // Release, from the canvas. Commits the hot spoke, or cancels — inside the hub, past the rim, or
    // on a gap. Nothing is ever committed from the centre.
    function release() {
        var e = root._hotEntry
        if (!e) { root.close(); return }
        if (e.values && e.values.length > 0) {
            if (root._hotCell < 0) { root.close(); return }
            var v = e.values[root._hotCell]
            // Releasing on the value it already has changes nothing and says nothing. The collar's
            // whole safety property is that a hold with no travel cannot alter anything, and this is
            // where that is enforced — rather than by reordering the cells, which would break the
            // reading of a ladder like sometimes · often · usually.
            if (v.current === true) { root.close(); return }
            root._committing = true
            root.committed(root._hotSlot, e, v.value)
            root.close()
            return
        }
        root._committing = true
        root.committed(root._hotSlot, e, "")
        root.close()
    }

    function _entryAt(i) {
        if (!root.model || i < 0 || i >= root.model.length) return null
        var e = root.model[i]
        // A gap and a refusal are the same thing here: there is no disabled state in this menu.
        if (!e || e.enabled === false) return null
        return e
    }

    // Degrees folded into (-180, 180], so an arc that straddles due west compares correctly.
    function _norm(a) {
        var x = a
        while (x <= -180) x += 360
        while (x > 180)   x -= 360
        return x
    }

    Timer {
        id: settleTimer
        interval: 600
        onTriggered: root._verbose = false
    }

    // ── Nothing in this menu is translucent ───────────────────────────────────
    //
    // The ring floats over a drawing of boxes and curves, and a wedge you can see the graph through
    // is a wedge you cannot read. Worse, the hot state used `colorAccentLight` — an OVERLAY tint at
    // roughly 7% alpha, correct on a solid pane and nearly invisible over a picture — so pointing at
    // a spoke made it MORE transparent than the seven it was competing with.
    //
    // These mix the same two theme colours to a solid one instead of laying one over the other. No
    // colour is invented: `t` is how far from the surface towards the accent, and the result is
    // opaque whatever is behind it.
    function _solid(base, over, t) {
        return Qt.rgba(base.r + (over.r - base.r) * t,
                       base.g + (over.g - base.g) * t,
                       base.b + (over.b - base.b) * t, 1.0)
    }
    readonly property color _fillRest: Theme.colorSurface
    readonly property color _fillHot:  _solid(Theme.colorSurface, Theme.colorAccent, 0.30)
    // The collar sits FURTHER out, over more of the picture, so its resting cell is pulled a step
    // away from the wedges rather than matching them — otherwise the two rings of the same colour
    // read as one shape with a seam in it.
    readonly property color _fillCell: _solid(Theme.colorSurface, Theme.colorText, 0.06)

    contentItem: Item {
        id: face

        readonly property real cx: root._outer
        readonly property real cy: root._outer

        function px(r, deg) { return cx + r * Math.cos(deg * Math.PI / 180) }
        function py(r, deg) { return cy + r * Math.sin(deg * Math.PI / 180) }

        // ── The wedges ────────────────────────────────────────────────────────
        //
        // ONE Shape, one ShapePath per sector — eight of them, declared, because there are always
        // exactly eight and a Repeater cannot produce ShapePaths. The labels are Text items over the
        // top rather than glyphs inside the paths, so they stay upright and selectable by the same
        // rules as every other label in the panel.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            component Wedge: ShapePath {
                id: wedge
                required property int slot

                readonly property var  entry: root._entryAt(slot)
                readonly property bool hot:   root._hotSlot === slot
                readonly property real a0:    -90 + slot * 45 - root._sweep / 2
                readonly property real a1:    -90 + slot * 45 + root._sweep / 2
                // An empty slot collapses to nothing rather than drawing a disabled wedge. The
                // hairline that remains is the 3° gap the lit wedges leave either side of it.
                readonly property real rOut:  entry ? root._r1 : root._r0

                fillColor: !entry ? "transparent"
                         : hot    ? root._fillHot
                                  : root._fillRest
                // Strong at rest, not mid: this edge is what separates the ring from a drawing
                // behind it, and it is the only thing doing that job — there is no scrim, because a
                // scrim would dim the picture the gesture is ABOUT.
                strokeColor: !entry ? "transparent"
                           : hot    ? Theme.colorAccent
                                    : Theme.colorBorderStrong
                strokeWidth: entry ? (hot ? 2 : 1) : 0
                capStyle:    ShapePath.FlatCap

                startX: face.px(root._r0, a0)
                startY: face.py(root._r0, a0)
                PathLine { x: face.px(wedge.rOut, wedge.a0); y: face.py(wedge.rOut, wedge.a0) }
                PathAngleArc {
                    centerX: face.cx; centerY: face.cy
                    radiusX: wedge.rOut; radiusY: wedge.rOut
                    startAngle: wedge.a0; sweepAngle: root._sweep
                    moveToStart: false
                }
                PathLine { x: face.px(root._r0, wedge.a1); y: face.py(root._r0, wedge.a1) }
                PathAngleArc {
                    centerX: face.cx; centerY: face.cy
                    radiusX: root._r0; radiusY: root._r0
                    startAngle: wedge.a1; sweepAngle: -root._sweep
                    moveToStart: false
                }
            }

            Wedge { slot: 0 }
            Wedge { slot: 1 }
            Wedge { slot: 2 }
            Wedge { slot: 3 }
            Wedge { slot: 4 }
            Wedge { slot: 5 }
            Wedge { slot: 6 }
            Wedge { slot: 7 }
        }

        // ── Wedge labels ──────────────────────────────────────────────────────
        Repeater {
            model: 8
            delegate: Item {
                id: lab
                required property int index

                readonly property var  entry: root._entryAt(index)
                readonly property bool hot:   root._hotSlot === index
                readonly property real ang:   -90 + index * 45
                readonly property real mid:   (root._r0 + root._r1) / 2

                visible: entry !== null
                width:   root._r1 - root._r0 - Theme.sp(10)
                height:  Theme.sp(46)
                x: face.px(mid, ang) - width / 2
                y: face.py(mid, ang) - height / 2

                Column {
                    anchors.centerIn: parent
                    width: parent.width
                    spacing: Theme.sp(2)

                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text:    lab.entry ? (lab.entry.icon || "") : ""
                        visible: text.length > 0
                        font.family:    Theme.fontSymbol
                        font.pixelSize: Theme.fontSzBody
                        // The hot wedge is already accented in its FILL and its border, so the
                        // text goes the other way — to full contrast. Accent on an accent wash is
                        // how a highlight ends up less legible than the thing it highlights.
                        color: lab.hot ? Theme.colorText : Theme.colorText2
                    }

                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        // A value spoke says so on its face. `▸` is the affordance for the collar,
                        // and it is the only thing that distinguishes a set from a make.
                        text: lab.entry
                                  ? (lab.entry.label || "")
                                    + (lab.entry.values && lab.entry.values.length > 0 ? " ▸" : "")
                                  : ""
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        font.weight:    Theme.fontBodyWeight
                        // Danger is TEXT, on the ordinary fill. A red wedge would make the ring a
                        // warning, and seven of its eight slots are not one.
                        color: lab.entry && lab.entry.danger ? Theme.colorError : Theme.colorText
                        wrapMode:            Text.WordWrap
                        maximumLineCount:    2
                        elide:               Text.ElideRight
                    }

                    // The spoken label on every spoke at once, for the first hold of a session only.
                    // After that the hub carries it, one spoke at a time.
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        visible: root._verbose && text.length > 0
                        text:    lab.entry ? (lab.entry.hint || "") : ""
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText3
                        wrapMode:         Text.WordWrap
                        maximumLineCount: 2
                        elide:            Text.ElideRight
                    }
                }
            }
        }

        // ── The collar ────────────────────────────────────────────────────────
        //
        // One level, three cells, past the rim, aligned to its own spoke's arc. A value with more
        // than three options gets no collar at all — its spoke opens the inspector — so there is
        // nothing here to paginate and never will be.
        Shape {
            visible: root._inCollar && root._hotEntry
                     && root._hotEntry.values && root._hotEntry.values.length > 0
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer

            component Cell: ShapePath {
                id: cell
                required property int idx

                readonly property var  vals: root._hotEntry && root._hotEntry.values
                                                 ? root._hotEntry.values : []
                readonly property int  n:    Math.min(3, vals.length)
                readonly property bool live: idx < n
                readonly property real span: root._sweep / Math.max(1, n)
                readonly property real base: -90 + root._hotSlot * 45 - root._sweep / 2
                readonly property real a0:   base + idx * span
                readonly property real a1:   base + (idx + 1) * span
                readonly property bool hot:  root._hotCell === idx

                fillColor: !live ? "transparent"
                         : hot   ? root._fillHot
                                 : root._fillCell
                strokeColor: !live ? "transparent"
                           : hot   ? Theme.colorAccent
                                   : Theme.colorBorderStrong
                strokeWidth: live ? (hot ? 2 : 1) : 0

                startX: face.px(root._r1, live ? a0 : 0)
                startY: face.py(root._r1, live ? a0 : 0)
                PathLine {
                    x: face.px(cell.live ? root._outer : root._r1, cell.live ? cell.a0 : 0)
                    y: face.py(cell.live ? root._outer : root._r1, cell.live ? cell.a0 : 0)
                }
                PathAngleArc {
                    centerX: face.cx; centerY: face.cy
                    radiusX: cell.live ? root._outer : root._r1
                    radiusY: cell.live ? root._outer : root._r1
                    startAngle: cell.live ? cell.a0 : 0
                    sweepAngle: cell.live ? cell.span : 0
                    moveToStart: false
                }
                PathLine {
                    x: face.px(root._r1, cell.live ? cell.a1 : 0)
                    y: face.py(root._r1, cell.live ? cell.a1 : 0)
                }
                PathAngleArc {
                    centerX: face.cx; centerY: face.cy
                    radiusX: root._r1; radiusY: root._r1
                    startAngle: cell.live ? cell.a1 : 0
                    sweepAngle: cell.live ? -cell.span : 0
                    moveToStart: false
                }
            }

            Cell { idx: 0 }
            Cell { idx: 1 }
            Cell { idx: 2 }
        }

        Repeater {
            model: 3
            delegate: Item {
                id: cellLab
                required property int index

                readonly property var  vals: root._hotEntry && root._hotEntry.values
                                                 ? root._hotEntry.values : []
                readonly property int  n:    Math.min(3, vals.length)
                readonly property real span: root._sweep / Math.max(1, n)
                readonly property real ang:  -90 + root._hotSlot * 45 - root._sweep / 2
                                             + (index + 0.5) * span
                readonly property real mid:  root._r1 + root._collar / 2

                visible: root._inCollar && index < n
                width:   root._collar - Theme.sp(6)
                height:  Theme.sp(30)
                x: face.px(mid, ang) - width / 2
                y: face.py(mid, ang) - height / 2

                Text {
                    anchors.centerIn: parent
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: cellLab.index < cellLab.n ? (cellLab.vals[cellLab.index].label || "") : ""
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    // The value it already has, marked. Releasing there is the no-op, and the mark
                    // is what tells the author which direction is a change before they make one.
                    font.weight: cellLab.index < cellLab.n
                                 && cellLab.vals[cellLab.index].current === true
                                     ? Font.DemiBold : Theme.fontBodyWeight
                    color: root._hotCell === cellLab.index ? Theme.colorText
                         : cellLab.index < cellLab.n
                           && cellLab.vals[cellLab.index].current === true
                                                           ? Theme.colorText
                                                           : Theme.colorText2
                    elide: Text.ElideRight
                }
            }
        }

        // ── The hub ───────────────────────────────────────────────────────────
        //
        // Text, not a button. Releasing here cancels, and nothing is ever committed from the centre
        // — which is what makes "back to the middle" the escape an author can find without being
        // told, from any direction, at any moment in the gesture.
        Rectangle {
            x: face.cx - root._r0
            y: face.cy - root._r0
            width:  root._r0 * 2
            height: root._r0 * 2
            radius: root._r0
            color:  Theme.colorBg
            border.width: 1
            border.color: root._hotSlot < 0 ? Theme.colorBorderStrong : Theme.colorBorderMid

            Column {
                anchors.centerIn: parent
                width: root._r0 * 1.7
                spacing: Theme.sp(3)

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root.hubTitle
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    font.weight:    Theme.fontBodyWeight
                    color:          Theme.colorText
                    wrapMode:         Text.WordWrap
                    maximumLineCount: 2
                    elide:            Text.ElideRight
                }

                // The one place the ring explains itself: what this spoke would do, in words, while
                // the cursor is on it. That is what buys the wedges the right to stay terse.
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: root._hotEntry ? (root._hotEntry.hint || root._hotEntry.label || "")
                                         : root.hubMeta
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color: root._hotEntry ? Theme.colorAccent : Theme.colorText3
                    wrapMode:         Text.WordWrap
                    maximumLineCount: 3
                    elide:            Text.ElideRight
                }
            }
        }

        // ── The latched ring's own input ──────────────────────────────────────
        //
        // Only for the right-click entrance, where no button is down and there is therefore no grab
        // to fight over. A held ring is driven entirely by the canvas, and this area is inert for
        // it — otherwise it would steal the very press the gesture is made of.
        MouseArea {
            anchors.fill: parent
            enabled:      root.latched
            visible:      root.latched
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPositionChanged: (mouse) => {
                var p = mapToItem(root.parent, mouse.x, mouse.y)
                root.track(p.x, p.y)
            }
            onClicked: (mouse) => {
                var p = mapToItem(root.parent, mouse.x, mouse.y)
                root.track(p.x, p.y)
                root.release()
            }
        }
    }
}
