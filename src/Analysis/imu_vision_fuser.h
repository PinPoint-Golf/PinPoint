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

#include <QQuaternion>
#include <QVector3D>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"   // SegmentRole, ImuSegmentBinding
#include "../IMU/orientation_refuser.h"   // RefuseConfig (optional offline re-fusion)

namespace pinpoint { class SwingWindow; }

namespace pinpoint::analysis {

// One segment's anatomical-orientation stream, sampled on the shared TimeGrid.
struct SegmentStream {
    SegmentRole role = SegmentRole::Unknown;
    std::vector<QQuaternion> qAnat;        // q_anat = A·q_raw·M at each TimeGrid index
    // Measured inertials resampled on the same grid, rotated into the ANATOMICAL
    // segment frame (M⁻¹·v_sensor — the frame qAnat maps to world). So the
    // world-frame gyro is qAnat·gyro·qAnat⁻¹ and a signed axial rate is a plain
    // dot product against a constant segment axis. Units as stored in ImuSample:
    // °/s and g. Same length as qAnat (hold-last across momentary gaps).
    std::vector<QVector3D>   gyroDps;
    std::vector<QVector3D>   accelG;
};

// The orientation slice of the fusion layer: every bound IMU resampled onto one
// master timeline as anatomical quaternions. M1 Wrist is IMU-only — no camera
// fusion yet. Joint angles are relative quaternions between adjacent segments, so
// the shared (drifting) 6-axis yaw cancels and no per-shot re-zero is needed.
struct FusedStreams {
    std::vector<int64_t> timeGrid;          // ascending absolute t_us
    std::vector<SegmentStream> segments;    // one per bound IMU with a known role

    const SegmentStream *streamFor(SegmentRole r) const {
        for (const auto &s : segments)
            if (s.role == r) return &s;
        return nullptr;
    }
    bool has(SegmentRole r) const { return streamFor(r) != nullptr; }
};

class ImuVisionFuser {
public:
    // Build a fixed-rate master TimeGrid over the bound IMUs' common sample coverage
    // and, per binding, the anatomical quaternion stream q_anat(t) = A·q_raw(t)·M,
    // resampled (slerp) via SwingWindow::interpolateImu. Bindings whose role is
    // Unknown or that have < 2 in-window samples are skipped. Returns an empty grid
    // if nothing is fusable.
    //
    // q_raw(t) is normally the STORED live-fused quaternion. When `refusion` is non-null
    // (SwingLab filter.refuse — validation §5.3.1), orientation is instead RE-DERIVED offline
    // from each source's raw accel+gyro under that RefuseConfig (warm-started from the stored
    // quat), so the phase-adaptive filter actually drives the wrist metric. Null ⇒ the
    // production path, byte-identical to the stored quaternion.
    static FusedStreams fuse(const SwingWindow &window,
                             const std::vector<ImuSegmentBinding> &bindings,
                             double gridHz = 200.0,
                             const pinpoint::RefuseConfig *refusion = nullptr);

    // ── The grid rate must follow the data (design §4.3) ─────────────────────
    //
    // A fixed 200 Hz discards most of what a deferred high-rate pull cost us to
    // retrieve; raising it globally makes every ordinary capture pay 4x for
    // nothing. So it is derived per window.
    //
    // ⚠ THE STATISTIC IS THE MAXIMUM OVER THE SWING SPAN, NOT THE MEDIAN OVER
    // THE WINDOW, and the design's own wording was wrong about this. A stitched
    // lane runs at ~100 Hz over a 3 s still pre-roll and ~800 Hz through the
    // swing, so its median across the window is dominated by the pre-roll and
    // would land near the base rate — throwing away precisely the dense span the
    // pull was performed to obtain. The library makes the same point about its
    // own `density` field: the block-level figure is the wrong scope, point the
    // measurement at the sub-range you care about.
    //
    // ⚠ CLAMPED, AND THE FLOOR IS LOAD-BEARING. kGridHzMin is today's fixed
    // rate, so a capture whose fastest lane is a 100 Hz Witmotion comes out at
    // exactly 200 Hz and is bit-identical to what it was before this existed.
    // The floor never caps the device; it stops the corpus moving underneath us.
    static constexpr double  kGridHzMin = 200.0;
    static constexpr double  kGridHzMax = 800.0;
    // The sliding probe peakHzFor() uses. 250 ms is about a downswing, so a
    // dense span short enough to matter is still long enough to measure.
    static constexpr int64_t kProbeUs   = 250'000;

    static double gridHzForWindow(const SwingWindow &window,
                                  const std::vector<ImuSegmentBinding> &bindings);

    // The AVERAGE rate one source achieved over [startUs, endUs) — the figure a
    // stage gates on. 0 means NOT MEASURABLE, never "a rate of zero".
    static double effectiveHzFor(const SwingWindow &window, SourceId source,
                                 int64_t startUs, int64_t endUs);

    // The PEAK local rate anywhere in the window, over a sliding probe. This is
    // what sizes the grid: an average would be dragged down by the still
    // pre-roll and discard the dense span the pull was performed to obtain.
    static double peakHzFor(const SwingWindow &window, SourceId source,
                            int64_t probeUs);

    // The sub-span whose sample spacing implies a rate above thresholdHz —
    // "where the pull actually delivered density", in host time. out[0] >= out[1]
    // means there is none, which is the ordinary answer for a live-only lane.
    static void highRateSpanFor(const SwingWindow &window, SourceId source,
                                double thresholdHz, int64_t out[2]);
};

} // namespace pinpoint::analysis
