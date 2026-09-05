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
    //            valid?:[0|1…], validFromUs?, validToUs?, sigma?, mean?:[…], meanSigma?:[…] }]
    //
    // `mean` is THE CURVE THIS PLOT STROKES (design §4 principle 1, Phase 6): the 40 ms centred
    // windowed mean at every sample, from ChartMetrics.windowedMean, which is the same array
    // ChartMetrics.summaryMasked reduces to produce the PEAK tile — so the line and the numbers are
    // one reduction and cannot drift apart. OPTIONAL: a caller that has not decorated it gets the
    // persisted `value` stroked instead, exactly as before this phase (see _meanOf). `meanSigma` is
    // the per-sample standard error of that mean; nothing in here reads it yet — the ±σ ribbon quotes
    // the SERIES' σ, which is a different statement (the instrument's noise, not this window's).
    //
    // `sigma` is the series' 1σ MEASUREMENT noise in its own unit (§5.3) and is read by exactly one
    // thing in here, the ±σ ribbon below. Optional, and ABSENT means never characterised rather
    // than zero — a series without it simply gets no ribbon.
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

    // ── THE RAW SAMPLES, BEHIND THE LINE — ON BY DEFAULT (Phase 6) ─────────────────
    //
    // One faint dot per MEASURED sample at its persisted value. This is what makes drawing a
    // reduction legitimate instead of a quiet filter: the stroke is the 40 ms windowed mean (the
    // number the tiles report), and the dots are what was actually recorded, on the same axes, at the
    // same time. A reader can see the wobble the reduction declined to call a peak, and the distance
    // between the dots and the line IS the reduction — visible, not described.
    //
    // ⚠ DEFAULT TRUE, unlike showSigmaBand. The ribbon is an undecided visual (design §8); these
    // dots are the honesty half of a change that alters the drawn line, so shipping the line without
    // them would be shipping the half that hides something. The property exists for a caller that
    // wants the bare reduced shape (a compact strip, a thumbnail), not as a preference to persist.
    //
    // Nothing is drawn for a series with no `mean`: there the stroke IS the raw curve and the dots
    // would just double it.
    property bool showRawDots:   true

    // ── THE ±σ RIBBON — OFF, AND OFF FOR A REASON (design §5.3, §8) ────────────────
    //
    // A band of ± the series' own measurement σ around the measured runs of its curve. It is the
    // only way to SHOW that a wobble is inside the noise rather than telling the reader so in
    // words, and design §8 lists it among the decisions still open — "keep dark until seen". So it
    // defaults false, nothing in the app switches it on, and the one thing that does is a probe
    // setting the property (tools/probes/plumb_bob_chart.qml → PpMetricChart.showSigmaBand). There
    // is no UI toggle on purpose: a toggle is a shipped visual and a persisted preference, and
    // neither has been agreed.
    //
    // ⚠ IT IS NOT A SMOOTHER AND IT MOVES NOTHING. The stroke still passes through every persisted
    // sample at its persisted value; the ribbon is drawn BEHIND it (declared before the traces) and
    // is purely additive. Display-only smoothing is forbidden outright (design §4 principle 1).
    property bool showSigmaBand: false

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
    // ⚠ THIS IS NOT A FILTER. Every persisted sample is on screen — as a raw dot at its persisted
    // value (see showRawDots), and the bridged runs draw the persisted value on the stroke as well
    // (an invalid sample's `mean` entry IS its raw value). What the stroke shows through the measured
    // runs is the 40 ms windowed MEAN, which is not display-only smoothing but the reduction the PEAK
    // tile reports: design §4 principle 1 forbids the chart showing one thing while the card computes
    // another, and Phase 6's answer is to make them the same thing rather than to leave the line raw
    // and the tile windowed — which is what the reader was being asked to reconcile before.
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
    //
    // ⚠ A NON-FINITE VALUE IS NOT A MEASUREMENT EITHER (F5), the same fold SeriesView::isValid makes
    // on the C++ side and for the same reason: nothing in the pipeline should produce a NaN, which is
    // exactly why it must not be trusted as an invariant. Here it is also a rendering matter — one
    // NaN in a ShapePath's point list takes out the WHOLE run, so a single bad sample would silently
    // erase a series' trace, its dots or its ribbon rather than showing a gap where it is.
    function _measured(s, i) {
        if (root._hasDomain(s) && (s.t_us[i] < s.validFromUs || s.t_us[i] > s.validToUs))
            return false
        if (!isFinite(s.value[i])) return false
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
    // WHICH ARRAY IS THE CURVE: `mean` where the host decorated one (PpMetricChart._plottable), the
    // persisted `value` otherwise. A "which array" choice rather than arithmetic, so it stays in QML;
    // the identical pair is in PpMetricChart and PpSegmentBrush because all three are handed a
    // `series` list and none of them may assume the others prepared it.
    //
    // The predicate is separate from the getter, and asked by name rather than by comparing the
    // returned array against s.value: an array identity test would be answering "is there a
    // reduction here" with a question about object lifetimes across the QML/C++ boundary, which is
    // not something a drawing decision should depend on.
    function _hasMean(s) {
        return !!(s && s.mean && s.t_us && s.mean.length === s.t_us.length)
    }
    function _meanOf(s) {
        return root._hasMean(s) ? s.mean : s.value
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
    //
    // ⚠ THE VALUES ARE `mean`, NOT `value` (Phase 6). One array for every run, and a dashed run still
    // shows the PERSISTED numbers, because windowedMean puts the raw value at every sample its mask
    // marked invalid — so the bridged and out-of-domain regions draw exactly what they drew before
    // this phase. The seam between a dashed and a solid run stays continuous because both runs read
    // the same array at the shared endpoint (a valid sample, hence a mean on both sides).
    //
    // ⚠ THAT DEPENDS ON THE HOST HAVING COMPOSED THE MASK (F4), and it is the reason
    // PpMetricChart._reduceMask exists. `_measured` below is `valid` AND in-domain, while
    // windowedMean only knows the mask it was given: hand it `valid` alone and on a swing whose
    // producer never marked the out-of-domain run, THAT run would be dashed at a mean rather than at
    // its persisted value, and its samples would be averaged into the in-domain anchors beside the
    // boundary as well. The host therefore reduces through `valid` ∧ in-domain, so "invalid to the
    // reducer" and "unmeasured on this plot" are the same set of samples. An undecorated caller
    // (no `mean`) draws the raw curve and none of this arises.
    readonly property var _traceRuns: {
        var out = []
        for (var k = 0; k < (root.series ? root.series.length : 0); ++k) {
            var s = root.series[k]
            if (!s || !s.t_us || !s.value || s.t_us.length < 2) continue
            var mv = root._meanOf(s)

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
                    // NON-FINITE POINTS ARE OMITTED FROM THE RUN, not drawn (F5). _measured already
                    // refuses to call them measurements, but a DASHED run draws the persisted value —
                    // and a NaN there is not a dash, it is the loss of every segment in the run. The
                    // line closes over the omitted sample exactly as it closes over a bridge.
                    for (var j = a; j <= b; ++j)
                        if (isFinite(mv[j])) { t.push(s.t_us[j]); v.push(mv[j]) }
                    if (t.length > 1) out.push({ color: s.color, dashed: !solid[i0], t: t, v: v })
                }
                i0 = i1 + 1
            }
        }
        return out
    }

    // ── THE RAW SAMPLES' GEOMETRY (Phase 6) ───────────────────────────────────────
    //
    // Per series, the MEASURED samples' times and PERSISTED values as { color, t[], v[] } — index
    // data, not screen points, for the same reason _traceRuns holds indices: a resize or a Y-range
    // change then re-runs only the cheap coordinate map in the path binding below.
    //
    // Unmeasured samples get no dot. A bridged or out-of-domain sample is not a measurement, so there
    // is nothing there to show as one — and the dashed stroke already draws its persisted value, so
    // the number is not being hidden either. Same predicate as everything else on this plot
    // (_measured), so the dots, the dashes, the phase dots and the crosshair cannot disagree about
    // which samples are readings.
    //
    // Skipped entirely when the series has no `mean`: the stroke is then the raw curve itself and a
    // dot on every sample would just thicken it. Also skipped, at one boolean test, when the dots are
    // off — no pass over any series.
    readonly property var _rawDots: {
        var out = []
        if (!root.showRawDots) return out
        for (var k = 0; k < (root.series ? root.series.length : 0); ++k) {
            var s = root.series[k]
            if (!s || !s.t_us || !s.value || s.t_us.length < 2) continue
            if (!root._hasMean(s)) continue               // no reduction ⇒ nothing to show behind
            var t = [], v = []
            for (var i = 0; i < s.t_us.length; ++i)
                if (root._measured(s, i)) { t.push(s.t_us[i]); v.push(s.value[i]) }
            if (t.length > 0) out.push({ color: s.color, t: t, v: v })
        }
        return out
    }
    // How many series are drawing raw dots — 0 when they are off, when no series carries a `mean`,
    // and when nothing was measured. Read by the plumb-bob probe (which duck-types for it while
    // walking the chart) so a probe run can REPORT that the dots drew rather than assert it from the
    // property being set, exactly as it does for sigmaBandRuns. Not used by any binding.
    readonly property int rawDotRuns: root._rawDots.length

    // The ±σ ribbon's geometry: per series, its MEASURED runs as { color, sigma, t[], v[] }, or an
    // empty list when the band is off. Only the measured runs get one, for the same reason the
    // dashed stroke exists — a bridged sample is not a measurement, so it has no measurement noise
    // to draw, and a band spanning the bridge would put an error bar on a number nobody measured.
    // Runs of one sample are dropped: a polygon needs two.
    //
    // A series with NO σ contributes nothing at all. Absent means "never characterised" (§5.3), and
    // the one thing a chart must not do with that is draw a zero-width ribbon, which reads as a
    // measurement of perfect precision. Nothing drawn is the honest picture of nothing known.
    //
    // The whole walk is behind `showSigmaBand`, so the default-off case costs one boolean test per
    // re-evaluation rather than a pass over every series.
    //
    // ⚠ CENTRED ON `mean` SINCE PHASE 6, not on the raw samples: the band is ± the series' σ around
    // THE DRAWN LINE, and a ribbon offset from the stroke it belongs to would read as a second curve.
    readonly property var _sigmaRuns: {
        var out = []
        if (!root.showSigmaBand) return out
        for (var k = 0; k < (root.series ? root.series.length : 0); ++k) {
            var s = root.series[k]
            if (!s || !s.t_us || !s.value || s.t_us.length < 2) continue
            var mv = root._meanOf(s)
            // ChartMetrics.seriesSigma, not a local guard: one implementation of "absent means
            // absent" (it was three). Called once per series here, on a binding that changes with
            // the DATA — never per frame; it marshals the whole series.
            var sg = cm.seriesSigma(s)
            if (!(sg > 0)) continue
            var t = [], v = []
            for (var i = 0; i < s.t_us.length; ++i) {
                if (root._measured(s, i)) {
                    t.push(s.t_us[i]); v.push(mv[i])
                } else if (t.length > 0) {
                    if (t.length > 1) out.push({ color: s.color, sigma: sg, t: t, v: v })
                    t = []; v = []
                }
            }
            if (t.length > 1) out.push({ color: s.color, sigma: sg, t: t, v: v })
        }
        return out
    }
    // How many ribbon polygons this plot is drawing — 0 whenever the band is off, whenever no
    // visible series carries a σ, and whenever nothing was measured. Read by the plumb-bob probe
    // (which duck-types for this property while walking the chart) so a probe run can REPORT that
    // the ribbon drew rather than assert it from the property being set. Not used by any binding.
    readonly property int sigmaBandRuns: root._sigmaRuns.length

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

        // ±σ ribbon — DECLARED FIRST so it paints BEHIND the traces, the dots and the playhead
        // (QQuickItem paints siblings in declaration order and none of these set z). A band drawn
        // over the stroke would obscure the one thing on this plot that is a measurement.
        //
        // 0.06 opacity: at that weight it is a tint the eye reads as "region", not as a second
        // curve, and two overlapping series' bands still resolve. See root.showSigmaBand for why
        // this is off by default and has no control.
        Repeater {
            model: root._sigmaRuns
            delegate: Shape {
                id: band
                required property var modelData
                anchors.fill: parent
                preferredRendererType: Shape.CurveRenderer
                opacity: 0.06
                ShapePath {
                    // Fill only. A negative strokeWidth is ShapePath's own "do not stroke", and it
                    // matters here: a 1px outline at this fill weight would be the most visible
                    // part of the band and would read as two extra curves.
                    strokeWidth: -1
                    strokeColor: "transparent"
                    fillColor:   band.modelData.color
                    PathPolyline {
                        // The +σ edge forward, the −σ edge back, then closed on the first point.
                        // One polygon per measured run, in plot coordinates — so a Y-range change
                        // or a resize re-runs only this map, not the run splitting above.
                        path: {
                            var pts = [], m = band.modelData, n = m.t.length, i
                            for (i = 0; i < n; ++i)
                                pts.push(Qt.point(root.xForT(m.t[i]),
                                                  root.yForV(m.v[i] + m.sigma)))
                            for (i = n - 1; i >= 0; --i)
                                pts.push(Qt.point(root.xForT(m.t[i]),
                                                  root.yForV(m.v[i] - m.sigma)))
                            if (pts.length > 0) pts.push(pts[0])
                            return pts
                        }
                    }
                }
            }
        }

        // Raw samples — ONE Shape per series, drawn after the ribbon and BEFORE the traces so the
        // reduced line sits on top of the measurements it reduces.
        //
        // ⚠ WHY ONE SHAPE AND A PathMultiline, and not a Repeater of dots. The All preset can put 25
        // series of ~250 samples on one plot: a Repeater over points is ~6 000 QQuickRectangles, each
        // with its own x/y bindings re-evaluated on every resize and every brush frame, which is not
        // affordable and would make the honest half of this phase the expensive half. A PathMultiline
        // takes a LIST OF POLYLINES in one property, so the whole dot field of a series is one path
        // element inside one ShapePath: 25 Shapes for the plot, and the per-frame work is the same
        // array map the trace already does (one Qt.point pair per sample) with no item creation at all.
        //
        // Each dot is a 1px horizontal segment with a ROUND CAP of the dot's diameter — the cheapest
        // shape Qt Quick Shapes can express as a dot, and a real (non-degenerate) segment because a
        // zero-length one is not guaranteed to raster at all. So the mark is a capsule about
        // (2r + 1) × 2r px, which at 0.35 opacity reads as a dot.
        //
        // ⚠ THE RADIUS HAS A ONE-PIXEL FLOOR (F7). Theme.sp() rounds, so sp(1.2) is 1 at the default
        // scale and 0 on a smaller one — and a zero stroke width does not draw a smaller dot, it
        // draws NOTHING. The honesty half of this phase would then be silently absent on exactly the
        // displays where the reduction is hardest to see. Math.max(1, …) makes the failure impossible
        // rather than unlikely; the dot cannot get thinner than a pixel, which is the right floor for
        // a mark whose job is to be present.
        Repeater {
            model: root._rawDots
            delegate: Shape {
                id: rawd
                required property var modelData
                readonly property real r: Math.max(1, Theme.sp(1.2))
                // Read back by the plumb-bob probe: the number of polylines the PathMultiline is
                // actually holding after the assignment, which is the only form of "the dots drew"
                // that is not just the JS model restated. −1 means the property came back in a shape
                // this cannot count, which is itself worth reporting (the paths conversion is the one
                // part of this that no test can reach).
                readonly property int pathCount: {
                    var pp = dotPath.paths
                    return (pp && pp.length !== undefined) ? pp.length : -1
                }
                objectName: "rawDots"
                anchors.fill: parent
                // The GEOMETRY renderer on purpose: these are 2px round caps at a third opacity, where
                // the curve renderer's antialiasing quality buys nothing and its per-segment curve
                // geometry would be paid thousands of times per plot.
                preferredRendererType: Shape.GeometryRenderer
                // Same colour as the trace, faint: the dots are the same measurement as the line, not
                // a second series. A different hue would read as one.
                opacity: 0.35
                ShapePath {
                    strokeColor: rawd.modelData.color
                    strokeWidth: 2 * rawd.r
                    capStyle:    ShapePath.RoundCap
                    fillColor:   "transparent"
                    PathMultiline {
                        id: dotPath
                        // One two-point polyline per sample, in plot coordinates — so a Y-range change
                        // or a resize re-runs only this map and not the sample walk in _rawDots.
                        paths: {
                            var out = [], m = rawd.modelData, n = m.t.length
                            for (var i = 0; i < n; ++i) {
                                var x = root.xForT(m.t[i]), y = root.yForV(m.v[i])
                                out.push([Qt.point(x - 0.5, y), Qt.point(x + 0.5, y)])
                            }
                            return out
                        }
                    }
                }
            }
        }

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
                    // ⚠ AND THEY KEEP THEIR PERSISTED VALUES — they are NOT moved onto the mean the
                    // trace now draws (Phase 6). A phaseSample is the PRODUCER's graded reading at
                    // that landmark, the thing a corridor scored and a coach was told; re-stating it
                    // as our window mean would silently overwrite someone else's measurement with our
                    // display arithmetic. A dot therefore sits a little off the line where the
                    // reduction moved, which is true and is exactly what the raw dots behind it show.
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
                // ON THE MEAN, because that is where the line is (Phase 6) and this marker's job is
                // to say "the curve is here at this instant". The readout beside it prints the same
                // mean and the raw sample after it (PpMetricChart._valueTextAt / _rawTextAt), so the
                // marker, the line and the two numbers are one statement.
                readonly property real cv: labels.valueAtNearest(cmark.modelData.t_us,
                                                                 root._meanOf(cmark.modelData),
                                                                 root.cursorUs)
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
