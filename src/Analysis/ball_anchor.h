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

// v3.4 — the ball as the club's far-end anchor (docs/design/club_tracking_v3_design.md
// §9). An ADDITIVE post-hoc pass applied AFTER decideTrack() returns — reads the
// frozen DP output, fills in individual samples that were NOT already a real
// vision measurement, never re-solves the DP/transition-band machinery. A
// missing or disagreeing ball can only leave the track unchanged, never
// degrade it (design §9.6).
//
// Three exploits (design §9.2-9.4), all gated the same way — "not already
// ShaftMeasured" — so a confirmed measurement is never second-guessed:
//   1. tk0 — the frame the club's measured/tracked theta first departs
//      theta_ball beyond a threshold (the clubhead rotating away from the
//      ball). Computed and logged only (trace.ballTk0Frame) — NOT fed into
//      the phase model/DP; that relabel is v3.0-internal, deferred to the
//      v3.0-r1 corpus re-gate (impl doc's "ACTION" section).
//   3. Impact anchor — the last pre-launch frame's theta, read directly off
//      grip->ball, bridging the impact blur with a measurement instead of a
//      pure kinematic reconstruction.
//   4. Address discrimination + scale floor — publishes ball-anchored theta
//      across the address hold (tk0's domain) where nothing was measured,
//      and derives out.measuredClubLenPx from the grip-to-ball distance.

#include <cstdint>
#include <vector>

#include <QPointF>

#include "swing_analysis.h"       // ShaftTrack2D, BallTrack2D
#include "shaft_track_assembly.h" // ShaftDecideTrace

struct ShotAnalysisJob;

namespace pinpoint::analysis {

// Per-coverage-frame grip→ball direction series (design §9.1). theta_ball(f) =
// atan2(B − G) wherever a stable ball sample sits near frame f, with the mis-lock
// jump gate (a "found" sample that jumped > kMaxJumpNormPx from the last accepted
// one is a mis-lock, not a moved ball — the ball is stationary by construction at
// address). haveBall marks the frames that resolved; thetaBall/ballPx are defined
// only there (0 / default elsewhere). Extracted from applyBallAnchor so BOTH it and
// the EventRefine Tier-A at-ball test build the series from ONE implementation
// (applyBallAnchor's byte-for-byte behaviour is guarded by ball_anchor_test).
struct ThetaBallSeries {
    std::vector<bool>    haveBall;    // one per frame — a stable ball resolved here
    std::vector<double>  thetaBall;   // atan2(B − G) radians (image atan2 convention)
    std::vector<QPointF> ballPx;      // ball centre (px); default where !haveBall
};

// Build the θ_ball series over the tUs grid. gx/gy are the per-frame grip anchors
// (px). frameW/H de-normalize the ball centres. Empty/mismatched inputs return an
// all-false series (haveBall.size() == tUs.size()).
ThetaBallSeries buildThetaBallSeries(const BallTrack2D &ball,
                                     const std::vector<double> &gx, const std::vector<double> &gy,
                                     const std::vector<int64_t> &tUs, int frameW, int frameH);

// Median grip→ball distance (px) over the ADDRESS HOLD PROPER — the swing's own
// per-camera club-length measurement (design §9.4): golfer set, club behind the
// ball, immediately before takeaway, where the straight grip→head shaft ends at
// the ball so |B−G| IS the club length. The window is derived here, not passed:
// the last contiguous quasi-still run of `still` (one char per coverage frame,
// nonzero = quasi-still) that ends at or spans bs0, intersected with
// [0, bs0+collar); null `still` / no such run near bs0 falls back to the
// trailing `collar` frames before bs0. Earlier still periods are deliberately
// excluded — teeing/setup is also quasi-still but with the hands AT the ball
// (tiny |B−G|), and it poisoned the whole-pre-takeaway median 100+ px short on
// the 2026-07-04 corpus. Mis-locks are rejected by a two-pass cluster gate
// (component-wise median ball position over the window, then accept within
// kMaxJumpNormPx of it) — order-independent, so a detector warm-up lock on the
// first frames cannot veto every later good sample the way a first-accepted
// chain gate can. Returns -1 with fewer than kMinLenSamples accepted samples.
// Shared so decideTrack can measure L_px BEFORE head placement (A1) instead of
// the old post-hoc path.
//
// Golf-prior plausibility gate: the cluster gate keeps any CONSISTENT lock, so
// a detector locked onto the driver head at address (gateA-0704: y 130–175 px
// above the truth ball) passed and shorted rung 1. Face-on the ball is always
// between the feet and below the ankle line, so when `ankles` (one entry per
// coverage frame; ok=false where the pose is absent) supplies at least
// kMinLenSamples usable frames inside the window, the pass-1 median ball
// position must sit below the median ankle line (margin 0.02·frameH) and
// inside the ankle x-extent widened 0.1·frameW per side — otherwise the
// measurement abstains (-1; `rejectReason` set 1 = ankle line, 2 = feet
// corridor, 0 = accepted/not gated) and the ladder degrades honestly to rung
// 3. Null / sparse ankles skip the gate (pose-free callers keep working).
//
// `clusterBallPx` (second consumer, P7 impact geometry): filled with the
// pass-1 cluster-median ball centre (px) ONLY on the accept path (return
// > 0), i.e. it inherits the cluster gate, kMinLenSamples, and the golf-prior
// gates for free — exactly the "reliable pre-swing ball XY" the impact
// crossing wants. Left untouched on abstain; nullptr callers byte-identical.
struct AnklePx {
    double lx = 0, ly = 0;   // left ankle (px)
    double rx = 0, ry = 0;   // right ankle (px)
    bool   ok = false;       // pose available this frame
};
double medianGripBallLenPx(const BallTrack2D &ball,
                           const std::vector<double> &gx, const std::vector<double> &gy,
                           const std::vector<int64_t> &tUs, int frameW, int frameH,
                           int bs0, int collar, const std::vector<char> *still = nullptr,
                           const std::vector<AnklePx> *ankles = nullptr,
                           int *rejectReason = nullptr,
                           QPointF *clusterBallPx = nullptr);

void applyBallAnchor(ShaftTrack2D &out, const BallTrack2D &ball,
                     const std::vector<double> &gx, const std::vector<double> &gy,
                     const std::vector<int64_t> &tUs, int frameW, int frameH,
                     int impf, const ShotAnalysisJob &job, ShaftDecideTrace *trace);

} // namespace pinpoint::analysis
