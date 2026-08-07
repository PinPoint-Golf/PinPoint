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

// A dashed rounded outline, because Rectangle's border cannot dash and on this panel the
// dash CARRIES MEANING rather than decorating: it is what separates "an expectation from
// your fault profile" from "a finding from this session" on the Cold cards, and "one of
// your patterns, read it on its card" from "new this swing" on a this-shot chip. A solid
// border in either place would be the panel asserting something it has not earned.
//
// Canvas rather than Shape: QtQuick.Shapes is a plugin the offscreen QML test target does
// not link, and a frame this small is one stroke.

import QtQuick
import PinPointStudio

Canvas {
    id: root

    property color strokeColor: Theme.colorBorderMid
    property real  lineWidth:   1
    property real  frameRadius: Theme.radius
    property real  dashOn:      Theme.sp(3)
    property real  dashOff:     Theme.sp(3)

    onStrokeColorChanged: requestPaint()
    onFrameRadiusChanged: requestPaint()
    onDashOnChanged:      requestPaint()
    onDashOffChanged:     requestPaint()
    onWidthChanged:       requestPaint()
    onHeightChanged:      requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        if (width <= 0 || height <= 0)
            return
        const inset = lineWidth / 2
        ctx.strokeStyle = strokeColor
        ctx.lineWidth   = lineWidth
        ctx.setLineDash([dashOn, dashOff])
        ctx.beginPath()
        ctx.roundedRect(inset, inset,
                        Math.max(0, width - lineWidth), Math.max(0, height - lineWidth),
                        frameRadius, frameRadius)
        ctx.stroke()
    }
}
