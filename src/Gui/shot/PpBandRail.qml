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

// PpBandRail — the dashboard's primary time-series read, and the reason the Motion
// zone shows no line charts. A curve answers "what happened"; a rail answers "was it
// in bounds, and where did it leave" — which is the only question a post-shot glance
// has time for. Checkpoints sit EVENLY SPACED in position order (P1…P8), not on a
// real-time axis: the coach reads positions, and real time would crush the whole
// downswing into the right-hand tenth of the tile.
//
// Every number painted here comes from C++ (ChartMetrics.railCheckpoints / railRange /
// valueAtUs / summary → dashboard_reductions.h); QML positions and paints only. The
// raw curve is hover-revealed, never resting furniture — so the non-hovered render is
// identical to the auto-closing wall cast, which is the guardrail that lets one
// component serve both surfaces.
//
// Degradation: no corridor anywhere ⇒ the rail becomes a phase-anchored SPARKLINE with
// peak/@impact markers (still visual, never a bare number). Nothing measured at all ⇒
// a GHOST rail — the label plus a dashed neutral baseline carrying `emptyLabel`
// ("NA" / "soon"). The dashboard config is swing-agnostic (a fixed template that must
// display ANY swing), so a configured metric this swing could not measure holds its
// place rather than vanishing: a stable layout is what makes the wall readable, and
// the ghosts double as a visible ledger of how much of the pipeline is still to land.

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // ── Data in (all pre-reduced; the component fetches nothing) ────────────────
    property string label: ""
    property string unit: ""
    property var    phaseValues: []      // series.phaseSamples
    property var    corridors:   []      // descriptor().normative.corridors
    property var    curveT:      []      // series.t_us
    property var    curveV:      []      // series.value
    property int    impactPhase:  5      // Phase::Impact
    property int    addressPhase: 0      // Phase::Address
    // Kept for the caller's benefit only — openness travels ON the checkpoints now
    // (lowOpen/highOpen, from the measure's shape) and the ribbon reads it there. This
    // says whether the METRIC as a whole is one-sided, which is a labelling fact, not a
    // geometry one. Never derive it from a unit.
    property bool   oneSided:    false

    // ── Interaction (window only; the wall cast leaves these at their defaults) ──
    property real   playheadUs: -1       // -1 = idle
    property bool   interactive: false

    signal seekRequested(real tUs)       // checkpoint clicked
    signal scrubRequested(real tUs)      // hover-scrub: video follows the finger
    signal labelActivated()              // heading clicked → open the catalogue page

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }

    // ── Reductions (C++) ────────────────────────────────────────────────────────
    readonly property var _pts:   cm.railCheckpoints(phaseValues || [], corridors || [])
    readonly property var _range: cm.railRange(_pts)
    readonly property int _n:     _pts.length

    readonly property bool _hasCurve: (curveT && curveT.length > 1
                                       && curveV && curveV.length > 1)
    readonly property var  _summary: _hasCurve
        ? cm.summary(curveT, curveV, curveT[0], curveT[curveT.length - 1]) : null

    // Corridor presence decides the primitive: rail proper, or the sparkline fallback.
    readonly property bool _banded: {
        for (var i = 0; i < _pts.length; ++i) if (_pts[i].hasCorridor) return true
        return false
    }
    readonly property bool _sparkline: !_banded && _hasCurve

    // Ghost text for a metric this swing carries no value for. "" ⇒ the rail hides
    // entirely (a caller that genuinely wants nothing rendered).
    property string emptyLabel: ""

    readonly property bool _hasData:  _n > 0 || _sparkline
    readonly property bool _hasContent: _hasData || emptyLabel.length > 0
    // The ghost is shorter than a live rail — it holds the column, it doesn't claim
    // the same weight as a measured metric.
    implicitHeight: _hasData ? Theme.sp(158) : (_hasContent ? Theme.sp(56) : 0)
    visible: _hasContent
    opacity: _hasData ? 1.0 : 0.55

    // ── Checkpoint lookups (filters, not reductions) ────────────────────────────
    function _ptAt(phase) {
        for (var i = 0; i < _pts.length; ++i) if (_pts[i].phase === phase) return _pts[i]
        return null
    }
    readonly property var _impactPt:  _ptAt(impactPhase)
    readonly property var _addressPt: _ptAt(addressPhase)

    function _bandColor(b) {
        return b === "green"  ? Theme.colorRagGood
             : b === "yellow" ? Theme.colorRagWatch
             : b === "red"    ? Theme.colorRagFault
             :                  Theme.colorRagNone
    }
    function _fmt(v) {
        if (v === undefined || v === null || isNaN(v)) return "–"
        var a = Math.abs(v)
        return a >= 100 ? String(Math.round(v)) : String(Math.round(v * 10) / 10)
    }

    // ── Headline: the playhead value while scrubbing/playing, else @impact, else peak ─
    readonly property bool _liveHead: interactive && playheadUs >= 0 && _hasCurve
    readonly property real _headValue: _liveHead ? cm.valueAtUs(curveT, curveV, playheadUs)
                                     : (_impactPt ? _impactPt.value
                                     : (_summary ? _summary.peak : NaN))
    readonly property color _headColor: _liveHead ? Theme.colorAccent
                                      : (_impactPt ? _bandColor(_impactPt.band) : Theme.colorText)

    // ── Geometry ────────────────────────────────────────────────────────────────
    // Checkpoints occupy equal slots and sit at slot CENTRES; the corridor ribbon
    // spans centre-to-centre only. Beyond the outermost checkpoint the catalogue
    // makes no claim, so the rail paints none — an extended ribbon would imply a
    // corridor that was never defined.
    readonly property real _slotW: _n > 0 ? plot.width / _n : plot.width
    function _x(i) { return _slotW * (i + 0.5) }

    readonly property real _rLo: _range.valid ? _range.lo : 0
    readonly property real _rHi: _range.valid ? _range.hi : 1
    readonly property real _rSpan: Math.max(1e-9, _rHi - _rLo)
    function _y(v) { return plot.height - ((v - _rLo) / _rSpan) * plot.height }

    // Real time → x, warped through the checkpoint anchors so the revealed curve
    // passes exactly through the dots it is explaining. Clamped outside the span.
    //
    // With fewer than two checkpoints there is nothing to warp through, so fall back
    // to a LINEAR map over the curve's own time domain. Without this a sparkline on
    // a metric that carries no phase samples collapsed to x=0 — the trace, its peak
    // marker and the playhead all piled up on the left edge.
    function _xForT(t) {
        if (_n < 2) {
            if (!_hasCurve) return 0
            var ct0 = curveT[0], ct1 = curveT[curveT.length - 1]
            if (!(ct1 > ct0)) return 0
            var cf = Math.max(0, Math.min(1, (t - ct0) / (ct1 - ct0)))
            return cf * plot.width
        }
        if (t <= _pts[0].tUs) return _x(0)
        if (t >= _pts[_n - 1].tUs) return _x(_n - 1)
        for (var i = 1; i < _n; ++i) {
            if (t > _pts[i].tUs) continue
            var t0 = _pts[i - 1].tUs, t1 = _pts[i].tUs
            var f = (t1 > t0) ? (t - t0) / (t1 - t0) : 0
            return _x(i - 1) + f * (_x(i) - _x(i - 1))
        }
        return _x(_n - 1)
    }

    // ── Header: label + derived chips ───────────────────────────────────────────
    Item {
        id: head
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(40)

        // The heading opens the metric's catalogue page; the plot below is reserved
        // for seek/scrub. Gated on interactive, so the wall cast's resting render is
        // unchanged (no cursor, no chevron, same as the in-app panel at rest).
        HoverHandler { id: headHover; enabled: root.interactive; cursorShape: Qt.PointingHandCursor }
        TapHandler   { enabled: root.interactive; onTapped: root.labelActivated() }

        Text {
            id: headLabel
            anchors { left: parent.left; top: parent.top }
            text: root.label.toUpperCase()
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
            font.letterSpacing: Theme.trackingMicro
            // Brightens on hover — part of the "this heading is clickable" cue.
            color: (root.interactive && headHover.hovered) ? Theme.colorText : Theme.colorText3
            elide: Text.ElideRight
            width: Math.min(implicitWidth, parent.width * 0.5)
        }
        // Hover affordance: a chevron marking the heading as a link to the catalogue.
        Text {
            visible: root.interactive && headHover.hovered
            anchors { left: headLabel.right; leftMargin: Theme.sp(4); baseline: headLabel.baseline }
            text: "›"
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody
            color: Theme.colorText2
        }

        // Headline value — flips to the interpolated playhead value during replay.
        Row {
            visible: root._hasData
            anchors { left: parent.left; top: headLabel.bottom; topMargin: Theme.sp(1) }
            spacing: Theme.sp(3)
            Text {
                anchors.baseline: parent.top
                anchors.baselineOffset: Theme.sp(21)
                text: root._fmt(root._headValue)
                font.family: Theme.fontData; font.pixelSize: Theme.sp(25)
                font.weight: Font.DemiBold
                color: root._headColor
            }
            Text {
                anchors.baseline: parent.top
                anchors.baselineOffset: Theme.sp(21)
                text: root.unit
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText3
            }
            Text {
                anchors.baseline: parent.top
                anchors.baselineOffset: Theme.sp(21)
                // Which reading the headline is showing — never leave it ambiguous.
                text: root._liveHead ? qsTr("@ PLAYHEAD")
                                     : (root._impactPt ? qsTr("@ IMP") : qsTr("PEAK"))
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: root._liveHead ? Theme.colorAccent : Theme.colorText3
            }
        }

        // Right-aligned derived chips: Δ address→impact, and peak @ phase.
        Row {
            visible: root._hasData
            anchors { right: parent.right; top: parent.top }
            spacing: Theme.sp(8)
            Text {
                visible: root._impactPt !== null && root._addressPt !== null
                text: qsTr("Δ ") + root._fmt(root._impactPt && root._addressPt
                                             ? root._impactPt.value - root._addressPt.value : 0)
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
            Text {
                visible: root._summary !== null
                text: qsTr("PEAK %1 %2").arg(root._fmt(root._summary ? root._summary.peak : NaN))
                      .arg(labels.phaseShortTag(cm.nearestPhase(
                           root.phaseValues || [], root._summary ? root._summary.tPeakUs : 0)))
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
        }
    }

    // ── Plot ────────────────────────────────────────────────────────────────────
    // Ghost rail — the placeholder a swing-agnostic template needs when this swing
    // measured nothing. A dashed neutral baseline (still a drawn primitive, not a
    // bare word) carrying "NA" / "soon".
    Item {
        visible: !root._hasData && root._hasContent
        anchors { left: parent.left; right: parent.right; top: head.bottom; bottom: parent.bottom }

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Theme.colorBorderMid
                strokeWidth: Theme.borderWidth
                fillColor: "transparent"
                strokeStyle: ShapePath.DashLine
                dashPattern: [3, 3]
                startX: 0; startY: height / 2
                PathLine { x: parent.width; y: parent.height / 2 }
            }
        }
        Text {
            anchors.centerIn: parent
            text: root.emptyLabel
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            font.italic: root.emptyLabel !== qsTr("NA")
            color: Theme.colorText3
            // A small plate so the dashes don't run through the word.
            Rectangle {
                anchors.centerIn: parent
                z: -1
                width: parent.implicitWidth + Theme.sp(8); height: parent.implicitHeight
                color: Theme.colorBg
            }
        }
    }

    Item {
        id: plot
        visible: root._hasData
        anchors { left: parent.left; right: parent.right; top: head.bottom; bottom: parent.bottom
                  bottomMargin: Theme.sp(15) }
        clip: true

        // Amber margin, then green core on top — one quad per checkpoint-to-checkpoint
        // segment, so bounds that vary per checkpoint render as a tapering ribbon.
        Repeater {
            model: Math.max(0, root._n - 1)
            delegate: Item {
                id: seg
                required property int index
                anchors.fill: parent
                readonly property var a: root._pts[index]
                readonly property var b: root._pts[index + 1]
                visible: a.hasCorridor && b.hasCorridor

                // An OPEN tail runs off the end of the plot rather than stopping, so the
                // corridor never terminates in a hard edge that reads as a bound it does
                // not hold. Per checkpoint, from the corridor — not from a rail-wide flag,
                // and never from the unit.
                //
                // FLOOR (open above): each zone runs from its lower bound to the top, so
                // red is below amberLo, amber up to greenLo, green above it.
                // CEILING (open below) is the mirror, and it had no representation at all
                // before this — the old code substituted the high edges and never the low
                // ones, so an aspirational floor was handled and its opposite was not.
                readonly property real aHiA: a.highOpen ? root._rHi : a.amberHi
                readonly property real bHiA: b.highOpen ? root._rHi : b.amberHi
                readonly property real aHiG: a.highOpen ? root._rHi : a.greenHi
                readonly property real bHiG: b.highOpen ? root._rHi : b.greenHi
                readonly property real aLoA: a.lowOpen  ? root._rLo : a.amberLo
                readonly property real bLoA: b.lowOpen  ? root._rLo : b.amberLo
                readonly property real aLoG: a.lowOpen  ? root._rLo : a.greenLo
                readonly property real bLoG: b.lowOpen  ? root._rLo : b.greenLo

                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {   // amber margin
                        fillColor: Theme.colorBandAmber
                        strokeColor: "transparent"
                        startX: root._x(seg.index);     startY: root._y(seg.aHiA)
                        PathLine { x: root._x(seg.index + 1); y: root._y(seg.bHiA) }
                        PathLine { x: root._x(seg.index + 1); y: root._y(seg.bLoA) }
                        PathLine { x: root._x(seg.index);     y: root._y(seg.aLoA) }
                    }
                    ShapePath {   // green core
                        fillColor: Theme.colorBandGreen
                        strokeColor: "transparent"
                        startX: root._x(seg.index);     startY: root._y(seg.aHiG)
                        PathLine { x: root._x(seg.index + 1); y: root._y(seg.bHiG) }
                        PathLine { x: root._x(seg.index + 1); y: root._y(seg.bLoG) }
                        PathLine { x: root._x(seg.index);     y: root._y(seg.aLoG) }
                    }
                }
            }
        }

        // Zero reference — only when 0 is actually inside the domain.
        Rectangle {
            visible: root._range.valid && root._rLo < 0 && root._rHi > 0
            x: 0; width: parent.width
            y: root._y(0); height: Theme.borderWidth
            color: Theme.colorBorderMid
            opacity: 0.7
        }

        // Impact marker — the one checkpoint every read is anchored to.
        Rectangle {
            visible: root._impactPt !== null
            width: Theme.borderWidth
            y: 0; height: parent.height
            x: {
                for (var i = 0; i < root._n; ++i)
                    if (root._pts[i].phase === root.impactPhase) return root._x(i)
                return -1
            }
            color: Theme.colorText3
            opacity: 0.55
        }

        // Hover-revealed raw curve — hidden at rest so the resting render matches the
        // wall cast. Built only while visible; an empty path costs nothing.
        Shape {
            id: curveShape
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            opacity: (root.interactive && hover.hovered) || root._sparkline ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }

            ShapePath {
                strokeColor: root._sparkline ? Theme.colorAccent : Theme.colorText2
                strokeWidth: Theme.sp(1)
                fillColor: "transparent"
                joinStyle: ShapePath.RoundJoin
                capStyle:  ShapePath.RoundCap
                PathPolyline {
                    path: curveShape.visible && root._hasCurve
                          ? root.curveT.map((t, i) => Qt.point(root._xForT(t),
                                                               root._y(root.curveV[i])))
                          : []
                }
            }
        }

        // Peak marker — a hollow ring on the trace at its extremum. Carries the
        // sparkline fallback: with no corridor there are no band-coloured dots, so
        // without this the trace would be a shape with nothing to read off it.
        // Shown for the sparkline only; a banded rail already marks every checkpoint.
        Item {
            visible: root._sparkline && root._summary !== null
            x: root._xForT(root._summary ? root._summary.tPeakUs : 0)
            y: root._y(root._summary ? root._summary.peak : 0)
            Rectangle {
                anchors.centerIn: parent
                width: Theme.sp(10); height: width; radius: width / 2
                color: "transparent"
                border.width: Theme.sp(2)
                border.color: Theme.colorAccent
            }
            Text {
                anchors { bottom: parent.top; bottomMargin: Theme.sp(4)
                          horizontalCenter: parent.horizontalCenter }
                text: qsTr("PEAK")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }
        }

        // Connecting line through the player dots.
        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            visible: root._n > 1
            ShapePath {
                strokeColor: Theme.colorText2
                strokeWidth: Theme.sp(1.5)
                fillColor: "transparent"
                joinStyle: ShapePath.RoundJoin
                capStyle:  ShapePath.RoundCap
                PathPolyline {
                    path: root._pts.map((p, i) => Qt.point(root._x(i), root._y(p.value)))
                }
            }
        }

        // Player dots — band-coloured, click-to-seek.
        Repeater {
            model: root._pts
            delegate: Item {
                required property var modelData
                required property int index
                x: root._x(index) - width / 2
                y: root._y(modelData.value) - height / 2
                width: Theme.sp(22); height: Theme.sp(22)

                Rectangle {   // hover ring — invisible at rest
                    anchors.centerIn: parent
                    width: Theme.sp(18); height: width; radius: width / 2
                    color: "transparent"
                    border.width: Theme.borderWidth
                    border.color: Theme.colorBorderMid
                    opacity: root.interactive && dotHover.hovered ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: Theme.sp(9); height: width; radius: width / 2
                    color: root._bandColor(modelData.band)
                    border.width: Theme.borderWidth
                    border.color: Theme.colorBg
                }
                HoverHandler {
                    id: dotHover
                    enabled: root.interactive
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    enabled: root.interactive
                    onTapped: root.seekRequested(modelData.tUs)
                }
            }
        }

        // Playhead sweep — one line across every rail, driven by shotReplay.positionUs.
        Rectangle {
            visible: root.interactive && root.playheadUs >= 0 && root._n > 0
            x: root._xForT(root.playheadUs)
            y: 0; width: Theme.sp(1.5); height: parent.height
            color: Theme.colorAccent
            opacity: 0.9
        }

        // Hover-scrub readout — follows the cursor, reading the CURVE at the hovered
        // time (not the checkpoint), which is the whole point of revealing it.
        HoverHandler { id: hover; enabled: root.interactive }

        readonly property real _hoverT: hover.hovered ? root._tForX(hover.point.position.x) : -1

        Rectangle {
            visible: root.interactive && hover.hovered && root._hasCurve
            x: Math.min(Math.max(0, hover.point.position.x + Theme.sp(8)),
                        parent.width - width)
            y: Theme.sp(2)
            implicitWidth: readout.implicitWidth + Theme.sp(10)
            implicitHeight: readout.implicitHeight + Theme.sp(5)
            radius: Theme.radius
            color: Theme.colorSurface
            border.width: Theme.borderWidth
            border.color: Theme.colorBorder
            Text {
                id: readout
                anchors.centerIn: parent
                text: root._fmt(cm.valueAtUs(root.curveT, root.curveV, plot._hoverT))
                      + (root.unit.length ? (" " + root.unit) : "")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                color: Theme.colorText2
            }
        }

        // Scrubbing: dragging inside the plot maps x back through the checkpoint
        // anchors to a capture time and asks the host to move the video there. No
        // TapHandler here — a bare tap belongs to the checkpoint dots (click-to-seek),
        // and a second one at this level would double-fire on every dot press.
        DragHandler {
            id: scrubDrag
            enabled: root.interactive
            target: null
            xAxis.enabled: true; yAxis.enabled: false
            onCentroidChanged: if (active) root.scrubRequested(root._tForX(centroid.position.x))
        }
    }

    // Inverse of _xForT: plot x → capture time, through the same checkpoint anchors
    // (and the same linear fallback below two checkpoints, so hover-scrub works on a
    // sparkline too).
    function _tForX(px) {
        if (_n < 2) {
            if (!_hasCurve) return -1
            var ct0 = curveT[0], ct1 = curveT[curveT.length - 1]
            if (plot.width <= 0) return ct0
            return ct0 + Math.max(0, Math.min(1, px / plot.width)) * (ct1 - ct0)
        }
        if (px <= _x(0)) return _pts[0].tUs
        if (px >= _x(_n - 1)) return _pts[_n - 1].tUs
        for (var i = 1; i < _n; ++i) {
            if (px > _x(i)) continue
            var x0 = _x(i - 1), x1 = _x(i)
            var f = (x1 > x0) ? (px - x0) / (x1 - x0) : 0
            return _pts[i - 1].tUs + f * (_pts[i].tUs - _pts[i - 1].tUs)
        }
        return _pts[_n - 1].tUs
    }

    // ── Checkpoint labels ───────────────────────────────────────────────────────
    Repeater {
        model: root._hasData ? root._pts : []
        delegate: Text {
            required property var modelData
            required property int index
            x: root._x(index) - width / 2
            y: root.height - Theme.sp(13)
            width: root._slotW
            horizontalAlignment: Text.AlignHCenter
            text: labels.phaseShortTag(modelData.phase)
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color: modelData.phase === root.impactPhase ? Theme.colorText2 : Theme.colorText3
            elide: Text.ElideRight
        }
    }
}
