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

// A horizontal corridor bar for ONE corridor map, as MetricCatalog marshals it out of the norm set
// { greenLo, greenHi, amberLo, amberHi, lowOpen, highOpen, deltaFromAddress, … }: a green
// "Ideal" band nested inside a wider amber band on a muted track, with the amber min/max
// tick labels below. Self-contained and prop-driven so charting can reuse it later — set
// `value` to render an optional "you" marker (hidden while it is not finite).
//
// ONE-SIDED CORRIDORS. Where the measure's shape leaves a tail ungraded, the norm collapses
// that side's edges onto `mu` — the aspiration, never a sentinel. Drawn naively that reads
// as a WALL: a floor's green band would stop dead at mu with a hard edge, and every reading
// above it (all of which grade Ideal) would clamp to the last pixel of the track, so 1.55
// and 5.0 would look the same and both would look like they had left the corridor. So the
// green band runs off the open end as a fade, and the domain leaves room for it to run into
// — see barDomain() in dashboard_reductions.h, which owns that arithmetic because a rule
// living inside a QML binding is a rule nothing can test.
//
// The amber band needs no such treatment: it genuinely ENDS at mu on a floor, because above
// mu the grade is Ideal and the colour there is green.

import QtQuick
import PinPointStudio

Item {
    id: root

    property var  corridor: ({})
    property real value: NaN            // optional "you" marker; non-finite = hidden

    readonly property real _aLo: (corridor && corridor.amberLo !== undefined) ? corridor.amberLo : 0
    readonly property real _aHi: (corridor && corridor.amberHi !== undefined) ? corridor.amberHi : 0
    readonly property real _gLo: (corridor && corridor.greenLo !== undefined) ? corridor.greenLo : 0
    readonly property real _gHi: (corridor && corridor.greenHi !== undefined) ? corridor.greenHi : 0

    // Which tail does not grade, from the MEASURE's shape — carried on the corridor map, never
    // re-derived here from a unit or a metric key. `=== true` rather than a truth test: an
    // absent key is `undefined`, and being explicit is what says the absence was considered.
    readonly property bool _lowOpen:  (corridor && corridor.lowOpen  === true)
    readonly property bool _highOpen: (corridor && corridor.highOpen === true)

    readonly property bool _hasValue: isFinite(value)

    ChartMetrics { id: cm }

    // The domain is C++ (ChartMetrics.barDomain): amber padded 12% each side, falling back to
    // green then to value±1, and on a one-sided corridor the open edge runs past the furthest
    // of (aspiration, reading). `valid` false means there was neither a corridor nor a finite
    // reading — the bands then do not draw at all, rather than collapsing onto the left edge.
    readonly property var  _dom:   cm.barDomain(_gLo, _gHi, _aLo, _aHi, _lowOpen, _highOpen,
                                                value, _hasValue)
    readonly property bool _valid: _dom.valid === true
    readonly property real _dLo:   _dom.lo
    readonly property real _dHi:   _dom.hi
    readonly property real _dSpan: Math.max(1e-6, _dHi - _dLo)

    // Domain value → 0..1 fraction, clamped to the track. Non-finite lands at 0, which is why
    // every caller of this is gated on `_valid` and on the value being finite: an unguarded
    // NaN would draw at the LEFT edge, which is a position, and a position is a claim.
    function _fx(v) {
        return isFinite(v) ? Math.max(0, Math.min(1, (v - _dLo) / _dSpan)) : 0
    }

    function _fmt(v) {
        if (v === undefined || v === null || !isFinite(v)) return "—"
        return (Math.abs(v - Math.round(v)) < 0.05) ? String(Math.round(v)) : v.toFixed(1)
    }

    // A tick label's x. Anchored to its end of the track normally; on the OPEN side the number
    // is the aspiration and sits somewhere in the middle of the domain, so it is centred under
    // where it actually falls — pinning it to the edge would put the corridor's last number
    // under a stretch of track the corridor does not bound.
    function _tickX(v, w, open, rightEdge) {
        if (!open) return rightEdge ? Math.max(0, width - w) : 0
        return Math.max(0, Math.min(width - w, width * _fx(v) - w / 2))
    }

    implicitHeight: track.height + Theme.sp(6) + tickRow.height

    // ── Track ───────────────────────────────────────────────────────────────
    Rectangle {
        id: track
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(12)
        radius: height / 2
        color:  Theme.colorBg3

        // Amber band — the wider acceptable corridor. Unchanged by shape: on a floor its
        // high edge IS mu and the corridor really does end there, because everything above
        // mu grades Ideal and is drawn green.
        Rectangle {
            visible: root._valid
            x:      track.width * root._fx(root._aLo)
            width:  track.width * (root._fx(root._aHi) - root._fx(root._aLo))
            height: parent.height
            radius: parent.radius
            color:  Qt.rgba(Theme.colorAttention.r, Theme.colorAttention.g, Theme.colorAttention.b, 0.18)
            border.width: 1
            border.color: Qt.rgba(Theme.colorAttention.r, Theme.colorAttention.g, Theme.colorAttention.b, 0.35)
        }

        // Green band — the tour-core target, nested inside amber.
        Rectangle {
            visible: root._valid
            x:      track.width * root._fx(root._gLo)
            width:  track.width * (root._fx(root._gHi) - root._fx(root._gLo))
            height: parent.height
            radius: parent.radius
            color:  Qt.rgba(Theme.colorGood.r, Theme.colorGood.g, Theme.colorGood.b, 0.38)
        }

        // The open tail, and it is ADDITIVE — a two-sided bar renders exactly the two bands
        // above and nothing else, which is what keeps 105 of the 106 measures pixel-identical.
        // Green continues from the aspiration to the end of the track and fades out, because
        // the grade genuinely continues and the track's end is the plot's limit, not the
        // norm's. A hard edge there would state a bound the measure does not have.
        Rectangle {
            visible: root._valid && root._highOpen
            x:      track.width * root._fx(root._gHi)
            width:  Math.max(0, track.width - x)
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0
                               color: Qt.rgba(Theme.colorGood.r, Theme.colorGood.g,
                                              Theme.colorGood.b, 0.38) }
                GradientStop { position: 1.0
                               color: Qt.rgba(Theme.colorGood.r, Theme.colorGood.g,
                                              Theme.colorGood.b, 0.0) }
            }
        }

        // The ceiling mirror.
        Rectangle {
            visible: root._valid && root._lowOpen
            x:      0
            width:  Math.max(0, track.width * root._fx(root._gLo))
            height: parent.height
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0
                               color: Qt.rgba(Theme.colorGood.r, Theme.colorGood.g,
                                              Theme.colorGood.b, 0.0) }
                GradientStop { position: 1.0
                               color: Qt.rgba(Theme.colorGood.r, Theme.colorGood.g,
                                              Theme.colorGood.b, 0.38) }
            }
        }

        // "You" marker — only when a finite value is supplied.
        Rectangle {
            visible: root._hasValue && root._valid
            x: track.width * root._fx(root.value) - width / 2
            width:  Theme.sp(2)
            height: parent.height + Theme.sp(6)
            y: -Theme.sp(3)
            radius: width / 2
            color: Theme.colorText
        }
    }

    // ── Min / max tick labels ─────────────────────────────────────────────────
    Item {
        id: tickRow
        anchors { left: parent.left; right: parent.right; top: track.bottom; topMargin: Theme.sp(6) }
        height: loTick.implicitHeight

        Text {
            id: loTick
            x: root._tickX(root._aLo, width, root._lowOpen, false)
            text: root._fmt(root._aLo)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }

        Text {
            x: root._tickX(root._aHi, width, root._highOpen, true)
            text: root._fmt(root._aHi)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }
    }
}
