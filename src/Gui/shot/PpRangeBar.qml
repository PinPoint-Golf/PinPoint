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

// A reusable corridor bar — a muted track carrying a nested amber/green band pair
// and a coloured marker for where the current value falls. Replaces bare numbers
// with a "where does it sit" read. Shared by the Verdict zone's tempo-ratio call-out
// (full size, axis labels) and the Setup zone's per-metric mini band-bar (compact,
// caller draws its own label/value row): `compact` toggles size and text, everything
// else — domain math, band tinting, marker placement — is identical either way.

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    property real   value: 0
    property real   greenLo: 0
    property real   greenHi: 0
    property real   amberLo: 0
    property real   amberHi: 0
    // Which tail does not grade, from the MEASURE's shape — set by the caller off the
    // corridor it already has, never re-derived here from `unit` or a metric key.
    property bool   lowOpen: false
    property bool   highOpen: false
    property string band: ""            // "green" | "yellow" | "red" | ""  → marker/value tint
    property string unit: ""
    property bool   compact: false       // true = Setup mini-bar (short, no axis labels)
    property bool   hasValue: true       // false = corridor drawn neutral, marker hidden

    implicitHeight: compact ? Theme.sp(22) : Theme.sp(40)

    function _bandColor(b) {
        return b === "green"  ? Theme.colorRagGood
             : b === "yellow" ? Theme.colorRagWatch
             : b === "red"    ? Theme.colorRagFault
             :                  Theme.colorRagNone
    }
    function _fmtVal(v) {
        var a = Math.abs(v)
        return a >= 100 ? String(Math.round(v)) : String(Math.round(v * 10) / 10)
    }

    readonly property color _markerColor: _bandColor(band)

    ChartMetrics { id: cm }

    // Domain = amber band padded 12% each side so the amber band reads as a band, not the
    // whole track (mirrors NormativeBar's corridor read), falling back to the green band
    // when amber is degenerate — the unconfigured default — and then to value±1. On a
    // ONE-SIDED corridor the open side instead runs past the furthest of (aspiration,
    // reading), leaving room for the band to fade out into: the norm collapses that side's
    // edges onto mu, so without the extra room every Ideal reading past the aspiration
    // clamps to the last pixel of the track and sits on a hard edge that reads as a bound.
    //
    // The rule itself is C++ (ChartMetrics.barDomain → dashboard_reductions.h) rather than
    // the four lines of QML it replaces, because it is shared with NormativeBar and because
    // there are no QML tests in this repo: a domain rule left in a binding is a rule nothing
    // can gate.
    readonly property var  _dom:      cm.barDomain(greenLo, greenHi, amberLo, amberHi,
                                                   lowOpen, highOpen, value, hasValue)
    readonly property bool _domValid: _dom.valid === true
    readonly property real _domLo:    _dom.lo
    readonly property real _domHi:    _dom.hi
    readonly property real _domSpan:  Math.max(1e-6, _domHi - _domLo)

    // Domain value → 0..1 fraction, clamped — this is what keeps the marker pinned
    // inside the corridor even when `value` sits outside the configured bounds.
    function _fx(v) {
        return isFinite(v) ? Math.max(0, Math.min(1, (v - _domLo) / _domSpan)) : 0
    }

    // A tick label's x. Anchored to its end of the track normally; on the OPEN side the
    // number is the aspiration, which sits somewhere in the middle of the domain, so it is
    // centred under where it actually falls. Pinned to the edge it would put the corridor's
    // last number under a stretch of track the corridor does not bound.
    function _tickX(v, w, open, rightEdge) {
        if (!open) return rightEdge ? Math.max(0, width - w) : 0
        return Math.max(0, Math.min(width - w, width * _fx(v) - w / 2))
    }

    // ── Value + unit read, above the marker (non-compact only) ────────────────
    Text {
        id: valueText
        visible: !root.compact && root.hasValue
        text: root._fmtVal(root.value) + (root.unit.length ? (" " + root.unit) : "")
        font.family:    Theme.fontData
        font.pixelSize: Theme.fontSzBody2
        color: root._markerColor
        anchors.top: parent.top
        x: Math.max(0, Math.min(root.width - width, root.width * root._fx(root.value) - width / 2))
    }

    // ── Track — amber margin + green core over a muted rail ───────────────────
    Item {
        id: track
        anchors.left:  parent.left
        anchors.right: parent.right
        // Compact: centred in the whole (short) item. Full: sits below the value
        // read, leaving room for the tick row below.
        anchors.top:            root.compact ? undefined : valueText.bottom
        anchors.topMargin:      root.compact ? 0 : Theme.sp(2)
        anchors.verticalCenter: root.compact ? parent.verticalCenter : undefined
        height:  root.compact ? Theme.sp(6) : Theme.sp(9)
        opacity: root.hasValue ? 1.0 : 0.5

        Rectangle {
            id: rail
            anchors.fill: parent
            radius: height / 2
            color:  Theme.colorBg2
        }

        // The amber band needs no shape treatment: on a floor its high edge IS mu, and the
        // corridor really does end there — above mu the grade is Ideal, so the colour is green.
        Rectangle {
            id: amberBand
            visible: root.hasValue && root._domValid
            x:      parent.width * root._fx(root.amberLo)
            width:  Math.max(0, parent.width * (root._fx(root.amberHi) - root._fx(root.amberLo)))
            height: parent.height
            radius: height / 2
            color:  Theme.colorBandAmber
        }

        Rectangle {
            id: greenBand
            visible: root.hasValue && root._domValid
            x:      parent.width * root._fx(root.greenLo)
            width:  Math.max(0, parent.width * (root._fx(root.greenHi) - root._fx(root.greenLo)))
            height: parent.height
            radius: height / 2
            color:  Theme.colorBandGreen
        }

        // The open tail, ADDITIVE so a two-sided bar draws exactly the two bands above and
        // nothing else. Green runs from the aspiration to the end of the track and fades:
        // the grade continues, and the track's end is the plot's limit rather than the norm's.
        Rectangle {
            visible: root.hasValue && root._domValid && root.highOpen
            x:      parent.width * root._fx(root.greenHi)
            width:  Math.max(0, parent.width - x)
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.colorBandGreen }
                GradientStop { position: 1.0
                               color: Qt.rgba(Theme.colorBandGreen.r, Theme.colorBandGreen.g,
                                              Theme.colorBandGreen.b, 0.0) }
            }
        }

        // The ceiling mirror.
        Rectangle {
            visible: root.hasValue && root._domValid && root.lowOpen
            x:      0
            width:  Math.max(0, parent.width * root._fx(root.greenLo))
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0
                               color: Qt.rgba(Theme.colorBandGreen.r, Theme.colorBandGreen.g,
                                              Theme.colorBandGreen.b, 0.0) }
                GradientStop { position: 1.0; color: Theme.colorBandGreen }
            }
        }

        // ── Marker — vertical stroke at the current value, nested in the track's
        // local coordinate space so its x math needs no extra offset. A non-finite value
        // would land at _fx()'s 0, i.e. the LEFT edge — a position, and a position is a
        // claim — so it is gated on being finite rather than merely on hasValue.
        Shape {
            id: marker
            visible: root.hasValue && root._domValid && isFinite(root.value)
            preferredRendererType: Shape.CurveRenderer
            width:  Theme.sp(2)
            height: track.height + Theme.sp(6)
            y: -Theme.sp(3)
            x: track.width * root._fx(root.value) - width / 2

            Behavior on x { NumberAnimation { duration: Theme.durationNormal } }

            ShapePath {
                strokeColor: root._markerColor
                strokeWidth: Theme.sp(2)
                capStyle:    ShapePath.RoundCap
                fillColor:   "transparent"
                startX: marker.width / 2
                startY: 0
                PathLine { x: marker.width / 2; y: marker.height }
            }
        }
    }

    // ── Corridor bounds — greenLo left, greenHi right (non-compact only) ──────
    Item {
        id: tickRow
        visible: !root.compact
        anchors.left:      parent.left
        anchors.right:     parent.right
        anchors.top:       track.bottom
        anchors.topMargin: Theme.sp(4)
        height: visible ? loTick.implicitHeight : 0

        Text {
            id: loTick
            x: root._tickX(root.greenLo, width, root.lowOpen, false)
            text: root._fmtVal(root.greenLo)
            font.family:      Theme.fontData
            font.pixelSize:   Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color: Theme.colorText3
        }
        Text {
            x: root._tickX(root.greenHi, width, root.highOpen, true)
            text: root._fmtVal(root.greenHi)
            font.family:      Theme.fontData
            font.pixelSize:   Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color: Theme.colorText3
        }
    }
}
