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

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// ChartMetrics — the C++ home for the derivation maths that the "no JavaScript logic
// in QML" rule keeps out of PpMetricChart, exactly the TimelineLabels shape. Stateless:
// every method is const and depends only on its arguments, so one shared instance can be
// declared declaratively (ChartMetrics { id: metrics }) and reused by every chart.
//
// Phase names/tags are NOT duplicated here — the chart composes segment labels in QML from
// phaseA/phaseB via TimelineLabels.phaseShortTag, and the crosshair value-at-cursor reuses
// TimelineLabels.valueAtNearest. This class owns only the segment vocabulary, the
// per-window summary statistics, and the metric-key → short-name map.
class ChartMetrics : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit ChartMetrics(QObject *parent = nullptr) : QObject(parent) {}

    // Segment list for a swing. [0] = Full ({startUs:0, endUs:spanUs, phaseA:-1, phaseB:-1});
    // then one entry per adjacent phase pair, ordered by time:
    //   { startUs, endUs, phaseA:int, phaseB:int }.
    // The label is composed in QML from phaseA/phaseB via TimelineLabels.phaseShortTag (no
    // tag strings duplicated here). Mirrors swing_data_source.cpp segment logic so the
    // segment vocabulary is identical. `phases` is analysisDetail.phases ([{phase,t_us,…}]).
    Q_INVOKABLE QVariantList segments(const QVariantList &phases, qint64 spanUs) const;

    // Per-metric summary over [startUs, endUs], window edges linearly interpolated:
    //   { start, end, min, max, peak, range, delta, rate, tPeakUs }
    // peak = the extremum of larger magnitude; delta = end-start; range = max-min;
    // rate = max |Δvalue/Δt| between consecutive in-window samples, in deg per 100 ms;
    // tPeakUs = the time at which peak occurs. `tUs`/`value` are the parallel arrays from
    // analysisDetail.series[i] (tUs ascending).
    Q_INVOKABLE QVariantMap summary(const QVariantList &tUs, const QVariantList &value,
                                    qint64 startUs, qint64 endUs) const;

    // Compact display name for a metric key (e.g. "leadWristFlexExt" → "Bow/cup"), or ""
    // when the key has no short form — the caller then falls back to series.label. Single
    // source of truth for metric short names, mirroring TimelineLabels::phaseShortTag.
    Q_INVOKABLE QString shortLabel(const QString &key) const;

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

    // Band ("good"/"attention"/"warn") of the phaseSample nearest `us` (default "good").
    // Used to tint a summary card's @end value by the swing's state at the window edge.
    Q_INVOKABLE QString bandAtNearest(const QVariantList &phaseSamples, qint64 us) const;

    // Ordered kinematic-sequence nodes for the dashboard Sequence zone. `series` is
    // analysisDetail.series ([{key,t_us,value,…}]); `keys` selects which series to
    // include (e.g. ["handSpeed","clubheadSpeed"]). Returns one map per series that
    // has data — { key, tPeakUs, peak, gapMs, order } — ordered by peak time, gapMs =
    // ms since the previous node. Series absent from `series` (or empty) are dropped,
    // so a partially-instrumented capture degrades to the measurable nodes only.
    // The reduction itself is the pure kinematicSequenceNodes() (kinematic_sequence.h).
    Q_INVOKABLE QVariantList sequenceNodes(const QVariantList &series,
                                           const QStringList &keys) const;

    // ── PpBandRail backing (dashboard_reductions.h) ─────────────────────────────
    // These three keep the band rail's arithmetic out of QML: the component
    // positions and paints, C++ decides every number it paints.

    // Measured checkpoints joined to their normative corridors, ordered by time.
    // `phaseSamples` is series.phaseSamples ([{phase,t_us,value,band}]); `corridors`
    // is descriptor().normative.corridors ([{phase,greenLo,greenHi,amberLo,amberHi}]).
    // Returns one map per MEASURED sample — { phase, tUs, value, band, hasCorridor,
    // greenLo, greenHi, amberLo, amberHi } — with hasCorridor false where the
    // catalogue has no corridor for that phase (the rail then draws a bare
    // checkpoint). A corridor with no matching sample is dropped: a ribbon with no
    // player dot would imply a measurement that was never taken.
    Q_INVOKABLE QVariantList railCheckpoints(const QVariantList &phaseSamples,
                                             const QVariantList &corridors) const;

    // The rail's value→y domain over `points` (the railCheckpoints result):
    //   { lo, hi, valid }
    // Spans every dot, every corridor bound in play, and the 0 reference line the
    // rail always draws, padded by 8%. valid=false for an empty rail.
    //
    // No one-sidedness parameter: a corridor's open edge is `mu`, the aspiration, which
    // belongs in the domain rather than being dropped from it. Openness travels ON the
    // points and changes only how the ribbon is painted. See railRange() in
    // dashboard_reductions.h for why the old parameter existed and why it does not now.
    Q_INVOKABLE QVariantMap railRange(const QVariantList &points) const;

    // The curve's value at `us`, linearly interpolated between bracketing samples and
    // clamped to the endpoints outside the sampled span (the playhead legitimately
    // sits in the pre-roll / follow-through). Drives the "@ PLAYHEAD" headline.
    // Returns NaN for an empty curve — QML's isNaN() then keeps the resting headline.
    Q_INVOKABLE double valueAtUs(const QVariantList &tUs, const QVariantList &value,
                                 qint64 us) const;

    // ── Corridor-bar backing (dashboard_reductions.h) ───────────────────────────

    // The value→x domain of ONE corridor bar (NormativeBar, PpRangeBar):
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

    // Score contribution buckets (analysisDetail.perRegion / .perPhase — a
    // {name → 0..100} map) as an ORDERED list for the Verdict donut's hover
    // breakdown: [{ label, value }], weakest first, so the thing costing the most
    // sits at the top where the athlete reads first. Empty list for an empty map
    // (a resemblance score has no buckets) — the donut then renders without
    // segments rather than showing an empty panel.
    Q_INVOKABLE QVariantList scoreSegments(const QVariantMap &buckets) const;

    // Categorical read of a signed alignment angle against its green corridor:
    // "square" | "open" | "closed", or "" when the corridor is degenerate. Drives
    // the Setup zone's PpOrientationGlyph; inside the corridor is always square,
    // even when the corridor is deliberately not centred on zero.
    Q_INVOKABLE QString orientationLabel(double value, double greenLo, double greenHi) const;
};
