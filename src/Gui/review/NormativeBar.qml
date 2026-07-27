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
// { greenLo, greenHi, amberLo, amberHi, deltaFromAddress, … }: a green "Ideal" band
// nested inside a wider amber band on a muted track, with the amber min/max tick
// labels below. Self-contained and prop-driven so charting can reuse it later — set
// `value` to render an optional "you" marker (hidden while it is NaN).

import QtQuick
import PinPointStudio

Item {
    id: root

    property var  corridor: ({})
    property real value: NaN            // optional "you" marker; NaN = hidden

    readonly property real _aLo: (corridor && corridor.amberLo !== undefined) ? corridor.amberLo : 0
    readonly property real _aHi: (corridor && corridor.amberHi !== undefined) ? corridor.amberHi : 0
    readonly property real _gLo: (corridor && corridor.greenLo !== undefined) ? corridor.greenLo : 0
    readonly property real _gHi: (corridor && corridor.greenHi !== undefined) ? corridor.greenHi : 0

    // Domain = amber band padded by 12% each side so the amber band reads as a band,
    // not the whole track.
    readonly property real _aSpan: Math.max(1e-6, _aHi - _aLo)
    readonly property real _pad:   _aSpan * 0.12
    readonly property real _dLo:   _aLo - _pad
    readonly property real _dHi:   _aHi + _pad
    readonly property real _dSpan: Math.max(1e-6, _dHi - _dLo)

    // Domain value → 0..1 fraction, clamped to the track.
    function _fx(v) { return Math.max(0, Math.min(1, (v - _dLo) / _dSpan)) }

    function _fmt(v) {
        if (v === undefined || v === null || isNaN(v)) return "—"
        return (Math.abs(v - Math.round(v)) < 0.05) ? String(Math.round(v)) : v.toFixed(1)
    }

    readonly property bool _hasValue: !isNaN(value)

    implicitHeight: track.height + Theme.sp(6) + tickRow.height

    // ── Track ───────────────────────────────────────────────────────────────
    Rectangle {
        id: track
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: Theme.sp(12)
        radius: height / 2
        color:  Theme.colorBg3

        // Amber band — the wider acceptable corridor.
        Rectangle {
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
            x:      track.width * root._fx(root._gLo)
            width:  track.width * (root._fx(root._gHi) - root._fx(root._gLo))
            height: parent.height
            radius: parent.radius
            color:  Qt.rgba(Theme.colorGood.r, Theme.colorGood.g, Theme.colorGood.b, 0.38)
        }

        // "You" marker — only when a value is supplied.
        Rectangle {
            visible: root._hasValue
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
            anchors.left: parent.left
            text: root._fmt(root._aLo)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }

        Text {
            anchors.right: parent.right
            text: root._fmt(root._aHi)
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
        }
    }
}
