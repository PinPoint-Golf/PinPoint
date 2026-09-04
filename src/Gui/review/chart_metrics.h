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
    //     peakSigma, rateSigma, rateOk, tRateUs }
    // start/end = the ±15 ms windowed MEDIAN at each edge; min/max = the extremum of the 40 ms
    // centred-window MEAN inside the window; peak = whichever of those has the larger magnitude,
    // at tPeakUs, ± peakSigma; range = max-min; delta = end-start; rate = the steepest
    // least-squares slope over any ≥50 ms window, SIGNED, per 100 ms, at tRateUs, ± rateSigma,
    // and only when `rateOk` (see below). `tUs`/`value` are the parallel arrays from
    // analysisDetail.series[i] (tUs ascending).
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
    // `peakSigma` / `rateSigma` are the σ of the winning window (the sample sd of its samples
    // about their mean; the standard error of the fitted slope), for the "± σ" the summary card
    // carries beside those two tiles — design §5.3.
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
    // formatValue = the number and its unit, for a surface with no header to lean on (legend
    // chips, the hover tooltip, the transit bead). Degrees keep the signed-deviation convention
    // ("+12°", closed up); every other unit takes a space ("75 mph", "12 %").
    Q_INVOKABLE QString formatValue(double v, const QString &unit) const;

    // formatBare = the number ALONE, for a surface whose container already names the unit — the
    // summary card's header, the split-mode gutter. Same sign convention as formatValue, so one
    // reading does not change shape depending on where it is shown.
    Q_INVOKABLE QString formatBare(double v, const QString &unit) const;

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
