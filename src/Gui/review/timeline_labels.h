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
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// TimelineLabels — the single home for all the iterative timeline maths that the
// "no JavaScript logic in QML" rule keeps out of PpTransitTimeline / PpMetricChart.
// Stateless: every method is const and depends only on its arguments, so one shared
// instance can be declared declaratively (TimelineLabels { id: solver }) and reused.
//
// The 1-D label distribution solver (distribute) is the train-map technique from the
// design mockup: stations sit at their true proportional position on the line; upright
// labels are nudged ALONG the line (the main axis) only as far as needed to clear their
// neighbours, with an elbow connector back to the dot. The same solver serves both
// orientations — horizontal distributes along X, vertical along Y.
class TimelineLabels : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit TimelineLabels(QObject *parent = nullptr) : QObject(parent) {}

    // 1-D non-overlap solver. Returns new centre coordinates (doubles) such that each
    // item keeps a half-size + `gap` clearance from its neighbours and stays within
    // [lo, hi] (centre ± size/2). Two passes: left→right push, then right→left clamp —
    // identical to the reference algorithm in the mockup. `centers` and `sizes` are
    // parallel; the result is the same length as `centers`.
    Q_INVOKABLE QVariantList distribute(const QVariantList &centers, const QVariantList &sizes,
                                        double gap, double lo, double hi) const;

    // Compose the full render model for one orientation in a single call so the QML can
    // stay declarative (it just binds a Repeater to the result). `phases` is
    // shotReplay.analysisDetail.phases ([{phase,t_us,conf}]). For each phase, in input
    // order, returns a map:
    //   { phase:int, tUs, frac (0..1 clamped), center (frac*mainLength),
    //     label (distributed main-axis position), name (full word), isImpact, elbow }
    // `horizontal` chooses the per-label size used by the solver: measured text width
    // (horizontal) vs a uniform line height (vertical). `mainLength` is the usable line
    // length in px (insets excluded); the caller adds its own inset uniformly.
    Q_INVOKABLE QVariantList stationLayout(const QVariantList &phases, qint64 startUs,
                                           qint64 endUs, bool horizontal, double mainLength,
                                           double gap, const QString &fontFamily,
                                           double fontPx) const;

    // Same idea as stationLayout but for the named coaching P1..P8 positions
    // (shotReplay.analysisDetail.club.positions — the fused TrackSample/MilestoneFit
    // track, each {p, t_us, grip, head, theta, lenPx, conf, sigmaThetaDeg, sigmaLenPx,
    // stackN, source}). `t_us` must already be in the SAME domain as `startUs`/`endUs`
    // (both the live and disk-replay facades re-time positions into window-relative µs,
    // matching phases/samples — see disk_replay_source.cpp relTimedMap).
    // Entries with `p` outside 1..8 or 10 (Finish; P9 reserved/deferred) are dropped;
    // the rest are sorted by t_us (milestone fits are not guaranteed to arrive in
    // order) before solving.
    // For each surviving entry, in time order, returns a map:
    //   { p:int, tUs, frac (0..1 clamped), center (distribute()-solved main-axis
    //     position — clustered downswing positions, e.g. P5/P6/P7, can sit within ~1%
    //     of the span of each other, so this is never the raw frac*mainLength), label
    //     ("P1".."P8", "P10"), source (0 TrackSample / 1 MilestoneFit), conf, elbow (true
    //     when the solved position was nudged >1.5px off its raw frac*mainLength spot) }.
    Q_INVOKABLE QVariantList positionLayout(const QVariantList &positions, qint64 startUs,
                                            qint64 endUs, bool horizontal, double mainLength,
                                            double gap, const QString &fontFamily,
                                            int fontPx) const;

    // Greatest station index whose t_us <= pos (the station "bracketing" the playhead),
    // or -1 when pos precedes the first station / the list is empty.
    Q_INVOKABLE int activeStation(const QVariantList &phases, qint64 pos) const;

    // Nearest-sample lookup for the readout chip. `tUs` / `value` are the parallel
    // arrays from analysisDetail.series[i]; returns value at the sample nearest `pos`
    // (0 when empty / mismatched).
    Q_INVOKABLE double valueAtNearest(const QVariantList &tUs, const QVariantList &value,
                                      qint64 pos) const;

    // Snap-to-phase: if a station t_us lies within `tolUs` of `us`, return that station's
    // t_us (nearest wins); otherwise return `us` unchanged.
    Q_INVOKABLE qint64 snap(const QVariantList &phases, qint64 us, qint64 tolUs) const;

    // Single source of truth for phase names (indices match Analysis Phase enum:
    // 0 Address … 7 Finish, 8 MidBackswing, 9 Delivery, 10 MaxSpeed, 11 FollowThrough,
    // 12 ShaftParallelBack, 13 ArmParallelDown, 14 ShaftParallelThrough).
    Q_INVOKABLE QString phaseFullName(int phase) const;   // "Address" … "Shaft-parallel through"
    Q_INVOKABLE QString phaseShortTag(int phase) const;   // "ADR" … "SPT"
};
