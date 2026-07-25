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

#include "disk_replay_source.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QTimer>
#include <QUrl>
#include <QVideoSink>
#include <algorithm>
#include <cmath>
#include <limits>

#include "../Core/pp_debug.h"

namespace {

// Slave players are nudged back onto the master clock only when they drift past
// this; below it, leaving them free-running keeps decode smooth.
constexpr qint64 kSlaveResyncMs = 120;

constexpr int kPerspectiveFaceOn = 2;   // CameraInstance::FaceOn

// Window-relative µs from an analysis t_us. Live captures write ABSOLUTE
// (t0-based) analysis t_us, but re-analysed swings write WINDOW-RELATIVE ones
// (the reconstructed window is 0-based). Subtract t0 only in the absolute domain
// (value ≥ t0); pass already-relative values (≪ t0) through unchanged.
qint64 relUs(const QJsonValue &v, qint64 t0)
{
    const qint64 t = static_cast<qint64>(v.toDouble());
    return t >= t0 ? t - t0 : t;
}

// Copy a swing.json analysis sub-object (pose frame / club sample) to a QVariantMap
// with its t_us shifted into the window-relative domain. Preserves every other
// field (kp/lead/trail/handConf, or grip/head/theta/…) verbatim.
QVariantMap relTimedMap(const QJsonObject &o, qint64 t0)
{
    QVariantMap m = o.toVariantMap();
    m[QStringLiteral("t_us")] = static_cast<qlonglong>(relUs(o[QStringLiteral("t_us")], t0));
    return m;
}

} // namespace

std::unique_ptr<ReplaySource> makeDiskReplaySource()
{
    return std::make_unique<DiskReplaySource>();
}

DiskReplaySource::DiskReplaySource(QObject *parent)
    : ReplaySource(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(33);   // ~30 Hz playhead poll (reads, never seeks)
    connect(m_timer, &QTimer::timeout, this, &DiskReplaySource::onTick);
}

DiskReplaySource::~DiskReplaySource()
{
    unload();
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool DiskReplaySource::load(const QString &swingDir, double speed, bool trimToSwing)
{
    if (swingDir.isEmpty())
        return false;

    QFile f(swingDir + QStringLiteral("/swing.json"));
    if (!f.open(QIODevice::ReadOnly)) {
        ppWarn() << "[ShotReplay] cannot open" << f.fileName();
        return false;   // bad path: leave any current replay intact
    }
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.isEmpty())
        return false;

    // NOTE: previous replay is NOT torn down here. Destroying/recreating the
    // QMediaPlayers per reload synchronously joined the old decode threads on the
    // UI thread (≈2–4 s) and made the new open contend with that teardown. The
    // players are reused below (just setSource); see the player-pool section.

    const qint64 t0 = static_cast<qint64>(
        root[QStringLiteral("clock")].toObject()[QStringLiteral("t0_us")].toDouble());

    // ── Video streams (window-relative frame tables + per-stream metadata) ──
    qint64 spanStart = std::numeric_limits<qint64>::max();
    qint64 spanEnd   = std::numeric_limits<qint64>::min();
    struct PendingStream {
        QString file; double fps; std::vector<int64_t> tUs;
        int perspective; double aspect;
    };
    std::vector<PendingStream> pending;

    for (const QJsonValue &sv : root[QStringLiteral("streams")].toArray()) {
        const QJsonObject s = sv.toObject();
        if (s[QStringLiteral("kind")].toString() != QLatin1String("video"))
            continue;
        const QString file = s[QStringLiteral("file")].toString();
        if (file.isEmpty() || !QFile::exists(swingDir + QStringLiteral("/") + file))
            continue;

        PendingStream ps;
        ps.file = file;
        ps.fps  = s[QStringLiteral("playback")].toObject()
                   .value(QStringLiteral("fps")).toDouble(30.0);
        if (ps.fps <= 0.0) ps.fps = 30.0;
        for (const QJsonValue &t : s[QStringLiteral("frames")].toObject()
                                    [QStringLiteral("t_us")].toArray())
            ps.tUs.push_back(static_cast<int64_t>(t.toDouble()));
        if (ps.tUs.empty())
            continue;

        // Per-stream metadata from the SWING's own doc (cross-machine safe).
        ps.perspective = s[QStringLiteral("setup")].toObject()
                          .value(QStringLiteral("perspective")).toInt(-1);
        const QJsonObject src = s[QStringLiteral("source")].toObject();
        const double sw = src.value(QStringLiteral("width")).toDouble();
        const double sh = src.value(QStringLiteral("height")).toDouble();
        ps.aspect = (sw > 0.0 && sh > 0.0) ? sw / sh : 16.0 / 9.0;

        spanStart = std::min<qint64>(spanStart, ps.tUs.front());
        spanEnd   = std::max<qint64>(spanEnd,   ps.tUs.back());
        pending.push_back(std::move(ps));
    }

    if (pending.empty()) {
        ppWarn() << "[ShotReplay] no playable video stream in" << swingDir;
        unload();        // fully clear → streamCount() == 0 (controller clears active)
        return false;
    }

    m_speed      = std::clamp(speed, 0.1, 1.0);
    m_startUs    = spanStart;
    m_endUs      = std::max(spanEnd, spanStart + 1);
    m_positionUs = m_startUs;

    // ── Analysis detail, offset to the window-relative (0-based) domain ─────
    m_analysisDetail = QVariantMap{};
    m_impactUs = -1;
    qint64 addressUs = -1, finishUs = -1;   // Phase::Address / Phase::Finish, for the trim
    bool hasAnalysis = false;
    if (root.contains(QStringLiteral("analysis"))) {
        const QJsonObject an = root[QStringLiteral("analysis")].toObject();
        QVariantList series;
        for (const QJsonValue &mv : an[QStringLiteral("metrics")].toArray()) {
            const QJsonObject m = mv.toObject();
            QVariantList ts, vs, samples;
            for (const QJsonValue &t : m[QStringLiteral("t_us")].toArray())
                ts.append(static_cast<qlonglong>(relUs(t, t0)));
            for (const QJsonValue &v : m[QStringLiteral("value")].toArray())
                vs.append(v.toDouble());
            for (const QJsonValue &sv2 : m[QStringLiteral("phaseSamples")].toArray()) {
                const QJsonObject s2 = sv2.toObject();
                samples.append(QVariantMap{
                    { QStringLiteral("phase"), s2[QStringLiteral("phase")].toInt() },
                    { QStringLiteral("t_us"),  static_cast<qlonglong>(relUs(s2[QStringLiteral("t_us")], t0)) },
                    { QStringLiteral("value"), s2[QStringLiteral("value")].toDouble() },
                    { QStringLiteral("band"),  s2[QStringLiteral("band")].toString() } });
            }
            series.append(QVariantMap{
                { QStringLiteral("key"),   m[QStringLiteral("key")].toString() },
                { QStringLiteral("label"), m[QStringLiteral("label")].toString() },
                { QStringLiteral("unit"),  m[QStringLiteral("unit")].toString() },
                { QStringLiteral("t_us"),  ts },
                { QStringLiteral("value"), vs },
                { QStringLiteral("phaseSamples"), samples } });
        }
        QVariantList phases;
        for (const QJsonValue &pv : an[QStringLiteral("phases")].toArray()) {
            const QJsonObject p = pv.toObject();
            const int phase = p[QStringLiteral("phase")].toInt();
            const qint64 tr = relUs(p[QStringLiteral("t_us")], t0);
            if (phase == 0)        // Phase::Address
                addressUs = tr;
            else if (phase == 5)   // Phase::Impact
                m_impactUs = tr;
            else if (phase == 7)   // Phase::Finish
                finishUs = tr;
            phases.append(QVariantMap{
                { QStringLiteral("phase"), phase },
                { QStringLiteral("t_us"),  static_cast<qlonglong>(tr) },
                { QStringLiteral("conf"),  p[QStringLiteral("conf")].toDouble() } });
        }
        m_analysisDetail = QVariantMap{
            { QStringLiteral("tier"),    an[QStringLiteral("tier")].toInt() },
            { QStringLiteral("overall"), an[QStringLiteral("score")].toInt() },
            { QStringLiteral("series"),  series },
            { QStringLiteral("phases"),  phases } };
        hasAnalysis = !series.isEmpty();

        // ── Replay-overlay detail (skeleton + club), same shapes ShotProcessor
        //    produces live (shot_processor.cpp toAnalysisDetail). Frame/sample
        //    t_us are offset to the window-relative domain so they scrub with the
        //    playhead. Club is a stub today (rarely present) — degrades to none.
        if (an.contains(QStringLiteral("pose2d"))) {
            const QJsonObject p2 = an[QStringLiteral("pose2d")].toObject();
            QVariantList frames;
            for (const QJsonValue &fv : p2[QStringLiteral("frames")].toArray())
                frames.append(relTimedMap(fv.toObject(), t0));
            QVariantMap pose2d{ { QStringLiteral("camera"), p2[QStringLiteral("camera")].toInt() },
                                { QStringLiteral("frames"), frames } };
            // Motion-overlay smoothed companion track (pose_smoother.cpp): re-time
            // each frame's t_us into the window-relative domain EXACTLY like `frames`
            // (relTimedMap re-times only the top-level t_us; kp/tier/sigma pass through
            // verbatim) so the replay playhead indexes it in the same domain. Present
            // only when the swing was analysed with the smoother.
            if (p2.contains(QStringLiteral("smoothed"))) {
                QVariantList smoothed;
                for (const QJsonValue &sv2 : p2[QStringLiteral("smoothed")].toArray())
                    smoothed.append(relTimedMap(sv2.toObject(), t0));
                pose2d.insert(QStringLiteral("smoothed"), smoothed);
            }
            m_analysisDetail.insert(QStringLiteral("pose2d"), pose2d);
        }
        if (an.contains(QStringLiteral("club"))) {
            const QJsonObject cb = an[QStringLiteral("club")].toObject();
            QVariantMap club = cb.toVariantMap();
            QVariantList samples;
            for (const QJsonValue &sv2 : cb[QStringLiteral("samples")].toArray())
                samples.append(relTimedMap(sv2.toObject(), t0));
            club[QStringLiteral("samples")] = samples;
            // Layer C synthesized series (shaft_position_first §2 Layer C): re-time
            // exactly like `samples` so the replay overlay's window-relative playhead
            // indexes it (present only when synthesis was on).
            if (cb.contains(QStringLiteral("synth"))) {
                QVariantList synth;
                for (const QJsonValue &sv2 : cb[QStringLiteral("synth")].toArray())
                    synth.append(relTimedMap(sv2.toObject(), t0));
                club[QStringLiteral("synth")] = synth;
            }
            // Named P1..P8 coaching positions (fused TrackSample/MilestoneFit — see
            // shaft_position_first): re-time exactly like samples/synth so the Transit
            // timeline's P-tick layer indexes them in the same window-relative domain
            // as the phase stations. The live path (shot_processor toAnalysisDetail)
            // already writes these absolute-consistent, so only the disk path needs it.
            if (cb.contains(QStringLiteral("positions"))) {
                QVariantList positions;
                for (const QJsonValue &sv2 : cb[QStringLiteral("positions")].toArray())
                    positions.append(relTimedMap(sv2.toObject(), t0));
                club[QStringLiteral("positions")] = positions;
            }
            m_analysisDetail.insert(QStringLiteral("club"), club);
        }
        if (an.contains(QStringLiteral("ball"))) {
            const QJsonObject bb = an[QStringLiteral("ball")].toObject();
            QVariantMap ball = bb.toVariantMap();
            QVariantList samples;
            for (const QJsonValue &sv2 : bb[QStringLiteral("samples")].toArray())
                samples.append(relTimedMap(sv2.toObject(), t0));
            ball[QStringLiteral("samples")] = samples;
            m_analysisDetail.insert(QStringLiteral("ball"), ball);
        }
    }
    if (m_impactUs < 0) {
        const QJsonObject thumb = root[QStringLiteral("thumbnail")].toObject();
        m_impactUs = thumb.contains(QStringLiteral("t_us"))
                         ? static_cast<qint64>(thumb[QStringLiteral("t_us")].toDouble())
                         : (m_startUs + m_endUs) / 2;
    }

    // ── Playback trim (Address → Finish) ────────────────────────────────────
    // Default to the full span; only narrow it when trimming is requested AND the
    // swing carries a usable Address/Finish pair. The scrub range [m_startUs,
    // m_endUs] is untouched — this only moves where auto-play starts and stops.
    m_playStartUs = m_startUs;
    m_playEndUs   = m_endUs;
    if (trimToSwing && addressUs >= 0 && finishUs > addressUs) {
        const qint64 lo = std::clamp(addressUs, m_startUs, m_endUs);
        const qint64 hi = std::clamp(finishUs,  m_startUs, m_endUs);
        if (hi > lo) {
            m_playStartUs = lo;
            m_playEndUs   = hi;
        }
    }
    m_endedAtTrim = false;
    // Seek to the trim start once media is ready (a pre-load setPosition is dropped
    // by the FFmpeg backend). Skip when playback starts at the span start anyway.
    m_pendingStartSeekUs = (m_playStartUs > m_startUs) ? m_playStartUs : -1;

    // ── Players + per-stream metadata. The player pool is REUSED across reloads:
    //    each MP4 swap is just setSource() on a persistent QMediaPlayer, which the
    //    backend handles asynchronously — load() stays near-instant. (Recreating
    //    players per reload joined the previous decode threads on the UI thread,
    //    ≈2–4 s, and starved the new open.) Signals are wired once, on creation;
    //    the per-stream file/fps/frame-table are refreshed every load. Each MP4 is
    //    30 fps; applyPlaybackRates() picks a per-stream rate so the whole window
    //    replays over the same wall time → a fixed capture-time speed. ───────────
    m_streamInfo.clear();

    // Retire pool players beyond the new stream count (rare — only a shot with
    // fewer cameras). deleteLater keeps the join off this call.
    while (m_streams.size() > pending.size()) {
        Stream &dead = m_streams.back();
        if (dead.player) {
            dead.player->setVideoSink(nullptr);
            dead.player->deleteLater();
        }
        m_streams.pop_back();
    }

    for (size_t i = 0; i < pending.size(); ++i) {
        const bool faceOn = pending[i].perspective == kPerspectiveFaceOn;
        m_streamInfo.append(ReplayStreamInfo{
            int(i), pending[i].perspective, pending[i].aspect,
            faceOn && hasAnalysis });

        if (i >= m_streams.size())
            m_streams.push_back(Stream{});
        Stream &st = m_streams[i];
        st.tUs         = std::move(pending[i].tUs);
        st.playbackFps = pending[i].fps;
        st.file        = pending[i].file;

        if (!st.player) {
            st.player = new QMediaPlayer(this);
            const int idx = int(i);

            // Surface decode failures instead of silently rendering black. The
            // file is read from the (index-stable) stream slot so the handler
            // stays correct after a source swap.
            connect(st.player, &QMediaPlayer::errorOccurred, this,
                    [this, idx](QMediaPlayer::Error err, const QString &msg) {
                        if (err == QMediaPlayer::NoError)
                            return;
                        const QString f = idx < int(m_streams.size()) ? m_streams[idx].file : QString();
                        ppWarn() << "[ShotReplay] media error for" << f << ":" << msg;
                        emit failed(msg.isEmpty() ? f : msg);
                    });

            if (idx == 0)
                connect(st.player, &QMediaPlayer::mediaStatusChanged, this,
                        [this](QMediaPlayer::MediaStatus s) {
                            if ((s == QMediaPlayer::LoadedMedia || s == QMediaPlayer::BufferedMedia)
                                && m_pendingStartSeekUs >= 0) {
                                // Apply the trim-start seek now the backend accepts it.
                                seekPlayersTo(m_pendingStartSeekUs);
                                setPositionUs(m_pendingStartSeekUs);
                                m_pendingStartSeekUs = -1;
                            }
                            if (s == QMediaPlayer::EndOfMedia) {
                                setPlaying(false);
                                emit playbackEnded();   // natural end, not a user pause
                            } else if (s == QMediaPlayer::InvalidMedia) {
                                ppWarn() << "[ShotReplay] master stream is invalid media — cannot replay";
                                emit failed(QStringLiteral("unsupported or corrupt video"));
                                // Defer teardown out of the player's own callback.
                                QMetaObject::invokeMethod(this, [this] { unload(); emit aborted(); },
                                                          Qt::QueuedConnection);
                            }
                        });
        }

        if (int(i) < m_sinks.size() && m_sinks[int(i)])
            st.player->setVideoSink(m_sinks[int(i)]);

        st.player->setSource(QUrl::fromLocalFile(swingDir + QStringLiteral("/") + pending[i].file));
    }
    applyPlaybackRates();
    for (Stream &st : m_streams)
        st.player->play();

    m_loaded = true;
    setPositionUs(m_playStartUs);
    emit spanChanged();
    emit speedChanged();   // load() may have changed m_speed
    setPlaying(true);
    m_timer->start();

    ppInfo() << "[ShotReplay] replaying" << swingDir << "—" << int(m_streams.size())
             << "stream(s), window" << (m_endUs - m_startUs) / 1000 << "ms";
    return true;
}

void DiskReplaySource::unload()
{
    m_timer->stop();
    for (Stream &s : m_streams) {
        if (s.player) {
            s.player->stop();
            s.player->setSource(QUrl());
            s.player->deleteLater();
        }
    }
    m_streams.clear();
    m_streamInfo.clear();
    m_analysisDetail = QVariantMap{};
    m_impactUs   = -1;
    m_startUs    = 0;
    m_endUs      = 0;
    m_positionUs = 0;
    m_playStartUs = 0;
    m_playEndUs   = 0;
    m_pendingStartSeekUs = -1;
    m_endedAtTrim = false;
    m_loaded     = false;
    setPlaying(false);
    emit spanChanged();
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void DiskReplaySource::togglePlay()
{
    if (!m_loaded || m_streams.empty())
        return;
    const bool atEnd = m_positionUs >= m_playEndUs - 1;
    if (atEnd) {
        // Restart the trimmed window from its Address start.
        m_endedAtTrim = false;
        seekPlayersTo(m_playStartUs);
        setPositionUs(m_playStartUs);
        for (Stream &s : m_streams) s.player->play();
        setPlaying(true);
        m_timer->start();
    } else if (m_playing) {
        for (Stream &s : m_streams) s.player->pause();
        setPlaying(false);
    } else {
        for (Stream &s : m_streams) s.player->play();
        setPlaying(true);
        m_timer->start();
    }
}

void DiskReplaySource::seekToFraction(double frac)
{
    seekToUs(m_startUs + static_cast<qint64>(std::clamp(frac, 0.0, 1.0) * (m_endUs - m_startUs)));
}

void DiskReplaySource::seekToUs(qint64 us)
{
    if (!m_loaded)
        return;
    us = std::clamp(us, m_startUs, m_endUs);
    seekPlayersTo(us);
    setPositionUs(us);
}

void DiskReplaySource::stepFrame(int delta)
{
    if (!m_loaded || m_streams.empty() || delta == 0)
        return;
    // Pause first — frame-stepping is a deliberate, settled action.
    if (m_playing) {
        for (Stream &s : m_streams) s.player->pause();
        setPlaying(false);
    }
    const Stream &s = m_streams.front();
    if (s.tUs.empty())
        return;
    // Locate the current frame, then offset by delta and seek to its capture µs.
    long idx = std::lround(double(s.player->position()) / 1000.0 * s.playbackFps);
    idx = std::clamp<long>(idx + delta, 0, long(s.tUs.size()) - 1);
    seekToUs(s.tUs[idx]);
}

void DiskReplaySource::setSpeed(double speed)
{
    speed = std::clamp(speed, 0.1, 1.0);
    if (qFuzzyCompare(speed, m_speed))
        return;
    m_speed = speed;
    applyPlaybackRates();   // live players pick the new rate up immediately
    emit speedChanged();
}

void DiskReplaySource::beginScrub()
{
    if (!m_loaded)
        return;
    m_wasPlayingBeforeScrub = m_playing;
    if (m_playing) {
        for (Stream &s : m_streams) s.player->pause();
        setPlaying(false);
    }
}

void DiskReplaySource::endScrub()
{
    if (!m_loaded)
        return;
    if (m_wasPlayingBeforeScrub && m_positionUs < m_playEndUs - 1) {
        m_endedAtTrim = false;   // scrubbing back inside the window re-arms the trim stop
        for (Stream &s : m_streams) s.player->play();
        setPlaying(true);
    }
    m_wasPlayingBeforeScrub = false;
}

void DiskReplaySource::setVideoSink(int index, QVideoSink *sink)
{
    if (index < 0)
        return;
    if (index >= m_sinks.size())
        m_sinks.resize(index + 1);
    m_sinks[index] = sink;
    if (index < int(m_streams.size()) && m_streams[index].player)
        m_streams[index].player->setVideoSink(sink);
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

void DiskReplaySource::onTick()
{
    if (!m_loaded || m_streams.empty())
        return;

    QMediaPlayer *master = m_streams.front().player;
    const qint64 captureUs = captureUsForStream(0, master->position());
    setPositionUs(captureUs);

    // Trim-end auto-stop: only when the play window is genuinely narrower than the
    // full span (the untrimmed case still ends naturally on the master's
    // EndOfMedia, preserving prior behaviour). Fires playbackEnded once.
    if (m_playing && !m_endedAtTrim && m_playEndUs < m_endUs && captureUs >= m_playEndUs) {
        m_endedAtTrim = true;
        for (Stream &s : m_streams) s.player->pause();
        setPlaying(false);
        emit playbackEnded();   // trimmed end — same signal the host acts on
        m_timer->stop();
        return;
    }

    // Keep slave cameras on the master's capture clock (R4); cheap no-op for one.
    for (size_t i = 1; i < m_streams.size(); ++i) {
        const qint64 target = mp4MsForStream(int(i), captureUs);
        if (std::llabs(m_streams[i].player->position() - target) > kSlaveResyncMs)
            m_streams[i].player->setPosition(target);
    }

    if (!m_playing)
        m_timer->stop();   // settled (paused / ended) — idle until the next action
}

qint64 DiskReplaySource::captureUsForStream(int streamIdx, qint64 mp4Ms) const
{
    const Stream &s = m_streams[streamIdx];
    if (s.tUs.empty())
        return m_startUs;
    long idx = std::lround(double(mp4Ms) / 1000.0 * s.playbackFps);
    idx = std::clamp<long>(idx, 0, long(s.tUs.size()) - 1);
    return s.tUs[idx];
}

qint64 DiskReplaySource::mp4MsForStream(int streamIdx, qint64 captureUs) const
{
    const Stream &s = m_streams[streamIdx];
    if (s.tUs.empty())
        return 0;
    // Nearest frame to captureUs.
    auto it = std::lower_bound(s.tUs.begin(), s.tUs.end(), captureUs);
    long idx;
    if (it == s.tUs.begin())      idx = 0;
    else if (it == s.tUs.end())   idx = long(s.tUs.size()) - 1;
    else {
        const long hi = long(it - s.tUs.begin());
        idx = (captureUs - s.tUs[hi - 1] <= s.tUs[hi] - captureUs) ? hi - 1 : hi;
    }
    return std::lround(double(idx) / s.playbackFps * 1000.0);
}

void DiskReplaySource::seekPlayersTo(qint64 captureUs)
{
    for (size_t i = 0; i < m_streams.size(); ++i) {
        QMediaPlayer *p = m_streams[i].player;
        // A stopped player (clip ran to its end) won't render a seek — pause it
        // first so scrubbing while not playing still updates the frame.
        if (p->playbackState() == QMediaPlayer::StoppedState)
            p->pause();
        p->setPosition(mp4MsForStream(int(i), captureUs));
    }
}

void DiskReplaySource::applyPlaybackRates()
{
    // Each MP4 is 30 fps, so its native duration is frameCount/fps. Scale every
    // stream so the whole window replays over the same wall time → m_speed ×
    // capture speed (1.0 = real time), fps-independent, all finishing together.
    const double desiredWallSec = std::max(0.001, double(m_endUs - m_startUs) / 1e6 / m_speed);
    for (Stream &s : m_streams) {
        const double nativeWallSec = double(s.tUs.size()) / s.playbackFps;
        s.player->setPlaybackRate(std::clamp(nativeWallSec / desiredWallSec, 0.05, 8.0));
    }
}

void DiskReplaySource::setPlaying(bool p)
{
    if (m_playing == p)
        return;
    m_playing = p;
    if (p && !m_timer->isActive())
        m_timer->start();
    emit playingChanged();
}

void DiskReplaySource::setPositionUs(qint64 us)
{
    us = std::clamp(us, m_startUs, m_endUs);
    if (us == m_positionUs)
        return;
    m_positionUs = us;
    emit positionChanged();
}
