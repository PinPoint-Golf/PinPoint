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

// Setup zone — the address-position checklist as a TILE GRID, one visual primitive
// per metric. Scalars (stance width, foot flare, low point) render as a PpRangeBar
// mini-bar: a marker sitting in its green/amber corridor, which answers "in bounds?"
// at a glance where a number only answers "what". Alignment angles render as a
// PpOrientationGlyph — the coaching read there is a WORD (square / open / closed),
// and the glyph draws the geometry rather than printing the word alone.
//
// The metric set is SWING-AGNOSTIC: the zone shows its CONFIGURED metrics (the pinned
// list, else every live point-in-time metric) and reads each at Address. A configured
// metric this swing has no sample for keeps its tile, rendered in the primitive's
// neutral no-value state — an empty corridor track, or the glyph's unknown form —
// labelled "NA" (live, not measured here) or "soon" (planned, no producer yet). A
// fixed template is what keeps the layout stable across swings, and the placeholders
// double as a visible ledger of remaining pipeline work.

import QtQuick
import QtQuick.Layouts
import PinPointStudio

Item {
    id: zone

    property var catalog: null
    property var shotCtx: ({})
    property var detail: ({})
    property int sessionType: -1
    property var pinnedMetrics: []
    property bool interactive: false

    signal metricActivated(string key)

    ChartMetrics { id: cm }

    readonly property int _phaseAddress: 0   // Phase::Address

    readonly property var _rows: _build()
    readonly property bool _hasContent: _rows.length > 0
    implicitHeight: _hasContent ? card.implicitHeight : 0
    visible: _hasContent

    function _seriesFor(key) {
        var s = detail && detail.series ? detail.series : []
        for (var i = 0; i < s.length; ++i) if (s[i].key === key) return s[i]
        return null
    }
    function _sampleAt(series, phase) {
        var ps = series && series.phaseSamples ? series.phaseSamples : []
        for (var i = 0; i < ps.length; ++i) if (ps[i].phase === phase) return ps[i]
        return null
    }
    function _corridorAt(desc, phase) {
        var cs = (desc && desc.normative && desc.normative.corridors)
                 ? desc.normative.corridors : []
        for (var i = 0; i < cs.length; ++i) if (cs[i].phase === phase) return cs[i]
        return null
    }
    function _bandColor(b) {
        return b === "green"  ? Theme.colorRagGood
             : b === "yellow" ? Theme.colorRagWatch
             : b === "red"    ? Theme.colorRagFault
             :                  Theme.colorRagNone
    }
    function _isRag(b) { return b === "green" || b === "yellow" || b === "red" }
    // The headline colour. A real RAG band drives it vibrant; a metric with no
    // corridor (nothing to score against — e.g. stance width, ball position) falls
    // back to bright PRIMARY text, never the muted grey a bare _bandColor("") gives.
    // This is exactly what the Motion tile's headline does (PpBandRail _headColor).
    function _valueColor(band, has) {
        if (has !== true) return Theme.colorText3
        return _isRag(band) ? _bandColor(band) : Theme.colorText
    }

    // The zone's CONFIGURED tiles, each joined to the current swing's Address sample.
    // Queried WITHOUT availableOnly — the config is a swing-agnostic template, so a
    // metric holds its tile whether or not this rig could measure it.
    function _build() {
        var out = []
        if (!catalog) return out
        var cat = catalog.query({ type: "pointInTime" }, {})
        var byKey = {}
        for (var i = 0; i < cat.length; ++i) byKey[cat[i].key] = cat[i]

        var pins = pinnedMetrics || []
        var keys = []
        if (pins.length > 0) {
            keys = pins
        } else {
            for (var j = 0; j < cat.length; ++j) if (!cat[j].planned) keys.push(cat[j].key)
        }

        for (var k = 0; k < keys.length; ++k) {
            var row = byKey[keys[k]]
            if (!row) continue                                  // not in the catalogue at all
            var s = _seriesFor(keys[k])
            var samp = s ? _sampleAt(s, _phaseAddress) : null

            var desc = catalog.descriptor(keys[k], shotCtx)
            var cor  = _corridorAt(desc, _phaseAddress)
            // Which tail does not grade, from the MEASURE's shape (A4a threaded it onto
            // every corridor map). Read once here and handed to the bar; nothing on this
            // screen re-derives it.
            var loOpen = cor ? cor.lowOpen  === true : false
            var hiOpen = cor ? cor.highOpen === true : false
            // Alignment angles read as an orientation word; everything else as a bar.
            // The glyph carries its own unknown state, so it still serves an NA tile.
            //
            // A ONE-SIDED corridor falls back to the bar. Every alignment measure is
            // `target` today and orientationLabel() is two-sided by construction — its
            // whole vocabulary is open / square / closed around a corridor with two real
            // edges. On a floor the high edge is mu, so a good reading above it would be
            // labelled "open", i.e. a fault word for the best possible answer. This is the
            // same fall-back-to-a-bar the degenerate-corridor case already takes, and it
            // makes the audit enforceable rather than a comment: change an alignment
            // measure's shape and the tile degrades visibly instead of inverting silently.
            var glyph = (row.group === "Alignment" || keys[k] === "toeLineAngle")
                        && !loOpen && !hiOpen
            out.push({ key: keys[k],
                       label: (row.shortLabel && row.shortLabel.length) ? row.shortLabel : row.label,
                       unit: row.unit,
                       has: samp !== null,
                       // "soon" = planned (no producer yet); "NA" = live metric this
                       // swing simply didn't measure. Distinct on purpose.
                       emptyLabel: row.planned === true ? qsTr("soon") : qsTr("NA"),
                       value: samp ? samp.value : 0,
                       band: samp ? samp.band : "",
                       glyph: glyph,
                       orientation: (glyph && samp && cor)
                                    ? cm.orientationLabel(samp.value, cor.greenLo, cor.greenHi) : "",
                       greenLo: cor ? cor.greenLo : 0, greenHi: cor ? cor.greenHi : 0,
                       amberLo: cor ? cor.amberLo : 0, amberHi: cor ? cor.amberHi : 0,
                       lowOpen: loOpen, highOpen: hiOpen,
                       hasCorridor: cor !== null })
        }
        return out
    }

    Rectangle {
        id: card
        width: zone.width
        implicitHeight: col.implicitHeight + Theme.sp(22)
        color: Theme.colorSurface
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.colorBorder

        ColumnLayout {
            id: col
            anchors { left: parent.left; right: parent.right; top: parent.top
                      leftMargin: Theme.sp(11); rightMargin: Theme.sp(11); topMargin: Theme.sp(9) }
            spacing: Theme.sp(9)

            Text {
                text: qsTr("SETUP · ADDRESS")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }

            Flow {
                Layout.fillWidth: true
                spacing: Theme.sp(10)
                Repeater {
                    model: zone._rows
                    delegate: SetupTile { required property var modelData; data_: modelData }
                }
            }
        }
    }

    // One metric = one tile: a coloured left edge, the heading, then the primitive.
    component SetupTile: Rectangle {
        property var data_: ({})

        // Sized for a glance, not a spreadsheet: wide enough that the big headline
        // value breathes, the corridor bar can place its marker meaningfully, and the
        // label doesn't elide. Matches the Motion tile's read-at-distance intent.
        width: Theme.sp(232)
        implicitHeight: tCol.implicitHeight + Theme.sp(20)
        radius: Theme.radius
        opacity: data_.has === true ? 1.0 : 0.6
        color: tileHover.hovered && zone.interactive ? Theme.colorBg2 : Theme.colorBg
        border.width: Theme.borderWidth
        border.color: tileHover.hovered && zone.interactive ? Theme.colorBorderMid : Theme.colorBorder
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        // Coloured left edge — the splash of colour that replaced the (useless) RAG
        // dot, and the tile's verdict at a glance: the value's RAG band, or the accent
        // when the metric carries no corridor to score against.
        Rectangle {
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
            width: Theme.sp(3)
            radius: Theme.radius
            color: zone._isRag(data_.band) ? zone._bandColor(data_.band) : Theme.colorAccent
            opacity: data_.has === true ? 1.0 : 0.5
        }

        ColumnLayout {
            id: tCol
            anchors { left: parent.left; right: parent.right; top: parent.top
                      leftMargin: Theme.sp(12); rightMargin: Theme.sp(10); topMargin: Theme.sp(8) }
            spacing: Theme.sp(5)

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.sp(4)
                Text {
                    Layout.fillWidth: true
                    text: (data_.label || "").toUpperCase()
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzLabel
                    font.letterSpacing: Theme.trackingMicro
                    // Brightens on hover — part of the "this heading is clickable" cue.
                    color: tileHover.hovered && zone.interactive ? Theme.colorText : Theme.colorText3
                    elide: Text.ElideRight
                }
                // Hover affordance: a chevron marking that the heading opens the
                // metric's catalogue page. Mirrors the Motion tile. Kept permanently in
                // the layout and toggled by OPACITY (not visible) so its taller glyph
                // always reserves its row space — otherwise the header, and the whole
                // tile, would grow on hover (a jarring geometry jump).
                Text {
                    opacity: tileHover.hovered && zone.interactive ? 1 : 0
                    text: "›"
                    font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody
                    color: Theme.colorText2
                    Behavior on opacity { NumberAnimation { duration: Theme.durationFast } }
                }
            }

            // Categorical alignment → glyph; scalar → corridor mini-bar with its value.
            PpOrientationGlyph {
                visible: data_.glyph === true
                Layout.alignment: Qt.AlignHCenter
                orientation: data_.orientation || ""
                band: data_.band || ""
            }

            ColumnLayout {
                visible: data_.glyph !== true
                Layout.fillWidth: true
                spacing: Theme.sp(3)
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.sp(3)
                    Text {
                        // Value, else "NA" (live metric, no value this swing) or
                        // "soon" (planned — no producer yet).
                        text: {
                            if (data_.has !== true) return data_.emptyLabel || qsTr("NA")
                            var v = data_.value
                            var a = Math.abs(v)
                            return a >= 100 ? String(Math.round(v)) : String(Math.round(v * 10) / 10)
                        }
                        // Motion-tile headline scale: a big number is the whole point
                        // of a wall-readable stat tile. Coloured vibrant by RAG band,
                        // or bright primary text when the metric has no corridor.
                        font.family: Theme.fontData; font.pixelSize: Theme.sp(25)
                        font.weight: Font.DemiBold
                        font.italic: data_.has !== true && data_.emptyLabel === qsTr("soon")
                        color: zone._valueColor(data_.band, data_.has)
                    }
                    Text {
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: Theme.sp(4)
                        visible: data_.has === true
                        text: data_.unit || ""
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        color: Theme.colorText3
                    }
                    Item { Layout.fillWidth: true }
                }
                // A corridor bar only when there IS a corridor: with none, the bar's
                // domain is value±1, so the marker always lands dead-centre — a bogus
                // "perfectly mid-range" read. There the big headline number stands
                // alone, which is what makes the tile read as a confident stat.
                PpRangeBar {
                    visible: data_.hasCorridor === true
                    Layout.fillWidth: true
                    compact: true
                    hasValue: data_.has === true
                    value: data_.value
                    band: data_.band || ""
                    greenLo: data_.greenLo; greenHi: data_.greenHi
                    amberLo: data_.amberLo; amberHi: data_.amberHi
                    lowOpen: data_.lowOpen === true; highOpen: data_.highOpen === true
                }
            }
        }

        // Click → the metric's catalogue page. There is deliberately NO hover
        // tooltip: a Controls ToolTip is unthemed chrome, unbounded in width, and
        // set in a size nobody can read from across the room — the exact failure a
        // wall surface cannot afford. MetricDetail is the readable home for a
        // metric's narrative, and one click away.
        HoverHandler {
            id: tileHover
            enabled: zone.interactive
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            enabled: zone.interactive
            onTapped: zone.metricActivated(data_.key)
        }
    }
}
