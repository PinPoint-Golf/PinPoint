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

// GRAPHICS MODE for the launch monitor panel — the same shot the tiles board lists,
// drawn instead. Five schematics carrying every metric the device reported except
// distance to pin, which is a fact about the hole and has no line to annotate.
//
// THIS FILE TAKES A MAP, NOT A MODEL. `g` is LmSessionModel's `graphics` property and
// nothing else is wired in. That is deliberate twice over: it keeps the arithmetic on
// the C++ side of the line the whole panel is built on (QML positions and paints), and
// it lets the offscreen layout test feed a literal fixture without standing up a shot
// list — which is what makes "no annotation escapes its card" cheap enough to assert on
// every run, after review caught two of exactly that.
//
// WHAT IS EXAGGERATED, AND WHERE IT SAYS SO. Drawn true at 672 px wide, a 1.3° start
// direction and a 3.5° attack angle are a pixel of deflection each — a picture that
// says "straight" about a shot that was not. So plan-view and impact-view angles carry a
// gain, the flight profile carries the brief's 2.1× height and 2.5× lateral, and EVERY
// CARD WITH A GAIN STATES IT IN ITS CAPTION. The printed values never move: only the
// geometry is stretched, and the number beside it is always the measurement.
//
// COLOUR IS THE METRIC'S BAND, NOT THE CARD'S. The launch ray inside IMPACT is Launch
// teal; the smash chip inside PATH & FACE is Strike purple. That is what lets a reader
// carry one colour meaning between this view and the tiles board. Lines carry the hue;
// numbers stay colorText2, because a 14 px figure in a mid-chroma hue on colorSurface is
// the contrast failure the tiles board already had to fix once.

import QtQuick
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // LmSessionModel.graphics. Defaulted so the component is safe to instantiate before
    // a shot exists — every accessor below degrades to "not reported".
    property var g: ({})

    // ── the design frame ────────────────────────────────────────────────────
    // 1168 × 560 stage, less the body's sp(10) side padding, over the brief's rows:
    // a 56 px headline strip, then 696 + 444, then 432 + 248 + 452, all 220 tall,
    // with sp(8) between everything.
    readonly property real designW: 1148
    readonly property real designH: 512

    // Below this, the two-row composition stops being legible and the cards stack
    // instead. Never a horizontal scroll: reaching a card by dragging sideways is worse
    // than reading it smaller, and worse again than reading it in a column.
    readonly property real kReflowFloor: 0.75

    readonly property real fitS: (width > 0 && height > 0)
        ? Math.min(width / designW, height / designH) : 1
    readonly property bool reflow: fitS < kReflowFloor
    // Capped so a bay TV grows the drawing without turning a 9 px label into a headline.
    readonly property real s: Math.min(fitS, 3.0)

    readonly property bool leftHanded: g && g.leftHanded === true

    // ── reading the map ─────────────────────────────────────────────────────
    // Every accessor answers safely for a field the device never reported. A missing
    // reading draws no line and prints an em dash; it never draws a line at zero, which
    // would claim a measurement of zero the device never made.
    function fld(key) {
        const v = g && g.values ? g.values[key] : undefined
        return v !== undefined ? v : null
    }
    function has(key) { const f = fld(key); return !!f && f.has === true }
    function num(key) { const f = fld(key); return (f && f.has) ? f.value : 0 }
    function txt(key) { const f = fld(key); return f ? f.text : "—" }
    function unit(key) { const f = fld(key); return f ? f.unit : "" }
    function hue(key) {
        const f = fld(key)
        return Theme.chartSeriesColor(f ? f.bandIndex : 0)
    }

    readonly property var flight: (g && g.flight) ? g.flight : ({ has: false })
    readonly property var shape:  (g && g.shape)  ? g.shape  : ({ has: false })
    readonly property var strike: (g && g.strike) ? g.strike : ({ has: false })

    // Band hues, by the tiles board's band order.
    readonly property color hueClub:   Theme.chartSeriesColor(0)
    readonly property color hueStrike: Theme.chartSeriesColor(1)
    readonly property color hueLaunch: Theme.chartSeriesColor(2)
    readonly property color hueSpin:   Theme.chartSeriesColor(3)
    readonly property color hueFlight: Theme.chartSeriesColor(4)

    // Plan-view and impact-view angle gains. Named, so the caption can print them and
    // a reviewer can find every use in one search.
    readonly property real kPlanGain:   5.0    // PATH & FACE
    readonly property real kSpinGain:   3.0    // SPIN axis
    readonly property real kAttackGain: 4.0    // IMPACT attack angle

    // ── the five cards ──────────────────────────────────────────────────────
    // Position and size in design pixels; `comp` is the schematic. One list drives both
    // layouts, so the wide composition and the reflowed column cannot drift apart.
    readonly property var cards: [
        { title: qsTr("PATH & FACE"), hue: hueClub,
          caption: qsTr("plan view · target right · angles ×%1").arg(kPlanGain),
          x: 0,   y: 64,  w: 696, comp: pathFaceComp },
        { title: qsTr("SPIN"), hue: hueSpin,
          caption: qsTr("down the line · axis ×%1").arg(kSpinGain),
          x: 704, y: 64,  w: 444, comp: spinComp },
        { title: qsTr("IMPACT"), hue: hueClub,
          caption: qsTr("side view · loft and launch true · attack ×%1").arg(kAttackGain),
          x: 0,   y: 292, w: 432, comp: impactComp },
        { title: qsTr("STRIKE"), hue: hueStrike,
          caption: root.leftHanded ? qsTr("face on · heel left") : qsTr("face on · toe left"),
          x: 440, y: 292, w: 248, comp: strikeComp },
        { title: qsTr("FLIGHT"), hue: hueFlight,
          caption: flightCaption,
          x: 696, y: 292, w: 452, comp: flightComp },
    ]

    // ── wide: the design's two-row composition ──────────────────────────────
    Item {
        visible: !root.reflow
        width: root.designW * root.s
        height: root.designH * root.s
        anchors.horizontalCenter: parent.horizontalCenter
        y: 0

        Loader { active: !root.reflow; sourceComponent: headlineComp
                 width: root.designW * root.s; height: 56 * root.s }

        Repeater {
            // Emptied rather than merely hidden when the other layout is showing. An
            // invisible card is still five diagrams, two Shapes and a hundred bindings
            // kept alive to draw nothing — and `visible: false` on the parent does not
            // stop any of it being built.
            model: root.reflow ? [] : root.cards
            PpLmCard {
                required property var modelData
                s: root.s
                hue: modelData.hue
                title: modelData.title
                caption: modelData.caption
                diagram: modelData.comp
                x: modelData.x * root.s
                y: modelData.y * root.s
                width: modelData.w * root.s
                height: 220 * root.s
            }
        }
    }

    // ── narrow: one column, scrolled vertically ─────────────────────────────
    // Each card takes the full width and scales to it, so a schematic in the column is
    // LARGER than it was in the composition, not smaller. That is the whole point of
    // reflowing rather than shrinking on.
    Flickable {
        id: column
        anchors.fill: parent
        visible: root.reflow
        clip: true
        contentWidth: width                 // never sideways
        contentHeight: stack.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        interactive: contentHeight > height

        Column {
            id: stack
            width: column.width
            spacing: 8 * root.kReflowFloor

            Loader {
                active: root.reflow; sourceComponent: headlineComp
                width: stack.width
                height: 56 * (stack.width / root.designW)
            }

            Repeater {
                model: root.reflow ? root.cards : []
                PpLmCard {
                    required property var modelData
                    // Each card gets its own scale from the width it was given.
                    readonly property real cardS: stack.width / modelData.w
                    s: cardS
                    hue: modelData.hue
                    title: modelData.title
                    caption: modelData.caption
                    diagram: modelData.comp
                    width: stack.width
                    height: 220 * cardS
                }
            }
        }
    }

    // ── headline strip ──────────────────────────────────────────────────────
    Component {
        id: headlineComp

        Item {
            id: strip
            readonly property real hs: width > 0 ? width / root.designW : root.s

            Row {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }

                Repeater {
                    model: root.g && root.g.headline ? root.g.headline : []

                    Row {
                        required property var modelData
                        required property int index

                        Rectangle {           // the sp(1) × 30 divider between figures
                            width: Math.max(1, Math.round(strip.hs))
                            height: 30 * strip.hs
                            anchors.verticalCenter: parent.verticalCenter
                            visible: index > 0
                            color: Theme.colorBorderMid
                        }
                        Item {
                            width: 20 * strip.hs * 2 + inner.width
                            height: inner.height
                            Column {
                                id: inner
                                x: 20 * strip.hs
                                spacing: Math.round(2 * strip.hs)
                                Text {
                                    text: modelData.abbrev
                                    font.family: Theme.fontData
                                    font.pixelSize: Math.max(1, Math.round(Theme.fontSzMicro * strip.hs))
                                    font.letterSpacing: Theme.trackingMicro
                                    color: Theme.colorText2
                                }
                                Row {
                                    spacing: Math.round(3 * strip.hs)
                                    Text {
                                        id: headlineValue
                                        text: modelData.text
                                        font.family: Theme.fontData
                                        font.pixelSize: Math.max(1, Math.round(21 * strip.hs))
                                        font.letterSpacing: -0.8 * strip.hs
                                        color: Theme.colorText
                                    }
                                    Text {
                                        anchors.baseline: headlineValue.baseline
                                        text: modelData.unit
                                        font.family: Theme.fontData
                                        font.pixelSize: Math.max(1, Math.round(9 * strip.hs))
                                        color: Theme.colorText2
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ── shared annotation pieces ────────────────────────────────────────────
    // An annotation is a micro label in its metric's band hue over the reading in
    // colorText2. The HUE IS ON THE LABEL, not the figure: it ties the words to the line
    // they describe while keeping every number at the contrast the tiles board settled on.
    component Anno : Column {
        id: anno
        objectName: "anno"
        property real s: 1
        property string label: ""
        property string value: "—"
        property string unit: ""
        property color hue: Theme.colorText2
        spacing: Math.round(2 * s)

        Text {
            text: anno.label
            font.family: Theme.fontData
            font.pixelSize: Math.max(1, Math.round(Theme.fontSzMicro * anno.s))
            font.letterSpacing: Theme.trackingMicro
            color: anno.hue
        }
        Row {
            spacing: Math.round(3 * anno.s)
            Text {
                id: annoValue
                text: anno.value
                font.family: Theme.fontData
                font.pixelSize: Math.max(1, Math.round(14 * anno.s))
                color: Theme.colorText2
            }
            Text {
                anchors.baseline: annoValue.baseline
                text: anno.unit
                font.family: Theme.fontData
                font.pixelSize: Math.max(1, Math.round(Theme.fontSzMicro * anno.s))
                color: Theme.colorText2
                visible: anno.unit !== ""
            }
        }
    }

    // A ball. The design has an image slot here; with no artwork shipped it is the disc
    // fallback the brief calls for, which is a complete read on its own.
    component Ball : Rectangle {
        radius: width / 2
        color: Theme.colorText
        opacity: 0.85
    }

    // ── 1. PATH & FACE — plan view, looking down, target to the right ───────
    // Up on screen is LEFT of the target line, so a shot that started left draws upward.
    Component {
        id: pathFaceComp

        PpLmDiagram {
            id: pf
            designW: 672; designH: 182

            PpLmRule {                                   // target line
                x: pf.d(20); y: pf.d(96); s: pf.s
                len: 632; kind: "reference"
            }
            Text {
                x: pf.d(20); y: pf.d(100)
                text: qsTr("TARGET LINE")
                font.family: Theme.fontData; font.pixelSize: pf.f(Theme.fontSzMicro)
                font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }

            PpLmRule {                                   // club path — dashed, club hue
                x: pf.d(300 - 340 / 2); y: pf.d(96); s: pf.s
                len: 340; thickness: 1
                kind: "club"; hue: root.hueClub
                pivot: "center"
                visible: root.has("lm.clubPath")
                angle: root.num("lm.clubPath") * root.kPlanGain
            }
            PpLmRule {                                   // face angle — dashed, club hue
                // A centre-pivoted rule is still POSITIONED by its top-left, so a line
                // meant to pass THROUGH a point starts half its length before it. The
                // face is 72 long and belongs on the ball at x 300, so it starts at 264 —
                // written as the subtraction rather than as 264, because the bug this
                // fixes was the face drawn beside the ball instead of across it.
                x: pf.d(300 - 72 / 2); y: pf.d(96); s: pf.s
                len: 72; kind: "club"; hue: root.hueClub
                pivot: "center"
                visible: root.has("lm.faceAngle")
                // Vertical at the ball, then tilted by the measured face angle.
                angle: 90 + root.num("lm.faceAngle") * root.kPlanGain
            }
            PpLmRule {                                   // start direction — SOLID, ball's own line
                x: pf.d(300); y: pf.d(96); s: pf.s
                len: 300; thickness: 1.5
                kind: "ball"; hue: root.hueLaunch
                pivot: "left"
                visible: root.has("lm.launchDirection")
                angle: root.num("lm.launchDirection") * root.kPlanGain
            }

            Ball { x: pf.d(294); y: pf.d(90); width: pf.d(12); height: pf.d(12) }
            Ball {                                       // where it was, 300 px on
                readonly property real a: root.num("lm.launchDirection") * root.kPlanGain * Math.PI / 180
                visible: root.has("lm.launchDirection")
                width: pf.d(10); height: pf.d(10)
                x: pf.d(300) + pf.d(300) * Math.cos(a) - width / 2
                y: pf.d(96) + pf.d(300) * Math.sin(a) - height / 2
            }

            Anno { x: pf.d(16);  y: pf.d(14);  s: pf.s; hue: root.hueClub
                   label: qsTr("CLUB SPEED");    value: root.txt("lm.clubheadSpeed"); unit: root.unit("lm.clubheadSpeed") }
            Anno { x: pf.d(190); y: pf.d(14);  s: pf.s; hue: root.hueClub
                   label: qsTr("CLOSURE RATE ↻"); value: root.txt("lm.closureRate");   unit: root.unit("lm.closureRate") }
            Anno { x: pf.d(372); y: pf.d(14);  s: pf.s; hue: root.hueClub
                   label: qsTr("FACE TO PATH");  value: root.txt("lm.faceToPath");     unit: root.unit("lm.faceToPath") }
            // Set well right of FACE TO PATH and under the far end of the start-direction
            // ray, which is where the eye already is — and spelled out, because "START
            // DIR." saves eleven pixels on a card that has room and costs the reader the
            // one word that says which direction is meant.
            Anno { x: pf.d(520); y: pf.d(14);  s: pf.s; hue: root.hueLaunch
                   label: qsTr("START DIRECTION"); value: root.txt("lm.launchDirection"); unit: root.unit("lm.launchDirection") }
            Anno { x: pf.d(16);  y: pf.d(128); s: pf.s; hue: root.hueClub
                   label: qsTr("CLUB PATH");     value: root.txt("lm.clubPath");        unit: root.unit("lm.clubPath") }
            Anno { x: pf.d(250); y: pf.d(146); s: pf.s; hue: root.hueClub
                   label: qsTr("FACE ANGLE");    value: root.txt("lm.faceAngle");       unit: root.unit("lm.faceAngle") }
            // BALL SPEED and SMASH are NOT repeated here. Both lead the headline strip a
            // few pixels above, and a figure printed twice on one screen makes a reader
            // check whether the two agree instead of reading either. The strip is the
            // right home for them: they are the shot's summary, not an annotation on any
            // particular line of the geometry.

            // The inferred flight read: a lit cell of the nine windows, the name, and the
            // evidence for a claim the device did not make.
            Item {
                objectName: "anno"
                x: pf.d(398); y: pf.d(100)
                width: pf.d(256); height: pf.d(64)
                visible: root.shape.has === true

                Grid {
                    id: windows
                    columns: 3; rows: 3
                    spacing: pf.d(3)
                    Repeater {
                        model: 9
                        Rectangle {
                            required property int index
                            readonly property int col: index % 3
                            readonly property int row: Math.floor(index / 3)
                            readonly property bool lit: root.shape.windowIdx === col
                                                        && root.shape.curveIdx === row
                            width: pf.d(5); height: pf.d(5)
                            color: lit ? root.hueFlight
                                       : Qt.rgba(Theme.colorText.r, Theme.colorText.g,
                                                 Theme.colorText.b, 0.13)
                        }
                    }
                }
                Column {
                    anchors { left: windows.right; leftMargin: pf.d(10); top: parent.top }
                    width: pf.d(210)
                    spacing: pf.d(2)
                    Text {
                        text: qsTr("INFERRED FLIGHT · START × CURVE")
                        font.family: Theme.fontData; font.pixelSize: pf.f(Theme.fontSzMicro)
                        font.letterSpacing: Theme.trackingMicro
                        color: Theme.colorText3
                    }
                    Text {
                        text: root.shape.name !== undefined ? root.shape.name : ""
                        font.family: Theme.fontData; font.pixelSize: pf.f(14)
                        color: root.hueFlight
                    }
                    Text {
                        // THE AUDIT TRAIL. Not decoration, and never demoted to
                        // colorText3 or below the micro floor — it is the only thing on
                        // the card that shows the reasoning behind an inferred word.
                        width: parent.width
                        text: root.shape.evidence !== undefined ? root.shape.evidence : ""
                        font.family: Theme.fontBody; font.pixelSize: pf.f(Theme.fontSzMicro)
                        color: Theme.colorText2
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    // ── 2. SPIN — down the line ─────────────────────────────────────────────
    Component {
        id: spinComp

        PpLmDiagram {
            id: sp
            designW: 420; designH: 182

            PpLmRule { x: sp.d(16); y: sp.d(84); s: sp.s; len: 264
                       kind: "reference" }

            Ball { x: sp.d(108); y: sp.d(42); width: sp.d(84); height: sp.d(84); opacity: 0.35 }

            PpLmRule {                                   // spin axis — dashed, spin hue
                // Centre-pivoted, so positioned half its length before the ball centre
                // at x 150 — see the face angle on PATH & FACE for why this is spelled out.
                x: sp.d(150 - 180 / 2); y: sp.d(84); s: sp.s
                len: 180; kind: "club"; hue: root.hueSpin
                pivot: "center"
                visible: root.has("lm.spinAxis")
                angle: root.num("lm.spinAxis") * root.kSpinGain
            }
            Text {
                x: sp.d(140); y: sp.d(6)
                text: "↻"
                font.pixelSize: sp.f(17)
                color: root.hueSpin
                visible: root.has("lm.spinRate")
            }

            Anno { x: sp.d(14);  y: sp.d(136); s: sp.s; hue: root.hueSpin
                   label: qsTr("SPIN AXIS"); value: root.txt("lm.spinAxis"); unit: root.unit("lm.spinAxis") }
            Anno { x: sp.d(272); y: sp.d(16);  s: sp.s; hue: root.hueSpin
                   label: qsTr("SPIN RATE · TOTAL"); value: root.txt("lm.spinRate"); unit: root.unit("lm.spinRate") }
            Anno { x: sp.d(272); y: sp.d(66);  s: sp.s; hue: root.hueSpin
                   label: qsTr("BACK SPIN"); value: root.txt("lm.backSpin"); unit: root.unit("lm.backSpin") }
            Anno { x: sp.d(272); y: sp.d(116); s: sp.s; hue: root.hueSpin
                   label: qsTr("SIDE SPIN"); value: root.txt("lm.sideSpin"); unit: root.unit("lm.sideSpin") }
        }
    }

    // ── 3. IMPACT — side view ───────────────────────────────────────────────
    Component {
        id: impactComp

        PpLmDiagram {
            id: im
            designW: 408; designH: 182

            PpLmRule { x: im.d(16); y: im.d(140); s: im.s; len: 376
                       kind: "reference" }

            PpLmRule {                                   // attack angle — into the ball
                x: im.d(20); y: im.d(132); s: im.s
                len: 130; kind: "club"; hue: root.hueClub
                // Origin at the RIGHT end, which is the ball: the line describes the
                // club's approach and is never drawn past impact.
                pivot: "right"
                visible: root.has("lm.attackAngle")
                angle: -root.num("lm.attackAngle") * root.kAttackGain
            }
            PpLmRule {                                   // dynamic loft — true angle
                x: im.d(150); y: im.d(132); s: im.s
                len: 66; kind: "club"; hue: root.hueClub
                pivot: "left"
                visible: root.has("lm.dynamicLoft")
                // Straight up from the ball, then leaned back by the delivered loft.
                angle: -90 - root.num("lm.dynamicLoft")
            }
            PpLmRule {                                   // launch ray — SOLID, true angle
                x: im.d(150); y: im.d(132); s: im.s
                len: 216; thickness: 1.5
                kind: "ball"; hue: root.hueLaunch
                pivot: "left"
                visible: root.has("lm.launchAngle")
                angle: -root.num("lm.launchAngle")
            }

            Ball { x: im.d(143); y: im.d(125); width: im.d(14); height: im.d(14) }
            Ball {
                readonly property real a: -root.num("lm.launchAngle") * Math.PI / 180
                visible: root.has("lm.launchAngle")
                width: im.d(10); height: im.d(10)
                x: im.d(150) + im.d(216) * Math.cos(a) - width / 2
                y: im.d(132) + im.d(216) * Math.sin(a) - height / 2
            }

            Anno { x: im.d(14);  y: im.d(18);  s: im.s; hue: root.hueClub
                   label: qsTr("ATTACK ANGLE"); value: root.txt("lm.attackAngle"); unit: root.unit("lm.attackAngle") }
            Anno { x: im.d(170); y: im.d(20);  s: im.s; hue: root.hueClub
                   label: qsTr("DYN. LOFT");    value: root.txt("lm.dynamicLoft"); unit: root.unit("lm.dynamicLoft") }
            Anno { x: im.d(262); y: im.d(118); s: im.s; hue: root.hueLaunch
                   label: qsTr("LAUNCH ANGLE"); value: root.txt("lm.launchAngle"); unit: root.unit("lm.launchAngle") }
            // Spin loft is not drawn as a wedge: it IS the gap between the two club
            // lines already on this card, so the card says so rather than adding a
            // third line that only restates them.
            Anno { x: im.d(150); y: im.d(152); s: im.s; hue: root.hueClub
                   label: qsTr("SPIN LOFT · LOFT LESS ATTACK")
                   value: root.txt("lm.spinLoft"); unit: root.unit("lm.spinLoft") }
        }
    }

    // ── 4. STRIKE — face on ─────────────────────────────────────────────────
    // TOE LEFT, HEEL RIGHT for a right-hander, and MIRRORED for a left-hander: looking
    // at the face of a left-handed club, the toe is on the other side. This orientation
    // was wrong once in review, which is why the mirror is one signed factor used by
    // every horizontal term on the card rather than three independent conditionals.
    Component {
        id: strikeComp

        PpLmDiagram {
            id: sk
            designW: 224; designH: 182

            readonly property real mirror: root.leftHanded ? -1 : 1
            readonly property real cx: 74
            readonly property real cy: 69
            readonly property real pxPerMm: 2.6

            PpLmRule { x: sk.d(14); y: sk.d(69); s: sk.s; len: 120     // crosshair, horizontal
                       kind: "reference" }
            PpLmRule { x: sk.d(74); y: sk.d(26); s: sk.s; len: 86      // crosshair, vertical
                       kind: "reference"; pivot: "left"; angle: 90 }

            // The strike itself: toward the toe is toward the mirrored side, low on the
            // face is DOWN the card.
            Item {
                id: mark
                visible: root.has("lm.strikeLocation") && root.has("lm.strikeHeight")
                readonly property real mx: sk.cx - sk.mirror * sk.pxPerMm * root.num("lm.strikeLocation")
                readonly property real my: sk.cy - sk.pxPerMm * root.num("lm.strikeHeight")
                x: sk.d(mx); y: sk.d(my)

                Rectangle {                              // halo
                    anchors.centerIn: parent
                    width: sk.d(15); height: width; radius: width / 2
                    color: Qt.rgba(root.hueStrike.r, root.hueStrike.g, root.hueStrike.b, 0.22)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: sk.d(9); height: width; radius: width / 2
                    color: root.hueStrike
                }
            }

            Text {
                x: root.leftHanded ? sk.d(106) : sk.d(14); y: sk.d(12)
                text: qsTr("TOE")
                font.family: Theme.fontData; font.pixelSize: sk.f(Theme.fontSzMicro)
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }
            Text {
                x: root.leftHanded ? sk.d(14) : sk.d(106); y: sk.d(12)
                text: qsTr("HEEL")
                font.family: Theme.fontData; font.pixelSize: sk.f(Theme.fontSzMicro)
                font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            }

            // Lie angle: a shaft rising away from the heel side, over its own ground line.
            PpLmRule { x: sk.d(158); y: sk.d(96); s: sk.s; len: 56
                       kind: "reference" }
            // A rule rotates its FAR end about the pivot. With pivot "right" the far end
            // starts at (−len, 0), so an angle of +lie carries it to
            // (−len·cos lie, −len·sin lie) — up and left, which is a right-hander's shaft
            // rising away from the heel. Mirrored, the pivot moves to the left end and
            // the sign flips, and the shaft rises up-RIGHT. Both derived rather than
            // eyeballed, because this orientation was wrong once in review.
            PpLmRule {
                x: root.leftHanded ? sk.d(158) : sk.d(168); y: sk.d(96); s: sk.s
                len: 46; kind: "club"; hue: root.hueClub
                pivot: root.leftHanded ? "left" : "right"
                visible: root.has("lm.lieAngle")
                angle: root.leftHanded ? -root.num("lm.lieAngle")
                                       :  root.num("lm.lieAngle")
            }

            // The two millimetre readings sit ON their own axes rather than under
            // headings. The crosshair already says which axis is which — horizontal is
            // toe-to-heel, vertical is high-to-low — so a heading would only name a thing
            // the picture has already named, and the number belongs where the reader is
            // looking. Sized and coloured as axis figures: fontData at the micro floor,
            // colorText2 because they are NUMBERS and colorText3 would be illegible.
            Text {                                       // strike location, on the toe-heel axis
                objectName: "anno"
                x: sk.d(104); y: sk.d(73)
                text: root.txt("lm.strikeLocation") + " " + root.unit("lm.strikeLocation")
                font.family: Theme.fontData; font.pixelSize: sk.f(Theme.fontSzMicro)
                color: Theme.colorText2
                visible: root.has("lm.strikeLocation")
            }
            Text {                                       // strike height, on the high-low axis
                objectName: "anno"
                x: sk.d(78); y: sk.d(99)
                text: root.txt("lm.strikeHeight") + " " + root.unit("lm.strikeHeight")
                font.family: Theme.fontData; font.pixelSize: sk.f(Theme.fontSzMicro)
                color: Theme.colorText2
                visible: root.has("lm.strikeHeight")
            }

            Anno { x: sk.d(152); y: sk.d(118); s: sk.s; hue: root.hueClub
                   label: qsTr("LIE ANGLE");       value: root.txt("lm.lieAngle");       unit: root.unit("lm.lieAngle") }

            Column {
                objectName: "anno"
                x: sk.d(142); y: sk.d(26)
                width: sk.d(78)
                spacing: sk.d(2)
                visible: root.strike.has === true
                Text {
                    text: qsTr("INFERRED STRIKE")
                    font.family: Theme.fontData; font.pixelSize: sk.f(Theme.fontSzMicro)
                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                    width: parent.width; elide: Text.ElideRight
                }
                Text {
                    text: root.strike.name !== undefined ? root.strike.name : ""
                    font.family: Theme.fontData; font.pixelSize: sk.f(14)
                    color: root.hueStrike
                }
                Text {
                    // Context, not evidence: smash is shown beside the read and takes no
                    // part in it. A toe strike with a great smash is still a toe strike.
                    width: parent.width
                    text: root.strike.evidence !== undefined ? root.strike.evidence : ""
                    font.family: Theme.fontBody; font.pixelSize: sk.f(Theme.fontSzMicro)
                    color: Theme.colorText2
                    elide: Text.ElideRight
                }
            }
        }
    }

    // ── 5. FLIGHT — side profile over ground track, one shared distance axis ─
    //
    // BOTH VIEWS SHARE THE DISTANCE AXIS, which is what lets a reader look straight down
    // from the apex to the ground track and see where the ball was. The scales are the
    // brief's nominal ones, CLAMPED so a 300 yd drive or a 40 yd slice still fits the
    // card — the caption prints the exaggeration that actually resulted, so the number
    // on screen is never a fiction.
    readonly property real flightSpanPx: 372              // x 40 → 412
    readonly property real flightTotalYd: Math.max(1, flight.totalYd !== undefined ? flight.totalYd : 1)
    readonly property real flightApexFt: Math.max(1, flight.apexFt !== undefined ? flight.apexFt : 1)
    readonly property real flightLatYd: Math.max(0.1, flight.lateralExtentYd !== undefined
                                                      ? flight.lateralExtentYd : 0.1)
    readonly property real xPerYd: Math.min(1.80, flightSpanPx / flightTotalYd)
    readonly property real yPerFt: Math.min(1.27, 80 / flightApexFt)
    readonly property real zPerYd: Math.min(4.50, 34 / flightLatYd)
    // Vertical and lateral exaggeration against the distance scale, in matched units.
    readonly property real vExag: yPerFt / (xPerYd / 3)
    readonly property real hExag: zPerYd / xPerYd
    readonly property string flightCaption:
        qsTr("profile over ground track · height ×%1 · lateral ×%2")
            .arg(vExag.toFixed(1)).arg(hExag.toFixed(1))

    Component {
        id: flightComp

        PpLmDiagram {
            id: fl
            designW: 428; designH: 182

            // Normalised (0…1 of total, 0…1 of apex, −1…1 of lateral) → design px.
            function px(nx) { return 40 + nx * root.flightTotalYd * root.xPerYd }
            function py(ny) { return 92 - ny * root.flightApexFt * root.yPerFt }
            function pz(nz) { return 146 + nz * root.flightLatYd * root.zPerYd }

            function poly(list, lateral) {
                const out = []
                if (!list) return out
                for (let i = 0; i < list.length; ++i)
                    out.push(Qt.point(fl.d(px(list[i].x)),
                                      fl.d(lateral ? pz(list[i].y) : py(list[i].y))))
                return out
            }

            PpLmRule { x: fl.d(16); y: fl.d(92);  s: fl.s; len: 396      // ground
                       kind: "reference" }
            PpLmRule { x: fl.d(16); y: fl.d(146); s: fl.s; len: 396      // target line
                       kind: "reference" }

            Text { x: fl.d(18); y: fl.d(124); text: qsTr("L")
                   font.family: Theme.fontData; font.pixelSize: fl.f(Theme.fontSzMicro)
                   color: Theme.colorText3 }
            Text { x: fl.d(18); y: fl.d(158); text: qsTr("R")
                   font.family: Theme.fontData; font.pixelSize: fl.f(Theme.fontSzMicro)
                   color: Theme.colorText3 }

            // The apex drop — a dashed plumb line tying the profile to its own distance.
            PpLmRule {
                x: fl.d(fl.px(root.flight.apexAtX !== undefined ? root.flight.apexAtX : 0))
                y: fl.d(12); s: fl.s
                len: Math.max(1, 80)
                kind: "club"; hue: root.hueFlight
                opacity: 0.30
                pivot: "left"; angle: 90
                visible: root.flight.has === true
            }

            // The two curves. These are the only elements on the five cards that are not
            // straight, so they are the only ones that need Shapes.
            Shape {
                anchors.fill: parent
                visible: root.flight.has === true

                ShapePath {                              // trajectory — SOLID, the ball's line
                    strokeColor: root.hueFlight
                    strokeWidth: Math.max(1, 1.4 * fl.s)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathPolyline { path: fl.poly(root.flight.profile, false) }
                }
                ShapePath {                              // ground track — SOLID
                    strokeColor: root.hueFlight
                    strokeWidth: Math.max(1, 1.4 * fl.s)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    PathPolyline { path: fl.poly(root.flight.track, true) }
                }
            }

            // The roll: where it finished after landing, at a lower emphasis because the
            // device modelled it rather than watched it.
            Shape {
                anchors.fill: parent
                visible: root.flight.has === true
                opacity: 0.45
                ShapePath {
                    strokeColor: root.hueFlight
                    strokeWidth: Math.max(1, 1.4 * fl.s)
                    fillColor: "transparent"
                    PathPolyline {
                        path: root.flight.has === true
                              ? [Qt.point(fl.d(fl.px(root.flight.landing.x)),
                                          fl.d(fl.pz(root.flight.landing.z))),
                                 Qt.point(fl.d(fl.px(root.flight.finish.x)),
                                          fl.d(fl.pz(root.flight.finish.z)))]
                              : []
                    }
                }
            }

            // The distance ruler, under the profile, spanning tee to touchdown.
            PpLmRule {
                x: fl.d(40); y: fl.d(104); s: fl.s
                len: Math.max(1, fl.px(root.flight.carryFraction !== undefined
                                       ? root.flight.carryFraction : 1) - 40)
                kind: "bracket"
                visible: root.flight.has === true
            }
            Rectangle {                                  // the label's own plate
                objectName: "anno"
                x: fl.d(112); y: fl.d(96)
                width: carryTxt.implicitWidth + fl.d(8)
                height: carryTxt.implicitHeight + fl.d(3)
                color: Theme.colorSurface
                visible: root.flight.has === true
                Text {
                    id: carryTxt
                    anchors.centerIn: parent
                    text: qsTr("CARRY %1 %2").arg(root.txt("lm.carryDistance"))
                                             .arg(root.unit("lm.carryDistance"))
                    font.family: Theme.fontData; font.pixelSize: fl.f(Theme.fontSzMicro)
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
            }

            Anno { x: fl.d(52);  y: fl.d(0);   s: fl.s; hue: root.hueFlight
                   label: qsTr("PEAK HEIGHT"); value: root.txt("lm.peakHeight"); unit: root.unit("lm.peakHeight") }
            Anno { x: fl.d(340); y: fl.d(44);  s: fl.s; hue: root.hueFlight
                   label: qsTr("DESCENT");     value: root.txt("lm.descentAngle"); unit: root.unit("lm.descentAngle") }
            Anno { x: fl.d(258); y: fl.d(116); s: fl.s; hue: root.hueFlight
                   label: qsTr("TOTAL");       value: root.txt("lm.totalDistance"); unit: root.unit("lm.totalDistance") }
            Anno { x: fl.d(44);  y: fl.d(150); s: fl.s; hue: root.hueFlight
                   label: qsTr("OFFLINE");     value: root.txt("lm.offline"); unit: root.unit("lm.offline") }
        }
    }
}
