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

#include <algorithm>
#include <cstdint>
#include <utility>

#include "imu_vision_fuser.h"   // FusedStreams / SegmentStream

// ---------------------------------------------------------------------------
// trimStreams — restrict the fused streams to the detected swing span
// ---------------------------------------------------------------------------
//
// The metric grid spans the swing, not the raw 4 s ring. Timestamps stay
// absolute; only the number of samples changes.
//
// ⚠ IT LIVES IN ITS OWN HEADER BECAUSE THE BUG IT CARRIES IS A FIELD-COPY BUG,
// AND THAT KIND RECURS. This was a static helper inside wrist_analyzer.cpp's
// anonymous namespace, which put it out of reach of every test in the tree. Then
// `SegmentStream::hackMotion` was added, this function was not updated, and a
// HackMotion swing came out with the binding right, the fuser right, the
// conjugate applied — and the metric keyed as a Witmotion, because provenance
// fell off HERE, between them. Nothing downstream could see it: the curve was
// correct, only its name was wrong.
//
// The defect is not "someone forgot hackMotion". It is that a field-by-field copy
// silently defaults every field its author did not know about, so EVERY future
// addition to SegmentStream inherits the same failure with no diff to review. The
// header exists so stream_trim_test can hold a fully-populated stream against its
// trimmed self and fail the day a new field is added and not carried — see the
// note on kNonSampleFields there.
//
// Header-only and free of the analyzer: it needs the two stream structs and
// nothing else, so the test compiles it without pulling pose, ball, club or
// segmentation in behind it.

namespace pinpoint::analysis {

// Copy of the fused streams restricted to [fromUs, toUs].
inline FusedStreams trimStreams(const FusedStreams &in, int64_t fromUs, int64_t toUs)
{
    const auto lo = std::lower_bound(in.timeGrid.begin(), in.timeGrid.end(), fromUs);
    const auto hi = std::upper_bound(in.timeGrid.begin(), in.timeGrid.end(), toUs);
    const size_t a = size_t(lo - in.timeGrid.begin());
    const size_t b = size_t(hi - in.timeGrid.begin());
    if (a >= b)
        return in;   // degenerate bounds — keep the full grid
    FusedStreams out;
    out.timeGrid.assign(in.timeGrid.begin() + long(a), in.timeGrid.begin() + long(b));
    for (const SegmentStream &s : in.segments) {
        if (s.qAnat.size() != in.timeGrid.size())
            continue;   // malformed stream — drop rather than misalign
        SegmentStream t;
        // ⚠ COPY EVERY NON-SAMPLE FIELD, not just the ones this function was written
        // with. Trimming is a window operation: it changes how many samples a stream
        // carries and nothing else about it. `hackMotion` was silently dropped here
        // when it was added — see the header note above, and add the field to
        // stream_trim_test's populated fixture at the same time as adding it here.
        t.role       = s.role;
        t.hackMotion = s.hackMotion;
        t.qAnat.assign(s.qAnat.begin() + long(a), s.qAnat.begin() + long(b));
        if (s.gyroDps.size() == in.timeGrid.size())
            t.gyroDps.assign(s.gyroDps.begin() + long(a), s.gyroDps.begin() + long(b));
        if (s.accelG.size() == in.timeGrid.size())
            t.accelG.assign(s.accelG.begin() + long(a), s.accelG.begin() + long(b));
        out.segments.push_back(std::move(t));
    }
    return out;
}

} // namespace pinpoint::analysis
