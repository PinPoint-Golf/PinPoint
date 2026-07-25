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
// not single numeric literals, and stay at their source of truth. The full reference-band
// lo/hi corridor arrays stay in reference_bands.cpp (a Corpus-2 re-seat, not a freeze).

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
inline constexpr double kApparentPenalty = 0.5;     // camera-plane apparent-angle confidence factor
                                                    //   (× min endpoint conf) — these are PROJECTED,
                                                    //   not anatomical, angles so trust is halved
} // namespace wristAngles
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

} // namespace pinpoint::tuned
