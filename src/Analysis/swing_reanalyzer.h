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

#include <QString>
#include <QVariantMap>
#include <optional>

#include "swing_window.h"
#include "shot_analyzer.h"

// SwingReanalyzer — reconstruct a SwingWindow from an exported swing folder and
// re-run the production analyzer OFFLINE, the same disk → SwingWindow → analyzer
// flow the SwingLab runner uses, but STREAMING camera frames from disk on demand
// rather than rebuilding a full EventBuffer (which would hold the whole window —
// multi-GB at high FPS — in RAM). Frames are read one at a time into a single
// reusable buffer per camera (raw sidecar offset reads, else cv::VideoCapture);
// IMU samples and the frame index live in RAM (tiny). Perf is sacrificed for
// bounded memory — re-analysis is a rare, offline operation.
//
// Shared by the in-app re-analyse path (ReanalysisController) and the
// swinglab_run tool, so both exercise one tested reconstruction.

namespace pinpoint::analysis {

// Tunables for which camera is treated as face-on. Default uses the recorded
// per-stream setup.perspective; the offline tool's --face-on forces a substring
// match (escape hatch for mislabelled recordings).
struct SwingLoadOptions {
    QString faceOnSubstring = QStringLiteral("Face");
    bool    faceOnExplicit  = false;
};

// A swing reconstructed into a streaming, disk-backed SwingWindow, plus the
// ShotAnalysisJob resolved from swing.json (sessionType, face-on-first camera
// sources, IMU sources, serial-matched A/M bindings, impact, handedness).
struct LoadedSwing {
    bool                                 ok = false;
    QString                              error;
    std::optional<pinpoint::SwingWindow> window;   // disk-backed; streams on demand
    ShotAnalysisJob                      job;
    bool                                 usedRaw = false;  // any camera used the raw sidecar
};

class SwingDiskLoader {
public:
    // Reconstruct from <swingDir>/swing.json + its media sidecars. Never opens the
    // live EventBuffer. On failure returns { ok=false, error set, window=nullopt }.
    static LoadedSwing load(const QString& swingDir, const SwingLoadOptions& opts = {});
};

struct ReanalyzeOptions {
    QVariantMap tuningOverrides;            // "<area>.<field>" → numeric (SwingLab)
    int         sessionTypeOverride = -1;   // -1 → use swing.json capture.sessionType
    QString     poseTrackPath;              // inject pose JSON, skip ViTPose (tests / --pose)
    // Forwarded to ShotAnalysisJob::fullWindow — disables the swing-span heavy-
    // stage bound so the whole captured window (not just the padded swing span)
    // is scanned. The in-app ReanalysisController sets this true on every
    // explicit re-analyse; SwingLab leaves it false to keep sweeps comparable
    // to production's live-capture bound unless a run opts in.
    bool        fullWindow = false;
};

struct ReanalyzeResult {
    bool               ok = false;
    QString            error;
    ShotAnalysisResult analysis;            // score / metrics / trace / detail
    bool               usedRaw = false;
};

// Convenience for the app: load + run makeShotAnalyzer(sessionType)->analyze().
// Self-contained — touches no live device. The analyzer call is wrapped so a
// thrown analyzer degrades to { ok=false, error } rather than propagating.
ReanalyzeResult reanalyzeSwingDir(const QString& swingDir, const ReanalyzeOptions& opts = {});

} // namespace pinpoint::analysis
