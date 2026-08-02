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

#include "pose_wrist_angle_source.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "hand_axis.h"                 // kHand*Mcp / kLeftHandFirstKp / kRightHandFirstKp
#include "metric_channel.h"            // MetricChannel / buildChannelSeries (trail-wrist series)
#include "wrist_analysis_adapter.h"    // wristCheckpoints() — shared checkpoint→Phase map

namespace pinpoint::analysis {

namespace {

constexpr double kRad2Deg = 57.29577951308232;

// Source-aware base-confidence floor for the pose (apparent) source (design §4).
// Deliberately well below the IMU floors (leadWristFlexExt 0.84, radUln 0.86 —
// wrist_analysis_adapter.cpp): a camera-plane projection is a resemblance signal,
// not an anatomical measurement. The per-sample apparent penalty compounds it.
constexpr float kPoseBaseConf = 0.5f;

// Signed image-plane angle (deg) FROM vector a TO vector b (atan2 of the 2-D
// cross and dot). NaN when either vector is degenerate.
double signedAngleDeg(double ax, double ay, double bx, double by)
{
    if ((ax == 0.0 && ay == 0.0) || (bx == 0.0 && by == 0.0))
        return std::numeric_limits<double>::quiet_NaN();
    return std::atan2(ax * by - ay * bx, ax * bx + ay * by) * kRad2Deg;
}

} // namespace

PoseWristAngleSource::PoseWristAngleSource(const PoseTrack2D &pose,
                                           const std::vector<PhaseEvent> &phases,
                                           int handedness, int frameW, int frameH,
                                           const PoseWristAngleConfig &cfg)
{
    // Canonical right-handed: a left-handed swing is pre-mirrored below, so the
    // sampler applies no further mirror (same contract as buildWristAngleSource).
    setHandedness(PpHandedness::Right);

    // Smoothed companion track preferred (bridges the occluded-hand gaps at the
    // top of the swing); fall back to raw on pre-smoother swings.
    const std::vector<PoseFrame2D> &frames =
        pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frameW <= 0 || frameH <= 0 || frames.empty())
        return;   // no series ⇒ every cell Gap (never a fabricated value)

    const bool leftLeads = (handedness != 2);
    const int  elbow     = leftLeads ? 7 : 8;     // COCO lead elbow (L / R)
    const int  wrist     = leftLeads ? 9 : 10;    // COCO lead wrist
    const int  base      = leftLeads ? kLeftHandFirstKp : kRightHandFirstKp;
    const int  wristRoot = base;                  // hand kp 0
    const int  middleMcp = base + kHandMiddleMcp;
    const int  indexMcp  = base + kHandIndexMcp;
    const int  pinkyMcp  = base + kHandPinkyMcp;
    // Left↔right IMAGE mirror for a left-handed golfer: the signed image-plane
    // angle negates under an x-flip, so BOTH apparent angles flip. This is the
    // whole-image mirror, NOT the engine's per-DOF anatomical mirrorSign (which
    // does not apply to a camera-plane projection).
    const double mirror = leftLeads ? 1.0 : -1.0;

    PpJointAngleSeries feSer;
    feSer.dof            = PpJointDof::LeadWristFlexExt;
    feSer.present        = true;
    feSer.baseConfidence = kPoseBaseConf;
    PpJointAngleSeries rudSer;
    rudSer.dof            = PpJointDof::LeadWristRadUln;
    rudSer.present        = true;
    rudSer.baseConfidence = kPoseBaseConf;
    feSer.samples.reserve(frames.size());
    rudSer.samples.reserve(frames.size());

    const double gate = cfg.confMin;
    const double pen  = cfg.apparentPenalty;

    for (const PoseFrame2D &f : frames) {
        // px-space vectors (isotropic via frameW/H).
        const double fx = (f.kp[wrist].x()     - f.kp[elbow].x())     * frameW;   // forearm elbow→wrist
        const double fy = (f.kp[wrist].y()     - f.kp[elbow].y())     * frameH;
        const double ax = (f.kp[middleMcp].x() - f.kp[wristRoot].x()) * frameW;   // hand axis root→middle-MCP
        const double ay = (f.kp[middleMcp].y() - f.kp[wristRoot].y()) * frameH;
        const double kx = (f.kp[pinkyMcp].x()  - f.kp[indexMcp].x())  * frameW;   // knuckle line index→pinky
        const double ky = (f.kp[pinkyMcp].y()  - f.kp[indexMcp].y())  * frameH;
        const double nx = -fy, ny = fx;                                           // forearm normal (⊥ F)

        // apparentFlexExt = signed angle forearm → hand axis.
        double apparentFlexExt = signedAngleDeg(fx, fy, ax, ay) * mirror;
        // apparentRadUln = signed angle forearm-normal → knuckle line.
        double apparentRadUln  = signedAngleDeg(nx, ny, kx, ky) * mirror;

        // Per-DOF endpoint confidences + the apparent-angle penalty.
        const double feMin = std::min(std::min(f.conf[elbow], f.conf[wrist]),
                                      std::min(f.conf[wristRoot], f.conf[middleMcp]));
        const double rudMin = std::min(std::min(f.conf[elbow], f.conf[wrist]),
                                       std::min(f.conf[indexMcp], f.conf[pinkyMcp]));

        PpJointAngleSample fe;
        fe.t_us          = f.t_us;
        fe.available     = (feMin >= gate) && !std::isnan(apparentFlexExt);
        fe.valueDeg      = fe.available ? apparentFlexExt : 0.0;
        fe.confidence    = fe.available ? float(feMin * pen) : 0.f;
        fe.pitchProxyDeg = 0.0;   // no gimbal proxy for a planar projection (never Indeterminate)
        feSer.samples.push_back(fe);

        PpJointAngleSample rud;
        rud.t_us          = f.t_us;
        rud.available     = (rudMin >= gate) && !std::isnan(apparentRadUln);
        rud.valueDeg      = rud.available ? apparentRadUln : 0.0;
        rud.confidence    = rud.available ? float(rudMin * pen) : 0.f;
        rud.pitchProxyDeg = 0.0;
        rudSer.samples.push_back(rud);
    }

    setSeries(feSer);
    setSeries(rudSer);

    // P1–P8 timeline from the swing phases (shared checkpoint→Phase map).
    const WristCheckpoint *cps = wristCheckpoints();
    PpSwingPositionTimeline tl;
    for (int c = 0; c < kNumPos; ++c) {
        for (const PhaseEvent &e : phases) {
            if (static_cast<int>(e.phase) != cps[c].phase)
                continue;
            PpSwingPositionTimeline::Entry en;
            en.present = true;
            en.t_us    = e.t_us;
            en.conf    = e.conf;
            tl.positions[c] = en;
            break;
        }
    }
    setTimeline(tl);
}

std::vector<MetricSeries> buildTrailWristSeries(const PoseTrack2D &pose,
                                                const std::vector<PhaseEvent> &phases,
                                                int handedness, int frameW, int frameH,
                                                const PoseWristAngleConfig &cfg)
{
    std::vector<MetricSeries> out;

    const std::vector<PoseFrame2D> &frames =
        pose.smoothed.empty() ? pose.frames : pose.smoothed;
    if (frameW <= 0 || frameH <= 0 || frames.empty())
        return out;

    const bool leftLeads = (handedness != 2);
    // The TRAIL side: the opposite elbow, wrist and hand block from the lead-side constructor above.
    const int elbow     = leftLeads ? 8 : 7;      // COCO trail elbow (R / L)
    const int wrist     = leftLeads ? 10 : 9;     // COCO trail wrist
    const int base      = leftLeads ? kRightHandFirstKp : kLeftHandFirstKp;
    const int wristRoot = base;
    const int middleMcp = base + kHandMiddleMcp;

    // See the header for the derivation. Face-on, the two hands are mirror images seen from the
    // same side, so the raw signed image angle already reads EXTENSION-positive on the trail hand
    // for a left-leading golfer; a left-handed swing is the whole-image mirror of that.
    const double mirror = leftLeads ? 1.0 : -1.0;

    MetricChannel ch;
    std::vector<int64_t> grid;
    grid.reserve(frames.size());
    for (const PoseFrame2D &f : frames) {
        grid.push_back(f.t_us);

        const double fx = (f.kp[wrist].x()     - f.kp[elbow].x())     * frameW;   // forearm elbow→wrist
        const double fy = (f.kp[wrist].y()     - f.kp[elbow].y())     * frameH;
        const double ax = (f.kp[middleMcp].x() - f.kp[wristRoot].x()) * frameW;   // hand axis root→middle-MCP
        const double ay = (f.kp[middleMcp].y() - f.kp[wristRoot].y()) * frameH;

        const double v = signedAngleDeg(fx, fy, ax, ay) * mirror;
        if (std::isnan(v))
            continue;
        // The same four-endpoint gate the lead side applies. A frame below it is ABSENT from the
        // channel and bridged by the resample — never a fabricated zero.
        const double minConf = std::min(std::min(f.conf[elbow], f.conf[wrist]),
                                        std::min(f.conf[wristRoot], f.conf[middleMcp]));
        if (minConf < cfg.confMin)
            continue;
        ch.push(f.t_us, v);
    }

    appendIfProduced(out, buildChannelSeries(grid, ch, QStringLiteral("trailWristFlexExt"),
                                             QStringLiteral("Trail wrist — bow / cup"),
                                             QStringLiteral("°"), phases));
    return out;
}

} // namespace pinpoint::analysis
