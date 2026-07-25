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

#include <QHash>
#include <QMetaType>
#include <QPointF>
#include <QQuaternion>
#include <QRectF>
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "types.h"   // pinpoint::SourceId, kInvalidSourceId
#include "wrist_assessment_result.h"   // PpWristFinding (Tier-2 offline assessment)

// Canonical intermediate + output data structures for the shot analyzer
// (design: docs/design/shot_analyzer_design.md). All rotation is QQuaternion — Euler
// appears only at the UI readout. Header-only value types, passed by value /
// shared_ptr across the QtConcurrent worker boundary.
namespace pinpoint::analysis {

// Degradation tier, chosen at job-build time from camera-calib + IMU availability.
enum class ReconstructionTier { Angles2D, Mono3DPlusImu, Stereo3D, ClubInstrumented };

// Anatomical segment an IMU is mounted on. Unknown = unmapped placement slot.
enum class SegmentRole {
    Unknown = 0,
    Pelvis, Thorax, T12,
    LeadUpperArm, LeadForearm, LeadHand,
    TrailThigh, LeadThigh,
    Club,
};

// Canonical placement-slot → anatomical role mapping — the single source of truth,
// shared by the swing exporter (analysis.bindings[].role + stream device.role) and
// the data viewer's settings fallback. Only Wrist Motion (sessionType 1) is mapped
// today; other session types resolve to Unknown until their placement UX lands.
inline SegmentRole segmentRoleForSlot(int sessionType, const QString &slot)
{
    if (sessionType == 1) {            // Wrist Motion
        if (slot == QLatin1String("A")) return SegmentRole::LeadForearm;
        if (slot == QLatin1String("B")) return SegmentRole::LeadHand;
        if (slot == QLatin1String("C")) return SegmentRole::LeadUpperArm;
    }
    return SegmentRole::Unknown;
}

// Stable string name for a role — written to swing.json (stream device.roleName,
// analysis.bindings[].roleName) and consumed by SwingLab / future post-hoc tools.
// Stable across enum renumbering; pair it with the int role value.
inline QString segmentRoleName(SegmentRole r)
{
    switch (r) {
    case SegmentRole::Pelvis:       return QStringLiteral("Pelvis");
    case SegmentRole::Thorax:       return QStringLiteral("Thorax");
    case SegmentRole::T12:          return QStringLiteral("T12");
    case SegmentRole::LeadUpperArm: return QStringLiteral("LeadUpperArm");
    case SegmentRole::LeadForearm:  return QStringLiteral("LeadForearm");
    case SegmentRole::LeadHand:     return QStringLiteral("LeadHand");
    case SegmentRole::TrailThigh:   return QStringLiteral("TrailThigh");
    case SegmentRole::LeadThigh:    return QStringLiteral("LeadThigh");
    case SegmentRole::Club:         return QStringLiteral("Club");
    case SegmentRole::Unknown:      break;
    }
    return QStringLiteral("Unknown");
}

// Resolved on the UI thread from AppSettings::imuPlacement + the live ImuInstance
// calibration snapshot (alignA/mountM are session-lifetime on the QObject — the
// worker can never read them, so they are copied into the job here).
struct ImuSegmentBinding {
    SourceId    source = kInvalidSourceId;
    SegmentRole role   = SegmentRole::Unknown;
    QQuaternion alignA;   // fusion-world  -> anatomical-world  (A)
    QQuaternion mountM;   // anatomical-body -> sensor-body     (M)
    // Calibration status snapshot (ImuInstance) — persisted so SwingLab can
    // filter a corpus by calibration provenance. `calibrated` is the composite
    // gate (anatCalibrated AND both mount checks within threshold).
    bool        anatCalibrated = false;
    bool        calibrated     = false;
    double      mountDeviationDeg    = 0.0;
    double      mountGravityErrorDeg = 0.0;
    QString     calibratedAtUtc;       // ISO8601 wallclock; empty = never calibrated
    double      calibAgeSec = -1.0;    // age at shot time; -1 = never calibrated
};

// APPEND-ONLY: phases persist as raw ints in swing.json and QML compares the
// ints directly (e.g. PpChartPlot's `phase === 5`), so existing values must
// keep their positions. v2 ladder additions start at 8 (design addendum A.3).
enum class Phase {
    Address, Takeaway, Top, Transition, Downswing, Impact, Release, Finish,
    MidBackswing  = 8,    // P3 — lead arm parallel (backswing)
    Delivery      = 9,    // P6 — shaft parallel (downswing); hand-proxy until shaft-measured
    MaxSpeed      = 10,   //      peak hand angular speed (clubhead-speed proxy)
    FollowThrough = 11,   // P9 — lead arm parallel (follow-through)
    ShaftParallelBack    = 12,  // P2 — shaft parallel (backswing); image-plane (shaft_position_first §1)
    ArmParallelDown      = 13,  // P5 — lead arm parallel (downswing)
    ShaftParallelThrough = 14,  // P8 — shaft parallel (follow-through); image-plane
};

// A detected swing-phase event on the shared phase timeline.
struct PhaseEvent {
    Phase   phase = Phase::Address;
    int64_t t_us  = 0;
    float   conf  = 1.0f;   // 0..1; low-confidence ticks fade in the UI
    // Which segment the event was measured from (provenance, design A.2.5);
    // Club once the shaft refinement lands. Unknown for anchors (Impact).
    SegmentRole provenance = SegmentRole::Unknown;
};

// The segmentation result (design addendum A.6/C.5): the event ladder plus
// the logical swing bounds consumers truncate to (export encode span, replay
// span, metric grid, heavy-stage scan bounds). The frozen window itself is
// never trimmed. conf == 0 means "bounds are just the window" — consumers
// needing real truncation must check it. Canonical home here (Qt-only) so it
// can ride SwingAnalysis and the jobs without pulling the segmenter header.
struct Segmentation {
    std::vector<PhaseEvent> events;        // time-ordered, monotone ladder
    int64_t swingStartUs = 0;              // Address − pad, clamped to coverage
    int64_t swingEndUs   = 0;              // Finish  + pad, clamped to coverage
    float   conf         = 0.0f;           // min over {Address, Top, Impact, Finish}
    int     version      = 2;              // 3 once the shaft refinement ran

    const PhaseEvent *eventFor(Phase p) const {
        for (const PhaseEvent &e : events)
            if (e.phase == p) return &e;
        return nullptr;
    }
};

// One labelled point on a metric's curve at a key swing phase.
struct PhaseSample {
    Phase   phase = Phase::Address;
    int64_t t_us  = 0;
    double  value = 0.0;
    QString band;           // "green"/"yellow"/"red" at scored phases, else ""
};

// A per-frame metric curve over the window's TimeGrid plus sparse phase samples.
struct MetricSeries {
    QString key, label, unit;
    std::vector<int64_t> t_us;            // shared TimeGrid (ascending, absolute us)
    std::vector<double>  value;           // one per t_us — the continuous curve
    std::vector<PhaseSample> phaseSamples;
    std::optional<double> bandLo, bandHi; // ideal/tour band (unit-space, one-sided ok)
    // 1σ MEASUREMENT uncertainty on this metric's value, in unit-space — the
    // separate track from bandLo/bandHi (which are coaching tolerance, not noise;
    // same split as score_uncertainty.h's §B.7 interval vs the band σ). ABSENT
    // means "not characterised", NOT "zero error" — every metric that predates
    // this field leaves it unset and serialises byte-identically. Set it only
    // when a real error budget was propagated; confidence must WIDEN this, never
    // nudge `value`.
    std::optional<double> sigma;
    bool flexPositive = true;             // stored-sign polarity (flip only at the label)
};

// A metric scored against its reference band.
struct ScoredMetric {
    QString key;
    double  value    = 0.0;
    double  mu       = 0.0;
    double  sigma    = 1.0;
    bool    oneSided = false;
    double  subScore = 0.0;   // 0..100 after band falloff
    QString band;             // "green"/"yellow"/"red"
    Phase   phase    = Phase::Impact;
    double  weight   = 0.0;
};

// A ranked coaching fault derived from a metric deviation.
struct Fault {
    QString id, title, cause, drill;
    double  pointsLost = 0.0;   // weight * (100 - subScore), the ranking key
    Phase   phase      = Phase::Impact;
};

// Which estimand `overall` represents (design §B.0). Adherence = closeness to a
// defined-good action (Swing/GRF, weighted geometric mean). Resemblance = which
// lead-wrist archetype the motion most resembles, and how strongly (Wrist; the
// per-archetype R_p map below carries the detail).
enum class ScoreKind { Adherence, Resemblance };

// Measurement-uncertainty interval on the score (design §B.7) — a SEPARATE track
// from the band σ (which is coaching tolerance only). Low confidence WIDENS this
// interval; it never moves `overall`. Invariant: it always brackets `overall`
// (0 ≤ lo ≤ overall ≤ hi). -1 = not computed. For the Wrist session the headline is
// the penalty-based assessment score, whose own error model is deferred, so the
// interval is left unset there (the §B.7 budget is computed on the resemblance value
// and only kept while that value is itself `overall` — see wrist_analyzer.cpp).
struct ScoreInterval {
    int halfWidth = -1;   // e.g. 9 means "±9"
    int lo = -1, hi = -1; // clamped [0,100]; convenience for UI/checks
    bool valid() const { return halfWidth >= 0; }
};

struct ScoreBreakdown {
    ScoreKind          kind    = ScoreKind::Adherence;
    int                overall = 0;       // adherence: weighted geo-mean | resemblance: max R_p
    // Resemblance estimand (Wrist) — independent absolute resemblances in 0..100,
    // keyed "bowed"/"neutral"/"cupped"; NOT normalised to sum 100 (design §B.0a).
    // Empty for adherence scores.
    QHash<QString,int> resemblance;
    QString            patternLabel;      // argmax label, e.g. "bowed" (empty for adherence)
    bool               blended = false;   // top-two within blendedDeltaPts (~10)
    // Measurement-uncertainty interval that brackets `overall` (§B.7). Filled by WP-4 for the
    // resemblance value; unset once the Wrist headline becomes the penalty-based assessment score
    // (its own interval is deferred). valid() false when not computed/unset.
    ScoreInterval      interval;
    // Adherence estimand (Swing/GRF), unchanged.
    QHash<QString,int> perRegion;         // "rotation","wrist","tempo",...
    QHash<QString,int> perPhase;
    std::vector<ScoredMetric> metrics;    // full audit trail
};

// ── ShaftTracker camera tracks (canonical output shapes) ────────────────────
// Produced by PoseRunner (S0) and ShaftTracker (S2) over the face-on camera;
// persisted in swing.json and replay-overlaid in PpCameraFrame (S4). Kept here
// (Qt-only) so consumers of SwingAnalysis never pull the OpenCV-typed
// detection headers.

// COCO-WholeBody keypoint layout (wholebody_pose_design.md §4): indices 0–16
// are the unchanged COCO body joints (PoseJoint order — every existing index
// constant stays valid verbatim); the tail is purely additive.
inline constexpr int kWholeBodyJoints  = 133;
inline constexpr int kFootFirstKp      = 17;   // feet 17–22 (L bigtoe/smalltoe/heel, R …)
inline constexpr int kFaceFirstKp      = 23;   // face 23–90 (68-pt contour)
inline constexpr int kLeftHandFirstKp  = 91;   // left hand 91–111 (21: wrist root + 4/finger)
inline constexpr int kRightHandFirstKp = 112;  // right hand 112–132 (21, same layout)

// One offline-posed frame: 133 COCO-WholeBody keypoints (0–16 = the unchanged
// COCO body joints; tail = feet/face/hands per the layout above) + both hands'
// centroid anchors, all normalized 0..1 frame coordinates (PoseResult
// convention). leadHand/trailHand stay the derived grip anchors every consumer
// uses — unchanged by the whole-body widen. Tracks loaded from pre-wholebody
// swing.json files (51-float kp) leave the tail default-initialized (conf 0).
struct PoseFrame2D {
    int64_t t_us = 0;
    std::array<QPointF, kWholeBodyJoints> kp{};
    std::array<float, kWholeBodyJoints>   conf{};
    QPointF leadHand, trailHand;     // hand centroids; COCO wrists on fallback
    float   handConf = 0.f;          // 0 when wrist-fallback
};

// ── Motion-overlay pose smoother (Phase 1) honesty aux ──────────────────────
// Per-frame per-keypoint provenance emitted by the offline RTS smoother
// (src/Analysis/pose_smoother.{h,cpp}) alongside a smoothed PoseFrame2D. `tier`
// drives the overlay paint policy (Off ⇒ raw passthrough, don't paint as
// confident; Pred ⇒ a coasted/bridged estimate rendered like measured; Meas ⇒ a
// measurement inside a confirmed run). `sigma` is the posterior positional σ in
// PIXELS; 0 ⇒ no smoothed value for that keypoint that frame (the output kp is
// the raw input passthrough). Parallel to the smoother's per-frame output.
enum class PoseTier : uint8_t { Off = 0, Pred = 1, Meas = 2 };
struct PoseKpAux {                       // per-frame per-keypoint smoother honesty
    std::array<uint8_t, kWholeBodyJoints> tier{};   // PoseTier values (0–16 body, tail = feet/face/hands)
    std::array<float, kWholeBodyJoints>   sigma{};  // posterior σ (px); 0 = no smoothed value
};

struct PoseTrack2D {
    pinpoint::SourceId camera = pinpoint::kInvalidSourceId;
    std::vector<PoseFrame2D> frames;
    // Offline-smoothed companion track (pose_smoother.cpp, Motion overlay):
    // parallel to `frames` (same t_us grid, normalized kp; conf carries the
    // render-alpha contract), with per-kp honesty in `smoothedAux`. Empty on
    // swings analysed before the smoother existed — additive only.
    std::vector<PoseFrame2D> smoothed;
    std::vector<PoseKpAux>   smoothedAux;
    // Dense VISUALIZATION-tier upsample of `smoothed` (pose_synthesis.h): a C¹
    // Hermite interpolation onto a fixed 240 Hz grid so the replay overlays scrub
    // the skeleton smoothly. Off joints (conf 0) stay Off. Metrics/scoring NEVER
    // read this (same discipline as ShaftTrack2D.synth) — additive, empty when the
    // pose-synth tier is off / < 2 smoothed frames, so swings without it serialize
    // byte-identically.
    std::vector<PoseFrame2D> smoothedSynth;
    // WB1 accuracy-pass provenance (wholebody_pose_design.md §3). cropRect is the
    // swing-level person crop in FULL-FRAME NORMALIZED coords; nullopt ⇒ the
    // full-frame fallback ran (no crop). `decode` is "dark" | "argmax" (empty ⇒
    // legacy/unset). Both are written to swing.json only when they differ from
    // the pre-WB1 legacy (decode == "argmax" and cropRect absent), so a
    // flags-off run serialises byte-identically to the pre-WB1 tree.
    std::optional<QRectF> cropRect;
    QString               decode;
};

// One frame of ball-detector output (src/Pose/ball_detector.h BallDetection,
// same normalized [0,1] full-frame convention as PoseFrame2D — no transform
// needed between the two). found=false frames still carry a t_us so gaps are
// distinguishable from "never sampled here" (v3.4 design §9.7 — a
// deliberately low-entropy "constant plus one step" stream).
struct BallSample2D {
    int64_t t_us       = 0;
    bool    found       = false;
    QPointF center;             // normalized [0,1], full frame
    float   radiusNorm  = 0.f;  // normalized to frame width
    float   conf        = 0.f;
    // W3 club-corridor activity (ball_runner.cpp; ball.clubActivity): mean
    // |crop − medRef| / σ over an annulus around the locked ball centre — high
    // when the clubhead is moving beside a still ball (a fidget bob). -1 = absent
    // (never computed: dark ball.clubActivity, the first refFrames warm-up frames,
    // or a frame where the ball wasn't found). TWO-CONSUMER CONTRACT (widened from
    // the W3 single consumer when EventRefine landed): this field feeds ONLY the
    // named pair (1) addressHoldEndFrame's club-quiet mask (shaft_positions.h) and
    // (2) the EventRefine Tier-B at-ball activity gate (event_refine.h) — still
    // never tk0, length, launch, or any DP evidence (ball_anchor_test asserts
    // applyBallAnchor is invariant to it). Persisted as "act" ONLY when >= 0, so a
    // dark run serialises byte-identically.
    float   clubActivity = -1.f;
};

// Face-on ball track: either populated live (CameraInstance's accumulator /
// a recorded swing.json "ball" stream) or replayed offline (BallRunner) over
// an archived swing that predates live recording. Empty ⇒ no ball data ⇒ the
// shaft tracker's ball-anchor pass is a no-op (additive-only, never degrades
// the existing track — design §9.6).
struct BallTrack2D {
    pinpoint::SourceId camera     = pinpoint::kInvalidSourceId;
    std::vector<BallSample2D> frames;
    int64_t launchTUs  = -1;    // the collapse-cliff instant (design §9.3); -1 = no launch observed
    QPointF launchCenter;       // ball position at the last pre-launch frame
};

// The persisted LIVE empty-mat baseline, resolved from swing.json
// setup.ballDetection.baseline. Lets offline re-analysis (BallRunner) reconstruct
// the exact baseline the studio session learned instead of self-seeding over the
// swing window's opening frames — where the ball is already placed (which bakes the
// ball into the baseline it subtracts against). The blob (row-major float32 B,
// w*h*4 bytes) stays on disk; BallRunner loads it (loadBallBaselineBlob), never the
// job. `path` is resolved to absolute by the loader; `roi` is full-frame normalized
// (the box B covers); `fps` is PROVENANCE ONLY — the offline tracker re-measures its
// own pushed-frame rate (plan risk R2). cv-free by design (Qt-only header).
struct BallBaselineRef {
    QString path;               // absolute path to the .ballbase.f32 blob
    int     w = 0, h = 0;       // ROI px dims B was seeded over
    QRectF  roi;                // full-frame normalized ROI
    double  rHat   = 0.0;       // radiusForWidth at seed time (provenance)
    double  fps    = 0.0;       // NOT used to build the tracker (see above)
    double  noise0 = 1.0;       // robustNoise fallback for the tracker
    bool isValid() const { return !path.isEmpty() && w > 0 && h > 0 && !roi.isEmpty(); }
};

// Widened uint8_t → uint16_t at Layer C (shaft_position_first §4): the byte was
// full (0x01–0x80 all assigned) and ShaftSynthesized needs 0x100. Pure-C++
// recompile — swing.json serializes flags as int, readers use toInt(), QML sees
// int, so no schema/JSON/QML break.
enum ShaftSampleFlags : uint16_t {
    ShaftMeasured          = 0x01,  // vision measurement fused at this sample
    ShaftImuBridged        = 0x02,  // IMU channel fused (no vision this sample)
    ShaftCoasted           = 0x04,  // predict-only (neither channel)
    ShaftWedge             = 0x08,  // vision measurement was a blur-wedge centroid
    ShaftHeadProjected     = 0x10,  // headPx projected from grip + L·dir(θ), not measured
    ShaftKinematicPredicted= 0x20,  // pure R6 kinematic-model sample (predicted series / fallback)
    ShaftBallAnchored      = 0x40,  // theta soft-anchored from the grip->ball line (v3.4 design §9)
    ShaftHeadOffFrame      = 0x80,  // Stage-2 head expected off-frame — headPx is a ray/edge-clamped
                                    // point (NOT a head position); always co-set with ShaftHeadProjected
    ShaftSynthesized       = 0x100, // kinematically synthesized between P anchors (shaft_position_first
                                    // §2 Layer C) — VISUALIZATION tier; EXCLUDED from metrics/scoring/
                                    // estimands. Carried only in ShaftTrack2D.synth, never in samples[].
};

struct ShaftSample2D {
    int64_t t_us         = 0;
    QPointF gripPx;             // anchor used for detection (image px)
    QPointF headPx;             // measured terminus blob, or projected (see flags)
    double  thetaRad     = 0.0; // unwrapped, RTS-smoothed image angle (atan2 convention)
    double  thetaDotRadS = 0.0; // smoothed angular velocity
    double  visibleLenPx = 0.0; // ridge extent (median/hold-filtered — θ is the precision channel)
    float    conf        = 0.f; // 0..1 from the smoothed θ posterior variance
    uint16_t flags       = 0;   // ShaftSampleFlags bitfield (widened to 16-bit at Layer C)
    // Stage-2 measured-clubhead (Phase B). headConf = the head's own confidence
    // (0..1); headSigmaPx = its posterior σ_r (px, ≤ sigMeasMax ⇒ label-grade).
    // Both −1 = the Stage-2 head pass did not run for this sample (metric gating
    // treats −1 as "no measured head", distinct from a low-confidence one).
    float   headConf     = -1.f;
    float   headSigmaPx  = -1.f;
    // Layer A line re-registration (shaft_position_first design §2). Normalized
    // ridge support under the DRAWN line — the first metric of "does this line lie
    // on the club", measured by the snap pass. −1 = snap pass off / non-vision
    // tier (no measurement). Serialized only when ≥ 0 so a snap-off run stays
    // byte-identical.
    float   lineConf     = -1.f;
};

// Multi-estimator club-length fusion result (club_length_fusion.h), recorded per
// swing on ShaftTrack2D. All px are grip→head at the FIXED face-on camera scale;
// <0 ⇒ absent / abstained. `fused*` include the persistent prior; `fusedInstant*`
// are PRIOR-FREE (they alone update the prior — no self-reinforcement). `ladder*`
// are the values the tracker ACTUALLY used (rung 0 = pre-pass fused). Head/length
// product only — never touches θ. Default-constructed = "no fusion recorded".
struct ClubLengthEstimate {
    double ballPx           = -1.0;  // E-ball   grip→ball @address
    double bandPx           = -1.0;  // E-band   band scale × (clubLenMm − r0)
    double headPx           = -1.0;  // E-head   p95 post-top Stage-2 Meas rOut (excl. pinned-at-bound)
    double posePx           = -1.0;  // E-pose   rung-3 stature surrogate (sanity bound only, never fused)
    double priorPx          = -1.0;  // E-prior  persistent EMA (joins when priorN ≥ 2)
    double fusedPx          = -1.0;  // inverse-variance posterior (WITH prior) — the recorded length
    double fusedSigmaPx     = -1.0;  // posterior σ, χ²-disagreement inflated
    double fusedConf        = 0.0;   // confAgree · confSupport ∈ [0, 1]
    double fusedInstantPx   = -1.0;  // PRIOR-FREE fusion — the only input to the prior update
    double fusedInstantConf = 0.0;
    int    ladderRung       = 0;     // rung the tracker used (0 = pre-pass fused, else 1..4)
    double ladderLenPx      = 0.0;   // projLenPx the tracker actually used
    int    nEstimators      = 0;     // surviving instantaneous estimators in the recorded fuse
    int    priorN           = 0;     // prior support that entered the recorded fuse
    int    headMeasN        = 0;     // # post-top Meas frames that fed E-head
};

// How a ShaftPosition's geometry was resolved (shaft_position_first §2 Layer B).
// TrackSample = sampled from the final emitted samples[] at the P-time (B1,
// report-only). MilestoneFit = the ±k-frame shift-and-stack joint (grip,θ,L) fit
// (B2) with honest σ from its residuals.
enum class PositionSource : uint8_t { TrackSample = 0, MilestoneFit = 1 };

// One located coaching P-position on the shaft track (shaft_position_first §2
// Layer B / §4). `p` is the coaching P-index 1..8, or 10 (Finish; P9 reserved,
// deferred) (NOT a Phase enum value);
// `t_us` is the sub-frame-located crossing time. gripPx/headPx here are IMAGE px
// (unlike samples[], which serialization normalizes by frameWidth/Height —
// documented so a reader never mixes the two spaces). thetaRad is the shaft
// direction grip→head (image atan2 convention). sigma* < 0 and stackN 0 mark a B1
// TrackSample (no fit); B2 fills them from the milestone fit.
struct ShaftPosition {
    int      p             = 0;
    int64_t  t_us          = 0;
    QPointF  gripPx, headPx;          // IMAGE px (not normalized)
    double   thetaRad      = 0.0;
    double   lenPx         = -1.0;    // drawn grip→head length (px); < 0 = absent
    float    conf          = 0.f;
    float    sigmaThetaDeg = -1.f;    // θ posterior σ (deg); −1 = not fitted (B1)
    float    sigmaLenPx    = -1.f;    // length posterior σ (px); −1 = not fitted (B1)
    int      stackN        = 0;       // shift-and-stack frame count (B2); 0 = track sample
    uint8_t  source        = 0;       // PositionSource
};

struct ShaftTrack2D {
    pinpoint::SourceId camera = pinpoint::kInvalidSourceId;
    bool  valid = false;        // coverage gate over the swing span (all-or-nothing for consumers)
    float coverage = 0.f;       // fraction of span frames Measured|ImuBridged
    float imuVisionCorr = 0.f;  // Pearson corr of vision vs IMU θ̇ (0 = no channel) — health metric
    int   frameWidth  = 0;      // camera dims so px samples can be normalized by consumers
    int   frameHeight = 0;
    // bs0 from the internal hands-only phase model (segmentPhases) — always
    // populated regardless of whether a trace sink was passed, so the ball
    // anchor's address-boundary correction (v3.4) works in fused mode too,
    // not just when a ShaftDecideTrace happens to be requested. -1 = unset.
    int   addressPhaseFrame = -1;
    // No-return onset floor (segmentPhases → PhaseModel::onsetFloor): the LAST
    // address settle the onset veto proved the track never left for good. Mirrors
    // addressPhaseFrame — always exposed (decideTrack copies pm.onsetFloor here
    // unconditionally) so the EventRefine slot can re-floor its refined-Address
    // walk-back (addressHoldEndFrame floorFrame) without threading the trace. -1 =
    // veto dark / never fired. IN-MEMORY ONLY: never serialized to swing.json (it
    // is a same-pass conduit decideTrack → EventRefine, recomputed on re-analysis),
    // so adding it keeps swing.json byte-identical.
    int   onsetFloorFrame = -1;
    std::vector<ShaftSample2D> samples;     // ACTUAL — detector-inferred (vision+IMU fused)
    // R7 dual output (skeleton-aware enhancement): the pure R6 kinematic-model
    // prediction emitted per frame alongside `samples`, plus its agreement with
    // the prior-free vision measurement. Empty / -1 until the K3 phase fills them.
    std::vector<ShaftSample2D> predicted;        // PREDICTED — pure kinematic model, all flags = ShaftKinematicPredicted
    float modelVisionResidualDeg = -1.f;         // RMS|actual − predicted| over prior-free measured frames (-1 = unset)
    // Measured club length in px, grip-to-ball at address (v3.4 design §9.4) — a scale floor for
    // implausibly-short shafts. -1.f = unmeasured (no ball anchor available for this swing).
    float measuredClubLenPx = -1.f;
    // Multi-estimator club-length fusion (club_length_fusion.h): the fused length,
    // its confidence, and every component estimator. Default = no fusion recorded
    // (all absent) — set by decideTrack's post-pass when cfg.fusion.enabled.
    ClubLengthEstimate lengths;
    // Coaching P-positions P1–P8 (shaft_position_first §2 Layer B). Empty =
    // extraction off (cfg.positions.enabled == false) / pre-v3.5 swing — readers
    // treat empty as "no position data" (same contract as `lengths`).
    std::vector<ShaftPosition> positions;
    // VISUALIZATION-TIER synthesized samples (shaft_position_first §2 Layer C):
    // kinematically-interpolated samples on a dense fixed cadence (SynthConfig::
    // rateHz, default 240 Hz) STRICTLY between consecutive `positions` anchors,
    // each flagged ShaftSynthesized. Empty =
    // synthesis off (cfg.synth.enabled == false) / < 2 anchors. EXCLUDED from
    // metrics/scoring/estimands — a smooth-scrub display channel only; the real
    // per-frame track stays in `samples`.
    std::vector<ShaftSample2D> synth;
};

// The IMU→segment binding as persisted in swing.json (keyed by the device
// serial, which is stable across runs — SourceIds are not). Lets the SwingLab
// offline runner re-fuse a recorded swing with the exact calibration the app
// used (alignA/mountM are session-lifetime and otherwise unrecoverable).
struct BindingRecord {
    QString     serial;                       // FormatDescriptor::device_serial
    SegmentRole role = SegmentRole::Unknown;
    QQuaternion alignA, mountM;
    // Calibration status at shot time (mirrors ImuSegmentBinding; see there).
    bool        anatCalibrated = false;
    bool        calibrated     = false;
    double      mountDeviationDeg    = 0.0;
    double      mountGravityErrorDeg = 0.0;
    QString     calibratedAtUtc;
    double      calibAgeSec = -1.0;
};

// Per-stage analyzer wall times (plan §2 telemetry — swing_span_bounding_plan.md):
// self-reported by every shot so the live < 20 s budget is measured, not
// anecdotal. -1 = stage not measured (ball/shaft stay -1 when the pose pass
// produced no frames). Persisted additively (swing.json analysis.timings + runmeta).
struct AnalysisTimings {
    int poseMs  = -1;
    int ballMs  = -1;
    int shaftMs = -1;
    int totalMs = -1;
};

// The rich detail behind ShotAnalysisResult::detail — the full analyzed swing.
struct SwingAnalysis {
    int tier = static_cast<int>(ReconstructionTier::Angles2D);
    std::vector<MetricSeries> series;
    std::vector<PhaseEvent>   phases;
    ScoreBreakdown            score;
    std::vector<Fault>        faults;
    // Tier-2 wrist assessment (faults + strengths) and its score-v2 total. Populated only when
    // the job opts in (ShotAnalysisJob::runAssessment — the SwingLab offline path); empty + -1
    // otherwise, so the live/GUI pipeline and existing swing.json baselines are unchanged.
    std::vector<PpWristFinding> findings;
    int                       assessmentScore = -1;   // score v2 (0..100); -1 = assessment not run
    // Orientation-filter quality diagnostics, populated ONLY when offline re-fusion ran
    // (filter.refuse — validation §5.3.1), so filter.* has an IMU-only objective even without a
    // vision shaft track. -1 = not computed. Max geodesic step (deg) between consecutive fused
    // samples across the impact window — a filter that lets the impact shock in spikes here;
    // blanking/saturation-reject smooth it (lower is better).
    double                    filterImpactStepDeg = -1.0;
    // Swing bounds + ladder meta (phases above IS segmentation.events — the
    // doc writer persists only the bounds/conf/version from here).
    Segmentation              segmentation;
    // IMU calibration snapshot per bound device (empty when no IMUs).
    std::vector<BindingRecord> bindings;
    PoseTrack2D               pose2d;  // face-on offline pose (empty when no camera ran)
    ShaftTrack2D              shaft;   // face-on club track (check .valid before use)
    BallTrack2D               ball;    // face-on ball track for the replay overlay (empty ⇒ none)
    AnalysisTimings           timings; // per-stage wall times (telemetry); -1 = not measured
};

} // namespace pinpoint::analysis

Q_DECLARE_METATYPE(std::shared_ptr<pinpoint::analysis::SwingAnalysis>)
