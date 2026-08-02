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
import QtTest
import PinPointStudio

// The chart's METRICS preset combo — the control that made the panel readable again once a full
// capture started producing thirty-odd curves in six units.
//
// The grouping itself is pinned in chart_metrics_test (C++, against the real manifest). What is
// tested HERE is the part that only exists once the component is running: that a preset actually
// reaches the plot, that it survives a new swing, and that hand-toggling a chip stops the combo
// claiming to show a group it no longer shows.
Item {
    id: probe
    width: 900; height: 700

    // Two groups' worth of curves plus a setup scalar. Keys are real catalogue keys — the preset
    // list is the catalogue's own grouping, so invented ones would all fall into "Other".
    function curve(key, unit) {
        var t = [], v = []
        for (var i = 0; i < 8; ++i) { t.push(i * 10000); v.push(i) }
        return { key: key, label: key, unit: unit, t_us: t, value: v, phaseSamples: [] }
    }
    readonly property var wristAndSpeed: [
        curve("leadWristFlexExt", "°"), curve("leadWristRadUln", "°"),
        curve("clubheadSpeed", "mph"),  curve("handSpeed", "mph"),
        // Empty curve, one phaseSample — the shape foot_metrics/tempo_metrics emit.
        { key: "tempoRatio", label: "Tempo ratio", unit: ":1", t_us: [], value: [],
          phaseSamples: [{ phase: 5, t_us: 0, value: 3.0, band: "" }] }
    ]
    // A different swing: the wrist group is gone entirely, a new one appears.
    readonly property var rotationOnly: [
        curve("pelvisRotation", "°"), curve("thoraxRotation", "°"), curve("xFactor", "°")
    ]
    // …and one that drops the wrist group but keeps Club & speed.
    readonly property var rotationAndSpeed: [
        curve("pelvisRotation", "°"), curve("thoraxRotation", "°"), curve("xFactor", "°"),
        curve("clubheadSpeed", "mph"), curve("handSpeed", "mph")
    ]

    PpMetricChart {
        id: chart
        anchors.fill: parent
        sessionType: 1                      // a real screen, so prefs persist and presets apply
        seriesList: probe.wristAndSpeed
        phases: []
        startUs: 0; endUs: 70000; impactUs: 40000
    }

    function keysOf(list) {
        var out = []
        for (var i = 0; i < list.length; ++i) out.push(list[i].key)
        return out.sort().join(",")
    }

    TestCase {
        name: "ChartPresets"
        when: windowShown

        function test_001_defaults_to_first_group_not_everything() {
            // The state being fixed was "every curve at once", so an unset preset must NOT
            // resolve to All.
            compare(chart.preset, "Wrist & forearm")
            compare(probe.keysOf(chart._visible), "leadWristFlexExt,leadWristRadUln")
        }

        function test_002_scalars_are_not_offered() {
            // tempoRatio has no curve: not plottable, not in the legend, not in any group.
            compare(probe.keysOf(chart._plottable),
                    "clubheadSpeed,handSpeed,leadWristFlexExt,leadWristRadUln")
            var names = []
            for (var i = 0; i < chart._groups.length; ++i) names.push(chart._groups[i].group)
            compare(names.join(","), "Wrist & forearm,Club & speed")
        }

        function test_003_the_legend_carries_the_preset_not_the_swing() {
            // The whole point of the combo. A legend listing every plottable curve would put the
            // wall of metrics back on screen one row below the facets.
            chart._applyPreset("Club & speed", true)
            compare(probe.keysOf(chart._visible), "clubheadSpeed,handSpeed")
            compare(probe.keysOf(chart._legendSeries), "clubheadSpeed,handSpeed")
            // The swing still HAS four plottable curves — the legend is narrowed, not the data.
            compare(chart._plottable.length, 4)
        }

        function test_004_all_shows_every_plottable_curve() {
            chart._applyPreset("All", true)
            compare(probe.keysOf(chart._visible),
                    "clubheadSpeed,handSpeed,leadWristFlexExt,leadWristRadUln")
            // "All" is how a reader picks freely across groups, so the legend opens right up.
            compare(probe.keysOf(chart._legendSeries),
                    "clubheadSpeed,handSpeed,leadWristFlexExt,leadWristRadUln")
        }

        function test_005_hand_toggling_a_chip_drops_to_custom() {
            chart._applyPreset("Wrist & forearm", true)
            compare(chart.preset, "Wrist & forearm")
            chart._toggle("leadWristRadUln")
            // The selection has left the group, so the combo must stop naming it.
            compare(chart.preset, "Custom")
            compare(probe.keysOf(chart._visible), "leadWristFlexExt")
            // …and "Custom" is offered as an option only once it is real.
            verify(chart._presetOptions.indexOf("Custom") >= 0)
        }

        function test_006_a_switched_off_chip_stays_in_the_legend() {
            // Following on from Custom above: the legend lists the GROUP, not what is visible, so
            // the chip just switched off is still there (dimmed) to switch back on. A legend you
            // can only subtract from is a one-way door.
            compare(probe.keysOf(chart._legendSeries), "leadWristFlexExt,leadWristRadUln")
            chart._toggle("leadWristRadUln")
            compare(probe.keysOf(chart._visible), "leadWristFlexExt,leadWristRadUln")
        }

        function test_007_custom_survives_a_new_swing_that_kept_its_group() {
            chart.seriesList = probe.wristAndSpeed
            chart._applyPreset("Club & speed", true)
            chart._toggle("handSpeed")
            compare(chart.preset, "Custom")
            // The new swing has no wrist data but still has Club & speed, so the hand-picked
            // selection is still meaningful — it is the user's own and must not be replaced.
            chart.seriesList = probe.rotationAndSpeed
            compare(chart.preset, "Custom")
            compare(probe.keysOf(chart._visible), "clubheadSpeed")
            compare(probe.keysOf(chart._legendSeries), "clubheadSpeed,handSpeed")
        }

        function test_008_custom_falls_back_when_its_group_is_gone() {
            chart.seriesList = probe.wristAndSpeed
            chart._applyPreset("Wrist & forearm", true)
            chart._toggle("leadWristRadUln")
            compare(chart.preset, "Custom")
            // This swing measured no wrist at all, so Custom would leave an empty plot under an
            // empty legend. A selection of nothing is not worth preserving.
            chart.seriesList = probe.rotationOnly
            compare(chart.preset, "Body rotation")
            compare(probe.keysOf(chart._visible), "pelvisRotation,thoraxRotation,xFactor")
        }

        function test_009_a_preset_the_new_swing_lacks_falls_back() {
            // Rather than an empty plot under a label naming a group that was never measured,
            // fall to the first group the swing DOES have.
            chart.seriesList = probe.wristAndSpeed
            chart._applyPreset("Wrist & forearm", true)
            chart.seriesList = probe.rotationOnly
            compare(chart.preset, "Body rotation")
            compare(probe.keysOf(chart._visible), "pelvisRotation,thoraxRotation,xFactor")
        }

        function test_010_a_preset_the_new_swing_still_has_is_kept() {
            chart.seriesList = probe.wristAndSpeed
            chart._applyPreset("Club & speed", true)
            chart.seriesList = probe.rotationOnly.concat([probe.curve("lagAngle", "°")])
            compare(chart.preset, "Club & speed")
            compare(probe.keysOf(chart._visible), "lagAngle")
            compare(probe.keysOf(chart._legendSeries), "lagAngle")
        }
    }
}
