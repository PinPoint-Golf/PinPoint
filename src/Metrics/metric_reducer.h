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

#include "swing_analysis.h"   // Phase

#include <QString>

#include <optional>

// MetricReducer — how a continuous curve becomes the single number a corridor can grade.
//
// This vocabulary already existed informally: metric_type.h describes MetricType::TimeSeries as
// "a continuous curve, reduced to peak / @impact / Δ / rate by the chart layer". It lived only as
// prose and as ad-hoc reduction code inside the chart layer. The Diagnostics measure model needs
// exactly the same vocabulary, so it is promoted here rather than defined a second time — one
// spelling, one set of semantics, both consumers.
//
// The distinction this type exists to enforce: a *series* is `what · quantity · reference` and is
// what a producer produces; a *reducer* is how that series is sampled. Conflating them (a single
// "when" facet) makes one producer look like four separate roadmap items, because "spine angle at
// P4" and "spine angle change P1→P4" read as unrelated measures when they are one curve sampled
// two ways.

namespace pinpoint::analysis {

// The four reductions. Anything expressible over a phase window belongs here, not on the signal.
enum class ReducerKind {
    At,        // value at a single phase
    Delta,     // value(window.second) − value(window.first)
    Rate,      // Delta divided by elapsed time across the window
    Extremum,  // max or min across the window; optionally the peak *deviation* from an anchor
};

// Which end of an Extremum is wanted. Ignored for every other kind.
enum class ExtremumSense { Max, Min };

// A reduction of one series to one number.
//
// `anchor` carries the address-relative semantics that an `AddressValue` *reference* used to fake.
// Address-relative is a time relationship, not a spatial one — "spine angle relative to its own
// value at P1" is the same series sampled twice, so it belongs here and not in the series tuple.
//
//   At:       anchor = the phase to read.                     window unused.
//   Delta:    anchor = start phase, window.second = end phase.
//   Rate:     as Delta, divided by elapsed time.
//   Extremum: window = the search span. anchor, when present, makes the result the peak
//             |value − value(anchor)| rather than the peak absolute value.
//
// Extremum is deliberately first-class rather than deferred: several real characteristics are peaks
// wearing an endpoint's clothes. Early extension is the *maximum* pelvis-toward-ball displacement
// across the downswing, not its value at impact — sampling At(Impact) misses a golfer who moves and
// recovers before the sample. Extremum also yields *when* the peak occurred, which is a second
// signal worth having (see ReducedValue::atUs).
struct Reducer {
    ReducerKind                  kind   = ReducerKind::At;
    std::optional<Phase>         anchor;                       // see per-kind semantics above
    std::pair<Phase, Phase>      window{Phase::Address, Phase::Impact};
    ExtremumSense                sense  = ExtremumSense::Max;   // Extremum only
};

// The outcome of applying a Reducer to a series. `valid == false` means the reduction could not be
// performed (a phase was not segmented, the series had no samples in the window) — which is
// distinct from a value of zero, and callers must not conflate them. An unavailable measure has to
// resolve as unavailable, never as a passing test.
struct ReducedValue {
    double  value = 0.0;
    int64_t atUs  = 0;       // Extremum: when the peak occurred. Other kinds: the sampled phase.
    bool    valid = false;
};

// Stable lower-camel string names — the QML-boundary and JSON spelling.
inline QString reducerKindName(ReducerKind k)
{
    switch (k) {
    case ReducerKind::At:       return QStringLiteral("at");
    case ReducerKind::Delta:    return QStringLiteral("delta");
    case ReducerKind::Rate:     return QStringLiteral("rate");
    case ReducerKind::Extremum: return QStringLiteral("extremum");
    }
    return QStringLiteral("at");
}

// Inverse of reducerKindName() for parsing a pack file. Returns nullopt for an unknown name, so a
// malformed pack is rejected by the validator rather than silently defaulting to At — a wrong
// reducer is a wrong measure, not a cosmetic difference.
inline std::optional<ReducerKind> reducerKindFromName(const QString &name)
{
    if (name == QLatin1String("at"))       return ReducerKind::At;
    if (name == QLatin1String("delta"))    return ReducerKind::Delta;
    if (name == QLatin1String("rate"))     return ReducerKind::Rate;
    if (name == QLatin1String("extremum")) return ReducerKind::Extremum;
    return std::nullopt;
}

inline QString extremumSenseName(ExtremumSense s)
{
    return s == ExtremumSense::Min ? QStringLiteral("min") : QStringLiteral("max");
}

inline std::optional<ExtremumSense> extremumSenseFromName(const QString &name)
{
    if (name == QLatin1String("max")) return ExtremumSense::Max;
    if (name == QLatin1String("min")) return ExtremumSense::Min;
    return std::nullopt;
}

// True when the reducer reads a span rather than an instant. Drives both the canonical label and
// the engine's sample-gathering.
inline bool reducerUsesWindow(ReducerKind k)
{
    return k != ReducerKind::At;
}

} // namespace pinpoint::analysis
