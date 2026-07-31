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
import PinPointStudio

// The corridor, drawn: the distribution the norm CLAIMS, over the readings the library actually
// holds.
//
// Every coordinate comes from ModelBrowser.corridorPlot() — which is corridor_plot.h — so this file
// draws and drags and decides nothing. The curve is split-normal because the tolerances are
// asymmetric by design; the bands are the bands that grade, monitor-band precedence included.
//
// ── Why the data is drawn the way it is ────────────────────────────────────────────────────────
//
// The old editor put a histogram behind the corridor. A histogram's bar edges are an artefact of
// the binning rather than of the swings, and at any useful bin count it dominates the picture —
// which is backwards, because the subject here is the CORRIDOR and the readings are evidence about
// it.
//
// So there is ONE curve — the one being set — and the readings appear on and beneath it:
//   * dots sitting on the curve, each at its own value, lifted to the density the norm gives it
//   * a rug of hairlines on the baseline: every reading, unbinned and uninterpreted
//
// Where the dots gather along the curve is the answer. A corridor centred where the swings are
// collects them around the peak; one centred elsewhere strands them along the floor of a tail.
Item {
    id: root

    // The map from ModelBrowser.corridorPlot().
    property var plot: ({})
    property bool editable: true

    // Lays the picture out. `opts` is forwarded straight to ModelBrowser.corridorPlot(), so this
    // component decides WHAT to ask for and the facade decides everything about the answer.
    //
    // The component asks rather than being handed a finished plot because it is the only thing that
    // knows how wide the canvas actually is. Laying out at a guessed width and drawing at the real
    // one scales every coordinate by the ratio between them — the curve, the bands and the handles
    // all land somewhere else, and a handle that is not where the pointer is cannot be dragged.
    property var plotSource: null

    // A handle releases ONE value, named by which handle it was: "mu", "idealLo" or "idealHi". The
    // edge handles deliberately do not send a tolerance — an edge is `mu ∓ idealMaxZ * sigma`, and
    // dividing back through the policy is a rule that belongs in C++ with the rest of them.
    signal handleCommitted(string handle, real value)
    // A field typed rather than dragged. `text` is passed through verbatim so an EMPTY one can mean
    // "no bound" — clearing a plausibility bound is a real edit, and a number cannot express it.
    signal fieldCommitted(string field, string text)
    signal scanRequested()

    // ── Drag state ────────────────────────────────────────────────────────────
    // NaN means "not being dragged", so an untouched handle keeps whatever is stored.
    property string _dragging: ""
    property real   _dragMu:   NaN
    property real   _dragLo:   NaN
    property real   _dragHi:   NaN

    // The window, FROZEN for the duration of a drag.
    //
    // The window is normally derived from the band edges, so moving mu moves it — and then a pointer
    // position no longer maps to one value: the value shifts the window, which shifts the value,
    // which is why the handle jumped rather than followed. Held still while dragging, the axis is a
    // ruler; it re-frames once on release, which is a single expected step rather than a fight.
    property real _frozenMin: NaN
    property real _frozenMax: NaN

    readonly property var _shown: {
        if (!root.plotSource || canvas.width <= 0) return ({})
        var o = { width: canvas.width, height: canvas.height }
        if (root._dragging !== "") {
            if (!isNaN(root._dragMu)) o.previewMu      = root._dragMu
            if (!isNaN(root._dragLo)) o.previewIdealLo = root._dragLo
            if (!isNaN(root._dragHi)) o.previewIdealHi = root._dragHi
            if (!isNaN(root._frozenMin)) { o.windowMin = root._frozenMin
                                           o.windowMax = root._frozenMax }
        }
        var v = root.plotSource(o)
        return v ? v : ({})
    }

    readonly property bool _found: _shown && _shown.found === true
    readonly property real _span:  _found ? (_shown.xMax - _shown.xMin) : 1

    // The quantum this unit is authored in — whole degrees, whole percent, whole millimetres. From
    // C++, because the drag, the typed field, the table cell and the readout have to agree about
    // what a value IS, and four roundings would be four answers.
    readonly property real _step:     root._found && root._shown.step     ? root._shown.step : 0.01
    readonly property int  _decimals: root._found && root._shown.decimals !== undefined
                                          ? root._shown.decimals : 2

    function _toValue(px) {
        // Against the CANVAS, which is what the layout was measured in — root.width includes
        // nothing else, but tying the two together is how a scale drifts.
        if (!root._found || canvas.width <= 0) return 0
        var v = root._shown.xMin + (px / canvas.width) * root._span
        // Snapped HERE as well as on commit, so the curve, the readout and the stored value are one
        // number all the way through. Unsnapped, the picture would move continuously and then jump
        // when released — and the readout would show a precision the file never keeps.
        return Math.round(v / root._step) * root._step
    }

    function _format(v) { return Number(v).toFixed(root._decimals) }

    function _beginDrag(key) {
        root._frozenMin = root._shown.xMin
        root._frozenMax = root._shown.xMax
        root._dragging  = key
    }

    function _endDrag() {
        root._dragging  = ""
        root._dragMu    = NaN
        root._dragLo    = NaN
        root._dragHi    = NaN
        root._frozenMin = NaN
        root._frozenMax = NaN
    }

    function _bandColor(grade) {
        switch (grade) {
        case "ideal":  return Theme.colorRagGood
        case "good":   return Theme.colorBandGreen
        case "watch":  return Theme.colorRagWatch
        case "action": return Theme.colorRagFault
        }
        return Theme.colorRagNone
    }

    implicitHeight: Theme.sp(210)

    Text {
        anchors.centerIn: parent
        width: parent.width - Theme.sp(48)
        visible: !root._found
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
        text: qsTr("No corridor resolves here, so there is nothing to grade against.")
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        color:          Theme.colorText3
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.sp(6)
        visible: root._found

        // ── The canvas ────────────────────────────────────────────────────────
        Item {
            id: canvas
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sp(150)

            // Grade bands, as regions. Coloured by the band's own WORD rather than by its position
            // in the list — the order flips on a one-sided measure, and a positional mapping would
            // paint Action green.
            Repeater {
                model: root._found ? root._shown.bands : []
                delegate: Rectangle {
                    required property var modelData
                    x:      modelData.x
                    width:  modelData.w
                    height: canvas.height
                    color:  root._bandColor(modelData.grade)
                    // Very low alpha: these are the context the curve sits in, not the subject.
                    opacity: modelData.grade === "ideal" ? 0.13 : 0.07
                }
            }

            // The rug: every reading, on the baseline. No binning and no interpretation — it is the
            // one place the raw data appears unmediated, and at 2000 swings it reads as density
            // rather than as a block because each tick is a hairline at low alpha.
            Repeater {
                model: root._found ? root._shown.rug : []
                delegate: Rectangle {
                    required property real modelData
                    x:       modelData
                    y:       canvas.height - Theme.sp(7)
                    width:   1
                    height:  Theme.sp(7)
                    color:   Theme.colorText
                    opacity: 0.20
                }
            }

            // The readings, on the curve. Each sits at its own value — nothing jittered and
            // nothing invented — lifted to the height the norm gives that value, so the cloud IS
            // the distribution as this corridor sees it.
            Repeater {
                model: root._found ? root._shown.samples : []
                delegate: Rectangle {
                    required property var modelData
                    x:      modelData.x - Theme.sp(1.5)
                    y:      modelData.y - Theme.sp(1.5)
                    width:  Theme.sp(3)
                    height: Theme.sp(3)
                    radius: width / 2
                    color:  Theme.colorAccent
                    // Overlapping dots darken, so a hundred swings at one value reads heavier than
                    // one — density without jitter, which on an axis carrying units would be a lie
                    // about the value.
                    opacity: 0.38
                }
            }

            // The curve the norm claims. Drawn over the cloud, because the claim is the subject and
            // has to stay legible through a thousand dots sitting on it.
            Shape {
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                visible: root._found && root._shown.curve.length > 1

                ShapePath {
                    strokeColor: Theme.colorText
                    strokeWidth: 1.5
                    fillColor:   "transparent"
                    capStyle:    ShapePath.RoundCap
                    PathPolyline {
                        path: {
                            if (!root._found) return []
                            var pts = []
                            for (var i = 0; i < root._shown.curve.length; i++)
                                pts.push(Qt.point(root._shown.curve[i].x, root._shown.curve[i].y))
                            return pts
                        }
                    }
                }
            }

            // The aspiration. A line rather than a handle body, so the number it names stays
            // readable against the curve behind it.
            Rectangle {
                x:       root._found ? root._shown.muX : 0
                width:   1
                height:  canvas.height
                color:   Theme.colorText
                opacity: 0.55
            }

            // ── Handles ───────────────────────────────────────────────────────
            //
            // Three explicit instances, NOT a Repeater. The model would be a fresh array on every
            // preview, so the delegates — and the DragHandler holding the gesture — were destroyed
            // and rebuilt under the pointer on every mouse move.
            component Handle: Item {
                id: handle

                required property string key
                required property real   px       // where the layout puts it
                property bool  open: false        // this tail does not grade, so it has no edge

                readonly property bool live: root._dragging === key

                visible: !open && root.editable && root._found
                y:       canvas.height - Theme.sp(16)
                width:   Theme.sp(11)
                height:  Theme.sp(11)
                z: 6

                // Bound only while it is NOT being dragged. A `x: live ? x : px` binding would
                // reference itself, and the DragHandler needs x to be its own to write to.
                Binding on x {
                    when:  !handle.live
                    value: handle.px - handle.width / 2
                }

                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color:  handle.live ? Theme.colorAccent : Theme.colorSurface
                    border.width: 1
                    border.color: Theme.colorAccent
                }

                // The value under the pointer, while it is under the pointer. A handle you have to
                // release to find out about is a handle you drag twice.
                Rectangle {
                    visible: handle.live
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.bottom: parent.top
                    anchors.bottomMargin: Theme.sp(4)
                    width:  readout.implicitWidth + Theme.sp(10)
                    height: readout.implicitHeight + Theme.sp(5)
                    radius: Theme.radius
                    color:  Theme.colorAccent

                    Text {
                        id: readout
                        anchors.centerIn: parent
                        text: root._format(root._toValue(handle.x + handle.width / 2))
                              + " " + (root._found ? root._shown.unit : "")
                        font.family:    Theme.fontData
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.dark ? Theme.colorBg : "#FFFFFF"
                    }
                }

                DragHandler {
                    yAxis.enabled: false
                    xAxis.minimum: -handle.width / 2
                    xAxis.maximum: canvas.width - handle.width / 2
                    onActiveChanged: {
                        if (active) { root._beginDrag(handle.key); return }
                        root.handleCommitted(handle.key,
                                             root._toValue(handle.x + handle.width / 2))
                        root._endDrag()
                    }
                }

                onXChanged: {
                    if (!handle.live) return
                    var v = root._toValue(handle.x + handle.width / 2)
                    if (handle.key === "mu")           root._dragMu = v
                    else if (handle.key === "idealLo") root._dragLo = v
                    else                               root._dragHi = v
                }
            }

            Handle { key: "mu";      px: root._found ? root._shown.muX : 0 }
            Handle {
                key:  "idealLo"
                px:   root._found ? root._shown.idealLoX : 0
                open: root._found && root._shown.lowOpen
            }
            Handle {
                key:  "idealHi"
                px:   root._found ? root._shown.idealHiX : 0
                open: root._found && root._shown.highOpen
            }

            // Live readout while dragging, so a handle is never a number you have to guess at.
            Text {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: Theme.sp(4)
                visible: root._found
                text: root._found
                          ? qsTr("μ %1 %2   ·   −%3 / +%4")
                                .arg(root._format(root._shown.mu)).arg(root._shown.unit)
                                .arg(root._format(root._shown.sigmaLo))
                                .arg(root._format(root._shown.sigmaHi))
                          : ""
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }
        }

        // ── What it found ─────────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.sp(12)

            // The band shares. This is the corridor editor's whole argument in four numbers: a
            // corridor grading almost everything into one band is visibly wrong to somebody who has
            // never heard of a standard deviation.
            Repeater {
                model: root._found && root._shown.n > 0
                           ? [ { w: qsTr("Ideal"),  n: root._shown.ideal,  g: "ideal" },
                               { w: qsTr("Good"),   n: root._shown.good,   g: "good" },
                               { w: qsTr("Watch"),  n: root._shown.watch,  g: "watch" },
                               { w: qsTr("Action"), n: root._shown.action, g: "action" } ]
                           : []
                delegate: RowLayout {
                    id: share
                    required property var modelData
                    spacing: Theme.sp(5)

                    Rectangle {
                        implicitWidth:  Theme.sp(7)
                        implicitHeight: Theme.sp(7)
                        radius: width / 2
                        color:  root._bandColor(share.modelData.g)
                    }
                    Text {
                        text: qsTr("%1 %2%").arg(share.modelData.w)
                                  .arg(Math.round(100 * share.modelData.n / root._shown.n))
                        font.family:    Theme.fontBody
                        font.pixelSize: Theme.fontSzMicro
                        color:          Theme.colorText2
                    }
                }
            }

            Item { Layout.fillWidth: true }

            Text {
                visible: root._found && root._shown.implausible > 0
                // NOT a grade, and never counted as one: "we do not believe this number" and "this
                // swing was bad" are different statements.
                text: qsTr("%n not believed", "", root._found ? root._shown.implausible : 0)
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorWarn
            }

            Text {
                visible: root._found && root._shown.truncated
                text:    qsTr("scatter thinned")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color:          Theme.colorText3
            }

            Text {
                text: root._found && root._shown.scanned
                          ? qsTr("%n reading(s)", "", root._shown.n)
                          : qsTr("scan the library →")
                font.family:    Theme.fontData
                font.pixelSize: Theme.fontSzMicro
                color: root._found && root._shown.scanned ? Theme.colorText3 : Theme.colorAccent
                PpPressable {
                    hoverScale: 1.0
                    enabled:    !(root._found && root._shown.scanned)
                    onClicked:  root.scanRequested()
                }
            }
        }

        // ── The numbers, typed ────────────────────────────────────────────────
        //
        // A handle is for FINDING a value against the data; a field is for STATING one. Neither
        // replaces the other: 30.5 is quicker typed than dragged, and the plausibility bounds have
        // no handle at all — they answer "is this reading real?", which is a different question from
        // "is this swing good?" and does not belong on the same ruler.
        GridLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.sp(4)
            visible:  root.editable && root._found
            columns:  root.width > Theme.sp(420) ? 6 : 4
            columnSpacing: Theme.sp(10)
            rowSpacing:    Theme.sp(4)

            component Field: RowLayout {
                id: field
                required property string label
                required property string fieldName
                required property string value
                property string placeholder: ""

                spacing: Theme.sp(5)

                Text {
                    text:           field.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                }

                PpTextField {
                    Layout.preferredWidth: Theme.sp(72)
                    implicitHeight:  Theme.sp(24)
                    font.family:     Theme.fontData
                    font.pixelSize:  Theme.fontSzMicro
                    placeholderText: field.placeholder
                    text:            field.value
                    // Committed on Enter and on leaving the field — PpTextField raises
                    // editingFinished for both, which is this project's existing convention.
                    onEditingFinished: {
                        if (text !== field.value) root.fieldCommitted(field.fieldName, text)
                    }
                }
            }

            Field {
                label:     qsTr("μ")
                fieldName: "mu"
                value:     root._found ? root._format(root._shown.mu) : ""
            }
            // Two tolerances, never one. They are asymmetric BY DESIGN — a corridor may be tight on
            // one side and loose on the other, and offering a single figure would make that
            // unauthorable.
            Field {
                label:     qsTr("tol −")
                fieldName: "sigmaLo"
                value:     root._found ? root._format(root._shown.sigmaLo) : ""
            }
            Field {
                label:     qsTr("tol +")
                fieldName: "sigmaHi"
                value:     root._found ? root._format(root._shown.sigmaHi) : ""
            }
            Field {
                label:       qsTr("believed ≥")
                fieldName:   "plausibleLo"
                placeholder: qsTr("none")
                value: root._found && root._shown.hasPlausibleLo
                           ? root._format(root._shown.plausibleLo) : ""
            }
            Field {
                label:       qsTr("believed ≤")
                fieldName:   "plausibleHi"
                placeholder: qsTr("none")
                value: root._found && root._shown.hasPlausibleHi
                           ? root._format(root._shown.plausibleHi) : ""
            }
        }

        // The finding, in words. The threshold is diagnostics_health.h's, so this view and the
        // health list cannot disagree about the same corridor.
        Text {
            Layout.fillWidth: true
            visible: root._found && root._shown.note !== ""
            text:    root._found ? root._shown.note : ""
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorWarn
            wrapMode:       Text.WordWrap
        }
    }
}
