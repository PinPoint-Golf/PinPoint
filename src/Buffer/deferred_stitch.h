/*
 * Copyright (C) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#pragma once

#include "types.h"
#include "imu_sample.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace pinpoint {

// One lane, stitched — deferred_sources_design.md §4.7.
//
// A deferred pull covers a span AROUND impact, not the whole window, so the
// retrieved block does not replace the live lane: it fills part of it. The
// result is one ascending, variable-rate trace — the shape the design calls
// [live prefix] + [high-rate span] + [live suffix].
//
// ⚠ THE MERGE RULE IS PER DELIVERED INTERVAL, NOT ONE SPAN, and that is a
// deliberate generalisation of the design's three-part picture. The device HOLES
// an over-wide request rather than clamping it — 33-58 % coverage with no error
// (spec §7.1) — so "the retrieved span" is in general several disjoint
// intervals with live-rate gaps between them. Taking deferred samples INSIDE the
// delivered intervals and live samples OUTSIDE them handles the clean pull and
// the holed one with the same code, and holed is the normal path.
//
// ⚠ Nothing is lost by preferring the deferred samples, PROVIDED history is a
// superset of live over the same span. That is not assumed here: the library
// measures it on every pull and reports live_overlap_samples /
// live_overlap_mismatches, which the caller records as a PAIR — a zero mismatch
// count beside a zero sample count is no evidence, not agreement.
struct DeferredStitchInput {
    // The live lane for this source as it sits in the frozen ring, ascending.
    std::vector<IndexEntry> liveEntries;
    // Payload access for a live entry; nullptr if the handle could not be read.
    std::function<const ImuSample *(const IndexEntry &)> liveSample;

    // The retrieved block, already converted and ascending. Same length.
    std::vector<int64_t>   deferredTUs;
    std::vector<ImuSample> deferredSamples;

    // Half-open [start, end) host-time intervals the pull actually delivered,
    // ascending and disjoint — wr_history_block::delivered.
    std::vector<std::pair<int64_t, int64_t>> delivered;
};

struct DeferredStitchResult {
    // Fresh sequence order: sequence i is samples[i] at tUs[i].
    std::vector<ImuSample> samples;
    std::vector<int64_t>   tUs;

    int usedLive             = 0;
    int usedDeferred         = 0;
    // ⚠ Samples dropped for not advancing the timestamp. The merger's
    // per-source monotonicity guarantee (event_buffer.cpp) does NOT apply here:
    // these bytes never went through the ring. A non-strictly-ascending lane
    // breaks the window's binary search silently, so it is enforced locally and
    // the count is surfaced rather than swallowed.
    int droppedNonMonotonic  = 0;
};

inline bool deferredCovers(const std::vector<std::pair<int64_t, int64_t>> &iv,
                           int64_t t) noexcept
{
    for (const auto &r : iv) {
        if (t >= r.first && t < r.second) return true;
        if (t < r.first) break;            // ascending — no later interval can match
    }
    return false;
}

inline DeferredStitchResult stitchDeferredLane(const DeferredStitchInput &in)
{
    DeferredStitchResult out;

    struct Row { int64_t t; const ImuSample *s; bool deferred; };
    std::vector<Row> rows;
    rows.reserve(in.liveEntries.size() + in.deferredSamples.size());

    // Live, but only where the pull did NOT deliver — inside a delivered
    // interval the deferred samples are the same measurement at a higher rate.
    for (const IndexEntry &e : in.liveEntries) {
        if (deferredCovers(in.delivered, e.timestamp_us)) continue;
        const ImuSample *s = in.liveSample ? in.liveSample(e) : nullptr;
        if (!s) continue;                  // unreadable handle — nothing to place
        rows.push_back(Row{ e.timestamp_us, s, false });
    }

    const size_t n = in.deferredTUs.size() < in.deferredSamples.size()
                   ? in.deferredTUs.size() : in.deferredSamples.size();
    for (size_t i = 0; i < n; ++i)
        rows.push_back(Row{ in.deferredTUs[i], &in.deferredSamples[i], true });

    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row &a, const Row &b) { return a.t < b.t; });

    out.samples.reserve(rows.size());
    out.tUs.reserve(rows.size());
    for (const Row &r : rows) {
        // ⚠ STRICTLY ascending. A duplicate timestamp is dropped rather than
        // kept: two samples at one instant make the bracketing pick arbitrary,
        // and a deferred sample and a live one can legitimately share an instant
        // at an interval edge.
        if (!out.tUs.empty() && r.t <= out.tUs.back()) {
            ++out.droppedNonMonotonic;
            continue;
        }
        out.tUs.push_back(r.t);
        out.samples.push_back(*r.s);
        if (r.deferred) ++out.usedDeferred; else ++out.usedLive;
    }
    return out;
}

} // namespace pinpoint
