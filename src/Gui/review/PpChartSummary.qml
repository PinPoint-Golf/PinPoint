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
// of nothing rather than a flat curve. PEAK has a third absence of its own (`extremumOk` false): a
// window narrower than the sample spacing contains no measurement to have an extremum, so the tile
// prints "—" rather than the interpolated edge the reducer falls back to.
//
// Since Phase 6 the CHART DRAWS those same windowed means (ChartMetrics.windowedMean, decorated as
// `mean` by PpMetricChart._plottable), so the PEAK tile here is now a point on the line beside it —
// bit-exactly, by construction, and pinned that way in chart_metrics_test. Nothing in this file
// changed for that: it already reduced on those means.
//
// ⚠ AND @IMPACT READS THE MEAN TOO. It was the last reading on this panel taken from a raw sample,
// which after Phase 6 meant the number a coach quotes could differ from the curve under the
// crosshair by the height of a single frame's wobble — one panel, two answers, at the one instant a
// reader trusts most. It is now the drawn value at the impact sample, with the RAW value one hover
// away in its tooltip (same "raw N" the chart's hover row prints). No reading on this panel is
// anything other than the line beside it.

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
    // The series' σ for FORMATTING is ChartMetrics.seriesSigma — one implementation of the
    // absent→0 substitution, in C++ where it can be tested, replacing what used to be a
    // four-clause guard copied into this file, PpMetricChart and PpChartPlot. Resolved once per
    // card (`card.sig`) because a QVariantMap argument marshals the whole series.

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
    // WHICH ARRAY IS THE CURVE (Phase 6): `mean` — the 40 ms centred windowed mean the chart strokes
    // and summaryMasked reduces — where the host decorated one (PpMetricChart._plottable), else the
    // persisted `value`. The same one-line rule as PpChartPlot._meanOf, PpMetricChart._meanOf and
    // PpSegmentBrush._meanOf: this component takes a `series` list from anywhere and may not assume
    // another prepared it. The predicate is asked by name rather than by comparing the returned array
    // against s.value, so no drawing or display decision rests on object identity across the bridge.
    //
    // ⚠ THE REDUCERS ARE STILL FED RAW. summaryMasked below gets `value`, never this — reducing a
    // reduction would window the curve twice and no tile would mean what its definition says.
    function _hasMean(s) {
        return !!(s && s.mean && s.t_us && s.mean.length === s.t_us.length)
    }
    function _meanOf(s) {
        return root._hasMean(s) ? s.mean : s.value
    }

    // The measured-at-an-instant test, in JS for the reason chart_metrics.h gives: cm.measuredAt
    // marshals the whole series per call, and these bindings re-evaluate as the window moves.
    // Same rule, same short-mask discipline (a mask that does not cover the curve is discarded).
    //
    // ⚠ isFinite IS FOLDED IN (F5), which is why the nearest sample is now found before the
    // short-mask early return rather than after it: a series with NO mask must still not print a NaN
    // as a reading — `formatBare` on one renders "nan", in the band colour, as the card's headline
    // number. Same fold as PpChartPlot._measured and SeriesView::isValid, for the same reason:
    // nothing in the pipeline should produce a NaN, which is exactly why it cannot be assumed.
    function _measuredAt(s, t) {
        if (s.validFromUs !== undefined && s.validToUs !== undefined
            && s.validToUs > s.validFromUs && (t < s.validFromUs || t > s.validToUs))
            return false
        var tt = s.t_us
        if (!tt || tt.length === 0) return true
        var best = -1, bd = Infinity
        for (var i = 0; i < tt.length; ++i) {
            var d = Math.abs(tt[i] - t)
            if (d < bd) { bd = d; best = i }
        }
        if (best < 0) return true
        if (s.value && !isFinite(s.value[best])) return false
        if (!s.valid || s.valid.length < tt.length) return true
        return s.valid[best] !== 0
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
                // ⚠ `reduceValid`, NOT `valid` (F4): the host composes `valid` AND in-domain into one
                // mask (PpMetricChart._reduceMask) and every reduction on the panel is given that
                // same one. Without it, a swing analysed before the producers marked out-of-domain
                // samples invalid has those samples averaged into the anchors beside the domain
                // boundary — a hip line past impact, which measures rotation and not tilt, feeding a
                // tile that design §5.1 says it must not reach — and the tile then differs from the
                // line the chart drew, which was told the truth. Falls back to `valid` for a caller
                // that has not composed one, which is the pre-Phase-6 behaviour exactly.
                readonly property var    st:  cm.summaryMasked(card.modelData.t_us,
                                                         card.modelData.value,
                                                         card.modelData.reduceValid
                                                         || card.modelData.valid || [],
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

                // ── PEAK's OWN GATE (F2) ────────────────────────────────────────────────────
                //
                // `extremumOk` false means NO VALID SAMPLE LIES IN THIS WINDOW, so min/max/peak/range
                // were taken from the two interpolated EDGES — between measurements, not from any.
                // It happens on a perfectly healthy series whenever the window is narrower than the
                // sample spacing (a brush pinched to 50 ms on a 100 ms-strided address, or a phase
                // pair closer together than a frame), and on an UNMASKED series `partial` cannot
                // report it: that chip needs an honoured mask, so this state used to wear nothing at
                // all and read as an ordinary PEAK.
                //
                // Since Phase 6 the tile claims more than it used to — the drawn line IS the reduced
                // curve, so PEAK is advertised as a point on it — and this is the one state where no
                // point of the line is in the window to be that peak. So it prints "—", exactly as
                // PK RATE does on `rateOk` false. Δ SEGMENT and @IMPACT are deliberately NOT gated on
                // it: both are statements about INSTANTS, which is precisely what the edges are.
                //
                // `!== false` like valueOk, not `=== true` like rateOk: a map from a C++ that never
                // heard of the key must not blank the tile on every card of every swing.
                readonly property bool   peakOk: card.valueOk && card.st.extremumOk !== false

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
                                                      && root._measuredAt(card.modelData,
                                                                          root.impactUs)
                //
                // ⚠ THE MEAN, NOT THE RAW SAMPLE (Phase 6). The chart strokes the 40 ms windowed
                // mean, so a tile quoting the persisted sample at impact could differ from the curve
                // the reader is looking at by one frame's wobble — the panel would carry two answers
                // for one instant, which is the exact failure design §4 principle 1 is about. The raw
                // sample is not lost: `impRaw` puts it in this tile's tooltip, the same "raw N" the
                // chart's hover row prints, so the reduction stays visible rather than silent.
                //
                // The FALLBACK is still `st.end`, the window's ±15 ms median edge, for a series with
                // no known impact — a different reduction, but it is answering a different question
                // ("where did this window leave off") and it always did.
                readonly property real   impVal: root.impactUs > 0
                                                 ? labels.valueAtNearest(card.modelData.t_us,
                                                       root._meanOf(card.modelData), root.impactUs)
                                                 : card.st.end
                // The persisted sample under that reading, for the tooltip. "" when there is no
                // reduction to explain (the tile IS the raw sample then) or no impact landmark, so
                // the tooltip is offered only where it has something to add. Same σ step as the
                // reading above: a raw sample is a reading like any other (design §5.3), and a
                // tooltip printing finer digits than the tile it explains would be the false
                // precision the step rule exists to remove, reintroduced in small type.
                readonly property string impRaw: (root.impactUs > 0 && root._hasMean(card.modelData))
                                                 ? qsTr("raw %1").arg(cm.formatBare(
                                                       labels.valueAtNearest(card.modelData.t_us,
                                                           card.modelData.value, root.impactUs),
                                                       card.modelData.unit, card.sig))
                                                 : ""
                // "" = NO VERDICT, and it must stay neutral rather than green: bandAtNearest now
                // refuses to answer when the nearest phaseSample is a frame or more away, or when
                // there are none, instead of defaulting to "good" off nothing at all.
                readonly property string bnd: cm.bandAtNearest(card.modelData.phaseSamples,
                                                  root.impactUs > 0 ? root.impactUs : root.endUs)
                readonly property string nm:  cm.shortLabel(card.modelData.key)
                                              || card.modelData.label || card.modelData.key
                // This series' measurement noise, resolved ONCE for the card: it governs the digits
                // of every READING printed below (ChartMetrics.displayStep) and it is what the chip
                // beside the unit quotes. Re-deriving it per tile would be four chances to disagree
                // about how coarse this card is, and four whole-series marshals per repaint.
                // 0 = uncharacterised, which asks for no coarsening — ChartMetrics::seriesSigma.
                //
                // ⚠ IT GOVERNS THE READINGS AND NOTHING ELSE. The three ± on this card are QUOTED,
                // not quantised (ChartMetrics::formatUncertainty says why at length): an uncertainty
                // is read against the value beside it, so a step chosen for the value would inflate
                // a small error to a whole step and round a smaller one to a false zero.
                readonly property real   sig: cm.seriesSigma(card.modelData)

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
                            //
                            // ⚠ QUOTED, NOT QUANTISED, and through the same ChartMetrics call as the
                            // other two ± on this card — which is the whole reason it stopped being
                            // a local `.toFixed(1)` here. Two bugs went with the copy. It printed
                            // "± 0.0in" for the plumb bob, whose σ is 0.03–0.06 in: a chip whose one
                            // job is to deny exactness, claiming it. And it jammed the unit against
                            // the number with no separator, the same defect formatValue exists to
                            // prevent ("12mph"). Now: "± <0.1 in", "± 2.5°".
                            //
                            // It could not be step-quantised in any case — σ is the number that SET
                            // the step, so quantising it would state the noise as the coarseness it
                            // chose (a σ of 2.5° as "± 5°") and make the chip circular.
                            readonly property real sigma: card.sig
                            visible: sigmaChip.sigma > 0
                            text: cm.formatUncertainty(sigmaChip.sigma,
                                                       root._unit(card.modelData.unit))
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
                            // σ GOVERNS THE DIGITS: formatBare rounds to displayStep(σ, unit), so
                            // a card whose series carries a 2.5° noise floor prints multiples of
                            // 5° here. NO ± beside this tile — @IMPACT is a reading of the curve at
                            // an instant, and the only uncertainty on it is the series σ already
                            // stated in the header chip. A second ± would double-count it.
                            Text {
                                id: impText
                                Layout.fillWidth: true; elide: Text.ElideRight
                                text: card.impMeasured
                                      ? cm.formatBare(card.impVal, card.modelData.unit, card.sig)
                                      : "—"
                                font.family: Theme.fontData
                                font.pixelSize: Theme.fontSzData
                                color: card.impMeasured ? root._bandColor(card.bnd)
                                                        : Theme.colorText3
                                // THE RAW SAMPLE, ONE HOVER AWAY. The tile is the drawn line's value
                                // at impact; this is what the frame actually recorded there. It is a
                                // tooltip rather than a second line because the 2×2 grid has no room
                                // for a fifth number and because the reduction is the reading — the
                                // raw sample is the evidence behind it, which is what a tooltip is
                                // for. The chart's hover row prints the same pair side by side for a
                                // reader who wants it at every instant, not just at impact.
                                //
                                // Offered ONLY where it adds something: no impact landmark, no mean,
                                // or nothing measured there ⇒ no tooltip. A "raw N" beside a "—"
                                // would say the reading exists and is being withheld.
                                HoverHandler { id: impHover }
                                ToolTip.visible: impHover.hovered && impText.ToolTip.text.length > 0
                                ToolTip.delay: 400
                                ToolTip.text: (card.impMeasured && card.impRaw.length > 0)
                                              ? qsTr("Drawn value at impact (40 ms windowed mean). "
                                                     + "Recorded sample there: %1.").arg(card.impRaw)
                                              : ""
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("PEAK"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: card.peakOk
                                         ? cm.formatBare(card.st.peak, card.modelData.unit, card.sig)
                                         : "—"
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzData
                                   color: card.peakOk ? Theme.colorText : Theme.colorText3 }
                            // ── ± ON THE PEAK, AND WHY IT IS ITS OWN LINE ────────────────────
                            //
                            // summaryMasked's `peakSigma`: the standard error of the winning 40 ms
                            // window's mean about a LOCAL STRAIGHT LINE through it, so a clean ramp
                            // reports 0 rather than reporting its own slope as uncertainty. Design
                            // §5.3 puts it here because PEAK and PK RATE are where a reader's trust
                            // in this panel is decided — a peak with no error bar is the one number
                            // on the card that invites over-reading.
                            //
                            // ⚠ NO UNIT ARGUMENT (the card names it above) AND NO σ ARGUMENT: this
                            // is QUOTED at one decimal, never quantised to the value's step. For
                            // peakSigma that distinction is the difference between a number and
                            // nothing — it is about σ/√k for a k-sample window, so it is SMALLER
                            // than the series σ by construction and a step chosen from σ rounded it
                            // to zero on essentially every card.
                            //
                            // ⚠ ON ITS OWN LINE, for the reason the PARTIAL chip above is: this
                            // cell is one of four in a card that can be 150px wide, and a second
                            // fixed-width token on the value's row leaves the value and the ± both
                            // elided into ellipses. A caveat that is illegible is worse than absent.
                            //
                            // Hidden with the value, not merely dimmed: `peakOk` false means either
                            // that the series carries no valid sample at all or that none of them is
                            // in this window, and peakSigma came back 0 out of nothing either way —
                            // "± 0" under an em dash claims a measured exactness.
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   visible: card.peakOk
                                   text: cm.formatUncertainty(card.st.peakSigma)
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro
                                   font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: qsTr("Δ SEGMENT"); font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro; font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                            // Same step rule; NO ±, per design §5.3 as pinned in C12. A Δ is a
                            // difference of two ±15 ms edge medians, whose combined error is not
                            // `peakSigma` and is not `rateSigma` — the reducers do not produce one,
                            // and inventing σ√2 here would be this file deriving an error budget,
                            // which is the analysis layer's job and nobody has done it yet.
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   text: card.valueOk
                                         ? cm.formatBare(card.st.delta, card.modelData.unit, card.sig)
                                         : "—"
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
                                //
                                // ⚠ AND IT IS NOT PUT THROUGH THE σ STEP RULE, deliberately. This
                                // tile's quantity is units PER 100 ms, and the series' σ is in the
                                // metric's own unit — quantising a slope to a step derived from a
                                // position's noise is a category error, and at σ = 2.5° it would
                                // round a rate of 291°/100 ms to 290 for a reason that has nothing
                                // to do with how well the slope was determined. What DOES say that
                                // is `rateSigma`, on the line below, and it is in the rate's unit.
                                // Math.round for the same reason a rate has never carried a decimal
                                // here: three digits of °/100ms is already more than the cell holds.
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
                            // ── ± ON THE RATE ────────────────────────────────────────────────
                            //
                            // summaryMasked's `rateSigma`: the standard error of the FITTED SLOPE,
                            // so an exact fit reports 0 and a slope fitted through noise reports
                            // the width of the family of lines that would have done as well. On the
                            // corpus's still-address window that number is the whole story — a
                            // "rate" of 1.8 per 100 ms with a ± of 1.5 is visibly not motion.
                            //
                            // Its unit is the rate's (per 100 ms), which the token above already
                            // names for the value it sits under; that is why the ± carries no unit
                            // of its own and why it is on this line rather than crowding that row
                            // (see the PEAK note). Hidden with the value on `rateOk` false: no
                            // window qualified, so rateSigma is 0 out of nothing.
                            //
                            // ⚠ AND IT DOES NOT TAKE THE SERIES σ. It briefly did, and a fitted-slope
                            // standard error of 3.0 printed "± 5" because the σ had chosen a 5-unit
                            // step — two thirds of inflation borrowed from a quantity in a different
                            // unit. Quoted, it prints 3.0. This is the tile the still-address gate is
                            // read on (§7 item 2), where the ± IS the finding, so it has to be the
                            // number the reducer produced.
                            Text { Layout.fillWidth: true; elide: Text.ElideRight
                                   visible: card.rateOk
                                   text: cm.formatUncertainty(card.st.rateSigma)
                                   font.family: Theme.fontData
                                   font.pixelSize: Theme.fontSzMicro
                                   font.letterSpacing: Theme.trackingData
                                   color: Theme.colorText3 }
                        }
                    }
                }
            }
        }
    }
}
