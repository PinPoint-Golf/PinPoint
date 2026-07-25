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

// Ball position at address — where the ball sits along the stance, as a fraction
// of the lead-heel→trail-heel line:
//
//     v    = trailHeel − leadHeel
//     frac = dot(ball − leadHeel, v) / |v|²
//
// 0 = at the lead heel, 1 = at the trail heel. DELIBERATELY UNCLAMPED: a ball
// forward of the lead heel (frac < 0) is a real, coachable driver setup, not an
// error to be squashed.
//
// `fracOfStance` IS THE RAW GEOMETRY, NOT THE PUBLISHED METRIC. It runs lead->trail
// from an origin at the lead heel, which is the opposite of the project sign
// convention (positive toward the lead side — docs/design/pinpoint_sign_conventions.md).
// The `ballPosition` metric is re-centred on the middle of the stance and flipped at
// the point of emission in wrist_analyzer.cpp:
//
//     published = (0.5 - fracOfStance) * 100     // +50 % lead heel, 0 % centre, -50 % trail heel
//
// The conversion lives there rather than here on purpose: the plausibility gate
// (cfg.fracLo/fracHi) and every test below are written against the raw fraction, and
// re-signing at source would silently invert that band.
//
// WHY THIS IS THE SCALE-FREE METRIC. The denominator is exactly foot_metrics'
// stanceWidth measurement — the same Euclidean heel-to-heel distance over the
// same address reference frames — so the two agree by construction. Both are px
// distances in the SAME image plane at the SAME depth, so their RATIO needs no
// px→mm scale factor and is genuinely comparable across captures, framings and
// focal lengths. (Stance width in absolute units is not: it needs the ball ruler
// below, which carries the ball-radius measurement error.)
//
// ROBUST ADDRESS BALL CENTRE. Component-wise median of the `found` samples inside
// the address window, then a cluster gate about that median. This mirrors
// ball_anchor.cpp's medianGripBallLenPx pass 1 deliberately, and for the reason
// documented there: a component-wise median is ORDER-INDEPENDENT, so a detector
// warm-up mis-lock on the opening frames cannot veto every later good sample the
// way a first-accepted chain gate can (observed on the 2026-07-04 corpus).
//
// DUPLICATION, ACKNOWLEDGED. medianGripBallLenPx already computes this exact
// (medX, medY) internally and then discards it — it returns only the grip→ball
// length. Plumbing it out would mean widening the shaft tracker's frozen
// decideTrack internals for a metric that needs none of the rest; recomputing ~10
// lines here against the same convention is the cheaper, lower-risk choice. If a
// third consumer ever appears, factor it out the way buildThetaBallSeries was.
//
// The pass also yields the ball-diameter px→mm ruler as a by-product (the ball's
// real diameter is a fixed R&A/USGA spec), which foot_metrics uses to express
// stance width in mm. That ruler is exact only at the ball's ground-plane depth —
// which face-on is essentially the feet's depth, so it is the RIGHT ruler for
// stance geometry, but it rests on a ~9.5 px radius measurement and so carries
// roughly 10 % scale error per ±1 px of radius error. Median it, never trust one
// frame, and treat mm stance width as an estimate rather than a calibration.

#include <QPointF>
#include <QVariantMap>
#include <cstdint>

#include "swing_analysis.h"        // BallTrack2D, BallSample2D
#include "analysis_tuning.h"       // tuning::apply
#include "../Core/pp_tuned_constants.h"   // tuned::ballpos::

namespace pinpoint::analysis {

// Ball-position knobs. Defaults track the frozen constants (pp_tuned_constants.h
// ballpos::); SwingLab sweeps them via "ballpos.*" dotted keys. enabled=false is
// the OFF-parity path — the result is invalid, so nothing is emitted.
struct BallPositionConfig {
    bool    enabled      = tuned::ballpos::kEnabled;        // ballpos.enabled
    int64_t addrWindowUs = tuned::ballpos::kAddrWindowUs;   // ballpos.addrWindowUs
    int     minSamples   = tuned::ballpos::kMinSamples;     // ballpos.minSamples
    double  maxJumpPx    = tuned::ballpos::kMaxJumpPx;      // ballpos.maxJumpPx — cluster gate radius
    double  fracLo       = tuned::ballpos::kFracLo;         // ballpos.fracLo
    double  fracHi       = tuned::ballpos::kFracHi;         // ballpos.fracHi

    static BallPositionConfig fromOverrides(const QVariantMap &ov)
    {
        using namespace tuning;
        BallPositionConfig c;
        apply(ov, "ballpos.enabled",      c.enabled);
        apply(ov, "ballpos.addrWindowUs", c.addrWindowUs);
        apply(ov, "ballpos.minSamples",   c.minSamples);
        apply(ov, "ballpos.maxJumpPx",    c.maxJumpPx);
        apply(ov, "ballpos.fracLo",       c.fracLo);
        apply(ov, "ballpos.fracHi",       c.fracHi);
        return c;
    }
};

struct BallPositionResult {
    bool    valid        = false;  // false ⇒ emit NOTHING (never a fabricated value)
    double  fracOfStance = 0.0;    // 0 = lead heel, 1 = trail heel; unclamped
    QPointF addressBallPx;         // robust address ball centre (px)
    // px→mm from the ball-diameter ruler; <= 0 = unresolved (no usable radius).
    // Valid INDEPENDENTLY of `valid` — a swing with a good ball lock but no
    // usable heels still yields a ruler, which is what foot_metrics wants.
    double  mmPerPx      = -1.0;
    int     samples      = 0;      // accepted (in-cluster) ball samples
};

// Measure the address ball position against a heel pair. `addressUs < 0` falls
// back to every found sample before launch (the ball is stationary until then).
// Returns valid=false on: disabled, no ball track, degenerate heel separation,
// too few accepted samples, or a frac outside [fracLo, fracHi] (an implausible
// result is a detector failure, not a wide stance). `mmPerPx` may still resolve
// on an otherwise-invalid result.
BallPositionResult computeBallPosition(const BallTrack2D &ball,
                                       QPointF leadHeelPx, QPointF trailHeelPx,
                                       int64_t addressUs, int frameW, int frameH,
                                       const BallPositionConfig &cfg = {});

} // namespace pinpoint::analysis
