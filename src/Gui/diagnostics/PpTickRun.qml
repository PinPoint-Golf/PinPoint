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
// THE SELECTED TICK IS THE REVIEW PANEL'S ONLY POSITIONAL DEVICE (brief §6). Reviewing a
// finished session, every count on the panel is a session total and the ONE thing that says
// which swing is being read is this tick: 6 px wide against 2.5, 14 px tall against 11, and
// outlined in colorText so it survives being drawn over a fired fill. It is deliberately not
// a colour change — the tick's colour already means fired/clean/not-assessable, and a
// selection that recoloured it would overwrite the fact it is pointing at. The model decides
// WHICH tick is selected (ticksFor() carries `selected`), because a delegate comparing shot
// ids would have to know the panel's selection to draw a run.
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
    // ...and the selected tick's, from the same table: 6 wide, 14 tall.
    readonly property real _selW:   6.0 * _unit
    readonly property int  _selH:   Math.max(3, Math.round(14 * _unit))

    readonly property int count: ticks ? ticks.length : 0
    // The run reserves the SELECTED tick's height whenever it holds one, so a card's layout
    // does not shift by 3 px as the reviewed shot moves along the carousel.
    readonly property bool hasSelected: {
        for (let i = 0; i < count; ++i)
            if (ticks[i].selected === true) return true
        return false
    }
    readonly property int _rowH: hasSelected ? _selH : _tallH

    // The pitch the run WANTS, and the pitch it can afford. Squeezing is preferred to
    // clipping for the reason in the header comment.
    readonly property real _wantPitch: _tickW + _gap
    readonly property real _pitch: (count > 0 && width > 0)
                                   ? Math.min(_wantPitch, width / count)
                                   : _wantPitch
    readonly property real _w: Math.max(1, _pitch - Math.min(_gap, _pitch * 0.4))
    // The selected tick keeps its extra width even where the run has been squeezed — it is
    // wider than its neighbours by DESIGN and a squeeze that equalised it would delete the
    // only thing on the panel saying which shot is being read. It overlaps them instead,
    // which is what the mock does at 14 ticks in a 220 px card.
    readonly property real _selDrawW: Math.max(_w, Math.min(_selW, _pitch * 2))

    implicitHeight: _rowH
    // The WANTED width, not the squeezed one: _pitch reads `width`, so an implicit
    // width derived from it depends on the width it is meant to inform, and any
    // parent that sizes from its content closes that into a binding loop (the wide
    // rail's Column did). What a caller asks for here is "how wide is the whole run
    // unconstrained" — the squeeze in _pitch is the answer to being given less.
    implicitWidth:  count * _wantPitch

    Repeater {
        model: root.ticks

        Rectangle {
            required property int index
            required property var modelData

            readonly property bool notAssessable: modelData.state === "notAssessable"
            readonly property bool fired:         modelData.state === "fired"
            readonly property bool selected:      modelData.selected === true

            // ONE objectName for every tick, selected or not: the invariant this run exists
            // to keep is "one tick per shot in the ledger and never a gap", and a test that
            // counted them would stop being able to see that if the selected one renamed
            // itself. What it is, is readable off `selected`.
            objectName: "sdTick"

            x: index * root._pitch
            width:  selected ? root._selDrawW : root._w
            height: selected ? root._selH
                             : (notAssessable ? root._shortH : root._tallH)
            // Bottom-aligned, so the short tick sits on the run's baseline and reads as a
            // gap in the EVIDENCE rather than a gap in the sequence.
            y: root._rowH - height
            radius: Math.max(1, Math.round(root._unit))
            // Above its neighbours, since it is wider than its pitch when the run is tight.
            z: selected ? 1 : 0

            color: notAssessable ? "transparent"
                                 : (fired ? Theme.colorError : Theme.colorGood)
            // Outlined in colorText when selected — over a fired fill, over a clean fill and
            // over the not-assessable tick's own grey outline, all three read.
            border.width: (selected || notAssessable) ? 1 : 0
            border.color: selected ? Theme.colorText : Theme.colorText3
        }
    }
}
