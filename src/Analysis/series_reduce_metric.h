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

// series_reduce.h ⇄ MetricSeries, and NOTHING ELSE.
//
// The reducers are deliberately std-only — no Qt, no analysis types — so that a tool, a probe or a
// test can point one at any pair of arrays, and so that neither consumer drags the other's headers
// in through them. MetricSeries carries QString, so the one function that knows about it lives
// here: including series_reduce.h costs a consumer nothing, and a consumer that wants the
// convenience asks for it by name.
//
// Both production consumers build their own view — chart_metrics.cpp lifts a QVariantList and
// measure_sample.cpp a QJsonArray, neither of which is a MetricSeries by the time it reaches the
// reducers — so this exists for the in-process callers (tests, probes, and any future producer-side
// reduction) and to put the SHORT-MASK RULE in one place for them.

#include "series_reduce.h"
#include "swing_analysis.h"      // MetricSeries

#include <algorithm>

namespace pinpoint::analysis {

// The one view constructor that knows about MetricSeries — and the one place the short-mask rule is
// applied for it. `n` is the shorter of t_us / value, defensively: a curve whose two arrays disagree
// has only as many samples as both of them carry. A `valid` array shorter than `n` is discarded
// WHOLESALE rather than applied to the samples it does cover: guessing which end it was truncated
// from would invent validity we were never told about, and it is the same rule chart_metrics.cpp's
// haveMask() and measure_sample.cpp's `validArr.size() >= n` apply to their own inputs.
inline SeriesView viewOf(const MetricSeries &m)
{
    SeriesView s;
    s.n = std::min(m.t_us.size(), m.value.size());
    if (s.n == 0)
        return s;
    s.t = m.t_us.data();
    s.v = m.value.data();
    if (m.valid.size() >= s.n)
        s.valid = m.valid.data();      // empty (the common case) or short ⇒ stays null: all valid
    return s;
}

} // namespace pinpoint::analysis
