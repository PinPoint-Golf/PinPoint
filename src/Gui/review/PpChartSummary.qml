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

// PpChartSummary — the per-window summary cards row: one card per series, each fed by
// ChartMetrics.summaryMasked(series.t_us, series.value, series.valid, winStart, winEnd), where
// the window is the active one CLAMPED to the metric's phase domain. A card shows @impact /
// peak / Δ-segment / peak-rate, with the @impact value tinted by the band of the swing state at
// impact. @impact is the value at the impact landmark (read from the whole series, a fixed
// reference more useful to compare against PEAK than the window edge); peak/Δ/rate stay
// window-scoped, so selecting TOP→IMP shows the downswing's numbers. Recomputes live as the
// segment chips / brush move the window. Pure binding; all stats come from ChartMetrics.
//
// Nothing here shows a number it cannot stand behind (design metric_presentation_honesty.md §5.1):
// a bridged sample is excluded from every reduction, a window the domain clamp emptied prints "—"
// in the three window-scoped tiles, @impact prints "—" where it was not measured at impact, and a
// window with any unmeasured part in it wears a PARTIAL chip.
//
// Since Phase 2 (§5.2) PEAK is a 40 ms windowed-mean extremum and PK RATE a ≥50 ms least-squares
// slope, both from src/Analysis/series_reduce.h — the same reducers the diagnostics engine grades
// with, so a card and a corridor cannot disagree about the same window. PK RATE therefore also has
// an ABSENT state (`rateOk` false: no window long enough, or too few valid samples in it), and it
// prints "—" with its unit hidden rather than a fitted-from-nothing 0. The same for PEAK and Δ on a
// series with no valid sample anywhere (`edgeOk` false), where every window number is a 0 read out
// of nothing rather than a flat curve.

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic   // ToolTip on the σ chip
import QtQuick.Layouts
import PinPointStudio

ColumnLayout {
    id: root

    // series: [{ key, label, unit, t_us, value, phaseSamples, color,
    //            sigma?, valid?, validFromUs?, validToUs? }]
    // `sigma` is OPTIONAL and its absence is meaningful — see the chip below. So are the last
    // three (design metric_presentation_honesty.md §5.1): `valid` is the per-sample validity mask
    // (0 = bridged across a gated or absent run), and validFromUs/validToUs are the metric's phase
    // domain resolved to instants by the host. Absent ⇒ nothing is masked and nothing is clamped,
    // which is every series that predates the field.
    property var    series:      []
    property real   startUs:     0
    property real   endUs:       0
    property real   impactUs:    -1       // impact instant; the @impact card reads the series here
    property string segmentName: ""
    property bool   showHeader:  true     // false when a host SectionHeader labels this

    spacing: Theme.sp(9)

    ChartMetrics   { id: cm }
    TimelineLabels { id: labels }         // value-at-time lookup (impact landmark)

    // ── THE CARD NAMES ITS UNIT ONCE, IN THE HEADER ──────────────────────────────
    //
    // Every value here used to carry the full unit, so a sway card said "% stance width" five
    // times — beside the name, and again on @impact, peak, Δ and the rate. In a 150px column with
    // a data face that is wider than the number it qualifies, and the grid overprinted itself:
    // "12 % stance wi34 %tance w". Two things were wrong and only one of them was the length.
    //
    // The other is placement. Four values in one card share one unit, so the unit is a property of
    // the CARD, not of each number — it belongs beside the name, where it already was, and nowhere
    // else. Values are bare. The same rule strips the unit from the split-mode @end readout, whose
    // gutter names it directly above (PpMetricChart's _fmt / _num note states it in full).
    //
    // ChartMetrics.formatBare keeps the same sign convention formatValue uses, so a reading does
    // not change shape between the card and the legend: degrees carry a leading "+" when positive,
    // nothing else does. _unit is the card's own token — short, and shown in exactly one place.
    function _unit(unit) {
        return cm.shortUnit((unit === undefined || unit === null || unit === "") ? "°" : unit)
    }
    // "" ⇒ NO VERDICT, tinted like any other unlabelled value. This used to fall through to
    // colorGood, and combined with bandAtNearest's old "good" default that meant a series with no
    // phaseSample anywhere near impact showed its @impact reading in PASS GREEN — a grade invented
    // from an empty list. A missing verdict is not a good one.
    function _bandColor(b) {
        return b === "warn"      ? Theme.colorWarn
             : b === "attention" ? Theme.colorAttention
             : b === "good"      ? Theme.colorGood
             :                     Theme.colorText
    }
    // The measured-at-an-instant test, in JS for the reason chart_metrics.h gives: cm.measuredAt
    // marshals the whole series per call, and these bindings re-evaluate as the window moves.
    // Same rule, same short-mask discipline (a mask that does not cover the curve is discarded).
    function _measuredAt(s, t) {
        if (s.validFromUs !== undefined && s.validToUs !== undefined
            && s.validToUs > s.validFromUs && (t < s.validFromUs || t > s.validToUs))
            return false
        var tt = s.t_us
        if (!tt || tt.length === 0) return true
        if (!s.valid || s.valid.length < tt.length) return true
        var best = -1, bd = Infinity
        for (var i = 0; i < tt.length; ++i) {
            var d = Math.abs(tt[i] - t)
            if (d < bd) { bd = d; best = i }
        }
        return best < 0 || s.valid[best] !== 0
    }

    // The card's window: the active window CLAMPED to this metric's phase domain, because the
    // reducers must search only where the geometry means something (design §5.1). A pelvis-sway
    // peak found after impact is a reading of the pelvis TURNING, not of it sliding, and it was
    // beating the real peak on the corpus.
    //
    // ORDERED: the end is floored at the start, so a window entirely past the domain collapses to
    // a point at the domain's end rather than inverting — summaryMasked swaps an inverted pair,
    // which would turn the clamp into a window over exactly the region it was removing.
    // ⚠ The same two lines live in PpMetricChart.qml (the split-mode @end readout); keep them equal.
    function _winStart(s) {
        return Math.max(root.startUs, s.validFromUs !== undefined ? s.validFromUs : root.startUs)
    }
    function _winEnd(s) {
        return Math.max(root._winStart(s),
                        Math.min(root.endUs, s.validToUs !== undefined ? s.validToUs : root.endUs))
    }

    // Header — "SUMMARY · <segment>" + a hairline rule.
    RowLayout {
        visible: root.showHeader
        Layout.fillWidth: true
        spacing: Theme.sp(9)
        Text {
            text: qsTr("SUMMARY") + (root.segmentName ? " · " + root.segmentName : "")
            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
            font.letterSpacing: Theme.trackingLabel
            color: Theme.colorText3
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.colorBorder }
    }

    // Cards — equal-width columns, wrapping to the available width.
    GridLayout {
        id: grid
        Layout.fillWidth: true
        columnSpacing: Theme.sp(10); rowSpacing: Theme.sp(10)
        columns: Math.max(1, Math.min(root.series.length,
                                      Math.floor(grid.width / Theme.sp(150))))

        Repeater {
            model: root.series
            delegate: Rectangle {
                id: card
                required property var modelData
                readonly property real   winStartUs: root._winStart(card.modelData)
                readonly property real   winEndUs:   root._winEnd(card.modelData)
                readonly property var    st:  cm.summaryMasked(card.modelData.t_us,
                                                         card.modelData.value,
                                                         card.modelData.valid || [],
                                                         card.winStartUs, card.winEndUs)
                // THE DOMAIN CLAMP EMPTIED THE WINDOW: the reader picked a span (IMP→P8 on a
                // P1–P7 metric, say) lying wholly outside where this metric means anything. The
                // three window-scoped tiles then print "—" rather than a delta of 0.0, a rate of
                // 0 and a peak — all four of which are reductions over a single instant and read
                // as measurements of a still, well-behaved curve. Guarded on a non-empty
                // selection so a chart that has not sized its window yet is not called empty.
                readonly property bool   collapsed: card.winEndUs <= card.winStartUs
                                                    && root.endUs > root.startUs
                // The card's numbers do not rest on a continuous measurement — summaryMasked says
                // so (invalid samples inside the window, or an edge read from across them), or the
                // window was emptied above and there is nothing behind any of them.
                readonly property bool   partial: card.st.partial === true || card.collapsed
                // ── IS THERE A PEAK RATE AT ALL? ────────────────────────────────────────────
                // Phase 2 (design §5.2) makes PK RATE the steepest least-squares slope over a
                // window of at least 50 ms holding at least 3 valid samples, and a window that
                // short — or that sparsely measured — simply has no slope to fit. summaryMasked
                // says so with `rateOk` and returns 0, and a 0 in this tile would read as the one
                // thing it must not: a still, well-behaved curve. So the tile prints "—" and the
                // "/100ms" unit token goes with it, because a unit beside an em dash still claims
                // a quantity was measured in it.
                //
                // `=== true` on purpose: an older analysisDetail summarised by a build without the
                // key gives `undefined`, and `!card.st.rateOk` would then print a rate that was
                // never fitted.
                readonly property bool   rateOk: card.st.rateOk === true && !card.collapsed
                // ── IS THERE ANY READING ON THIS SERIES AT ALL? ─────────────────────────────
                // `edgeOk` false means the series carries no valid sample anywhere — every sample
                // bridged, or an empty curve — so PEAK, Δ and every other window number came back
                // 0.0 out of nothing. A "PEAK 0 / Δ 0" card wearing only a PARTIAL chip reads as a
                // still, well-behaved curve, which is the same confident absurdity `rateOk` exists
                // to stop for the rate. Same treatment: "—".
                //
                // ⚠ `!== false`, where rateOk above is `=== true`, and the asymmetry is deliberate.
                // A map without `rateOk` costs one tile; a map without `edgeOk` would blank EVERY
                // tile on EVERY card, so the missing-key direction has to differ. (Neither can
                // happen with this build — chart_metrics_test pins both keys as present — the
                // guards are for a QML file that outlives a C++ change.)
                readonly property bool   valueOk: card.st.edgeOk !== false && !card.collapsed

                // Value at the impact landmark — a fixed anatomical reference, so it reads the
                // whole series (not the view window); the more useful thing to compare against
                // PEAK. Falls back to the window @end when no impact is known.
                //
                // ⚠ AND IT IS GATED ON HAVING BEEN MEASURED THERE. valueAtNearest snaps to the
                // nearest sample unconditionally, so a series whose geometry was gated across
                // impact — which is exactly what design §5.1 arranges — printed the bridged value
                // as its headline number, in the band colour, as the one figure on the card a
                // reader trusts most. `impMeasured` false ⇒ the tile prints "—" in a neutral
                // colour. It is NOT gated on the window: this tile is deliberately not
                // window-scoped, and an emptied window says nothing about the impact landmark.
                readonly property bool   impMeasured: root.impactUs > 0
                                                      && root._measuredAt(card.modelData, root.impactUs)
                readonly property real   impVal: root.impactUs > 0
                                                 ? labels.valueAtNearest(card.modelData.t_us,
                                                       card.modelData.value, root.impactUs)
                                                 : card.st.end
                // "" = NO VERDICT, and it must stay neutral rather than green: bandAtNearest now
                // refuses to answer when the nearest phaseSample is a frame or more away, or when
                // there are none, instead of defaulting to "good" off nothing at all.
                readonly property string bnd: cm.bandAtNearest(card.modelData.phaseSamples,
                                                  root.impactUs > 0 ? root.impactUs : root.endUs)
                readonly property string nm:  cm.shortLabel(card.modelData.key)
                                              || card.modelData.label || card.modelData.key

                Layout.fillWidth: true
                Layout.preferredWidth: 1            // equal columns
                implicitHeight: cardCol.implicitHeight + Theme.sp(22)
                radius: Theme.sp(10)
                color: Theme.colorBg
                border.width: 1; border.color: Theme.colorBorder
                clip: true

                Rectangle {                          // series colour edge
                    width: Theme.sp(3); height: parent.height
                    color: card.modelData.color
                }

                ColumnLayout {
                    id: cardCol
                    anchors { left: parent.left; right: parent.right; top: parent.top
                              leftMargin: Theme.sp(13); rightMargin: Theme.sp(11)
                              topMargin: Theme.sp(11) }
                    spacing: Theme.sp(10)

                    RowLayout {                       // name + unit
                        Layout.fillWidth: true
                        Text {
                            Layout.fillWidth: true
                            text: card.nm; elide: Text.ElideRight
                            font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody
                            color: Theme.colorText
                        }
                        Text {
                            text: root._unit(card.modelData.unit)
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                            font.letterSpacing: Theme.trackingData
                            color: Theme.colorText3
                        }
                        // Measurement NOISE on this series, when its producer characterised one.
                        // Sits by the unit rather than on each of the four values because it is
                        // one number for the whole curve — repeating it four times would read as
                        // four separate error bars. Absent ⇒ nothing is drawn: a series with no
                        // σ has not been characterised, which is not the same as being exact.
                        Text {
                            id: sigmaChip
                            // Resolved once, because `visible` does not gate a binding: QML
                            // evaluates `text` whether or not the item is shown, so a series
                            // with no σ reached .toFixed() on undefined and warned per frame.
                            readonly property real sigma:
                                (card.modelData.sigma !== undefined
                                 && card.modelData.sigma !== null) ? card.modelData.sigma : 0
                            visible: sigmaChip.sigma > 0
                            text: "± " + sigmaChip.sigma.toFixed(1)
                                       + root._unit(card.modelData.unit)
                            font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                            font.letterSpacing: Theme.trackingData
                            color: Theme.colorText3
                            HoverHandler { id: sigmaHover }
                            ToolTip.visible: sigmaHover.hovered
                            ToolTip.delay: 400
                            // Deliberately says what it is NOT. This is frame-to-frame noise in
                            // the detector, not the accuracy of the reading — the projection
                            // error on a camera-derived angle is larger and uncharacterised, and
                            // a reader who took ±σ for total accuracy would trust it too far.
                            ToolTip.text: qsTr("Frame-to-frame measurement noise on this curve. "
                                               + "Not the overall accuracy of the reading.")
                        }
                    }

                    // PARTIAL — part of this window was never measured. Styled exactly like the
                    // σ chip above (fontData, micro, colorText3) because it belongs to the same
                    // track: both qualify the numbers below without changing one of them. It is a
                    // caveat, not an error, so it is NOT warn-coloured — the values are the best
                    // the valid samples support, and that is what it says.
                    //
                    // ⚠ ITS OWN LINE, not the header row. That row is name + unit + ±σ, and only
                    // the NAME can give up width (it is the fillWidth item); adding a fourth token
                    // meant three fixed-width chips competing for what was left of a 150px card and
                    // overlapping each other on the narrow ones. A caveat that is illegible is
                    // worse than absent, since the reader still sees ink and mistrusts the number.
                    Text {
                        visible: card.partial
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: qsTr("PARTIAL")
                        font.family: Theme.fontData; font.pixelSize: Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingData
                        color: Theme.colorText3
                        HoverHandler { id: partialHover }
                        ToolTip.visible: partialHover.hovered
                        ToolTip.delay: 400
                        ToolTip.text: qsTr("Part of this window had no valid measurement.")
                    }

                    // ── THE 2×2 VALUE GRID, AND WHY EVERY CELL IS A LAYOUT ───────────────────
                    //
                    // These four cells were plain `Column`s: a Column takes its width FROM its
                    // widest child, so `Layout.fillWidth` on it granted the cell room the Texts
                    // inside never received, and a Text with no width and no elide simply drew
                    // past the cell into its neighbour. In a three-across row on a narrow panel
                    // the PEAK value overprinted Δ SEGMENT's label.
                    //
                    // ColumnLayout + Layout.fillWidth + elide is the fix: the cell's width now
                    // reaches the Texts, so they truncate at their own boundary instead of over
                    // the next one. Nothing about the look changes at a width where it already
                    // fitted — an elide is inert until it is needed.
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: Theme.sp(12); rowSpacing: Theme.sp(9)

                        // @ impact — the landmark value, tinted by band at impact.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("@ IMPACT"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            // "—" where impact was not measured on this series, in the neutral
                            // colour: a band tint on a withheld reading would still claim a verdict.
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: card.impMeasured
                                         ? cm.formatBare(card.impVal, card.modelData.unit) : "—"
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData
                                   color: card.impMeasured ? root._bandColor(card.bnd)
                                                           : Theme.colorText3 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("PEAK"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: card.valueOk
                                         ? cm.formatBare(card.st.peak, card.modelData.unit) : "—"
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData
                                   color: card.valueOk ? Theme.colorText : Theme.colorText3 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("Δ SEGMENT"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: card.valueOk
                                         ? cm.formatBare(card.st.delta, card.modelData.unit) : "—"
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData
                                   color: card.valueOk ? Theme.colorText : Theme.colorText3 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("PK RATE"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.sp(3)
                                // Both halves take AlignBaseline — a RowLayout aligns to a shared
                                // baseline only for the items that ask, and the plain `Row` this
                                // replaced sat the unit on the value's baseline by anchor (which a
                                // Layout forbids on its children).
                                // The VALUE elides as well, and needs to: once the unit token
                                // below has collapsed to its ellipsis there is nothing left to give
                                // way, and an un-elided number then drew straight over it.
                                // ⚠ THE MAGNITUDE, of a value that is now SIGNED. summaryMasked's
                                // `rate` carries the direction of the steepest change since Phase
                                // 2 (a least-squares slope has one, and throwing it away in C++
                                // would leave no consumer able to recover it), but this tile has
                                // always answered "how fast, at its fastest" — the same question
                                // the corpus baseline table and design §7 item 2's "under 2 units
                                // per 100 ms" are written against — and its neighbours PEAK and
                                // Δ SEGMENT already carry sign where the sign IS the reading.
                                // Printing "-291" here would silently redefine the tile mid-phase.
                                // The signed value stays available in the map (the plumb-bob probe
                                // prints it), and tRateUs says where it was.
                                Text { id: rateVal
                                       Layout.alignment: Qt.AlignBaseline
                                       Layout.fillWidth: true
                                       elide: Text.ElideRight
                                       text: card.rateOk ? Math.round(Math.abs(card.st.rate)) : "—"
                                       font.family: Theme.fontData
                                       font.pixelSize: Theme.fontSzData
                                       color: card.rateOk ? Theme.colorText : Theme.colorText3 }
                                // The ONE value whose unit differs from the card's — a rate, not a
                                // reading — so it says so, and is the only one that may. It is also
                                // the half that gives way when the cell is too narrow: the NUMBER is
                                // the reading, and eliding "°/100ms" to "°/1…" costs less than
                                // eliding the digits.
                                Text { Layout.fillWidth: true; elide: Text.ElideRight
                                       Layout.alignment: Qt.AlignBaseline
                                       // Hidden with the value, not just when the window
                                       // collapsed: "— °/100ms" reads as a measurement in units
                                       // per 100 ms that happens to be missing, when the truth is
                                       // that no rate over 100 ms was fitted at all.
                                       visible: card.rateOk
                                       text: root._unit(card.modelData.unit) + qsTr("/100ms")
                                       font.family: Theme.fontData
                                       font.pixelSize: Theme.fontSzMicro; color: Theme.colorText3 }
                            }
                        }
                    }
                }
            }
        }
    }
}
