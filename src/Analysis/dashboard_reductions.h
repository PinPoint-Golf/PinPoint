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

// Dashboard band-rail reductions — pure, header-only, no Qt-GUI. The maths behind
// PpBandRail: joining a metric's measured phase samples to the catalogue's normative
// corridors, deciding the value→y domain, and interpolating the curve at the replay
// playhead. Sibling of kinematic_sequence.h and kept out of QML JS by the same rule
// (analysis pipeline guide §6.2): QML positions and paints, C++ decides the numbers.
//
// The join is BY PHASE, not by index — a metric may be sampled at phases the
// catalogue has no corridor for (and vice versa), and a partially-corridored metric
// must still render its measured checkpoints. Nothing here fabricates a sample: a
// phase with no measurement is simply absent from the output.
//
// The QML façade is ChartMetrics::railCheckpoints / railRange / valueAtUs.
// Unit-tested standalone in src/Analysis/tests/dashboard_reductions_test.cpp.

#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace pinpoint::analysis {

// One measured checkpoint of a metric — a MetricSeries::phaseSamples entry.
struct RailSample {
    int     phase = -1;
    int64_t tUs   = 0;
    double  value = 0.0;
    QString band;            // "green" | "yellow" | "red" | "" (unscored)
};

// One phase's normative corridor, flattened — the shape MetricCatalog marshals out of the norm set
// (Diagnostics/metric_corridor.h) and QML hands back in.
struct RailCorridor {
    int    phase   = -1;
    double greenLo = 0.0, greenHi = 0.0;
    double amberLo = 0.0, amberHi = 0.0;
};

// A checkpoint ready to paint: the measurement plus whatever corridor applies to it.
struct RailPoint {
    int     phase = -1;
    int64_t tUs   = 0;
    double  value = 0.0;
    QString band;
    bool    hasCorridor = false;
    double  greenLo = 0.0, greenHi = 0.0;
    double  amberLo = 0.0, amberHi = 0.0;
};

// The value→y domain of a rail.
struct RailRange {
    double lo    = 0.0;
    double hi    = 0.0;
    bool   valid = false;
};

// Corridor for `phase`, or nullptr. Linear scan — a metric has a handful of phases.
inline const RailCorridor *railCorridorAt(const std::vector<RailCorridor> &corridors, int phase)
{
    for (const RailCorridor &c : corridors)
        if (c.phase == phase) return &c;
    return nullptr;
}

// Join measured samples to normative corridors BY PHASE, ordered by time.
//
// Every measured sample survives; the corridor is attached only where one exists for
// that phase (hasCorridor false otherwise, so the caller draws a bare checkpoint
// rather than a fabricated ribbon). Corridors with no matching sample are dropped —
// a ribbon with no player dot would imply a measurement that was never taken.
inline std::vector<RailPoint> railCheckpoints(const std::vector<RailSample>   &samples,
                                              const std::vector<RailCorridor> &corridors)
{
    std::vector<RailPoint> out;
    out.reserve(samples.size());

    for (const RailSample &s : samples) {
        if (s.phase < 0) continue;
        RailPoint p;
        p.phase = s.phase;
        p.tUs   = s.tUs;
        p.value = s.value;
        p.band  = s.band;
        if (const RailCorridor *c = railCorridorAt(corridors, s.phase)) {
            p.hasCorridor = true;
            p.greenLo = c->greenLo; p.greenHi = c->greenHi;
            p.amberLo = c->amberLo; p.amberHi = c->amberHi;
        }
        out.push_back(p);
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const RailPoint &a, const RailPoint &b) { return a.tUs < b.tUs; });
    return out;
}

// The value→y domain covering every player dot, every corridor bound in play, and the
// 0 reference line the rail always draws — padded by `padFrac` of the raw span.
//
// `oneSided` is the speeds case: the corridor is a FLOOR/target, whose upper bound is
// either absent or an aspirational number far above anything an athlete produced.
// Including it would crush the actual trace into the bottom of the tile, so only the
// lower bounds participate in the domain there.
//
// Returns valid=false for an empty rail. A degenerate span (every value identical)
// widens to ±1 so the caller never divides by zero.
inline RailRange railRange(const std::vector<RailPoint> &points, bool oneSided,
                           double padFrac = 0.08)
{
    RailRange r;
    if (points.empty()) return r;

    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    auto take = [&lo, &hi](double v) {
        if (!std::isfinite(v)) return;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };

    take(0.0);                                  // the rail always shows its 0 reference
    for (const RailPoint &p : points) {
        take(p.value);
        if (!p.hasCorridor) continue;
        take(p.amberLo);
        take(p.greenLo);
        if (!oneSided) {                        // see note above
            take(p.amberHi);
            take(p.greenHi);
        }
    }
    if (lo > hi) return r;                      // nothing finite at all

    double span = hi - lo;
    if (span <= 0.0) { lo -= 1.0; hi += 1.0; span = hi - lo; }
    const double pad = span * padFrac;

    r.lo    = lo - pad;
    r.hi    = hi + pad;
    r.valid = true;
    return r;
}

// The curve's value at `us`, linearly interpolated between bracketing samples.
//
// Clamps to the endpoints outside the sampled span (the playhead legitimately sits in
// the pre-roll / follow-through, where holding the nearest value beats extrapolating
// a number the swing never produced). NaN for an empty curve — the caller then keeps
// its resting headline rather than printing a placeholder.
inline double interpolateAtUs(const std::vector<int64_t> &tUs,
                              const std::vector<double>  &value, int64_t us)
{
    const int n = int(std::min(tUs.size(), value.size()));
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    if (n == 1 || us <= tUs[0])     return value[0];
    if (us >= tUs[n - 1])           return value[n - 1];

    // First sample at or after `us`; tUs is ascending by the MetricSeries contract.
    const auto it = std::lower_bound(tUs.begin(), tUs.begin() + n, us);
    const int  hiIdx = int(it - tUs.begin());
    if (hiIdx == 0) return value[0];
    const int  loIdx = hiIdx - 1;

    const int64_t dt = tUs[hiIdx] - tUs[loIdx];
    if (dt <= 0) return value[hiIdx];           // duplicate stamps: take the later sample
    const double f = double(us - tUs[loIdx]) / double(dt);
    return value[loIdx] + f * (value[hiIdx] - value[loIdx]);
}

// One score-contribution bucket (a perRegion / perPhase entry).
struct ScoreSegment {
    QString label;
    int     value = 0;
};

// Score buckets ordered for the Verdict donut's hover breakdown: WEAKEST FIRST, so
// the thing costing the athlete the most sits where they read first. Ties break on
// label, because a breakdown that reshuffles between two swings with equal scores
// is unreadable — stable order is what makes it comparable shot to shot.
inline std::vector<ScoreSegment> orderScoreSegments(std::vector<ScoreSegment> segs)
{
    std::stable_sort(segs.begin(), segs.end(),
                     [](const ScoreSegment &a, const ScoreSegment &b) {
                         if (a.value != b.value) return a.value < b.value;
                         return a.label < b.label;
                     });
    return segs;
}

// Categorical read of a signed ALIGNMENT angle against its green corridor, for the
// Setup zone's orientation glyph. The alignment metrics (shoulders / hips / feet /
// toe line) are stored as signed degrees, but the coaching read is a WORD — and a
// word is what the glyph draws, so the mapping has to be decided somewhere testable
// rather than inside a QML ternary.
//
// Sign convention follows the manifest: positive = open (aiming left of target for a
// right-hander), negative = closed. Inside the green corridor the athlete is square,
// regardless of which side of zero the corridor sits on — a corridor that is not
// centred on zero is deliberate (a touch of hip-open at address is correct), and
// calling that "open" would flag a good setup as a fault.
//
// Returns "square" | "open" | "closed", or "" when the corridor is degenerate (no
// corridor ⇒ nothing to be square against, and the caller falls back to a bar).
inline QString orientationLabel(double value, double greenLo, double greenHi)
{
    if (!(greenHi > greenLo)) return QString();
    if (value > greenHi) return QStringLiteral("open");
    if (value < greenLo) return QStringLiteral("closed");
    return QStringLiteral("square");
}

} // namespace pinpoint::analysis
