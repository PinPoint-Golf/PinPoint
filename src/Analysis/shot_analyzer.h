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

#include <QRectF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <memory>
#include <vector>

#include "types.h"
#include "swing_analysis.h"

namespace pinpoint { class SwingWindow; }

// Value-type analysis job — everything is resolved on the UI thread before the
// job is handed to the worker (same rule as SwingExportJob): the worker must
// never touch AppSettings or controllers.
struct ShotAnalysisJob {
    int     sessionType = -1;   // SessionController::Type (Swing 0, Wrist 1, GRF 2, Coach 3)
    int     shotSource  = 0;    // ShotController::Source as int (avoids a Gui header dep)
    qint64  impactUs    = -1;   // impact instant, EventBuffer::nowMicros() domain

    std::vector<pinpoint::SourceId> cameraSources;  // exported cameras, face-on first
    int faceOnCameraCount = 0;  // leading cameraSources entries that are face-on
    std::vector<pinpoint::SourceId> imuSources;     // IMU sources present in the window
    pinpoint::SourceId markerSourceId = pinpoint::kInvalidSourceId;  // shot_marker_v1 source

    double clubLengthM = 1.12;  // shaft search radius (driver default until club selection is real)
    // Retro-band geometry for the v3 E1 band matcher (taped clubs only). Band
    // centres measured from the butt (mm) + shaft type, taken from the athlete's
    // active club record (athlete_controller.h bandCentersMm/shaftType). Empty ⇒
    // untaped club: the shaft tracker runs E2 (ray) evidence only, no band tier.
    // Filled on the UI thread in shot_processor buildAnalysisJob.
    std::vector<double> bandCentersMm;
    QString             shaftType;   // "steel" | "graphite" | "" (unknown)
    double hoselFromButtMm = 0.0;  // hosel offset from the butt (mm), athlete club record; 0 = unknown

    // Persistent club-length prior (club_length_fusion.h). clubName is the
    // canonical club-vocabulary id — half the prior key (athleteUuid|clubName|
    // cameraKey; the rest resolved in ShotProcessor). priorClubLen* carry the
    // persisted LengthPriorState so decideTrack can join E-prior into the fuse:
    // filled by ShotProcessor from AppSettings on the LIVE path, or by
    // SwingDiskLoader from the recorded swing.json on RE-ANALYSIS (deterministic,
    // never AppSettings) — Agent B wires both. Defaults (N=0) ⇒ no prior joins.
    QString clubName;
    double  priorClubLenPx    = -1.0;  // LengthPriorState::emaPx (px); <0 ⇒ uninitialised
    double  priorClubLenVarPx = -1.0;  // LengthPriorState::varPx (px²); <0 ⇒ absent
    int     priorClubLenN     = 0;     // LengthPriorState::n (updates folded in); 0 ⇒ no prior

    // Resolved IMU -> anatomical-segment bindings (placement slot + the live
    // calibration A/M snapshot), filled on the UI thread — the worker cannot
    // read the live ImuInstance. Empty when no IMU is bound.
    std::vector<pinpoint::analysis::ImuSegmentBinding> imuBindings;
    int     handedness = 0;     // 0 unknown, 1 right, 2 left (lead-arm sign)
    QString swingDir;           // swing folder for persistence (set at the join)

    // Motion-capture quality tier ("Medium"/"High") selecting the offline pose
    // model: "High" runs ViTPose++-L when downloaded, everything else runs
    // ViTPose-B (pose_model_selection.h::useVitPoseLarge). Filled on the UI thread
    // in ShotProcessor::buildAnalysisJob from AppSettings on the LIVE path, or by
    // SwingDiskLoader from the recorded swing.json on RE-ANALYSIS (deterministic,
    // never AppSettings). Empty ⇒ ViTPose-B (legacy/default).
    QString motionCaptureQuality;

    // Optional progress sink, 0..1 over the whole analysis. Called from the
    // WORKER thread — the installer must marshal to its own thread (the
    // ShotProcessor lambda posts a queued invoke). May be null; analyzers
    // must check before calling.
    std::function<void(float)> progress;

    // SwingLab: when set, the analyzer loads the face-on PoseTrack2D from
    // this JSON file instead of running ViTPose — synthetic-corpus injection
    // and pose-cache reuse during shaft tuning. Empty in production.
    QString poseTrackPath;

    // Face-on ball track (v3.4 design §9), resolved on the UI thread from the live
    // CameraInstance's ball accumulator or a recorded swing.json "ball" block.
    // Empty ⇒ the shaft tracker's offline ball-anchor pass falls back to replaying
    // the production ball detector over the frozen window (BallRunner::run) —
    // never a failure, just no anchor for that swing (design §9.6, additive-only).
    pinpoint::analysis::BallTrack2D ballTrack;

    // SwingLab: when set, the analyzer loads an INJECTED ground-truth BallTrack2D
    // from this JSON file instead of ballTrack/BallRunner — synthetic-fixture
    // injection, mirrors poseTrackPath. Empty in production.
    QString ballTrackPath;

    // Hitting-area ROI (full-frame normalized) captured with the swing. When set,
    // BallRunner searches this box instead of the pose-derived stance corridor,
    // so offline ball detection matches the live detector and skips out-of-box
    // distractors (feet/shoes). Empty ⇒ fall back to the stance corridor.
    QRectF ballSearchRoi;

    // Persisted live empty-mat baseline, resolved from swing.json
    // setup.ballDetection.baseline. ONLY SwingDiskLoader fills it (live capture
    // never does) — that scoping is what confines the precedence change to
    // re-analysis. Empty ⇒ BallRunner self-seeds over the window as before.
    pinpoint::analysis::BallBaselineRef ballBaseline;

    // SwingLab tuning overrides (docs/implementation/swinglab_impl.md):
    // "<area>.<field>" → numeric value, applied onto the config structs at
    // analysis time (e.g. "shaft.ridgeKernelPx", "assembly.coverageMin",
    // "seg.backswingMinUs"). Empty in production — the app never sets it;
    // the offline runner fills it from a params JSON so the lab can iterate
    // without rebuilds. Unknown keys are logged and ignored.
    QVariantMap tuningOverrides;

    // SwingLab: run the Tier-2 wrist assessment engine (faults/strengths + score v2) inside
    // the offline analyzer and emit findings into swing.json, so known-groups diagnosis is
    // observable/tunable (sampler.*/rules.*/bands.* — pipeline_validation_and_tuning.md §5.6).
    // Default OFF: the production GUI runs assessment in its own diagnostics model; only the
    // lab opts in, so live behaviour and existing baselines are untouched.
    bool runAssessment = false;

    // Skip the heavy-stage swing-span bounding (v3 G3 pose/ball scan window +
    // shaft.spanBound) and process the entire captured window instead. Live
    // capture always leaves this false (throughput matters, the padded span
    // already covers address→finish); explicit re-analysis sets it true
    // (correctness over speed — see ReanalyzeOptions::fullWindow).
    bool fullWindow = false;
};

// Result shapes mirror the ShotListModel roles so the join can hand them to
// addShot() unmodified: metrics is key → { label, value }, tracePoints is a
// normalised (0..1) QPointF list.
struct ShotAnalysisResult {
    bool         ok = false;
    int          score = 0;        // 0–100 quality
    QVariantMap  metrics;
    QVariantList tracePoints;
    QString      error;

    // Rich analyzed-swing detail (skeleton/series/score/faults). Null for the
    // stub analyzers; folded into swing.json's "analysis" object at the join.
    std::shared_ptr<pinpoint::analysis::SwingAnalysis> detail;
};

// Abstract per-session-type shot analyzer. analyze() runs on a QtConcurrent
// worker thread while the EventBuffer is Paused; it may only read the frozen
// SwingWindow (const methods, zero-copy) and the job values. It runs
// concurrently with the swing export over the same window — both are pure
// readers of frozen ring memory.
class ShotAnalyzer
{
public:
    virtual ~ShotAnalyzer() = default;
    virtual ShotAnalysisResult analyze(const pinpoint::SwingWindow &window,
                                       const ShotAnalysisJob &job) = 0;
};

// Factory keyed by SessionController::Type. Unknown / -1 falls back to the
// generic stub. Never returns nullptr.
//
// v1 ships stub analyzers that return immediately with placeholder
// score/metrics/trace — the real per-type analysis pipelines are future work.
std::unique_ptr<ShotAnalyzer> makeShotAnalyzer(int sessionType);
