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

// Motion-overlay pose smoother (Phase 1) — an offline, NON-CAUSAL RTS smoother
// for the 133 COCO-WholeBody keypoints of a PoseTrack2D (0–16 the unchanged
// COCO body joints, tail = feet/face/hands — swing_analysis.h kWholeBodyJoints).
// It is a deliberate sibling of the clubhead Stage-2 temporal model
// (clubhead_track.h — HeadKf1D + runHeadTemporal): the same segmented-Kalman +
// per-segment RTS + honesty-tier idioms, taken one derivative order up. Where
// the club runs a 2-state [r, ṙ] constant-velocity / white-ACCEL scalar filter
// on the club-head radius, this runs a 3-state [p, v, a] constant-acceleration
// / white-JERK scalar filter — independently on the x and the y pixel
// coordinate of every keypoint (266 scalar filters, x ⊥ y). Read
// clubhead_track.{h,cpp} FIRST: the 3σ Mahalanobis gate, the coast budget, the
// trimTail-before-RTS, the confirmed-run flush and the Off/Pred/Meas tier
// vocabulary are all lifted from it near-verbatim; only the deltas below differ.
//
// Deliberate departures from the club model, each forced by the pose problem:
//   * 3-state (jerk) not 2-state (accel): keypoints accelerate hard through
//     impact, so a constant-velocity model lags the arc. The process noise is
//     white jerk (jerk is the noise term, NOT a stored 4th state) — the standard
//     discrete white-noise-jerk Q, one order up from the club's white-accel Q.
//   * VARIABLE dt per step (REQUIRED): PoseRunner samples non-uniformly (a dense
//     stride near impact, ~4× sparser through the mid-swing, ~100 ms coarse
//     address coverage). F(dt) and Q(dt) are rebuilt every step from the t_us
//     deltas, and each step stores its own dt for the backward RTS pass (the
//     club's dt is a single per-swing constant).
//   * PIXEL domain internally: input kp are normalized 0..1, but W ≠ H makes
//     measurement noise anisotropic in that domain, so we convert IN with (W,H),
//     smooth in px, and emit normalized again.
//   * Segmentation is coast-budget-in-TIME only (coastBudgetMs, not a frame count
//     — the sampling is non-uniform) plus the 3σ gate. There is no θ-jump analogue:
//     nothing external ever re-inits a keypoint, so the gate + the budget ARE the
//     whole segmentation policy (kept intentionally minimal).
//   * Confidence gate: conf < confMeasMin ⇒ that frame offers no measurement and
//     the filter coasts. The classic occluded trail-wrist at the top of the swing
//     becomes a gap that the per-segment RTS bridges — that bridge, rendered like a
//     real measurement, IS the point of the feature.
//
// Not done here (documented residuals — later phases own them):
//   * Hands (leadHand / trailHand / handConf) are copied through UNCHANGED: v1
//     does not smooth the hand centroids.
//   * Derived midpoints (pelvis / neck mids) are NOT computed here — they are a
//     linear combination of already-smoothed parent keypoints downstream, hence
//     equally smooth for free.
//
// Deterministic: same input ⇒ byte-identical output (no clock, no random anywhere),
// exactly like runHeadTemporal. Qt-only (QPointF, via swing_analysis.h) — no
// OpenCV, mirroring swing_analysis.h's cv-free convention. A single pure free
// function; no class, no factory (the segmented KF is a private .cpp helper).

#include "swing_analysis.h"   // PoseFrame2D, PoseKpAux, PoseTier (Qt-only)
#include "analysis_tuning.h"  // tuning::apply (header-only, Qt types only)
#include "../Core/pp_tuned_constants.h"   // tuned::pose::smoother::

#include <QVariantMap>

#include <cmath>
#include <vector>

namespace pinpoint::analysis {

// The LEGS group: COCO body keypoints 11–16 (L/R hip, knee, ankle). swing_analysis.h
// owns the wholebody-TAIL boundaries (kFootFirstKp / kFaceFirstKp / kLeftHandFirstKp);
// this pair names a range INSIDE the 17 COCO body joints, so it lives with the only
// code that treats it as a group. Shoulders/arms (0–10) are deliberately not a group —
// they move fast through transition and the wrist-tuned window is nearer right for them
// (metric_presentation_honesty.md §5.4: "measure before assuming").
inline constexpr int kLegFirstKp = 11;   // left_hip
inline constexpr int kLegLastKp  = 16;   // right_ankle
inline constexpr bool isLegKeypoint(int k) { return k >= kLegFirstKp && k <= kLegLastKp; }

// The full smoother parameter set. Every field defaults to a validated constant;
// see pose_smoother.cpp for how sigmaJerk was derived (the effective-bandwidth /
// fps-independence argument). Names/semantics are FROZEN — later phases integrate
// against them.
struct PoseSmootherConfig {
    // ── measurement acceptance ───────────────────────────────────────────────
    double confMeasMin    = 0.35;   // conf below this ⇒ no measurement (coast) — the
                                    //   club's CONF_MEAS_MIN, applied per keypoint.
    double measSigBasePx  = 2.0;    // σ_meas = measSigBasePx + (1−conf)·measSigSlopePx
    double measSigSlopePx = 6.0;    //   (px) — the club's measSigBase/Slope idiom.
    double gateSig        = 3.0;    // GATE_SIG — 3σ Mahalanobis innovation gate (per axis)

    // ── process model ────────────────────────────────────────────────────────
    // White-JERK process noise. q = sigmaJerk² drives the standard discrete
    // white-noise-jerk Q(dt). The default was tuned empirically (see the
    // derivation block in pose_smoother.cpp): on a stationary-point noise probe it
    // yields an effective smoothing window of ≈ 33 ms at 150 fps, ≈ 42 ms at
    // 30 fps — the fixed σ_jerk + variable dt makes the window auto-adapt (tighter
    // in the dense impact burst, ≈40–60 ms at the sparse phases where noise
    // averaging matters). Nudged up from the pure ≈42 ms-window value (1e5) for
    // 3σ-gate robustness on fast joints (a lower Q rejects valid fast measurements
    // and collapses segments); validated fps-independent in the tests.
    double sigmaJerk      = 2.0e5;  // px/s³

    // ── segmentation / bridging ──────────────────────────────────────────────
    double coastBudgetMs  = 250.0;  // coast budget in TIME (not frames). On overrun
                                    //   the coasted tail is trimmed and the segment
                                    //   closes; re-init on the next confident frame.
    int    runMin         = 4;      // RUN_MIN — confirmed-run length for the meas tier
                                    //   (tolerates a single-frame hole, club flush logic)

    // ── filter init covariance (loose priors; the RTS pass corrects early frames) ─
    double initSigPPx     = 10.0;   // KF init σ_p (px)
    double initSigV       = 4000.0; // KF init σ_v (px/s)  — a wrist can move fast
    double initSigA       = 6.0e4;  // KF init σ_a (px/s²)

    // ── per-group scales (wholebody tail; ADDITIVE, all default 1.0) ─────────
    // The COCO-WholeBody groups have different noise/dynamics profiles (hands
    // move much faster than hips through impact; the face barely moves), so
    // each non-body group gets a multiplicative scale on the measurement-σ
    // constants (measSigBasePx AND measSigSlopePx) and on sigmaJerk. Body
    // keypoints 0–10 ALWAYS use the frozen base values above — no scale ever
    // applies to them — and ×1.0 is exact in IEEE-754, so the defaults leave
    // every keypoint's output byte-identical to a pre-scale run. (Body 11–16
    // gained the legs group below in phase 4.1, on the same ×1.0-default terms.)
    double feetSigmaScale = 1.0;    // × measSigBasePx/measSigSlopePx, kp 17–22
    double faceSigmaScale = 1.0;    // × measSigBasePx/measSigSlopePx, kp 23–90
    double handSigmaScale = 1.0;    // × measSigBasePx/measSigSlopePx, kp 91–132
    double feetJerkScale  = 1.0;    // × sigmaJerk, kp 17–22
    double faceJerkScale  = 1.0;    // × sigmaJerk, kp 23–90
    double handJerkScale  = 1.0;    // × sigmaJerk, kp 91–132

    // ── legs group (COCO BODY kp 11–16: hips, knees, ankles) ─────────────────
    // The one exception to "body 0–16 always runs the frozen base constants",
    // and it exists because those constants were tuned on a WRIST: the ≈33 ms
    // window at 150 fps (see the derivation block in the .cpp) is far shorter
    // than anything a pelvis does, so the hip-derived series (pelvisSway,
    // hipLineTilt, plumbBobDistance, leadKneeDrift) keep keypoint noise the
    // smoother could have averaged away — metric_presentation_honesty.md §5.4.
    // Applied EXACTLY like the tail scales above: × measSigBasePx AND
    // measSigSlopePx, and × sigmaJerk, for keypoints 11–16 only. Keypoints
    // 0–10 and the wholebody tail are untouched.
    //
    // Both default to 1.0 (from the frozen constants header, the single
    // edit-point for the phase-4.3 flip): ×1.0 is exact in IEEE-754, so the
    // shipped defaults leave every keypoint byte-identical to the pre-phase-4
    // tree. `legWindowMsForJerkScale` below turns a candidate jerk scale into
    // the window it buys.
    double legsSigmaScale = pinpoint::tuned::pose::smoother::kLegsSigmaScale;
    double legsJerkScale  = pinpoint::tuned::pose::smoother::kLegsJerkScale;

    // SwingLab dark keys — the ONLY smoother parameters registered for a sweep.
    // Every other field above is frozen and deliberately unreachable from an
    // override map: they were validated together (window vs 3σ-gate robustness)
    // and a sweep that moved one of them alone would break that balance without
    // saying so. Empty map ⇒ the frozen defaults, byte-identical.
    static PoseSmootherConfig fromOverrides(const QVariantMap &ov)
    {
        PoseSmootherConfig c;
        tuning::apply(ov, "poseSmooth.legsSigmaScale", c.legsSigmaScale);
        tuning::apply(ov, "poseSmooth.legsJerkScale",  c.legsJerkScale);
        return c;
    }
};

// Predicted effective smoothing window (ms) for a candidate `legsJerkScale` — the
// sweep helper for phase 4.2, which is picking the scale that lands the 80–100 ms
// the design targets on the hips.
//
// The scaling is the Wiener-cutoff argument spelled out in the .cpp's derivation
// block: for a 3rd-order (white-JERK) process observed at spacing dt with
// measurement noise σ_m, the smoother's cutoff is ω_c = (σ_jerk²/(σ_m²·dt))^(1/6),
// so the window T = 1/ω_c ∝ σ_jerk^(−1/3) at fixed dt and σ_m. A scale s therefore
// multiplies the window by s^(−1/3) — and, on a stationary noisy point where the
// residual is pure noise averaging (σ_out = σ_in/√(T/dt)), divides the residual σ by
// s^(−1/6), i.e. multiplies it by s^(1/6).
//
// `baseWindowMs` is the MEASURED window at the current default, and 33 ms (150 fps,
// σ_jerk = 2e5) is the number the derivation block reports; pass 42.0 for a 30 fps
// track. This is a predictor for choosing sweep points, NOT a measurement: the law
// reproduces the measured 1e5→2e5 step to within 1 % and the 150→30 fps step to
// within 3 %, but overestimates the 3e5 window by ≈12 %. Judge a scale by the
// corpus, never by this function.
inline double legWindowMsForJerkScale(double scale, double baseWindowMs = 33.0)
{
    return (scale > 0.0) ? baseWindowMs * std::pow(scale, -1.0 / 3.0) : 0.0;
}

// Parallel outputs, both sized == frames.size(): a smoothed PoseFrame2D per input
// frame (same t_us grid) and its per-keypoint honesty aux (tier + posterior σ).
struct PoseSmootherOutput {
    std::vector<PoseFrame2D> smoothed;   // normalized kp; hands copied through
    std::vector<PoseKpAux>   aux;        // parallel per-frame tier/sigma
};

// Smooth an offline pose track. `frames` is one PoseFrame2D per posed frame in
// decode/time order (non-uniform t_us is expected). frameW/frameH are the source
// frame pixel dimensions used to de-normalize the kp for the pixel-domain filters.
//
// Output rules (the overlay paint-alpha contract):
//   * tier Meas  → kp = smoothed, conf = raw conf.
//   * tier Pred  → kp = smoothed/bridged, conf = max(raw conf, 0.5) (bridged points
//                  render like measured — that is the feature).
//   * tier Off   → kp = raw passthrough (byte-identical), conf = 0, sigma = 0.
// leadHand / trailHand / handConf are copied through unchanged (residual: v1 does
// not smooth the hands). Deterministic; empty in ⇒ empty out.
PoseSmootherOutput smoothPoseTrack(const std::vector<PoseFrame2D> &frames,
                                   int frameW, int frameH,
                                   const PoseSmootherConfig &cfg = {});

} // namespace pinpoint::analysis
