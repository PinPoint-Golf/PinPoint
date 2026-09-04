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

// PpChartPlot — one plot region: a Y axis (grid + ticks + unit), an X time axis (ms from
// impact), swing-phase bands + impact emphasis, N metric traces, band-coloured P-position
// dots, a replay playhead, and a shared hover crosshair. Given a series subset (each
// decorated with its `color`), a value range [valueLo,valueHi], a time domain
// [domStartUs,domEndUs], and geometry — so it serves BOTH overlay (one plot, N series) and
// split (N plots, one series each). Pure scale/path binding only; axis maths lives in
// ChartMetrics, phase tags in TimelineLabels. This is the unit future charts reuse.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // ── Data (decorated by the parent) ────────────────────────────────────────────
    // series: [{ key, label, unit, t_us:[…], value:[…], phaseSamples:[…], color,
    //            valid?:[0|1…], validFromUs?, validToUs? }]
    //
    // The last three are the HONESTY triple (design metric_presentation_honesty.md §5.1) and all
    // three are OPTIONAL, because absence is the common case and must cost nothing:
    //   `valid`       — swing.json metrics[].valid, parallel to t_us; 0 = a sample the grid
    //                   BRIDGED across a gated or absent run. Absent/empty ⇒ every sample valid.
    //   `validFromUs` / `validToUs` — the metric's phase domain resolved to INSTANTS by the host
    //                   against this swing's own ladder (PpMetricChart._domainWindow). Absent, or
    //                   a degenerate pair, ⇒ no domain clipping. NOT to be confused with
    //                   domStartUs/domEndUs below, which are the VISIBLE TIME WINDOW: one is
    //                   "where does this metric mean anything", the other "what is on screen".
    property var  series:  []
    property var  phases:  []            // [{ phase, t_us, conf }]
    property real valueLo: 0
    property real valueHi: 1
    property real domStartUs: 0
    property real domEndUs:   1
    property real impactUs:   0
    property string unitLabel: ""

    // ── Playhead + shared cursor ──────────────────────────────────────────────────
    property real playheadUs:   0
    property bool showPlayhead:  true
    property bool showDots:      true
    property bool showCrosshair: true
    property real cursorUs:     -1       // shared hover cursor (−1 = inactive)

    // ── Geometry knobs (parent tunes per mode) ────────────────────────────────────
    property int  yTickCount: 4
    property bool showXAxis:  true       // X tick labels at the bottom of this plot
    property bool showFrame:  false      // thin border around the plot rect (split facets)
    property real gutterLeft: Theme.sp(54)
    property real padR:       Theme.sp(10)

    // Split-mode gutter captions (empty in overlay): the metric's name + a compact @end
    // readout, supplied by the parent so this stays a dumb plotter.
    property string facetName:    ""
    property string facetEndText: ""
    property real padT:       Theme.sp(8)
    property real xAxisH:     Theme.sp(20)

    // Reported up to the orchestrator, which fans the cursor back to every plot.
    signal hoverMoved(real tUs)
    signal hoverExited()

    // Optional click/drag-to-seek. When enabled the plot becomes a scrub surface —
    // a tap seeks, a drag scrubs — reported up as seekRequested/scrubBegan/scrubEnded
    // for the host to wire to the replay (this plotter never touches it directly).
    // Off by default so decorative / compact instances stay non-interactive.
    property bool seekEnabled: false
    signal seekRequested(real tUs)
    signal scrubBegan()
    signal scrubEnded()

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }

    // ── Plot rectangle + scales ───────────────────────────────────────────────────
    readonly property real _plotLeft:   gutterLeft
    readonly property real _plotRight:  width - padR
    readonly property real _plotTop:    padT
    readonly property real _plotBottom: height - (showXAxis ? xAxisH : 0)
    readonly property real _plotW: Math.max(1, _plotRight - _plotLeft)
    readonly property real _plotH: Math.max(1, _plotBottom - _plotTop)

    function xForT(t) {
        return (domEndUs > domStartUs)
             ? (t - domStartUs) / (domEndUs - domStartUs) * root._plotW : 0
    }
    function yForV(v) {
        return (valueHi > valueLo)
             ? root._plotH - (v - valueLo) / (valueHi - valueLo) * root._plotH
             : root._plotH / 2
    }
    function _inDom(t) { return t >= domStartUs && t <= domEndUs }
    function _bandColor(b) {
        return b === "warn"      ? Theme.colorWarn
             : b === "attention" ? Theme.colorAttention
             :                     Theme.colorGood
    }

    // ── Measured vs bridged: how a curve says which of it is a measurement ────────
    //
    // A sample is drawn SOLID only when it was measured: inside the metric's phase domain
    // (validFromUs..validToUs) AND not marked 0 in `valid`. Everything else — the invalid runs
    // and the whole out-of-domain region — is drawn in the SAME colour at low opacity with a
    // dashed stroke. A gap would be honest too, but a dashed bridge is honest AND keeps the eye
    // on where the curve resumes (design §5.1, the Chart bullet).
    //
    // ⚠ THIS IS NOT A FILTER AND NOT A SMOOTHER. Every persisted sample is on screen at its
    // persisted value; only the stroke changes. Display-only smoothing is forbidden outright
    // (design §4 principle 1) — a chart that shows one thing while the card computes another is
    // where dishonesty starts, and it hides the measurement problem from whoever must fix it.
    function _hasDomain(s) {
        return s.validFromUs !== undefined && s.validToUs !== undefined
               && s.validToUs > s.validFromUs
    }
    // The INDEX form of the predicate — three comparisons, no marshalling. ChartMetrics.measuredAt
    // is the same rule at an arbitrary instant and stays the reference implementation, but every
    // call into it copies the whole series across the QML boundary, so it is reserved for bindings
    // that change only with the DATA (the phase dots). Anything that re-evaluates with the cursor,
    // the playhead or the view window answers it here instead.
    //
    // Short-mask rule, shared with the C++ (chart_metrics.h) and with measure_sample.cpp: a `valid`
    // list that does not cover the curve is a malformed document and is discarded WHOLESALE, not
    // applied to the prefix it covers — two views of one curve must not disagree about it.
    function _measured(s, i) {
        if (root._hasDomain(s) && (s.t_us[i] < s.validFromUs || s.t_us[i] > s.validToUs))
            return false
        if (!s.valid || s.valid.length < s.t_us.length) return true
        return s.valid[i] !== 0
    }
    // Nearest sample index to `t`, or -1 on an empty curve — what the per-frame callers need so
    // they can use the index form above.
    function _nearestIndex(s, t) {
        var tt = s.t_us
        if (!tt || tt.length === 0) return -1
        var best = -1, bd = Infinity
        for (var i = 0; i < tt.length; ++i) {
            var d = Math.abs(tt[i] - t)
            if (d < bd) { bd = d; best = i }
        }
        return best
    }
    function _measuredAtT(s, t) {
        var i = root._nearestIndex(s, t)
        return i < 0 || root._measured(s, i)
    }

    // Each series split into RUNS of one drawn state: [{ color, dashed, t:[…], v:[…] }] in time
    // order. Index ranges, not screen points, so a resize or a Y-range change re-runs only the
    // cheap coordinate map in the PathPolyline binding below and not the splitting.
    //
    // A DASHED run reaches one sample BACKWARD and one sample FORWARD into its solid neighbours.
    // That does two things: it closes the seam (consecutive runs share an endpoint, so the curve
    // is continuous), and it means the two segments that TOUCH an unmeasured sample are
    // themselves dashed — a segment with one unmeasured end is not a measured segment. Solid runs
    // therefore stay strictly inside their own samples, and a solid run left with fewer than two
    // samples draws nothing: its neighbours already cover those segments.
    readonly property var _traceRuns: {
        var out = []
        for (var k = 0; k < (root.series ? root.series.length : 0); ++k) {
            var s = root.series[k]
            if (!s || !s.t_us || !s.value || s.t_us.length < 2) continue

            // Per-sample drawn state first, so the run walk below reads a plain boolean array.
            var solid = []
            for (var i = 0; i < s.t_us.length; ++i) solid.push(root._measured(s, i))

            var i0 = 0
            while (i0 < solid.length) {
                var i1 = i0
                while (i1 + 1 < solid.length && solid[i1 + 1] === solid[i0]) ++i1
                var a = i0, b = i1
                if (!solid[i0]) {                  // dashed: reach into the solid neighbours
                    if (a > 0) --a
                    if (b + 1 < solid.length) ++b
                }
                if (b > a) {
                    var t = [], v = []
                    for (var j = a; j <= b; ++j) { t.push(s.t_us[j]); v.push(s.value[j]) }
                    out.push({ color: s.color, dashed: !solid[i0], t: t, v: v })
                }
                i0 = i1 + 1
            }
        }
        return out
    }

    // ── Y grid + tick labels (absolute coords; gutter holds the labels) ────────────
    Repeater {
        model: cm.niceTicks(root.valueLo, root.valueHi, root.yTickCount)
        delegate: Item {
            id: yt
            required property var modelData
            readonly property real yy: root._plotTop + root.yForV(yt.modelData)
            Rectangle {
                x: root._plotLeft; y: yt.yy
                width: root._plotW; height: 1
                color: yt.modelData === 0 ? Theme.colorBorderStrong : Theme.colorBorderMid
                opacity: yt.modelData === 0 ? 0.7 : 0.45
            }
            Text {
                x: root._plotLeft - Theme.sp(4) - width      // right edge sits just left of the grid
                y: yt.yy - height / 2
                text: Math.round(yt.modelData)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }
        }
    }
    Text {                                   // Y unit (overlay: above the plot, left)
        x: Theme.sp(2); y: root._plotTop - height - Theme.sp(1)
        text: root.unitLabel
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        font.letterSpacing: Theme.trackingData
        color: Theme.colorText2
        visible: root.unitLabel.length > 0 && root.facetName.length === 0
    }

    // Split-mode gutter: facet name + unit (top) and @end readout (bottom).
    Column {
        visible: root.facetName.length > 0
        x: Theme.sp(4); y: root._plotTop + Theme.sp(2)
        spacing: Theme.sp(1)
        Text {
            text: root.facetName
            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
            color: Theme.colorText
        }
        Text {
            text: root.unitLabel
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }
    }
    Text {
        visible: root.facetName.length > 0 && root.facetEndText.length > 0
        x: Theme.sp(4); y: root._plotBottom - height - Theme.sp(2)
        text: root.facetEndText
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        color: Theme.colorText2
    }

    // ── Phase bands + ticks + tags ────────────────────────────────────────────────
    Repeater {
        model: root.phases
        delegate: Item {
            id: ph
            required property var modelData
            required property int index
            readonly property bool isImpact: ph.modelData.phase === 5      // Phase::Impact
            readonly property real tx: root._plotLeft + root.xForT(ph.modelData.t_us)
            readonly property var  nextPhase: (ph.index + 1 < root.phases.length)
                                              ? root.phases[ph.index + 1] : null

            // Alternating shaded band from this phase to the next (even indices only).
            Rectangle {
                visible: ph.index % 2 === 0 && ph.nextPhase !== null
                         && ph.nextPhase.t_us > root.domStartUs && ph.modelData.t_us < root.domEndUs
                x: root._plotLeft + root.xForT(Math.max(ph.modelData.t_us, root.domStartUs))
                y: root._plotTop
                width: ph.nextPhase ? Math.max(0,
                          root.xForT(Math.min(ph.nextPhase.t_us, root.domEndUs))
                        - root.xForT(Math.max(ph.modelData.t_us, root.domStartUs))) : 0
                height: root._plotH
                color: Theme.colorAccent; opacity: 0.04
            }
            // Tick line.
            Rectangle {
                visible: root._inDom(ph.modelData.t_us)
                x: ph.tx; y: root._plotTop
                width: ph.isImpact ? Theme.sp(1.5) : 1; height: root._plotH
                color: ph.isImpact ? Theme.colorAccent : Theme.colorBorderMid
                opacity: ph.isImpact ? 0.85 : 0.4
            }
            // Short tag at the foot of the tick.
            Text {
                visible: root._inDom(ph.modelData.t_us)
                x: ph.tx + Theme.sp(3)
                y: root._plotBottom - height - Theme.sp(2)
                text: labels.phaseShortTag(ph.modelData.phase)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: ph.isImpact ? Theme.colorText2 : Theme.colorText3
            }
        }
    }

    // Optional thin frame around the plot rect (split facets read as separate cards).
    Rectangle {
        visible: root.showFrame
        x: root._plotLeft; y: root._plotTop
        width: root._plotW; height: root._plotH
        color: "transparent"; border.width: 1; border.color: Theme.colorBorder
    }

    // ── Clipped drawing area: traces / dots / playhead / crosshair ─────────────────
    Item {
        id: clipArea
        x: root._plotLeft; y: root._plotTop
        width: root._plotW; height: root._plotH
        clip: true

        // Traces — one Shape per RUN (see _traceRuns): a series is one run when everything in it
        // was measured, which is every series that predates the validity mask.
        Repeater {
            model: root._traceRuns
            delegate: Shape {
                id: curve
                required property var modelData
                anchors.fill: parent
                // ⚠ RENDERER PER RUN, deliberately. The curve renderer is what gives the solid
                // traces their quality, but dash patterns are a stroking feature of the geometry
                // renderer; asking the curve renderer for a DashLine gets a solid line and the
                // distinction the design rests on disappears silently. Solid runs keep the good
                // renderer; the dimmed dashed runs take the plainer one, which is invisible at
                // 0.35 opacity on a 2px line.
                preferredRendererType: curve.modelData.dashed ? Shape.GeometryRenderer
                                                              : Shape.CurveRenderer
                // Same colour, low opacity: the reader is meant to follow the curve across a
                // bridged run, not lose it. A different HUE would read as a different series.
                opacity: curve.modelData.dashed ? 0.35 : 1.0
                ShapePath {
                    strokeColor: curve.modelData.color
                    strokeWidth: Theme.sp(2)
                    fillColor:   "transparent"
                    joinStyle:   ShapePath.RoundJoin
                    capStyle:    ShapePath.RoundCap
                    strokeStyle: curve.modelData.dashed ? ShapePath.DashLine
                                                        : ShapePath.SolidLine
                    // Dash lengths are MULTIPLES OF strokeWidth, so this is ~8px on / ~6px off
                    // at the 2px stroke above — long enough to read as deliberate at a glance,
                    // short enough that a 40ms bridged run still shows two dashes.
                    dashPattern: [4, 3]
                    PathPolyline {
                        path: (curve.modelData.t || []).map(function (t, i) {
                            return Qt.point(root.xForT(t),
                                            root.yForV(curve.modelData.v[i]))
                        })
                    }
                }
            }
        }

        // P-position dots (band-coloured), per series.
        Repeater {
            model: root.showDots ? root.series : []
            delegate: Repeater {
                id: dots
                required property var modelData
                model: dots.modelData.phaseSamples || []
                delegate: Rectangle {
                    id: dot
                    required property var modelData
                    readonly property real r: Theme.sp(3.2)
                    // NO DOT ON AN UNMEASURED SAMPLE, and none outside the metric's domain. The
                    // producers stopped emitting those phaseSamples (design §5.1), but a swing
                    // analysed before that still has them persisted, and a band-coloured dot is
                    // the single most confident thing on this chart — it says "graded reading
                    // here". It must not sit on a bridged value or on a foreshortened body line.
                    //
                    // Held in its own property rather than inlined into `visible`: `visible` also
                    // depends on the view window, which changes on every frame of a brush drag,
                    // and measuredAt marshals the whole series across the QML boundary. This way
                    // it is evaluated once per dot, when the data changes.
                    readonly property bool measured:
                        cm.measuredAt(dots.modelData.t_us, dots.modelData.valid || [],
                                      dot.modelData.t_us,
                                      root._hasDomain(dots.modelData) ? dots.modelData.validFromUs : 0,
                                      root._hasDomain(dots.modelData) ? dots.modelData.validToUs   : 0)
                    visible: dot.measured && root._inDom(dot.modelData.t_us)
                    width: 2 * r; height: 2 * r; radius: r
                    x: root.xForT(dot.modelData.t_us) - r
                    y: root.yForV(dot.modelData.value) - r
                    color: root._bandColor(dot.modelData.band)
                    border.width: Theme.sp(1.5); border.color: Theme.colorBg
                }
            }
        }

        // Playhead — clipped when outside the domain.
        Rectangle {
            visible: root.showPlayhead && root._inDom(root.playheadUs)
            width: Theme.sp(1.5); height: parent.height
            x: root.xForT(root.playheadUs) - width / 2
            color: Theme.colorText; opacity: 0.85
        }

        // Shared hover crosshair + per-series markers.
        Rectangle {
            visible: root.showCrosshair && root.cursorUs >= 0 && root._inDom(root.cursorUs)
            width: 1; height: parent.height
            x: root.xForT(root.cursorUs)
            color: Theme.colorText2; opacity: 0.55
        }
        Repeater {
            model: (root.showCrosshair && root.cursorUs >= 0 && root._inDom(root.cursorUs))
                   ? root.series : []
            delegate: Rectangle {
                id: cmark
                required property var modelData
                readonly property real r: Theme.sp(3)
                readonly property real cv: labels.valueAtNearest(cmark.modelData.t_us,
                                                                 cmark.modelData.value, root.cursorUs)
                // The readout beside it prints "—" here (PpMetricChart._valueTextAt), so the
                // marker has to go too: a dot pinned to the bridged value with a dash beside it
                // would tell the reader the number exists and is merely being withheld.
                //
                // JS, not cm.measuredAt: this binding re-evaluates on EVERY cursor move, and the
                // C++ form would copy every series across the QML boundary each time.
                visible: root._measuredAtT(cmark.modelData, root.cursorUs)
                width: 2 * r; height: 2 * r; radius: r
                x: root.xForT(root.cursorUs) - r
                y: root.yForV(cv) - r
                color: cmark.modelData.color
                border.width: Theme.sp(1.5); border.color: Theme.colorBg
            }
        }

        // Hover tracking — NoButton so the seek handlers below own the press while
        // this keeps the shared crosshair alive.
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            onPositionChanged: (m) => root.hoverMoved(
                root.domStartUs + (m.x / clipArea.width) * (root.domEndUs - root.domStartUs))
            onExited: root.hoverExited()
        }

        // Click/drag-to-seek — mirrors the transit timeline's scrub surface so the
        // chart is a second master seek surface (gated on seekEnabled). A tap seeks
        // (preserving play state); a drag brackets a scrub so playback pauses while
        // dragging and resumes after if it was playing. hoverMoved keeps the crosshair
        // riding with the playhead through a drag (the hover MouseArea above freezes
        // once the drag grab is taken).
        function _seekT(mx) {
            return root.domStartUs
                 + Math.max(0, Math.min(1, mx / clipArea.width)) * (root.domEndUs - root.domStartUs)
        }
        DragHandler {
            enabled: root.seekEnabled
            target: null
            dragThreshold: 0
            cursorShape: Qt.PointingHandCursor
            onActiveChanged: active ? root.scrubBegan() : root.scrubEnded()
            onCentroidChanged: if (active) {
                var t = clipArea._seekT(centroid.position.x)
                root.seekRequested(t)
                root.hoverMoved(t)
            }
        }
        TapHandler {
            enabled: root.seekEnabled
            onTapped: (ep) => root.seekRequested(clipArea._seekT(ep.position.x))
        }
    }

    // ── X axis (ms relative to impact) ────────────────────────────────────────────
    Repeater {
        model: root.showXAxis ? cm.timeTicksMs(root.domStartUs, root.domEndUs, root.impactUs) : []
        delegate: Text {
            id: xt
            required property var modelData
            x: root._plotLeft + root.xForT(root.impactUs + xt.modelData * 1000) - width / 2
            y: root._plotBottom + Theme.sp(4)
            text: (xt.modelData > 0 ? "+" : "") + xt.modelData
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }
    }
    Text {
        visible: root.showXAxis
        anchors.right: parent.right; anchors.rightMargin: root.padR
        y: root._plotBottom + Theme.sp(4)
        text: qsTr("ms ← impact")
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        font.letterSpacing: Theme.trackingData
        color: Theme.colorText2
    }
}
