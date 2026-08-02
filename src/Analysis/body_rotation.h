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

// Axial body rotation — pelvis and thorax turn, X-factor and X-factor stretch.
// See docs/design/body_rotation_estimation.md.
//
// ── One producer, two tiers ─────────────────────────────────────────────────
//
// This is the module that puts the standing rule into code: PRODUCE THE MEASUREMENT FROM WHATEVER
// IS AVAILABLE, AND TAKE THE BETTER PATH AUTOMATICALLY WHEN A BETTER SENSOR IS THERE. Axial turn is
// rotation about the body's vertical axis, which a frontal projection cannot see directly — so a
// module built only for the ideal sensor would emit nothing on every swing this product actually
// records, and a module built only for the camera would throw away a bound IMU.
//
//   Tier IMU (`RotationTier::Imu`) — a bound Pelvis / Thorax segment stream. The turn is the
//     segment's medio-lateral axis carried into world by q_anat and projected into the horizontal
//     plane, referenced to its own address direction. This is the quantity the descriptor names,
//     measured rather than inferred, and it is what the metric resolves as `Measured`.
//
//   Tier FORESHORTENING (`RotationTier::Foreshortening`) — a face-on camera only. As a body line
//     turns away from the camera its image width collapses by the cosine of the turn, so
//
//         turn(t) = acos( clamp( w(t) / w_address, 0, 1 ) )
//
//     where w is the hip span for the pelvis and the shoulder span for the thorax. Reported as
//     `Bridged`: produced, honestly, at reduced fidelity.
//
// ── The magnitude decision, and why it is not an oversight ──────────────────
//
// The series is the UNSIGNED MAGNITUDE of turn away from the address orientation, not a signed
// away/toward reading. That is what the shipped corridors ask for and the only reading consistent
// with all of them: `m_pelvisRotP4` is seated at +45° (the top, turned away from the target) and
// `m_pelvisRotP7` at +40° (impact, turned toward it). A signed curve cannot satisfy both without
// one of them being seated negative, and neither is. `m_thoraxRotFinish` at +110° agrees.
//
// It also happens to be exactly what the foreshortening estimator can honestly deliver: a cosine
// carries no sign, so a signed camera reading would have to be manufactured from the phase ladder.
// The magnitude convention means the camera tier invents nothing. The cost is real and stated: the
// curve passes through zero as the body squares up in the downswing, so a peak reducer over
// P4→P7 sees the larger of the two excursions rather than the open one. Any measure that wants the
// open peak specifically must window from square to impact, not from the top.
//
// ── Where the camera tier is weak, stated plainly ───────────────────────────
//
//   * NEAR SQUARE the cosine is flat: dθ/dw = −1 / (w₀ · sin θ) diverges as θ → 0, so a pixel of
//     span noise becomes many degrees of turn. The producer PROPAGATES this rather than hiding it —
//     each series carries a `sigma` derived from the span noise through that derivative, with sin θ
//     floored so the number stays finite. A reader that ignores sigma will over-read small turns.
//   * ABOVE ~70° the span has collapsed into the noise and the estimate saturates toward the 90°
//     ceiling acos can reach. A full shoulder turn is right at that edge, which is why the thorax
//     corridors are seated wide.
//   * The address frame is assumed SQUARE TO THE CAMERA. It is the only reference available and a
//     golfer set open or closed biases every reading by that amount. This is a bias, not noise: it
//     does not average out across a session.
//   * PELVIC TILT and lateral bend also shorten the apparent span, and are indistinguishable from
//     turn in one projection.
//
// None of that makes the number worthless — it makes it a camera estimate, which is what
// `MetricAvailability::Bridged` exists to say. It does mean no corridor over this may be seated
// tighter than the method supports.
//
// Pure, deterministic, Qt-only (no OpenCV / no Qt-GUI); unit-tested standalone with no fixture.

#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <vector>

#include "swing_analysis.h"          // PoseTrack2D, MetricSeries, PhaseEvent, SegmentRole
#include "imu_vision_fuser.h"        // FusedStreams, SegmentStream
#include "metric_channel.h"          // MetricChannel, buildChannelSeries
#include "analysis_tuning.h"         // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::bodyRotation::

namespace pinpoint::analysis {

// Body-rotation knobs. Defaults track the frozen constants (pp_tuned_constants.h bodyRotation::);
// SwingLab sweeps them via "bodyRotation.*" dotted keys.
struct BodyRotationConfig {
    double  confMin      = tuned::bodyRotation::kConfMin;      // bodyRotation.confMin
    int     addrMinFrames = tuned::bodyRotation::kAddrMinFrames; // bodyRotation.addrMinFrames
    int64_t addrWindowUs = tuned::bodyRotation::kAddrWindowUs;  // bodyRotation.addrWindowUs
    double  minSpanPx    = tuned::bodyRotation::kMinSpanPx;     // bodyRotation.minSpanPx
    // 1σ of the measured image span, in pixels — the input to the propagated uncertainty. Not a
    // fudge factor: it is the pose estimator's endpoint jitter, doubled through the difference.
    double  spanNoisePx  = tuned::bodyRotation::kSpanNoisePx;   // bodyRotation.spanNoisePx
    // Floor on sin θ when propagating that noise, so the reported sigma near square stays finite
    // instead of running to infinity. sin 5° ≈ 0.087 caps the reported uncertainty at roughly
    // 11× the span noise in radians.
    double  sinFloor     = tuned::bodyRotation::kSinFloor;      // bodyRotation.sinFloor

    static BodyRotationConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        BodyRotationConfig c;
        apply(ov, "bodyRotation.confMin",       c.confMin);
        apply(ov, "bodyRotation.addrMinFrames", c.addrMinFrames);
        apply(ov, "bodyRotation.addrWindowUs",  c.addrWindowUs);
        apply(ov, "bodyRotation.minSpanPx",     c.minSpanPx);
        apply(ov, "bodyRotation.spanNoisePx",   c.spanNoisePx);
        apply(ov, "bodyRotation.sinFloor",      c.sinFloor);
        return c;
    }
};

// Which path produced a channel. Carried out of the producer so the stage can record it and the
// provider can answer Measured vs Bridged from the same fact rather than re-deriving it.
enum class RotationTier { None, Imu, Foreshortening };

// One segment's turn curve plus how it was obtained.
struct RotationChannel {
    MetricChannel turn;                        // degrees, unsigned magnitude from address
    RotationTier  tier     = RotationTier::None;
    double        sigmaDeg = 0.0;              // representative 1σ (median over the swing); 0 = unset
};

struct BodyRotationResult {
    std::vector<int64_t> grid;                 // one entry per pose frame, time order

    RotationChannel pelvis;                    // pelvisRotation   °
    RotationChannel thorax;                    // thoraxRotation   °
    MetricChannel   xFactor;                   // thorax − pelvis  °
    MetricChannel   xFactorStretch;            // xFactor(t) − xFactor(Top)  °

    double addrHipSpanPx = 0.0, addrShoulderSpanPx = 0.0;
    bool   valid = false;                      // at least one segment produced a turn curve
};

// Pelvis / thorax turn from the best available source.
//
// `streams` is consulted FIRST for each segment independently — a swing with a pelvis IMU and no
// thorax IMU gets a measured pelvis and an estimated chest, which is the correct answer rather than
// a reason to refuse. `pose` supplies the camera tier; either may be empty.
//
// `phases` is needed for the address reference and for the X-factor stretch anchor at the top.
BodyRotationResult trackBodyRotation(const PoseTrack2D &pose, const FusedStreams &streams,
                                     int frameW, int frameH, bool leadIsLeft,
                                     const std::vector<PhaseEvent> &phases,
                                     const BodyRotationConfig &cfg = {});

// Emit pelvisRotation / thoraxRotation / xFactor / xFactorStretch. A channel that could not be
// produced is simply absent — never a curve of zeros.
std::vector<MetricSeries> buildBodyRotationSeries(const BodyRotationResult &res,
                                                  const std::vector<PhaseEvent> &phases);

} // namespace pinpoint::analysis
