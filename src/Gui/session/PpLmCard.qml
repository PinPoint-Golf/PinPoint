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

// One launch monitor schematic and the readings that go with it: a title band, the
// drawing, and a strip of figures under it.
//
// THE CARD'S TYPE DOES NOT SCALE. Every size here is a Theme token, so a card in a narrow
// split and the same card on a bay TV set their labels at the same pixel size as every
// other panel in the app. This is a REVERSAL: the whole graphics view used to multiply one
// fit factor into every coordinate AND every font, which meant the view had no fixed size
// of its own — it read as a different application at either end of the range, and looked
// wrong at both. What scales now is the drawing, and only the drawing.
//
// THE THREE BANDS. Title and strip are fixed; the diagram takes what is left. That is what
// makes "scale the diagram to the frame" true in the plain sense — a taller card is a
// bigger drawing and nothing else moves.
//
// THE READINGS LEFT THE DRAWING, and had to. They used to sit at design coordinates over
// the schematic, which works only while the type shrinks with the geometry it is threaded
// between; with fixed type, a card at half size puts full-size labels over a half-size
// drawing and they collide. In the strip they are laid out rather than positioned, so
// there is no size at which they can overlap the picture or each other. What is lost is
// the label sitting beside the line it names — paid for by the labels staying legible,
// which is the trade the drawing's own reference marks (TARGET LINE, TOE/HEEL) do not have
// to make because they are short, few, and part of the picture.
//
// THE CAPTION MUST NOT WRAP. It states the card's exaggeration ("attack ×4") and a second
// line would eat the drawing's height. It elides instead.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root
    objectName: "lmCard"

    // The band this card is named for — the TITLE's hue only. Readings in the strip are
    // coloured by their own metric's band, which is what lets one colour meaning carry
    // between the tiles board and these schematics.
    property color hue: Theme.colorText
    property string title: ""
    property string caption: ""
    // The schematic. Instantiated by a Loader so only the active layout builds one.
    property Component diagram: null

    // The card's readings, ALREADY RESOLVED: a list of
    // { label, value, unit, hue, metricKey, note }. Resolved by the body, which owns the
    // graphics map, rather than looked up here — it keeps this file free of any knowledge
    // of what a reading is, which is what lets the layout test drive it with a literal.
    property var reads: []

    // An inferred read — { label, name, evidence } — or null. Rendered as its own line
    // under the strip, because the EVIDENCE is a sentence and a sentence does not belong
    // in a row of figures.
    property var inferred: null

    // A reading was pointed at (or left). The body turns this into `hoveredKey`, which
    // lights that metric's ±1 SD region inside the drawing. The strip is the hover target
    // rather than the line itself: a real-sized block of text can be pointed at, where a
    // rotated 1 px rule cannot.
    signal readHovered(string key, bool on)

    // The brief's card padding, now in Theme units rather than design pixels.
    readonly property int padTop: Theme.sp(9)
    readonly property int padX: Theme.sp(12)
    readonly property int padBottom: Theme.sp(10)
    // Sized for a fontSzHeading title rather than the micro label this used to be. The
    // five pixels come out of the drawing, which is the right place for them to come from:
    // a schematic reads fine a little shorter, a heading two sizes too small does not.
    readonly property int titleH: Theme.sp(24)
    // Below this the drawing stops being a drawing. The card never shrinks past it: the
    // composition reflows to a column first, and in the column the card is full width.
    readonly property int minDiagramH: Theme.sp(96)

    // What the drawing should be tall, when the CALLER knows better than "whatever is
    // left": the reflowed column sizes each card from its own diagram's aspect, so a
    // schematic in the column keeps its shape instead of being squashed into a fixed band.
    // 0 means fill, which is the wide composition's case.
    property int diagramH: 0
    readonly property int effectiveDiagramH: Math.max(minDiagramH, diagramH)

    implicitHeight: padTop + titleH + effectiveDiagramH + strip.implicitHeight + padBottom

    // ── where the readings go ────────────────────────────────────────────────
    // The schematic's own design box, so the card can work out whether the drawing is big
    // enough to carry its own labels.
    property real dw: 0
    property real dh: 0

    // WHETHER THE DRAWING HOSTS ITS OWN READINGS. The brief anchors every label at a design
    // pixel beside the line it names — "attack angle at (14, 18)" — and those anchors were
    // laid out against type at its natural size in a design-size box. So they fit exactly
    // when the drawing is AT LEAST design size, and only then. That is the test, stated
    // literally: at or above 1.0 the labels sit on the drawing where they were designed to;
    // below it they would crowd the geometry and each other, and they move to the strip.
    //
    // It answers to the DRAWING, not to the layout. A full-width card in the reflowed
    // column has a bigger schematic than a card in the composition does, so it hosts its
    // labels too — "limited space" means the drawing is small, which is the thing that
    // actually decides whether a label fits, rather than which layout happens to be on.
    //
    // MEASURED WITHOUT THE STRIP, or this would not resolve: the strip's height depends on
    // whether there are readings in it, which is what this decides. `diagramH` when the
    // caller set one, the externally-given height otherwise — neither carries the strip.
    // "Essentially design size", not "exactly". At the brief's own 1148 × 516 the drawing
    // lands at 0.978 of its design box — the title band is five pixels taller than it was
    // when those anchors were drawn, and the anchors carry more than 2% of slack. Demanding
    // a literal 1.0 would push the canonical size into the fallback, which would be the
    // rule defeating its own purpose.
    readonly property real hostFloor: 0.95
    readonly property real hostDiagramH: diagramH > 0
        ? diagramH
        : height - padTop - titleH - Theme.sp(6) - padBottom
    readonly property real hostScale: (dw > 0 && dh > 0 && width > 0 && hostDiagramH > 0)
        ? Math.min((width - 2 * padX) / dw, hostDiagramH / dh) : 0
    readonly property bool hosted: hostScale >= hostFloor

    Rectangle {
        anchors.fill: parent
        radius: Theme.radius
        color: Theme.colorSurface
        border.width: 1
        border.color: Theme.colorBorderMid
    }

    Row {
        id: titleRow
        anchors {
            top: parent.top; topMargin: root.padTop
            left: parent.left; leftMargin: root.padX
            right: parent.right; rightMargin: root.padX
        }
        height: root.titleH
        spacing: Theme.sp(8)

        // THE APP'S SECTION HEADING, exactly — the four properties ModelTrail.qml:61-64
        // sets on the Diagnostic Model's "Characteristics", which is the heading every
        // other content surface in PinPoint is titled with.
        //
        // A HEADING, NOT AN EYEBROW, and that was the whole miss. These titles used to be
        // fontSzMicro: ten pixels, uppercased and tracked, in the band's hue. Against a
        // sixteen-pixel sentence-case heading on the next screen over it did not read as
        // the same application — the launch monitor sat entirely on the two smallest tokens
        // in the scale while everything else had a type hierarchy. Size was the difference
        // a reader saw; the colour was the smaller half of it.
        //
        // colorText, and no band hue. The hue is not lost — it is on the readings in the
        // strip below and on the lines in the drawing above, which is where it does work.
        // A title is chrome, and chrome does not carry data.
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: root.title
            font.family: Theme.fontBody
            font.pixelSize: Theme.fontSzHeading
            font.weight: Theme.fontBodyWeight
            color: Theme.colorText
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            // Chrome, so colorText3 — the one place on this panel that token is correct.
            // Every NUMBER and every evidence line is colorText2.
            width: Math.max(0, parent.width - x)
            text: root.caption
            font.family: Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color: Theme.colorText3
            elide: Text.ElideRight
            maximumLineCount: 1
        }
    }

    // The drawing gets everything between the two fixed bands. When it is hosting its own
    // readings the strip is empty, so "everything" is very nearly the whole card.
    Loader {
        id: diagramSlot
        objectName: "diagramSlot"
        anchors {
            top: titleRow.bottom
            left: parent.left; leftMargin: root.padX
            right: parent.right; rightMargin: root.padX
            bottom: strip.top; bottomMargin: Theme.sp(6)
        }
        sourceComponent: root.diagram
    }

    // Told, rather than left to find out. Every schematic is a PpLmDiagram and every
    // PpLmDiagram carries `hosted`; a Binding rather than a property assignment because the
    // Loader's item does not exist until the component is loaded.
    Binding {
        target: diagramSlot.item
        property: "hosted"
        value: root.hosted
        when: diagramSlot.item !== null
        restoreMode: Binding.RestoreNone
    }

    // ── the readings strip ───────────────────────────────────────────────────
    // A Flow, so a narrow card runs its figures onto a second line rather than eliding
    // them or scrolling sideways. The card's implicitHeight follows, which is what lets
    // the reflowed column give a wrapped strip the room it asked for.
    Column {
        id: strip
        anchors {
            left: parent.left; leftMargin: root.padX
            right: parent.right; rightMargin: root.padX
            bottom: parent.bottom; bottomMargin: root.padBottom
        }
        spacing: Theme.sp(6)

        Flow {
            width: parent.width
            spacing: Theme.sp(14)
            // Not merely empty: an empty Flow still contributes the Column's spacing, which
            // would steal six pixels from a drawing that is hosting its own labels.
            visible: root.reads && root.reads.length > 0

            Repeater {
                model: root.reads
                PpLmRead {
                    required property var modelData
                    onHovered: (key, on) => root.readHovered(key, on)
                    label: modelData.label !== undefined ? modelData.label : ""
                    value: modelData.value !== undefined ? modelData.value : "—"
                    unit: modelData.unit !== undefined ? modelData.unit : ""
                    hue: modelData.hue !== undefined ? modelData.hue : Theme.colorText2
                    metricKey: modelData.metricKey !== undefined ? modelData.metricKey : ""
                    note: modelData.note !== undefined ? modelData.note : ""
                }
            }
        }

        // The inferred read: a name this panel put on the shot, and the evidence for it.
        Item {
            objectName: "anno"
            width: parent.width
            height: visible ? inferredRow.implicitHeight : 0
            visible: root.inferred !== null && root.inferred !== undefined
                     && root.inferred.has === true

            RowLayout {
                id: inferredRow
                width: parent.width
                spacing: Theme.sp(6)

                Text {
                    text: root.inferred && root.inferred.label ? root.inferred.label : ""
                    font.family: Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color: Theme.colorText3
                }
                Text {
                    text: root.inferred && root.inferred.name ? root.inferred.name : ""
                    font.family: Theme.fontData
                    font.pixelSize: Theme.fontSzData
                    color: root.hue
                }
                Text {
                    // THE AUDIT TRAIL. Not decoration, and never demoted to colorText3 or
                    // below the micro floor — it is the only thing on the card that shows
                    // the reasoning behind an inferred word.
                    Layout.fillWidth: true
                    text: root.inferred && root.inferred.evidence ? root.inferred.evidence : ""
                    font.family: Theme.fontBody
                    font.pixelSize: Theme.fontSzMicro
                    color: Theme.colorText2
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }
        }
    }

}
