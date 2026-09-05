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

#include <QByteArray>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstdint>
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

// ── phase 5: the MOTION-ADAPTIVE window (PROMOTED 2026-09-05) ────────────────
// Why a per-frame window and not another static scale: phase 4.2 measured the
// static one and it failed on its own terms (the 2026-09-05 Log entry in
// metric_presentation_honesty_impl_plan.md). A global legsJerkScale of 0.1 buys the
// hips exactly the jitter reduction the window law predicts, but it moves the P7
// (impact) samples by 3–4 σ, because the hips move FAST through impact and a ≈70 ms
// window blends the post-impact rotation into the impact frame — and the corridors
// are seeded at P7. So the honest lever is a window that is long while the joint is
// quiet (the address hold, the P1→P4 sway, where noise averaging is all upside) and
// back to today's window while it accelerates (P6/P7).
//
// The mechanism is ONE multiplier on the process-noise variance q = σ_jerk² for the
// transition INTO a frame (Kf3::predict(dt, qScale) in the .cpp). Nothing else moves:
// not the 3σ gate, not the coast budget, not the segmentation, not the confirmed-run
// marking, not the per-group static scales, and nothing new is persisted. The RTS
// pass needs no changes at all because it reads the STORED predicted covariance and
// dt for each step, so a per-step Q is already in its arithmetic.
//
// ⚠ THE SCALE IS ON q, i.e. ON σ_jerk², AND THAT CHANGES THE EXPONENTS. The legs
// scales of phase 4 multiply σ_jerk, so `legWindowMsForJerkScale` reads window ∝
// s^(−1/3) and stationary residual σ ∝ s^(+1/6). A q scale s is a σ_jerk scale of
// √s, so in q terms window ∝ s^(−1/6) and residual σ ∝ s^(+1/12):
//        minScale = 0.05  →  window ×1.648 (33 → 54 ms at 150 fps), residual σ ×0.779
// NOT the ×2.71 / ×0.61 the same number means as a legsJerkScale. Do not read the
// two families of numbers off the same table.
enum class AdaptMode  : uint8_t { Off = 0, Accel, Innov };
enum class AdaptGroup : uint8_t { Legs = 0, Body };

// Constexpr C-string equality — one parser reads BOTH the frozen default in
// pp_tuned_constants.h and a sweep's override string, so an unrecognised value means
// the same thing in both places (see adaptModeFromName below).
inline constexpr bool adaptNameEq(const char *a, const char *b)
{
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
// Unrecognised ⇒ Off / Legs: the dark default. A sweep line with a typo therefore
// reads as a CONTROL run, which is the safe direction (this header has no log
// channel — pose_smoother is Qt-only by design — so it cannot complain).
inline constexpr AdaptMode adaptModeFromName(const char *s)
{
    return adaptNameEq(s, "accel") ? AdaptMode::Accel
         : adaptNameEq(s, "innov") ? AdaptMode::Innov
                                   : AdaptMode::Off;
}
inline constexpr AdaptGroup adaptGroupFromName(const char *s)
{
    return adaptNameEq(s, "body") ? AdaptGroup::Body : AdaptGroup::Legs;
}

struct AdaptConfig {
    // PROMOTED 2026-09-05: the shipped default is "accel" (the C15 gate row is in the
    // constants header). "off" remains exact rather than merely cheap — with it no scale
    // vector is ever built and predict() is never handed anything but 1.0, so the output
    // is byte-identical to the pre-phase-5 tree BY CONSTRUCTION, not by an arithmetic
    // identity, which is what a parity run against this feature has to lean on.
    AdaptMode  mode  = adaptModeFromName(pinpoint::tuned::pose::smoother::adapt::kMode);
    // Which keypoints adapt. "legs" = 11–16 (the hip/knee/ankle group phase 4 built),
    // "body" = 0–16. The wholebody TAIL (feet/face/hands, 17+) never adapts: the hands
    // are copied through unsmoothed anyway and the face/feet groups have their own
    // static scales, neither of which this design has measured.
    AdaptGroup group = adaptGroupFromName(pinpoint::tuned::pose::smoother::adapt::kGroup);

    // Clamp floor on the q scale. 0.01 (the swept default) ⇒ window ×2.154 (33 → 71 ms at
    // 150 fps) and stationary residual σ ×0.681 — see the exponent warning above.
    double minScale   = pinpoint::tuned::pose::smoother::adapt::kMinScale;
    // Acceleration that maps to scale 1.0 (today's window), px/s² AT A REFERENCE
    // FRAME WIDTH of adapt::kARefFrameWidthPx. |a| is a pixel quantity, so the run
    // scales this by (frameW / kARefFrameWidthPx) — see the constant's comment.
    double aRefPxS2   = pinpoint::tuned::pose::smoother::adapt::kARefPxS2;
    double expo       = pinpoint::tuned::pose::smoother::adapt::kExpo;      // contrast: s = (|a|/aRef)^expo
    // Symmetric ±leadMs running max on the scale vector. A DURATION, not a frame
    // count: the pose grid is non-uniform (≈27 ms at the address, 6.7 ms dense), so a
    // frame count would mean ±81 ms at the address and ±20 ms through impact. NB at a
    // 27 ms grid ±20 ms reaches no neighbour, so the filter is a no-op out there.
    double leadMs     = pinpoint::tuned::pose::smoother::adapt::kLeadMs;
    double innovRef   = pinpoint::tuned::pose::smoother::adapt::kInnovRef;  // innov policy divisor
    int    innovRun   = pinpoint::tuned::pose::smoother::adapt::kInnovRun;  // accepted-step window

    // TEST-ONLY (C14's hook): fill PoseSmootherOutput::adaptScale with the q scale
    // actually used per keypoint per frame so a test can assert the scale vector.
    // Never set in production — it costs one double per adapting keypoint per frame
    // and it is not persisted anywhere.
    bool   emitScalesForTest = false;

    // Does the adaptive window reach keypoint k? mode Off ⇒ never, tail ⇒ never.
    bool appliesTo(int k) const
    {
        if (mode == AdaptMode::Off) return false;
        if (k >= kFootFirstKp) return false;              // wholebody tail never adapts
        return (group == AdaptGroup::Body) ? true : isLegKeypoint(k);
    }
};

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
    // keypoints 0–10 ALWAYS use the frozen base values above — no STATIC scale
    // ever applies to them (the phase-5 per-frame adaptive window is the one
    // thing that can reach 0–10, and only with adapt.group == Body; it never
    // reaches the tail) — and ×1.0 is exact in IEEE-754, so the defaults leave
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

    // ── motion-adaptive window (phase 5, PROMOTED — see AdaptConfig above) ───
    // Composes with the static scales rather than replacing them: pass 1 of the
    // accel policy runs the group's static scales, and the per-frame q scale is a
    // multiplier ON TOP of that q. Since 2026-09-05 the shipped default is
    // mode "accel" over the legs group; mode "off" is the parity switch.
    AdaptConfig adapt{};

    // SwingLab keys — the ONLY smoother parameters registered for a sweep.
    // Every other field above is frozen and deliberately unreachable from an
    // override map: they were validated together (window vs 3σ-gate robustness)
    // and a sweep that moved one of them alone would break that balance without
    // saying so. An empty map ⇒ the frozen defaults, which since 2026-09-05 include
    // the PROMOTED adaptive window (adapt.mode "accel"); the pre-phase-5 output is
    // `adapt.mode = Off`, not the empty map.
    static PoseSmootherConfig fromOverrides(const QVariantMap &ov)
    {
        PoseSmootherConfig c;
        tuning::apply(ov, "poseSmooth.legsSigmaScale", c.legsSigmaScale);
        tuning::apply(ov, "poseSmooth.legsJerkScale",  c.legsJerkScale);

        // Phase-5 adapt keys. mode/group are STRINGS, and analysis_tuning.h has no
        // QString overload (nor is it this phase's file to extend), so those two are
        // read straight off the map here through the same constexpr name parser the
        // frozen defaults use — an unrecognised value therefore means "off"/"legs"
        // in a sweep exactly as it does in the header.
        if (const auto it = ov.constFind(QLatin1String("poseSmooth.adapt.mode")); it != ov.cend()) {
            const QByteArray name = it->toString().trimmed().toLower().toLatin1();
            c.adapt.mode = adaptModeFromName(name.constData());
        }
        if (const auto it = ov.constFind(QLatin1String("poseSmooth.adapt.group")); it != ov.cend()) {
            const QByteArray name = it->toString().trimmed().toLower().toLatin1();
            c.adapt.group = adaptGroupFromName(name.constData());
        }
        tuning::apply(ov, "poseSmooth.adapt.minScale",   c.adapt.minScale);
        tuning::apply(ov, "poseSmooth.adapt.aRefPxS2",   c.adapt.aRefPxS2);
        tuning::apply(ov, "poseSmooth.adapt.expo",       c.adapt.expo);
        tuning::apply(ov, "poseSmooth.adapt.leadMs",     c.adapt.leadMs);
        tuning::apply(ov, "poseSmooth.adapt.innovRef",   c.adapt.innovRef);
        tuning::apply(ov, "poseSmooth.adapt.innovRun",   c.adapt.innovRun);

        // Range guards on the two keys where an out-of-range value would be silently
        // WRONG rather than merely odd: minScale > 1 would make the "adaptive" window
        // SHORTER than today's everywhere (a q scale is a multiplier on the frozen q,
        // and > 1 is a different experiment than this design's), minScale < 0 would
        // invert the clamp, and an unbounded innovRun would let one sweep line hold a
        // multi-second memory of a single innovation. A sweep that asks for either gets
        // the nearest legal value, not a surprise.
        c.adapt.minScale = std::clamp(c.adapt.minScale, 0.0, 1.0);
        c.adapt.innovRun = std::clamp(c.adapt.innovRun, 1, 32);
        // leadMs < 0 is harmless (the max filter is a no-op) and expo is unbounded on
        // purpose — the sweep uses 1 and 2 and a larger contrast is a legitimate ask.
        // emitScalesForTest is deliberately NOT a key: it is a test hook, not a tunable.
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

    // TEST-ONLY, and empty unless cfg.adapt.emitScalesForTest is set (C14's hook):
    // adaptScale[k] is the per-frame q scale ACTUALLY handed to predict() for
    // keypoint k — 1.0 on a frame that started a segment or belongs to none. A
    // keypoint the adaptive window does not reach keeps an EMPTY row, so an empty
    // outer vector is the production case and costs nothing. Never persisted.
    std::vector<std::vector<double>> adaptScale;
    // TEST-ONLY, same flag: the accel policy's pass-1 |a| (px/s²) per frame — the
    // policy's actual INPUT, first-class rather than reverse-engineered from a clamped
    // scale. 0 on a frame with no smoothed value. Empty row for the innov policy,
    // which has no pass 1. Never persisted.
    std::vector<std::vector<double>> adaptAccel;

    // ALWAYS populated (not a test hook): how many keypoints had their adaptive pass
    // rejected by the divergence guard and fell back to the unadapted output. 0 with
    // the window off, and 0 is the only value the C15 gate should ever accept
    // silently — see the guard in pose_smoother.cpp. Copied onto PoseTrack2D and
    // written to swing.json as analysis.pose2d.adaptFallbacks when non-zero.
    int adaptFallbacks = 0;
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
