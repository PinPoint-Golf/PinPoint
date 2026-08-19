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

#include "shot_replay_controller.h"

#include "app_settings.h"

#include "../Analysis/swing_analysis.h"   // SegmentRole + segmentRoleName

#include <algorithm>

ShotReplayController::ShotReplayController(AppSettings *appSettings, QObject *parent)
    : QObject(parent)
    , m_appSettings(appSettings)
    , m_source(makeDiskReplaySource())
{
    // Forward the source's transport/clock signals as our own; the QML-facing
    // change signals (activeChanged) are owned by the controller and emitted on
    // start()/stop()/abort.
    connect(m_source.get(), &ReplaySource::positionChanged, this, &ShotReplayController::positionChanged);
    connect(m_source.get(), &ReplaySource::spanChanged,     this, &ShotReplayController::spanChanged);
    connect(m_source.get(), &ReplaySource::playingChanged,  this, &ShotReplayController::playingChanged);
    connect(m_source.get(), &ReplaySource::speedChanged,    this, &ShotReplayController::speedChanged);
    connect(m_source.get(), &ReplaySource::failed,          this, &ShotReplayController::replayFailed);
    connect(m_source.get(), &ReplaySource::aborted,         this, &ShotReplayController::onAborted);
    // Queued: the source emits this from inside QMediaPlayer's own mediaStatusChanged
    // callback, and the host's reaction (auto-return to Capture) tears the source down
    // via stop()/unload(). Deferring out of the player's callback avoids re-entrant
    // teardown — same reasoning as DiskReplaySource's InvalidMedia path.
    connect(m_source.get(), &ReplaySource::playbackEnded,   this, &ShotReplayController::playbackEnded,
            Qt::QueuedConnection);
}

ShotReplayController::~ShotReplayController() = default;

QVariantList ShotReplayController::streams() const
{
    QVariantList out;
    for (const ReplayStreamInfo &s : m_source->streams())
        out.append(QVariantMap{
            { QStringLiteral("index"),       s.index },
            { QStringLiteral("perspective"), s.perspective },
            { QStringLiteral("aspect"),      s.aspect },
            { QStringLiteral("hasAnalysis"), s.hasAnalysis } });
    return out;
}

bool ShotReplayController::start(int shotId, const QString &swingDir, double speed)
{
    if (swingDir.isEmpty())
        return false;

    const bool trim = m_appSettings && m_appSettings->replayTrimToSwing();
    if (m_source->load(swingDir, std::clamp(speed, 0.1, 1.0), trim)) {
        m_shotId   = shotId;
        m_swingDir = swingDir;
        m_active   = true;
        emit activeChanged();   // covers streams / analysisDetail / swingDir / shotId
        return true;
    }

    // Load failed. A valid doc with no playable stream leaves the source empty —
    // clear our active state so no zombie stage lingers. A bad path/doc leaves the
    // previous replay's streams intact (streamCount() > 0) — keep it running.
    if (m_active && m_source->streamCount() == 0) {
        m_active   = false;
        m_shotId   = -1;
        m_swingDir.clear();
        emit activeChanged();
    }
    return false;
}

void ShotReplayController::stop()
{
    if (!m_active)
        return;
    m_source->unload();
    m_active   = false;
    m_shotId   = -1;
    m_swingDir.clear();
    emit activeChanged();
}

void ShotReplayController::onAborted()
{
    // The source tore itself down (e.g. invalid master media). Mirror it.
    if (!m_active)
        return;
    m_active   = false;
    m_shotId   = -1;
    m_swingDir.clear();
    emit activeChanged();
}

QVariantMap ShotReplayController::shotContext(int sessionType) const
{
    // Face-on perspective value (swing.json setup.perspective); see disk_replay_source.
    constexpr int kPerspectiveFaceOn = 2;

    QVariantMap ctx;
    const QVariantMap d = m_source->analysisDetail();

    // The reconstruction tier the shot was analysed at (drives normative-band lookup).
    if (d.contains(QStringLiteral("tier")))
        ctx.insert(QStringLiteral("tier"), d.value(QStringLiteral("tier")).toInt());

    // A face-on camera: pose only runs on a face-on stream, so an analysisDetail
    // pose2d block implies one; also accept an explicit face-on stream.
    bool faceOn = d.contains(QStringLiteral("pose2d"));
    if (!faceOn) {
        for (const ReplayStreamInfo &s : m_source->streams())
            if (s.perspective == kPerspectiveFaceOn) { faceOn = true; break; }
    }
    ctx.insert(QStringLiteral("hasFaceOn"), faceOn);

    // Club / ball tracks — present AND valid in the analysis detail.
    ctx.insert(QStringLiteral("hasClubTrack"),
               d.value(QStringLiteral("club")).toMap().value(QStringLiteral("valid")).toBool());
    ctx.insert(QStringLiteral("hasBallTrack"),
               d.value(QStringLiteral("ball")).toMap().value(QStringLiteral("valid")).toBool());

    // IMU roles bound + calibrated this shot — from the persisted analysis.bindings[]
    // (present on the disk path; role names round-trip through roleFromName()).
    QVariantList roles;
    const QVariantList bindings = d.value(QStringLiteral("bindings")).toList();
    for (const QVariant &bv : bindings) {
        const int r = bv.toMap().value(QStringLiteral("role"), 0).toInt();
        const pinpoint::analysis::SegmentRole role =
            static_cast<pinpoint::analysis::SegmentRole>(r);
        if (role == pinpoint::analysis::SegmentRole::Unknown)
            continue;
        roles.append(pinpoint::analysis::segmentRoleName(role));
    }
    ctx.insert(QStringLiteral("imuRoles"), roles);

    // A launch monitor reported this shot. Read from the PRESENCE OF ITS METRICS rather
    // than from the top-level "launchMonitor" block, because that block is not part of
    // analysisDetail and this is the only document view we hold here. It is also the more
    // honest test: what makes the readings resolve is that they are in `metrics[]`, so
    // asking the same question the resolver will ask cannot disagree with it.
    // A HackMotion measured this shot's wrist — same test, same reason. `bindings[]`
    // above carries roles but not instruments, and a wG3's roles are indistinguishable
    // from a pair of Witmotions'; the `hm.` prefix is what says which. Asking the
    // question the resolver will ask keeps the two from disagreeing.
    bool hasLm = false, hasHm = false;
    for (const QVariant &sv : d.value(QStringLiteral("series")).toList()) {
        const QString key = sv.toMap().value(QStringLiteral("key")).toString();
        if (key.startsWith(QStringLiteral("lm."))) hasLm = true;
        if (key.startsWith(QStringLiteral("hm."))) hasHm = true;
        if (hasLm && hasHm) break;
    }
    ctx.insert(QStringLiteral("hasLaunchMonitor"), hasLm);
    ctx.insert(QStringLiteral("hasHackMotion"),    hasHm);

    ctx.insert(QStringLiteral("sessionType"), sessionType);
    // archetype/club/shape: not derivable from swing.json today → catalogue defaults.
    return ctx;
}
