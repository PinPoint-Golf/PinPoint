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

#include "shot_analyzer.h"
#include "wrist_analyzer.h"
#include "analysis_stage.h"
#include "analysis_profiling.h"
#include "kinematic_series.h"

#include <QPointF>
#include <algorithm>
#include <cmath>
#include <memory>

#include "swing_window.h"
#include "../Core/pp_debug.h"

namespace {

// Deterministic pseudo-random trace, normalised to 0..1 — moved verbatim from
// shot_list_model.cpp's stub seeding so each stub-analysed shot gets a distinct
// but stable curve until the real analysis pipelines land.
QVariantList stubTracePoints(int seed)
{
    QVariantList pts;
    double y = 0.6;
    for (int i = 0; i <= 12; ++i) {
        const double noise = double((seed * 9301 + i * 49297) % 233280) / 233280.0 - 0.5;
        y += (std::sin(i * 1.2 + seed) * 0.5 + noise) * 0.16;
        y = std::clamp(y, 0.16, 0.84);
        pts.append(QPointF(i / 12.0, y));
    }
    return pts;
}

// Deterministic placeholder score from the impact timestamp — distinct per shot,
// stable across re-runs. Shared by the dark stub path and the kinematics analyzer.
int stubScore(const ShotAnalysisJob &job)
{
    return 40 + static_cast<int>((job.impactUs / 1000) % 56);   // 40–95
}

// Shared stub behaviour: an immediate, deterministic placeholder result.
ShotAnalysisResult stubResult(const pinpoint::SwingWindow &window,
                              const ShotAnalysisJob &job,
                              QVariantMap metrics)
{
    ppInfo() << "[ShotAnalysis] stub analysis — sessionType" << job.sessionType
             << "impact_ts_us" << job.impactUs
             << "window entries" << static_cast<qint64>(window.entries().size());

    ShotAnalysisResult r;
    r.ok          = true;
    r.score       = stubScore(job);
    r.tracePoints = stubTracePoints(static_cast<int>(job.impactUs % 97));
    r.metrics     = std::move(metrics);
    return r;
}

// Non-Wrist session analyzer (Swing 0 / GRF 2 / Coach 3). Placeholder score/trace like
// the old per-type stubs, PLUS the real camera-derived kinematic series (clubhead/hand
// speed + lag) on the detail so the review chart shows them for every session type. The
// camera pipeline (cameraKinematicsProfile — Pose→Ball→Shaft→…→Kinematics) runs ONLY
// when kinematics.enabled; dark, this is byte-identical to the old instant stub (no
// pose/shaft compute). Real Swing/GRF/Coach scoring profiles land later
// (analysis_pipeline_developer_guide.md §4) — this reuses the shared camera stages now.
class CameraKinematicsAnalyzer : public ShotAnalyzer
{
public:
    ShotAnalysisResult analyze(const pinpoint::SwingWindow &window,
                               const ShotAnalysisJob &job) override
    {
        using namespace pinpoint::analysis;
        // Dark ⇒ the old instant stub, unchanged (the camera pipeline never runs).
        if (!KinematicSeriesConfig::fromOverrides(job.tuningOverrides).enabled)
            return stubResult(window, job, {});

        // Enabled ⇒ run the shared camera profile to produce the real kinematic series.
        AnalysisContext ctx{ CaptureCapabilities::fromJob(job, window), job, &window };
        ctx.detail = std::make_shared<SwingAnalysis>();
        ctx.wall.start();
        runStages(cameraKinematicsProfile(), ctx);

        ShotAnalysisResult r;
        r.ok          = true;
        r.score       = stubScore(job);
        r.tracePoints = stubTracePoints(static_cast<int>(job.impactUs % 97));
        // Attach the detail only when the camera actually produced series, so a
        // no-camera shot stays stub-identical (no empty analysis block persisted).
        if (!ctx.detail->series.empty()) {
            ctx.detail->timings.totalMs = static_cast<int>(ctx.wall.elapsed());
            r.detail = ctx.detail;
        }
        recordAnalysisRun(QStringLiteral("CameraKinematics"), ctx);   // per-stage profiler + run history
        return r;
    }
};

} // namespace

std::unique_ptr<ShotAnalyzer> makeShotAnalyzer(int sessionType)
{
    // Wrist (1) runs the full IMU + camera Wrist profile. Swing (0) / GRF (2) / Coach (3)
    // run the shared camera-kinematics analyzer — a placeholder score today, plus the real
    // clubhead/hand speed + lag series from the face-on camera once kinematics.enabled.
    if (sessionType == 1)
        return std::make_unique<WristAnalyzer>();
    return std::make_unique<CameraKinematicsAnalyzer>();
}
