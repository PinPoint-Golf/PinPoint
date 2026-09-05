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

#pragma once

#include "../../Metrics/metric_catalogue.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// ChartMetrics — the C++ home for the derivation maths that the "no JavaScript logic
// in QML" rule keeps out of PpMetricChart, exactly the TimelineLabels shape. Every method is
// const and depends only on its arguments, so one shared instance can be declared
// declaratively (ChartMetrics { id: metrics }) and reused by every chart.
//
// It is no longer literally stateless: seriesGroups() needs the metric catalogue, so one is
// assembled in the constructor and read-only thereafter (the same "built once, then const"
// shape MetricCatalog uses). No method mutates it, so the reuse property above is unchanged.
//
// Phase names/tags are NOT duplicated here — the chart composes segment labels in QML from
// phaseA/phaseB via TimelineLabels.phaseShortTag, and the crosshair value-at-cursor reuses
// TimelineLabels.valueAtNearest. This class owns only the segment vocabulary and the
// per-window summary statistics; short names are the catalogue's, read through shortLabel().
class ChartMetrics : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit ChartMetrics(QObject *parent = nullptr)
        : QObject(parent), m_catalogue(pinpoint::analysis::makeMetricCatalogue()) {}

    // Segment list for a swing. [0] = Full ({startUs:0, endUs:spanUs, phaseA:-1, phaseB:-1});
    // then one entry per adjacent phase pair, ordered by time:
    //   { startUs, endUs, phaseA:int, phaseB:int }.
    // The label is composed in QML from phaseA/phaseB via TimelineLabels.phaseShortTag (no
    // tag strings duplicated here). Mirrors swing_data_source.cpp segment logic so the
    // segment vocabulary is identical. `phases` is analysisDetail.phases ([{phase,t_us,…}]).
    Q_INVOKABLE QVariantList segments(const QVariantList &phases, qint64 spanUs) const;

    // Per-metric summary over [startUs, endUs]:
    //   { start, end, min, max, peak, range, delta, rate, tPeakUs, partial,
    //     peakSigma, rateSigma, edgeOk, rateOk, tRateUs }
    // start/end = the ±15 ms windowed MEDIAN at each edge; min/max = the extremum of the 40 ms
    // centred-window MEAN inside the window; peak = whichever of those has the larger magnitude,
    // at tPeakUs, ± peakSigma; range = max-min; delta = end-start; rate = the steepest
    // least-squares slope over any ≥50 ms window, SIGNED, per 100 ms, at tRateUs, ± rateSigma,
    // and only when `rateOk` (see below). `tUs`/`value` are the parallel arrays from
    // analysisDetail.series[i] (tUs ascending).
    //
    // TWO KEYS SAY WHETHER THERE IS ANYTHING TO SHOW, and a display must consult them before it
    // prints anything: `edgeOk` false ⇒ the series carries no valid sample at all, so start, end,
    // min, max, peak, range and delta are zeros from nothing; `rateOk` false ⇒ no window qualified
    // for a slope. Both are the same principle as `partial`'s, one step stronger: partial qualifies
    // a number, these two say there is no number.
    //
    // ⚠ range AND delta DO NOT SHARE EVIDENCE. range = max − min, a span of 40 ms windowed MEANS
    // taken at the samples (ANCHORS) inside the window; delta = end − start, a difference of ±15 ms
    // MEDIANS taken at its two EDGES. Both supports may reach outside the window — neither is
    // clamped to it — so the difference is not the support but what each is anchored on: a window
    // with few anchors in it has little to span while its edges can still have moved. They agree on
    // any window more than a few samples wide, and on a window narrower than the sample spacing
    // they can openly contradict — RANGE 0.2 beside Δ 4, when the edge medians moved and nothing inside the window
    // had a span. That is left as it is on purpose: `range = max(range, |delta|)` would make range a
    // blend of two reducers (so it would no longer be the number the diagnostics engine's Extremum
    // measures compare against, design §7 item 5) and would hide the one signal a reader gets that
    // the window is too narrow for the curve in it. A caller that wants them consistent should
    // widen the window, not launder the number.
    //
    // ⚠ EVERY ONE OF THOSE REDUCTIONS IS src/Analysis/series_reduce.h's, NOT THIS CLASS'S — the
    // diagnostics engine's buildPhaseGrid calls the same four functions with the same tuned
    // windows, which is what makes design §7 item 5 ("the chart and the engine agree to display
    // precision on every authored measure") a property of the code rather than a hope. The only
    // arithmetic left here is the interpolated window edge that stands in when reduceAt has no
    // valid sample within ±15 ms to take a median of, and that case sets `partial`.
    //
    // Delegates to summaryMasked() with an EMPTY mask, which is the "every sample is valid"
    // case — so this overload is exactly the pre-validity behaviour and `partial` is always
    // false. Kept because most callers have no mask to pass and should not have to invent one.
    Q_INVOKABLE QVariantMap summary(const QVariantList &tUs, const QVariantList &value,
                                    qint64 startUs, qint64 endUs) const;

    // The same summary, respecting the series' per-sample validity mask (swing.json
    // `metrics[].valid`, design metric_presentation_honesty.md §5.1). `valid` is an int list
    // parallel to `tUs` where 0 marks a sample the grid BRIDGED across a gated or absent run;
    // EMPTY means every sample is valid, which is what every series that predates the field
    // carries and why summary() above can simply pass {}. A mask SHORTER than the curve is
    // discarded wholesale — see the short-mask rule on measuredAt() below, which this shares.
    //
    // An invalid sample is NOT a measurement, so it enters no reduction: it is not in an edge's
    // median window, not in an extremum's 40 ms mean, not in a rate window's fit. The one place
    // this has to reach across a gap is a window edge with no valid sample within ±15 ms of it,
    // and it says so rather than hiding it:
    //
    //   `partial` (bool) — the window's numbers do not rest on a continuous measurement. True
    //   when the window contains an invalid sample, or when an edge fell back to interpolating
    //   between the nearest valid samples because there was none within ±15 ms of it (which is
    //   also the case that catches a window sitting ENTIRELY inside a bridged run, where there is
    //   no sample to scan at all). The card renders it as a "PARTIAL" chip; it never changes a
    //   value.
    //
    //   ⚠ BOTH RULES REQUIRE A HONOURED MASK, so `partial` is unreachable on a series that
    //   declares nothing — exactly as it was before Phase 2. The fallback ALSO fires with no mask,
    //   whenever a window edge lands more than 15 ms from any sample, and on a real timeline that
    //   is ordinary rather than exotic (a fifth of a series' span can be that far from a sample,
    //   with gaps to 80 ms). A coarse series is not an incomplete one: it was measured everywhere
    //   it claims to have been. The chip's claim is "the producer BRIDGED part of this window", and
    //   the mask is the only thing that ever says so; a series with nothing readable at all is
    //   `edgeOk` false, not `partial` true.
    //
    //   `edgeOk` (bool) — false when the series carries NO valid sample: every sample bridged, or
    //   an empty curve. There is then nothing for the window edges to interpolate between, and
    //   start/end/min/max/peak/range/delta are all 0.0 — a "PEAK 0, Δ 0, RANGE 0" card that reads
    //   as a still, well-behaved curve. The tiles print "—" instead, the same way PK RATE does on
    //   `rateOk` false. The zeros are still returned so a caller mid-migration degrades rather
    //   than crashes; a caller that displays them has been told not to.
    //
    // ⚠ THE RATE AND PEAK DEFINITIONS CHANGED IN PHASE 2 (design §5.2), and they changed the
    // numbers on every card, on every swing, whether or not anything is masked:
    //
    //   peak/min/max — was the raw argmax over the in-window samples plus the two interpolated
    //   edges; is now the extremum of the 40 ms centred-window MEAN of the valid samples. A
    //   one-sample outlier can no longer be the peak, because a peak now has to have been there
    //   for 40 ms: one 99 among 4s at 8 ms sampling reports about 23, not 99.
    //
    //   rate — was max |Δvalue/Δt| between consecutive samples, which on a still address is
    //   frame noise divided by 8 ms (39 and 291 units per 100 ms on the corpus, design §7 item 2);
    //   is now the steepest least-squares slope over a window of at least 50 ms carrying at least
    //   3 valid samples. It is SIGNED (a slope has a direction, and no consumer could recover one
    //   this class had thrown away), and it can be ABSENT:
    //
    //   `rateOk` (bool) — false when no window in [startUs, endUs] qualifies (a window shorter
    //   than 50 ms, or fewer than 3 valid samples in it, e.g. a two-sample series). `rate`,
    //   `rateSigma` and `tRateUs` are then 0 and MUST NOT be displayed: the card prints "—" and
    //   hides the per-100 ms unit with it. A fabricated 0 would read as a still, well-behaved
    //   curve, which is the exact class of confident absurdity this design exists to remove.
    //
    // `peakSigma` / `rateSigma` are the σ of the winning window, for the "± σ" the summary card
    // carries beside those two tiles — design §5.3. Both are the NOISE, never the motion:
    // peakSigma is the standard error of the window's mean about a LOCAL STRAIGHT LINE through it
    // (so a clean ramp reports 0 rather than reporting its own slope as uncertainty), rateSigma
    // the standard error of the fitted slope (so an exact fit reports 0).
    Q_INVOKABLE QVariantMap summaryMasked(const QVariantList &tUs, const QVariantList &value,
                                          const QVariantList &valid,
                                          qint64 startUs, qint64 endUs) const;

    // Where a metric's geometry MEANS something:
    //   { firstPhase:int, lastPhase:int, firstNarrowed:bool, lastNarrowed:bool, narrowed:bool }
    // The phases are Phase ENUM values, not ladder indices — the caller resolves them against the
    // swing's own phases[] to get instants, because only the swing knows when its P4 happened.
    //
    // Straight off MetricDescriptor::domain, so the manifest is the single author of it and the
    // chart, the pack validator and the diagnostics engine cannot disagree about where a
    // pelvis-sway reading stops meaning translation and starts meaning rotation. A key the
    // catalogue has never heard of gets the descriptor default — the WHOLE swing — because an
    // unknown metric is not a licence to hide part of its curve.
    //
    // ⚠ THE NARROWED FLAGS ARE LOAD-BEARING, PER SIDE, and are why this returns five keys and not
    // two. The default domain is Address..Finish, but the chart's AXIS is the PADDED swing
    // (Segmentation swingStart/End ± boundPadUs = 250 ms), so Address sits 250 ms inside the axis
    // start and Finish 250 ms inside its end. A caller that clipped to the default domain would
    // therefore dash 250 ms off each end of EVERY whole-swing metric — headSway, xFactor,
    // clubheadSpeed — and change its Full-window PEAK/Δ/RATE on every swing, which is precisely
    // the "nothing changes where this does not fire" property phase 1 rests on. It would also
    // collapse any legitimately pre-address window (the still-address check reads
    // Address−300 ms → Address) to nothing.
    //
    // So: clip a side ONLY when the MANIFEST moved that side. `firstNarrowed` is
    // `domain.first != Phase::Address`, `lastNarrowed` is `domain.last != Phase::Finish`, and
    // `narrowed` is either. hipLineTilt is narrowed on the LAST side only.
    Q_INVOKABLE QVariantMap domainFor(const QString &key) const;

    // Was this series actually MEASURED at `us`? — the reference form of the predicate behind the
    // suppressed phase dots, the suppressed crosshair marker and the "—" in the hover and legend
    // readouts, so those cannot drift into slightly different notions of "no reading here".
    //
    // False when `us` lies outside [fromUs, toUs] — the metric's phase domain resolved to
    // instants by the caller, since only the swing knows when its P7 happened — or when the
    // NEAREST sample carries a 0 in `valid` (bridged across a gated or absent run). Nearest is
    // exact for its caller: a phase dot sits on a sample. Pass fromUs == toUs for "no domain".
    //
    // ⚠ THE SHORT-MASK RULE, shared with summaryMasked() and with measure_sample.cpp's
    // buildPhaseGrid: a `valid` list is honoured only when it covers the whole curve
    // (size >= t_us.size()). An EMPTY one is "every sample valid" (C4); a SHORTER one is a
    // malformed document, not a partial statement, and is discarded wholesale — guessing which
    // end it was truncated from would invent validity nobody stated, and bounding the scan at
    // qMin(sizes) instead would make this answer a different question than summaryMasked does
    // about the very same series.
    //
    // ⚠ NOT FOR PER-FRAME BINDINGS. Every call marshals the whole series across the QML boundary,
    // so a binding that depends on the cursor or the playhead must answer this in JS off a sample
    // INDEX (PpChartPlot._measured / PpMetricChart._measuredAt) rather than call in here.
    Q_INVOKABLE bool measuredAt(const QVariantList &tUs, const QVariantList &valid,
                                qint64 us, qint64 fromUs, qint64 toUs) const;

    // Compact display name for a metric key (e.g. "leadWristFlexExt" → "Bow/cup"), or ""
    // when the key is uncatalogued or its descriptor names no short form — the caller then
    // falls back to series.label. This is a straight read of MetricDescriptor::shortLabel,
    // so the chart, the Metric Library and the summary cards all say the same word for the
    // same metric, and a metric added to the manifest is short-named everywhere at once.
    Q_INVOKABLE QString shortLabel(const QString &key) const;

    // The DISPLAY form of a unit — what goes beside a number on the chart panel.
    //
    // The catalogue's unit is a full phrase where the denominator matters: "% stance width" and
    // "% shoulder width" are different quantities and the Metric Library, which is a reference
    // surface with room, is right to spell both out. On the chart it is repeated beside every
    // value in a data face, and "12 % stance width" next to "34 % stance width" overprinted its
    // neighbour in the summary grid — the phrase was longer than the number it qualified.
    //
    // So this returns the SHORT token: "%" for every percent-of-something, the unit unchanged for
    // everything else. It is keyed on the UNIT, not the metric, deliberately — six metrics share
    // "% stance width" and all six want the same token, so authoring it per descriptor would be six
    // chances to disagree. The canonical unit is untouched: it still has to match the norm's unit
    // (the loader refuses a mismatch) and measureUnitMismatch still compares it against the
    // producer's, so this cannot drift into being the real unit.
    //
    // What replaces the lost words is CONTEXT, not guesswork — see the rule below.
    Q_INVOKABLE QString shortUnit(const QString &unit) const;

    // ── The two value formatters, and why they live here ────────────────────────────────────────
    //
    // ONE rule, ONE implementation. This was three: PpMetricChart._fmt, PpChartSummary._fmt (whose
    // comment said "see PpMetricChart._fmt" — a copy that knew it was a copy), and a third in
    // PpTransitTimeline that concatenated value and unit with no separator and so read "12mph".
    // Three copies of a five-line rule is three chances to disagree, and they already did.
    //
    // It belongs in C++ for the reason this class exists at all: it is derivation, and the "no
    // JavaScript logic in QML" rule keeps derivation out of the .qml files. It is also the only
    // way to TEST it — chart_metrics_test can assert "-8°" and "12 %"; a QML function cannot be
    // asserted anywhere.
    //
    // ── σ GOVERNS THE DIGITS (design metric_presentation_honesty.md §5.3, principle 3) ───────────
    //
    // "A reading is shown no finer than its characterised noise." displayStep is that rule as one
    // number: the DISPLAY QUANTUM a value of a series with measurement noise `sigma` may be printed
    // in — the smallest of {1, 2, 5}×10ⁿ that is NOT BELOW sigma, floored at one unit.
    //
    //   σ 0 → 1    σ 0.3 → 1    σ 1.4 → 2    σ 2.5 → 5    σ 6 → 10    σ 12 → 20    σ 30 → 50
    //
    // ⚠ σ ≤ 0 AND σ ABSENT ARE THE SAME ANSWER HERE, 1, AND ONLY HERE. `MetricSeries::sigma` is
    // optional and absent means "never characterised", which is emphatically not "zero" — nothing
    // in the DATA path may substitute one for the other. But a formatter has to print SOMETHING,
    // and the only honest fallback for an uncharacterised series is the rounding we already did
    // before σ existed: whole units. So the bridge's "no sigma key" becomes a 0 at the QML call
    // site (seriesSigma() below), and that 0 means "no claim", not "exact".
    //
    // ⚠ AND +INFINITY IS ALSO ABSENT. A σ of inf is not "infinitely coarse, print one digit for the
    // whole swing" — it is a producer that divided by a zero span, i.e. a broken error budget, and
    // an unusable σ is indistinguishable from an unstated one. NaN goes the same way, and a NEGATIVE
    // σ likewise: all four collapse to the floor, which is the display that claims nothing extra.
    //
    // ⚠ IT FLOORS AT 1 AND ONLY EVER RETURNS ≥ 1 — this rule can coarsen a reading and can never
    // sharpen one. Two reasons, and the second is mechanical: a plumb bob with σ = 0.1 in has not
    // earned a decimal (its noise being small is no evidence the projection and calibration agree to
    // a hundredth of an inch); and the formatters llround to an INTEGER, so a sub-unit step could not
    // survive the round trip anyway — 0.5 would collapse straight back to 1 and the caller would
    // never know it had been ignored. `unit` is consulted for the floor (unitStepFloor in the .cpp)
    // so a unit that one day wants a coarser one has one place to say so.
    Q_INVOKABLE double displayStep(double sigma, const QString &unit) const;

    // The series' σ as a DISPLAY number — `series` is one entry of analysisDetail.series (the QML
    // bridge's map), and the answer is its `sigma` when the producer characterised one and 0 when it
    // did not. 0 asks displayStep for no coarsening at all, so an uncharacterised series prints
    // exactly as it did before §5.3 existed.
    //
    // ⚠ THE ABSENT→0 SUBSTITUTION LIVES HERE AND NOWHERE ELSE, and this function exists because it
    // was living in three places: a `_sigma()` copy in PpChartSummary, another in PpMetricChart, and
    // an inline expression inside PpChartPlot._sigmaRuns. Three copies of a four-clause guard
    // (undefined / null / non-finite / ≤ 0) is three chances to disagree about what absence means,
    // which is precisely the confusion the field's contract exists to prevent. It is also the only
    // form that can be tested.
    //
    // The substitution is legitimate ONLY at the display boundary: a reading with no characterised
    // noise is printed at the precision we always printed it, and a 0 out of here means "no claim",
    // never "exact". Nothing that touches DATA may call this to fill in a missing σ.
    //
    // ⚠ NOT FOR PER-FRAME BINDINGS, for the reason measuredAt() carries the same warning: a
    // QVariantMap argument marshals the WHOLE series (t_us, value, valid, phaseSamples) across the
    // QML boundary. Every caller resolves it once per card / chip / row / plot, on a binding that
    // changes with the DATA, and passes the resulting number down to the per-frame formatters.
    Q_INVOKABLE double seriesSigma(const QVariantMap &series) const;

    // formatValue = the number and its unit, for a surface with no header to lean on (legend
    // chips, the hover tooltip, the transit bead). Degrees keep the signed-deviation convention
    // ("+12°", closed up); every other unit takes a space ("75 mph", "12 %").
    //
    // `sigma` (the series' measurement noise, 0 = uncharacterised) rounds the number to
    // displayStep(sigma, unit) instead of to whole units: at σ = 2.5° a reading of 12.6° prints
    // "+15°", and at σ = 0 the output is byte-identical to the pre-σ formatter.
    //
    // ⚠ A DEFAULT ARGUMENT, NOT AN OVERLOAD, and the difference is what makes it safe to call at
    // either arity from QML. moc emits a CLONED method entry per default argument (QMetaMethod::
    // MethodCloned; QQmlPropertyData::isCloned() carries it into the QML property cache), so
    // `cm.formatValue(v, unit)` binds to a distinct 2-argument entry rather than resolving an
    // overload set — the thing that is fragile. Verified against shipping call sites in this Qt
    // 6.11.1 build, deliberately at BOTH arities, because a clone being reachable says nothing about
    // the full form still being reachable beside it:
    //   · SHORT form  — PpDetectCluster.qml:145 `shotController.triggerShot()`, none of its two
    //                   defaulted arguments; DiagnosticModel.qml:594 `browser.createObject(type)`,
    //                   one of two.
    //   · FULL form   — DiagnosticModel.qml:242 and :1664 `browser.rows(type, filters)` against
    //                   `rows(const QString &, const QVariantMap & = {})`, i.e. the same method
    //                   called at full arity elsewhere in the same file.
    // The probe pins the full form for THESE methods unconditionally: tools/probes/plumb_bob_chart
    // prints `formatBare(12.6, unit, 2.5)` and only a correct 3-argument resolution gives "+15".
    // So the names stay ONE name each (this class's whole point is one rule, one implementation)
    // instead of gaining a parallel …Sigma pair.
    Q_INVOKABLE QString formatValue(double v, const QString &unit, double sigma = 0.0) const;

    // formatBare = the number ALONE, for a surface whose container already names the unit — the
    // summary card's header, the split-mode gutter. Same sign convention as formatValue, so one
    // reading does not change shape depending on where it is shown. Same `sigma` rule, and the
    // same default-argument note above.
    Q_INVOKABLE QString formatBare(double v, const QString &unit, double sigma = 0.0) const;

    // Every "± x" on the panel: the series σ chip beside the card's unit, and the ± beside PEAK
    // (summaryMasked's `peakSigma`) and PK RATE (`rateSigma`). Returns the whole displayed string,
    // "± " included, so what a reader sees is one testable value; unsigned, because an uncertainty
    // has no direction. `unit` appends a display token with formatValue's spacing (degrees close up,
    // everything else spaced); EMPTY means no token, which is what the two tiles want because the
    // card names their unit above them — note that this is the OPPOSITE of formatBare/formatValue,
    // where an empty unit falls back to degrees.
    //
    // ── READINGS ARE QUANTISED, UNCERTAINTIES ARE QUOTED ────────────────────────────────────────
    //
    // The rule, and the one line to remember: displayStep governs READINGS. It has no business
    // touching a ±. An uncertainty is not a reading of the athlete taken at some precision, it is
    // the statement of how far the reading can be trusted, and it is only ever read AGAINST the
    // value beside it — so it is QUOTED at a fixed one decimal and never quantised:
    //   · one decimal, always ("± 2.4", "± 12.4");
    //   · "± <0.1" below 0.05, which is also where an exactly-zero `err` goes;
    //   · never "± 0.0", and never "± 0".
    //
    // Three concrete things went wrong when the step DID govern this, all found in review:
    //   · PK RATE — a fitted-slope standard error of 3.0 printed "± 5" on a series whose σ chose a
    //     5-unit step, inflating the stated uncertainty by two thirds for a reason belonging to
    //     another quantity entirely (the σ is in the metric's unit; a slope's error is per 100 ms).
    //   · PEAK — peakSigma is about σ/√k for a k-sample window, so it is SMALLER than σ by
    //     construction and a step chosen from σ rounded it to nothing on essentially every card;
    //     the "fall back to one decimal" branch was the only one that ever ran, and a rule whose
    //     main branch is unreachable is a rule that is not doing what it says.
    //   · THE σ CHIP — the plumb bob's own σ is 0.03–0.06 in, so quantising it printed "± 0.0in",
    //     which claims exactness in the one place on the card whose entire job is to deny it.
    // Quoting removes all three at once, and removes the discontinuity with them: an err of 2.4 and
    // an err of 2.6 now print "± 2.4" and "± 2.6" instead of jumping "± 0" → "± 5" across half a
    // step. There is no `sigma` parameter, deliberately — not an ignored one, an absent one, so a
    // future caller cannot reintroduce the coupling by passing it.
    //
    // An empty string for a non-finite `err` (NaN, or a ±inf from a degenerate fit), which the
    // caller renders as nothing rather than as "± nan".
    Q_INVOKABLE QString formatUncertainty(double err, const QString &unit = QString()) const;

    // "Nice" Y-axis tick values across [lo, hi] at a 1/2/5×10ⁿ step chosen so there are
    // about `maxTicks` of them. Returns the tick values (doubles) the chart labels + grids.
    Q_INVOKABLE QVariantList niceTicks(double lo, double hi, int maxTicks) const;

    // X-axis tick offsets in milliseconds relative to impact, for the domain
    // [domStartUs, domEndUs]. Each returned int `ms` marks a gridline at impactUs+ms*1000
    // that falls inside the domain; the step widens with the span. The chart labels them
    // "(+)ms" and positions each via its own xForT(impactUs + ms*1000).
    Q_INVOKABLE QVariantList timeTicksMs(qint64 domStartUs, qint64 domEndUs,
                                         qint64 impactUs) const;

    // Phase enum of the station nearest `us` (or -1 when `phases` is empty). Used to label
    // a free-dragged ("Custom") window with the phases bracketing its edges.
    Q_INVOKABLE int nearestPhase(const QVariantList &phases, qint64 us) const;

    // Band ("good"/"attention"/"warn") of the phaseSample nearest `us`, used to tint a summary
    // card's @impact value by the swing's state there.
    //
    // Returns "" — NO BAND, tint neutrally — when the list is empty or the nearest sample is
    // more than `kBandNearUs` (one generous frame) from `us`. It used to return "good" in both
    // cases, and that is a graded verdict invented out of nothing: a series whose producer
    // emitted no sample at impact (because the geometry was gated there, which is exactly what
    // design §5.1 makes happen) had its @impact reading tinted GREEN off an empty list, or off
    // the Address sample 900 ms away. The caller must treat "" as "no verdict", not as a pass.
    Q_INVOKABLE QString bandAtNearest(const QVariantList &phaseSamples, qint64 us) const;

    // ── Corridor-bar backing (dashboard_reductions.h) ───────────────────────────

    // The value→x domain of ONE corridor bar (NormativeBar):
    //   { lo, hi, valid }
    // Two-sided: the amber band padded 12% each side, falling back to green then to
    // value±1. One-sided: the open side runs past the furthest of (aspiration, reading)
    // by 35% of the graded span, leaving the room the caller fades the band across —
    // without it a floor's Ideal readings all clamp to the last pixel of the track.
    // valid=false when there is neither a corridor nor a finite reading; the bar then
    // draws its rail and no bands, rather than a band pinned to the left edge.
    Q_INVOKABLE QVariantMap barDomain(double greenLo, double greenHi,
                                      double amberLo, double amberHi,
                                      bool lowOpen, bool highOpen,
                                      double value, bool hasValue) const;

    // ── Chart metric presets ────────────────────────────────────────────────────
    //
    // The swing's series bucketed by the catalogue's `.group` — the combo in the chart's
    // CONTROLS section. `seriesList` is analysisDetail.series; returns
    //   [{ group:QString, keys:[QString…] }]
    // in MANIFEST order (MetricCatalogue::all()'s order, which is also the order the Metric
    // Library lists groups in, so the two surfaces agree).
    //
    // Only PLOTTABLE series count — a curve of at least two samples, the same test the chart's
    // own `_visible` applies. The setup scalars (stance width, tempo, attack angle …) carry one
    // phaseSample and an empty curve, so a group made only of those is omitted rather than
    // offered as a preset that draws nothing.
    //
    // A group is present only when this swing produced at least one of its members, which is
    // what makes the control degrade honestly: no IMU wrist data and there is simply no "Wrist
    // & forearm" preset. That gating is on the DATA, never on the session type.
    //
    // After the groups come the CROSS-CUTTING presets — MetricDescriptor::presets, which is how a
    // coaching read that spans groups ("Plumb Bob" = the hip centre over the stance plus the tilt
    // of the hip line plus pelvis sway) gets one entry without any of its members leaving the group
    // it is properly filed under. A preset is offered only when at least TWO of its members are
    // plottable on this swing: one curve is a legend chip, not a preset.
    //
    // Curve keys the catalogue has never heard of are collected into a trailing "Other" group
    // rather than dropped: a metric added to the pipeline before the manifest should be awkward
    // to find, not invisible.
    Q_INVOKABLE QVariantList seriesGroups(const QVariantList &seriesList) const;

private:
    pinpoint::analysis::MetricCatalogue m_catalogue;   // built once in the ctor; never mutated
};
