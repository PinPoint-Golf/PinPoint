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

#include "swing_analysis.h"
#include "analysis_tuning.h"
#include "../Core/pp_tuned_constants.h"

#include <QVariantMap>

#include <vector>

// Club-kinematics chart series (clubhead speed, hand speed, lag angle) — three
// per-frame curves the session review chart shows alongside the wrist/shaft metrics.
//
// These are UNSCORED display curves (no reference band, no scorer/trace involvement),
// appended to SwingAnalysis::series exactly like the head/foot detail series. They are
// derived PURELY from the face-on camera products — no IMU input:
//   - clubhead speed = linear speed of the shaft far end (headPx)
//   - hand speed     = linear speed of the shaft grip end (gripPx)
//   - lag angle      = angle between the lead forearm (elbow→wrist pose) and the
//                      clubshaft (grip→head) direction
// Speeds prefer the dense synth shaft channel (ShaftTrack2D.synth) and fall back to the
// measured samples. Hand speed differentiates the grip path; clubhead speed is COMPOSED
// from the track's constituents — |v_grip + L·θ̇·n̂| with the fused club length and the
// sample's own rate (KinematicSeriesInputs::composed) — because differentiating the
// interpolated head path reported the shape of the interpolation near impact. The
// px→metre scale comes from the club-length fusion so the speeds read in mph.
//
// The builder lives here rather than inside the wrist analyzer so every session-type
// profile can reuse it: the Wrist profile appends these via KinematicsStage, and the
// Swing/GRF/Coach analyzers reuse the same camera stages (Pose→Ball→Shaft) to produce
// the shaft/pose products this consumes. When a product is absent (no face-on camera)
// the affected curve is simply omitted — never fabricated.

namespace pinpoint::analysis {

// Config for the kinematics display stage (developer guide §6.3): the master gate and
// the composed-speed producer switch, "kinematics.*" keys.
struct KinematicSeriesConfig {
    bool enabled  = tuned::kinematics::kEnabled;    // kinematics.enabled — master gate (dark)
    bool composed = tuned::kinematics::kComposed;   // kinematics.composed — see KinematicSeriesInputs (ON 2026-09-06)

    static KinematicSeriesConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        KinematicSeriesConfig c;
        apply(ov, "kinematics.enabled",  c.enabled);
        apply(ov, "kinematics.composed", c.composed);
        return c;
    }
};

struct KinematicSeriesInputs {
    // Real per-frame camera geometry. Either may be null/invalid; the affected curve
    // is then omitted (speeds need shaft; lag needs shaft AND pose).
    const ShaftTrack2D *shaft = nullptr;   // clubhead (headPx) + grip (gripPx) per frame
    const PoseTrack2D  *pose  = nullptr;   // lead forearm (elbow→wrist) keypoints per frame

    int64_t impactUs    = -1;              // impact instant (absolute µs) — Impact phase dot
    int     handedness  = 0;               // 0 unknown, 1 right, 2 left (lead-arm sign)
    double  clubLengthM = 1.12;            // physical club length → px→metre scale for mph

    // Clubhead speed COMPOSED from the track's constituents — |v_grip + L·θ̇·n̂| with
    // L the fused club length (px) and θ̇ the sample's own rate — instead of
    // differentiating the head path. Differentiating the synthesized arc reported the
    // shape of the interpolation near impact (peak at the P6→P7 bracket midpoint, 1 mph/ms
    // decay into P7); composing reads the rate the tier actually carries, and applies no
    // smoothing across the impact step. Hand speed and lag are unaffected.
    bool    composed    = false;
    // Butt→grip-anchor distance (m) the px→metre scale must subtract when composing: the
    // tracker's grip anchor sits at the BOTTOM of the hands and its fused length is the
    // anchor→head span, which it draws as (clubLengthM − lenGripDownM) — see
    // ShaftV3Config::lenGripDownM and the length ladder. Mapping that span to the FULL club
    // length (the differentiated path's convention) over-scales every speed by ~14 %.
    // Only the composed producer applies it, so the legacy path stays byte-identical.
    double  gripDownM   = 0.13;

    // Phase timeline (ctx.seg.events) so the curves carry Address/Top/Impact phase dots
    // matching the other detail series. Empty ⇒ only an Impact dot (from impactUs).
    std::vector<PhaseEvent> phases;
};

// Returns up to three MetricSeries in a stable order: clubhead speed (mph), hand
// speed (mph), lag angle (°). A curve is included only when its camera product exists;
// with no shaft track the result is empty.
std::vector<MetricSeries> buildKinematicSeries(const KinematicSeriesInputs &in);

} // namespace pinpoint::analysis
