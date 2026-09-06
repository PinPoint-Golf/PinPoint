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
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

// Capture data-integrity check: did every camera lane in the frozen window
// deliver its frames on time?
//
// 2026-08-18 swing_0003 lost ~80 face-on frames starting 33 ms after impact —
// the host stalled for ~0.8 s and the backlog then arrived stamped ~1 ms apart
// (arrival times, not exposure times). Nothing told the user. The analysis
// still ran, its post-impact milestones were fabricated from the burst, and
// the shot counted in the session assessment like any other. This check is the
// verdict that makes such a recording visible: a ⚠ on the shot, a persisted
// "captureIntegrity" block in swing.json, and exclusion from session-level
// aggregation. It judges TIMESTAMPS only — it never says the frames' content is
// wrong, only that frames are missing between them.
//
// A hole is one inter-frame interval longer than `holePeriods` × the lane's
// median period (3 periods by default: real jitter on the 150 fps rig peaks at
// ~1.6 periods; a single dropped frame is 2). `impactUs` (window domain, −1 =
// unknown) splits holes into pre- and post-impact: a pre-impact hole means the
// swing itself has missing frames; a post-impact one means only the follow-
// through is affected (the analysis' phase model clips its candidacy at that
// hole — shaft_track_assembly captureHolePeriods — but the milestones after it
// are still not trustworthy).
//
// Header-only and free of Qt, mirroring imu_refusion_check.h, so the GUI join
// (ShotProcessor), the offline re-analysis and the tools all reach one verdict.

#include "swing_window.h"
#include "format_descriptor.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <variant>
#include <vector>

namespace pinpoint {

struct CaptureIntegrityVerdict {
    bool   ok             = true;   // no hole in any checked camera lane
    int    camerasChecked = 0;      // camera lanes with enough frames to judge
    int    holes          = 0;      // holes across all checked lanes
    int    framesLost     = 0;      // frames the holes swallowed (period-rounded)
    double worstHoleMs    = 0.0;    // the longest single hole
    double firstHoleUs    = -1.0;   // window-domain instant the first hole opens (−1 = none)
    bool   preImpact      = false;  // a hole opened at or before impact (or impact unknown)
    bool   postImpact     = false;  // a hole opened after impact
    double holePeriods    = 3.0;    // the boundary this verdict used

    // A data warning is raised only when a lane was actually checked and failed.
    bool warns() const { return camerasChecked > 0 && !ok; }
};

// Judge ONE lane from its frame timestamps (ascending, window domain). Lanes
// with fewer than 3 frames are not checkable (camerasChecked stays 0).
inline CaptureIntegrityVerdict captureIntegrityOf(const std::vector<int64_t> &tUs,
                                                  int64_t impactUs,
                                                  double holePeriods = 3.0)
{
    CaptureIntegrityVerdict v;
    v.holePeriods = holePeriods;
    if (tUs.size() < 3 || holePeriods <= 0.0) return v;

    std::vector<int64_t> dt;
    dt.reserve(tUs.size() - 1);
    for (size_t i = 1; i < tUs.size(); ++i) dt.push_back(tUs[i] - tUs[i - 1]);
    std::vector<int64_t> sorted = dt;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double periodUs = double(sorted[sorted.size() / 2]);
    if (periodUs <= 0.0) return v;
    const double holeUs = holePeriods * periodUs;

    v.camerasChecked = 1;
    for (size_t i = 0; i < dt.size(); ++i) {
        const double gap = double(dt[i]);
        if (gap <= holeUs) continue;
        ++v.holes;
        v.framesLost += std::max(0, int(std::lround(gap / periodUs)) - 1);
        v.worstHoleMs = std::max(v.worstHoleMs, gap * 1e-3);
        const int64_t opensAt = tUs[i];                // the last frame before the hole
        if (v.firstHoleUs < 0.0) v.firstHoleUs = double(opensAt);
        if (impactUs < 0 || opensAt <= impactUs) v.preImpact = true;
        else                                     v.postImpact = true;
    }
    v.ok = v.holes == 0;
    return v;
}

// Merge two lane verdicts into one shot-level verdict.
inline CaptureIntegrityVerdict mergeCaptureIntegrity(CaptureIntegrityVerdict a,
                                                     const CaptureIntegrityVerdict &b)
{
    a.camerasChecked += b.camerasChecked;
    a.holes          += b.holes;
    a.framesLost     += b.framesLost;
    a.worstHoleMs     = std::max(a.worstHoleMs, b.worstHoleMs);
    if (b.firstHoleUs >= 0.0 && (a.firstHoleUs < 0.0 || b.firstHoleUs < a.firstHoleUs))
        a.firstHoleUs = b.firstHoleUs;
    a.preImpact  = a.preImpact  || b.preImpact;
    a.postImpact = a.postImpact || b.postImpact;
    a.ok = a.ok && b.ok;
    return a;
}

// Judge every camera lane in the window. `impactUs` must be in the window's own
// domain (absolute buffer clock for a live window, window-relative for the
// offline loader's) — the same contract the analyzer's job.impactUs follows.
inline CaptureIntegrityVerdict checkCaptureIntegrity(const SwingWindow &window,
                                                     int64_t impactUs,
                                                     double holePeriods = 3.0)
{
    CaptureIntegrityVerdict v;
    v.holePeriods = holePeriods;
    std::vector<SourceId> cams;
    for (const IndexEntry &e : window.entries())
        if (std::holds_alternative<CameraFormat>(window.formatOf(e.source_id).format)
            && std::find(cams.begin(), cams.end(), e.source_id) == cams.end())
            cams.push_back(e.source_id);
    for (const SourceId sid : cams) {
        std::vector<int64_t> tUs;
        for (const IndexEntry &e : window.entriesFor(sid)) tUs.push_back(e.timestamp_us);
        v = mergeCaptureIntegrity(v, captureIntegrityOf(tUs, impactUs, holePeriods));
    }
    return v;
}

} // namespace pinpoint
