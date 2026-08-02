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

// The sparse-channel → MetricSeries plumbing every face-on producer repeats.
//
// head_track.cpp, foot_metrics.cpp and lower_body_metrics.cpp each carry their own private copy of
// medianOf / interpChannel / phaseTime / nearestIndex, written three times to the same contract
// ("linear interp, hold at the ends, bridge gaps, NEVER NaN"). A fourth, fifth, sixth and seventh
// copy landed with the face-on producer batch, so the contract is promoted here instead — one
// spelling, one set of semantics, every consumer.
//
// The three older producers are deliberately NOT migrated in the same change: they are corpus-gated
// and byte-compared, and a refactor that touches them would have to re-gate them to prove a
// no-op. They should move here the next time one of them is opened for a real reason.
//
// Pure, header-only, Qt-only (no OpenCV / no Qt-GUI). Everything is deterministic and testable
// without video.

#include "swing_analysis.h"   // MetricSeries, PhaseEvent, Phase

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pinpoint::analysis {

// One sparse channel: the valid subset of frames, ascending in t_us. A frame whose geometry could
// not be resolved is simply ABSENT — never a sentinel and never a zero, because a channel that
// substitutes zero for "not measured" grades as a confident reading of nothing.
struct MetricChannel {
    std::vector<int64_t> t_us;
    std::vector<double>  value;

    void push(int64_t t, double v) { t_us.push_back(t); value.push_back(v); }
    bool empty() const { return t_us.empty(); }
    size_t size() const { return t_us.size(); }
};

// Median of a copy. Order-independent by construction, which is what makes it safe against a
// detector warm-up mis-lock on the opening frames (see ball_position.h's pass-1 note).
inline double medianOfCopy(std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return (n & 1u) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// Linear interpolation of an ascending sparse channel at t: hold at the ends, bridge gaps, never
// NaN. A low-confidence run coasts across rather than dropping the metric out.
inline double interpChannel(const std::vector<int64_t> &xs, const std::vector<double> &ys, int64_t x)
{
    if (xs.empty()) return 0.0;                     // guarded by the caller (channel non-empty)
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back())  return ys.back();
    const auto it = std::lower_bound(xs.begin(), xs.end(), x);
    const size_t hi = size_t(it - xs.begin());
    const size_t lo = hi - 1;
    const int64_t span = xs[hi] - xs[lo];
    if (span <= 0) return ys[lo];                   // coincident samples (defensive)
    const double f = double(x - xs[lo]) / double(span);
    return ys[lo] + (ys[hi] - ys[lo]) * f;
}

// The instant of a phase in the ladder, or `fallback` when the segmenter never found it. Callers
// pass the grid front so an unsegmented phase reads the first frame rather than time zero.
inline int64_t phaseTime(const std::vector<PhaseEvent> &phases, Phase p, int64_t fallback)
{
    for (const PhaseEvent &e : phases)
        if (e.phase == p) return e.t_us;
    return fallback;
}

// Index of the grid sample nearest t. The grid is the full per-frame timeline, so this is the frame
// a phase instant lands on.
inline int nearestIndex(const std::vector<int64_t> &grid, int64_t t)
{
    if (grid.empty()) return 0;
    if (t <= grid.front()) return 0;
    if (t >= grid.back())  return int(grid.size()) - 1;
    const auto it = std::lower_bound(grid.begin(), grid.end(), t);
    const int hi = int(it - grid.begin());
    const int lo = hi - 1;
    return (t - grid[lo] <= grid[hi] - t) ? lo : hi;
}

// The phases every face-on curve carries as `phaseSamples`.
//
// The REDUCTION does not depend on this list — measure_sample.h samples the curve itself at each
// segmented phase, so a measure reading P5 works whether or not P5 appears here. phaseSamples exist
// for the chart layer and for the one class of metric that has no curve at all (the setup scalars).
// Four is the useful set: the three the older producers already emit, plus the finish, which the
// balance and thorax-rotation measures read and which nothing sampled before.
inline const std::vector<Phase> &defaultPhaseSamples()
{
    static const std::vector<Phase> kPhases{ Phase::Address, Phase::Top, Phase::Impact,
                                            Phase::Finish };
    return kPhases;
}

// Resample one sparse channel onto the full frame grid and wrap it as a MetricSeries.
//
// UNSCORED, always: the corridor that judges this number lives in the diagnostics norm set and is
// resolved in the shot's own context (Diagnostics/metric_corridor.h). A producer that also carried
// a band would be a second, unversioned opinion about what is normal.
//
// Returns a series with an empty key when the channel is empty, which the caller drops. That is the
// refuse-don't-fabricate contract: no samples means no metric, not a metric that is zero.
inline MetricSeries buildChannelSeries(const std::vector<int64_t> &grid, const MetricChannel &ch,
                                       const QString &key, const QString &label, const QString &unit,
                                       const std::vector<PhaseEvent> &phases,
                                       const std::vector<Phase> &sampleAt = defaultPhaseSamples())
{
    MetricSeries m;
    if (ch.empty() || grid.empty())
        return m;

    m.key   = key;
    m.label = label;
    m.unit  = unit;
    m.t_us  = grid;
    m.value.resize(grid.size());
    for (size_t i = 0; i < grid.size(); ++i)
        m.value[i] = interpChannel(ch.t_us, ch.value, grid[i]);

    for (const Phase p : sampleAt) {
        const int idx = nearestIndex(grid, phaseTime(phases, p, grid.front()));
        m.phaseSamples.push_back({ p, grid[idx], m.value[size_t(idx)], QString() });
    }
    return m;
}

// Append `s` to `out` unless it refused (empty key). Keeps every producer's emit block to one line
// per series and makes the refusal path impossible to forget.
inline void appendIfProduced(std::vector<MetricSeries> &out, MetricSeries &&s)
{
    if (!s.key.isEmpty())
        out.push_back(std::move(s));
}

} // namespace pinpoint::analysis
