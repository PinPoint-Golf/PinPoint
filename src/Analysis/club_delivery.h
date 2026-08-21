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
// ── Two channels, on purpose ────────────────────────────────────────────────
//
// The two ANGLES are taken from `ShaftSample2D::headPx`, and `headPx` is NOT always a measurement:
// without the Stage-2 head pass it is projected from the grip along the shaft direction at an
// assumed club length (`ShaftHeadProjected`). A projected head is a rigid function of θ and length,
// so its "velocity" is the grip's velocity plus a length-scaled rotation term — differentiating it
// produces a confident attack angle that contains no information about the clubhead at all. Samples
// carrying `ShaftHeadProjected` are therefore EXCLUDED from the angles, not down-weighted.
//
// `lowPointAhead` DOES NOT USE THAT CHANNEL, and the reason is a corpus fact rather than a
// preference. Across 108 recorded swings the head detector holds a measured lock through roughly
// −45 ms to +40 ms of impact on almost none of them — the club is at its fastest, most motion-
// blurred, and lowest against the turf exactly where this metric is read. Requiring 5 measured
// heads inside ±60 ms of Impact produced a value on 9 of 108 swings, and on the three that could be
// checked the located vertex sat +16 to +38 ms PAST impact: over a metre beyond the ball, which is
// not a low point at all but the lowest of a handful of scattered survivors. The gate was not
// filtering out bad swings, it was admitting bad vertices on the rare swing it fired.
//
// So the low point is read off `ShaftTrack2D::synth` — the kinematically-synthesized arc
// (`shaft_synthesis.h`) Hermite-interpolated between the located P-anchors at a dense fixed
// cadence. On the same corpus that yields a value on every swing with the vertex landing within a
// few ms of impact, and its attack angle at impact is UNBIASED against a launch monitor (+0.02°)
// where the measured-head channel was out by tens of degrees.
//
// ⚠ THIS RELAXES A STATED INVARIANT, NARROWLY. `shaft_synthesis.h` calls the synthesized tier a
// visualization channel "excluded from every metric/scoring/estimand", and that sentence was true
// when it was written. It now has exactly one exception, this metric, and the exception is
// deliberate: the arc IS the thing being measured, and reading the same series the overlay draws is
// what makes the number and the picture agree. Everything else — scoring, estimands, the plane fit,
// the wrist channel — still excludes `synth`. The coupling that comes with it: `synth.enabled=false`
// now takes `lowPointAhead` with it, where before it changed nothing.
//
// ⚠ AND IT IS AN ESTIMATE, published as one. The arc through impact is an interpolation between
// P6, P7 and P8 rather than an observation, so its vertex is pinned near the P7 anchor and what the
// metric really reports is where that anchor puts the head relative to the ball. Measured against a
// launch monitor the spread is ±2.0 in (tuned::clubDelivery::kLowPointSigmaIn) against a corridor
// only 3.2 in wide. That number ships WITH the metric as `MetricSeries::sigma`, and the catalogue
// route is RouteQuality::Estimated so the reading resolves as Bridged rather than Measured. It
// averages well — six swings of one session recovered the device's own session mean to 0.02° — so
// read it as a session tendency and distrust any single swing.
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
    // Minimum SYNTHESIZED-ARC samples inside that window before a low point is reported. At the
    // 240 Hz synthesis cadence ±60 ms is ~29 samples, so this is not a coverage bar the way it was
    // for measured heads — it is a floor that refuses a truncated or barely-populated arc.
    int     lowPointMinSamples = tuned::clubDelivery::kLowPointMinSamples; // clubDelivery.lowPointMinSamples
    // Published 1σ on lowPointAhead, in inches. See the constant for its provenance.
    double  lowPointSigmaIn = tuned::clubDelivery::kLowPointSigmaIn; // clubDelivery.lowPointSigmaIn
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
        apply(ov, "clubDelivery.lowPointSigmaIn",    c.lowPointSigmaIn);
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
    double lowPointSigmaIn = 0.0;       // published 1σ (in) — 0 when no low point was produced

    bool valid = false;                 // at least one measured-head sample was usable
};

// Read the club-delivery channels off a face-on shaft track.
//
// `addressBallPx` and `mmPerPx` come from `computeBallPosition` (which yields both even when the
// heel pair is unusable — the ruler survives what the stance geometry does not). Pass mmPerPx <= 0
// or an unresolved ball to suppress `lowPointAhead` alone; the two angles do not need either.
//
// The two channels fail INDEPENDENTLY: a track with no measured heads still yields a low point if
// `shaft.synth` is populated, and a track with measured heads but no synthesized arc still yields
// the two angles. Neither is allowed to take the other down.
ClubDeliveryResult trackClubDelivery(const ShaftTrack2D &shaft, const std::vector<PhaseEvent> &phases,
                                     QPointF addressBallPx, bool ballValid, double mmPerPx,
                                     const ClubDeliveryConfig &cfg = {});

// Emit shaftAngleVsHorizontal (a curve) plus attackAngle and lowPointAhead (Impact scalars, carried
// as phaseSamples over an empty curve — the representation every setup metric already uses).
std::vector<MetricSeries> buildClubDeliverySeries(const ClubDeliveryResult &res,
                                                  const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
