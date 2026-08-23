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

import QtQuick
import PinPointStudio

Rectangle {
    id: root

    property string label:       ""
    // Optional leading glyph rendered in the symbol font (e.g. "▶" on Replay).
    property string glyph:       ""
    // Optional leading DRAWN icon, for the cases the symbol font cannot cover —
    // fontSymbol is Apple Symbols on macOS and Segoe UI Symbol on Windows, and
    // there are icons in neither (PpQrGlyph is the first). The component is
    // handed `contentColor` through the loader's own `color` property if it has
    // one, so it tracks the primary/outline/destructive palettes like the label
    // does. `iconSize` sizes the square it is given.
    property Component icon:     null
    property real   iconSize:    Theme.sp(15)
    property bool   primary:     false
    property bool   destructive: false
    // Bold amber call-to-action fill (the Theme.colorAttention framing). Filled,
    // like `primary`, but in the attention hue — used for the main action the user
    // should take next (e.g. Cancel during in-panel calibration).
    property bool   attention:   false

    signal clicked()

    implicitWidth:  btnContent.implicitWidth + Theme.sp(24)
    implicitHeight: Theme.sp(34)
    radius:         Theme.radius
    opacity:        root.enabled ? 1.0 : 0.4

    readonly property bool  _filled:    primary || attention || destructive
    // Resting fill. The outline variant rests transparent but RGB-matched to its
    // hover fill, so the hover ColorAnimation only ramps alpha — no colour flash
    // (the same lesson as the home-tile hover).
    readonly property color _restColor: primary     ? Theme.colorAccent
                                       : attention   ? Theme.colorAttention
                                       : destructive ? Theme.colorWarnLight
                                       : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)

    // Hover brighten — filled variants lighten, the outline variant fades a faint
    // bg fill in. Pairs with the PpPressable scale-grow / press-dip below so the
    // button visibly responds (it previously only blipped opacity on press).
    color:        btnMa.containsMouse ? (_filled ? Qt.lighter(_restColor, 1.08) : Theme.colorBg2)
                                      : _restColor
    border.width: (primary || attention) ? 0 : 1
    border.color: destructive ? Theme.colorWarn : Theme.colorBorderStrong
    Behavior on color { ColorAnimation { duration: Theme.durationFast } }

    readonly property color contentColor: (primary || attention)
                                              ? (Theme.dark ? Theme.colorBg : "#FFFFFF")
                                              : (destructive ? Theme.colorWarn : Theme.colorText)

    Row {
        id: btnContent
        anchors.centerIn: parent
        spacing: Theme.sp(7)

        Loader {
            visible:  root.icon !== null
            active:   root.icon !== null
            width:    active ? root.iconSize : 0
            height:   root.iconSize
            anchors.verticalCenter: parent.verticalCenter
            sourceComponent: root.icon
            onLoaded: if (item && item.color !== undefined) item.color = Qt.binding(
                          function() { return root.contentColor })
        }
        Text {
            visible:        root.glyph !== ""
            anchors.verticalCenter: parent.verticalCenter
            text:           root.glyph
            font.family:    Theme.fontSymbol
            font.pixelSize: Math.round(Theme.fontSzBody * Theme.symbolScale(root.glyph))
            color:          root.contentColor
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text:           root.label
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzBody
            color:          root.contentColor
        }
    }

    // Standard hover-grow / press-dip motion (drives root.scale). Replaces the old
    // opacity-on-press blip; disabled dimming stays on root.opacity above.
    PpPressable {
        id: btnMa
        enabled:   root.enabled
        onClicked: root.clicked()
    }
}
