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
// TYPE IS FIXED; ONLY THE DRAWINGS SCALE. This view used to fit a 1148 × 512 design frame
// to whatever room it had and multiply that one factor into every card position, every
// diagram coordinate AND every font size. The result had no size of its own: in a stage
// split the labels shrank below the app's smallest type, on a bay TV they outgrew its
// largest, and neither end looked like the rest of PinPoint. Now the chrome is Theme
// tokens throughout (see PpLmCard) and the composition is a real layout, so what grows
// with the frame is the geometry — which is the only thing on this view that has a reason
// to. Each schematic still scales by coordinate inside PpLmDiagram, into the box the fixed
// chrome leaves it.
//
// ONE CARD PER ROW WHEN SPACE IS SHORT. Below `kMinWideW` × `kMinWideH` the two-row
// composition stops being readable and the cards stack, each full width, each drawing
// therefore LARGER than its slot in the composition was. Both thresholds are in Theme.sp,
// so a golfer running larger type reflows EARLIER — with fixed fonts that is a real
// constraint, and the old scale-based floor could not express it.
//
// WHAT IS EXAGGERATED, AND WHERE IT SAYS SO. Drawn true, a 1.3° start direction and a 3.5°
// attack angle are a pixel of deflection each — a picture that says "straight" about a
// shot that was not. So plan-view and impact-view angles carry a gain, the flight profile
// carries the brief's 2.1× height and 2.5× lateral, and EVERY CARD WITH A GAIN STATES IT
// IN ITS CAPTION. The printed values never move: only the geometry is stretched, and the
// number beside it is always the measurement.
//
// COLOUR IS THE METRIC'S BAND, NOT THE CARD'S. The launch ray inside IMPACT is Launch
// teal; the smash chip inside PATH & FACE is Strike purple. That is what lets a reader
// carry one colour meaning between this view and the tiles board. Lines carry the hue;
// numbers stay colorText2, because a mid-chroma hue on colorSurface is the contrast
// failure the tiles board already had to fix once.

import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import PinPointStudio

Item {
    id: root

    // LmSessionModel.graphics. Defaulted so the component is safe to instantiate before
    // a shot exists — every accessor below degrades to "not reported".
    property var g: ({})

    // ── when the composition gives way to a column ──────────────────────────
    // Two-row composition or one card per row, decided on ROOM rather than on a scale
    // factor. There is no global scale any more: the chrome is fixed, so the question is
    // no longer "how small would this have to be drawn" but "is there space for five cards
    // side by side with legible labels in them". The narrowest card in the composition is
    // STRIKE at 248/1148 of the width, and its strip carries three figures; below roughly
    // sp(880) that card cannot show them without wrapping twice.
    //
    // NEVER A HORIZONTAL SCROLL. Reaching a card by dragging sideways is worse than
    // reading it smaller, and worse again than reading it in a column.
    readonly property int kMinWideW: Theme.sp(880)
    readonly property int kMinWideH: Theme.sp(470)
    readonly property bool reflow: width < kMinWideW || height < kMinWideH

    // The headline strip's band. Chrome, so a Theme size and not a fitted one.
    readonly property int headlineH: Theme.sp(46)
    readonly property int gap: Theme.sp(8)

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

    // The session's own spread for a field, for the shaded regions. `spread()` is the
    // three-shot floor from lm_session_reductions.h reaching the drawing: under it there
    // is no region at all, exactly as the tiles board hides its dispersion strip. A ±1 SD
    // wedge drawn from two shots is a claim about repeatability made from one gap.
    function spread(key) { const f = fld(key); return !!f && f.hasSpread === true }
    function sd(key)     { const f = fld(key); return (f && f.hasSpread) ? f.sdValue : 0 }
    function avg(key)    { const f = fld(key); return (f && f.n > 0) ? f.meanValue : 0 }

    // Which metric the reader is currently asking about, or "" for none.
    //
    // ONE PROPERTY RATHER THAN A FLAG PER REGION. Hover is exclusive by nature — a
    // pointer is over one label — and holding it centrally means a region and its label
    // cannot disagree about whether they are lit, which two independent booleans
    // eventually would.
    property string hoveredKey: ""
    function setHovered(key, on) {
        if (on) root.hoveredKey = key
        else if (root.hoveredKey === key) root.hoveredKey = ""
    }

    readonly property var flight: (g && g.flight) ? g.flight : ({ has: false })
    readonly property var shape:  (g && g.shape)  ? g.shape  : ({ has: false })
    readonly property var strike: (g && g.strike) ? g.strike : ({ has: false })
    // The one read on this view that may be PinPoint's own rather than the device's —
    // see LmSessionModel::buildGraphics for why the IMPACT card is allowed the fallback
    // and why `source` travels with the number.
    readonly property var lowPoint: (g && g.lowPoint) ? g.lowPoint
                                  : ({ has: false, value: 0, text: "—",
                                       unit: "in", source: "inferred" })

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
    // `weight` is the design width the composition shares out; `dw`/`dh` are the
    // schematic's own design box, which is what gives a card in the reflowed column its
    // height. `readKeys` is the card's strip — the metrics it reports, in reading order.
    // One list drives both layouts, so the wide composition and the reflowed column cannot
    // drift apart.
    readonly property var cards: [
        { title: qsTr("Path & face"), hue: hueClub,
          caption: qsTr("plan view · target right · angles ×%1").arg(kPlanGain),
          weight: kLeftWeight, dw: 672, dh: 182, comp: pathFaceComp, inferred: "shape",
          readKeys: [
              { key: "lm.clubheadSpeed",   label: qsTr("CLUB SPEED") },
              { key: "lm.clubPath",        label: qsTr("CLUB PATH") },
              { key: "lm.faceAngle",       label: qsTr("FACE ANGLE") },
              { key: "lm.faceToPath",      label: qsTr("FACE TO PATH") },
              { key: "lm.launchDirection", label: qsTr("START DIRECTION") },
              { key: "lm.closureRate",     label: qsTr("CLOSURE RATE ↻") },
          ] },
        { title: qsTr("Spin"), hue: hueSpin,
          caption: qsTr("down the line · axis ×%1").arg(kSpinGain),
          weight: kRightWeight, dw: 300, dh: 220, comp: spinComp, inferred: "",
          readKeys: [
              { key: "lm.spinRate", label: qsTr("SPIN RATE · TOTAL") },
              { key: "lm.backSpin", label: qsTr("BACK SPIN") },
              { key: "lm.sideSpin", label: qsTr("SIDE SPIN") },
              { key: "lm.spinAxis", label: qsTr("SPIN AXIS") },
          ] },
        { title: qsTr("Impact"), hue: hueClub,
          caption: qsTr("side view · loft and launch true · attack ×%1").arg(kAttackGain),
          weight: 432, dw: 408, dh: 182, comp: impactComp, inferred: "",
          readKeys: [
              { key: "lm.attackAngle", label: qsTr("ATTACK ANGLE") },
              { key: "lm.dynamicLoft", label: qsTr("DYN. LOFT") },
              { key: "lm.launchAngle", label: qsTr("LAUNCH ANGLE") },
              // Spin loft has no line of its own: it IS the gap between the two club
              // lines already on the card, so the note says which two rather than adding
              // a third rule that only restates them.
              { key: "lm.spinLoft",    label: qsTr("SPIN LOFT"),
                note: qsTr("· LOFT LESS ATTACK") },
              { key: "lowPoint",       label: qsTr("LOW POINT") },
          ] },
        { title: qsTr("Strike"), hue: hueStrike,
          caption: "", captionKind: "strike",
          weight: 248, dw: 280, dh: 190, comp: strikeComp, inferred: "strike",
          readKeys: [
              { key: "lm.strikeLocation", label: qsTr("STRIKE LOC.") },
              { key: "lm.strikeHeight",   label: qsTr("STRIKE HT.") },
              { key: "lm.lieAngle",       label: qsTr("LIE ANGLE") },
          ] },
        { title: qsTr("Flight"), hue: hueFlight,
          caption: "", captionKind: "flight",
          weight: kRightWeight, dw: 428, dh: 182, comp: flightComp, inferred: "",
          readKeys: [
              { key: "lm.carryDistance", label: qsTr("CARRY") },
              { key: "lm.totalDistance", label: qsTr("TOTAL") },
              { key: "lm.peakHeight",    label: qsTr("PEAK HEIGHT") },
              { key: "lm.descentAngle",  label: qsTr("DESCENT") },
              { key: "lm.offline",       label: qsTr("OFFLINE") },
          ] },
    ]

    // THE COMPOSITION IS TWO COLUMNS. Both rows split on the same pair of numbers, so the
    // seam between the left and right columns is one vertical line down the whole board
    // rather than two that nearly agree. From the brief's own top row, which is the one
    // that was already self-consistent.
    readonly property int kLeftWeight:  696     // PATH & FACE · IMPACT + STRIKE
    readonly property int kRightWeight: 444     // SPIN · FLIGHT

    // The composition's slots, by index into `cards` — so the grouping and the card
    // definitions cannot drift apart either.
    readonly property var topRow:     [cards[0], cards[1]]
    readonly property var bottomLeft:  [cards[2], cards[3]]
    readonly property var bottomRight: [cards[4]]

    // ── resolving what changes ──────────────────────────────────────────────
    // NOTHING ON `cards` MAY DEPEND ON THE SHOT OR THE GOLFER. The list is a Repeater's
    // model, so a dependency in it re-evaluates the whole list, and a Repeater handed a
    // new model tears down and rebuilds every delegate — five cards, five diagrams and
    // their Shapes — to change a caption. Worse than the cost: for a beat there are two
    // sets of cards alive, one of them detached, and anything measuring the view can
    // measure the wrong one. That is not hypothetical; the mirroring test caught it,
    // reading a mark on the STRIKE card against a stale frame.
    //
    // So the list carries KINDS and KEYS, and the delegate's own bindings resolve them.
    // A caption changes; a card does not.
    function captionFor(card) {
        if (card.captionKind === "strike")
            return root.leftHanded ? qsTr("face on · heel left") : qsTr("face on · toe left")
        if (card.captionKind === "flight")
            return root.flightCaption
        return card.caption
    }

    function readsFor(list) {
        const out = []
        if (!list) return out
        for (let i = 0; i < list.length; ++i) {
            const r = list[i]
            if (r.key === "lowPoint") {
                // Ours or the device's, and it says which. No metricKey: there is no
                // ±1 SD region behind a figure the session has no spread for.
                const lp = root.lowPoint
                out.push({ label: r.label,
                           value: lp.has === true ? lp.text : "—",
                           unit:  lp.has === true ? lp.unit : "",
                           hue:   root.hueClub,
                           metricKey: "",
                           note:  (lp.has === true && lp.source === "inferred")
                                  ? qsTr("· PPS EST.") : "" })
                continue
            }
            out.push({ label: r.label,
                       value: root.txt(r.key),
                       unit:  root.unit(r.key),
                       hue:   root.hue(r.key),
                       metricKey: r.key,
                       note:  r.note !== undefined ? r.note : "" })
        }
        return out
    }

    function inferredFor(kind) {
        if (kind === "shape")
            return { has: root.shape.has === true,
                     label: qsTr("INFERRED FLIGHT · START × CURVE"),
                     name: root.shape.name !== undefined ? root.shape.name : "",
                     evidence: root.shape.evidence !== undefined ? root.shape.evidence : "" }
        if (kind === "strike")
            return { has: root.strike.has === true,
                     label: qsTr("INFERRED STRIKE"),
                     name: root.strike.name !== undefined ? root.strike.name : "",
                     evidence: root.strike.evidence !== undefined ? root.strike.evidence : "" }
        return null
    }

    // One card, wired to the body. Declared once so the composition and the column cannot
    // wire the same card two different ways.
    component BodyCard : PpLmCard {
        required property var modelData
        hue: modelData.hue
        title: modelData.title
        caption: root.captionFor(modelData)
        diagram: modelData.comp
        // The schematic's design box, which is what lets the card work out whether the
        // drawing is big enough to carry its own readings.
        dw: modelData.dw
        dh: modelData.dh
        // THE STRIP IS THE FALLBACK, not the home. When the drawing is at design size or
        // better it annotates itself and the strip is empty — a reading beside the line it
        // names beats the same reading in a row underneath, every time. The strip exists
        // for the sizes where that is not on offer.
        reads: hosted ? [] : root.readsFor(modelData.readKeys)
        inferred: hosted ? null : root.inferredFor(modelData.inferred)
        onReadHovered: (key, on) => root.setHovered(key, on)
    }

    // ── wide: the design's two-row composition ──────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        visible: !root.reflow
        spacing: root.gap

        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: root.headlineH
            active: !root.reflow
            sourceComponent: headlineComp
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: root.gap
            Repeater {
                // Emptied rather than merely hidden when the other layout is showing. An
                // invisible card is still a diagram, its Shapes and a hundred bindings
                // kept alive to draw nothing — and `visible: false` on the parent does not
                // stop any of it being built.
                model: root.reflow ? [] : root.topRow
                BodyCard {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    // PINNED TO ZERO, AND IT HAS TO BE. A card's implicitHeight counts its
                    // strip, the strip exists only when the drawing is too small to host
                    // its readings, and that is decided from the height the row gave the
                    // card — so letting implicitHeight feed the row closes the circle:
                    // row height → hosted → strip → implicitHeight → row height. It spins
                    // forever. The rows are filled from above and split evenly; nothing
                    // about how tall a card would LIKE to be may reach them.
                    Layout.preferredHeight: 0
                    Layout.minimumHeight: 0
                    // The design's own proportions, as weights rather than as pixels: the
                    // cards keep their relative widths at any frame size, and the chrome
                    // inside them keeps its own. Preferred width AND stretch factor both
                    // carry the weight, which is what makes the share exactly proportional
                    // in both directions — growing distributes the surplus by stretch,
                    // shrinking takes it back in proportion to preferred.
                    Layout.preferredWidth: modelData.weight
                    Layout.horizontalStretchFactor: modelData.weight
                    Layout.minimumWidth: 0
                }
            }
        }

        // THE BOTTOM ROW IS TWO COLUMNS, NOT THREE, and that is what makes the seam line
        // up. Five independent weights cannot: the brief's own figures put SPIN at
        // 704…1148 and FLIGHT at 696…1148, so the right-hand column stepped 8 px sideways
        // between the two rows, and at full screen the eye caught it every time. Nesting
        // IMPACT and STRIKE inside one block carrying the SAME weight as PATH & FACE above
        // makes the alignment structural — the column edges are one number used twice, so
        // there is nothing left to drift.
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: root.gap

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: 0
                Layout.minimumHeight: 0
                Layout.preferredWidth: root.kLeftWeight
                Layout.horizontalStretchFactor: root.kLeftWeight
                Layout.minimumWidth: 0
                spacing: root.gap

                Repeater {
                    model: root.reflow ? [] : root.bottomLeft
                    BodyCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: 0
                        Layout.minimumHeight: 0
                        // Weights again, but here only their RATIO matters: the pair is
                        // handed the left column's width and splits it between them.
                        Layout.preferredWidth: modelData.weight
                        Layout.horizontalStretchFactor: modelData.weight
                        Layout.minimumWidth: 0
                    }
                }
            }

            Repeater {
                model: root.reflow ? [] : root.bottomRight
                BodyCard {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 0
                    Layout.minimumHeight: 0
                    // The right column's width — the same number SPIN uses above it.
                    Layout.preferredWidth: root.kRightWeight
                    Layout.horizontalStretchFactor: root.kRightWeight
                    Layout.minimumWidth: 0
                }
            }
        }
    }

    // ── narrow: one column, scrolled vertically ─────────────────────────────
    // Each card takes the full width and its drawing keeps its own aspect, so a schematic
    // in the column is LARGER than it was in the composition, not smaller. That is the
    // whole point of reflowing rather than shrinking on.
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
            spacing: root.gap

            Loader {
                active: root.reflow
                sourceComponent: headlineComp
                width: stack.width
                height: root.headlineH
            }

            Repeater {
                model: root.reflow ? root.cards : []
                BodyCard {
                    width: stack.width
                    // Capped, or a full-width STRIKE card (the squarest schematic) would
                    // be taller than the panel it is scrolling inside.
                    diagramH: Math.min(Theme.sp(200),
                                       Math.round((stack.width - 2 * padX)
                                                  * modelData.dh / modelData.dw))
                    height: implicitHeight
                }
            }
        }
    }

    // ── headline strip ──────────────────────────────────────────────────────
    // The six figures a golfer reads first. Fixed type, like everything else that is not
    // a drawing — this strip is the panel's summary line, and a summary that changed size
    // with the window would be the loudest thing on the view at one end and invisible at
    // the other.
    Component {
        id: headlineComp

        Item {
            id: strip

            Row {
                anchors { left: parent.left; verticalCenter: parent.verticalCenter }

                Repeater {
                    model: root.g && root.g.headline ? root.g.headline : []

                    Row {
                        required property var modelData
                        required property int index

                        Rectangle {           // the divider between figures
                            width: 1
                            height: Theme.sp(30)
                            anchors.verticalCenter: parent.verticalCenter
                            visible: index > 0
                            color: Theme.colorBorderMid
                        }
                        Item {
                            width: Theme.sp(20) * 2 + inner.width
                            height: inner.height
                            Column {
                                id: inner
                                x: Theme.sp(20)
                                spacing: Theme.sp(2)
                                // The same eyebrow as a card's title and as every section
                                // heading on the Diagnostics screen. colorText3 here is
                                // correct where it would not be on a tile: this is a LABEL
                                // on the panel background, not a figure on colorSurface —
                                // the rule the tiles board settled on is about numbers.
                                Text {
                                    text: modelData.abbrev
                                    font.family: Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    font.capitalization: Font.AllUppercase
                                    font.letterSpacing: Theme.trackingMicro
                                    color: Theme.colorText3
                                }
                                Row {
                                    spacing: Theme.sp(3)
                                    Text {
                                        id: headlineValue
                                        text: modelData.text
                                        font.family: Theme.fontData
                                        font.pixelSize: Theme.fontSzData
                                        font.letterSpacing: -0.8
                                        color: Theme.colorText
                                    }
                                    Text {
                                        anchors.baseline: headlineValue.baseline
                                        text: modelData.unit
                                        font.family: Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
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

    // A ball. The design has an image slot here; with no artwork shipped it is the disc
    // fallback the brief calls for, which is a complete read on its own.
    component Ball : Rectangle {
        radius: width / 2
        color: Theme.colorText
        opacity: 0.85
    }

    // A reading at its design anchor on a drawing. Thin wrapper over the shared block so
    // the two dozen call sites below do not each repeat the hover wiring.
    component Read : PpLmRead {
        onHovered: (key, on) => root.setHovered(key, on)
    }

    // A BAND OF READINGS BESIDE A DRAWING — two across, or two down.
    //
    // For the cards whose schematic is nearly square (SPIN, STRIKE) there is no good design
    // anchor for a reading: put it beside the drawing and it wastes the height, put it under
    // and it wastes the width. Which is right depends on the shape of the box the card got,
    // so the band does both and the card picks.
    //
    // CELL WIDTH IS GIVEN, NOT MEASURED, in the across arrangement. Two bands sized to their
    // own content would set their columns in different places, and two rows of readings that
    // nearly line up look worse than either arrangement done plainly.
    component ReadBand : Item {
        id: band
        property var keys: []            // [{ key, label }]
        property bool vertical: false
        property real cellW: 0
        property real cellH: 0
        property real gap: Theme.sp(10)

        // How wide this band wants to be when it runs down the side, where the card has to
        // ask before deciding what to give the drawing.
        //
        // MEASURED OFF A HIDDEN COPY, not off childrenRect. The band's width comes from this
        // number, so reading it back out of the laid-out children closes the loop — Qt says
        // so out loud, and the arrangement it settles on is then whichever pass happened to
        // run last. The sizer depends on nothing but its own text.
        readonly property string widestLabel: {
            let w = ""
            for (let i = 0; i < keys.length; ++i)
                if (keys[i].label.length > w.length) w = keys[i].label
            return w
        }
        PpLmRead { id: sizer; visible: false; label: band.widestLabel; value: "0000"; unit: "rpm" }
        implicitWidth: vertical ? sizer.implicitWidth : 0
        // What one cell needs if this band runs ACROSS. The card asks before choosing that
        // arrangement — a row of readings the words do not fit into is how two labels ended
        // up printed over each other.
        readonly property real naturalCellW: sizer.implicitWidth

        Repeater {
            model: band.keys
            PpLmRead {
                required property var modelData
                required property int index
                x: band.vertical ? 0 : index * (band.cellW + band.gap)
                y: band.vertical ? index * (band.cellH + band.gap) : 0
                label: modelData.label
                metricKey: modelData.key
                value: root.txt(modelData.key)
                unit:  root.unit(modelData.key)
                hue:   root.hue(modelData.key)
                onHovered: (key, on) => root.setHovered(key, on)
            }
        }
    }

    // An inferred read, in place on the drawing: the name this panel put on the shot and
    // the evidence for it. The card's strip carries the same thing when the drawing is too
    // small to host it — see PpLmCard.
    component Inferred : Column {
        id: inf
        objectName: "anno"
        property string label: ""
        property string name: ""
        property string evidence: ""
        property color hue: Theme.colorText2
        spacing: Theme.sp(2)

        Text {
            width: inf.width
            text: inf.label
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
            elide: Text.ElideRight
        }
        Text {
            // The read sits where a reading's figure sits in every other block on the
            // board, so it takes the same size — a block whose headline was smaller than
            // the numbers beside it read as a caption rather than as the answer.
            text: inf.name
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzData
            color: inf.hue
        }
        Text {
            // THE AUDIT TRAIL. Not decoration, and never demoted to colorText3 or below
            // the micro floor — it is the only thing on the card that shows the reasoning
            // behind an inferred word.
            width: inf.width
            text: inf.evidence
            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText2
            elide: Text.ElideRight
        }
    }

    // ── 1. PATH & FACE — plan view, looking down, target to the right ───────
    // Up on screen is LEFT of the target line, so a shot that started left draws upward.
    Component {
        id: pathFaceComp

        PpLmDiagram {
            id: pf
            designW: 672; designH: 182

            // The shared top of the bottom row, measured up from the frame's floor by the
            // tallest thing in it. See the row itself, below, for why it is derived rather
            // than written as a design y.
            readonly property real bottomRowTop:
                Math.max(pf.d(104), pf.ly(174) - shapeBlock.height)

            // ── the session's spread, behind everything ──────────────────────
            // Declared first, so it stacks under every line and label on the card. Each
            // wedge carries the SAME gain as the vector it sits behind — a region drawn
            // true while its line is drawn ×5 would shade the wrong place entirely.
            PpLmSpread {
                s: pf.s; hue: root.hueClub; shape: "wedge"
                metricKey: "lm.clubPath"
                active: root.hoveredKey === "lm.clubPath"
                onIsHoveredChanged: root.setHovered("lm.clubPath", isHovered)
                visible: root.spread("lm.clubPath")
                pivotX: 300; pivotY: 96; radius: 170
                meanAngle: root.avg("lm.clubPath") * root.kPlanGain
                halfAngle: root.sd("lm.clubPath") * root.kPlanGain
            }
            PpLmSpread {
                s: pf.s; hue: root.hueClub; shape: "wedge"
                metricKey: "lm.faceAngle"
                active: root.hoveredKey === "lm.faceAngle"
                onIsHoveredChanged: root.setHovered("lm.faceAngle", isHovered)
                visible: root.spread("lm.faceAngle")
                pivotX: 300; pivotY: 96; radius: 36
                meanAngle: 90 + root.avg("lm.faceAngle") * root.kPlanGain
                halfAngle: root.sd("lm.faceAngle") * root.kPlanGain
            }
            PpLmSpread {
                s: pf.s; hue: root.hueLaunch; shape: "wedge"
                metricKey: "lm.launchDirection"
                active: root.hoveredKey === "lm.launchDirection"
                onIsHoveredChanged: root.setHovered("lm.launchDirection", isHovered)
                visible: root.spread("lm.launchDirection")
                pivotX: 300; pivotY: 96; radius: 300
                meanAngle: root.avg("lm.launchDirection") * root.kPlanGain
                halfAngle: root.sd("lm.launchDirection") * root.kPlanGain
            }

            PpLmRule {                                   // target line
                x: pf.d(20); y: pf.d(96); s: pf.s
                len: 632; kind: "reference"
            }
            Text {
                x: pf.d(20); y: pf.d(100)
                text: qsTr("TARGET LINE")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
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

            // The brief's anchors, restored. Each reading sits beside the line it names —
            // which is the whole reason a schematic beats a table, and is why these move to
            // the card's strip only when the drawing is too small to hold them.
            Read { visible: pf.hosted; x: pf.d(16);  y: pf.ly(14);  hue: root.hueClub
                   label: qsTr("CLUB SPEED");    metricKey: "lm.clubheadSpeed"; value: root.txt("lm.clubheadSpeed"); unit: root.unit("lm.clubheadSpeed") }
            Read { visible: pf.hosted; x: pf.d(190); y: pf.ly(14);  hue: root.hueClub
                   label: qsTr("CLOSURE RATE ↻"); metricKey: "lm.closureRate"; value: root.txt("lm.closureRate");   unit: root.unit("lm.closureRate") }
            Read { visible: pf.hosted; x: pf.d(372); y: pf.ly(14);  hue: root.hueClub
                   label: qsTr("FACE TO PATH");  metricKey: "lm.faceToPath"; value: root.txt("lm.faceToPath");     unit: root.unit("lm.faceToPath") }
            // Set well right of FACE TO PATH and under the far end of the start-direction
            // ray, which is where the eye already is — and spelled out, because "START
            // DIR." saves eleven pixels on a card that has room and costs the reader the
            // one word that says which direction is meant.
            Read { visible: pf.hosted; x: pf.d(520); y: pf.ly(14);  hue: root.hueLaunch
                   label: qsTr("START DIRECTION"); metricKey: "lm.launchDirection"; value: root.txt("lm.launchDirection"); unit: root.unit("lm.launchDirection") }
            // THE BOTTOM ROW: club path, face angle and the inferred read, all on one y.
            // Ragged tops under a drawing read as three separate afterthoughts; one line
            // reads as a row, which is what they are.
            //
            // The y comes from the BOTTOM of the frame rather than being a number, because
            // the tallest item in the row — the inferred block — is fixed-size type inside
            // a frame that scales, so its height in design pixels is not a constant. Anchor
            // the row to the frame's floor and it sits right at every size instead of only
            // at the one the anchors were written for.
            //
            // FACE ANGLE moved left, off x 250. The face rule is 72 long centred on the ball
            // at x 300, so it reaches down to y 132 through exactly where that label used to
            // start — which the old y of 146 dodged by sitting lower. It cannot sit lower
            // now that it shares the row, so it steps aside instead.
            Read { visible: pf.hosted; x: pf.d(16);  y: pf.bottomRowTop; hue: root.hueClub
                   label: qsTr("CLUB PATH");     metricKey: "lm.clubPath"; value: root.txt("lm.clubPath");        unit: root.unit("lm.clubPath") }
            Read { visible: pf.hosted; x: pf.d(190); y: pf.bottomRowTop; hue: root.hueClub
                   label: qsTr("FACE ANGLE");    metricKey: "lm.faceAngle"; value: root.txt("lm.faceAngle");       unit: root.unit("lm.faceAngle") }
            // BALL SPEED and SMASH are NOT repeated here. Both lead the headline strip a
            // few pixels above, and a figure printed twice on one screen makes a reader
            // check whether the two agree instead of reading either. The strip is the
            // right home for them: they are the shot's summary, not an annotation on any
            // particular line of the geometry.

            // The inferred flight read: the name this panel put on the shot, and the
            // evidence for a claim the device did not make.
            //
            // THE NINE-WINDOW GRID IS GONE. It drew start direction × curvature as a 3 × 3
            // with the shot's cell lit — and it was the only element on these five cards
            // that restated a value printed beside it rather than adding to it. "Pull–fade"
            // IS the lit column and the lit row, spelled out, with the evidence line under
            // it giving the numbers that put it there. Worse than redundant, it misread: a
            // 3 × 3 of squares is a shape people fill in from what they expect, and the
            // expectation it met was a height axis — low/mid/high — which is not on this
            // card at all. A picture that has to be captioned to stop it saying the wrong
            // thing is not carrying its weight beside the sentence that says the right one.
            Inferred {
                id: shapeBlock
                // Bottom right of the frame: 16 design px in from the right edge, and its
                // floor is what the row's shared y is measured up from. It is the only thing
                // on this card that is a block rather than a line or a figure, and the
                // corner is where a block belongs — out of the geometry's way, and at the
                // end of the reading order rather than in the middle of it.
                x: pf.d(672 - 256 - 16)
                y: pf.bottomRowTop
                width: pf.d(256)
                visible: pf.hosted && root.shape.has === true
                hue: root.hueFlight
                label: qsTr("INFERRED FLIGHT · START × CURVE")
                name: root.shape.name !== undefined ? root.shape.name : ""
                evidence: root.shape.evidence !== undefined ? root.shape.evidence : ""
            }
        }
    }

    // ── 2. SPIN — down the line ─────────────────────────────────────────────
    Component {
        id: spinComp

        // TWO ABOVE AND TWO BELOW, OR TWO DOWN EACH SIDE. The ball is round and the four
        // spin readings are equals — there is no line for any of them to sit beside, so the
        // only question is which way the card's spare room runs. Stacked when the height is
        // there, flanked when it is not, and the drawing takes whatever is left either way.
        Item {
            id: sc
            property bool hosted: true

            // One reading's height, measured rather than assumed — it is two Theme sizes and
            // a gap, and hard-coding that sum would rot the first time either token moved.
            PpLmRead { id: rowSizer; visible: false; label: "X"; value: "0"; unit: "y" }
            readonly property real cellH: rowSizer.implicitHeight
            readonly property real gapY: Theme.sp(12)
            readonly property real gapX: Theme.sp(16)
            readonly property real minDrawH: Theme.sp(120)

            // Stacked is preferred where it fits: a reading over the drawing keeps the
            // drawing its full width, and the width is what this schematic is short of.
            readonly property bool stacked: (height - 2 * (cellH + gapY)) >= minDrawH
            readonly property real sideW: Math.max(bandA.implicitWidth, bandB.implicitWidth)

            readonly property real insetY: (hosted && stacked)  ? cellH + gapY : 0
            readonly property real insetX: (hosted && !stacked) ? sideW + gapX : 0

            ReadBand {
                id: bandA
                visible: sc.hosted
                vertical: !sc.stacked
                keys: [ { key: "lm.spinRate", label: qsTr("SPIN RATE · TOTAL") },
                        { key: "lm.backSpin", label: qsTr("BACK SPIN") } ]
                cellW: (sc.width - gap) / 2
                cellH: sc.cellH
                x: 0
                y: sc.stacked ? 0 : (sc.height - (2 * sc.cellH + gap)) / 2
                width: sc.stacked ? sc.width : sc.sideW
            }
            ReadBand {
                id: bandB
                visible: sc.hosted
                vertical: !sc.stacked
                keys: [ { key: "lm.sideSpin", label: qsTr("SIDE SPIN") },
                        { key: "lm.spinAxis", label: qsTr("SPIN AXIS") } ]
                cellW: (sc.width - gap) / 2
                cellH: sc.cellH
                x: sc.stacked ? 0 : sc.width - sc.sideW
                y: sc.stacked ? sc.height - sc.cellH
                              : (sc.height - (2 * sc.cellH + gap)) / 2
                width: sc.stacked ? sc.width : sc.sideW
            }

            PpLmDiagram {
                id: sp
                x: sc.insetX
                y: sc.insetY
                width: sc.width - 2 * sc.insetX
                height: sc.height - 2 * sc.insetY
                // The GEOMETRY's own box, with the label margins the old one carried taken out —
                // the readings live outside the drawing now, so the drawing no longer has to
                // reserve room for them and grows into what it saves.
                designW: 296; designH: 132

                PpLmSpread {
                    s: sp.s; hue: root.hueSpin; shape: "wedge"
                    metricKey: "lm.spinAxis"
                    active: root.hoveredKey === "lm.spinAxis"
                    onIsHoveredChanged: root.setHovered("lm.spinAxis", isHovered)
                    visible: root.spread("lm.spinAxis")
                    pivotX: 150; pivotY: 84; radius: 90
                    meanAngle: root.avg("lm.spinAxis") * root.kSpinGain
                    halfAngle: root.sd("lm.spinAxis") * root.kSpinGain
                }

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
                    // A drawn mark rather than type: it is the ball's rotation, and it belongs
                    // to the picture's scale in a way that a label does not.
                    font.pixelSize: Math.max(1, Math.round(sp.d(17)))
                    color: root.hueSpin
                    visible: root.has("lm.spinRate")
                }

            }
        }
    }

    // ── 3. IMPACT — side view ───────────────────────────────────────────────
    Component {
        id: impactComp

        PpLmDiagram {
            id: im
            designW: 408; designH: 182

            // Where the arc bottomed out, in design pixels from the ball. Clamped rather
            // than scaled to the value: the card is a fixed drawing and a 14 in fat shot
            // must not push the mark off it, but the FIGURE in the strip is never clamped
            // — the drawing saturates, the reading does not.
            readonly property real kPxPerIn: 14
            readonly property real lowPointX:
                Math.max(30, Math.min(380, 150 + root.lowPoint.value * kPxPerIn))

            // Attack angle's line runs BACKWARD from the ball, so its wedge is swept
            // about the same pivot but 180° round — the region has to lie where the line
            // is drawn, not where its angle nominally points.
            PpLmSpread {
                s: im.s; hue: root.hueClub; shape: "wedge"
                metricKey: "lm.attackAngle"
                active: root.hoveredKey === "lm.attackAngle"
                onIsHoveredChanged: root.setHovered("lm.attackAngle", isHovered)
                visible: root.spread("lm.attackAngle")
                pivotX: 150; pivotY: 132; radius: 130
                meanAngle: 180 - root.avg("lm.attackAngle") * root.kAttackGain
                halfAngle: root.sd("lm.attackAngle") * root.kAttackGain
            }
            PpLmSpread {
                s: im.s; hue: root.hueClub; shape: "wedge"
                metricKey: "lm.dynamicLoft"
                active: root.hoveredKey === "lm.dynamicLoft"
                onIsHoveredChanged: root.setHovered("lm.dynamicLoft", isHovered)
                visible: root.spread("lm.dynamicLoft")
                pivotX: 150; pivotY: 132; radius: 66
                meanAngle: -90 - root.avg("lm.dynamicLoft")
                halfAngle: root.sd("lm.dynamicLoft")
            }
            PpLmSpread {
                s: im.s; hue: root.hueLaunch; shape: "wedge"
                metricKey: "lm.launchAngle"
                active: root.hoveredKey === "lm.launchAngle"
                onIsHoveredChanged: root.setHovered("lm.launchAngle", isHovered)
                visible: root.spread("lm.launchAngle")
                pivotX: 150; pivotY: 132; radius: 216
                meanAngle: -root.avg("lm.launchAngle")
                halfAngle: root.sd("lm.launchAngle")
            }

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

            // ── low point ────────────────────────────────────────────────────
            // The third side of the figure this card already draws: attack angle says how
            // steeply the club arrived, dynamic loft what it presented, and this says
            // where the arc actually bottomed out relative to the ball. Drawn on the
            // GROUND, because that is the reference it is stated against.
            //
            // A RULER, in the line grammar's sense — kBracket, neutral and fainter, with
            // end ticks. It measures a distance between two things on the drawing rather
            // than describing a direction, which is exactly what that kind is for.
            PpLmRule {
                // y 148, not 152: the readings row starts at 152 and the ruler has to sit
                // between the ground line and it.
                x: im.d(Math.min(150, im.lowPointX)); y: im.d(148); s: im.s
                len: Math.max(1, Math.abs(im.lowPointX - 150))
                kind: "bracket"
                visible: root.lowPoint.has === true
            }
            PpLmRule {                                   // the mark itself, on the ground
                x: im.d(im.lowPointX); y: im.d(133); s: im.s
                len: 14; thickness: 1.5
                kind: "club"; hue: root.hueClub
                pivot: "left"; angle: 90
                visible: root.lowPoint.has === true
            }

            Ball { x: im.d(143); y: im.d(125); width: im.d(14); height: im.d(14) }
            Ball {
                readonly property real a: -root.num("lm.launchAngle") * Math.PI / 180
                visible: root.has("lm.launchAngle")
                width: im.d(10); height: im.d(10)
                x: im.d(150) + im.d(216) * Math.cos(a) - width / 2
                y: im.d(132) + im.d(216) * Math.sin(a) - height / 2
            }

            Read { visible: im.hosted; x: im.d(14);  y: im.ly(18);  hue: root.hueClub
                   label: qsTr("ATTACK ANGLE"); metricKey: "lm.attackAngle"; value: root.txt("lm.attackAngle"); unit: root.unit("lm.attackAngle") }
            Read { visible: im.hosted; x: im.d(152); y: im.ly(18);  hue: root.hueClub
                   label: qsTr("DYN. LOFT");    metricKey: "lm.dynamicLoft"; value: root.txt("lm.dynamicLoft"); unit: root.unit("lm.dynamicLoft") }
            Read { visible: im.hosted; x: im.d(290); y: im.ly(18);  hue: root.hueLaunch
                   label: qsTr("LAUNCH ANGLE"); metricKey: "lm.launchAngle"; value: root.txt("lm.launchAngle"); unit: root.unit("lm.launchAngle") }
            // Spin loft is not drawn as a wedge: it IS the gap between the two club lines
            // already on this card, so the card says so rather than adding a third line
            // that only restates them.
            Read { visible: im.hosted; x: im.d(14);  y: im.ly(146); hue: root.hueClub
                   label: qsTr("SPIN LOFT"); note: qsTr("· LOFT LESS ATTACK")
                   metricKey: "lm.spinLoft"; value: root.txt("lm.spinLoft"); unit: root.unit("lm.spinLoft") }
            // Beside the ruler it belongs to, at the right-hand end of the ground line.
            Read { visible: im.hosted; x: im.d(272); y: im.ly(146); hue: root.hueClub
                   label: qsTr("LOW POINT")
                   note: (root.lowPoint.has === true && root.lowPoint.source === "inferred")
                         ? qsTr("· PPS EST.") : ""
                   value: root.lowPoint.has === true ? root.lowPoint.text : "—"
                   unit: root.lowPoint.has === true ? root.lowPoint.unit : "" }
        }
    }

    // ── 4. STRIKE — face on ─────────────────────────────────────────────────
    // TOE LEFT, HEEL RIGHT for a right-hander, and MIRRORED for a left-hander: looking
    // at the face of a left-handed club, the toe is on the other side. This orientation
    // was wrong once in review, which is why the mirror is one signed factor used by
    // every horizontal term on the card rather than three independent conditionals.
    //
    // THE FACE PATTERN IS THE CARD. It briefly shared the box with a lie-angle schematic,
    // which cost a third of the width to draw one short line and squeezed the thing a coach
    // actually reads — where on the face the ball was struck, against the session's own
    // pattern — down to a thumbnail. Lie angle is still reported; it just does not need a
    // picture. One drawing, given the whole card.
    Component {
        id: strikeComp

        Item {
            id: sc
            property bool hosted: true

            PpLmRead { id: rowSizer; visible: false; label: "X"; value: "0"; unit: "y" }
            readonly property real cellH: rowSizer.implicitHeight
            readonly property real gapY: Theme.sp(12)
            readonly property real gapX: Theme.sp(14)
            readonly property real minDrawH: Theme.sp(110)

            readonly property bool showInferred: hosted && root.strike.has === true
            readonly property real inferredH: showInferred ? inferred.implicitHeight : 0
            readonly property real inferredW: Theme.sp(150)

            // ONE TEST DRIVES EVERYTHING ON THIS CARD. Where the readings go and where the
            // inferred read goes are the same question asked once — is the spare room
            // vertical or horizontal — and answering it twice would let the card end up with
            // its labels stacked and its verdict beside them, which reads as two layouts
            // colliding rather than one adapting.
            //
            // WIDTH IS PART OF THE TEST, not just height. Three readings across a narrow card
            // is how "STRIKE LOC." and "STRIKE HT." ended up printed over each other: there
            // was height for a row, so a row is what it drew, with no one asking whether the
            // words fit in it.
            readonly property bool roomAcross:
                (width - 2 * gapX) / 3 >= band.naturalCellW
            readonly property bool stacked: roomAcross
                && (height - cellH - gapY - (showInferred ? inferredH + gapY : 0)) >= minDrawH

            readonly property real bodyW: width - ((showInferred && !stacked) ? inferredW + gapX : 0)
            readonly property real bodyH: height - ((showInferred && stacked) ? inferredH + gapY : 0)

            ReadBand {
                id: band
                visible: sc.hosted
                vertical: !sc.stacked
                keys: [ { key: "lm.strikeLocation", label: qsTr("STRIKE LOC.") },
                        { key: "lm.strikeHeight",   label: qsTr("STRIKE HT.") },
                        { key: "lm.lieAngle",       label: qsTr("LIE ANGLE") } ]
                cellW: (sc.bodyW - 2 * gap) / 3
                cellH: sc.cellH
                x: 0
                y: sc.stacked ? 0 : (sc.bodyH - (3 * sc.cellH + 2 * gap)) / 2
                width: sc.stacked ? sc.bodyW : implicitWidth
            }

            PpLmDiagram {
                id: sk
                x: sc.stacked ? 0 : (sc.hosted ? band.implicitWidth + sc.gapX : 0)
                y: (sc.stacked && sc.hosted) ? sc.cellH + sc.gapY : 0
                width: sc.bodyW - x
                height: sc.bodyH - y
                designW: 148; designH: 120

                readonly property real mirror: root.leftHanded ? -1 : 1
                readonly property real cx: 74
                readonly property real cy: 64
                readonly property real pxPerMm: 2.6

                // The session's strike PATTERN — a tilted ±1 SD ellipse, because a golfer's
                // misses lie on a diagonal and an axis-aligned blob would hide the one thing
                // a coach reads off this card. Legitimate as a rotated ellipse here (and
                // nowhere else on the panel) because both axes are millimetres drawn at the
                // same 2.6 px/mm — see lmPairStats() for why the landing pattern is not.
                //
                // Mirrored for a left-hander by negating the tilt along with the horizontal
                // offset: rotating the pattern without flipping it would draw a heel-low
                // golfer a toe-low one.
                PpLmSpread {
                    s: sk.s; hue: root.hueStrike; shape: "ellipse"
                    active: root.hoveredKey === "lm.strikeLocation"
                             || root.hoveredKey === "lm.strikeHeight"
                    metricKey: "lm.strikeLocation"
                    onIsHoveredChanged: root.setHovered("lm.strikeLocation", isHovered)
                    visible: root.g && root.g.strikeEllipse ? root.g.strikeEllipse.has === true : false
                    centreX: sk.cx - sk.mirror * sk.pxPerMm
                                     * (root.g.strikeEllipse ? root.g.strikeEllipse.meanX : 0)
                    centreY: sk.cy - sk.pxPerMm
                                     * (root.g.strikeEllipse ? root.g.strikeEllipse.meanY : 0)
                    radiusX: sk.pxPerMm * (root.g.strikeEllipse ? root.g.strikeEllipse.majorSd : 0)
                    radiusY: sk.pxPerMm * (root.g.strikeEllipse ? root.g.strikeEllipse.minorSd : 0)
                    // Screen y runs down while strike height runs up, so the data-space tilt
                    // is negated once for the flip and again for a left-hander.
                    tiltDeg: -sk.mirror * (root.g.strikeEllipse ? root.g.strikeEllipse.tiltDeg : 0)
                }

                PpLmRule { x: sk.d(14); y: sk.d(64); s: sk.s; len: 120   // crosshair, horizontal
                           kind: "reference" }
                PpLmRule { x: sk.d(74); y: sk.d(19); s: sk.s; len: 90    // crosshair, vertical
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
                    x: root.leftHanded ? sk.d(106) : sk.d(14); y: sk.d(4)
                    text: qsTr("TOE")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                }
                Text {
                    x: root.leftHanded ? sk.d(14) : sk.d(106); y: sk.d(4)
                    text: qsTr("HEEL")
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro; color: Theme.colorText3
                }
            }

            // ── the inferred read ────────────────────────────────────────────
            // Under the drawing when the room is vertical, out to the right when it is
            // horizontal — the same test the readings answered.
            Inferred {
                id: inferred
                visible: sc.showInferred
                x: sc.stacked ? 0 : sc.bodyW + sc.gapX
                y: sc.stacked ? sc.bodyH + sc.gapY : (sc.height - implicitHeight) / 2
                width: sc.stacked ? sc.width : sc.inferredW
                hue: root.hueStrike
                label: qsTr("INFERRED STRIKE")
                name: root.strike.name !== undefined ? root.strike.name : ""
                // Context, not evidence: smash is shown beside the read and takes no part in
                // it. A toe strike with a great smash is still a toe strike.
                evidence: root.strike.evidence !== undefined ? root.strike.evidence : ""
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

            // The landing pattern: ±1 SD of carry against ±1 SD of offline, on the
            // ground track. AXIS-ALIGNED, unlike the strike ellipse, and deliberately —
            // this card draws distance at 1.8 px/yd and lateral at 4.5, so a tilt
            // computed in yards would be the wrong tilt once drawn. An untilted region
            // understates the pattern; a wrongly-tilted one would misdescribe it.
            PpLmSpread {
                s: fl.s; hue: root.hueFlight; shape: "ellipse"
                active: root.hoveredKey === "lm.carryDistance"
                         || root.hoveredKey === "lm.offline"
                metricKey: "lm.carryDistance"
                onIsHoveredChanged: root.setHovered("lm.carryDistance", isHovered)
                visible: root.flight.has === true
                         && root.spread("lm.carryDistance") && root.spread("lm.offline")
                centreX: fl.px(root.avg("lm.carryDistance") / root.flightTotalYd)
                centreY: fl.pz(root.avg("lm.offline") / root.flightLatYd)
                radiusX: root.sd("lm.carryDistance") * root.xPerYd
                radiusY: root.sd("lm.offline") * root.zPerYd
                tiltDeg: 0
            }

            PpLmRule { x: fl.d(16); y: fl.d(92);  s: fl.s; len: 396      // ground
                       kind: "reference" }
            PpLmRule { x: fl.d(16); y: fl.d(146); s: fl.s; len: 396      // target line
                       kind: "reference" }

            Text { x: fl.d(18); y: fl.d(124); text: qsTr("L")
                   font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                   color: Theme.colorText3 }
            Text { x: fl.d(18); y: fl.d(158); text: qsTr("R")
                   font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
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
                visible: fl.hosted && root.flight.has === true
                Text {
                    id: carryTxt
                    anchors.centerIn: parent
                    text: qsTr("CARRY %1 %2").arg(root.txt("lm.carryDistance"))
                                             .arg(root.unit("lm.carryDistance"))
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText2
                }
            }

            Read { visible: fl.hosted; x: fl.d(52);  y: fl.ly(0);   hue: root.hueFlight
                   label: qsTr("PEAK HEIGHT"); metricKey: "lm.peakHeight"; value: root.txt("lm.peakHeight"); unit: root.unit("lm.peakHeight") }
            Read { visible: fl.hosted; x: fl.d(340); y: fl.ly(44);  hue: root.hueFlight
                   label: qsTr("DESCENT");     metricKey: "lm.descentAngle"; value: root.txt("lm.descentAngle"); unit: root.unit("lm.descentAngle") }
            Read { visible: fl.hosted; x: fl.d(258); y: fl.ly(116); hue: root.hueFlight
                   label: qsTr("TOTAL");       metricKey: "lm.totalDistance"; value: root.txt("lm.totalDistance"); unit: root.unit("lm.totalDistance") }
            Read { visible: fl.hosted; x: fl.d(44);  y: fl.ly(144); hue: root.hueFlight
                   label: qsTr("OFFLINE");     metricKey: "lm.offline"; value: root.txt("lm.offline"); unit: root.unit("lm.offline") }
        }
    }
}
