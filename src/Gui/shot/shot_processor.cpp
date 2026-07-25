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

#include "shot_processor.h"

#include "app_settings.h"
#include "athlete_controller.h"
#include "camera_instance.h"
#include "camera_manager.h"
#include "device_enumerator.h"
#include "event_buffer.h"
#include "imu_instance.h"
#include "imu_manager.h"
#include "session_controller.h"
#include "shot_list_model.h"
#include "../Analysis/club_length_fusion.h"
#include "../Analysis/imu_refusion_check.h"
#include "../Analysis/imu_vision_fuser.h"
#include "../Analysis/phase_segmenter.h"
#include "../Analysis/swing_analysis.h"
#include "../Export/swing_doc.h"
#include "../Core/pp_debug.h"
#include "../Core/pp_os_metrics.h"
#include "pp_version.h"

#include <QSysInfo>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QTime>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cmath>
#include <variant>

namespace {

// Post-trigger capture continuation, per source — the buffer keeps capturing
// for this long after the trigger so the follow-through lands in the ring
// before it freezes. Auto detectors use 1250 ms (segmentation v3: the finish
// is follow-through ~0.67 s + decay-to-still, physically truncated at 500 ms
// — design A.6; impact then sits ~2.75 s into the 4 s ring, leaving ample
// pre-impact room). Manual stays 500 ms: the user presses after the swing and
// impact is back-dated, so the finish is already in the ring.
constexpr int kPostRollManualMs   = 500;
constexpr int kPostRollImuMs      = 1250;
constexpr int kPostRollPoseMs     = 1250;
constexpr int kPostRollBallMs     = 1250;
constexpr int kPostRollAcousticMs = 1250;

int postRollMsFor(ShotController::Source s)
{
    switch (s) {
    case ShotController::Source::Manual:   return kPostRollManualMs;
    case ShotController::Source::Imu:      return kPostRollImuMs;
    case ShotController::Source::Pose:     return kPostRollPoseMs;
    case ShotController::Source::Ball:     return kPostRollBallMs;
    case ShotController::Source::Acoustic: return kPostRollAcousticMs;
    }
    return kPostRollManualMs;
}

// The entire trailing ring — every source's window_duration. Trimmed 5 s → 4 s
// (2026-07-19): the window is anchored at the pause instant, so it only reaches
// back from impact — post-impact room (~1.5 s: hold + back-date + post-roll) is
// unaffected and shrinking only trims pre-impact reach. The analyzer never reads
// further back than impact − 1.75 s (onset clamp bsMaxBeforeImpactUs = 1.6 s +
// 150 ms fill pad, shaft_track_assembly.h), so the floor is set by that clamp,
// not by tempo. 4 s leaves ~2.45 s pre-impact — ~0.7 s of margin over the floor —
// while cutting a fifth off the frozen window: ~190 MB of raw frame copy per
// camera at export and 20 % of the x264 encode. The per-source ring retention
// (SourceDescriptor::window_duration, still 5 s) is deliberately left larger so
// the captured window keeps drain/post-roll headroom inside the ring.
constexpr std::chrono::milliseconds kWindowDuration{4000};

// Multi-estimator club-length fusion result (club_length_fusion.h), nested as
// "lengths" under the "club" detail block — identical shape in both parity
// writers (this live-detail path and swing_doc.cpp's disk-reload path; plan:
// robust club length — starry-shimmying-wind). Always written, even when the
// fuse abstained (nEstimators==0, fusedPx<0): readers treat <0 as absent, which
// is simpler than conditionally omitting the block in only one of the writers.
QVariantMap toLengthsDetail(const pinpoint::analysis::ClubLengthEstimate &l)
{
    return QVariantMap{
        { QStringLiteral("ballPx"),           l.ballPx },
        { QStringLiteral("bandPx"),           l.bandPx },
        { QStringLiteral("headP95Px"),        l.headPx },
        { QStringLiteral("posePx"),           l.posePx },
        { QStringLiteral("priorPx"),          l.priorPx },
        { QStringLiteral("fusedPx"),          l.fusedPx },
        { QStringLiteral("fusedSigmaPx"),     l.fusedSigmaPx },
        { QStringLiteral("fusedConf"),        l.fusedConf },
        { QStringLiteral("fusedInstantPx"),   l.fusedInstantPx },
        { QStringLiteral("fusedInstantConf"), l.fusedInstantConf },
        { QStringLiteral("ladderRung"),       l.ladderRung },
        { QStringLiteral("ladderLenPx"),      l.ladderLenPx },
        { QStringLiteral("nEstimators"),      l.nEstimators },
        { QStringLiteral("priorN"),           l.priorN },
        { QStringLiteral("headMeasN"),        l.headMeasN },
    };
}

// Convert the analyzer's rich SwingAnalysis into QML-friendly data for the shot's
// analysisDetail role (the future scrubbable metric graph reads series + phases).
QVariantMap toAnalysisDetail(const pinpoint::analysis::SwingAnalysis &a)
{
    using namespace pinpoint::analysis;
    QVariantList series;
    for (const MetricSeries &m : a.series) {
        QVariantList ts, vs, samples;
        for (const int64_t t : m.t_us) ts.append(static_cast<qlonglong>(t));
        for (const double v : m.value) vs.append(v);
        for (const PhaseSample &ps : m.phaseSamples)
            samples.append(QVariantMap{ { QStringLiteral("phase"), int(ps.phase) },
                                        { QStringLiteral("t_us"),  static_cast<qlonglong>(ps.t_us) },
                                        { QStringLiteral("value"), ps.value },
                                        { QStringLiteral("band"),  ps.band } });
        series.append(QVariantMap{ { QStringLiteral("key"),   m.key },
                                   { QStringLiteral("label"), m.label },
                                   { QStringLiteral("unit"),  m.unit },
                                   { QStringLiteral("t_us"),  ts },
                                   { QStringLiteral("value"), vs },
                                   { QStringLiteral("phaseSamples"), samples } });
    }
    QVariantList phases;
    for (const PhaseEvent &e : a.phases)
        phases.append(QVariantMap{ { QStringLiteral("phase"),   int(e.phase) },
                                   { QStringLiteral("t_us"),    static_cast<qlonglong>(e.t_us) },
                                   { QStringLiteral("conf"),    e.conf },
                                   { QStringLiteral("segment"), int(e.provenance) } });
    QVariantMap detail{ { QStringLiteral("tier"),    a.tier },
                        { QStringLiteral("overall"), a.score.overall },
                        { QStringLiteral("series"),  series },
                        { QStringLiteral("phases"),  phases } };

    // Resemblance estimand + uncertainty interval (design §B.0a/§B.7) — same sibling-key
    // shape SwingDocReader reloads, so live and reloaded swings expose identical detail.
    if (a.score.kind == ScoreKind::Resemblance) {
        QVariantMap res;
        for (auto it = a.score.resemblance.constBegin(); it != a.score.resemblance.constEnd(); ++it)
            res.insert(it.key(), it.value());
        detail.insert(QStringLiteral("resemblance"), res);
        detail.insert(QStringLiteral("pattern"), a.score.patternLabel);
        detail.insert(QStringLiteral("blended"), a.score.blended);
    }
    if (a.score.interval.valid())
        detail.insert(QStringLiteral("interval"),
                      QVariantMap{ { QStringLiteral("halfWidth"), a.score.interval.halfWidth },
                                   { QStringLiteral("lo"),        a.score.interval.lo },
                                   { QStringLiteral("hi"),        a.score.interval.hi } });

    // Adherence contribution maps — the Verdict donut's hover breakdown. Omitted when
    // empty (a resemblance score has neither), matching serializeScore/SwingDocReader
    // so a live shot and its reloaded twin expose the same keys.
    auto insertBuckets = [&detail](const char *name, const QHash<QString,int> &h) {
        if (h.isEmpty()) return;
        QVariantMap m;
        for (auto it = h.constBegin(); it != h.constEnd(); ++it)
            m.insert(it.key(), it.value());
        detail.insert(QString::fromLatin1(name), m);
    };
    insertBuckets("perRegion", a.score.perRegion);
    insertBuckets("perPhase",  a.score.perPhase);

    // Swing bounds + ladder meta (v3 G2) — same shape the doc reader reloads.
    if (a.segmentation.swingEndUs > a.segmentation.swingStartUs)
        detail.insert(QStringLiteral("segmentation"),
                      QVariantMap{
                          { QStringLiteral("swingStartUs"),
                            static_cast<qlonglong>(a.segmentation.swingStartUs) },
                          { QStringLiteral("swingEndUs"),
                            static_cast<qlonglong>(a.segmentation.swingEndUs) },
                          { QStringLiteral("conf"),    double(a.segmentation.conf) },
                          { QStringLiteral("version"), a.segmentation.version } });

    // ShaftTracker blocks for the replay overlay — IDENTICAL shapes to the
    // swing.json blocks SwingDocReader reloads (swing_doc.cpp), keypoints and
    // club grip/head normalized 0..1 so QML never sees pixel spaces.
    if (!a.pose2d.frames.empty()) {
        QVariantList frames;
        for (const PoseFrame2D &f : a.pose2d.frames) {
            QVariantList kp;
            kp.reserve(kWholeBodyJoints * 3);
            for (int j = 0; j < kWholeBodyJoints; ++j) {
                kp.append(f.kp[size_t(j)].x());
                kp.append(f.kp[size_t(j)].y());
                kp.append(double(f.conf[size_t(j)]));
            }
            frames.append(QVariantMap{
                { QStringLiteral("t_us"), static_cast<qlonglong>(f.t_us) },
                { QStringLiteral("kp"),   kp },
                { QStringLiteral("lead"),  QVariantList{ f.leadHand.x(),  f.leadHand.y() } },
                { QStringLiteral("trail"), QVariantList{ f.trailHand.x(), f.trailHand.y() } },
                { QStringLiteral("handConf"), double(f.handConf) } });
        }
        // keypointCount mirrors the swing.json pose2d block (swing_doc.cpp) so
        // disk and in-memory replay payloads agree; QML consumers index j*3 for
        // j<17 and are unaffected by the wholebody tail.
        QVariantMap pose2d{ { QStringLiteral("camera"), int(a.pose2d.camera) },
                            { QStringLiteral("keypointCount"), kWholeBodyJoints },
                            { QStringLiteral("frames"), frames } };
        // WB1 accuracy-pass provenance — same conditional-presence rule as the
        // swing.json writer (swing_doc.cpp) so disk and in-memory replay agree.
        if (a.pose2d.decode == QLatin1String("dark"))
            pose2d.insert(QStringLiteral("decode"), a.pose2d.decode);
        if (a.pose2d.cropRect) {
            const QRectF &r = *a.pose2d.cropRect;
            pose2d.insert(QStringLiteral("cropRect"),
                          QVariantMap{ { QStringLiteral("x"), r.x() },
                                       { QStringLiteral("y"), r.y() },
                                       { QStringLiteral("w"), r.width() },
                                       { QStringLiteral("h"), r.height() } });
        }
        // Motion-overlay smoothed companion track (pose_smoother.cpp) — same flat
        // kp layout as `frames` (399 doubles: [x,y,c]×133, conf carries the render-
        // alpha contract), plus per-kp honesty tier[133] (int) / sigma[133] (px). No
        // lead/trail/handConf — hands are not smoothed. The QML renderer reads
        // d.pose2d.smoothed[i].kp with this exact layout. Present only when the
        // analyzer ran the smoother (absent otherwise → the UI greys the motion modes).
        if (!a.pose2d.smoothed.empty()) {
            QVariantList smoothed;
            const size_t n = std::min(a.pose2d.smoothed.size(), a.pose2d.smoothedAux.size());
            for (size_t i = 0; i < n; ++i) {
                const PoseFrame2D &f = a.pose2d.smoothed[i];
                const PoseKpAux   &x = a.pose2d.smoothedAux[i];
                QVariantList kp, tier, sigma;
                kp.reserve(kWholeBodyJoints * 3);
                for (int j = 0; j < kWholeBodyJoints; ++j) {
                    kp.append(f.kp[size_t(j)].x());
                    kp.append(f.kp[size_t(j)].y());
                    kp.append(double(f.conf[size_t(j)]));
                    tier.append(int(x.tier[size_t(j)]));
                    sigma.append(double(x.sigma[size_t(j)]));
                }
                smoothed.append(QVariantMap{
                    { QStringLiteral("t_us"),  static_cast<qlonglong>(f.t_us) },
                    { QStringLiteral("kp"),    kp },
                    { QStringLiteral("tier"),  tier },
                    { QStringLiteral("sigma"), sigma } });
            }
            pose2d.insert(QStringLiteral("smoothed"), smoothed);
        }
        detail.insert(QStringLiteral("pose2d"), pose2d);
    }
    if (a.shaft.valid && !a.shaft.samples.empty()
        && a.shaft.frameWidth > 0 && a.shaft.frameHeight > 0) {
        const double iw = 1.0 / a.shaft.frameWidth, ih = 1.0 / a.shaft.frameHeight;
        QVariantList samples;
        for (const ShaftSample2D &s : a.shaft.samples) {
            QVariantMap sm{
                { QStringLiteral("t_us"),  static_cast<qlonglong>(s.t_us) },
                { QStringLiteral("grip"),  QVariantList{ s.gripPx.x() * iw, s.gripPx.y() * ih } },
                { QStringLiteral("head"),  QVariantList{ s.headPx.x() * iw, s.headPx.y() * ih } },
                { QStringLiteral("theta"), s.thetaRad },
                { QStringLiteral("thetaDot"), s.thetaDotRadS },
                { QStringLiteral("lenPx"), s.visibleLenPx },
                { QStringLiteral("conf"),  double(s.conf) },
                // Stage-2 head confidence + posterior σ (Phase B; −1 = head pass
                // off) — the overlay scales the measured-head dot alpha by it.
                { QStringLiteral("headConf"),  double(s.headConf) },
                { QStringLiteral("headSigma"), double(s.headSigmaPx) },
                { QStringLiteral("flags"), int(s.flags) } };
            // Layer A snap registration (shaft_position_first §2A) — lock-step with
            // swing_doc.cpp: written only when measured (≥0), absent ⇒ reader −1.
            if (s.lineConf >= 0.f) sm.insert(QStringLiteral("lineConf"), double(s.lineConf));
            samples.append(sm);
        }
        // R7 predicted series (pure R6 model) for the ghost overlay — same
        // normalized shape as `samples`; σ_β recoverable from conf for the cone.
        QVariantList predicted;
        for (const ShaftSample2D &s : a.shaft.predicted)
            predicted.append(QVariantMap{
                { QStringLiteral("t_us"),  static_cast<qlonglong>(s.t_us) },
                { QStringLiteral("grip"),  QVariantList{ s.gripPx.x() * iw, s.gripPx.y() * ih } },
                { QStringLiteral("head"),  QVariantList{ s.headPx.x() * iw, s.headPx.y() * ih } },
                { QStringLiteral("theta"), s.thetaRad },
                { QStringLiteral("lenPx"), s.visibleLenPx },
                { QStringLiteral("conf"),  double(s.conf) },
                { QStringLiteral("flags"), int(s.flags) } });
        // Layer C synthesized series (shaft_position_first §2 Layer C) — lock-step
        // with swing_doc.cpp; VISUALIZATION-tier interpolation between P-anchors,
        // each flagged ShaftSynthesized (0x100). Same normalized shape as `samples`
        // minus lineConf. Written only when non-empty (synth off ⇒ absent). Consumers
        // EXCLUDE these from metrics/scoring by the flag.
        QVariantList synth;
        for (const ShaftSample2D &s : a.shaft.synth)
            synth.append(QVariantMap{
                { QStringLiteral("t_us"),  static_cast<qlonglong>(s.t_us) },
                { QStringLiteral("grip"),  QVariantList{ s.gripPx.x() * iw, s.gripPx.y() * ih } },
                { QStringLiteral("head"),  QVariantList{ s.headPx.x() * iw, s.headPx.y() * ih } },
                { QStringLiteral("theta"), s.thetaRad },
                { QStringLiteral("thetaDot"), s.thetaDotRadS },
                { QStringLiteral("lenPx"), s.visibleLenPx },
                { QStringLiteral("conf"),  double(s.conf) },
                { QStringLiteral("headConf"),  double(s.headConf) },
                { QStringLiteral("headSigma"), double(s.headSigmaPx) },
                { QStringLiteral("flags"), int(s.flags) } });
        // Coaching P-positions P1–P8 (shaft_position_first §2 Layer B) — lock-step
        // with swing_doc.cpp; grip/head normalized 0..1, t_us absolute like this
        // path's `samples`. Written only when non-empty (extraction off ⇒ absent).
        QVariantList positions;
        for (const ShaftPosition &p : a.shaft.positions)
            positions.append(QVariantMap{
                { QStringLiteral("p"),     p.p },
                { QStringLiteral("t_us"),  static_cast<qlonglong>(p.t_us) },
                { QStringLiteral("grip"),  QVariantList{ p.gripPx.x() * iw, p.gripPx.y() * ih } },
                { QStringLiteral("head"),  QVariantList{ p.headPx.x() * iw, p.headPx.y() * ih } },
                { QStringLiteral("theta"), p.thetaRad },
                { QStringLiteral("lenPx"), p.lenPx },
                { QStringLiteral("conf"),  double(p.conf) },
                { QStringLiteral("sigmaThetaDeg"), double(p.sigmaThetaDeg) },
                { QStringLiteral("sigmaLenPx"),    double(p.sigmaLenPx) },
                { QStringLiteral("stackN"), p.stackN },
                { QStringLiteral("source"), int(p.source) } });
        QVariantMap clubMap{
            { QStringLiteral("camera"),        int(a.shaft.camera) },
            { QStringLiteral("valid"),         a.shaft.valid },
            { QStringLiteral("coverage"),      double(a.shaft.coverage) },
            { QStringLiteral("imuVisionCorr"), double(a.shaft.imuVisionCorr) },
            { QStringLiteral("modelVisionResidualDeg"), double(a.shaft.modelVisionResidualDeg) },
            // v3.4 (design §9.4): measured club length in px (grip-to-ball
            // at address) — mirrors the swing.json club block (swing_doc.cpp)
            // so the live/in-window detail path carries it too, not just the
            // disk-reload path. -1 = unmeasured (no ball anchor).
            { QStringLiteral("measuredClubLenPx"), double(a.shaft.measuredClubLenPx) },
            { QStringLiteral("frameWidth"),    a.shaft.frameWidth },
            { QStringLiteral("frameHeight"),   a.shaft.frameHeight },
            // Multi-estimator length fusion (club_length_fusion.h) — see
            // toLengthsDetail(); mirrors swing_doc.cpp's analysis.club.lengths.
            { QStringLiteral("lengths"),       toLengthsDetail(a.shaft.lengths) },
            { QStringLiteral("samples"),       samples },
            { QStringLiteral("predicted"),     predicted } };
        if (!positions.isEmpty()) clubMap.insert(QStringLiteral("positions"), positions);
        if (!synth.isEmpty())     clubMap.insert(QStringLiteral("synth"), synth);
        detail.insert(QStringLiteral("club"), clubMap);
    }
    // Ball track (v3.4 design §9) for the replay overlay — normalized [0,1]
    // full-frame center + radius, same convention as pose2d/club so QML never
    // sees pixel spaces. found=false samples mark the post-launch gap (the
    // circle vanishes at impact). Same shape as the analysis.ball swing.json block.
    if (!a.ball.frames.empty()) {
        QVariantList samples;
        for (const BallSample2D &s : a.ball.frames)
            samples.append(QVariantMap{
                { QStringLiteral("t_us"),  static_cast<qlonglong>(s.t_us) },
                { QStringLiteral("x"),     s.center.x() },
                { QStringLiteral("y"),     s.center.y() },
                { QStringLiteral("r"),     double(s.radiusNorm) },
                { QStringLiteral("conf"),  double(s.conf) },
                { QStringLiteral("found"), s.found } });
        detail.insert(QStringLiteral("ball"),
                      QVariantMap{
                          { QStringLiteral("camera"),    int(a.ball.camera) },
                          { QStringLiteral("valid"),     true },
                          { QStringLiteral("launchTUs"), static_cast<qlonglong>(a.ball.launchTUs) },
                          { QStringLiteral("samples"),   samples } });
    }
    return detail;
}

// Placement-slot → SegmentRole mapping lives in swing_analysis.h
// (pinpoint::analysis::segmentRoleForSlot) — one source of truth shared with the
// stream device.role export and the data viewer's settings fallback.
using pinpoint::analysis::segmentRoleForSlot;

// Human label for a SessionController::Type, used in session-folder naming.
QString sessionTypeLabel(int sessionType)
{
    switch (sessionType) {
    case 0:  return QStringLiteral("Swing");
    case 1:  return QStringLiteral("Wrist");
    case 2:  return QStringLiteral("GRF");
    case 3:  return QStringLiteral("Coach");
    default: return QString();
    }
}

} // namespace

ShotProcessor::ShotProcessor(pinpoint::EventBuffer *buffer,
                             CameraManager         *cameraManager,
                             ImuManager            *imuManager,
                             AppSettings           *appSettings,
                             AthleteController     *athleteController,
                             SessionController     *sessionController,
                             ShotListModel         *shotModel,
                             QObject               *parent)
    : QObject(parent)
    , m_buffer(buffer)
    , m_cameraManager(cameraManager)
    , m_imuManager(imuManager)
    , m_appSettings(appSettings)
    , m_athlete(athleteController)
    , m_session(sessionController)
    , m_shotModel(shotModel)
{
    m_postRollTimer.setSingleShot(true);
    connect(&m_postRollTimer, &QTimer::timeout, this, &ShotProcessor::onPostRollExpired);

    // Worker completion is delivered on this (UI) thread, strictly after the
    // worker lambda has returned — the join in maybeJoin()/finishShot() relies
    // on that ordering to destroy the SwingWindow safely.
    connect(&m_swingSaveWatcher, &QFutureWatcher<pinpoint::SwingExportResult>::finished,
            this, &ShotProcessor::onSwingSaveFinished);
    connect(&m_analysisWatcher, &QFutureWatcher<ShotAnalysisResult>::finished,
            this, &ShotProcessor::onAnalysisFinished);
    connect(&m_segmentationWatcher,
            &QFutureWatcher<pinpoint::analysis::Segmentation>::finished,
            this, &ShotProcessor::onSegmentationFinished);
}

ShotProcessor::~ShotProcessor()
{
    // main.cpp declares the processor after CameraManager, so this runs first
    // in the stack unwind: join the workers and destroy the window before
    // ~CameraManager deregisters sources and ~EventBuffer frees ring memory.
    finishNowBlocking();
    if (m_cameraManager)
        m_cameraManager->setShotProcessor(nullptr);
    if (m_imuManager)
        m_imuManager->setShotProcessor(nullptr);
}

QString ShotProcessor::stateName() const
{
    switch (m_state) {
    case State::Idle:       return QStringLiteral("idle");
    case State::PostRoll:   return QStringLiteral("postroll");
    case State::Processing: return QStringLiteral("processing");
    case State::Replaying:  return QStringLiteral("replaying");
    }
    return QStringLiteral("unknown");
}

void ShotProcessor::setState(State s)
{
    if (m_state == s)
        return;
    const bool busyBefore   = busy();
    const bool replayBefore = isReplaying();
    m_state = s;
    emit stateChanged();
    if (busyBefore != busy())
        emit busyChanged();
    if (replayBefore != isReplaying())
        emit isReplayingChanged();
}

void ShotProcessor::setAnalysisProgress(double p)
{
    // Monotonic except for the explicit per-shot reset to 0: queued worker
    // updates can land after the completion 1.0 and must not drag the bar back.
    if (p == m_analysisProgress || (p < m_analysisProgress && p != 0.0))
        return;
    m_analysisProgress = p;
    emit analysisProgressChanged();
}

// ---------------------------------------------------------------------------
// Trigger → post-roll
// ---------------------------------------------------------------------------

void ShotProcessor::onShotDetected(ShotController::Source source,
                                   qint64 timestampUs, int sessionType)
{
    // ShotController's busy gate refuses triggers while we are non-Idle; this
    // guard is the belt-and-braces backstop.
    if (m_state != State::Idle) {
        ppWarn() << "[ShotProcessor] shot ignored — already processing (state"
                 << stateName() << ")";
        return;
    }
    if (!m_buffer || !m_buffer->isCapturing()) {
        ppWarn() << "[ShotProcessor] shot ignored — buffer not capturing";
        return;
    }

    m_shotSource     = source;
    m_impactUs       = timestampUs;
    m_sessionType    = sessionType;
    m_timestampLabel = QTime::currentTime().toString(QStringLiteral("hh:mm:ss"));

    setAnalysisProgress(0.0);   // the ANALYSING bar starts empty for each shot
    setState(State::PostRoll);
    m_postRollTimer.start(postRollMsFor(source));
}

void ShotProcessor::onPostRollExpired()
{
    if (m_state != State::PostRoll)
        return;   // finishNowBlocking() raced the timer
    captureWindowAndLaunch();
}

// ---------------------------------------------------------------------------
// Window capture → concurrent analysis + export
// ---------------------------------------------------------------------------

void ShotProcessor::captureWindowAndLaunch()
{
    // The user may have pressed Stop during the post-roll: the rings froze at
    // the pause instant, truncating the follow-through there — still a valid
    // shot, so proceed from Paused. Only buffer teardown aborts.
    m_cameraManager->pauseBuffer();
    if (!m_buffer || m_buffer->state() != pinpoint::BufferState::Paused) {
        ppWarn() << "[ShotProcessor] shot aborted — buffer unavailable at post-roll expiry";
        abortToIdle();
        return;
    }

    Q_ASSERT(!m_swingWindow);   // state machine forbids a second shot while busy
    m_swingWindow.emplace(m_buffer->captureSwingWindow(kWindowDuration));

    // One replay track per live camera with captured frames.
    m_replayTracks.clear();
    const std::vector<CameraInstance *> instances = m_cameraManager->liveCameraInstances();
    for (CameraInstance *ctrl : instances) {
        const pinpoint::SourceId sid = ctrl->sourceId();
        if (sid == pinpoint::kInvalidSourceId) continue;
        auto entries = m_swingWindow->entriesFor(sid);
        if (entries.empty()) continue;
        ReplayTrack track;
        track.ctrl     = ctrl;
        track.sourceId = sid;
        track.entries  = std::move(entries);
        // Freeze the ball-detector accumulator NOW (design §9): both job builders
        // run 12–37 s later (from onAnalysisFinished), by when the live deque has
        // scrolled to post-shot junk and a phantom re-launch may have overwritten
        // the launch. At this instant the 6 s accumulator still fully covers the
        // 4 s window. Absolute buffer-clock tUs kept (the analyzer consumes the
        // window's native domain; the exporter rebases).
        const auto &ballAccum = ctrl->ballSamples();
        track.ball.samples.reserve(ballAccum.size());
        for (const auto &s : ballAccum)
            track.ball.samples.push_back({s.tUs, s.found, s.x, s.y, s.r, s.conf});
        // CameraInstance's stored launch is never reset between shots, so when
        // the detector misses THIS shot's launch the stored one belongs to a
        // previous swing (observed: Wrist_02 sw4/5 exported sw3's launch).
        // Accept it only when it falls inside this window.
        qint64 lTUs = -1; double lx = 0.0, ly = 0.0;
        if (ctrl->ballLaunchInfo(lTUs, lx, ly)
            && lTUs >= m_swingWindow->startTimestampUs()
            && lTUs <= m_swingWindow->endTimestampUs()) {
            track.ball.hasLaunch = true;
            track.ball.launchTUs = lTUs;
            track.ball.launchX   = lx;
            track.ball.launchY   = ly;
        }
        m_replayTracks.push_back(std::move(track));
    }

    m_exportOutcome   = Outcome::Pending;
    m_analysisOutcome = Outcome::Pending;
    m_analysisResult  = {};
    m_segmentation    = {};
    m_swingDir.clear();
    m_thumbnailPath.clear();
    // Per-shot club-length prior stash (Phase 3) — re-resolved from scratch in
    // buildAnalysisJob() below; stays empty/cold when this shot has no
    // athlete/club/fixed-in-place face-on camera, which onAnalysisFinished()
    // reads as "nothing to persist".
    m_lengthPriorKey.clear();
    m_lengthPriorState = {};
    setState(State::Processing);

    ppInfo() << "[ShotProcessor] window captured —"
             << static_cast<qint64>(m_swingWindow->entries().size()) << "entries,"
             << m_replayTracks.size() << "camera track(s)";

    // Corpus capture: when saving raw frames with "skip analysis" on, export the
    // frames only — bypass segmentation + analysis and suppress replay (see
    // maybeJoin) — so each shot captures instantly and is re-analysed later. The
    // raw-only swing.json (analysis-skipped) still carries capture.impactUs, so
    // offline re-analysis has its impact reference.
    {
        AppSettings  fallback;
        AppSettings *s = m_appSettings ? m_appSettings : &fallback;
        m_skipAnalysisCapture = s->saveRawFrames() && s->skipAnalysisForRawCapture();
    }
    if (m_skipAnalysisCapture) {
        ppInfo() << "[ShotProcessor] skip-analysis corpus capture — export only";
        m_analysisOutcome = Outcome::Skipped;   // maybeJoin → raw-only swing.json
        startSwingSave();                        // onSwingSaveFinished() joins
        return;
    }

    // Segmentation pre-stage (v3 G2): a milliseconds-cheap fuse + inertial
    // ladder over the frozen window, gating both heavy workers — its swing
    // bounds trim the export encode span and the replay. The job is resolved
    // on the UI thread NOW (value types only); failure or no-IMU yields a
    // conf-0 result and everything below degrades to full-window behaviour.
    m_analysisJob = buildAnalysisJob();
    const pinpoint::SwingWindow *win = &*m_swingWindow;
    m_segmentationInFlight = true;
    m_segmentationWatcher.setFuture(QtConcurrent::run(
        [bindings = m_analysisJob.imuBindings, impactUs = m_impactUs, win] {
            try {
                const pinpoint::analysis::FusedStreams streams =
                    pinpoint::analysis::ImuVisionFuser::fuse(*win, bindings);
                return pinpoint::analysis::PhaseSegmenter::segment(streams, impactUs);
            } catch (...) {
                return pinpoint::analysis::Segmentation{};   // conf 0 → full window
            }
        }));
}

void ShotProcessor::onSegmentationFinished()
{
    if (!m_segmentationInFlight)
        return;   // already joined blockingly in finishNowBlocking()
    m_segmentationInFlight = false;
    if (m_state != State::Processing)
        return;   // aborted while the pre-stage ran
    m_segmentation = m_segmentationWatcher.result();

    // Sequence the two heavy workers rather than overlapping them. The ViTPose
    // pose pass dominates wall time and runs far faster with the cores to itself
    // (multi-threaded intra-op); the export's x264 encode threads otherwise
    // starved the pose pass, inflating per-frame inference roughly 5×. The
    // export is launched once analysis completes (onAnalysisFinished). Analysis
    // always launches a worker, so there is no synchronous-skip path to cover
    // with maybeJoin() here. Both workers read the same frozen window — const,
    // zero-copy reads over stable memory (producers stopped while Paused).
    startAnalysis();
}

ShotAnalysisJob ShotProcessor::buildAnalysisJob()
{
    ShotAnalysisJob job;
    job.sessionType = m_sessionType;
    job.shotSource  = static_cast<int>(m_shotSource);
    job.impactUs    = m_impactUs;
    // Offline pose-model tier (High -> ViTPose++-L when downloaded, else ViTPose-B).
    job.motionCaptureQuality = m_appSettings ? m_appSettings->motionCaptureQuality() : QString();
    // Produce WristAssessmentEngine findings on the live Wrist pipeline (design §B.0:
    // faults are the AI-coach feedback layer, decoupled from the headline resemblance
    // score — D-3). Was offline-only (SwingLab); now always-on for Wrist so swing.json
    // carries the coach feed. Other session types leave it off (no producer yet).
    job.runAssessment = (m_sessionType == 1);

    // Face-on first so analyzers can prefer it without re-sorting; the count
    // makes "face-on first" verifiable from the worker (0 = none captured).
    for (const ReplayTrack &track : m_replayTracks) {
        if (track.ctrl->perspective() == CameraInstance::FaceOn) {
            job.cameraSources.insert(job.cameraSources.begin(), track.sourceId);
            ++job.faceOnCameraCount;
        } else {
            job.cameraSources.push_back(track.sourceId);
        }
    }

    // IMU and marker sources discovered from the window's own formats.
    QSet<pinpoint::SourceId> seen;
    for (const pinpoint::IndexEntry &e : m_swingWindow->entries()) {
        if (seen.contains(e.source_id)) continue;
        seen.insert(e.source_id);
        const pinpoint::FormatDescriptor &fd = m_swingWindow->formatOf(e.source_id);
        if (fd.device == pinpoint::DeviceKind::Marker_App)
            job.markerSourceId = e.source_id;
        else if (std::holds_alternative<pinpoint::ImuFormat>(fd.format))
            job.imuSources.push_back(e.source_id);
    }

    // v3.4 (design §9): face-on ball track for the live analyzer, resolved
    // from whichever camera's accumulator has data — prefer face-on (the
    // hitting-area ROI only ever runs there in practice), else any camera
    // with ball detection enabled. Empty ⇒ ShaftTracker falls back to
    // BallRunner's offline replay (correct even live: e.g. detection was
    // enabled mid-swing and the accumulator is still short).
    // Read from the per-track snapshot frozen at window capture (ReplayTrack::
    // ball) — NOT live controller state, which by now records post-shot
    // accumulation, not the swing. Time base: the analyzer consumes everything
    // in the WINDOW'S NATIVE domain — live SwingWindow entries and job.impactUs
    // (line above) are absolute buffer-clock, so the ball samples must stay
    // absolute too (ShaftTracker matches them against entry timestamps; the
    // offline loader passes window-relative ball + window-relative entries, the
    // same contract). swing_doc's rel() normalizes on export either way. Do NOT
    // rebase here — a relative track against absolute entries matches nothing.
    for (int pass = 0; pass < 2 && job.ballTrack.frames.empty(); ++pass) {
        for (const ReplayTrack &track : m_replayTracks) {
            if (pass == 0 && track.ctrl->perspective() != CameraInstance::FaceOn)
                continue;
            const auto &samples = track.ball.samples;
            if (samples.empty()) continue;
            pinpoint::analysis::BallTrack2D bt;
            bt.camera = track.sourceId;
            bt.frames.reserve(samples.size());
            for (const auto &s : samples)
                bt.frames.push_back({s.tUs, s.found, QPointF(s.x, s.y), s.r, s.conf});
            if (track.ball.hasLaunch) {
                bt.launchTUs    = track.ball.launchTUs;
                bt.launchCenter = QPointF(track.ball.launchX, track.ball.launchY);
            }
            job.ballTrack = std::move(bt);
            break;
        }
    }

    // Hitting-area ROI for the offline BallRunner fallback (empty accumulator /
    // detection enabled mid-swing) — prefer the face-on camera so ball search
    // uses the same box the live detector did, skipping feet/shoe distractors.
    for (const ReplayTrack &track : m_replayTracks) {
        if (track.ctrl->perspective() == CameraInstance::FaceOn && !track.ctrl->roi().isEmpty()) {
            job.ballSearchRoi = track.ctrl->roi();
            break;
        }
    }

    // Athlete handedness (lead-arm sign) and IMU -> segment bindings, resolved
    // here on the UI thread — the worker can read neither the athlete controller
    // nor the live ImuInstance calibration (alignA/mountM are session-lifetime).
    const QString hand = m_athlete ? m_athlete->currentHandedness() : QString();
    job.handedness = hand.compare(QLatin1String("Left"),  Qt::CaseInsensitive) == 0 ? 2
                   : hand.compare(QLatin1String("Right"), Qt::CaseInsensitive) == 0 ? 1 : 0;

    // Club length (m) sizes the shaft-tracker search radius. Resolve the session's
    // active club against the athlete's bag; leave the ShotAnalysisJob default
    // (driver ≈ 1.12 m) when unset or the club record has no recorded length.
    if (m_athlete && m_session) {
        const QString club = m_session->activeClub();
        if (!club.isEmpty()) {
            const QVariantMap rec = m_athlete->clubsFor(m_athlete->currentUuid())
                                        .value(club).toMap();
            const int lengthMm = rec.value(QStringLiteral("lengthMm")).toInt();
            if (lengthMm > 0)
                job.clubLengthM = lengthMm / 1000.0;
            // Retro-band geometry for the v3 E1 band matcher. Empty (untaped
            // club) ⇒ the shaft tracker runs E2 (ray) evidence only, no band tier.
            const QVariantList bands = rec.value(QStringLiteral("bandCentersMm")).toList();
            for (const QVariant &bv : bands) job.bandCentersMm.push_back(bv.toDouble());
            job.shaftType = rec.value(QStringLiteral("shaftType")).toString();
            const double hoselMm = rec.value(QStringLiteral("hoselFromButtMm")).toDouble();
            if (hoselMm > 0)
                job.hoselFromButtMm = hoselMm;

            // Persistent club-length prior (club_length_fusion.h / plan: robust
            // club length — starry-shimmying-wind). Keyed athleteUuid|clubName|
            // cameraKey (a px prior is meaningless if the camera moves) and only
            // trusted while that camera is marked fixed-in-place. Resolve the
            // face-on camera's cameraKey from the already-frozen replay tracks
            // (m_replayTracks is populated earlier in captureWindowAndLaunch).
            job.clubName = club;
            CameraInstance *faceOnCtrl = nullptr;
            QString faceOnKey;
            for (const ReplayTrack &track : m_replayTracks) {
                if (track.ctrl && track.ctrl->perspective() == CameraInstance::FaceOn) {
                    faceOnCtrl = track.ctrl;
                    faceOnKey  = track.ctrl->cameraKey();
                    break;
                }
            }
            if (!faceOnKey.isEmpty() && m_appSettings
                && m_appSettings->cameraFixedInPlace().value(faceOnKey).toBool()) {
                // Stashed even on a cold/absent entry — onAnalysisFinished() still
                // needs the key to seed a brand-new prior on the first confident fuse.
                m_lengthPriorKey = m_athlete->currentUuid() + QLatin1Char('|') + club
                                  + QLatin1Char('|') + faceOnKey;
                const QVariantMap prior = m_appSettings->clubLenPrior().value(m_lengthPriorKey).toMap();
                if (!prior.isEmpty()) {
                    const int entryW = prior.value(QStringLiteral("frameW"), 0).toInt();
                    const int entryH = prior.value(QStringLiteral("frameH"), 0).toInt();
                    // Camera-move self-heal: an entry with recorded dims must match
                    // the live camera's resolved frame size, or a silently-moved
                    // camera would keep feeding a stale px prior. An entry that
                    // predates this field (no dims recorded) is trusted as-is —
                    // there is nothing to compare against.
                    const bool dimsOk = (entryW <= 0 || entryH <= 0)
                        || (faceOnCtrl && entryW == faceOnCtrl->frameWidth()
                                       && entryH == faceOnCtrl->frameHeight());
                    if (dimsOk) {
                        m_lengthPriorState.emaPx       = prior.value(QStringLiteral("emaPx"), -1.0).toDouble();
                        m_lengthPriorState.varPx       = prior.value(QStringLiteral("varPx"), 0.0).toDouble();
                        m_lengthPriorState.n           = prior.value(QStringLiteral("n"), 0).toInt();
                        m_lengthPriorState.disagreeRun = prior.value(QStringLiteral("disagreeRun"), 0).toInt();
                        if (m_lengthPriorState.n > 0 && m_lengthPriorState.emaPx > 0.0) {
                            job.priorClubLenPx    = m_lengthPriorState.emaPx;
                            job.priorClubLenVarPx = m_lengthPriorState.varPx;
                            job.priorClubLenN     = m_lengthPriorState.n;
                        }
                    }
                }
            }
        }
    }

    if (m_imuManager) {
        const QVariantMap placement = m_appSettings ? m_appSettings->imuPlacement() : QVariantMap{};
        const QVariantList insts = m_imuManager->instances();
        for (const QVariant &v : insts) {
            auto *imu = qobject_cast<ImuInstance *>(v.value<QObject *>());
            if (!imu) continue;
            const pinpoint::SourceId sid = imu->sourceId();
            if (std::find(job.imuSources.begin(), job.imuSources.end(), sid) == job.imuSources.end())
                continue;   // this IMU is not a source in the captured window
            pinpoint::analysis::ImuSegmentBinding b;
            b.source = sid;
            b.role   = segmentRoleForSlot(m_sessionType, placement.value(imu->deviceId()).toString());
            b.alignA = imu->alignA();
            b.mountM = imu->mountM();
            // Calibration status snapshot — persisted into swing.json's
            // analysis.bindings so SwingLab can filter by provenance.
            b.anatCalibrated       = imu->anatCalibrated();
            b.calibrated           = imu->fullyCalibrated();
            b.mountDeviationDeg    = imu->mountDeviationDeg();
            b.mountGravityErrorDeg = imu->mountGravityErrorDeg();
            if (imu->calibratedAtUtc().isValid()) {
                b.calibratedAtUtc = imu->calibratedAtUtc().toString(Qt::ISODateWithMs);
                b.calibAgeSec     = imu->calibratedAtUtc()
                                        .msecsTo(QDateTime::currentDateTimeUtc()) / 1000.0;
            }
            job.imuBindings.push_back(b);
        }
    }

    // Worker → UI progress marshalling: queued invoke with `this` as context
    // (auto-cancelled if the processor dies first), throttled to whole-percent
    // steps so per-frame reporting stays a handful of events per second.
    auto lastPct = std::make_shared<int>(-1);
    job.progress = [this, lastPct](float p) {
        const int pct = static_cast<int>(p * 100.0f);
        if (pct <= *lastPct)
            return;            // single worker thread — no synchronisation needed
        *lastPct = pct;
        QMetaObject::invokeMethod(this, [this, p] { setAnalysisProgress(p); },
                                  Qt::QueuedConnection);
    };
    return job;
}

void ShotProcessor::startAnalysis()
{
    ShotAnalysisJob job = m_analysisJob;   // resolved in captureWindowAndLaunch

    const pinpoint::SwingWindow *win = &*m_swingWindow;   // stable optional storage
    m_analysisInFlight = true;
    m_analysisWatcher.setFuture(QtConcurrent::run(
        [job = std::move(job), win] {
            // Name this pooled thread so it shows in the profiler's per-thread
            // CPU table for the duration of the analysis (RAII across all the
            // return paths below).
            pinpoint::osmetrics::ThreadScope _tscope("Analysis.Worker");
            // Exception barrier: anything escaping the worker (e.g. a
            // cv::Exception on malformed frame geometry) would be rethrown by
            // QtConcurrent on the GUI thread at result()/waitForFinished()
            // with no handler — std::terminate. Degrade to a failed analysis
            // instead; the join still adds the shot and resumes the buffer.
            try {
                auto analyzer = makeShotAnalyzer(job.sessionType);
                return analyzer->analyze(*win, job);
            } catch (const std::exception &e) {
                ShotAnalysisResult r;
                r.error = QString::fromUtf8(e.what());
                return r;
            } catch (...) {
                ShotAnalysisResult r;
                r.error = QStringLiteral("unknown exception in shot analysis");
                return r;
            }
        }));
}

void ShotProcessor::startSwingSave()
{
    pinpoint::SwingExportJob job = buildSwingExportJob();
    // Cache the job + dir up front: even when there is nothing to encode (no
    // cameras, or an encode failure), an analysis-only swing.json is written
    // from these at the join so the shot still survives a restart.
    m_swingDir  = job.swingDir;
    m_exportJob = job;

    if (job.swingDir.isEmpty()) {
        ppWarn() << "[SwingExport] could not allocate a swing directory — not saving";
        m_exportOutcome = Outcome::Skipped;
        emit swingSaveFailed(tr("could not create the swing folder — check the athlete "
                                "library path in Settings"));
        return;
    }
    if (job.cameras.empty()) {
        ppWarn() << "[SwingExport] no exportable cameras — analysis-only swing";
        m_exportOutcome = Outcome::Skipped;
        return;
    }

    // The optional's storage is stable; the window is destroyed only in
    // finishShot()/finishNowBlocking(), strictly after the worker has returned.
    const pinpoint::SwingWindow *win = &*m_swingWindow;
    m_swingSaveInFlight = true;
    ppInfo() << "[SwingExport] saving swing to" << job.swingDir;
    m_swingSaveWatcher.setFuture(QtConcurrent::run(
        [job = std::move(job), win] {
            // Same exception barrier as the analysis worker above.
            try {
                return pinpoint::SwingExporter::run(*win, job);
            } catch (const std::exception &e) {
                pinpoint::SwingExportResult r;
                r.swingDir = job.swingDir;
                r.error = QString::fromUtf8(e.what());
                return r;
            } catch (...) {
                pinpoint::SwingExportResult r;
                r.swingDir = job.swingDir;
                r.error = QStringLiteral("unknown exception in swing export");
                return r;
            }
        }));
}

pinpoint::SwingExportJob ShotProcessor::buildSwingExportJob()
{
    AppSettings  fallback;
    AppSettings *s = m_appSettings ? m_appSettings : &fallback;

    pinpoint::SwingExportJob job;

    // CRF from videoQuality (storage/videoQuality).
    const QString quality = s->videoQuality();
    job.crf = quality == QLatin1String("low")      ? 28
            : quality == QLatin1String("high")     ? 18
            : quality == QLatin1String("lossless") ? 0
                                                   : 23;   // "medium"
    job.codec   = s->videoCodec();
    job.saveImu = s->saveImuStreams();
    job.resolutionMode = s->videoResolutionMode();
    job.saveRaw        = s->saveRawFrames();
    job.imuFormat      = s->imuDataFormat();
    job.savePose       = s->savePoseKeypoints();
    // job.poseStreams intentionally left empty: pose production (analyzer / pose
    // buffering) is a separate scope. The exporter serialises whatever is here,
    // so this is forward-compatible — populate it upstream once a producer lands.

    // Container extension drives the FFmpeg muxer (avformat guesses from the
    // output path). mp4/mov/mkv all carry H.264/H.265; fall back to mp4 for
    // anything else so a stale setting can never break the export.
    QString container = s->videoContainer().toLower();
    if (container != QLatin1String("mp4") && container != QLatin1String("mov")
        && container != QLatin1String("mkv"))
        container = QStringLiteral("mp4");

    if (m_athlete) {
        job.athleteName = m_athlete->currentName();
        job.athleteUuid = m_athlete->currentUuid();
        job.handedness  = m_athlete->currentHandedness();
    }

    // Wallclock anchor: right now, wallclock ~= monotonic endTimestampUs().
    job.wallclockAnchorUtc = QDateTime::currentDateTimeUtc();

    // Session context + host provenance for the manifest's "capture" block.
    job.sessionType = m_sessionType;
    job.shotSource  = static_cast<int>(m_shotSource);
    job.swingDetectionSensitivity = s->swingDetectionSensitivity();
    job.motionCaptureQuality      = s->motionCaptureQuality();
    // Club geometry (shaft-tracker E1 band matcher) — persisted into capture.club
    // so re-analysis recovers the club that was used (mirrors buildAnalysisJob).
    if (m_athlete && m_session) {
        const QString club = m_session->activeClub();
        if (!club.isEmpty()) {
            const QVariantMap rec = m_athlete->clubsFor(m_athlete->currentUuid()).value(club).toMap();
            const int lengthMm = rec.value(QStringLiteral("lengthMm")).toInt();
            if (lengthMm > 0) job.clubLengthM = lengthMm / 1000.0;
            job.shaftType = rec.value(QStringLiteral("shaftType")).toString();
            const QVariantList bands = rec.value(QStringLiteral("bandCentersMm")).toList();
            for (const QVariant &bv : bands) job.bandCentersMm.push_back(bv.toDouble());
            const double hoselMm = rec.value(QStringLiteral("hoselFromButtMm")).toDouble();
            if (hoselMm > 0) job.hoselFromButtMm = hoselMm;
        }
    }
    // Club-length prior (club_length_fusion.h): reuse the exact values already
    // resolved into m_analysisJob for THIS shot (buildAnalysisJob, called earlier
    // in captureWindowAndLaunch) — the prior the live analysis fuse actually used,
    // so re-analysis can reproduce it byte-for-byte.
    job.clubName          = m_analysisJob.clubName;
    job.priorClubLenPx    = m_analysisJob.priorClubLenPx;
    job.priorClubLenVarPx = m_analysisJob.priorClubLenVarPx;
    job.priorClubLenN     = m_analysisJob.priorClubLenN;
    job.imuBleLatencyUs      = ImuInstance::kImuBleLatencyUs;
    job.audioDeviceLatencyUs = s->audioDeviceLatencyUs();
    job.host.appVersion = QStringLiteral(PP_APP_VERSION);
    job.host.gitSha     = QStringLiteral(PP_GIT_SHA);
    job.host.hostname   = QSysInfo::machineHostName();
    job.host.platform   = QSysInfo::prettyProductName();
    for (const ReplayTrack &track : m_replayTracks) {
        if (!track.ctrl->poseBackendLabel().isEmpty()) {
            job.host.poseBackend = track.ctrl->poseBackendLabel();
            break;
        }
    }

    // Cameras: every replay track, with its alias resolved and sanitised.
    // Filename = alias (live-updated on the instance), falling back to the
    // device description, then serial.
    QSet<QString> usedNames;
    for (const ReplayTrack &track : m_replayTracks) {
        QString alias = track.ctrl->deviceAlias().trimmed();
        if (alias.isEmpty()) alias = track.ctrl->deviceDescription();
        if (alias.isEmpty()) alias = QStringLiteral("camera-%1")
                                         .arg(track.ctrl->deviceSerialNumber());

        QString base = pinpoint::SwingPaths::sanitise(alias);
        QString name = base;
        for (int n = 2; usedNames.contains(name); ++n)
            name = base + QStringLiteral("-%1").arg(n);
        usedNames.insert(name);

        pinpoint::SwingExportCamera cam;
        cam.sourceId = track.sourceId;
        cam.alias    = name;
        cam.fileName = name + QLatin1Char('.') + container;
        cam.perspective  = track.ctrl->perspective();
        cam.mirrored     = track.ctrl->isMirrored();
        cam.fixedInPlace = s->cameraFixedInPlace()
                               .value(track.ctrl->cameraKey()).toBool();
        // The v2 temporal detector carries no calibration profile, so the
        // CamRecord ball-calibration fields keep their defaults (uncalibrated).
        // Recording the v2 auto-detected ball position (locked centre + satFrac)
        // into swing.json is the additive "Provenance v2" follow-up.
        cam.ballSearchRoi = track.ctrl->roi();   // hitting area — re-analysis ball search box
        // Learned empty-mat baseline snapshot — cv-free cache, no HAVE_OPENCV
        // guard needed at this call site. Plain copies on the UI thread; blob
        // stays empty (valid() false) when nothing has been learned live yet
        // or a relearn/ROI change invalidated the cache mid-reseed.
        const auto &baseline = track.ctrl->ballBaseline();
        if (baseline.valid()) {
            cam.ballBaselineBlob   = baseline.blob;
            cam.ballBaselineW      = baseline.w;
            cam.ballBaselineH      = baseline.h;
            cam.ballBaselineRoi    = baseline.roi;
            cam.ballBaselineRHat   = baseline.rHat;
            cam.ballBaselineFps    = baseline.fps;
            cam.ballBaselineNoise0 = baseline.noise0;
        }
        job.cameras.push_back(std::move(cam));

        // v3.4 (design §9.7): this camera's ball stream, window-relative —
        // additive, empty when ball detection never ran on this camera. Read
        // from the snapshot frozen at window capture (ReplayTrack::ball), NOT
        // live controller state: buildSwingExportJob runs 12–37 s after impact,
        // by when the live accumulator had scrolled to post-shot junk and any
        // re-launch had overwritten the launch — the archived stream must cover
        // the swing, not the post-shot accumulation.
        const auto &ballSamples = track.ball.samples;
        if (!ballSamples.empty() && m_swingWindow) {
            const int64_t t0 = m_swingWindow->startTimestampUs();
            pinpoint::SwingBallStream bs;
            bs.alias  = name;
            bs.serial = track.ctrl->deviceSerialNumber();
            bs.tUs.reserve(ballSamples.size());
            bs.data.reserve(ballSamples.size() * 5);
            for (const auto &s : ballSamples) {
                bs.tUs.push_back(s.tUs - t0);
                bs.data.push_back(s.found ? 1.f : 0.f);
                bs.data.push_back(s.x);
                bs.data.push_back(s.y);
                bs.data.push_back(s.r);
                bs.data.push_back(s.conf);
            }
            if (track.ball.hasLaunch) {
                bs.launchTUs = track.ball.launchTUs - t0;
                bs.launchX   = float(track.ball.launchX);
                bs.launchY   = float(track.ball.launchY);
            }
            job.ballStreams.push_back(std::move(bs));
        }
    }

    // Impact thumbnail from the face-on camera, else the exporter falls back
    // to the first exported stream.
    job.thumbnailTimestampUs = m_impactUs;
    // Window-relative impact for swing.json's capture.impactUs — the re-analysis
    // impact reference (survives analysis-skipped corpus captures).
    job.impactUs = (m_impactUs >= 0 && m_swingWindow)
                       ? m_impactUs - m_swingWindow->startTimestampUs()
                       : -1;
    for (const ReplayTrack &track : m_replayTracks) {
        if (track.ctrl->perspective() == CameraInstance::FaceOn) {
            job.thumbnailSourceId = track.sourceId;
            break;
        }
    }

    // IMU aliases keyed by the same identifier the sources registered with
    // (serial when present, else device id — mirrors ImuInstance).
    const QVariantMap imuAliases   = s->imuAlias();
    const QVariantMap imuPlacement = s->imuPlacement();
    const QVariantMap imuFusion    = s->imuFusionMode();
    const QVariantMap imuRates     = s->imuOutputRateHz();
    const QList<Device> imus = DeviceEnumerator::instance()->devices(DeviceType::Imu);
    for (const Device &dev : imus) {
        const QString serial = dev.imuCapabilities.serialNumber.isEmpty()
                                   ? dev.id
                                   : dev.imuCapabilities.serialNumber;
        const QString imuKey = dev.description + QStringLiteral("|") + dev.id;
        const QString alias  = imuAliases.value(imuKey).toString().trimmed();
        if (!alias.isEmpty())
            job.imuAliasBySerial.insert(serial, alias);

        // Per-device config for the stream "device" object. The live
        // instance's rate is authoritative; settings are the fallback when
        // the device disconnected between capture and export.
        pinpoint::SwingImuDeviceInfo info;
        info.outputRateHz      = imuRates.value(dev.id, 200).toInt();
        info.fusionMode        = imuFusion.value(dev.id, s->imuDefaultFusionMode()).toString();
        info.orientationFilter = s->imuOrientationFilter();
        info.placementSlot     = imuPlacement.value(dev.id).toString();
        // Resolve the anatomical body role from slot+sessionType (same canonical
        // mapping as analysis.bindings) so it is baked into the stream itself —
        // needed by the data viewer, SwingLab and future post-hoc analysis.
        const pinpoint::analysis::SegmentRole role =
            segmentRoleForSlot(m_sessionType, info.placementSlot);
        info.role     = int(role);
        info.roleName = pinpoint::analysis::segmentRoleName(role);
        if (m_imuManager) {
            const QVariantList insts = m_imuManager->instances();
            for (const QVariant &v : insts) {
                auto *imu = qobject_cast<ImuInstance *>(v.value<QObject *>());
                if (imu && imu->deviceId() == dev.id) {
                    info.outputRateHz = imu->outputRateHz();
                    // A/M calibration snapshot baked into the stream — lets an
                    // analysis-skipped corpus swing be re-analysed (no
                    // analysis.bindings). Mirrors buildAnalysisJob's resolution.
                    info.hasCalibration = true;
                    info.alignA         = imu->alignA();
                    info.mountM         = imu->mountM();
                    info.anatCalibrated = imu->anatCalibrated();
                    info.calibrated     = imu->fullyCalibrated();
                    info.mountDeviationDeg    = imu->mountDeviationDeg();
                    info.mountGravityErrorDeg = imu->mountGravityErrorDeg();
                    if (imu->calibratedAtUtc().isValid()) {
                        info.calibratedAtUtc = imu->calibratedAtUtc().toString(Qt::ISODateWithMs);
                        info.calibAgeSec     = imu->calibratedAtUtc()
                                                   .msecsTo(QDateTime::currentDateTimeUtc()) / 1000.0;
                    }
                    break;
                }
            }
        }
        job.imuDeviceBySerial.insert(serial, info);
    }

    const auto alloc = m_swingPaths.allocateSwingDir(s->athleteLibraryPath(),
                                                     job.athleteName,
                                                     job.athleteUuid,
                                                     s->sessionNamingPattern(),
                                                     sessionTypeLabel(m_sessionType));
    job.swingDir   = alloc.swingDir;
    job.swingId    = alloc.swingId;
    job.swingIndex = alloc.swingIndex;
    job.sessionId  = alloc.sessionId;
    return job;
}

// ── Session folder lifecycle (QML) ──────────────────────────────────────────
// Same library-path / athlete / naming-pattern accessors buildSwingExportJob
// uses, so the folder chosen at session start matches the one swings save into.

QString ShotProcessor::todaySessionDir(int sessionType)
{
    if (!m_appSettings || !m_athlete)
        return {};
    return m_swingPaths.findTodaySessionDir(m_appSettings->athleteLibraryPath(),
                                            m_athlete->currentName(),
                                            m_athlete->currentUuid(),
                                            m_appSettings->sessionNamingPattern(),
                                            sessionTypeLabel(sessionType));
}

void ShotProcessor::beginSessionFolder(int sessionType, bool extend)
{
    m_sessionType = sessionType;   // so a shot's folder base uses the right type
    if (m_appSettings && m_athlete)
        m_swingPaths.beginSession(m_appSettings->athleteLibraryPath(),
                                  m_athlete->currentName(),
                                  m_athlete->currentUuid(),
                                  m_appSettings->sessionNamingPattern(),
                                  sessionTypeLabel(sessionType),
                                  extend);
    emit activeSessionDirChanged();
}

void ShotProcessor::endSessionFolder()
{
    m_swingPaths.endSession(/*discardIfNoNewSwings=*/true);
    emit activeSessionDirChanged();
}

QJsonObject ShotProcessor::buildSynthManifest() const
{
    // Mirrors the exporter's header exactly (swing_exporter.cpp), minus the
    // media streams — so a reloaded analysis-only shot is indistinguishable
    // from an exported one apart from hasVideo=false. writeSwingJson stamps the
    // schema and appends the "analysis" block.
    const int64_t t0         = m_swingWindow->startTimestampUs();
    const int64_t durationUs = m_swingWindow->endTimestampUs() - t0;
    const QDateTime wallclock = m_exportJob.wallclockAnchorUtc.addMSecs(-durationUs / 1000);

    QJsonObject root;
    root[QStringLiteral("swing")] = QJsonObject{
        { QStringLiteral("index"), m_exportJob.swingIndex },
        { QStringLiteral("id"),    m_exportJob.swingId },
    };
    root[QStringLiteral("athlete")] = QJsonObject{
        { QStringLiteral("name"),       m_exportJob.athleteName },
        { QStringLiteral("uuid"),       m_exportJob.athleteUuid },
        { QStringLiteral("handedness"), m_exportJob.handedness },
    };
    root[QStringLiteral("session")] = QJsonObject{{ QStringLiteral("dir"), m_exportJob.sessionId }};
    root[QStringLiteral("clock")] = QJsonObject{
        { QStringLiteral("t0_us"),     static_cast<qint64>(t0) },
        { QStringLiteral("unit"),      QStringLiteral("us") },
        { QStringLiteral("wallclock"), wallclock.toString(Qt::ISODateWithMs) },
    };
    root[QStringLiteral("window")] = QJsonObject{
        { QStringLiteral("start_us"), 0 },
        { QStringLiteral("end_us"),   static_cast<qint64>(durationUs) },
    };
    root[QStringLiteral("capture")] = pinpoint::SwingExporter::captureBlock(m_exportJob);
    root[QStringLiteral("streams")] = QJsonArray{};   // analysis-only — no media
    return root;
}

// ---------------------------------------------------------------------------
// Worker completion → join
// ---------------------------------------------------------------------------

void ShotProcessor::onAnalysisFinished()
{
    if (!m_analysisInFlight)
        return;   // already joined blockingly in finishNowBlocking()
    m_analysisInFlight = false;
    m_analysisResult   = m_analysisWatcher.result();
    m_analysisOutcome  = m_analysisResult.ok ? Outcome::Succeeded : Outcome::Failed;
    setAnalysisProgress(1.0);   // beat any still-queued worker updates to full
    if (m_analysisResult.ok)
        ppInfo() << "[ShotProcessor] analysis done — score" << m_analysisResult.score;
    else
        emit analysisFailed(m_analysisResult.error);   // toast, not a log line

    // Persistent club-length prior update (club_length_fusion.h / plan: robust
    // club length — starry-shimmying-wind) — LIVE path only. m_lengthPriorKey was
    // stashed by buildAnalysisJob() (empty ⇒ no athlete/club/fixed-in-place
    // face-on camera this shot, nothing to persist). Folds ONLY the PRIOR-FREE
    // fusedInstant* value into the prior — never the WITH-prior fusedPx, which
    // would self-reinforce (club_length_fusion.h updateLengthPrior comment).
    if (m_analysisResult.ok && m_analysisResult.detail && !m_lengthPriorKey.isEmpty()) {
        const pinpoint::analysis::ClubLengthEstimate &lengths = m_analysisResult.detail->shaft.lengths;
        const pinpoint::analysis::LengthFusionConfig cfg;   // defaults — live path doesn't sweep fusion.*
        if (lengths.fusedInstantPx > 0.0 && lengths.fusedInstantConf >= cfg.updateConfMin) {
            pinpoint::analysis::updateLengthPrior(m_lengthPriorState, lengths.fusedInstantPx,
                                                  lengths.fusedInstantConf, cfg);
            AppSettings  fallback;
            AppSettings *s = m_appSettings ? m_appSettings : &fallback;
            QVariantMap all   = s->clubLenPrior();
            QVariantMap entry = all.value(m_lengthPriorKey).toMap();
            entry[QStringLiteral("emaPx")]          = m_lengthPriorState.emaPx;
            entry[QStringLiteral("varPx")]          = m_lengthPriorState.varPx;
            entry[QStringLiteral("n")]              = m_lengthPriorState.n;
            entry[QStringLiteral("disagreeRun")]    = m_lengthPriorState.disagreeRun;
            entry[QStringLiteral("lengthMm")]       = static_cast<int>(std::lround(m_analysisJob.clubLengthM * 1000.0));
            entry[QStringLiteral("frameW")]         = m_analysisResult.detail->shaft.frameWidth;
            entry[QStringLiteral("frameH")]         = m_analysisResult.detail->shaft.frameHeight;
            entry[QStringLiteral("lastUpdatedUtc")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            all[m_lengthPriorKey] = entry;
            s->setClubLenPrior(all);
        }
    }

    // Pose pass is done — only now launch the x264 export, so it has the cores
    // to itself and never contends with the (just-finished) inference. maybeJoin()
    // still covers the export-skipped path completing synchronously.
    startSwingSave();
    maybeJoin();
}

void ShotProcessor::onSwingSaveFinished()
{
    if (!m_swingSaveInFlight)
        return;   // already joined blockingly in finishNowBlocking()
    m_swingSaveInFlight = false;
    const pinpoint::SwingExportResult result = m_swingSaveWatcher.result();
    if (result.ok) {
        ppInfo() << "[SwingExport] media saved:" << result.swingDir;
        m_exportOutcome  = Outcome::Succeeded;
        m_thumbnailPath  = result.thumbnailPath;
        m_exportManifest = result.manifest;   // unified swing.json written at the join
        emit swingSaved(result.swingDir);
    } else {
        ppError() << "[SwingExport] swing save failed:" << result.error;
        m_exportOutcome = Outcome::Failed;
        emit swingSaveFailed(result.error);
    }
    maybeJoin();   // failure joins identically — the buffer must resume
}

void ShotProcessor::maybeJoin()
{
    if (m_state != State::Processing)
        return;   // aborted via finishNowBlocking()
    if (m_analysisOutcome == Outcome::Pending || m_exportOutcome == Outcome::Pending)
        return;   // wait for BOTH workers

    const bool analysisOk = m_analysisOutcome == Outcome::Succeeded;
    const bool exportOk   = m_exportOutcome   == Outcome::Succeeded;

    // IMU data-integrity (offline re-fusion parity): re-fuse each IMU source from its
    // recorded raw accel+gyro and confirm it reproduces the stored quaternion. A
    // mismatch means the IMU record is internally inconsistent — the shot can't be
    // re-analysed offline — so the carousel item is flagged (⚠) and an "imuIntegrity"
    // block is persisted to swing.json. Only meaningful for the Madgwick default (the
    // only exactly-warm-startable filter); skip under ESKF to avoid a false warning.
    bool imuDataWarning = false;
    if (m_swingWindow
        && (!m_appSettings
            || m_appSettings->imuOrientationFilter().compare(QStringLiteral("ESKF"),
                                                             Qt::CaseInsensitive) != 0)) {
        const pinpoint::ImuRefusionVerdict v = pinpoint::checkImuRefusion(*m_swingWindow);
        if (v.sourcesChecked > 0) {
            imuDataWarning = v.warns();
            m_exportManifest[QStringLiteral("imuIntegrity")] = QJsonObject{
                { QStringLiteral("refusionOk"),     v.ok },
                { QStringLiteral("worstMaxDeg"),    v.worstMaxDeg },
                { QStringLiteral("sourcesChecked"), v.sourcesChecked },
                { QStringLiteral("thresholdDeg"),   v.thresholdDeg },
                { QStringLiteral("filter"),         QStringLiteral("madgwick") } };
            if (imuDataWarning)
                ppInfo() << "[ShotProcessor] IMU re-fusion parity FAILED — worst"
                         << v.worstMaxDeg << "deg over" << v.sourcesChecked
                         << "source(s); shot flagged not re-analysable";
        }
    }

    // The ONE unified swing.json (raw manifest + inline "analysis"), written here on the
    // GUI thread now that both workers have finished — no parallel-write race (the workers
    // wrote only media + returned values). savedSwingDir is set only when a swing.json was
    // actually written, so the carousel row links to a real file (rating/note write-through,
    // reload) and an unwritten shot stays in-memory only.
    QString savedSwingDir;
    if (exportOk) {
        QString werr;
        if (pinpoint::SwingDocWriter::writeSwingJson(
                m_swingDir, m_exportManifest,
                analysisOk && m_analysisResult.detail ? m_analysisResult.detail.get() : nullptr,
                &werr)) {
            savedSwingDir = m_swingDir;
            ppInfo() << "[SwingDoc] wrote" << m_swingDir + QStringLiteral("/swing.json")
                     << (analysisOk ? "(with analysis)" : "(raw only)");
        } else {
            ppError() << "[SwingDoc]" << werr;
        }
    } else if (analysisOk && m_analysisResult.detail && !m_swingDir.isEmpty()) {
        // Degraded persist: export failed/skipped but analysis succeeded — write a
        // minimal, analysis-only swing.json so the shot reloads after a restart.
        QString werr;
        QJsonObject synthManifest = buildSynthManifest();
        if (m_exportManifest.contains(QStringLiteral("imuIntegrity")))
            synthManifest[QStringLiteral("imuIntegrity")] = m_exportManifest[QStringLiteral("imuIntegrity")];
        if (pinpoint::SwingDocWriter::writeSwingJson(
                m_swingDir, synthManifest, m_analysisResult.detail.get(), &werr)) {
            savedSwingDir = m_swingDir;
            ppInfo() << "[SwingDoc] wrote analysis-only" << m_swingDir + QStringLiteral("/swing.json");
        } else {
            ppError() << "[SwingDoc] (degraded)" << werr;
        }
    }

    // The shot happened — it always lands on the carousel, with whatever the
    // pipeline produced. Club is the session's active club (Home CLUB chip →
    // SessionController.activeClub, seeded from the athlete's preferred club);
    // the user can still change it per-shot via the swing-edit popover (persisted
    // to review.club). Falls back to the athlete's preferred club, then "DRIVER".
    QString shotClub = m_session ? m_session->activeClub() : QString();
    if (shotClub.isEmpty() && m_athlete)
        shotClub = m_athlete->effectivePrimaryClub(m_athlete->currentUuid());
    if (shotClub.isEmpty())
        shotClub = QStringLiteral("DRIVER");

    // Publish the analyzed detail of the shot about to replay (the ScreenWrist in-replay
    // graph binds to it) before addShot, so it's ready when REPLAYING begins.
    m_replayAnalysisDetail = (analysisOk && m_analysisResult.detail)
                                 ? toAnalysisDetail(*m_analysisResult.detail)
                                 : QVariantMap{};
    emit replayAnalysisDetailChanged();

    int newShotId = -1;
    if (m_shotModel) {
        const QUrl thumbUrl = m_thumbnailPath.isEmpty()
                                  ? QUrl()
                                  : QUrl::fromLocalFile(m_thumbnailPath);
        newShotId = m_shotModel->addShot(savedSwingDir,
                             m_timestampLabel,
                             shotClub,
                             exportOk,
                             thumbUrl,
                             analysisOk ? m_analysisResult.tracePoints : QVariantList{},
                             analysisOk ? m_analysisResult.score : 0,
                             analysisOk ? m_analysisResult.metrics : QVariantMap{},
                             m_replayAnalysisDetail,
                             imuDataWarning);
    }

    // "Reviewable on disk": analysis + export both succeeded AND a swing.json was
    // actually written — that is the swing the UI promotes straight into Review for
    // instant playback (the disk replay reads the just-written MP4(s), not the ring).
    const bool reviewableOnDisk = analysisOk && exportOk && !savedSwingDir.isEmpty();

    if (analysisOk && exportOk)
        emit shotProcessed(newShotId, savedSwingDir);
    else
        emit shotFailed(!analysisOk ? m_analysisResult.error
                                    : QStringLiteral("export failed or skipped"));

    // Post-shot playback now lives on the Review stage: a reviewable shot is auto-
    // promoted into Review (disk replay) by the UI from shotProcessed(), so skip the
    // on-screen ¼× window transient and resume capture immediately. Only when there
    // is no reviewable disk shot do we fall back to the in-window transient (reading
    // the frozen window's frames directly) so the user still sees the swing they
    // just made.
    // Auto-replay after capture is a user setting (View menu). Off → the shot
    // still lands on the carousel, but neither the disk-replay auto-promotion (gated
    // in Main.qml.onShotProcessed) nor this in-window fallback transient plays. Handy
    // for corpus capture, where uninterrupted back-to-back hitting matters.
    const bool autoReplay = !m_appSettings || m_appSettings->autoReplayAfterCapture();

    if (reviewableOnDisk) {
        finishShot();
    } else if (autoReplay && !m_skipAnalysisCapture && !m_replayTracks.empty()) {
        startReplay();
    } else {
        if (!autoReplay)
            ppInfo() << "[ShotProcessor] replay skipped — auto-replay disabled (View menu)";
        else if (m_skipAnalysisCapture)
            ppInfo() << "[ShotProcessor] replay skipped — skip-analysis corpus capture";
        else
            ppInfo() << "[ShotProcessor] replay skipped — no captured camera tracks"
                     << "(analysisOk" << analysisOk << "exportOk" << exportOk << ")";
        finishShot();
    }
}

// ---------------------------------------------------------------------------
// ¼× replay (migrated from CameraManager)
// ---------------------------------------------------------------------------

void ShotProcessor::startReplay()
{
    // Anchor to the actual first/last captured entry so replay starts
    // immediately rather than waiting for the (potentially empty) leading
    // portion of the window.
    m_replayWindowStartUs = m_swingWindow->endTimestampUs();
    m_replayWindowEndUs   = m_swingWindow->startTimestampUs();
    for (const ReplayTrack &track : m_replayTracks) {
        if (!track.entries.empty()) {
            m_replayWindowStartUs = std::min(m_replayWindowStartUs,
                                             track.entries.front().timestamp_us);
            m_replayWindowEndUs   = std::max(m_replayWindowEndUs,
                                             track.entries.back().timestamp_us);
        }
    }

    // Clamp to the detected swing (v3 G2): replay starts at address, not
    // mid-fidget. conf 0 (no IMU / failed pre-stage) keeps the full span.
    if (m_segmentation.conf > 0.f) {
        const int64_t lo = std::max(m_replayWindowStartUs, m_segmentation.swingStartUs);
        const int64_t hi = std::min(m_replayWindowEndUs,   m_segmentation.swingEndUs);
        if (hi > lo) {
            m_replayWindowStartUs = lo;
            m_replayWindowEndUs   = hi;
        }
    }

    for (ReplayTrack &track : m_replayTracks) {
        track.idx = 0;
        track.ctrl->setReplaying(true);
    }

    m_replayPositionUs = m_replayWindowStartUs;   // playhead at the window start
    emit replaySpanChanged();
    emit replayPositionChanged();

    setState(State::Replaying);

    m_replayElapsed.start();
    m_replayTimer = new QTimer(this);
    m_replayTimer->setInterval(16);   // ~60 Hz drive
    connect(m_replayTimer, &QTimer::timeout, this, &ShotProcessor::onReplayTick);
    m_replayTimer->start();
}

void ShotProcessor::onReplayTick()
{
    // Quarter speed: divide real elapsed time by 4 to get virtual footage time.
    const int64_t realElapsedUs   = m_replayElapsed.elapsed() * 1000LL;
    const int64_t virtualTimeUs   = realElapsedUs / 4;
    const int64_t footageDuration = m_replayWindowEndUs - m_replayWindowStartUs;

    // Publish the playhead (window µs) — the graph and video both follow this one clock.
    const int64_t pos = m_replayWindowStartUs + virtualTimeUs;
    if (pos != m_replayPositionUs) {
        m_replayPositionUs = pos;
        emit replayPositionChanged();
    }

    if (virtualTimeUs >= footageDuration) {
        stopReplay(true);
        return;
    }

    for (ReplayTrack &track : m_replayTracks) {
        // Advance to the newest frame whose offset from the first entry <= virtual time.
        while (track.idx + 1 < track.entries.size()) {
            const int64_t nextOffset =
                track.entries[track.idx + 1].timestamp_us - m_replayWindowStartUs;
            if (nextOffset <= virtualTimeUs)
                ++track.idx;
            else
                break;
        }

        const auto &entry  = track.entries[track.idx];
        const auto  handle = m_swingWindow->payloadOf(entry);
        if (!handle.data) continue;

        const auto &fd = m_swingWindow->formatOf(track.sourceId);
        if (const auto *cfmt = std::get_if<pinpoint::CameraFormat>(&fd.format)) {
            track.ctrl->displayReplayFrame(
                handle.data,
                handle.bytes,
                static_cast<int>(cfmt->width),
                static_cast<int>(cfmt->height),
                cfmt->pixel_format,
                cfmt->plane_strides);
        }
    }
}

void ShotProcessor::cancelReplay()
{
    if (m_state != State::Replaying)
        return;
    ppInfo() << "[ShotProcessor] replay cancelled by user";
    stopReplay(true);   // normal end-of-replay path, taken early
}

void ShotProcessor::stopReplay(bool thenFinish)
{
    if (m_replayTimer) {
        m_replayTimer->stop();
        m_replayTimer->deleteLater();
        m_replayTimer = nullptr;
    }

    for (ReplayTrack &track : m_replayTracks)
        track.ctrl->setReplaying(false);

    if (thenFinish)
        finishShot();
}

// ---------------------------------------------------------------------------
// Finish / teardown
// ---------------------------------------------------------------------------

void ShotProcessor::finishShot()
{
    m_swingWindow.reset();   // both workers returned, replay stopped
    m_replayTracks.clear();

    // The window held the buffer Paused; now that it is gone, return the
    // buffer to whatever the user last chose (Capture/Stop). The unconditional
    // bufferStateChanged it emits re-arms ShotController.
    m_cameraManager->applyCaptureIntent();

    setState(State::Idle);
}

void ShotProcessor::abortToIdle()
{
    m_postRollTimer.stop();
    setState(State::Idle);
}

void ShotProcessor::finishNowBlocking()
{
    m_postRollTimer.stop();   // a pending shot is forfeited — acceptable on teardown

    if (m_state == State::Replaying)
        stopReplay(false);

    // Block until the pre-stage and both workers have returned: they read ring
    // memory through the window, which the caller is about to invalidate
    // (deregister/teardown).
    if (m_segmentationInFlight)
        m_segmentationWatcher.waitForFinished();
    if (m_analysisInFlight)
        m_analysisWatcher.waitForFinished();
    if (m_swingSaveInFlight)
        m_swingSaveWatcher.waitForFinished();
    // The queued finished() handlers will still be delivered later; flag-off
    // makes them no-ops.
    m_segmentationInFlight = false;
    m_analysisInFlight  = false;
    m_swingSaveInFlight = false;

    m_swingWindow.reset();
    m_replayTracks.clear();

    // Deliberately no applyCaptureIntent(): callers (setSelected, destructors)
    // own the buffer-state sequence around source registration.
    setState(State::Idle);
}
