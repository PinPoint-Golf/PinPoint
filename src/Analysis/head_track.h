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

// Head tracking (WB2, wholebody_pose_design.md). Head position is a first-class
// golf metric — a stable head anchors the swing centre. This module turns the
// (smoothed, else raw) face-on PoseTrack2D into per-frame head state and three
// Address-referenced traces: sway (lateral), lift (vertical), tilt (eye-line
// angle). Pure, deterministic, Qt-only (no OpenCV / ORT) — same cv-free contract
// as swing_analysis.h; unit-tested standalone.
//
// It reads only the COCO body head keypoints (nose=0, LEye=1, REye=2, LEar=3,
// REar=4) so it works even when the WB1 face channels are absent/noisy; the chin
// (kFaceFirstKp+8 = 31) optionally refines the centre when its confidence clears
// the gate (OFF by default, head.chinConfWeight).
//
// ANISOTROPY: kp are normalized by frame WIDTH and HEIGHT separately, so a
// naive delta mixes two scales. All geometry (centroid, inter-ear scale, tilt
// angle) is computed in PIXELS via (frameW, frameH) — the same de-normalization
// ShaftTracker / pose_smoother use — then sway/lift are re-normalized by a SINGLE
// reference dimension (frame width), so both channels are isotropic "×frame"
// units. buildHeadSeries scales to cm ONLY when the caller supplies a clean
// px-per-cm factor.

#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // PoseTrack2D, PoseFrame2D, MetricSeries, PhaseEvent, kFaceFirstKp
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::head::

namespace pinpoint::analysis {

// Head-tracking knobs. Defaults track the frozen constants (pp_tuned_constants.h
// head::); SwingLab sweeps them via "head.*" dotted keys.
struct HeadTrackConfig {
    double  confMin        = tuned::head::kConfMin;        // head.confMin
    double  earIpdFactor   = tuned::head::kEarIpdFactor;   // head.earIpdFactor
    double  earWidthMm     = tuned::head::kEarWidthMm;     // head.earWidthMm (inter-ear px→mm ruler)
    double  chinConfWeight = tuned::head::kChinConfWeight; // head.chinConfWeight (0 ⇒ body 0–4 only)
    int     minContribPts  = tuned::head::kMinContribPts;  // head.minContribPts
    int     addrMinFrames  = tuned::head::kAddrMinFrames;  // head.addrMinFrames
    int64_t addrWindowUs   = tuned::head::kAddrWindowUs;   // head.addrWindowUs

    static HeadTrackConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        HeadTrackConfig c;
        apply(ov, "head.confMin",        c.confMin);
        apply(ov, "head.earIpdFactor",   c.earIpdFactor);
        apply(ov, "head.earWidthMm",     c.earWidthMm);
        apply(ov, "head.chinConfWeight", c.chinConfWeight);
        apply(ov, "head.minContribPts",  c.minContribPts);
        apply(ov, "head.addrMinFrames",  c.addrMinFrames);
        apply(ov, "head.addrWindowUs",   c.addrWindowUs);
        return c;
    }
};

// Per-frame head state. Coordinates are PIXELS (isotropic). A frame with fewer
// than minContribPts confident head keypoints leaves valid=false (the trace
// skips it — the series resample bridges the gap; never NaN).
struct HeadState {
    int64_t t_us      = 0;
    bool    valid     = false;   // head centre localizable this frame
    QPointF centerPx;            // conf-weighted centroid of {nose,eyes,ears(+chin)}, px
    double  scalePx   = 0.0;     // inter-ear px, else inter-eye × earIpdFactor; 0 = unknown
    double  tiltDeg   = 0.0;     // eye-line (fallback ear-line) angle vs image horizontal
    bool    tiltValid = false;   // an eye or ear pair backed tiltDeg
    float   conf      = 0.f;     // mean of the contributing head-kp confidences
};

// One sparse address-referenced channel (valid-subset t_us, ascending).
struct HeadChannel {
    std::vector<int64_t> t_us;
    std::vector<double>  value;
};

struct HeadTrackResult {
    std::vector<HeadState> states;   // one per input frame (time order)
    // Address-referenced sparse channels. sway = x(t)−x(addr) (+ = image-right),
    // lift = y(addr)−y(t) (+ = up), both in ×frame-width units; tilt = tilt(t)−
    // tilt(addr) in degrees. sway/lift share t_us (centre-valid frames); tilt has
    // its own (tilt-valid frames).
    HeadChannel sway, lift, tilt;
    QPointF addrCenterPx;            // robust-mean centre over the address hold, px
    double  addrTiltDeg = 0.0;       // robust-mean eye-line angle, deg
    double  addrScalePx = 0.0;       // robust-mean head scale @address, px (body-scale proxy)
    int     frameW = 0, frameH = 0;
    bool    valid  = false;          // address reference resolved (≥1 centre-valid frame)
};

// Per-frame head state + Address-referenced traces from the (smoothed, else raw)
// pose track. addressUs = the Address phase instant for the robust reference;
// < 0 ⇒ fall back to the first cfg.addrMinFrames centre-valid frames.
HeadTrackResult trackHead(const PoseTrack2D &pose, int frameW, int frameH,
                          int64_t addressUs = -1, const HeadTrackConfig &cfg = {});

// Resample the sparse channels onto the full per-frame grid (linear interp, hold
// at ends, gaps bridged — NEVER NaN) and emit headSway / headLift / headTilt
// MetricSeries with Address/Top/Impact phase samples. UNSCORED (no reference
// bands until a corpus exists — deliberately no bandLo/bandHi).
//
// UNIT INVARIANCE: sway/lift are CENTIMETRES and are emitted ONLY when pxPerMm
// resolves (> 0). There is no fallback scale — a metric whose unit changes per
// swing cannot carry a norm, and both of these carry one. tilt is always
// degrees and is emitted regardless. Empty when the address reference is
// unresolved (no head anywhere) or the channel never had a sample.
// The caller owns the px→mm scale-source priority (2D calibration → inter-ear
// addrScalePx/earWidthMm → athlete height → club length).
std::vector<MetricSeries> buildHeadSeries(const HeadTrackResult &res,
                                          const std::vector<PhaseEvent> &phases,
                                          double pxPerMm = -1.0);

} // namespace pinpoint::analysis
