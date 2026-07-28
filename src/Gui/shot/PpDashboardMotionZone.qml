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

// Motion zone — a grid of PpBandRail tiles, one per time-series metric. The rail is
// the primary read (see PpBandRail): checkpoints in position order against their
// normative corridor, which answers "was it in bounds, and where did it leave" —
// the question a post-shot glance actually has. A one-sided corridor is drawn as a
// zone running off the end of the plot rather than as a band with an edge it does not
// hold — and WHICH corridors those are comes from the measure's shape, resolved in
// C++, never from the unit string. A metric with no corridor yet degrades to the
// rail's phase-anchored sparkline; nothing here ever renders as a bare number.
//
// The metric set is SWING-AGNOSTIC: the zone shows its CONFIGURED metrics (the pinned
// list, else every live time-series metric) and joins each to the current swing. A
// configured metric this swing has no curve for renders as a GHOST rail carrying "NA"
// (live, not measured here) or "soon" (planned, no producer yet) — it is NOT dropped.
// A fixed template is what keeps the layout stable across swings, which is what makes
// the wall readable; and the ghosts double as a visible ledger of remaining pipeline
// work. The zone therefore does not collapse.

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

    // Interaction (window only). playheadUs is -1 on the wall cast, so every rail's
    // resting render there is identical to the in-app panel's at rest.
    property bool interactive: false
    property real playheadUs: -1

    signal metricActivated(string key)
    signal seekRequested(real tUs)

    readonly property int _phaseImpact: 5    // Phase::Impact

    readonly property var _tiles: _build()
    readonly property bool _hasContent: _tiles.length > 0
    implicitHeight: _hasContent ? card.implicitHeight : 0
    visible: _hasContent

    function _seriesFor(key) {
        var s = detail && detail.series ? detail.series : []
        for (var i = 0; i < s.length; ++i) if (s[i].key === key) return s[i]
        return null
    }

    // The zone's CONFIGURED metric set, each joined to the current swing. Queried
    // WITHOUT availableOnly — the config is a swing-agnostic template, so a metric
    // stays on the grid whether or not this particular rig could measure it.
    function _build() {
        var out = []
        if (!catalog) return out
        var cat = catalog.query({ type: "timeSeries" }, {})
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
            if (!row) continue                                   // not in the catalogue at all
            var s = _seriesFor(keys[k])
            var has = s && s.t_us && s.t_us.length > 0
            var desc = catalog.descriptor(keys[k], shotCtx)
            out.push({ key: keys[k],
                       label: (row.shortLabel && row.shortLabel.length) ? row.shortLabel : row.label,
                       unit: has ? (s.unit || row.unit) : row.unit,
                       has: has,
                       // "soon" = planned (no producer yet); "NA" = live metric this
                       // swing simply didn't measure. Distinct on purpose.
                       emptyLabel: row.planned === true ? qsTr("soon") : qsTr("NA"),
                       series: has ? s : null,
                       corridors: (desc && desc.normative && desc.normative.corridors)
                                  ? desc.normative.corridors : [],
                       // From the MEASURE's shape, resolved in C++. There was a
                       // _isOneSided() here that string-matched the unit against
                       // m/s, mph, °/s and deg/s — a presentation-layer heuristic
                       // standing in for a semantic property, which got the answer
                       // right only by coincidence and silently missed any metric
                       // whose unit was spelled differently. Shape is decided once,
                       // on the measure, and every surface reads it from there.
                       oneSided: (desc && desc.normative
                                  && desc.normative.oneSided === true) })
        }
        return out
    }

    // Rails per row. A rail needs real width to separate its checkpoints — at a
    // glance from across the room, two generous rails beat four cramped ones, so the
    // thresholds are deliberately high and a narrow panel goes single-column rather
    // than shrinking the tile.
    readonly property real _gap: Theme.sp(8)
    readonly property real _avail: Math.max(0, zone.width - Theme.sp(22))
    readonly property int  _cols: _avail >= Theme.sp(1180) ? 3
                                : _avail >= Theme.sp(700)  ? 2 : 1
    readonly property real _tileW: (_avail - _gap * (_cols - 1)) / _cols

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
            spacing: Theme.sp(8)

            Text {
                text: qsTr("MOTION")
                font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                font.capitalization: Font.AllUppercase; font.letterSpacing: Theme.trackingMicro
                color: Theme.colorText3
            }

            Flow {
                Layout.fillWidth: true
                spacing: zone._gap
                Repeater {
                    model: zone._tiles
                    delegate: RailTile { required property var modelData; data_: modelData }
                }
            }
        }
    }

    component RailTile: Rectangle {
        property var data_: ({})

        width: zone._tileW
        implicitHeight: rail.implicitHeight + Theme.sp(18)
        radius: Theme.radius
        color: tileHover.hovered && zone.interactive ? Theme.colorBg2 : Theme.colorBg
        border.width: Theme.borderWidth
        border.color: tileHover.hovered && zone.interactive ? Theme.colorBorderMid : Theme.colorBorder
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

        PpBandRail {
            id: rail
            anchors { left: parent.left; right: parent.right; top: parent.top
                      leftMargin: Theme.sp(11); rightMargin: Theme.sp(11); topMargin: Theme.sp(9) }

            label: data_.label || ""
            unit:  data_.unit || ""
            phaseValues: data_.series && data_.series.phaseSamples ? data_.series.phaseSamples : []
            corridors:   data_.corridors || []
            curveT:      data_.series ? data_.series.t_us : []
            curveV:      data_.series ? data_.series.value : []
            impactPhase: zone._phaseImpact
            oneSided:    data_.oneSided === true
            emptyLabel:  data_.has ? "" : (data_.emptyLabel || qsTr("NA"))

            interactive: zone.interactive
            playheadUs:  zone.playheadUs

            onSeekRequested:  (tUs) => zone.seekRequested(tUs)
            onScrubRequested: (tUs) => zone.seekRequested(tUs)
            // Clicking the HEADING opens the catalogue page (same as the Setup tiles);
            // the rail body stays reserved for seek/scrub. The rail draws its own
            // hover affordance (label brightens + a chevron). There is deliberately NO
            // hover tooltip: a Controls ToolTip is unthemed chrome, unbounded in width,
            // and set in a size nobody can read from across the room. MetricDetail is
            // the readable home for a metric's narrative, and one click away.
            onLabelActivated: zone.metricActivated(data_.key)
        }

        // Tile-level hover only drives the card's background/border tint; the rail
        // owns the heading click-through and its cursor/affordance.
        HoverHandler { id: tileHover; enabled: zone.interactive }
    }
}
