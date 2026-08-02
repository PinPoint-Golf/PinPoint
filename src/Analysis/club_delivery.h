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

// Club-delivery metrics a FACE-ON camera can resolve: backswing length, attack angle and low point.
//
// ── Which club numbers a face-on camera can and cannot answer ───────────────
//
// A face-on camera is perpendicular to the target line, so its image plane is (ALONG THE TARGET
// LINE, VERTICAL) and its blind axis is the depth axis, toward and away from the ball. That
// division decides this module's contents exactly:
//
//   IN  — `shaftAngleVsHorizontal` (how far past parallel at the top), `attackAngle` (the vertical
//         angle of the head's velocity) and `lowPointAhead` (where the arc bottoms out along the
//         target line). All three live entirely in the plane the camera sees.
//   OUT — `clubPath` (in-to-out is the depth axis), `swingPlane` (needs the plane's azimuth) and
//         `shaftDirection` (where the shaft points relative to the target line). All three are
//         down-the-line questions and stay planned.
//
// ATTACK ANGLE IS DELIBERATELY IN THIS LIST, and its descriptor used to say the opposite. The
// argument for "down the line makes it fully in-plane" does not survive being written down: a
// down-the-line camera looks ALONG the target line, so the direction the head is travelling at
// impact is its optical axis — the one direction it cannot measure. The vertical angle of a
// velocity that runs along the target line is a face-on reading. The residual error is a cosine
// term from the path's depth component, which is a few degrees of path acting on the tangent, not
// a projection that destroys the quantity.
//
// ── Measured heads only ─────────────────────────────────────────────────────
//
// Every reading here is taken from `ShaftSample2D::headPx`, and `headPx` is NOT always a
// measurement: without the Stage-2 head pass it is projected from the grip along the shaft
// direction at an assumed club length (`ShaftHeadProjected`). A projected head is a rigid function
// of θ and length, so its "velocity" is the grip's velocity plus a length-scaled rotation term —
// differentiating it produces a confident attack angle that contains no information about the
// clubhead at all. Samples carrying `ShaftHeadProjected` are therefore EXCLUDED, not down-weighted,
// and a swing whose head was never measured produces nothing rather than something plausible.
//
// This is also why `lowPointAhead` was deferred when it was first designed: the measured-head
// detector did not exist. It does now (`clubhead_track.{h,cpp}`, default ON), which is what makes
// this module possible at all.
//
// ── Units, and the ruler ────────────────────────────────────────────────────
//
// Angles are degrees and need no scale. `lowPointAhead` is signed INCHES, which needs one — the
// ball-diameter px→mm ruler from `ball_position.cpp`, resolved by the caller and passed in. Without
// it the metric is ABSENT rather than emitted in pixels: the invariant-unit rule foot_metrics
// states at length applies here too, because a corridor declares one unit and grading never looks
// at it again.
//
// Pure, deterministic, Qt-only (no OpenCV / no Qt-GUI); unit-tested standalone with no fixture.

#include <QPointF>
#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // ShaftTrack2D, MetricSeries, PhaseEvent
#include "metric_channel.h"          // MetricChannel, buildChannelSeries
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::clubDelivery::

namespace pinpoint::analysis {

// Club-delivery knobs. Defaults track the frozen constants (pp_tuned_constants.h clubDelivery::);
// SwingLab sweeps them via "clubDelivery.*" dotted keys.
struct ClubDeliveryConfig {
    // Half-width, in samples, of the centred difference the head velocity is taken over. One frame
    // either side of a ~9 px head at 240 fps is mostly quantisation noise; a few frames of span
    // trades a little time resolution for an attack angle that is not dominated by it.
    int     velHalfSpan   = tuned::clubDelivery::kVelHalfSpan;   // clubDelivery.velHalfSpan
    // Search half-window about Impact for the arc vertex, in microseconds.
    int64_t lowPointWinUs = tuned::clubDelivery::kLowPointWinUs; // clubDelivery.lowPointWinUs
    // Minimum measured-head samples inside that window before a low point is reported.
    int     lowPointMinSamples = tuned::clubDelivery::kLowPointMinSamples; // clubDelivery.lowPointMinSamples
    // Head-confidence gate. −1 in the sample means the Stage-2 pass never ran, which is a harder
    // refusal than a low confidence and is handled separately.
    double  headConfMin   = tuned::clubDelivery::kHeadConfMin;   // clubDelivery.headConfMin

    static ClubDeliveryConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        ClubDeliveryConfig c;
        apply(ov, "clubDelivery.velHalfSpan",        c.velHalfSpan);
        apply(ov, "clubDelivery.lowPointWinUs",      c.lowPointWinUs);
        apply(ov, "clubDelivery.lowPointMinSamples", c.lowPointMinSamples);
        apply(ov, "clubDelivery.headConfMin",        c.headConfMin);
        return c;
    }
};

struct ClubDeliveryResult {
    std::vector<int64_t> grid;          // measured-head sample times, ascending

    MetricChannel shaftVsHorizontal;    // ° — 0 is parallel to the ground, + is PAST parallel
    MetricChannel attackAngle;          // ° — + is a more UPWARD strike

    bool   lowPointValid = false;
    double lowPointIn    = 0.0;         // signed inches, + = low point ahead of (target-side of) the ball
    int64_t lowPointTUs  = 0;           // when the arc bottomed out

    bool valid = false;                 // at least one measured-head sample was usable
};

// Read the club-delivery channels off a face-on shaft track.
//
// `addressBallPx` and `mmPerPx` come from `computeBallPosition` (which yields both even when the
// heel pair is unusable — the ruler survives what the stance geometry does not). Pass mmPerPx <= 0
// or an unresolved ball to suppress `lowPointAhead` alone; the two angles do not need either.
ClubDeliveryResult trackClubDelivery(const ShaftTrack2D &shaft, const std::vector<PhaseEvent> &phases,
                                     QPointF addressBallPx, bool ballValid, double mmPerPx,
                                     const ClubDeliveryConfig &cfg = {});

// Emit shaftAngleVsHorizontal (a curve) plus attackAngle and lowPointAhead (Impact scalars, carried
// as phaseSamples over an empty curve — the representation every setup metric already uses).
std::vector<MetricSeries> buildClubDeliverySeries(const ClubDeliveryResult &res,
                                                  const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
