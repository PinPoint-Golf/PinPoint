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

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <QHash>
#include <QVector>
#include <memory>
#include <optional>
#include <vector>

#include "swing_window.h"
#include "shot_controller.h"
#include "hm_instance.h"
#include "../Analysis/club_length_fusion.h"
#include "../Analysis/shot_analyzer.h"
#include "../Export/swing_exporter.h"
#include "../Export/swing_paths.h"

namespace pinpoint { class EventBuffer; }
class AppSettings;
class AthleteController;
class CameraInstance;
class CameraManager;
class ImuManager;
class SessionController;
class ShotListModel;

// Orchestrates the post-shot pipeline (QML context property `shotProcessor`):
//
//   shotDetected ─► POSTROLL  per-source delay; the buffer keeps capturing so
//                             the follow-through lands in the ring
//                ─► pauseBuffer → captureSwingWindow(4 s trailing ring)
//                ─► PROCESSING analysis (ShotAnalyzer) ∥ export (SwingExporter)
//                             — both QtConcurrent workers reading the frozen
//                             window concurrently (const, zero-copy)
//                ─► join      ShotListModel::addShot() always; then
//                ─► REPLAYING ¼× on-screen replay iff analysis AND export
//                             both succeeded
//                ─► finish    window destroyed → applyCaptureIntent() → Idle
//                             (bufferStateChanged re-arms ShotController)
//
// Owns the SwingWindow for its entire lifetime (migrated from CameraManager).
// The window is only valid while the buffer stays Paused: every resume path
// funnels through CameraManager::resumeBuffer(), which is blocked while
// EventBuffer::swingWindowLive() — and the window is destroyed only in
// finishShot() or after the blocking joins in finishNowBlocking().
class ShotProcessor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state       READ stateName   NOTIFY stateChanged)
    Q_PROPERTY(bool    busy        READ busy        NOTIFY busyChanged)
    Q_PROPERTY(bool    isReplaying READ isReplaying NOTIFY isReplayingChanged)
    // Analysis worker progress, 0..1 — drives the toolbar ANALYSING bar.
    // Reset on each shot; reported from the worker via the job's progress
    // callback (queued to the UI thread, throttled to ~1% steps).
    Q_PROPERTY(double analysisProgress READ analysisProgress NOTIFY analysisProgressChanged)
    // Replay playhead — the single source of truth both the video tile and the
    // synced metric graph bind to. EventBuffer::nowMicros() domain, == MetricSeries
    // t_us, so no conversion. Position fires per ~60 Hz tick; span/impact on start.
    Q_PROPERTY(qint64  replayPositionUs READ replayPositionUs NOTIFY replayPositionChanged)
    Q_PROPERTY(qint64  replayStartUs    READ replayStartUs    NOTIFY replaySpanChanged)
    Q_PROPERTY(qint64  replayEndUs      READ replayEndUs      NOTIFY replaySpanChanged)
    Q_PROPERTY(qint64  replayImpactUs   READ replayImpactUs   NOTIFY replaySpanChanged)
    // The analyzed swing detail of the shot currently replaying — the ScreenWrist
    // in-replay graph binds to it (same shape as ShotListModel's analysisDetail role).
    Q_PROPERTY(QVariantMap replayAnalysisDetail READ replayAnalysisDetail NOTIFY replayAnalysisDetailChanged)
    // The active session folder (absolute path), chosen at session start via
    // beginSessionFolder(). "" when no explicit session is in progress.
    Q_PROPERTY(QString activeSessionDir READ activeSessionDir NOTIFY activeSessionDirChanged)

public:
    // Gathering sits between PostRoll and Processing — deferred_sources_design.md
    // §4.1. The rings freeze at the SAME instant as before; what moves is when
    // the window is CONSTRUCTED, which now waits for any deferred source to
    // report or time out. busy() is already `m_state != Idle`, so ShotController
    // stays disarmed across it for free.
    enum class State { Idle, PostRoll, Gathering, Processing, Replaying };

    ShotProcessor(pinpoint::EventBuffer *buffer,
                  CameraManager         *cameraManager,
                  ImuManager            *imuManager,
                  AppSettings           *appSettings,
                  AthleteController     *athleteController,
                  SessionController     *sessionController,
                  ShotListModel         *shotModel,
                  QObject               *parent = nullptr);
    ~ShotProcessor() override;

    QString stateName()   const;
    bool    busy()        const { return m_state != State::Idle; }
    bool    isReplaying() const { return m_state == State::Replaying; }
    qint64  replayPositionUs() const { return m_replayPositionUs; }
    qint64  replayStartUs()    const { return m_replayWindowStartUs; }
    qint64  replayEndUs()      const { return m_replayWindowEndUs; }
    qint64  replayImpactUs()   const { return m_impactUs; }
    QVariantMap replayAnalysisDetail() const { return m_replayAnalysisDetail; }
    double  analysisProgress() const { return m_analysisProgress; }
    QString activeSessionDir() const { return m_swingPaths.currentSessionDir(); }
    // The folder allocated for the shot being processed, or the last one processed.
    // Set BEFORE the export runs, so it survives a shot that produced no document at
    // all — which is exactly when a launch monitor reading has somewhere to go and the
    // rest of the pipeline does not. Empty when no shot has been processed.
    QString lastSwingDir() const { return m_swingDir; }
    // The carousel row id the last processed shot was given. addShot() runs even when
    // everything else failed, so a shot that produced no document still HAS a row —
    // in-memory, with no swingDir. A launch monitor filling that folder must point that
    // row at it rather than adding a second one. -1 when no shot has been processed.
    int lastShotId() const { return m_lastShotId; }

    // User-initiated skip (ESC). Only meaningful mid-replay: the shot is
    // already on the carousel and saved by the time the replay runs, so
    // cancelling is just the normal end-of-replay path taken early.
    Q_INVOKABLE void cancelReplay();

    // Teardown stop-barrier (camera deselect, destructors). Cancels any pending
    // post-roll, stops replay, BLOCKS until both workers return, destroys the
    // window. Deliberately does not touch buffer state — the caller owns the
    // pause/deregister/intent sequence around source registration.
    void finishNowBlocking();

    // ── Session folder lifecycle (QML) ──────────────────────────────────────
    // The wrist session screen decides the on-disk session folder up front (at
    // Capture / wizard start) rather than lazily at first save, so it can offer
    // "extend today vs new" and discard a session that captured nothing.

    // Most-recent existing session folder for TODAY (this athlete + session type),
    // or "" when none. Drives the Extend/New prompt and the today-scoped carousel.
    Q_INVOKABLE QString todaySessionDir(int sessionType);
    // Begin the session's folder: extend today's most-recent one, or allocate a
    // fresh "_NN". Every swing captured this session lands in it.
    Q_INVOKABLE void beginSessionFolder(int sessionType, bool extend);
    // End the session's folder: discard it (to the OS trash) when it captured no
    // new swings since beginSessionFolder(). Safe with no session in progress.
    Q_INVOKABLE void endSessionFolder();

public slots:
    void onShotDetected(ShotController::Source source, qint64 timestampUs, int sessionType);

signals:
    void stateChanged();
    void busyChanged();
    void isReplayingChanged();
    void analysisProgressChanged();
    void replayPositionChanged();
    void replaySpanChanged();
    void replayAnalysisDetailChanged();
    void activeSessionDirChanged();
    // analysis+export join reached, all ok — carries the reviewable on-disk swing
    // (swingDir) and its carousel row id so the UI can promote it straight into Review.
    void shotProcessed(int shotId, const QString &swingDir);
    void shotFailed(const QString &error);
    void swingSaved(const QString &path);
    void swingSaveFailed(const QString &error);
    // Analyzer returned !ok — the shot degrades to no-score/no-metrics but the
    // pipeline continues. Surfaced as a window-level toast (Main.qml), not a
    // log line: the user must see why a shot has no analysis.
    void analysisFailed(const QString &error);

private slots:
    void onPostRollExpired();
    void onSegmentationFinished();
    void onAnalysisFinished();
    void onSwingSaveFinished();
    void onReplayTick();

private:
    enum class Outcome { Pending, Succeeded, Failed, Skipped };

    // Per-camera ball-detector state frozen at window-capture time. Both job
    // builders run 12–37 s after impact (from onAnalysisFinished), by when the
    // live CameraInstance accumulator has scrolled to post-shot junk (ball
    // detection never stops) and a phantom re-launch — the struck ball rolling —
    // may have overwritten ballLaunchInfo(). So we snapshot the instant the
    // SwingWindow is frozen, when the 6 s accumulator still fully covers the 4 s
    // window; both builders read this, never live controller state. Samples
    // mirror CameraInstance::BallAccumSample (POD, avoids a header dependency).
    struct BallSnapshot {
        struct Sample { qint64 tUs = 0; bool found = false; float x = 0.f, y = 0.f, r = 0.f, conf = 0.f; };
        std::vector<Sample> samples;
        bool   hasLaunch = false;
        qint64 launchTUs = -1;
        double launchX = 0.0, launchY = 0.0;
    };

    struct ReplayTrack {
        CameraInstance                    *ctrl     = nullptr;
        pinpoint::SourceId                 sourceId = pinpoint::kInvalidSourceId;
        std::vector<pinpoint::IndexEntry>  entries;
        size_t                             idx      = 0;
        BallSnapshot                       ball;   // frozen at window capture
    };

    int  m_lastShotId = -1;

    void setState(State s);
    void setAnalysisProgress(double p);
    // The two halves of what captureWindowAndLaunch() used to do in one breath.
    // Every deferred source bound to this capture. One wG3 is ONE peripheral
    // with one reservation, even though it feeds two lanes.
    QVector<HmInstance *> deferredSources() const;
    void beginGather();             // pause the rings, raise the guard, start the wait
    void onGatherPoll();            // deferred sources reported, or the deadline passed
    void finishGatherAndLaunch();   // compose the backing, construct the window, launch
    ShotAnalysisJob buildAnalysisJob();   // UI-thread value resolution
    void startAnalysis();
    void startSwingSave();
    pinpoint::SwingExportJob buildSwingExportJob();
    // Minimal raw header (athlete/session/clock/window, no streams) synthesised
    // from the cached export job + the live window — used to persist an
    // analysis-only swing.json when the media export failed or was skipped.
    QJsonObject buildSynthManifest() const;
    void maybeJoin();
    void startReplay();
    void stopReplay(bool thenFinish);
    void finishShot();
    void abortToIdle();

    // Dependencies — UI-thread only; workers see value-type jobs.
    pinpoint::EventBuffer *m_buffer        = nullptr;
    CameraManager         *m_cameraManager = nullptr;
    ImuManager            *m_imuManager    = nullptr;
    AppSettings           *m_appSettings   = nullptr;
    AthleteController     *m_athlete       = nullptr;
    SessionController     *m_session       = nullptr;
    ShotListModel         *m_shotModel     = nullptr;

    State  m_state = State::Idle;
    double m_analysisProgress = 0.0;

    // Pending shot, captured at trigger time.
    ShotController::Source m_shotSource = ShotController::Source::Manual;
    qint64  m_impactUs    = -1;
    int     m_sessionType = -1;
    QString m_timestampLabel;   // wallclock "hh:mm:ss" at trigger
    QTimer  m_postRollTimer;    // single-shot post-trigger capture continuation

    // ── Deferred gather (design §4.1) ───────────────────────────────────────
    // ⚠ m_ringSource HOLDS THE RESUME GUARD from the pause instant until the
    // window that adopts it dies. It is built at pause rather than at window
    // construction because `resume_clear_rings` is true, so a resume landing in
    // between would CLEAR THE RINGS THIS SHOT IS ABOUT TO SNAPSHOT (§3.5). Any
    // path that abandons a gather must reset it, or the buffer can never resume.
    std::unique_ptr<const pinpoint::SwingPayloadSource> m_ringSource;
    QTimer  m_gatherTimer;          // polls the deferred sources during Gathering
    qint64  m_gatherDeadlineMs = 0; // QDateTime msecs; the wait is bounded
    // ⚠ FROZEN AT THE PAUSE INSTANT, NOT READ AFTER THE GATHER. The window is a
    // TRAILING span, so computing it from a post-gather `now` would slide it
    // several seconds forward — past the swing it is supposed to contain — and
    // the snapshot would come back empty with nothing reporting an error.
    int64_t m_windowStartUs = 0;
    int64_t m_windowEndUs   = 0;
    // What each pull actually delivered, keyed by device id — coverage, gaps,
    // the fit and the overlap counters. Read by the exporter into swing.json's
    // per-stream provenance, so a swing says a year later what it was built from.
    QHash<QString, HmInstance::HistoryResult> m_historyProvenance;
    // Per stitched lane: {live samples used, deferred samples used}. Keyed by
    // source because the two units of one peripheral stitch independently — the
    // palm can come back denser than the lower arm.
    QHash<pinpoint::SourceId, std::pair<int, int>> m_stitchCounts;

    // Window + replay (migrated from CameraManager).
    std::optional<pinpoint::SwingWindow> m_swingWindow;
    std::vector<ReplayTrack> m_replayTracks;
    int64_t       m_replayWindowStartUs = 0;
    int64_t       m_replayWindowEndUs   = 0;
    int64_t       m_replayPositionUs    = 0;   // published playhead (window µs)
    QVariantMap   m_replayAnalysisDetail;      // detail of the shot being replayed
    QElapsedTimer m_replayElapsed;
    QTimer       *m_replayTimer = nullptr;

    // Segmentation pre-stage (v3 G2): a cheap fuse+segment pass on the frozen
    // window whose future GATES both heavy workers — its swing bounds trim the
    // export encode span and the replay, and ride the jobs as values. Borrows
    // &*m_swingWindow like the workers, so finishNowBlocking() must join it.
    QFutureWatcher<pinpoint::analysis::Segmentation> m_segmentationWatcher;
    bool                            m_segmentationInFlight = false;
    pinpoint::analysis::Segmentation m_segmentation;   // result for THIS shot
    ShotAnalysisJob                 m_analysisJob;      // resolved pre-stage, used at launch

    // Persistent club-length prior (club_length_fusion.h / plan: robust club
    // length — starry-shimmying-wind). Stashed by buildAnalysisJob() when the
    // face-on camera is cameraFixedInPlace (else left empty/cold — nothing to
    // update); onAnalysisFinished() folds the recorded fuse's fusedInstant* into
    // m_lengthPriorState and persists it to AppSettings under m_lengthPriorKey.
    // Cleared at the start of every shot alongside m_analysisResult.
    QString                            m_lengthPriorKey;
    pinpoint::analysis::LengthPriorState m_lengthPriorState;

    // Workers. Both borrow &*m_swingWindow; the window may only be destroyed
    // once BOTH have returned (and replay has stopped) — see maybeJoin()/
    // finishShot()/finishNowBlocking().
    QFutureWatcher<pinpoint::SwingExportResult> m_swingSaveWatcher;
    QFutureWatcher<ShotAnalysisResult>          m_analysisWatcher;
    bool    m_swingSaveInFlight = false;
    bool    m_analysisInFlight  = false;
    Outcome m_exportOutcome     = Outcome::Pending;
    Outcome m_analysisOutcome   = Outcome::Pending;
    // Corpus capture: saveRawFrames && skipAnalysisForRawCapture, resolved per shot
    // at window capture. When set, the pipeline exports frames only — no analysis,
    // no segmentation, no on-screen replay — leaving the swing to be re-analysed
    // later from the Shots view.
    bool    m_skipAnalysisCapture = false;
    ShotAnalysisResult m_analysisResult;
    QString m_swingDir;          // cached from the export job for the join
    pinpoint::SwingExportJob m_exportJob;  // cached job — synth header on export fail/skip
    QString m_thumbnailPath;     // from SwingExportResult
    QJsonObject m_exportManifest; // raw manifest from the exporter, for the unified write
    pinpoint::SwingPaths m_swingPaths;   // per-app-run session-folder cache
};
