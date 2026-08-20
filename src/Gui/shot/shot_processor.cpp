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
#include "composite_payload_source.h"
#include "deferred_stitch.h"
#include "event_buffer.h"
#include "hm_instance.h"
#include "ram_payload_source.h"
#include "imu_instance.h"
#include "imu_manager.h"
#include "session_controller.h"
#include "shot_list_model.h"
#include "../Analysis/club_length_fusion.h"
#include "../Analysis/imu_refusion_check.h"
#include "../Analysis/imu_vision_fuser.h"
#include "../Analysis/phase_segmenter.h"
#include "../Analysis/swing_analysis.h"
#include "../IMU/hm_frame.h"          // isSelected() — no frame, no binding
#include "../Export/swing_doc.h"
#include "../Core/club_vocabulary.h"
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
#include <memory>
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

// ── Deferred gather (deferred_sources_design.md §4.1, brief Phase E) ────────
//
// A pull takes about as long as its window spans and the library serialises
// them, so a 3 s pre / 1.5 s post request costs ~4.5 s — and it cannot start
// until the window's last sample exists, at impact + 1.5 s. Reserving at
// DETECTION hides most of that inside the post-roll we were taking anyway.
//
// ⚠ THE TWO DEADLINES ARE ORDERED ON PURPOSE. The library's deadline sits just
// INSIDE ours, so a slow pull materialises its own block — carrying whatever
// arrived plus its coverage — rather than being cancelled by our timeout. A
// cancelled request still produces a block, but one that says CANCELLED where
// the honest answer was TIMED_OUT with 60 % coverage.
constexpr qint64 kHistoryDeadlineUs = 7'000'000;   // past impact: library gives up here
constexpr int    kGatherDeadlineMs  = 6'000;       // past pause:  we give up here
constexpr int    kGatherPollMs      = 50;

// The subset of a snapshot belonging to one source, ascending — the live half
// of a stitch. Cheap: this runs once per deferred lane per shot.
std::vector<pinpoint::IndexEntry> entriesForIn(
    const std::vector<pinpoint::IndexEntry> &all, pinpoint::SourceId id)
{
    std::vector<pinpoint::IndexEntry> out;
    for (const pinpoint::IndexEntry &e : all)
        if (e.source_id == id) out.push_back(e);
    return out;
}

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

// Face-on swing-plane transition delta (shaft_plane.h), nested as "plane" under
// the "club" detail block — identical shape in both parity writers, like
// toLengthsDetail above. Always written, even when nothing fitted: valid=false
// with the per-window reject codes set says "the producer ran and found nothing",
// which a reader must be able to tell from an older file that has no key at all.
// The two channels' quality fields are NOT interchangeable — see ShaftPlaneChannel.
QVariantMap toPlaneChannelDetail(const pinpoint::analysis::ShaftPlaneChannel &c)
{
    return QVariantMap{
        { QStringLiteral("fitted"),           c.fitted },
        { QStringLiteral("iotaBackDeg"),      c.iotaBackDeg },
        { QStringLiteral("iotaDownDeg"),      c.iotaDownDeg },
        { QStringLiteral("deltaDeg"),         c.deltaDeg },
        { QStringLiteral("nodeBackDeg"),      c.nodeBackDeg },
        { QStringLiteral("nodeDownDeg"),      c.nodeDownDeg },
        { QStringLiteral("nBack"),            c.nBack },
        { QStringLiteral("nDown"),            c.nDown },
        { QStringLiteral("conicResidBack"),   c.conicResidBack },
        { QStringLiteral("conicResidDown"),   c.conicResidDown },
        { QStringLiteral("ratioBack"),        c.ratioBack },
        { QStringLiteral("ratioDown"),        c.ratioDown },
        { QStringLiteral("splitHalfBackDeg"), c.splitHalfBackDeg },
        { QStringLiteral("splitHalfDownDeg"), c.splitHalfDownDeg },
        { QStringLiteral("anchorsBack"),      c.anchorsBack },
        { QStringLiteral("anchorsDown"),      c.anchorsDown },
        { QStringLiteral("anchorConfMin"),    double(c.anchorConfMin) },
        { QStringLiteral("rejectBack"),       c.rejectBack },
        { QStringLiteral("rejectDown"),       c.rejectDown },
    };
}

QVariantMap toPlaneDetail(const pinpoint::analysis::ShaftPlaneEstimate &p)
{
    return QVariantMap{
        { QStringLiteral("valid"),    p.valid },
        { QStringLiteral("channel"),  p.channel },
        { QStringLiteral("measured"), toPlaneChannelDetail(p.measured) },
        { QStringLiteral("synth"),    toPlaneChannelDetail(p.synth) },
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
        QVariantMap sm{ { QStringLiteral("key"),   m.key },
                        { QStringLiteral("label"), m.label },
                        { QStringLiteral("unit"),  m.unit },
                        { QStringLiteral("t_us"),  ts },
                        { QStringLiteral("value"), vs },
                        { QStringLiteral("phaseSamples"), samples } };
        // 1σ measurement noise, ABSENT when the producer never characterised one — the key is
        // omitted rather than set to 0, because a zero would read as "measured, and perfect".
        // Mirrored in disk_replay_source.cpp: a live shot and its reloaded self must agree.
        if (m.sigma)
            sm.insert(QStringLiteral("sigma"), *m.sigma);
        series.append(sm);
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
            // Face-on swing plane (shaft_plane.h) — see toPlaneDetail(); mirrors
            // swing_doc.cpp's analysis.club.plane.
            { QStringLiteral("plane"),         toPlaneDetail(a.shaft.plane) },
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
    // Repeating, not single-shot: the gather polls until every deferred source
    // has reported or the deadline passes.
    m_gatherTimer.setSingleShot(false);
    connect(&m_gatherTimer, &QTimer::timeout, this, &ShotProcessor::onGatherPoll);

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
    case State::Gathering:  return QStringLiteral("gathering");
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

    // ⚠ RESERVE AT DETECTION, COLLECT AT THE GATHER (history.h C1). Retrieval
    // cannot begin until the window's last sample exists, and it then costs
    // about as long as the window spans — so telling the deferred sources NOW
    // what range we are going to want lets that cost hide inside the post-roll
    // instead of being added after it. Reserving does not block and issues no
    // radio traffic. A refusal is recorded by the source and changes nothing
    // here: a shot with no deferred data is the ordinary path, not an error.
    for (HmInstance *hm : deferredSources())
        hm->reserveHistory(timestampUs, timestampUs + kHistoryDeadlineUs);

    setAnalysisProgress(0.0);   // the ANALYSING bar starts empty for each shot
    setState(State::PostRoll);
    m_postRollTimer.start(postRollMsFor(source));
}

void ShotProcessor::onPostRollExpired()
{
    if (m_state != State::PostRoll)
        return;   // finishNowBlocking() raced the timer
    beginGather();
}

// ---------------------------------------------------------------------------
// Window capture → concurrent analysis + export
// ---------------------------------------------------------------------------

QVector<HmInstance *> ShotProcessor::deferredSources() const
{
    QVector<HmInstance *> out;
    if (!m_imuManager) return out;
    for (const QVariant &v : m_imuManager->instances())
        if (auto *hm = qobject_cast<HmInstance *>(v.value<QObject *>()))
            out.push_back(hm);
    return out;
}

void ShotProcessor::beginGather()
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

    // ⚠ THE WINDOW BOUNDS ARE FROZEN HERE, AT THE PAUSE. kWindowDuration is a
    // TRAILING span, so resolving it against a post-gather `now` would slide the
    // window several seconds past the swing it is meant to contain and the
    // snapshot would come back empty — with nothing reporting an error.
    m_historyProvenance.clear();   // per shot — never inherited from the last one
    m_stitchCounts.clear();

    m_windowEndUs   = pinpoint::EventBuffer::nowMicros();
    m_windowStartUs = m_windowEndUs - kWindowDuration.count() * 1000LL;

    // ⚠ AND THE RESUME GUARD GOES UP HERE, for the same instant's reason (§3.5):
    // `resume_clear_rings` is true, so a resume arriving between the freeze and
    // the window's construction would clear the very rings we are about to
    // snapshot. CameraManager::resumeBuffer() is the hard backstop and it reads
    // exactly this flag, so raising it now is what makes the backstop cover the
    // whole gather.
    m_ringSource = m_buffer->makeRingPayloadSource();

    int pending = 0;
    for (HmInstance *hm : deferredSources())
        if (hm->historyPending()) ++pending;

    if (pending == 0) {
        // Nothing deferred — no HackMotion, no reservation, or it refused at the
        // call site. Construct immediately, exactly as this pipeline did before
        // deferred sources existed. ⚠ This is the ORDINARY path, not an error
        // path, and it must stay byte-identical to the undeferred behaviour.
        finishGatherAndLaunch();
        return;
    }

    ppInfo() << "[ShotProcessor] gathering —" << pending << "deferred source(s)";
    setState(State::Gathering);
    m_gatherDeadlineMs = QDateTime::currentMSecsSinceEpoch() + kGatherDeadlineMs;
    m_gatherTimer.start(kGatherPollMs);
}

void ShotProcessor::onGatherPoll()
{
    if (m_state != State::Gathering)
        return;   // finishNowBlocking() raced the timer

    int stillPending = 0;
    for (HmInstance *hm : deferredSources())
        if (hm->historyPending()) ++stillPending;

    const bool timedOut = QDateTime::currentMSecsSinceEpoch() >= m_gatherDeadlineMs;
    if (stillPending > 0 && !timedOut)
        return;

    if (timedOut && stillPending > 0) {
        // ⚠ Not an error, and NOT a cancel. The library's own deadline sits
        // inside ours, so a request still outstanding here has already
        // materialised its block with whatever arrived; we simply stop waiting.
        ppWarn() << "[ShotProcessor] gather deadline reached with"
                 << stillPending << "deferred source(s) outstanding";
    }

    m_gatherTimer.stop();
    finishGatherAndLaunch();
}

void ShotProcessor::finishGatherAndLaunch()
{
    m_gatherTimer.stop();

    // Compose the window's backing: the frozen ring for everything, overridden
    // by an in-RAM stitched lane for any source whose high-rate samples only
    // arrived after the freeze (design §3.2). With nothing deferred the
    // composite is a single catch-all route and the window is what it always was.
    auto entries   = m_buffer->snapshot(m_windowStartUs, m_windowEndUs);
    auto composite = std::make_unique<pinpoint::CompositePayloadSource>();
    auto ram       = std::make_unique<pinpoint::RamPayloadSource>();
    std::vector<pinpoint::SourceId> deferredIds;

    for (HmInstance *hm : deferredSources()) {
        HmInstance::HistoryResult hist = hm->takeHistoryResult();
        if (!hist.valid)
            continue;      // no block, or nothing usable in it — live lane stands

        HmUnit *const units[2] = { hm->unitLowerArm(), hm->unitPalm() };
        for (int u = 0; u < 2; ++u) {
            HmUnit *unit = units[u];
            if (!unit || unit->sourceId() == pinpoint::kInvalidSourceId)
                continue;
            const pinpoint::SourceId sid = unit->sourceId();

            pinpoint::DeferredStitchInput in;
            in.liveEntries     = entriesForIn(entries, sid);
            in.liveSample      = [this, sid](const pinpoint::IndexEntry &e)
                                     -> const pinpoint::ImuSample * {
                const auto h = m_ringSource->payloadOf(sid, e.source_sequence);
                if (!h.data || h.bytes != sizeof(pinpoint::ImuSample))
                    return nullptr;
                return reinterpret_cast<const pinpoint::ImuSample *>(h.data);
            };
            in.deferredTUs     = hist.tUs;
            in.deferredSamples = (u == 0) ? hist.lowerArm : hist.palm;
            in.delivered       = hist.delivered;

            const pinpoint::DeferredStitchResult st =
                pinpoint::stitchDeferredLane(in);
            if (st.samples.empty())
                continue;

            // The stitched lane is served WHOLLY from RAM: one id cannot split
            // its sequence space across two backings, so its ring entries are
            // replaced rather than added to.
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                              [sid](const pinpoint::IndexEntry &e) {
                                  return e.source_id == sid;
                              }),
                          entries.end());
            for (size_t i = 0; i < st.tUs.size(); ++i)
                entries.push_back(pinpoint::IndexEntry{
                    st.tUs[i], sid, uint64_t(i), 0, 0 });

            ram->addImu(sid, m_ringSource->formatOf(sid), st.samples);
            deferredIds.push_back(sid);

            m_stitchCounts.insert(sid, { st.usedLive, st.usedDeferred });

            ppInfo() << "[ShotProcessor] stitched lane" << sid
                     << "— live" << st.usedLive
                     << "deferred" << st.usedDeferred
                     << "dropped" << st.droppedNonMonotonic;
        }
        m_historyProvenance.insert(hm->deviceId(), hist);
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const pinpoint::IndexEntry &a, const pinpoint::IndexEntry &b) {
                         return a.timestamp_us < b.timestamp_us;
                     });

    if (!deferredIds.empty())
        composite->add(std::move(ram), deferredIds);   // claimed first
    composite->add(std::move(m_ringSource), {});       // catch-all: the ring

    m_swingWindow.emplace(std::move(composite), std::move(entries),
                          m_windowStartUs, m_windowEndUs);

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
    // bounds bound the heavy-stage scan windows and the in-window replay. The
    // job is resolved on the UI thread NOW (value types only); failure or
    // no-IMU yields a conf-0 result and everything below degrades to
    // full-window behaviour.
    //
    // ⚠ IT DOES NOT TRIM THE EXPORT, whatever this comment used to claim.
    // SwingExportJob carries no span and SwingExporter reads no bounds: the
    // encode is the whole frozen window, always. Playback trims instead —
    // DiskReplaySource on the persisted Finish event, startReplay() below on
    // the analysed one — which is why a swing.json can hold a 4 s clip and
    // still replay 2.7 s of swing.
    m_analysisJob = buildAnalysisJob();
    const pinpoint::SwingWindow *win = &*m_swingWindow;
    m_segmentationInFlight = true;
    m_segmentationWatcher.setFuture(QtConcurrent::run(
        [bindings = m_analysisJob.imuBindings, impactUs = m_impactUs, win] {
            try {
                const pinpoint::analysis::FusedStreams streams =
                    pinpoint::analysis::ImuVisionFuser::fuse(
                        *win, bindings,
                        pinpoint::analysis::ImuVisionFuser::gridHzForWindow(*win, bindings));
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

        // ── HackMotion, which the loop above cannot see ──────────────────────────
        //
        // An HmInstance is a PEER of ImuInstance, not a subclass — both derive
        // ImuDeviceBase (hm_instance.h:229-235) — so the qobject_cast above returns
        // null for it and always has. That is why no wG3 has ever produced a wrist
        // metric: no binding, so the fuser never sees the lane and MetricExtractor
        // never runs on it. Phase E3 proved the samples arrive and are recorded
        // honestly; it proved nothing downstream consumed them, because nothing did.
        //
        // Written as a second pass rather than folded into the first so the Witmotion
        // path above is untouched, byte for byte.
        for (const QVariant &v : insts) {
            auto *hm = qobject_cast<HmInstance *>(v.value<QObject *>());
            if (!hm) continue;

            // ⚠ NO FRAME, NO BINDING. Until a directed capture has selected a
            // candidate, hm_frame::toAnatomical() returns identity by design — a lane
            // with no anatomical frame must drive nothing rather than drive a plausible
            // guess. Binding it anyway would feed identity quaternions to the wrist
            // decomposition and publish the result as a measurement.
            if (!pinpoint::hm_frame::isSelected()) {
                ppWarn() << "[ShotProcessor]" << hm->deviceDescription()
                         << "— no HackMotion frame candidate selected, so this device's"
                            " lanes are recorded but not bound; no wrist metric will be"
                            " produced from them.";
                continue;
            }

            HmUnit *const units[] = { hm->unitLowerArm(), hm->unitPalm() };
            for (HmUnit *const unit : units) {
                if (!unit) continue;
                const pinpoint::SourceId sid = unit->sourceId();
                if (sid == pinpoint::kInvalidSourceId)
                    continue;   // this unit never registered a lane
                if (std::find(job.imuSources.begin(), job.imuSources.end(), sid)
                    == job.imuSources.end())
                    continue;   // this unit is not a source in the captured window

                // ⚠ PLACEMENT IS UNIT-KEYED FOR A HACKMOTION, so the bare
                // placement.value(deviceId) lookup the Witmotion path uses cannot work
                // here: a wG3's keys are "<deviceId>#lowerArm" / "<deviceId>#palm"
                // (Phase C). Rather than respell that format — the parser for it is
                // file-static inside imu_manager.cpp, and a second copy of the spelling
                // is precisely the drift that would silently orphan a device's
                // placement — ask the canonical resolver which object holds each slot
                // and match on identity. instanceForSlot() returns the HmUnit for a
                // HackMotion, which is the whole reason HmUnit exists.
                QString slot;
                for (const QString &s : { QStringLiteral("A"), QStringLiteral("B"),
                                          QStringLiteral("C") }) {
                    if (m_imuManager->instanceForSlot(s) == static_cast<QObject *>(unit)) {
                        slot = s;
                        break;
                    }
                }
                if (slot.isEmpty())
                    continue;   // unassigned unit — recorded, but bound to no segment

                pinpoint::analysis::ImuSegmentBinding b;
                b.source     = sid;
                b.role       = segmentRoleForSlot(m_sessionType, slot);
                b.hackMotion = true;
                // A is IDENTITY for this lane by design, not as a placeholder: the
                // device referenced the pair at its own calibration pose, so there is
                // no per-session world->anatomical solve to record. M carries the frame
                // constant. Both are read off the unit rather than recomputed here, so
                // the binding and the live readout cannot diverge.
                b.alignA = unit->alignA();
                b.mountM = unit->mountM();
                // ⚠ THE MOUNT DEVIATIONS STAY 0.0 BECAUSE THERE IS NO MOUNT SOLVE FOR
                // THIS DEVICE, not because nobody filled them in. A Witmotion earns
                // those two numbers from our own two-pose calibration; a wG3 applies
                // its own and streams the result, so there is no residual to report and
                // any value here would be an invention. anatCalibrated is the real gate
                // — it is false unless the device says CALIBRATED and a frame is
                // selected — and CaptureCapabilities reads `calibrated`, so the two
                // carry the same honest answer rather than a fabricated composite.
                b.anatCalibrated = unit->anatCalibrated();
                b.calibrated     = unit->anatCalibrated();
                job.imuBindings.push_back(b);
            }
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
    job.micTravelUs          = s->micTravelUs();
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

            // A HackMotion wG3 is ONE peripheral that registers TWO EventBuffer
            // sources, "<deviceId>#lowerArm" / "<deviceId>#palm" (hm_instance.h) —
            // neither equals `serial` above (the bare device id), so the alias
            // and `info` this loop iteration built reach no stream at all: a
            // captured wG3's two lanes would export with the raw unit id as
            // their only label and no device object. Insert under each real
            // unit id instead; additive, and unreachable for a Witmotion
            // (qobject_cast below returns null).
            for (const QVariant &v : insts) {
                auto *hm = qobject_cast<HmInstance *>(v.value<QObject *>());
                if (!hm || hm->deviceId() != dev.id)
                    continue;
                const QString baseAlias = alias.isEmpty() ? serial : alias;
                HmUnit *const units[2] = { hm->unitLowerArm(), hm->unitPalm() };
                for (HmUnit *unit : units) {
                    // sourceId() is kInvalidSourceId if this unit's own
                    // registerSource() call failed — BLE registration is
                    // allowed to fail per unit, and a lane with no source
                    // never appears in the export window, so an alias/device
                    // entry for it would just be dead.
                    if (!unit || unit->sourceId() == pinpoint::kInvalidSourceId)
                        continue;
                    const QString unitId = unit->unitId();
                    job.imuAliasBySerial.insert(
                        unitId, baseAlias + QStringLiteral(" · ") + unit->unitLabel());

                    // ⚠ Deliberately NOT a copy of `info` above: hasCalibration/
                    // alignA/mountM must stay unset (SwingImuDeviceInfo's
                    // defaults) — the re-analyzer treats a device object
                    // carrying a 4-element alignA + mountM pair as a fallback
                    // IMU->segment binding (swing_reanalyzer.cpp:471), so
                    // writing them here would silently bind a HackMotion lane
                    // into the Witmotion wrist maths in a frame nobody has
                    // reconciled — that reconciliation is Phase D.
                    // fusionMode/orientationFilter stay empty too, rather than
                    // inheriting the app's Witmotion defaults: this device
                    // fuses on-device, and a persisted "madgwick" against a
                    // HackMotion lane would be a fabricated provenance record.
                    // placementSlot/role/roleName stay unset — unit-keyed
                    // placement is Phase C; an empty slot correctly resolves to
                    // no role rather than a guessed one.
                    pinpoint::SwingImuDeviceInfo hmInfo;
                    // Measured average of an ADAPTIVE rate (25 Hz at rest, 100 Hz
                    // in motion, dense bursts to ~799 Hz) — not a configured
                    // output rate like Witmotion's outputRateHz, so a reader must
                    // not treat this single number as a period. Rounded to the
                    // nearest Hz; the settings default of 200 never applies here.
                    hmInfo.outputRateHz = int(std::lround(hm->dataRateHz()));
                    // Palm-minus-lower-arm skew: the two blocks are NOT paired as
                    // simultaneous (see SwingImuDeviceInfo::skewUs). NaN until a
                    // sample has been seen — leave 0.0 (the "absent" value the
                    // exporter checks) rather than writing a fake measurement.
                    // ⚠ The MEDIAN: a single record's difference is mostly ±½-sample
                    // pairing jitter, so a mean would bake those outliers in.
                    const double skew = hm->skewUsMedian();
                    if (std::isfinite(skew))
                        hmInfo.skewUs = skew;

                    // ── Capture provenance, scoped to THIS window (Phase B′) ──
                    // ⚠ Queried per unit, not per device: pinning is a property of
                    // one board — the palm sits at the larger radius and saturates
                    // first — so folding both units into one count would report a
                    // clipped palm as a fault of the lower arm as well.
                    //
                    // ⚠ Left entirely at its "not measured" defaults when there is no
                    // window. A swing exported without one cannot say what happened
                    // inside it, and inventing a clean answer is the failure this
                    // whole block exists to prevent.
                    if (m_swingWindow) {
                        const HmInstance::CaptureProvenance prov =
                            hm->captureProvenance(m_swingWindow->startTimestampUs(),
                                                  m_swingWindow->endTimestampUs());
                        hmInfo.hmCalibrationStateAtStart = prov.calibration.stateAtStart;
                        hmInfo.hmCalibrationStateAtEnd   = prov.calibration.stateAtEnd;
                        hmInfo.hmCalibrationSpansTransition =
                            prov.calibration.spansTransition;
                        hmInfo.hmConfigBits           = prov.configBits;
                        hmInfo.hmProvenanceDropped    = int(prov.exceptionsDropped);
                        hmInfo.hmNoFitSkippedSession  = int(prov.noFitSkipped);

                        const quint8 thisUnit =
                            quint8(unit == hm->unitPalm() ? WR_UNIT_PALM
                                                          : WR_UNIT_LOWER_ARM);
                        for (const HmInstance::SampleException &e : prov.exceptions) {
                            switch (e.reason) {
                            case HmInstance::SampleException::Pinned:
                                if (e.unit == thisUnit)
                                    ++hmInfo.hmPinnedSamples;
                                break;
                            case HmInstance::SampleException::QuatNormSuspect:
                                // ⚠ Record-level, so it counts for BOTH units rather
                                // than being assigned to one. A misaligned decode is
                                // not something one board did.
                                ++hmInfo.hmQuatNormSuspect;
                                break;
                            default:
                                break;
                            }
                        }
                    }
                    // ── Deferred history provenance (Phase E) ───────────
                    // ⚠ Keyed by DEVICE, applied to BOTH units: one wG3 is one
                    // peripheral with one stream and one pull, so the coverage,
                    // the gaps and the fit describe both lanes equally. The
                    // stitch COUNTS differ per lane and are set below.
                    const auto hIt = m_historyProvenance.constFind(hm->deviceId());
                    if (hIt != m_historyProvenance.constEnd() && hIt->valid) {
                        const HmInstance::HistoryResult &h = *hIt;
                        hmInfo.hmHistoryStatus      = h.status;
                        hmInfo.hmHistoryAttempts    = h.attempts;
                        hmInfo.hmCoverageOverflowed = h.coverageOverflowed;
                        hmInfo.hmCoverageFraction   = h.coverageFraction;
                        hmInfo.hmDensity            = h.density;
                        hmInfo.hmAchievedHz         = h.achievedHz;
                        hmInfo.hmLargestGapUs       = qint64(h.largestGapUs);
                        hmInfo.hmLiveOverlapSamples    = int(h.liveOverlapSamples);
                        hmInfo.hmLiveOverlapMismatches = int(h.liveOverlapMismatches);
                        hmInfo.hmSelfRecordingGapStartUs = h.selfRecordingGapStartUs;
                        hmInfo.hmSelfRecordingGapEndUs   = h.selfRecordingGapEndUs;
                        hmInfo.hmDelivered = h.delivered;
                        for (const auto &g : h.gaps)
                            hmInfo.hmGaps.emplace_back(g.startUs, g.endUs, g.kind);

                        hmInfo.hmFitValid            = true;
                        hmInfo.hmFitFlags            = h.fit.flags;
                        hmInfo.hmFitObservations     = h.fit.observations;
                        hmInfo.hmFitRateHz           = h.fit.fitted_rate_hz;
                        hmInfo.hmFitAnchorHostUs     = qint64(h.fit.anchor_host_us);
                        hmInfo.hmFitAnchorIndex      = h.fit.anchor_index;
                        hmInfo.hmFitSlopeUsPerIndex  = h.fit.slope_us_per_index;
                        hmInfo.hmFitOffsetUs         = qint64(h.fit.offset_us);
                        hmInfo.hmFitSpanUs           = qint64(h.fit.span_us);
                        hmInfo.hmFitResidualMedianUs = h.fit.residual_median_us;
                        hmInfo.hmFitResidualP90Us    = h.fit.residual_p90_us;
                        hmInfo.hmFitResidualMaxUs    = h.fit.residual_max_us;
                        hmInfo.hmFitDriftUsPerS      = h.fit.accuracy_drift_us_per_s;

                        const auto sIt = m_stitchCounts.constFind(unit->sourceId());
                        if (sIt != m_stitchCounts.constEnd()) {
                            hmInfo.hmStitchedFromLive     = sIt->first;
                            hmInfo.hmStitchedFromDeferred = sIt->second;
                        }
                    }

                    job.imuDeviceBySerial.insert(unitId, hmInfo);
                }
                break;
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
            // Shared with the re-analysis write-back (swing_doc.h) so the two
            // producers of this block cannot drift.
            m_exportManifest[QStringLiteral("imuIntegrity")] = pinpoint::imuIntegrityJson(v);
            if (imuDataWarning)
                ppInfo() << "[ShotProcessor] IMU re-fusion parity FAILED — worst"
                         << v.worstMaxDeg << "deg over" << v.sourcesChecked
                         << "source(s); shot flagged not re-analysable";
        }
    }

    // The club this shot was hit with: the session's active club (Home CLUB chip →
    // SessionController.activeClub, seeded from the athlete's preferred club), else the
    // athlete's preferred club, else the stub. Resolved BEFORE the document is written —
    // it goes into swing.json as well as onto the carousel row, and a club that reached
    // only the row was lost the moment the session reloaded from disk.
    QString shotClub = m_session ? m_session->activeClub() : QString();
    if (shotClub.isEmpty() && m_athlete)
        shotClub = m_athlete->effectivePrimaryClub(m_athlete->currentUuid());
    if (shotClub.isEmpty())
        shotClub = pinpoint::clubStub();

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
                &werr, shotClub)) {
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
                m_swingDir, synthManifest, m_analysisResult.detail.get(), &werr, shotClub)) {
            savedSwingDir = m_swingDir;
            ppInfo() << "[SwingDoc] wrote analysis-only" << m_swingDir + QStringLiteral("/swing.json");
        } else {
            ppError() << "[SwingDoc] (degraded)" << werr;
        }
    }

    // The shot happened — it always lands on the carousel, with whatever the pipeline
    // produced, carrying the same shotClub the document above records. The user can still
    // change it per-shot via the swing-edit popover (persisted to review.club).
    //
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

    m_lastShotId = newShotId;

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

// Breathing room after the located Finish, so the club is seen to arrive rather than the
// picture cutting on the frame the detector settled on. Matches the segmenter's own
// boundPadUs in spirit; kept local because this is a viewing choice, not a measurement.
static constexpr int64_t kReplayFinishPadUs = 250000;   // 250 ms

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
        // END ON THE ANALYSED FINISH WHERE THERE IS ONE. m_segmentation is the PRE-STAGE
        // ladder — fused from the IMU before the club track exists — so its swingEndUs
        // carries the segmenter's window-edge fallback whenever the quiet-run detector
        // never fired, which on this corpus was every swing: the replay then ran ~1.3 s
        // past a finish the camera had already located. The analysed ladder has been
        // arbitrated against the club track by then (timeline_fusion.h), so its Finish is
        // the real one. Bounds are deliberately NOT rewritten by fusion — it moves labels,
        // never spans — so the correction belongs here, at the point of use.
        int64_t endUs = m_segmentation.swingEndUs;
        if (m_analysisResult.ok && m_analysisResult.detail) {
            using pinpoint::analysis::Phase;
            const auto &seg = m_analysisResult.detail->segmentation;
            if (const auto *fin = seg.eventFor(Phase::Finish))
                endUs = fin->t_us + kReplayFinishPadUs;
        }
        const int64_t hi = std::min(m_replayWindowEndUs, endUs);
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
    m_gatherTimer.stop();
    // ⚠ RELEASING THIS IS NOT OPTIONAL. It holds the resume guard from the pause
    // instant, and CameraManager::resumeBuffer() refuses while the guard is up —
    // so an abandoned gather that kept it would leave the buffer unable to
    // resume for the rest of the session, with capture silently dead.
    m_ringSource.reset();
    setState(State::Idle);
}

void ShotProcessor::finishNowBlocking()
{
    m_postRollTimer.stop();   // a pending shot is forfeited — acceptable on teardown
    m_gatherTimer.stop();

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
    // A gather abandoned on teardown: release the guard the window never adopted
    // (a no-op once the window exists, which took ownership of it).
    m_ringSource.reset();

    // Deliberately no applyCaptureIntent(): callers (setSelected, destructors)
    // own the buffer-state sequence around source registration.
    setState(State::Idle);
}
