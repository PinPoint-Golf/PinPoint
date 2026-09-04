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

// PpSegmentBrush — whole-swing overview strip with a draggable selection window. Shows a
// faint per-series sparkline (each normalised to its own range) and the swing-phase ticks
// across the full extent, then a movable/resizable window the user drags to scope the
// chart. Emits windowChanged(startUs,endUs) as the body is dragged, an edge handle is
// pulled, or a fresh window is swept out — the parent writes that into its chart-local
// viewStartUs/viewEndUs (never shotReplay / SwingDataSource). Pure scale/path binding;
// per-series ranges come from ChartMetrics.summaryMasked, phase tags from TimelineLabels.
//
// It obeys the same validity contract as the chart it scopes (design
// metric_presentation_honesty.md §5.1): the y-range excludes bridged samples, and bridged /
// out-of-domain runs are drawn dashed and dimmer rather than as measurements. A sparkline is
// small, but it is the surface a reader uses to DECIDE WHERE TO LOOK, so a single bridged
// outlier flattening the whole trace into a spike sends them to the wrong place.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // series: [{ t_us, value, color, valid?, validFromUs?, validToUs?, … }] (the decorated,
    // currently-visible series). The last three are the optional validity triple PpMetricChart
    // decorates on — see _measured below; absent ⇒ every sample is a measurement.
    property var  series: []
    property var  phases: []
    property real axisStartUs: 0
    property real axisEndUs:   1
    property real winStartUs:  0
    property real winEndUs:    1
    property real impactUs:    0
    property real minWidthUs:  40000      // clamp so the window can't collapse

    // NB: not "windowChanged" — that collides with QQuickItem's inherited window NOTIFY.
    signal viewWindowChanged(real startUs, real endUs)

    implicitHeight: Theme.sp(64)

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }

    // ── Geometry + scales ─────────────────────────────────────────────────────────
    readonly property real _plotL: Theme.sp(8)
    readonly property real _plotR: width - Theme.sp(8)
    readonly property real _plotT: Theme.sp(6)
    readonly property real _plotB: height - Theme.sp(15)     // room for phase tags
    readonly property real _plotW: Math.max(1, _plotR - _plotL)
    readonly property real _plotH: Math.max(1, _plotB - _plotT)
    readonly property real _hit:   Theme.sp(7)               // edge-grab tolerance (px)

    function xs(t) {
        return _plotL + (axisEndUs > axisStartUs
                         ? (t - axisStartUs) / (axisEndUs - axisStartUs) * _plotW : 0)
    }
    function toT(px) {
        var f = (px - _plotL) / _plotW
        return Math.max(axisStartUs, Math.min(axisEndUs, axisStartUs + f * (axisEndUs - axisStartUs)))
    }

    // ── Frame ─────────────────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        radius: Theme.sp(9)
        color: Theme.colorBg
        border.width: 1; border.color: Theme.colorBorder
    }

    // ── Measured vs bridged — the same rule as PpChartPlot, in its index form ─────
    //
    // A sample counts as measured when it is inside the metric's phase domain and not marked 0 in
    // `valid`. Short-mask rule as everywhere else (chart_metrics.h): a mask that does not cover the
    // curve is discarded wholesale rather than applied to the part it covers.
    function _hasDomain(s) {
        return s.validFromUs !== undefined && s.validToUs !== undefined
               && s.validToUs > s.validFromUs
    }
    function _measured(s, i) {
        if (root._hasDomain(s) && (s.t_us[i] < s.validFromUs || s.t_us[i] > s.validToUs))
            return false
        if (!s.valid || s.valid.length < s.t_us.length) return true
        return s.valid[i] !== 0
    }

    // Each series split into runs of one drawn state, flattened across all series:
    //   [{ color, dashed, t:[…], v:[…], lo, hi }]
    // `lo`/`hi` are the series' y-range over its MEASURED samples only, carried on every run of
    // that series so each run normalises identically. Mirrors PpChartPlot._traceRuns, including
    // the rule that a dashed run reaches one sample into each solid neighbour so the trace stays
    // continuous and every segment touching an unmeasured sample is itself dashed.
    readonly property var _sparkRuns: {
        var out = []
        for (var k = 0; k < (root.series ? root.series.length : 0); ++k) {
            var s = root.series[k]
            if (!s || !s.t_us || !s.value || s.t_us.length < 2) continue

            var solid = []
            for (var i = 0; i < s.t_us.length; ++i) solid.push(root._measured(s, i))

            // ⚠ THE RANGE IS OVER MEASURED SAMPLES ONLY, and it comes from the SAME reducer the
            // cards use rather than a fourth hand-rolled min/max — one implementation, so the
            // strip and the chart cannot disagree about a series' extent. Scaled to include a
            // bridged outlier the whole sparkline collapsed to a flat line with one spike, which
            // is exactly the wrong picture: the reader is choosing a window from this shape.
            //
            // The window is the axis clipped to the metric's domain, per side, using the instants
            // PpMetricChart already resolved — so out-of-domain samples do not set the scale either.
            var winA = root._hasDomain(s) ? Math.max(root.axisStartUs, s.validFromUs)
                                          : root.axisStartUs
            var winB = root._hasDomain(s) ? Math.min(root.axisEndUs, s.validToUs)
                                          : root.axisEndUs
            var st = cm.summaryMasked(s.t_us, s.value, s.valid || [], winA, Math.max(winA, winB))
            var lo = st.min, hi = st.max
            if (!(hi > lo)) { lo = st.min; hi = st.min + 1 }   // flat or nothing to scale by

            var i0 = 0
            while (i0 < solid.length) {
                var i1 = i0
                while (i1 + 1 < solid.length && solid[i1 + 1] === solid[i0]) ++i1
                var a = i0, b = i1
                if (!solid[i0]) {
                    if (a > 0) --a
                    if (b + 1 < solid.length) ++b
                }
                if (b > a) {
                    var t = [], v = []
                    for (var j = a; j <= b; ++j) { t.push(s.t_us[j]); v.push(s.value[j]) }
                    out.push({ color: s.color, dashed: !solid[i0], t: t, v: v, lo: lo, hi: hi })
                }
                i0 = i1 + 1
            }
        }
        return out
    }

    // ── Faint per-series sparklines (each normalised to its own MEASURED range) ────
    Repeater {
        model: root._sparkRuns
        delegate: Shape {
            id: spark
            required property var modelData
            anchors.fill: parent
            // 0.5 is this strip's own faintness; a bridged run is dimmer again by the same 0.35
            // factor the chart uses, so the two surfaces read as one convention.
            opacity: spark.modelData.dashed ? 0.5 * 0.35 : 0.5
            // Dashes come from the geometry renderer — see PpChartPlot's note; the curve renderer
            // silently draws a DashLine solid and the distinction would vanish.
            preferredRendererType: spark.modelData.dashed ? Shape.GeometryRenderer
                                                          : Shape.CurveRenderer
            // ⚠ CLAMPED TO THE STRIP, and the clamp is load-bearing since Phase 2. `lo`/`hi` are
            // ChartMetrics' min/max, which are now the extremes of the 40 ms WINDOWED MEAN — by
            // construction strictly inside the raw range they used to be. The polyline still draws
            // the RAW samples, so an excursion sharper than 40 ms maps outside [lo, hi]: on
            // rich_7iron shoulderPlaneAngle reached 11.9 % of the strip height above the band and
            // hipLineTilt 24 % below it, drawing over the phase tags underneath.
            //
            // Clamping rather than rescaling to the raw extremes on purpose: the outlier-resistant
            // scale is the whole point of the choice made at :119-123 — scaled to include a bridged
            // outlier the sparkline collapses to a flat line with one spike, which is the wrong
            // picture for someone choosing a window from this shape. A clipped peak reads as "it
            // goes off the top here", which is true; a flattened curve reads as "nothing happens
            // here", which is not.
            function ys(v) {
                var lo = spark.modelData.lo, hi = spark.modelData.hi
                var y = (hi > lo) ? root._plotT + root._plotH - (v - lo) / (hi - lo) * root._plotH
                                  : root._plotT + root._plotH / 2
                return Math.max(root._plotT, Math.min(root._plotB, y))
            }
            ShapePath {
                strokeColor: spark.modelData.color
                strokeWidth: 1
                fillColor:   "transparent"
                joinStyle:   ShapePath.RoundJoin
                strokeStyle: spark.modelData.dashed ? ShapePath.DashLine : ShapePath.SolidLine
                dashPattern: [4, 3]
                PathPolyline {
                    path: (spark.modelData.t || []).map(function (t, i) {
                        return Qt.point(root.xs(t), spark.ys(spark.modelData.v[i]))
                    })
                }
            }
        }
    }

    // ── Phase ticks + tags ────────────────────────────────────────────────────────
    Repeater {
        model: root.phases
        delegate: Item {
            id: pt
            required property var modelData
            readonly property bool isImpact: pt.modelData.phase === 5
            readonly property real tx: root.xs(pt.modelData.t_us)
            Rectangle {
                x: pt.tx; y: root._plotT
                width: 1; height: root._plotH
                color: pt.isImpact ? Theme.colorAccent : Theme.colorBorderMid
                opacity: pt.isImpact ? 0.7 : 0.3
            }
            Text {
                x: pt.tx + Theme.sp(2); y: root._plotB + Theme.sp(2)
                text: labels.phaseShortTag(pt.modelData.phase)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: pt.isImpact ? Theme.colorText2 : Theme.colorText3
            }
        }
    }

    // ── Selection window: dim sides + body + outline + handles ─────────────────────
    readonly property real _xL: root.xs(root.winStartUs)
    readonly property real _xR: root.xs(root.winEndUs)

    Rectangle {                                   // dim left of window
        x: root._plotL; y: root._plotT
        width: Math.max(0, root._xL - root._plotL); height: root._plotH
        color: Theme.colorBg2; opacity: 0.7
    }
    Rectangle {                                   // dim right of window
        x: root._xR; y: root._plotT
        width: Math.max(0, root._plotR - root._xR); height: root._plotH
        color: Theme.colorBg2; opacity: 0.7
    }
    Rectangle {                                   // window body fill
        x: root._xL; y: root._plotT
        width: Math.max(1, root._xR - root._xL); height: root._plotH
        color: Theme.colorAccent; opacity: 0.06
    }
    Repeater {                                     // L / R edge handles
        model: [{ x: root._xL }, { x: root._xR }]
        delegate: Item {
            id: handle
            required property var modelData
            Rectangle {
                x: handle.modelData.x - Theme.sp(3); y: root._plotT
                width: Theme.sp(6); height: root._plotH
                color: Theme.colorAccent; opacity: 0.85
            }
            Rectangle {                            // grip pip
                x: handle.modelData.x - 1; y: root._plotT + root._plotH / 2 - Theme.sp(6)
                width: 2; height: Theme.sp(12)
                color: Theme.colorSurface; opacity: 0.9
            }
        }
    }

    // ── Drag interaction ──────────────────────────────────────────────────────────
    property string _mode: ""        // "L" | "R" | "move" | "new"
    property real   _origStart: 0
    property real   _origEnd:   0
    property real   _anchorT:   0

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: {
            if (root._mode === "L" || root._mode === "R") return Qt.SizeHorCursor
            var px = mouseX
            if (Math.abs(px - root._xL) < root._hit || Math.abs(px - root._xR) < root._hit)
                return Qt.SizeHorCursor
            if (px > root._xL && px < root._xR) return Qt.OpenHandCursor
            return Qt.CrossCursor
        }

        onPressed: (m) => {
            var px = m.x
            root._origStart = root.winStartUs
            root._origEnd   = root.winEndUs
            root._anchorT   = root.toT(px)
            if (Math.abs(px - root._xL) < root._hit)        root._mode = "L"
            else if (Math.abs(px - root._xR) < root._hit)   root._mode = "R"
            else if (px > root._xL && px < root._xR)         root._mode = "move"
            else {
                root._mode = "new"
                root.viewWindowChanged(root._anchorT, root._anchorT)
            }
        }
        onPositionChanged: (m) => {
            if (root._mode === "")  return
            var t = root.toT(m.x)
            var s = root.winStartUs, e = root.winEndUs
            if (root._mode === "L")        { s = Math.min(t, root.winEndUs - root.minWidthUs) }
            else if (root._mode === "R")   { e = Math.max(t, root.winStartUs + root.minWidthUs) }
            else if (root._mode === "new") {
                s = Math.min(root._anchorT, t); e = Math.max(root._anchorT, t)
            } else if (root._mode === "move") {
                var off = t - root._anchorT
                s = root._origStart + off; e = root._origEnd + off
                if (s < root.axisStartUs) { e += root.axisStartUs - s; s = root.axisStartUs }
                if (e > root.axisEndUs)   { s -= e - root.axisEndUs;   e = root.axisEndUs }
            }
            if (e - s < root.minWidthUs) e = s + root.minWidthUs
            root.viewWindowChanged(s, e)
        }
        onReleased: root._mode = ""
        onCanceled: root._mode = ""
    }

    // Hint text (top-right).
    Text {
        anchors { top: parent.top; right: parent.right; topMargin: Theme.sp(5); rightMargin: Theme.sp(8) }
        text: qsTr("drag to select · drag edges to resize")
        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
        font.letterSpacing: Theme.trackingMicro
        color: Theme.colorText3
    }
}
