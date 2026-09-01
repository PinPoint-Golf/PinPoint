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
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
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
    // Neither has a local modality: Manual commits directly, and a PPCP Shot
    // has ALREADY been arbitrated by the time it is seen here — feeding it to
    // the local arbiter would be the second arbiter this design exists to
    // prevent.
    case ShotController::Source::Manual:
    case ShotController::Source::Ppcp:     break;
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

ShotController::~ShotController()
{
    // ⚠ NOTHING TO UNDO, AND THAT IS THE FIX.  This used to clear the
    // corroboration callback off the live bridge, which meant dereferencing a
    // raw pointer at a moment when nothing guaranteed it was still valid.  The
    // callback now holds a QPointer to this object and answers harmlessly once
    // it is gone, so there is no teardown obligation left to get wrong.
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
    // Recorded BEFORE any gate, and before the nomination that may be refused:
    // what this host's own detectors saw is evidence for the corroboration rule
    // whether or not it can be put on the wire.  See the note on m_detections.
    noteDetection(source, estImpactUs);

    // ⚠ 8.2d1 / E29 — AND THIS IS WHAT MAKES THE CORROBORATION POLICY WORK.
    // A device Candidate that arrived before this detector fired was excluded
    // for want of evidence and RETAINED; the evidence has just arrived, so ask
    // again.  Without this line the policy would be decided purely by which
    // packet won a race, and a phone that detects a millisecond faster than our
    // microphone — which it will, routinely — would never be believed.
    if (m_ppcpBridge && m_ppcpBridge->active()) {
        const std::size_t back = m_ppcpBridge->reconsider();
        if (back)
            ppDebug() << "[ppcp] reconsidered" << back
                      << "candidate(s) after a" << sourceName(source) << "detection";
    }

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
    // ⚠ THE OUTGOING BRIDGE IS NOT TOUCHED, AND THAT IS DELIBERATE.  This
    // function used to clear the old bridge's callback here, "belt and braces".
    // It crashed on a range on 27 Aug: `dropPhone()` destroyed the Phone — and
    // the bridge inside it — before announcing the change, so the pointer this
    // object still held was already freed and clearing through it read reused
    // memory.  Two things now make that impossible rather than unlikely: the
    // service announces the change BEFORE it erases the phone, and the callback
    // below holds a QPointer rather than a bare `this`.  Reaching for the
    // outgoing pointer at all is what the crash was, so nothing here does.
    m_ppcpBridge = bridge;
    if (bridge) {
        // ── 8.2d as corroboration, decided BEFORE a Shot exists ────────────
        //
        // The same rule `commitArbitratedShot()` applies, moved to the only
        // place where refusing costs nothing: a Candidate excluded here never
        // sets `t0`, and a group in which every Candidate is excluded issues NO
        // SHOT.  So an uncorroborated device detection produces silence on the
        // wire instead of a Shot we then decline to record — and the device's
        // own 8.2i deadline lets it mint on its own authority, which is the
        // honest ending to "it saw the strike and we did not".
        //
        // ⚠ THE GATE IN commitArbitratedShot() STAYS, AND IS NOW A MEASUREMENT.
        // If it ever refuses a Shot on corroboration grounds, this policy
        // failed to catch it first — which is precisely the number the change
        // request against 8.2d needs, so it is worth more running than removed.
        // ⚠ A QPointer, NOT `this`.  The bridge lives inside a phone owned by a
        // service that outlives main()'s stack, so a captured raw `this` is a
        // dangling call waiting for a shutdown ordering to change.  A destroyed
        // ShotController answers `true` — no host left to weigh the Candidate,
        // which is §2.1's "no detector available" case and the honest answer.
        QPointer<ShotController> self(this);
        bridge->setCorroborationCallback([self](std::int64_t atRefNs) {
            if (!self) return true;
            QString      why;
            const bool   ok = self->corroborated(atRefNs / 1000, &why);
            if (!ok)
                ppDebug() << "[ppcp] candidate EXCLUDED (8.2d, uncorroborated) — t0_us"
                          << (atRefNs / 1000) << "—" << why;
            return ok;
        });
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

void ShotController::setDetectorAvailable(Source source, bool available)
{
    if (available) m_availableDetectors.insert(static_cast<int>(source));
    else           m_availableDetectors.remove(static_cast<int>(source));
}

void ShotController::injectDetection(Source source, qint64 tUs)
{
#ifdef PP_SHIPPING_BUILD
    Q_UNUSED(source); Q_UNUSED(tUs);
#else
    // -1 means "now", which is what a test driving a phone alongside this host
    // usually wants: both instants land inside the 50 ms window by construction.
    const qint64 t = tUs >= 0 ? tUs : static_cast<qint64>(pinpoint::EventBuffer::nowMicros());
    ++m_counters.injected;
    noteDetection(source, t);
    // ⚠ SAID OUT LOUD, EVERY TIME, AT ppWarn.  An injected detection is
    // indistinguishable downstream from one a detector made, which is the whole
    // point and also the danger: a log that did not name it would let a test
    // rig's evidence be read later as a measurement.
    ppWarn() << "[ppcp] INJECTED detection —" << sourceName(source) << "at t_us" << t
             << "(test only; no Candidate is put on the wire)";
    // 8.2d1 — the same reconsideration a real detection triggers, or the
    // injection would corroborate nothing already excluded.
    if (m_ppcpBridge && m_ppcpBridge->active()) m_ppcpBridge->reconsider();
#endif
}

QVariantMap ShotController::shotStats() const
{
    QVariantMap m;
    m[QStringLiteral("detections")]      = m_counters.detections;
    m[QStringLiteral("injected")]        = m_counters.injected;
    m[QStringLiteral("arbitratedSeen")]  = m_counters.arbitratedSeen;
    m[QStringLiteral("corroboratePass")] = m_counters.corroboratePass;
    m[QStringLiteral("corroborateFail")] = m_counters.corroborateFail;
    m[QStringLiteral("droppedBusy")]     = m_counters.droppedBusy;
    m[QStringLiteral("committed")]       = m_counters.committed;
    // The per-detector deltas behind the last decision — the thing the debug
    // log carries and a release build does not.
    m[QStringLiteral("lastVerdict")]     = m_lastVerdict;
    m[QStringLiteral("armed")]           = armed();
    QStringList avail;
    for (int raw : m_availableDetectors) avail << QLatin1String(sourceName(static_cast<Source>(raw)));
    avail.sort();
    m[QStringLiteral("availableDetectors")] = avail;
#ifdef HAVE_PPCP
    m[QStringLiteral("bridgeActive")] = (m_ppcpBridge && m_ppcpBridge->active());
#else
    m[QStringLiteral("bridgeActive")] = false;
#endif
    return m;
}

void ShotController::noteDetection(Source source, qint64 tUs)
{
    ++m_counters.detections;
    m_detections.append({source, tUs});
    if (m_detections.size() > kDetectionRing)
        m_detections.remove(0, m_detections.size() - kDetectionRing);
}

bool ShotController::corroborated(qint64 t0Us, QString *outWhy) const
{
    // "No detector available" is the honest exemption, not a loophole: with
    // nothing listening there is no evidence to weigh, and refusing on none
    // would be an assertion this host cannot make.
    if (m_availableDetectors.isEmpty()) {
        if (outWhy) *outWhy = QStringLiteral("no host detector available — accepted unweighed");
        return true;
    }

    // ⚠ AVAILABILITY DECIDES WHETHER THE RULE APPLIES; IT DOES NOT DECIDE WHAT
    // COUNTS AS AGREEMENT.  Any detection this host actually made corroborates,
    // including from a modality never listed as available — ball-launch is
    // exactly that case, corroboration-grade by construction and so never
    // required, but perfectly good evidence on the occasions it fires.
    bool    agreed = false;
    QString verdicts;

    const auto bestDeltaFor = [&](Source s) {
        qint64 best = std::numeric_limits<qint64>::max();
        for (const Detection &d : m_detections) {
            if (d.source != s) continue;
            best = std::min<qint64>(best, std::llabs(d.tUs - t0Us));
        }
        return best;
    };
    const auto record = [&](Source s, qint64 best, bool required) {
        const QLatin1String name(sourceName(s));
        if (best == std::numeric_limits<qint64>::max()) {
            // Only worth saying for a detector that was supposed to be
            // listening; a modality that never fired and was never required is
            // not news.
            if (required) verdicts += QStringLiteral(" %1=none").arg(name);
            return;
        }
        // Recorded whether it agreed or not, because "would the stricter
        // all-must-agree rule have been better here" is answerable only from
        // the misses.
        verdicts += QStringLiteral(" %1=%2ms%3")
                        .arg(name)
                        .arg(static_cast<double>(best) / 1000.0, 0, 'f', 1)
                        .arg(required ? QString() : QStringLiteral("(unlisted)"));
        if (best <= kCorroborationWindowUs) agreed = true;
    };

    for (const Source s : {Source::Acoustic, Source::Imu, Source::Ball, Source::Pose})
        record(s, bestDeltaFor(s), m_availableDetectors.contains(static_cast<int>(s)));

    if (outWhy) *outWhy = verdicts.trimmed();
    return agreed;
}

void ShotController::commitArbitratedShot(qint64 t0HostNs, const QString &shotId)
{
    // I7 — `t0` is what the arbiter decided and is never revised, so this
    // converts units and does nothing else.  The rounding is to the nearest
    // microsecond because the event buffer's index is microseconds; it is a
    // truncation of PRECISION at the local store, not a correction of the
    // instant, and the Shot on the wire keeps its nanoseconds.
    const qint64 tUs = t0HostNs / 1000;

    // ── The corroboration rule ─────────────────────────────────────────────
    ++m_counters.arbitratedSeen;
    QString why;
    const bool ok = corroborated(tUs, &why);
    m_lastVerdict = QStringLiteral("%1 shot=%2 t0_us=%3 — %4")
                        .arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"),
                             shotId, QString::number(tUs), why);
    if (ok) ++m_counters.corroboratePass; else ++m_counters.corroborateFail;
    // ⚠ ppDebug, AND THAT IS THE POINT.  It is compiled away below log level 3,
    // so the full decision — every available detector and its delta — is there
    // while we are testing and absent from a release build.
    ppDebug() << "[ppcp] corroboration" << (ok ? "PASS" : "FAIL") << "shot" << shotId
              << "t0_us" << tUs << "—" << why;
    if (!ok) {
        ppWarn() << "[ppcp] shot REFUSED — no host detector agreed within"
                 << (kCorroborationWindowUs / 1000) << "ms of t0_us" << tUs
                 << "— shot" << shotId << "—" << why;
        // ⚠ THIS IS NOT A RETRACTION, AND CANNOT BE.  `arb_issue` put the Shot
        // on the wire before any line of this function ran, and I7 makes an
        // issued Shot a fact.  What is refused is the LOCAL consequence — this
        // host does not record a swing for it.  The device is not told, because
        // PPCP has no way to say it; that is an open request to the PPC team.
        emit shotRefused(
            tr("Shot from the phone was not recorded — nothing here confirmed it."),
            QStringLiteral("shot.corroboration.refused"));
        return;
    }

    ppInfo() << "[ppcp] arbitrated shot — t0_us" << tUs << "shot" << shotId;
    // `Source::Ppcp`, not `Acoustic`: commitShot's `Source` is a LOCAL label for
    // what produced the shot, a PPCP Shot may have been decided by a Candidate
    // from another peer entirely, and the ordinal is persisted — see the note on
    // the enum.  It is a display and marker label, not an authority claim; the
    // authority is `Shot.issued_by` and lives on the wire.
    if (!commitShot(Source::Ppcp, tUs)) return;

    // ⭐ CORE §8.4 — AND NOW ASK FOR THE FOOTAGE.  Until this line the phone was
    // never asked: `requestCapture()` existed, correct and tested, with no
    // caller anywhere in the tree, and every link ended `bulk 0/0`.
    //
    // ⚠ Back to nanoseconds on `tb:host`, which is exactly what arrived: the
    // division above is a truncation of PRECISION for the event buffer's
    // microsecond index, not a change of domain, so this restores units and
    // nothing else.  The Shot on the wire kept its nanoseconds throughout.
    emit captureRequested(shotId, tUs * 1000);
}
#endif

bool ShotController::commitShot(Source source, qint64 timestampUs)
{
    if (!armed()) {
#ifdef HAVE_PPCP
        // ⚠ A PPCP SHOT DROPPED HERE IS A DIVERGENCE, NOT A NON-EVENT.  The
        // arbiter already issued it on the wire and told the device so; if this
        // host then records nothing, the two ends disagree about how many shots
        // happened and nothing anywhere is red.  A local detector dropped while
        // the processor is busy is an ordinary miss and stays quiet; this is
        // not that, so it is said out loud.
        //
        // The real fix is overlapping shot processing — the pipeline is
        // unavailable for 15-40 s per shot, which a golfer hitting a bucket
        // will outrun.  Deliberately not attempted here.
        if (source == Source::Ppcp) {
            ++m_counters.droppedBusy;
            ppWarn() << "[ppcp] arbitrated shot DROPPED — still processing the previous shot"
                     << "— t0_us" << timestampUs;
            emit shotRefused(
                tr("Shot from the phone was missed — still working on the previous one."),
                QStringLiteral("shot.dropped.busy"));
            return false;
        }
#endif
        ppDebug() << "[ShotController] commit ignored (not capturing or busy) — source"
                  << sourceName(source);
        return false;
    }

    const qint64 impactUs = timestampUs >= 0 ? timestampUs
                                             : pinpoint::EventBuffer::nowMicros();

    // Session type captured at the moment of the shot (-1 = no active session).
    const int sessionType = m_session ? m_session->activeSessionType() : -1;

    // Marker must be in the ring before the signal — the shot processor will
    // pause/freeze the buffer from shotDetected.
    ++m_counters.committed;
    writeShotMarker(source, impactUs, sessionType);

    ppInfo() << "[ShotController] shot detected — source" << sourceName(source)
             << "impact_ts_us" << impactUs << "sessionType" << sessionType;
    emit shotDetected(source, impactUs, sessionType);
    return true;
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
