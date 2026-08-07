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

// The evidence run: one tick per shot in the ledger, in the order they were struck.
//
// THE NOT-ASSESSABLE TICK IS THE WHOLE POINT OF DRAWING IT AS A RUN RATHER THAN A COUNT.
// A shot whose measure did not exist is neither clean nor a zero, and the design's answer
// is a SHORT OUTLINED tick — present, obviously different, never a gap. A gap would read as
// "nothing happened on that swing", which is the one thing the panel must not say (brief
// §3.1, §5.2). So there is no branch in here that skips a tick, and there is no width at
// which one disappears: a ledger longer than the card narrows the pitch until it fits.
//
// Every tick's state comes from SessionDiagnosticsModel::ticksFor(). This file positions
// and paints.

import QtQuick
import PinPointStudio

Item {
    id: root

    // [{ state: "fired"|"clean"|"notAssessable", shotId: int, selected: bool }]
    property var ticks: []
    // The panel's fit scale. See PpSessionDiagnosticsBody._fitFor().
    property real fit: 1.0

    // The design's own tick metrics, at k = 1: 2.5 px wide, 2 px gap, 11 px tall, and the
    // not-assessable tick 5 px. Through Theme.fontScale so the run grows with the app's
    // type, then through the panel's fit — the same two factors every metric on the panel
    // carries. Not Theme.sp(), which rounds before the fit is applied and would collapse
    // the 2.5 to a 3.
    readonly property real _unit:   Theme.fontScale * root.fit
    readonly property real _tickW:  2.5 * _unit
    readonly property real _gap:    2.0 * _unit
    readonly property int  _tallH:  Math.max(2, Math.round(11 * _unit))
    readonly property int  _shortH: Math.max(1, Math.round(5 * _unit))

    readonly property int count: ticks ? ticks.length : 0

    // The pitch the run WANTS, and the pitch it can afford. Squeezing is preferred to
    // clipping for the reason in the header comment.
    readonly property real _wantPitch: _tickW + _gap
    readonly property real _pitch: (count > 0 && width > 0)
                                   ? Math.min(_wantPitch, width / count)
                                   : _wantPitch
    readonly property real _w: Math.max(1, _pitch - Math.min(_gap, _pitch * 0.4))

    implicitHeight: _tallH
    implicitWidth:  count * _pitch

    Repeater {
        model: root.ticks

        Rectangle {
            required property int index
            required property var modelData

            readonly property bool notAssessable: modelData.state === "notAssessable"
            readonly property bool fired:         modelData.state === "fired"

            objectName: "sdTick"

            x: index * root._pitch
            width:  root._w
            height: notAssessable ? root._shortH : root._tallH
            // Bottom-aligned, so the short tick sits on the run's baseline and reads as a
            // gap in the EVIDENCE rather than a gap in the sequence.
            y: root._tallH - height
            radius: Math.max(1, Math.round(root._unit))

            color: notAssessable ? "transparent"
                                 : (fired ? Theme.colorError : Theme.colorGood)
            border.width: notAssessable ? 1 : 0
            border.color: Theme.colorText3
        }
    }
}
