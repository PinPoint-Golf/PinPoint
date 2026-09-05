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

#include <cstdint>

// SINGLE SOURCE OF TRUTH for analysis-pipeline parameters that are tuned during
// the validation programme and then FROZEN (docs/validation/pipeline_validation_and_tuning.md
// §2.4). When validation locks a value, edit it HERE and every consumer — live IMU
// path AND offline analyzer — picks it up; the tuned_constants_parity_test guards that
// the indirection stays byte-identical to the historical defaults.
//
// These are the DEFAULTS. SwingLab sweeps them at run time via the dotted-key
// `tuningOverrides` mechanism (analysis_tuning.h) without rebuilding — this header is
// not consulted on the override path, only for the baseline value an override starts from.
//
// LAYERING: src/Core is the lowest common layer (both src/IMU and src/Analysis already
// include from it). To keep `orientation_filter.h` Qt-free and standalone-unit-testable,
// this header carries ONLY numeric literals (no Qt types, no module types). Quaternions
// are expressed as {w, x, y, z} float arrays; imu_calibration.h builds the QQuaternion.
//
// NOT here (deliberately): the wrist axis SIGN conventions and ZXY decomposition in
// src/Analysis/wrist_angles.h — those are sign/axis CHOICES baked into code structure,
// not single numeric literals, and stay at their source of truth. The reference-band lo/hi
// corridor arrays are no longer code at all — they are norm rows in
// src/Resources/diagnostics/norms.json, edited as content and re-seated from a corpus.

namespace pinpoint::tuned {

// --- Orientation filter (src/IMU/orientation_filter.h) -----------------------------
namespace filter {
// Madgwick gradient-descent gain: higher trusts the accelerometer more. ~0.03–0.1
// typical for consumer MEMS. The phase-adaptive schedule (validation §5.3.1) starts
// from this as its static-phase value.
inline constexpr float kBeta = 0.05f;
} // namespace filter

// --- Stillness-gated seed tolerances (src/IMU/imu_base.h) --------------------------
namespace seed {
inline constexpr float kInitAccelTolG      = 0.15f;   // |‖a‖ − 1g| within this ⇒ "still"
inline constexpr float kInitGyroMaxRadps   = 0.5f;    // ~28 °/s gyro magnitude ceiling
inline constexpr int   kInitMaxSeedAttempts = 200;    // ~1 s @200 Hz, ~2 s @100 Hz fallback
} // namespace seed

// --- Anatomical calibration (src/IMU/imu_calibration.h) ----------------------------
namespace calib {
// Functional-axis orthogonality gate: the two solved joint axes are anatomically ~90°;
// outside this band the calibration capture was poor and is rejected.
inline constexpr float kAxisAngleMinDeg = 60.0f;
inline constexpr float kAxisAngleMaxDeg = 120.0f;

// Nominal mount quaternions {w, x, y, z} (anatomical-segment-body -> sensor-body, M).
// Arm strap convention (gravity-pinned signs, 2026-05-31); all three arm segments share it.
inline constexpr float kNominalArmMount[4]  = { 0.5f, -0.5f, -0.5f, -0.5f };
// Hand (dorsal) mount, solved numerically from a characterization capture.
inline constexpr float kNominalHandMount[4] = { 0.4388f, 0.6054f, 0.4965f, -0.4409f };
} // namespace calib

// --- HackMotion frame reconciliation (src/IMU/hm_frame.h) --------------------------
//
// The constant rotation carrying the device's post-calibration anatomical frame onto
// ours. ⚠ THIS IS A MOUNTING CONSTANT, NOT A DEVICE CONSTANT: it describes where the
// boards sit on the strap, so a changed strap position changes it. Record the mounting
// alongside any value written here.
//
// Only the SELECTION lives in this header — the four candidates themselves are a
// structural fact about the two frames, not tuned numbers, so they stay at their source
// of truth in hm_frame.h (same reason wrist_angles.h keeps its own axis choices). What
// is tunable is which of the four a given mounting selects, via "hmframe.candidate".
namespace hmframe {
// Index into hm_frame.h's candidate table; -1 = NOT YET SELECTED, which is the
// honest state until a directed capture has selected one. While it is -1 a
// HackMotion lane reports no anatomical frame at all and drives nothing — never a
// plausible-looking guess, which is the one failure mode this phase exists to avoid.
inline constexpr int kCandidateUnset = -1;

// SELECTED: C2 — Ry(+90°), x→−z, y→+y, z→+x.
//
// ⚠ MOUNTING THIS DESCRIBES: wG3 `wg3-mount1`, capture of 18 Aug 2026
// (`phased2.wrwire`). Move the strap and this must be re-selected.
//
// Chosen by `tools/hm_frame_select.py` from a directed capture, on the sign of
// a known bow and a known ulnar deviation — NOT on cross-talk, which all four
// candidates pass identically. Evidence from that capture:
//   · calibration applied     relative angle 6.82° → 2.03° across the 0x94
//   · axis roles MEASURED     flexion joint axis 19.7°/14.5° from device X,
//                             deviation 16.5°/14.4° from device Z, each ≥74.6°
//                             from the nearest other axis
//   · limb axis MEASURED      forearm rate axis 2.0° from device Y, 99%
//                             single-axis
//   · sign consistency        2/2 bursts agreed in each DOF
//
// ⚠ C2 inverts flexion relative to the device's own reported sense, and that is
// the expected result, not a red flag: we report ISB (flexion positive) and the
// vendor reports the inverse on bow/cup. A correct selection looks wrong beside
// their application. See docs/design/pinpoint_sign_conventions.md Rule 0.
//
// REPEATABILITY — MEASURED, not assumed. Three captures on one uninterrupted
// mounting (`phased2/3/4.wrwire`), each with its own run of the calibration
// routine, all select C2; the two richer ones agree on all THREE DOFs including
// the rotation. So the selection is a property of the mounting and not of the
// calibration attempt, which is what baking it in requires.
//
// ⚠ AND A REPRODUCIBLE ~17-18° RESIDUAL, WHICH IS NOT A FRAME ERROR AND MUST NOT
// BE "CORRECTED" AWAY. Real single-axis motions sit 17.0/13.9/20.9° off the
// device X for flexion and 15.5/21.6/18.2° off the device Z for deviation, and
// the offsets point the same way across captures (deviation means agree to
// 3.7-10.2°). Two reasons that is a measurement of the golfer rather than of the
// device: both frames here are CONVENTIONAL — defined by landmarks, not by
// measured helical axes — and a real human wrist's flexion and deviation axes
// are oblique to them, so the residual is what an anatomy-versus-convention gap
// looks like. `wrist_angles.h` already records 10-15° of the same family on our
// own Witmotion lane. Folding it into R would bake ONE golfer's anatomy into a
// mounting constant, which is precisely the one-golfer trap that stalled the
// earlier per-swing wrist work. Recorded here so the next reader treats an 18°
// cross-talk reading as expected rather than as a defect.
inline constexpr int kCandidate = 1;
} // namespace hmframe

// --- Swing scoring bands + deadbands (src/Analysis/swing_scorer.cpp) ----------------
namespace scoring {
// Deadband + bounded falloff (design §B.1): |z| ≤ kZIn ⇒ 100; ramps to ~0 at kZOut.
inline constexpr double kZIn       = 1.0;
inline constexpr double kZOut      = 3.0;
inline constexpr double kFalloffPow = 2.0;

// PROVISIONAL Wrist (session type 1) reference bands — μ, σ, oneSidedDir, weight.
// See docs/reference/wristmetrics.md; locked against HackMotion in Corpus 2.
namespace bands {
inline constexpr double kFlexExtMu = 15.0, kFlexExtSigma = 12.0, kFlexExtWeight = 0.45;
inline constexpr int    kFlexExtOneSided = +1;   // penalise BELOW μ (cupping)
inline constexpr double kRadUlnMu  = 0.0,  kRadUlnSigma  = 12.0, kRadUlnWeight  = 0.15;
inline constexpr int    kRadUlnOneSided = 0;     // two-sided
inline constexpr double kPronationMu = 0.0, kPronationSigma = 25.0, kPronationWeight = 0.20;
inline constexpr int    kPronationOneSided = 0;  // two-sided
inline constexpr double kArmFlexionMu = 5.0, kArmFlexionSigma = 12.0, kArmFlexionWeight = 0.20;
inline constexpr int    kArmFlexionOneSided = -1; // penalise ABOVE μ (bent lead arm)
} // namespace bands

// PROVISIONAL per-archetype lead-wrist FE resemblance centres (design §B.0a; validation
// §5.6/§6.3). Flexion-positive, neutral-relative degrees, sampled at Top and Impact. v1
// scores FE only. σ_p is the pattern's COACHING TOLERANCE (its natural spread), NOT sensor
// noise (§B.7). Externally anchored to HackMotion published tour ranges (top −30/+5, impact
// −15/−40, extension-positive → ×−1 to flexion-positive here): the bowed centre IS the tour
// reference; neutral and cupped are extrapolated away from it. NOT FINAL — re-seated against
// the corpus + HackMotion concurrent capture at Corpus 2; frozen (not swept) until then.
namespace resemblance {
inline constexpr double kBlendedDeltaPts = 10.0;   // top-two within this ⇒ "blended"
inline constexpr double kBowedMuTop   =  13.0, kBowedMuImpact   =  27.0, kBowedSigma   = 18.0;
inline constexpr double kNeutralMuTop =  -8.0, kNeutralMuImpact =   5.0, kNeutralSigma = 18.0;
inline constexpr double kCuppedMuTop  = -30.0, kCuppedMuImpact  = -18.0, kCuppedSigma  = 18.0;
} // namespace resemblance

// Score measurement-uncertainty budget (design §B.7) — the per-cell angle error that
// PROPAGATES INTO a score interval, kept strictly separate from the band σ (which is
// coaching tolerance). σ_sensor + σ_crosstalk are the FE error floor; the timing term is
// dθ/dt × phase-timing jitter, inflated by low phase confidence (so low confidence WIDENS
// the interval, never moves the central score). σ_crosstalk is the ~10–15° systematic
// FE↔RUD leak carried conservatively as uncertainty until Corpus 2 localises it (C4).
namespace uncertainty {
inline constexpr double       kSensorSigmaDeg    = 6.0;     // IMU FE noise (~5–8°)
inline constexpr double       kCrosstalkSigmaDeg = 12.0;    // FE↔RUD leak (~10–15°), until Corpus 2
inline constexpr std::int64_t kTimingSigmaUs     = 10000;   // phase-timing jitter (~10 ms) × dθ/dt
inline constexpr double       kConfInflate       = 1.5;     // σ_x ×= 1 + (1−conf)·this — low conf widens
inline constexpr double       kIntervalSigmas    = 1.0;     // coverage factor on σ(d²)
} // namespace uncertainty
} // namespace scoring

// --- Wrist-angle windowed-median sampler (src/Analysis/wrist_angle_sampler.h) -------
namespace sampler {
inline constexpr std::int64_t kWindowHalfUs       = 15000;  // ±15 ms about Pn
inline constexpr double       kGimbalThresholdDeg = 75.0;   // pitch-proxy ≥ this ⇒ Indeterminate
inline constexpr int          kMinValidSamples    = 1;      // fewer in window ⇒ Gap
} // namespace sampler

// --- Assessment rule engine (src/Analysis/assessment_rule.h) ------------------------
namespace rules {
inline constexpr float  kConfidenceFloor              = 0.45f; // below ⇒ lowConfidence (demoted)
inline constexpr double kScoreScale                   = 18.0;  // score v2 penalty scale
inline constexpr double kSeverityWeightFault          = 1.0;
inline constexpr double kSeverityWeightWatch          = 0.5;
inline constexpr double kCorroborationBoost           = 0.30;  // confidence add when corroborated
inline constexpr bool   kStrengthsRequireAdjacentFault = true;
// Discrimination thresholds (validation C1 / A.5 #15) — the most behaviourally-decisive cut
// points, lifted out of the .cpp so they are one source of truth + parity-guarded + sweepable.
// FROZEN until labels exist (supervised fault-rule calibration; SwingLab refuses score.*/rules.*).
inline constexpr double kFlipFaultDeg          = -8.0;  // F3: P6→P7 FE drop ≤ this ⇒ Fault
inline constexpr double kFlipWatchDeg          = -5.0;  // F3: ≤ this ⇒ Watch
inline constexpr double kTrailFlattenDeg       = -8.0;  // flip corroboration: trail-wrist P6→P7 drop
inline constexpr double kArchetypeTopDeltaDeg  = 10.0;  // detectArchetype: |FE Δ@Top| ⇒ bowed/cupped
// The archetype face-corridor shift. No longer APPLIED from here: it now exists as 16 norm rows
// (8 positions × bowed/cupped, lead-wrist flex-ext) under the archetype contexts, which is what
// lets it be re-seated per position rather than staying flat. Kept as the frozen record of the
// value those rows were migrated from — see the plan's ledger C1d.
inline constexpr double kArchetypeFaceOffsetDeg = 10.0; // archetype face-corridor shift (±)
} // namespace rules

// --- Offline pose accuracy: person crop + DARK decode (wholebody_pose_design.md §3) ---
// WB1 upgrades the OFFLINE ViTPose pass only (PoseRunner); the live 60 Hz MoveNet
// path is untouched. Both upgrades default ON; setting crop.kEnabled AND decode.kDark
// false via "pose.*" overrides reproduces the pre-WB1 full-frame + argmax pipeline
// byte-for-byte (the WB1 parity gate). Consumed by PoseAccuracyConfig::fromOverrides
// (src/Analysis/pose_crop.h) and PoseRunner.
namespace pose {
// Offline ViTPose ORT intra-op thread count (PoseRunner → PoseEstimatorViTPose::load).
// The offline pose pass is 70%+ of analysis wall-time on CPU hosts, so pool sizing
// matters. THREE-WAY resolution via the "pose.intraOpThreads" dotted key:
//   > 0   → pin exactly this many intra-op threads (manual override)
//   == -1 → topology auto: clamp(pinpoint::physicalCoreCount(), 1, 16) from
//           src/Core/cpu_topology.h — OPT-IN, deliberately NOT yet the default (a
//           determinism A/B on the affected hardware — no-SMT, hybrid P/E-core,
//           >16-logical — is owed before it can become the default)
//    0    → (DEFAULT) today's proxy heuristic clamp(hardware_concurrency()/2, 1, 8),
//           left UNCHANGED so the default path stays byte/thread-count-identical to
//           the historical behaviour
// The live 60 Hz MoveNet path is untouched (pinned at 1 in its own estimator).
inline constexpr int kIntraOpThreads = 0;   // pose.intraOpThreads (0 = legacy auto)

namespace crop {
inline constexpr bool   kEnabled       = true;
inline constexpr double kMarginFrac    = 0.15;   // bbox expansion each side (arms/club headroom)
inline constexpr double kMaxAreaFrac   = 0.90;   // crop ≥ this fraction of frame area ⇒ no gain ⇒ full-frame
inline constexpr int    kMinBboxFrames = 3;      // < this many contributing bbox frames ⇒ full-frame fallback
} // namespace crop
namespace decode {
inline constexpr bool kDark = true;              // DARK sub-pixel decode (else argmax + ±0.25)
} // namespace decode
// Per-group confidence-threshold scales (design §3.4). REGISTERED here so a
// "pose.confScale.*" sweep resolves to a value; WB1 wires NO consumer (the
// wholebody-group consumers land in WB2/WB3). Body thresholds stay frozen (×1.0).
namespace confScale {
inline constexpr double kFeet  = 1.0;
inline constexpr double kFace  = 1.0;
inline constexpr double kHands = 1.0;
} // namespace confScale
// WB4 hand consumers (wholebody_pose_design.md §2.2). Both ship DARK: with the
// defaults below the pipeline output is byte-identical to the pre-WB4 tree.
namespace grip {
// Recompute leadHand/trailHand/handConf from the SMOOTHED hand keypoints after
// the RTS smoother, so the grip anchor ShaftTracker consumes inherits the
// smoother's honesty tiers. false ⇒ smoothed hands copied through unchanged.
inline constexpr bool kFromSmoothedHands = false;   // pose.gripFromSmoothedHands
} // namespace grip
namespace wristAngles {
inline constexpr bool   kEnabled         = false;   // pose.wristAngles.enabled — IMU-less pose source
inline constexpr double kConfMin         = 0.30;    // per-keypoint conf gate (codebase-wide)
                                                    //   ⚠ THIS GATE DOES NOT FILTER THE HAND. Measured
                                                    //   over 83 swings it rejects 0.2% of lead and 2.6%
                                                    //   of trail frames, and the reported confidence
                                                    //   correlates with the frame's ACTUAL error at
                                                    //   −0.00 / +0.06. For hand keypoints, confidence
                                                    //   is not optimistic — it is uninformative, so
                                                    //   nothing downstream may treat this as a
                                                    //   guarantee that bad frames were excluded.
                                                    //   kFeLimitDeg below is what actually fires.
inline constexpr double kApparentPenalty = 0.5;     // camera-plane apparent-angle confidence factor
                                                    //   (× min endpoint conf) — these are PROJECTED,
                                                    //   not anatomical, angles so trust is halved
// An |apparent FE| this large is not a wrist. The corpus distribution is cleanly
// BIMODAL: a real lobe (median 34–38°, p90 55–64°) and a detector-failure lobe at
// 167–180° where the hand root and middle-MCP collapse or invert. Frames in the
// second lobe used to reach the curve, and because the angle is an atan2 result they
// carried it across the ±180 branch cut — 22 of 83 trail swings held a 360° step in a
// series that is displayed and graded. Dropping them costs 2.7% of lead and 3.3% of
// trail frames; the lobes are separated widely enough that any limit from 90° to 150°
// selects near-identical frames, so this is a threshold in a gap, not a tuned edge.
inline constexpr double kFeLimitDeg      = 120.0;   // pose.wristAngles.feLimitDeg
// The band a wrist can actually produce. Measured on the CRITERION instrument rather
// than assumed: the HackMotion lead-wrist series over P1→P7 holds 95% of its energy
// below 1.9 Hz, 99% below 2.7 Hz and 99.9% below 3.7 Hz, so 6 Hz keeps all of the real
// signal with ~1.6× headroom and everything above it is keypoint noise.
//
// Drives the σ estimate always, and the emitted curve only when kFilterCurve is on.
// 0 disables the filter entirely (lowpassZeroPhase returns its input unchanged), which
// also withdraws σ.
inline constexpr double kFcHz            = 6.0;     // pose.wristAngles.fcHz
// Emit the FILTERED curve rather than the raw one. Off, and there is currently no
// evidence that would turn it on — which is the point of it being a switch rather than
// a comment saying "flip this later".
//
// The physics is sound: above ~4 Hz is not wrist motion, so the jitter it removes is
// provably noise. What is missing is any reason to believe the filtered VALUE is more
// correct. Two measurements say it is not, or at least not measurably:
//
//   · on the lead hand, where HackMotion gives a right answer, filtering improved
//     agreement with the criterion by +0.03 — it removes noise the correlation was
//     already averaging out, and does not touch the projection error that dominates;
//   · it moves ~23% of the graded m_trailWristFlexExt_* readings between bands at EVERY
//     position, including p4 at 31% — the one corridor here that is well seated. The
//     earlier reasoning that this churn sat inside the questionable p6/p7 corridors was
//     measured and is false: withholding those two leaves 24% → 23%.
//
// So flipping this needs a CRITERION ON THE TRAIL WRIST, not a bigger corpus. More
// swings through one camera cannot separate "the filter improved the value" from "the
// filter moved the value". Until then the honest position is that the curve a user reads
// is the one the detector produced, with σ saying how much of it is noise.
inline constexpr bool   kFilterCurve     = false;   // pose.wristAngles.filterCurve
} // namespace wristAngles

// Offline RTS pose smoother — the LEGS group (metric_presentation_honesty.md §5.4,
// phase 4.1; consumed by PoseSmootherConfig in src/Analysis/pose_smoother.h via the
// "poseSmooth.*" dotted keys). Multiplicative scales on the measurement-σ constants
// (measSigBasePx AND measSigSlopePx) and on σ_jerk, applied to the COCO body
// keypoints 11–16 (L/R hip, knee, ankle) ONLY. Keypoints 0–10 keep the frozen
// values, exactly as the feet/face/hand tail scales leave the body alone.
//
// Why the group exists: ONE σ_jerk (2.0e5 px/s³) is shared by every body keypoint and
// it was tuned on a WRIST — the derivation block in pose_smoother.cpp puts its
// effective smoothing window at ≈33 ms at 150 fps. A hip does nothing at that
// timescale, so the hips inherit the wrist's window and every hip-derived series
// (pelvisSway, hipLineTilt, plumbBobDistance, leadKneeDrift) carries keypoint noise
// the smoother could have averaged away: p95 frame-to-frame jitter of 3.1° on
// hipLineTilt and a whole-swing PK RATE of 291°/100 ms on the motivating swing.
//
// The window scales as σ_jerk^(−1/3) (the Wiener-cutoff argument in pose_smoother.cpp,
// and `legWindowMsForJerkScale` in pose_smoother.h computes it), so a jerk scale of
// ≈0.05 lands the 80–100 ms the design targets and ≈0.1 lands ≈71 ms.
//
// ⚠ BOTH SHIP AT 1.0 AND THAT IS DELIBERATE. ×1.0 is exact in IEEE-754, so with these
// defaults every keypoint of every frame is byte-identical to the pre-phase-4 tree and
// no persisted value[] moves. Changing a default here is phase 4.3: Mark's decision
// after a corpus before/after WITH a control run (pose is non-deterministic — ~20
// metrics differ at 1e-14 between identical runs), because it is the only phase of
// this design that moves value[], and the P4 corridor content was seeded on the
// current smoothing (docs/design/norm_shapes.md).
namespace smoother {
inline constexpr double kLegsSigmaScale = 1.0;   // poseSmooth.legsSigmaScale — × measSigBasePx/measSigSlopePx, kp 11–16
inline constexpr double kLegsJerkScale  = 1.0;   // poseSmooth.legsJerkScale  — × sigmaJerk, kp 11–16
} // namespace smoother
} // namespace pose

// --- Head tracking (WB2 — src/Analysis/head_track.h) --------------------------
// Head position/tilt from the COCO-WholeBody head keypoints (nose/eyes/ears +
// optional chin). Consumed by HeadTrackConfig::fromOverrides via "head.*" dotted
// keys. FROZEN defaults; SwingLab sweeps them without rebuild.
namespace head {
inline constexpr double  kConfMin        = 0.30;    // per-keypoint conf gate (codebase-wide)
inline constexpr double  kEarIpdFactor   = 1.8;     // inter-ear ≈ 1.8× inter-eye (anatomical bi-tragion
                                                    //   vs inter-pupillary ratio) — head-scale fallback
inline constexpr double  kEarWidthMm     = 145.0;   // nominal inter-ear (bi-tragion) breadth, mm — the
                                                    //   head-plane px→mm ruler for head sway/lift until
                                                    //   2D camera calibration lands (head.earWidthMm)
inline constexpr double  kChinConfWeight = 0.0;     // chin (kp 31) centroid weight when confident; 0 ⇒
                                                    //   OFF (body 0–4 only — face channels may be noisy)
inline constexpr int     kMinContribPts  = 2;       // min confident head kps for a valid head centre
inline constexpr int     kAddrMinFrames  = 5;       // fallback address ref = first N valid frames
inline constexpr std::int64_t kAddrWindowUs = 250000; // ±window about the Address event for the robust ref
} // namespace head

// --- Ball stance corridor v2 (WB3 — src/Analysis/ball_runner.cpp) -------------
// Toe/heel span + ground line replace the ankle-based v1 corridor when foot
// keypoint coverage is sufficient (wholebody_pose_design.md §2.1/§5). Consumed
// by BallCorridorConfig::fromOverrides via "ball.corridor.*" dotted keys.
// kUseFeet=false, or too few feet-confident frames, falls back VERBATIM to the
// v1 ankle path — byte-identical on legacy 17-kp tracks (conf[17..] == 0).
namespace ball {
namespace corridor {
inline constexpr bool   kUseFeet     = true;   // false ⇒ ankle path only (pre-WB3 behaviour)
inline constexpr double kFootConfMin = 0.30;   // per-keypoint conf gate (codebase-wide convention)
inline constexpr int    kMinFrames   = 5;      // < this many feet-confident frames ⇒ ankle fallback
} // namespace corridor

// --- Club-corridor activity (W3 — src/Analysis/ball_runner.cpp) ---------------
// Per-frame "is the club moving near the ball" signal, computed over an ANNULUS
// around the locked ball centre (inner radius excludes the ball disc so ball-lock
// jitter isn't read as activity; outer covers the resting clubhead beside it):
//   act = mean(|crop − medRef|) / σ
// medRef = rolling per-pixel temporal median of the previous kActivityRefFrames
// gray crops (a bob dwells at its extremes, so a median reference beats a raw
// frame-diff), σ = the crop's robustNoise (exposure/noise normalisation). Feeds
// the NAMED PAIR of consumers — (1) addressHoldEndFrame's club-quiet mask
// (shaft_positions.h) and (2) the EventRefine Tier-B at-ball activity gate
// (event_refine.h) — corroborating that the address hold is quiet at the CLUB,
// not just the grip (a club bob about a frozen wrist is invisible to the
// grip-only stillness test). Still never tk0 / length / launch / DP evidence
// (ball_anchor_test asserts applyBallAnchor is invariant to it).
// Consumed by BallActivityConfig::fromOverrides via "ball.*" dotted keys.
// kClubActivity=false ⇒ NO crop retention / ring buffer / annulus math ⇒ the ball
// track and swing.json are byte-identical (and code-path-identical) to pre-W3.
namespace activity {
// FROZEN ON 2026-07-18 with refine::kEnabled (activity is the load-bearing EventRefine
// Tier-B input); live cost ballMs +207 ms median on the corpus run. 0 still disables.
inline constexpr bool   kClubActivity      = true;  // ball.clubActivity — master gate
inline constexpr int    kActivityRefFrames = 9;     // ball.activityRefFrames — median-ref ring depth
inline constexpr double kActivityInnerR    = 1.5;   // ball.activityInnerR — inner annulus radius (× ball r)
inline constexpr double kActivityOuterR    = 5.0;   // ball.activityOuterR — outer annulus radius (× ball r)
} // namespace activity

// tk0 Address override A/B (W4 — src/Analysis/ball_anchor.cpp). FROZEN OFF
// 2026-07-17: the earliest-departure tk0 fires on the first FIDGET departure
// and overwrote a good hold-end Address (w2s4: −0.134 s → −1.533 s; part of
// the 17-swing truth freeze, Address-error median 0.564 → 0.060 s). true
// restores the old overwrite for A/B comparison. Long-term tk0 is conceptually
// the Takeaway instant, not the Address hold end — see the ball_anchor.cpp
// TODO. "ball.tk0AddressOverride" dotted key.
inline constexpr bool kTk0AddressOverride = false;
} // namespace ball

// --- Layer B P-position extraction (src/Analysis/shaft_positions.h) ------------
// "positions.*" tuning. Most PositionsConfig defaults are struct literals; the
// club-quiet sigma is frozen here because it is the W3 addition consumed by
// addressHoldEndFrame's optional club-quiet mask — a frame counts as club-quiet
// when its ball-corridor activity (ball::activity) is below this many robustNoise
// σ. SwingLab sweeps "positions.p1ClubQuietSigma"; the mask is only built when the
// ball track actually carries activity, so a dark ball.clubActivity ⇒ no mask ⇒
// the legacy grip-only hold-end (byte-identical).
namespace positions {
inline constexpr double kP1ClubQuietSigma = 3.0;   // positions.p1ClubQuietSigma
} // namespace positions

// --- Setup + footwork metrics (WB3 — src/Analysis/foot_metrics.h) ------------
// Stance width / per-foot flare / toe-line angle (address) + the lead-heel-lift
// trace, from the COCO-WholeBody foot keypoints (bigtoe/heel). Consumed by
// FootMetricsConfig::fromOverrides via "foot.*" dotted keys. FROZEN defaults;
// SwingLab sweeps them without rebuild. Mirrors head:: exactly (same defaults
// for the shared conf-gate / address-reference-window shape).
namespace foot {
inline constexpr double       kConfMin       = 0.30;    // per-keypoint conf gate (heel + bigtoe)
inline constexpr int          kAddrMinFrames = 5;       // fallback address ref = first N valid frames
inline constexpr std::int64_t kAddrWindowUs  = 250000;  // ±window about the Address event for the robust ref
} // namespace foot

// --- Sparse-channel resample (src/Analysis/metric_channel.h) ------------------
// Every face-on producer resamples its sparse channel onto the full frame grid,
// holding at the ends and bridging gaps so the curve is continuous and never NaN.
// kMaxBridgeUs is where a bridge stops being a measurement: a grid sample farther
// than this from any real channel sample is filled as before but MARKED INVALID in
// MetricSeries::valid, so the reducers skip it, the chart draws it dashed and no
// phase sample is taken there.
//
// 60 ms is chosen so that today's behaviour survives where it was honest and stops
// where it was not: a one- or two-frame confidence dropout at 150 fps (7–13 ms) still
// bridges silently, and a run of frames a geometry gate refused — a body line turned
// out of the image plane for a tenth of a second — does not.
//
// ⚠ A FIXED BUDGET IS NOT ENOUGH, and kBridgeSpacingFactor is why. The grid is NOT
// uniformly sampled: PoseRunner poses every frame only inside the dense zone, and the
// address region is sampled at addressStride 15 (≈100 ms at 150 fps) or coarseStride 12
// (≈80 ms) — pose_runner.h. A single dropped frame there is 80–100 ms from its
// neighbours, so a fixed 60 ms would mark it invalid, and that is the wrong answer: in a
// sparsely posed STILL address, holding the previous value across one missing sample is a
// hold, not a fabrication. So the allowance is
// max(maxBridgeUs, kBridgeSpacingFactor × the local grid spacing). Mid-swing the grid is
// dense (≈8 ms) and the 60 ms floor dominates, so a genuinely gated run is still marked.
// At 1.5 a sample bridges while it is within one-and-a-half spacings of a measurement, so a
// hole of one or two missing samples still holds and the middle of a hole of three or more
// is marked — one dropped frame is a hold, a run of them is a fabrication.
//
// Consumed by LowerBodyConfig / UpperBodyConfig via the "channel.*" dotted keys.
namespace channel {
inline constexpr std::int64_t kMaxBridgeUs        = 60000;   // channel.maxBridgeUs
inline constexpr double       kBridgeSpacingFactor = 1.5;    // channel.bridgeSpacingFactor
} // namespace channel

// --- Robust series reducers (src/Analysis/series_reduce.h) ---------------------
// The windows the CHART SUMMARY and the DIAGNOSTICS PHASE GRID both reduce with, so the card and
// the corridors cannot disagree about the same word. Design
// docs/design/metric_presentation_honesty.md §5.2; the At window is sampler::kWindowHalfUs above,
// which measure_sample.cpp has always used and which the chart now adopts.
//
// Both are stated in TIME because the grid is not uniformly sampled (≈8 ms inside the dense pose
// zone, 27 ms outside it, 80–100 ms at the address end — pose_runner.h): a window counted in
// SAMPLES would be 40 ms mid-swing and half a second at address.
//
// kExtremumWindowUs (40 ms) is the support a PEAK has to have. The extremum is taken over the
// centred-window MEAN, so a one-sample outlier cannot be the peak — it has to be there for 40 ms.
// 40 ms is ≈5 samples in the dense zone and ≈2 outside it: long enough that the jitter (0.5° median,
// 3.1° at the 95th percentile on the 2026-08-18 corpus swing) averages down by √5, and short enough
// that a real excursion is not flattened — the pelvis and thorax quantities this protects move over
// 100–300 ms, and impact itself is located to ±5 ms by other means, not by this.
//
// kRateWindowUs (50 ms) is the minimum TIME BASE a rate may be fitted over, and it is the whole fix
// for the PK RATE numbers in design §2: an adjacent-frame difference at 8 ms spacing turns half a
// degree of jitter into 6°/100 ms and the 95th-percentile 3° into 37°/100 ms. A least-squares slope
// over 50 ms divides the noise by roughly √n and the time base by 6. Reported per 100 ms as it
// always has been.
//
// kMinExtremumSamples (3) is what stops the centred window from being a SINGLE SAMPLE where the
// grid is sparse, which is the failure the corpus probe found after the first cut of this work: at
// 27 ms spacing a ±20 ms window holds exactly one sample, so the "windowed mean" was the sample and
// the reducer did nothing at all across the address and the backswing (every still-address row
// printed peakSigma 0.000 — the tell). The window widens symmetrically until it holds this many
// valid samples, or until the reduction's own [from, to] leaves nothing further to add. 3 is the
// same floor kMinRateSamples uses and for the same reason: it is the smallest window that has a
// residual left over after a straight line, so the peak's sigma means something.
//
// kMinRateSamples (3) is the floor that stops a "slope" being a line through two points, which has
// no residual and therefore no standard error to be honest with. A window with fewer — a sparsely
// posed address — yields NO rate rather than a confident one. THIS one is enforced at 3 in code
// whatever is configured here — a two-point fit's zero standard error reads as certainty — where
// kMinExtremumSamples above is a target the widening aims at and 1 legitimately restores the
// single-sample window, which is how the test pins what the widening changed.
namespace reduce {
inline constexpr std::int64_t kExtremumWindowUs   = 40000; // reduce.extremumWindowUs — centred, ±20 ms
inline constexpr int          kMinExtremumSamples = 3;     // reduce.minExtremumSamples — widen to this
inline constexpr std::int64_t kRateWindowUs       = 50000; // reduce.rateWindowUs — minimum fit span
inline constexpr int          kMinRateSamples     = 3;     // reduce.minRateSamples
} // namespace reduce

// --- Lower-body frontal-plane metrics (src/Analysis/lower_body_metrics.h) -----
// leadKneeDrift / hipLineTilt / pelvisSway / pelvisLift, from the COCO BODY hips,
// knees and ankles (11–16). Consumed by LowerBodyConfig::fromOverrides via
// "lowerBody.*" dotted keys. Same conf-gate / address-window shape as foot:: and
// head::, deliberately — a third set of defaults for the same job would be three
// things to sweep and one thing to reason about.
//
// kMinStanceSpanPx guards the DENOMINATOR. Every channel here is a percentage of
// the address ankle span, so a collapsed or mis-detected stance would divide a
// few pixels of noise by a few pixels of stance and emit hundreds of percent. A
// floor is the difference between "we could not measure this" and a confident
// absurdity; 40 px is well under any usable framing and well over the noise.
//
// kMinHipSpanRatio guards the hip LINE, which is a different failure from the
// denominator above. `lineTiltDeg` divides by the hips' horizontal separation, so as
// the pelvis turns toward the target the two hips foreshorten into the same image
// column, dx → 0 and the angle swings to ±90° — a −88° hip tilt just after impact is
// that, and it is not noise. It is a reading of the camera. Below the ratio the frame
// has NO hip line and is absent. The SAME RATIO is applied to the hip half of
// spineSideBend in upper_body_metrics, but that module resolves the line against its
// OWN address reference frames and its own denominator, so one key does not make the
// two agree frame by frame — one ratio, two references (see the note there).
// 0.40 of the address span is roughly 66° out of the image
// plane (acos 0.4), where a 2 px keypoint σ on a 120 px span is about 2.4° of angle
// error — the same order as the residual jitter; the error grows as 1/ratio below it
// and passes 10° by 0.1. The value is sweepable and is not the point; that a floor
// exists is the point.
namespace lowerBody {
inline constexpr double       kConfMin        = 0.30;    // lowerBody.confMin — per-keypoint gate
inline constexpr int          kAddrMinFrames  = 5;       // lowerBody.addrMinFrames
inline constexpr std::int64_t kAddrWindowUs   = 250000;  // lowerBody.addrWindowUs
inline constexpr double       kMinStanceSpanPx = 40.0;   // lowerBody.minStanceSpanPx
inline constexpr double       kMinHipSpanRatio = 0.40;   // lowerBody.minHipSpanRatio
} // namespace lowerBody

// --- Upper-body frontal-plane metrics (src/Analysis/upper_body_metrics.h) -----
// secondaryAxisTilt / spineSideBend / thoraxLateralDrift / shoulderPlaneAngle /
// elbowAlignment / trailElbowHeight / leadHandWidth / leadUpperArmToChest /
// leadArmToTorso, resolved through the anatomy vocabulary rather than raw keypoint
// indices. Consumed by UpperBodyConfig::fromOverrides via "upperBody.*" dotted keys.
//
// Same conf-gate / address-window shape as foot::, head:: and lowerBody::,
// deliberately — a fourth set of defaults for the same job would be four things to
// sweep and one thing to reason about.
//
// kMinShoulderSpanPx guards the DENOMINATOR, exactly as kMinStanceSpanPx does for
// the lower body. It is smaller (30 px) because the shoulder span is genuinely
// narrower than the stance in the same framing, so reusing the stance floor would
// refuse usable swings rather than absurd ones.
//
// kMinShoulderSpanRatio is the shoulder line's foreshortening gate, exactly what
// lowerBody::kMinHipSpanRatio is for the hip line and for the same geometric reason —
// a shoulder plane of +88° AT THE TOP, which is a GRADED phase sample, is the turn
// collapsing the span rather than a posture. It gates shoulderPlaneAngle, the shoulder
// half of spineSideBend, and trailElbowHeight — that last one is a height, not a tilt, but it
// interpolates the shoulder line's y at the elbow's x and so divides by the same dx,
// and an unbounded % shoulder width is worse than an angle that saturates at 90°.
// The hip half of spineSideBend uses lowerBody.minHipSpanRatio.
//
// kMinElbowSpanPx is the ELBOW line's gate, and it is an ABSOLUTE floor where the
// others are ratios — deliberately, because a ratio is INERT on this line. The elbows
// are at their NARROWEST at address (the arms hang together and separate through the
// swing), so |dx| / address |dx| is ≈1 at address and ≥1 everywhere else: the gate
// would never fire, and it would be exactly 1.0 at address, where a 20 px elbow
// separation is pure keypoint noise and `elbowAlignment` is READ. A ratio needs an
// address value that represents the line at its widest; this line's does the opposite,
// so the floor is stated in pixels instead. 25 px is a few keypoint σ (≈2 px each) —
// below it the tilt error exceeds 10° and the reading is noise.
namespace upperBody {
inline constexpr double       kConfMin              = 0.30;    // upperBody.confMin — per-keypoint gate
inline constexpr int          kAddrMinFrames        = 5;       // upperBody.addrMinFrames
inline constexpr std::int64_t kAddrWindowUs         = 250000;  // upperBody.addrWindowUs
inline constexpr double       kMinShoulderSpanPx    = 30.0;    // upperBody.minShoulderSpanPx
inline constexpr double       kMinShoulderSpanRatio = 0.40;    // upperBody.minShoulderSpanRatio
inline constexpr double       kMinElbowSpanPx       = 25.0;    // upperBody.minElbowSpanPx
} // namespace upperBody

// --- Axial body rotation (src/Analysis/body_rotation.h) -----------------------
// pelvisRotation / thoraxRotation / xFactor / xFactorStretch, from a bound Pelvis /
// Thorax IMU where one exists and from the face-on collapse of the hip and shoulder
// spans where one does not. Consumed by BodyRotationConfig::fromOverrides via
// "bodyRotation.*" dotted keys.
//
// kSpanNoisePx and kSinFloor are NOT cosmetic. The camera tier inverts a cosine, so
// dθ/dw = −1/(w₀·sin θ) diverges as the body squares up: the producer propagates the
// span noise through that derivative into MetricSeries::sigma, and the floor is what
// keeps the reported uncertainty finite instead of infinite near zero turn. 3 px is
// the pose endpoint jitter carried through a difference of two endpoints; sin 5°
// caps the reported sigma at roughly 11× the span noise expressed in radians.
namespace bodyRotation {
inline constexpr double       kConfMin       = 0.30;    // bodyRotation.confMin
inline constexpr int          kAddrMinFrames = 5;       // bodyRotation.addrMinFrames
inline constexpr std::int64_t kAddrWindowUs  = 250000;  // bodyRotation.addrWindowUs
inline constexpr double       kMinSpanPx     = 30.0;    // bodyRotation.minSpanPx — denominator floor
inline constexpr double       kSpanNoisePx   = 3.0;     // bodyRotation.spanNoisePx — 1σ of the span
inline constexpr double       kSinFloor      = 0.0872;  // bodyRotation.sinFloor — sin 5°
} // namespace bodyRotation

// --- Club delivery from a face-on camera (src/Analysis/club_delivery.h) -------
// shaftAngleVsHorizontal / attackAngle read off the MEASURED clubhead terminus;
// lowPointAhead read off the SYNTHESIZED ARC instead (see club_delivery.h "Two
// channels, on purpose"). Consumed by ClubDeliveryConfig::fromOverrides via
// "clubDelivery.*" dotted keys.
//
// kVelHalfSpan is the half-width of the centred difference the head velocity — and
// therefore the attack angle — is taken over. One frame either side of a ~9 px head
// is mostly quantisation noise landing squarely on a single-instant reading, so a
// few frames of span buys a usable angle at the cost of a little time resolution.
namespace clubDelivery {
inline constexpr int          kVelHalfSpan        = 2;        // clubDelivery.velHalfSpan (samples)
inline constexpr std::int64_t kLowPointWinUs      = 60000;    // clubDelivery.lowPointWinUs — ±60 ms about Impact
inline constexpr int          kLowPointMinSamples = 5;        // clubDelivery.lowPointMinSamples
inline constexpr double       kHeadConfMin        = 0.30;     // clubDelivery.headConfMin

// THE PUBLISHED 1σ ON lowPointAhead, IN INCHES — the health warning, as a number.
//
// MEASURED, not assumed, and the measurement is thin: one session (2026-08-18
// Wrist_02, six 7-iron swings) with a launch monitor present. The arc's attack
// angle at impact was compared against the device's, giving a bias of +0.02° and
// a spread of 3.26°; over the arc radii that session fitted (33–42 in, mean 36)
// that is 36 · tan(3.26°) ≈ 2.0 in of low point. The bias being ~0 is what makes
// the number publishable at all — the estimator is unbiased and noisy, not skewed.
//
// SIX SWINGS FROM ONE GOLFER ON ONE SESSION IS NOT AN ERROR BUDGET, it is the
// first evidence we have. It is a frozen constant rather than a per-swing
// propagation deliberately: a σ computed per swing from a 6-sample calibration
// would dress up the same one number as if it tracked the swing. Re-seat it the
// day a multi-session corpus with launch-monitor truth exists, and scale it by
// the fitted arc radius at the same time.
inline constexpr double       kLowPointSigmaIn    = 2.0;      // clubDelivery.lowPointSigmaIn
} // namespace clubDelivery

// --- Tempo metrics (src/Analysis/tempo_metrics.h) -----------------------------
// tempoBackswing (Address→Top, s) and tempoRatio ((Top−Address)/(Impact−Top)).
// Consumed by TempoConfig::fromOverrides via "tempo.*" dotted keys.
//
// BASIS NOTE: the numerator is ADDRESS→Top, not Takeaway→Top. This matches the
// metric catalogue's own descriptions; the ~3:1 / 2.2–3.0:1 tour figures in the
// literature (Tour Tempo, TPI 0.847 ± 0.111 s) are TAKEAWAY-based and therefore
// read slightly LOW against this basis by the Address→Takeaway gap. The gap is
// structurally small (Address ≤ Takeaway by construction) but uncharacterised —
// the tempoRatio corridor in metric_catalogue_manifest.cpp is provisional until
// the corpus supplies that distribution.
//
// UNCERTAINTY: Top appears in BOTH the numerator and the denominator with
// opposite sign, so its timing error is doubly leveraged (a 30 ms Top error —
// exactly the ≤30 ms validation target — swings the ratio ~15 %). Real-capture
// Top error has never been measured, so every emitted tempo series carries a
// propagated 1σ rather than pretending to a precision nobody has demonstrated.
// Confidence WIDENS the interval, it never nudges the value (score_uncertainty).
namespace tempo {
inline constexpr bool   kEnabled     = true;   // tempo.enabled — false ⇒ emit nothing (OFF-parity path)
inline constexpr double kMinConf     = 0.0;    // tempo.minConf — refuse at or below this seg.conf;
                                               //   0 rejects the IMU clampFallback ladder (conf == 0,
                                               //   Address pinned to the window edge, NO Top at all)
inline constexpr double kBaseSigmaS  = 0.020;  // tempo.baseSigmaS — 1σ event-timing floor, s. Seeded at
                                               //   the ≤30 ms Top target's order of magnitude; re-seat
                                               //   from the labelled-swing Top-error distribution
inline constexpr double kConfInflate = 1.0;    // tempo.confInflate — σ_e = base·(1 + (1−conf)·inflate)
} // namespace tempo

// --- Ball position at address (src/Analysis/ball_position.h) ------------------
// Where the ball sits along the stance, as a fraction of the heel-to-heel line:
// 0 = at the lead heel, 1 = at the trail heel. UNCLAMPED — a ball forward of the
// lead heel is a real (and coachable) setup, not an error. The denominator is
// exactly foot_metrics' stanceWidth measurement, so the two agree by
// construction. Both are px distances in the same image plane at the same depth,
// so the RATIO needs no scale factor and IS comparable across captures — unlike
// stance width in absolute units. Consumed via "ballpos.*" dotted keys.
namespace ballpos {
inline constexpr bool         kEnabled      = true;    // ballpos.enabled — false ⇒ no series (OFF-parity)
inline constexpr std::int64_t kAddrWindowUs = 250000;  // ±window about Address (mirrors foot::/head::)
inline constexpr int          kMinSamples   = 3;       // min accepted ball samples for a valid measurement
inline constexpr double       kMaxJumpPx    = 40.0;    // cluster gate about the component-wise median —
                                                       //   an off-cluster sample is a detector mis-lock,
                                                       //   not a moved ball (it is stationary at address)
inline constexpr double       kFracLo       = -0.5;    // ballpos.fracLo — plausibility floor
inline constexpr double       kFracHi       = 1.5;     // ballpos.fracHi — plausibility ceiling
} // namespace ballpos

// --- Shaft onset segmentation (src/Analysis/shaft_track_assembly.cpp) ----------
// Camera-only Address/Takeaway hardening (fidget-proofing). The Stage-A onset
// walk-back (A1 grip speed + A2 φ witness) cannot tell fidget motion that
// DEPARTS and RETURNS from a one-piece takeaway, so on a fidgety address it
// walks the onset back THROUGH the whole fidget (real capture: the lerped-pose
// grip keeps 2–4 px/f smoothed speed through every fidget settle, so A1 only
// stops at the DEEP pre-fidget stillness — 0.5–1.5 s early on the 17-swing
// truth set). The "no-return" veto post-processes onset = min(A1, A2) with a
// departure-referenced revisit scan and can only push the onset LATER — to the
// last frame whose own grip position the track ever comes back to before the
// takeaway run (everything after departs for good). No absolute-rest gate and
// no address anchor: both were unsatisfiable on real capture (the golfer
// settles into an address DISPLACED from the pre-fidget stance; true grip rest
// never happens). Consumed by ShaftV3Config (shaft_track_assembly.h); SwingLab
// sweeps "shaft.onsetReturn*"/"shaft.onsetRunBridgeFrames". (The 2026-07-17
// anchor-box veto's kOnsetReturnPhiDeg / kOnsetReturnStillFrames are RETIRED —
// the revisit scan needs neither.)
// FROZEN ON 2026-07-17 (user-approved after the in-app eyeball): veto box 7 +
// gap 15 + bridging 10 + Takeaway event. Evidence: 17-swing truth evaluation —
// Address-error median 0.564 s → 0.060 s. 0 still disables each (the swLow<=0
// idiom) — the dark values remain the byte-identical-legacy baseline for soaks.
namespace shaft {
inline constexpr double kOnsetReturnBoxPx     = 7.0;   // revisit radius (px); 0 ⇒ veto OFF (legacy onset)
inline constexpr int    kOnsetReturnGapFrames = 15;    // forward exclusion before a revisit counts (~100 ms @150fps)
// Run bridging for the two-longest-runs picker: merge >swSpd runs separated by
// fewer than this many quiet frames BEFORE ranking, so a slow backswing that
// the lerped-pose speed profile fragments into short bursts still competes as
// one run (w2s4-class mis-pick: a 14-frame follow-through fragment beat two
// 9-frame backswing fragments and bs0 landed at the DOWNSWING). 0 = off
// (legacy ranking). Separate key from the veto so its effect stays separable.
inline constexpr int    kOnsetRunBridgeFrames = 10;
// m3gate — chain-qualified net-displacement gate on the two-longest run
// ranking. A grip-anchor oscillation cluster (s0002's presentation-move pose
// flapping) bridges >= 3 raw runs into a chain long enough to win the ranking
// while going nowhere; the gate demands net displacement >= this fraction of
// path length from such chains. FROZEN ON 2026-07-18 at 0.2 (0 still
// disables). Evidence: 17-swing truth — s0002 Takeaway 1.857 → 2.480 s
// (+0.100 vs truth), s0001 Address → +0.042, the other 15 swings zero-
// movement; 61-swing corpus — 19 move in the corrective direction, 0 score
// changes; net/path separation 25× (flap chain 0.013 vs >= 0.34 for every
// legitimate merged run). m = 2 merges (the frozen w2s4 evidence, including
// the legitimately low-net downswing+follow-through reversal merge at 0.08)
// are structurally exempt.
inline constexpr double kOnsetBridgeMinNetFrac = 0.2;
inline constexpr bool   kEmitTakeaway         = true;  // vision Takeaway event at bs0 (ladder gains 1 event)
} // namespace shaft

// --- Late-pipeline timeline-event refinement (src/Analysis/event_refine.h) -----
// EventRefineStage (analysis_pipeline_fusion_architecture_proposal.md P3 — event
// fusion) fine-tunes the timeline events users see from the FINISHED shaft/ball/
// pose products, per Mark's definition (Address = the last static point before the
// clubhead departs the ball and doesn't come back). V1 refines Takeaway + Address
// only (never Impact — the acoustic-anchored marker contract), retimes EXISTING
// events (never inserts), and abstains unless the evidence clears minConf and the
// shift stays within maxShiftS. Consumed by EventRefineConfig::fromOverrides via
// "refine.*" dotted keys; SwingLab sweeps them without rebuild.
//
// kEnabled=false ⇒ the stage never runs ⇒ ctx.seg (and every downstream consumer)
// is byte-identical AND code-path-identical to the pre-refine pipeline.
//
// FROZEN ON 2026-07-18 (V1 evidence freeze, paired with ball::activity::
// kClubActivity — the load-bearing Tier-B input): 17-swing truth A/B — median
// |p1 err| held 0.052 s, max 0.577 → 0.145 s (the s0002 holdout rescued),
// within-100ms 12 → 14, ZERO regressions at minConf 0.8; 61-swing corpus —
// 3 movers, 0 score changes. false still darks the whole stage (the soak baseline).
namespace refine {
inline constexpr bool   kEnabled           = true;  // refine.enabled — master gate (2026-07-18 freeze)
inline constexpr bool   kTakeaway          = true;  // refine.takeaway — retime the Takeaway event
inline constexpr bool   kAddress           = true;  // refine.address — retime the Address event
inline constexpr bool   kImpactResidual    = true;  // refine.impactResidual — log-only launch−impact telemetry
inline constexpr double kDepartThetaDeg    = 25.0;  // refine.departThetaDeg — at-ball θ-vs-θ_ball tolerance
                                                    //   (adaptive-floored at the address ref, like tk0)
inline constexpr double kActivityQuietSigma = positions::kP1ClubQuietSigma; // refine.activityQuietSigma
                                                    //   — Tier-B club-quiet σ (seeded from the P1 gate)
inline constexpr int    kReturnHoldMs      = 200;   // refine.returnHoldMs — min at-ball run to count as a
                                                    //   genuine return (shorter = flicker, debounced out)
inline constexpr double kMinConf           = 0.8;   // refine.minConf — apply floor on the fused confidence
                                                    //   (0.5 → 0.8 at the 2026-07-18 freeze: zero
                                                    //   regressions on the 17-swing truth A/B at 0.8)
inline constexpr double kMaxShiftS         = 3.0;   // refine.maxShiftS — abstain if |t_refined − t_old| exceeds
inline constexpr bool   kPositionsLadder   = true;  // refine.positionsLadder — promote club P2/P3/P5/P6/P8
                                                    //   positions into ladder PhaseEvents (positions_ladder.h,
                                                    //   version 4). ON since the 2026-08-09 corpus gate:
                                                    //   61/61 byte-identity dark, +289 resolved rows, ladder
                                                    //   order clean (P4<P5<P6<P7 on all 11 dual-carry swings);
                                                    //   the club-track P6 crossing firing ~120 ms early vs
                                                    //   truth is a DETECTOR finding, tracked separately.
                                                    //   false darks the stage (the soak baseline).
// ── Timeline fusion (timeline_fusion.h, docs/design/timeline-fusion.md) ──────
// The positions ladder's occupancy test — "whoever got there first wins" — hands
// an IMU-bound swing a conf-0.35 hand-orientation PROXY at P6, a conf-0.35
// forearm proxy at P8 and a conf-0.20 window-edge CLAMP at P10, discarding the
// camera's measured value in each slot. Fusion arbitrates on measurement CLASS
// (TimingClass) and estimand OWNERSHIP instead, so a measurement displaces a
// proxy and a proxy never displaces a measurement.
//
// ON since the 2026-08-19 corpus gate (docs/implementation/timeline_fusion_impl.md).
// Evidence: OFF parity 61/61 byte-identical vs the pre-fusion binary; camera-only
// ON moved zero events and produced zero residual diffs; on the eleven truth-marked
// 2026-08-18 swings the hi-res stratum went P6 −39→+6 ms, P8 +96→+7 ms, P10
// +1686→−0 ms against the hand markup, while every retained slot (P1–P5, P7) moved
// by exactly 0 ms; no measure regressed on coverage; the blast radius was the
// predicted set only (phase samples at the moved rungs, the 32 flipped events, and
// the wrist grid — no curve, score, bound or tempo). false still darks the stage
// entirely ⇒ PositionsLadderStage runs exactly as before ⇒ byte- AND
// code-path-identical to the pre-fusion pipeline (the soak baseline).
inline constexpr bool   kFusion            = true;  // refine.fusion — arbitrate the ladder
                                                    //   (V1 flips P6/P8/P10 on IMU-bound swings)
inline constexpr bool   kFusionP1          = false; // refine.fusionP1 — Address/P1 arbitration.
                                                    //   Implemented but DARK: Address is the
                                                    //   reference instant for tempo, every
                                                    //   Address-referenced pose metric and the
                                                    //   replay trim, so it gets its own Phase-2
                                                    //   gate (timeline-fusion.md §4.4, §9.1).
inline constexpr int    kFusionDisputeMs   = 300;   // refine.fusionDisputeMs — a replacement is
                                                    //   refused (and counted `disputed`) when the
                                                    //   two witnesses disagree by more than this
                                                    //   AND the incumbent is itself Measured. A
                                                    //   PROXY incumbent gets NO cap: its class
                                                    //   already says the time is a stand-in, so no
                                                    //   magnitude of disagreement rehabilitates it
                                                    //   (Wrist_01/0003, where the capped draft would
                                                    //   have preserved a 667 ms error — §4.3).
                                                    //   Generous by design: a different-SWING level
                                                    //   of disagreement, never a consistent bias.
} // namespace refine

// ── Kinematics display series (kinematic_series.*) ───────────────────────────
// Clubhead speed / hand speed (mph) + lag angle (°) for the review chart — three
// UNSCORED per-frame curves derived purely from the face-on camera products: the
// shaft grip/head positions give the two linear speeds, and the lead-forearm vs
// clubshaft direction gives the lag angle. Prefer the dense synth shaft channel,
// fall back to the measured samples. Session-type-agnostic: the Wrist profile
// appends them (KinematicsStage) and the Swing/GRF/Coach analyzers reuse the same
// camera stages (Pose→Ball→Shaft) to produce them. Consumed by
// KinematicSeriesConfig::fromOverrides via the "kinematics.*" dotted key.
//
// kEnabled=false ⇒ KinematicsStage never runs (Wrist byte-identical) AND the
// Swing/GRF/Coach analyzers keep their instant stub (no pose/shaft compute) — the
// whole feature is dark and code-path-identical to before.
//
// ENABLED 2026-07-18 by product decision: this is an UNSCORED, additive display
// feature (three review-chart curves; it touches neither the score nor segmentation),
// its OFF state is proven byte-identical (swing_window_parity_test Test 4), and the ON
// path is verified end-to-end on a real Wrist swing (clubhead/hand speed + lag land in
// analysis.metrics with Address/Top/Impact dots). The corpus ON-eval (guide §6.6 gate
// 5) is a quality check on the curves, not a correctness gate — set false to dark it.
namespace kinematics {
inline constexpr bool kEnabled = true;   // kinematics.enabled — master gate (ON 2026-07-18, display-only)
} // namespace kinematics

// Face-on swing-plane transition delta (shaft_plane.h / ShaftPlaneStage). Ships ON:
// the measure it feeds is EXPERIMENTAL and normless — status `planned`, no norm row —
// so it cannot fire a fault however the numbers come out. What the stage buys while
// it is on is the accumulating measured-vs-synth pair the promotion gate will need
// (transition_plane_producer_brief.md §8.3, §9). Set false to dark it.
namespace shaftPlane {
inline constexpr bool kEnabled = true;   // shaftPlane.enabled — master gate (ON 2026-08-11)
} // namespace shaftPlane

} // namespace pinpoint::tuned
