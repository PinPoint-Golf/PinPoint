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

// WHERE THIS SHOT SITS IN THE SESSION'S OWN PATTERN — a ±1 SD region shaded behind a
// vector, in one of two shapes.
//
//   "wedge"    an angular sector about a pivot, for every line that swings about a
//              point: club path, face angle, start direction, spin axis, attack angle,
//              dynamic loft, launch angle.
//   "ellipse"  a 2D region, for the strike pattern on the face and the landing pattern
//              on the ground track.
//
// IT IS NOT A CORRIDOR AND MUST NEVER READ AS ONE. This is the golfer against their own
// other shots with the same club — no norm, no target, no grade, exactly like the tiles
// board's dispersion strip. Two things keep it honest: the resting fill is NEUTRAL grey,
// never a hue that could be read as a verdict, and it is never green or red under any
// state. The band hue appears only on hover, where it means "this region belongs to the
// line you are asking about" and nothing more.
//
// THE PANEL DOES GRADE, and this region is why it does so in a different channel. A
// reading outside its corridor is said in COLOUR, on the figure that states it (PpLmRead)
// — never as a second shaded region on the drawing. One drawing carrying two shaded areas
// that mean different things is the failure this separation exists to prevent, and it is
// why nothing in this file consults `grade`. Region = the golfer's own spread, always.
// Colour on a number = outside the corridor. Neither ever borrows the other's channel.
//
// SUBTLE AT REST, SHARPER ON ENQUIRY. Nine of these on screen at full strength would bury
// the shot they exist to place. So at rest they are texture — faint enough to read as
// grain behind the drawing, present enough to still be visible on a bay TV across a
// hitting bay, which is where this panel actually lives and where no pointer will ever
// hover. Hover then lifts one region, tints it its band hue and draws the session mean
// through it, so the clutter is a response to a question rather than the default state.
//
// Declared BEFORE the rules and annotations in every diagram, so it stacks behind them.

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // "wedge" | "ellipse"
    property string shape: "wedge"
    property real   s: 1                 // the parent PpLmDiagram's scale
    property color  hue: Theme.colorText
    // Raised when the reader asks about this metric. See PpLmGraphicsBody.hoveredKey.
    property bool   active: false
    // The metric this region describes, and whether the pointer is on it.
    //
    // THE SHADING IS ITSELF A HOVER TARGET, not only the label that names it. Pointing at
    // the thing you want to know about is the gesture people actually make; requiring
    // them to find a 10 px label first is a puzzle, and it was the first thing that went
    // wrong when this was put in front of someone.
    property string metricKey: ""
    readonly property bool isHovered: wedgeHover.hovered || ellipseHover.hovered

    // ── wedge ───────────────────────────────────────────────────────────────
    property real pivotX: 0              // design px
    property real pivotY: 0
    property real radius: 100            // design px
    property real meanAngle: 0           // degrees, screen convention (clockwise, y down)
    property real halfAngle: 0           // degrees — one SD, already through the card's gain

    // ── ellipse ─────────────────────────────────────────────────────────────
    property real centreX: 0             // design px
    property real centreY: 0
    property real radiusX: 0             // design px, = 1 SD
    property real radiusY: 0
    property real tiltDeg: 0

    // The two alphas. Both are low: the resting one is a wash the eye reads as texture,
    // and even the active one sits under a 1 px line without competing with it. A wedge
    // covers a hundred times the area of the tiles board's 3 px strip, so it cannot
    // borrow that control's 0.07/0.17 — the same alpha over that much area is a fog.
    readonly property real restAlpha: 0.055
    readonly property real liveAlpha: 0.150

    // Not readonly: a Behavior needs a writable property to animate through.
    property color _fill: active
        ? Qt.rgba(hue.r, hue.g, hue.b, liveAlpha)
        : Qt.rgba(Theme.colorText.r, Theme.colorText.g, Theme.colorText.b, restAlpha)
    Behavior on _fill { ColorAnimation { duration: Theme.durationFast } }

    // AN EDGE, NOT JUST A FILL — and this is what makes the thin regions exist at all.
    // The wedges differ enormously in area: a 6° start-direction spread over 300 px is
    // 63 px wide at its tip, while a 1° launch-angle spread over 216 px is 7 px. The same
    // fill alpha over those two areas is not the same subtlety, it is a visible region and
    // an invisible one — which is exactly how this first shipped, with four of the seven
    // wedges rendering perfectly and nobody able to see them.
    //
    // Raising the fill to suit the thin ones would turn the broad ones into fog. A defined
    // boundary at a higher alpha than the fill fixes both ends at once: a thin sliver reads
    // as a hairline pair, a broad wash gains a quiet edge that says where ±1 SD stops. The
    // geometry is untouched — the region is still exactly one standard deviation.
    property color _edge: active
        ? Qt.rgba(hue.r, hue.g, hue.b, 0.45)
        : Qt.rgba(Theme.colorText.r, Theme.colorText.g, Theme.colorText.b, 0.16)
    Behavior on _edge { ColorAnimation { duration: Theme.durationFast } }

    anchors.fill: parent

    // A sector from the pivot: out along one edge, round the arc, back. PathAngleArc
    // takes the screen convention already (degrees clockwise from +x, y down), so the
    // angles arrive here needing no conversion beyond the card's own gain.
    Shape {
        anchors.fill: parent
        visible: root.shape === "wedge" && root.halfAngle > 0

        ShapePath {
            fillColor: root._fill
            strokeColor: root._edge
            strokeWidth: 1
            startX: root.pivotX * root.s
            startY: root.pivotY * root.s
            PathAngleArc {
                centerX: root.pivotX * root.s
                centerY: root.pivotY * root.s
                radiusX: root.radius * root.s
                radiusY: root.radius * root.s
                startAngle: root.meanAngle - root.halfAngle
                sweepAngle: 2 * root.halfAngle
                moveToStart: false
            }
            PathLine { x: root.pivotX * root.s; y: root.pivotY * root.s }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.shape === "ellipse" && root.radiusX > 0 && root.radiusY > 0

        // Tilt is applied as a transform about the centre rather than baked into the
        // arc, because PathAngleArc has no rotation of its own.
        transform: Rotation {
            origin.x: root.centreX * root.s
            origin.y: root.centreY * root.s
            angle: root.tiltDeg
        }

        ShapePath {
            fillColor: root._fill
            strokeColor: root._edge
            strokeWidth: 1
            PathAngleArc {
                centerX: root.centreX * root.s
                centerY: root.centreY * root.s
                radiusX: Math.max(0.5, root.radiusX * root.s)
                radiusY: Math.max(0.5, root.radiusY * root.s)
                startAngle: 0
                sweepAngle: 360
            }
        }
    }

    // ── the pointer targets ─────────────────────────────────────────────────
    //
    // Shaped to the region rather than to this item, which fills the whole card: a
    // handler on the full frame would light a wedge whenever the pointer was anywhere
    // near the diagram, including over another card's business.
    //
    // The strip is RECTANGULAR where the region is a sector — close enough to point at,
    // and generous where the sector narrows to nothing at the pivot. It also carries a
    // minimum thickness and a margin, so the 1° launch-angle wedge can still be pointed
    // at. THAT PADDING IS THE TARGET ONLY, never the drawing: the shaded region stays
    // exactly one standard deviation wide however small that is.
    readonly property real _hitMin: 18

    Item {
        visible: root.shape === "wedge" && root.halfAngle > 0
        width: root.radius * root.s
        height: Math.max(root._hitMin * root.s,
                         2 * root.radius * root.s
                           * Math.tan(Math.min(60, root.halfAngle) * Math.PI / 180))
        x: root.pivotX * root.s
        y: root.pivotY * root.s - height / 2
        transformOrigin: Item.Left
        rotation: root.meanAngle

        // THE INNER THIRD CLAIMS NOTHING. Three of the wedges on PATH & FACE pivot on the
        // same ball and three more on IMPACT pivot on the same impact point, so close to
        // the pivot they all lie on top of one another and whichever was declared last
        // would swallow the pointer — the face-angle spread was unreachable behind the
        // start-direction one for exactly this reason. Wedges only become distinguishable
        // as they diverge, so the target begins where the answer does.
        Item {
            x: parent.width * 0.3
            width: parent.width * 0.7
            height: parent.height
            HoverHandler { id: wedgeHover; margin: 4 * root.s; enabled: root.metricKey !== "" }
        }
    }

    Item {
        visible: root.shape === "ellipse" && root.radiusX > 0 && root.radiusY > 0
        width: Math.max(root._hitMin * root.s, 2 * root.radiusX * root.s)
        height: Math.max(root._hitMin * root.s, 2 * root.radiusY * root.s)
        x: root.centreX * root.s - width / 2
        y: root.centreY * root.s - height / 2
        transformOrigin: Item.Center
        rotation: root.tiltDeg
        HoverHandler { id: ellipseHover; margin: 4 * root.s; enabled: root.metricKey !== "" }
    }

    // The session mean, drawn only while the region is being asked about.
    //
    // This is the payoff for hovering: the shaded region says how spread out the golfer
    // is, and the gap between THIS line and the solid one already on the card says where
    // this particular shot fell inside that. At rest it would be a tenth line on a card
    // that already has nine.
    PpLmRule {
        visible: root.shape === "wedge" && root.active
        opacity: root.active ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        x: root.pivotX * root.s
        y: root.pivotY * root.s
        s: root.s
        len: root.radius
        thickness: 1
        kind: "club"
        hue: root.hue
        pivot: "left"
        angle: root.meanAngle
    }

    // The mean strike, for the ellipse case — a ring rather than a disc, so it cannot be
    // mistaken for the solid marker showing where THIS shot actually struck the face.
    Rectangle {
        visible: root.shape === "ellipse" && root.active
        opacity: root.active ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
        width: 9 * root.s
        height: width
        radius: width / 2
        x: root.centreX * root.s - width / 2
        y: root.centreY * root.s - height / 2
        color: "transparent"
        border.width: Math.max(1, root.s)
        border.color: root.hue
    }
}
