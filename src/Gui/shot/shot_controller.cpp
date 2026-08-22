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

#include "shot_controller.h"

#ifdef HAVE_PPCP
#include "../../Ppcp/ppcp_shot_bridge.h"
#endif
#include "session_controller.h"
#include "pp_debug.h"

#include <QMetaEnum>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

namespace {
const char *sourceName(ShotController::Source s)
{
    return QMetaEnum::fromType<ShotController::Source>()
        .valueToKey(static_cast<int>(s));
}

// ShotController::Source ↔ the arbiter's modality set. Manual has no arbiter
// modality (it commits directly); Pose rides the vision slot with Ball.
bool toArbSource(ShotController::Source s, pinpoint::ArbSource &out)
{
    switch (s) {
    case ShotController::Source::Acoustic: out = pinpoint::ArbSource::Acoustic; return true;
    case ShotController::Source::Imu:      out = pinpoint::ArbSource::Imu;      return true;
    case ShotController::Source::Ball:
    case ShotController::Source::Pose:     out = pinpoint::ArbSource::Ball;     return true;
    case ShotController::Source::Manual:   break;
    }
    return false;
}

ShotController::Source fromArbSource(pinpoint::ArbSource a)
{
    switch (a) {
    case pinpoint::ArbSource::Acoustic: return ShotController::Source::Acoustic;
    case pinpoint::ArbSource::Imu:      return ShotController::Source::Imu;
    case pinpoint::ArbSource::Ball:     return ShotController::Source::Ball;
    }
    return ShotController::Source::Manual;
}
} // namespace

ShotController::ShotController(pinpoint::EventBuffer *buffer,
                               SessionController     *session,
                               QObject               *parent)
    : QObject(parent)
    , m_buffer(buffer)
    , m_session(session)
{
    if (m_buffer) {
        pinpoint::SourceDescriptor desc;
        desc.name       = "Shot Marker";
        desc.identifier = "shot_controller";   // singleton app-level source

        pinpoint::ImuFormat fmt{};             // fixed-size-packet descriptor
        fmt.device         = pinpoint::DeviceKind::Marker_App;
        fmt.sample_rate_hz = 2;                // sizes ring: ceil(2×5s)=10 → 16 slots
        fmt.packet_bytes   = sizeof(ShotMarker);
        fmt.packet_schema  = "shot_marker_v1";

        desc.format.device            = pinpoint::DeviceKind::Marker_App;
        desc.format.format            = fmt;
        desc.window_duration          = std::chrono::milliseconds(5000);
        desc.expected_interarrival_us = std::chrono::microseconds(0); // sporadic — no stall watchdog
        desc.sync_source              = pinpoint::SyncSource::SoftwareTimestamp;

        // registerSource requires Idle or Paused. Registering the first source
        // auto-resumes the buffer — main.cpp restores the user capture intent
        // via cameraManager.applyCaptureIntent() right after construction.
        if (m_buffer->state() == pinpoint::BufferState::Capturing)
            m_buffer->pause();
        m_sourceId = m_buffer->registerSource(desc);
    }

    m_arbTimer.setSingleShot(true);
    connect(&m_arbTimer, &QTimer::timeout, this, &ShotController::onArbHoldExpired);

    m_lastArmed = armed();
}

bool ShotController::armed() const
{
    return m_buffer && m_buffer->isCapturing() && !m_processorBusy && !m_reviewActive;
}

void ShotController::setProcessorBusy(bool busy)
{
    if (m_processorBusy == busy)
        return;
    m_processorBusy = busy;
    reevaluateArmed();
}

void ShotController::setReviewActive(bool active)
{
    if (m_reviewActive == active)
        return;
    m_reviewActive = active;
    reevaluateArmed();
}

void ShotController::reevaluateArmed()
{
    const bool now = armed();
    if (now == m_lastArmed)
        return;
    m_lastArmed = now;
    // A hold window opened while armed is void once disarmed (capture
    // stopped, processor busy, review entered) — drop it.
    if (!now) {
        m_arbTimer.stop();
        m_arbiter.cancel();
    }
    emit armedChanged();
}

void ShotController::triggerShot(Source source, qint64 timestampUs)
{
    if (!armed()) {
        ppDebug() << "[ShotController] trigger ignored (not capturing or busy) — source"
                  << sourceName(source);
        return;
    }

    // A direct trigger supersedes any pending hold window, and the arbiter
    // refractory must still cover subsequent auto candidates.
    m_arbTimer.stop();
    m_arbiter.cancel();
    m_arbiter.noteCommit(static_cast<int64_t>(pinpoint::EventBuffer::nowMicros()));

    commitShot(source, timestampUs);
}

void ShotController::reportCandidate(Source source, qint64 estImpactUs, float confidence)
{
    pinpoint::ArbSource arbSrc;
    if (!toArbSource(source, arbSrc)) {
        triggerShot(source, estImpactUs);   // Manual never holds
        return;
    }

#ifdef HAVE_PPCP
    // ⚠ THE REPLACEMENT, AND IT RETURNS.  See the note on setPpcpBridge() for
    // why running both arbiters would be worse than running either.
    if (m_ppcpBridge && m_ppcpBridge->active()) {
        if (!armed()) {
            ppDebug() << "[ShotController] candidate ignored (not capturing or busy) — source"
                      << sourceName(source);
            return;
        }
        const char *basis = nullptr;
        QString     srcId;
        switch (source) {
        case Source::Acoustic: basis = Ppcp::kBasisAcoustic; srcId = m_ppcpAcousticSourceId; break;
        case Source::Imu:      basis = Ppcp::kBasisMotion;   srcId = m_ppcpMotionSourceId;   break;
        // 5.12's registry is OPEN (10.3a), so `vision` round-trips at a peer
        // that has never heard of it rather than being fatal.  Ball launch and
        // pose are both optical and both this host's.
        case Source::Ball:
        case Source::Pose:     basis = Ppcp::kBasisVision;   srcId = m_ppcpVisionSourceId;   break;
        default:               break;
        }
        if (!basis || srcId.isEmpty()) {
            // I26 / 8.1e — this host declared no Source of that kind, and a
            // Candidate MUST name one it declared.  Attributing the detection
            // to some other Source, or synthesising one, is exactly what 8.1e
            // forbids; so it is dropped and said out loud.
            ppWarn() << "[ppcp] candidate NOT nominated — no declared Source for"
                     << sourceName(source) << "(I26)";
            return;
        }

        // `tb:host` IS EventBuffer::nowMicros(): both are the same
        // std::steady_clock reading, one in microseconds and one in
        // nanoseconds.  ppcp_live_session_test asserts that, because the day it
        // stops being true every arbitrated t0 is wrong by an unbounded amount
        // with nothing red.
        //
        // ⚠ `exposureNs` is 0 and `tof` is null, and both are honest gaps.
        // 6.1d fixes `convention: mid` for a Source with no `format` — a
        // microphone, an IMU — so the exposure is ignored there and 0 is
        // correct.  8.1d asks an acoustic nominator to correct for time of
        // flight and REPORT the correction; this host's detectors do not
        // measure their distance to the ball, so there is no correction to
        // report and none is invented.  See the conformance note.
        std::string err;
        if (!m_ppcpBridge->nominate(srcId.toStdString(), basis,
                                    static_cast<std::int64_t>(estImpactUs) * 1000,
                                    /*exposureNs=*/0, static_cast<double>(confidence),
                                    /*tof=*/nullptr, nullptr, &err)) {
            ppWarn() << "[ppcp] nomination refused —" << QString::fromStdString(err);
        }
        // ⚠ AND NOTHING BELOW THIS LINE RUNS.  m_arbiter is not told, and no
        // hold timer is started: 8.2h's issue hold is the arbiter's and is
        // driven by PpcpHostPeer::tick(), not by a QTimer here.
        return;
    }
#endif

    if (!armed()) {
        ppDebug() << "[ShotController] candidate ignored (not capturing or busy) — source"
                  << sourceName(source);
        return;
    }

    const auto nowUs =
        static_cast<int64_t>(pinpoint::EventBuffer::nowMicros());
    const bool opened = m_arbiter.report(
        {arbSrc, static_cast<int64_t>(estImpactUs), confidence}, nowUs);

    ppDebug() << "[ShotController] candidate — source" << sourceName(source)
              << "est_t_us" << estImpactUs << "conf" << confidence
              << (opened ? "(hold window opened)" : "(joined window)");

    if (opened)
        m_arbTimer.start(m_arbiter.config().holdMs);
}

void ShotController::onArbHoldExpired()
{
    const auto nowUs =
        static_cast<int64_t>(pinpoint::EventBuffer::nowMicros());
    const pinpoint::ShotArbiter::Decision d = m_arbiter.decide(nowUs);
    if (!d.commit) {
        ppDebug() << "[ShotController] hold window expired — no commit";
        return;
    }

    ppInfo() << "[ShotController] arbiter commit —" << d.modalities
             << "modalit" << (d.modalities == 1 ? "y" : "ies")
             << "authoritative" << sourceName(fromArbSource(d.src))
             << "conf" << d.conf;
    commitShot(fromArbSource(d.src), d.t_us);
}

#ifdef HAVE_PPCP
void ShotController::setPpcpBridge(Ppcp::PpcpShotBridge *bridge)
{
    m_ppcpBridge = bridge;
    if (bridge) {
        // Any hold window the local arbiter had open belongs to an arbitration
        // that is no longer running.  Cancelling rather than letting it expire
        // stops a stale decide() committing a shot the PPCP arbiter is about to
        // issue properly.
        m_arbTimer.stop();
        m_arbiter.cancel();
    }
}

void ShotController::setPpcpSourceIds(const QString &acoustic, const QString &motion,
                                      const QString &vision)
{
    m_ppcpAcousticSourceId = acoustic;
    m_ppcpMotionSourceId   = motion;
    m_ppcpVisionSourceId   = vision;
}

void ShotController::commitArbitratedShot(qint64 t0HostNs)
{
    // I7 — `t0` is what the arbiter decided and is never revised, so this
    // converts units and does nothing else.  The rounding is to the nearest
    // microsecond because the event buffer's index is microseconds; it is a
    // truncation of PRECISION at the local store, not a correction of the
    // instant, and the Shot on the wire keeps its nanoseconds.
    const qint64 tUs = t0HostNs / 1000;
    ppInfo() << "[ppcp] arbitrated shot — t0_us" << tUs;
    // Reported as Acoustic because commitShot's `Source` is a LOCAL label for
    // which of this application's detectors fired, and a PPCP Shot may have
    // been decided by a Candidate from another peer entirely.  It is a display
    // and marker label, not an authority claim; the authority is `Shot.issued_by`
    // and lives on the wire.
    commitShot(Source::Acoustic, tUs);
}
#endif

void ShotController::commitShot(Source source, qint64 timestampUs)
{
    if (!armed()) {
        ppDebug() << "[ShotController] commit ignored (not capturing or busy) — source"
                  << sourceName(source);
        return;
    }

    const qint64 impactUs = timestampUs >= 0 ? timestampUs
                                             : pinpoint::EventBuffer::nowMicros();

    // Session type captured at the moment of the shot (-1 = no active session).
    const int sessionType = m_session ? m_session->activeSessionType() : -1;

    // Marker must be in the ring before the signal — the shot processor will
    // pause/freeze the buffer from shotDetected.
    writeShotMarker(source, impactUs, sessionType);

    ppInfo() << "[ShotController] shot detected — source" << sourceName(source)
             << "impact_ts_us" << impactUs << "sessionType" << sessionType;
    emit shotDetected(source, impactUs, sessionType);
}

void ShotController::writeShotMarker(Source source, int64_t impactUs, int sessionType)
{
    if (!m_buffer || m_sourceId == pinpoint::kInvalidSourceId)
        return;

    auto slot = m_buffer->acquireWriteSlot(m_sourceId);
    if (!slot.valid || slot.capacity < sizeof(ShotMarker)) {
        ppWarn() << "[ShotController] shot marker dropped — no valid write slot";
        return;
    }

    const ShotMarker marker{1, static_cast<uint16_t>(source),
                            static_cast<int16_t>(sessionType), impactUs};
    std::memcpy(slot.data, &marker, sizeof marker);
    *slot.bytes_written = sizeof(ShotMarker);
    *slot.timestamp_us  = impactUs;            // entry timestamp IS the impact instant
    m_buffer->publish(m_sourceId, slot.sequence);
}
