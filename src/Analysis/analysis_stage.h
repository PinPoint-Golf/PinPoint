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

// Capability-gated stage pipeline over a shared typed context (constrained
// blackboard) — the target orchestration architecture from
// docs/design/analysis_pipeline_fusion_architecture_proposal.md §10. Every future
// fusion proposal (P1–P11) lands as one AnalysisStage with an off-switch instead
// of another branch inside a monolithic analyze(). This header owns only the
// mechanism: the typed context, the stage ABC, the profile, and the dumb
// orchestrator. The per-session stage lists live with their analyzer (wrist
// stages are file-local in wrist_analyzer.cpp until the Swing session needs to
// share them — §10.5 step 4).
//
// Anti-goals (§10.6): no stage registration, no dependency sorting, no
// parallelism. Stages run in authored order; the orchestrator is a loop.

#include <QElapsedTimer>
#include <QSet>
#include <QString>
#include <initializer_list>
#include <memory>
#include <optional>
#include <vector>

#include "shot_analyzer.h"      // ShotAnalysisJob
#include "swing_analysis.h"     // Segmentation, MetricSeries, BallTrack2D, SwingAnalysis, SegmentRole, ImuSegmentBinding
#include "imu_vision_fuser.h"   // FusedStreams
#include "pose_runner.h"        // ShotAnalysisRunnerOptions

namespace pinpoint { class SwingWindow; }

namespace pinpoint::analysis {

// Where a camera sits relative to the golfer. FaceOn is the only placement the
// current analysis consumes (pose/shaft/head/foot all run off it); DownTheLine
// is reserved for the stereo/DTL fusion proposals and is not populated yet.
enum class CameraPlacement { FaceOn, DownTheLine };

// The capture's device inventory, resolved once from the job before any stage
// runs. Stages gate on it (canRun) so a webcam-only or IMU-only capture skips
// the stages it can't feed instead of branching inside them.
struct CaptureCapabilities {
    struct BoundImu {
        SegmentRole role        = SegmentRole::Unknown;
        bool        calibValid  = false;   // composite calibration gate (ImuSegmentBinding::calibrated)
        double      calibAgeSec = -1.0;    // age at shot time; -1 = never calibrated

        // ── Deferred sources (design §4.4) ───────────────────────────────────
        // ⚠ MEASURED OVER THIS WINDOW, NEVER TAKEN FROM A DEVICE DECLARATION.
        // That is the whole point: a pull that silently under-delivered then
        // gates exactly like no pull at all. The device HOLES an over-wide
        // request rather than clamping it — 33-58 % coverage with no error — so
        // a stage that trusted a declared rate would run on a window that is
        // mostly absent and report a number for it.
        //
        // ⚠ AND THIS IS A RATE, NOT A COUNT. Coverage is intervals and density;
        // a sample count cannot distinguish a dense span from a holed one.
        double      effectiveHz = 0.0;
        // The sub-span that exceeded the base rate, if any — empty when the lane
        // is uniformly live-rate. A stage needing high-rate input can then land
        // dark and skip with a reason until the data is actually present.
        int64_t     highRateSpanUs[2]{};
    };

    QSet<CameraPlacement> cameras;
    std::vector<BoundImu> imus;

    bool hasCamera(CameraPlacement p) const { return cameras.contains(p); }
    bool hasRole(SegmentRole r) const {
        for (const BoundImu &b : imus)
            if (b.role == r) return true;
        return false;
    }
    bool hasRoles(std::initializer_list<SegmentRole> roles) const {
        for (SegmentRole r : roles)
            if (!hasRole(r)) return false;
        return true;
    }

    // One BoundImu per job.imuBindings entry (imus.empty() == imuBindings.empty()) —
    // the fuser still drops Unknown-role / under-sampled bindings downstream, so a
    // present-but-unfusable binding still counts here (matching the monolith, where
    // the resample stage runs and hasImuStreams() then reports the empty result).
    // FaceOn present iff the job carries a face-on camera source — the exact gate
    // the monolith's `hasCamera` local used.
    static CaptureCapabilities fromJob(const ShotAnalysisJob &job)
    {
        CaptureCapabilities caps;
        if (job.faceOnCameraCount > 0 && !job.cameraSources.empty())
            caps.cameras.insert(CameraPlacement::FaceOn);
        caps.imus.reserve(job.imuBindings.size());
        for (const ImuSegmentBinding &b : job.imuBindings)
            caps.imus.push_back(BoundImu{ b.role, b.calibrated, b.calibAgeSec });
        return caps;
    }

    // As above, but with the rate fields MEASURED FROM THE WINDOW rather than
    // left at zero (design §4.4) — so a stage gating on high-rate input sees
    // what this capture actually carries, not what a device claimed.
    static CaptureCapabilities fromJob(const ShotAnalysisJob &job,
                                       const pinpoint::SwingWindow &window)
    {
        CaptureCapabilities caps = fromJob(job);
        // ⚠ MEASURED AROUND IMPACT, not across the whole window. A stitched lane
        // is ~100 Hz over the still pre-roll and full rate through the swing, so
        // a window-wide average answers a question nobody asked: what gates a
        // metric computed AT IMPACT is the density THERE. ±125 ms is the region
        // the library's own guidance names for exactly this decision.
        constexpr int64_t kImpactHalfSpanUs = 125'000;
        if (job.impactUs <= 0)
            return caps;   // no anchor to measure around — leave NOT MEASURED
        const int64_t lo = job.impactUs - kImpactHalfSpanUs;
        const int64_t hi = job.impactUs + kImpactHalfSpanUs;
        for (size_t i = 0; i < caps.imus.size() && i < job.imuBindings.size(); ++i) {
            const SourceId src = job.imuBindings[i].source;
            caps.imus[i].effectiveHz =
                ImuVisionFuser::effectiveHzFor(window, src, lo, hi);
            ImuVisionFuser::highRateSpanFor(window, src,
                                            ImuVisionFuser::kGridHzMin,
                                            caps.imus[i].highRateSpanUs);
        }
        return caps;
    }
};

// Per-stage orchestration record — timing + skip provenance for the log/telemetry
// only. NEVER serialized (AnalysisTimings keeps its four fields; this is a
// separate, richer per-stage trace that stays in memory).
struct StageTraceEntry {
    QString name;
    bool    ran        = false;   // false ⇒ the stage was skipped
    QString skipReason;           // "halted", or the canRun skip reason; empty when ran
    qint64  elapsedNs  = 0;       // run() wall time; 0 when skipped
};

// The shared typed context the stages read and write — a constrained blackboard.
// Typed slots (not a bag of variants) so a stage's inputs/outputs are the compiler's
// business. `window` is a pointer so the orchestrator is unit-testable with no
// window (nullptr). Aggregate: construct with { caps, job, &window }; every other
// slot default-initializes.
struct AnalysisContext {
    CaptureCapabilities          caps;
    const ShotAnalysisJob        &job;
    const pinpoint::SwingWindow  *window = nullptr;

    FusedStreams                 streams;      // resampled IMU streams (ImuResample)
    bool                         doRefuse = false;
    std::optional<Segmentation>  segImu;       // IMU-derived segmentation (pre-adoption)
    std::optional<Segmentation>  segVision;    // camera-derived segmentation (pre-adoption)
    Segmentation                 seg;          // resolved segmentation (SegResolve onward)
    std::vector<MetricSeries>    series;        // LOCAL series — the scorer/metrics/trace read this
    std::optional<ShotAnalysisRunnerOptions> runnerOpt;   // pose/ball runner knobs (Pose stage)
    std::optional<BallTrack2D>   ball;          // resolved ball track (Ball stage)
    std::shared_ptr<SwingAnalysis> detail;      // the rich SwingAnalysis, written in place
    bool                         halted = false;
    QString                      haltError;
    QElapsedTimer                wall;           // whole-analysis wall time
    std::vector<StageTraceEntry> trace;

    // Fusable IMU orientation present — the monolith's `hasImu` gate verbatim.
    bool hasImuStreams() const {
        return !streams.timeGrid.empty() && !streams.segments.empty();
    }
};

// One pipeline stage. canRun gates on capabilities/context; skipReason annotates a
// skip for the trace; run mutates the context. Stages own no state — all state
// lives in the context, so a profile is reusable and order is the only contract.
class AnalysisStage {
public:
    virtual ~AnalysisStage() = default;
    virtual QString name() const = 0;
    virtual bool    canRun(const AnalysisContext &) const { return true; }
    virtual QString skipReason(const AnalysisContext &) const { return QString(); }
    virtual void    run(AnalysisContext &) = 0;
};

// An ordered, named stage list for one session type.
struct SessionProfile {
    QString                                     name;
    std::vector<std::unique_ptr<AnalysisStage>> stages;
};

// The orchestrator: authored order, halted short-circuit (recorded as skip
// "halted"), canRun gate (recorded with the stage's skipReason), per-stage wall
// clock into ctx.trace only. No registration, no sorting, no parallelism.
inline void runStages(const SessionProfile &profile, AnalysisContext &ctx)
{
    for (const std::unique_ptr<AnalysisStage> &stage : profile.stages) {
        StageTraceEntry e;
        e.name = stage->name();
        if (ctx.halted) {
            e.skipReason = QStringLiteral("halted");
        } else if (!stage->canRun(ctx)) {
            e.skipReason = stage->skipReason(ctx);
        } else {
            QElapsedTimer t;
            t.start();
            stage->run(ctx);
            e.ran       = true;
            e.elapsedNs = t.nsecsElapsed();
        }
        ctx.trace.push_back(std::move(e));
    }
}

} // namespace pinpoint::analysis
