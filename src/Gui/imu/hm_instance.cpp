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

#include "hm_instance.h"

#include "pp_debug.h"
#include "ble_adapter_pool.h"
#include "ble_imu_transport.h"
#include "event_buffer.h"
#include "imu_sample.h"
#include "hm_frame.h"
#include "hm_unit_id.h"
// The session itself — every hm_session_* call, on the I/O thread. It lives in
// src/IMU because it is device-layer code: this class only marshals its output
// across the thread boundary and onto properties QML can bind.
#include "hm_session_worker.h"

#include <hackmotion/hackmotion.h>

#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QtMath>

#include <chrono>
#include <cmath>
#include <limits>

// ---------------------------------------------------------------------------
// HmUnit
// ---------------------------------------------------------------------------

// ⚠ NOT TRANSLATED, and spelled once here: this string becomes the EventBuffer
// SourceDescriptor::identifier in Phase B and the AppSettings::imuPlacement key
// in Phase C, and both are persisted. A localised identifier would silently
// split one device's history in two the first time the application ran in
// another language.
//
// STATIC because the two persisted uses need the string for a device that has no
// live HmUnit: ImuManager resolves a placement key, and migrates Phase A's
// interim bare-device-id entry, for a wG3 that has been enumerated but never
// connected. The alternative a caller reaches for is respelling the "#lowerArm" /
// "#palm" literal at each site, and that is the one drift nothing here would
// catch — two spellings would not fail, they would silently orphan a device's
// entire placement and its recorded lanes with it.
// ⚠ The literals moved to src/IMU/hm_unit_id.h so the offline re-analyzer can go the
// other way — recorded `source.serial` back to which segment that lane measured —
// without the GUI layer or the vendor SDK. This stays the name everything in the GUI
// calls; it is no longer the place the string is spelled. Still exactly one spelling.
static_assert(pinpoint::hm_unit_id::kLowerArm == HM_UNIT_LOWER_ARM
              && pinpoint::hm_unit_id::kPalm == HM_UNIT_PALM
              && pinpoint::hm_unit_id::kCount == HM_UNIT_COUNT,
              "hm_unit_id's SDK-free unit indices must track hm_unit");

QString HmUnit::unitIdFor(const QString &deviceId, hm_unit unit)
{
    return pinpoint::hm_unit_id::unitIdFor(deviceId, static_cast<int>(unit));
}

// Display only, so this one IS translated.
QString HmUnit::unitLabelFor(hm_unit unit)
{
    return unit == HM_UNIT_PALM ? tr("Palm") : tr("Lower arm");
}

HmUnit::HmUnit(const QString &deviceId, hm_unit unit, QObject *parent)
    : QObject(parent)
    , m_unit(unit)
    , m_unitId(unitIdFor(deviceId, unit))
    , m_unitLabel(unitLabelFor(unit))
{
}

// ---------------------------------------------------------------------------
// HmInstance
// ---------------------------------------------------------------------------

HmInstance::HmInstance(const Device &device,
                       pinpoint::EventBuffer *buffer,
                       QThread *ioThread,
                       QObject *parent)
    : ImuDeviceBase(parent)
    , m_eventBuffer(buffer)
    , m_ioThread(ioThread)
    , m_worker(new HmSessionWorker(device.id))   // no parent — lives on the I/O thread
    , m_device(device)
    , m_deviceId(device.id)
    , m_deviceDescription(device.description)
    , m_lowerArm(new HmUnit(device.id, HM_UNIT_LOWER_ARM, this))
    , m_palm(new HmUnit(device.id, HM_UNIT_PALM, this))
{
    // ⚠ NaN, not 0. Zero is a real presence angle — the best one §8.2 ever
    // measured, in fact, from the calibration that carried no axis information
    // at all — so a default of 0 would read as "calibrated, superbly" before
    // anything has been measured. The same argument applies to the pose spread (a
    // spread of 0° is a perfectly held pose) and to the inter-unit angle (0°
    // is what an applied calibration approaches).
    m_presenceAngleDeg = std::numeric_limits<double>::quiet_NaN();
    m_poseSpreadMaxDeg = std::numeric_limits<double>::quiet_NaN();
    m_relativeAngleDeg = std::numeric_limits<double>::quiet_NaN();
    // At rest, before any routine has been run. ⚠ HM_CAL_UNKNOWN is not
    // HM_CAL_UNCALIBRATED: "nobody has checked" and "there is demonstrably no
    // transform" are different claims, and only a link-down licenses the second.
    m_calibrationPhase       = HM_CALP_IDLE;
    m_calibrationState       = HM_CAL_UNKNOWN;
    m_calibrationAbortReason = HM_CAL_ABORT_NONE;

    // ── TWO SOURCES, ONE DEVICE ──────────────────────────────────────────────
    //
    // Registering from a constructor is legal because ImuManager pauses the
    // EventBuffer around createInstance() exactly as it does for a Witmotion
    // (imu_manager.cpp setSelected()), and registerSource() requires Idle or
    // Paused. This device is nonetheless the first thing in the tree to take TWO
    // slots at once, which is why the failure below is handled rather than
    // assumed away.
    HmUnit *const units[HM_UNIT_COUNT] = { m_lowerArm, m_palm };
    pinpoint::SourceId ids[HM_UNIT_COUNT] =
        { pinpoint::kInvalidSourceId, pinpoint::kInvalidSourceId };

    for (int u = 0; m_eventBuffer && u < HM_UNIT_COUNT; ++u) {
        HmUnit *const unit = units[u];

        pinpoint::SourceDescriptor desc;
        // Display only — the resource monitor shows this as the source name.
        desc.name = (m_deviceDescription + QStringLiteral(" · ") + unit->unitLabel())
                        .toStdString();
        // ⚠ NOT RESPELLED HERE. unitId() is spelled once, in HmUnit's constructor,
        // because it is persisted twice over: it is this identifier now and the
        // AppSettings::imuPlacement key in Phase C.
        desc.identifier = unit->unitId().toStdString();

        pinpoint::ImuFormat fmt{};
        // ⚠ IMU_HackMotion IS THIS LANE'S PROVENANCE, and it is the only machine-
        // readable thing that says the accel column is not what a Witmotion lane's
        // accel column is: the same 40-byte imu_sample_v2 struct holds GRAVITY-
        // REMOVED linear acceleration here (≈0 at rest) and a raw accelerometer
        // reading there. It travels with every window through formatOf(), so a
        // reader can tell them apart; nothing downstream should compare or pool
        // the two, and nothing may average this device's two units either.
        fmt.device         = pinpoint::DeviceKind::IMU_HackMotion;
        fmt.sample_rate_hz = kRingSizingRateHz;   // a sizing ceiling — see the header
        fmt.packet_bytes   = sizeof(pinpoint::ImuSample);
        fmt.packet_schema  = "imu_sample_v2";     // unchanged: 40 bytes, no schema bump

        desc.format.device            = pinpoint::DeviceKind::IMU_HackMotion;
        desc.format.format            = fmt;
        desc.window_duration          = std::chrono::milliseconds(kSourceWindowMs);
        desc.expected_interarrival_us = std::chrono::microseconds(kExpectedInterarrivalUs);
        desc.sync_source              = pinpoint::SyncSource::SoftwareTimestamp;
        // ⚠ desc.format.device_serial IS DELIBERATELY LEFT EMPTY. registerSource()
        // normalises it to the identifier (event_buffer.cpp:152), and that is what
        // the swing exporter keys `source.serial` on — so leaving it empty is how
        // the two stay in step. Setting it to the device id would give both units
        // the same serial and make the pair indistinguishable downstream.

        try {
            const pinpoint::SourceId id = m_eventBuffer->registerSource(desc);
            if (id == pinpoint::kInvalidSourceId) {
                // The buffer was neither Idle nor Paused. Same outcome as a full
                // buffer: this lane does not record.
                appendLog(timestamp()
                          + QStringLiteral("  ERROR: could not register the %1 lane — the "
                                           "event buffer refused it in its current state. "
                                           "This unit will stream to the display but will "
                                           "not be recorded.").arg(unit->unitLabel()));
                ppError() << "[HmInstance]" << m_deviceDescription
                          << "— registerSource refused for" << unit->unitId();
                continue;
            }
            unit->setSourceId(id);
            ids[u] = id;
        } catch (const std::exception &e) {
            // ⚠ registerSource() THROWS std::runtime_error at MAX_SOURCES (16), and
            // this device is the first that can hit it mid-way through claiming its
            // second slot. It is reached from a QML button press, so an escaping
            // exception terminates the application over a full buffer. Degrade to
            // "connected, not recording" instead: Phase A already made every
            // consumer handle a short or empty sourceIds() gracefully.
            //
            // ⚠ A HALF-REGISTERED wG3 IS A REAL STATE, not an impossible one — the
            // lower arm can take the last free slot and leave the palm without one.
            // It records one lane and cannot produce a wrist angle, so the log says
            // so plainly rather than leaving the coach to infer it from a wizard row.
            appendLog(timestamp()
                      + QStringLiteral("  ERROR: could not register the %1 lane (%2). "
                                       "The sensor will connect and display, but this "
                                       "unit will not be recorded — and a wrist angle "
                                       "needs both.")
                            .arg(unit->unitLabel(), QString::fromUtf8(e.what())));
            ppError() << "[HmInstance]" << m_deviceDescription
                      << "— registerSource failed for" << unit->unitId() << ":" << e.what();
        }
    }

    // Hand the ids to the I/O-thread worker BEFORE moveToThread() — see
    // HmSessionWorker::attachBuffer() for why that ordering is the lock.
    m_worker->attachBuffer(m_eventBuffer, ids);

    // Host the session worker on the shared IMU I/O thread. The BLE transport is
    // its child and migrates with it, and the QLowEnergyController is created
    // inside connectToDevice() — which runs there — so its affinity is correct
    // from birth.
    if (m_ioThread)
        m_worker->moveToThread(m_ioThread);

    // The session must be CREATED on the thread that will own it for its whole
    // life, so this is posted rather than called. Queued deliveries to one
    // thread are FIFO, so a start() issued in the same breath still lands after
    // the session exists.
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::initialise,
                              Qt::QueuedConnection);

    connect(m_worker, &HmSessionWorker::logLine, this, [this](const QString &text) {
        appendLog(timestamp() + QStringLiteral("  ") + text);
    });

    connect(m_worker, &HmSessionWorker::transportState,
            this,     &HmInstance::onTransportState);

    connect(m_worker, &HmSessionWorker::mtuTooSmall, this, [this](int negotiated, int required) {
        // ⚠ Its own signal, deliberately not folded into a connect failure:
        // everything up to here SUCCEEDED, no Qt platform lets an application
        // request an MTU, and therefore no retry can change the outcome. A UI
        // that treats it as transient retries forever against a wall.
        m_retrySuppressed = true;
        m_retryTimer.stop();
        appendLog(timestamp()
                  + QStringLiteral("  ERROR: negotiated ATT MTU %1 is below the %2 this "
                                   "device needs — its frames would arrive truncated. "
                                   "No retry can fix this.")
                        .arg(negotiated).arg(required));
        ppError() << "[HmInstance]" << m_deviceDescription << "— MTU" << negotiated
                  << "<" << required << "; refusing to run";
        emit mtuRejected(negotiated, required);
    });

    connect(m_worker, &HmSessionWorker::deviceVersions, this, [this](const QString &summary) {
        const QString refined = m_device.description + QStringLiteral(" (") + summary + QLatin1Char(')');
        if (refined == m_deviceDescription) return;
        m_deviceDescription = refined;
        emit deviceDescriptionChanged();
    });

    connect(m_worker, &HmSessionWorker::batteryPercent, this, [this](int percent) {
        if (percent == m_batteryPercent) return;
        m_batteryPercent = percent;
        emit batteryPercentChanged();
        appendLog(timestamp() + QStringLiteral("  Battery: %1%").arg(percent));
    });

    connect(m_worker, &HmSessionWorker::streamingChanged, this, [this](bool streaming) {
        // ⚠ THE PROPERTY IS SET BEFORE THE CONNECTED GUARD BELOW, ON PURPOSE. A
        // stream stopping BECAUSE the link went away must still clear `streaming`,
        // or the UI keeps offering "Calibrate" for a session that would refuse it
        // with HM_ERR_NO_STREAM. Only the LABEL is guarded, because that is the part
        // that would otherwise overwrite "Disconnected" with "Connected".
        if (m_streaming != streaming) {
            m_streaming = streaming;
            emit streamingChanged();
        }
        if (!m_connected) return;
        setStateLabel(streaming ? QStringLiteral("Streaming") : QStringLiteral("Connected"));
    });

    // ── The calibration surface ──────────────────────────────────────────────
    //
    // Every one of these arrives from the I/O thread as a queued signal, so the
    // handlers run on the GUI thread and may touch the properties directly.
    connect(m_worker, &HmSessionWorker::calibrationPhaseEvent, this,
            [this](int phase, int previousPhase, int abortReason,
                   int libraryState, qint64 elapsedUs) {
        const QString from = QString::fromLatin1(hm_calibration_phase_name(
                                 static_cast<hm_calibration_phase>(previousPhase)));
        const QString to   = QString::fromLatin1(hm_calibration_phase_name(
                                 static_cast<hm_calibration_phase>(phase)));

        // ⚠ A NEW ROUTINE CLEARS THE PREVIOUS ATTEMPT'S PRESENCE FIGURES. Leaving
        // them on screen beside the next attempt's is precisely the comparison §8.2
        // shows would prefer the WORST calibration available — the no-raise attempt
        // scored 0.70° against the correct routine's 1.96° — so the numbers do not
        // survive into an attempt they do not describe.
        if (phase == HM_CALP_AWAIT_HORIZONTAL)
            clearPresenceSurface();

        m_calibrationPhase       = phase;
        m_calibrationState       = libraryState;
        m_calibrationAbortReason = abortReason;
        emit calibrationStateChanged();

        // ⚠ THE DEVICE LOG RING HAS NO REACHABLE UI, so a phase ladder that only
        // appeared there would be invisible in a support bundle — and "which step
        // did it stop at" is the first question about a calibration that did not
        // take. Every transition goes to the application log, named.
        ppInfo() << "[HmInstance]" << m_deviceId << "— calibration" << from << "→" << to
                 << "after" << elapsedUs / 1000 << "ms, library state" << libraryState;

        // ⚠ abort_reason != NONE IS NOT "ABORTED". HM_CAL_ABORT_CALLER is carried on
        // a transition to COMPLETE as well, because aborting at HM_CALP_VERIFYING
        // declines the presence check on a transform the device HAS ALREADY APPLIED
        // — the routine finished, the check was declined, and reporting that as
        // aborted would claim nothing happened to a stream whose frame had just
        // changed underneath it. So the phase decides the wording and the reason only
        // qualifies it.
        if (abortReason != HM_CAL_ABORT_NONE) {
            const QString reason = phase == HM_CALP_COMPLETE
                ? QStringLiteral("  Calibration finished, but the presence check was "
                                 "declined — the device applied its transform and "
                                 "nothing has verified it. Recorded as unverified.")
                : QStringLiteral("  Calibration abandoned at %1 (reason %2). Nothing "
                                 "was applied; run it again.").arg(from).arg(abortReason);
            appendLog(timestamp() + reason);
        } else {
            appendLog(timestamp() + QStringLiteral("  Calibration: %1 → %2").arg(from, to));
        }
    });

    connect(m_worker, &HmSessionWorker::calibrationPresenceMeasured, this,
            [this](int libraryState) {
        // The whole measurement in one copy — the angle, both forms of the anchor
        // and the spread that says whether the anchor is worth anything.
        m_anchor = m_worker->referenceAnchor();
        m_calibrationState    = libraryState;
        m_presenceAngleDeg    = double(m_anchor.relativeAngleDeg);
        m_presenceSamplesUsed = int(m_anchor.samplesUsed);
        m_presenceNotMeasured = false;
        // ⚠ THE WORSE OF THE TWO UNITS, not a mean of them: this is evidence about
        // whether the athlete HELD the pose, and one unit drifting is enough to
        // contaminate the anchor a Phase D solve would bake into every reading.
        m_poseSpreadMaxDeg = double(qMax(m_anchor.poseSpreadDeg[HM_UNIT_LOWER_ARM],
                                         m_anchor.poseSpreadDeg[HM_UNIT_PALM]));
        emit calibrationStateChanged();

        // ⚠ REPORTED, NEVER RANKED. Both numbers go to the log as state: the angle
        // because its one sound use is catching "calibration never happened or was
        // lost" (an order-of-magnitude gap), and the spread because a mean without a
        // spread is an estimate without evidence.
        ppInfo() << "[HmInstance]" << m_deviceId << "— reference pose measured:"
                 << "presence angle" << m_presenceAngleDeg << "° over"
                 << m_presenceSamplesUsed << "samples, pose spread"
                 << m_anchor.poseSpreadDeg[HM_UNIT_LOWER_ARM] << "/"
                 << m_anchor.poseSpreadDeg[HM_UNIT_PALM] << "° (lowerArm/palm), library"
                    " state" << libraryState << ". ⚠ The angle is a PRESENCE check and"
                    " it inverts — §8.2 measured a no-raise calibration scoring BEST on"
                    " it — so it is never a quality score and attempts are never ranked"
                    " on it. The anchor is kept for the Phase D frame solve.";
        appendLog(timestamp()
                  + QStringLiteral("  Reference pose measured: presence angle %1° "
                                   "(%2 samples, hold spread %3°). This says the "
                                   "calibration is PRESENT, not that it is good.")
                        .arg(m_presenceAngleDeg, 0, 'f', 2)
                        .arg(m_presenceSamplesUsed)
                        .arg(m_poseSpreadMaxDeg, 0, 'f', 2));
    });

    connect(m_worker, &HmSessionWorker::calibrationPresenceUnmeasured, this,
            [this](int samplesCollected, int libraryState) {
        // ⚠ THE PHASE STILL REACHES HM_CALP_COMPLETE, so this flag is the only thing
        // standing between "we could not check" and a UI that claims success. The
        // angle stays NaN and the state stays HM_CAL_UNKNOWN — the recording must say
        // we did not check rather than imply we did.
        m_presenceNotMeasured = true;
        m_presenceSamplesUsed = samplesCollected;
        m_presenceAngleDeg    = std::numeric_limits<double>::quiet_NaN();
        m_poseSpreadMaxDeg    = std::numeric_limits<double>::quiet_NaN();
        m_anchor = ReferenceAnchor{};   // no measurement means no anchor for Phase D
        m_calibrationState = libraryState;
        emit calibrationStateChanged();

        ppWarn() << "[HmInstance]" << m_deviceId << "— the presence check could NOT be"
                    " taken: only" << samplesCollected << "live samples reached the run."
                    " The device applied a transform and nothing has verified it, and"
                    " there is no reference anchor for the frame solve. Re-run the"
                    " routine.";
        appendLog(timestamp()
                  + QStringLiteral("  Could not measure the reference pose — only %1 "
                                   "samples arrived. The calibration is UNVERIFIED, "
                                   "not confirmed. Run it again.").arg(samplesCollected));
    });

    connect(m_worker, &HmSessionWorker::calibrationStateRefreshed, this,
            [this](int libraryState) {
        // ABSENT or INDETERMINATE. The angle already arrived with the presence event;
        // all that changes here is what the LIBRARY now knows, which is read rather
        // than decided — guessing in the indeterminate band is how a presence check
        // becomes a quality score.
        if (m_calibrationState == libraryState) return;
        m_calibrationState = libraryState;
        emit calibrationStateChanged();
    });

    connect(m_worker, &HmSessionWorker::calibrationLost, this,
            [this](int libraryState, const QString &why) {
        invalidateCalibration(libraryState, why);
    });

    connect(m_worker, &HmSessionWorker::calibrationCallRefused, this,
            [this](int status, const QString &call) {
        // ⚠ THE THREE REACHABLE REFUSALS WANT THREE DIFFERENT THINGS FROM THE COACH,
        // which is why the status is carried through raw to QML as well as being
        // spelled out here rather than folded into one "calibration error".
        QString advice;
        switch (status) {
        case HM_ERR_NO_STREAM:
            advice = QStringLiteral("the sensor is not streaming, and calibration "
                                    "cannot be done without it — the device watches a "
                                    "continuous raise. Reconnect and try again.");
            break;
        case HM_ERR_BUSY:
            // ⚠ Not reachable until Phase E opens a history bracket. Carried legibly
            // NOW rather than discovered THEN: a retrieval suspends live delivery and
            // the presence check is measured FROM live samples, so the honest answer
            // is "a second later", with the athlete standing still either way.
            advice = QStringLiteral("the sensor is busy replaying its buffer for the "
                                    "last shot. Wait a second and try again.");
            break;
        case HM_ERR_INVALID_STATE:
            advice = QStringLiteral("there is no calibration routine at that step.");
            break;
        default:
            advice = QString::fromLatin1(hm_status_str(static_cast<hm_status>(status)));
            break;
        }
        appendLog(timestamp()
                  + QStringLiteral("  Calibration step refused (%1): %2")
                        .arg(call, advice));
        ppWarn() << "[HmInstance]" << m_deviceId << "—" << call << "refused:"
                 << hm_status_str(static_cast<hm_status>(status)) << "—" << advice;
        emit calibrationCallRefused(status, call);
    });

    connect(m_worker, &HmSessionWorker::retryUseless, this, [this](const QString &advice) {
        m_retrySuppressed = true;
        if (m_retryTimer.isActive()) {
            // The classification always arrives a moment AFTER the transport's
            // own disconnect, so the retry it cancels has usually just been
            // scheduled. That ordering is harmless precisely because this flag
            // only ever cancels — it never starts anything.
            m_retryTimer.stop();
            m_retryCount = 0;
            if (m_busy) { m_busy = false; emit busyChanged(); }
            setStateLabel(QStringLiteral("Error"));
        }
        appendLog(timestamp()
                  + QStringLiteral("  Auto-retry cancelled — the library classified this "
                                   "drop as %1. Only you can clear it.").arg(advice));
        ppWarn() << "[HmInstance]" << m_deviceDescription << "— retry suppressed:" << advice;
    });

    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        appendLog(timestamp() + QStringLiteral("  Auto-retrying connection…"));
        start();
    });

    m_logTimer.setSingleShot(false);
    connect(&m_logTimer, &QTimer::timeout, this, [this]() {
        const HmSessionWorker::Snapshot snap = m_worker->snapshot();
        const QString bat = m_batteryPercent >= 0
            ? QStringLiteral("  BAT=%1%").arg(m_batteryPercent)
            : QString();
        QString dropped;
        if (snap.droppedLive > m_lastDroppedLive) {
            // Non-zero means the host did not keep up and data was lost, which
            // must be visible rather than silent.
            dropped = QStringLiteral("  DROPPED=%1").arg(snap.droppedLive - m_lastDroppedLive);
            m_lastDroppedLive = snap.droppedLive;
        }

        // What actually reached the rings, per lane. The display rate above says
        // data is flowing; only these say it is being RECORDED, and the pair
        // diverges precisely when a registration failed.
        const QString written = QStringLiteral("  WROTE=%1/%2")
                                    .arg(snap.written[HM_UNIT_LOWER_ARM])
                                    .arg(snap.written[HM_UNIT_PALM]);

        // Provenance, not a correction: the two blocks are never paired as
        // simultaneous and this is never applied to a timestamp. §10.3 measures a
        // stable 0.92 ms at the SESSION level.
        //
        // ⚠ THE WIDE MIN/MAX IS EXPECTED AND IS NOT THE OFFSET MOVING. A single
        // record's difference is dominated by ±½-sample pairing jitter — 89 and 99
        // ticks on two consecutive records against a session median of 59 — so the
        // extremes describe the jitter and only the MEDIAN describes the skew.
        // Reading the spread as "the offset is not constant" is the misreading this
        // line is spelled out to prevent.
        const HmSessionWorker::SkewStats skewSt =
            m_worker ? m_worker->skewStats() : HmSessionWorker::SkewStats{};
        const QString skew = skewSt.stored > 0
            ? QStringLiteral("  SKEW(palm−arm)=%1/%2/%3 µs (min/MEDIAN/max, n=%4%5)")
                  .arg(snap.skewMinUs)
                  .arg(skewSt.medianUs, 0, 'f', 2)
                  .arg(snap.skewMaxUs)
                  .arg(skewSt.stored)
                  // A truncation nobody reports reads as coverage: say when the
                  // median is over a prefix rather than the whole session.
                  .arg(skewSt.stored < skewSt.total
                           ? QStringLiteral(" of %1").arg(skewSt.total)
                           : QString())
            : QString();

        // ⚠ THE INTER-UNIT ANGLE, AND IT IS THE FIRST THING TO READ WHEN THE TWO
        // LIVE CUBES LOOK WRONG. They are shown in two UNRECONCILED raw device
        // frames — no per-unit rotation exists until Phase D solves R_lowerArm and
        // R_palm — so how far apart they LOOK carries no verdict. This number does,
        // because it is a rotation magnitude and independent of both conventions.
        // Bands are in writeSample(), and the load-bearing part is that they only
        // mean anything AT REST WITH A STRAIGHT WRIST. Uncalibrated and in motion
        // this angle reaches 180° as a matter of course.
        QString relAngle;
        if (snap.relAngleCount > 0) {
            const double relMean = snap.relAngleSumDeg / double(snap.relAngleCount);
            relAngle = QStringLiteral("  REL-ANGLE now=%1° (session %2/%3/%4°)")
                           .arg(snap.relAngleNowDeg, 0, 'f', 1)
                           .arg(snap.relAngleMinDeg, 0, 'f', 1)
                           .arg(relMean, 0, 'f', 1)
                           .arg(snap.relAngleMaxDeg, 0, 'f', 1);
            if (!m_relAngleReported) {
                m_relAngleReported = true;
                ppInfo() << "[HmInstance]" << m_deviceId << "— inter-unit angle now"
                         << snap.relAngleNowDeg << "°, session mean" << relMean
                         << "° (" << snap.relAngleMinDeg << "-" << snap.relAngleMaxDeg
                         << "). ⚠ Only interpretable AT REST WITH THE WRIST STRAIGHT, where"
                            " uncalibrated reads ~15° and an applied calibration reads 0.4-0.8°."
                            " In any other pose an uncalibrated pair reaches 180° routinely —"
                            " the library's own swing fixtures sit at 170-180° for 28 % of their"
                            " samples — so a large value here is not a fault. Not a quality"
                            " score either: it is blind to the calibration axis.";
            }
        }

        if (skewSt.stored > 0) {
            const qint32 spread = snap.skewMaxUs - snap.skewMinUs;
            // ONCE PER SESSION, into the application log. The MEDIAN is what the
            // exporter bakes into swing.json (device.skewUs), so a reader of a
            // capture can see the same figure the capture was made under — and
            // this is the only reachable place it appears live, the device log ring
            // having no UI.
            if (!m_skewReported) {
                m_skewReported = true;
                ppInfo() << "[HmInstance]" << m_deviceId << "— inter-unit skew (palm − lower arm)"
                         << skewSt.medianUs << "µs median, spread" << spread
                         << "µs over" << skewSt.stored
                         << "samples. ⚠ The spread is EXPECTED to be wide — a single record's"
                            " difference is dominated by ±½-sample pairing jitter — so only the"
                            " median describes the offset. Carried into provenance, never"
                            " applied to a timestamp.";
            }
            // ⚠ THIS USED TO WARN ON THE MIN/MAX SPREAD, AND THAT WAS WRONG. The two
            // units share a sample index by construction but run two free-running MCU
            // timers, so single-record values scatter by ±½ sample — libhackmotion
            // measured 89 and 99 ticks on consecutive records against a session median
            // of 59 — which is ~1250 µs of entirely healthy scatter at the internal
            // rate. A spread threshold therefore fires on noise, on a good device.
            //
            // §10.3's stability claim is about the MEDIAN, and it was established by
            // splitting a 238 s session and finding the two halves' medians identical.
            // So that is what is tested: a half-split delta beyond a few ticks is the
            // offset genuinely moving, which is what Phase E/G may not assume away.
            if (skewSt.halfSplitValid
                && std::abs(skewSt.halfSplitDeltaUs) > kSkewHalfSplitWarnUs
                && !m_skewSpreadWarned) {
                m_skewSpreadWarned = true;
                ppWarn() << "[HmInstance]" << m_deviceId << "— inter-unit skew is NOT stable:"
                         << "the median moved" << skewSt.halfSplitDeltaUs
                         << "µs between the first and second halves of the run (§10.3 measured"
                            " no movement at all). The published 0.92 ms constant does not hold"
                            " here, so it cannot be treated as a subtractable offset.";
            }
        }

        // ⚠ Both of the next two are expected to be zero for the whole life of a
        // session. They are reported as TOTALS and warned on GROWTH, because either
        // one appearing means an assumption this lane rests on has broken: that the
        // clock fit exists from the first frame (brief §0 #1), and that the
        // published host_time_us never steps back across a fit re-anchor.
        QString noFit;
        if (snap.noFitSkipped > 0) {
            noFit = QStringLiteral("  NO-FIT-SKIPPED=%1").arg(snap.noFitSkipped);
            if (snap.noFitSkipped > m_lastNoFitSkipped) {
                ppWarn() << "[HmInstance]" << m_deviceDescription
                         << "— samples with no mapped host time were skipped, total"
                         << snap.noFitSkipped
                         << "— the clock fit is supposed to exist from the first live frame";
            }
        }
        m_lastNoFitSkipped = snap.noFitSkipped;

        const quint64 backSteps = snap.nonMonotonic[HM_UNIT_LOWER_ARM]
                                + snap.nonMonotonic[HM_UNIT_PALM];
        QString nonMono;
        if (backSteps > 0) {
            // ⚠ MEASURED, NOT CLAMPED HERE — the merger clamps, and the count it
            // keeps is what a support bundle carries. This line says the same thing
            // per lane, with the size of the worst step the merger had to absorb.
            nonMono = QStringLiteral("  HOST-TIME-BACKSTEPS=%1/%2 (worst %3/%4 µs)")
                          .arg(snap.nonMonotonic[HM_UNIT_LOWER_ARM])
                          .arg(snap.nonMonotonic[HM_UNIT_PALM])
                          .arg(snap.maxBackStepUs[HM_UNIT_LOWER_ARM])
                          .arg(snap.maxBackStepUs[HM_UNIT_PALM]);
            if (backSteps > m_lastNonMonotonic) {
                ppWarn() << "[HmInstance]" << m_deviceDescription
                         << "— host_time_us stepped backwards; the merger clamped it. Total"
                         << backSteps << "worst step (µs) lowerArm"
                         << snap.maxBackStepUs[HM_UNIT_LOWER_ARM]
                         << "palm" << snap.maxBackStepUs[HM_UNIT_PALM];
            }
        }
        m_lastNonMonotonic = backSteps;

        appendLog(timestamp()
            + QStringLiteral("  Data: %1 samples total  (+%2 in last 10s)  %3 Hz avg%4%5%6%7%8%9%10")
                .arg(m_totalSamples)
                .arg(m_samplesSinceLog)
                .arg(m_dataRateHz, 0, 'f', 1)
                .arg(bat)
                .arg(dropped)
                .arg(written)
                .arg(relAngle)
                .arg(skew)
                .arg(noFit)
                .arg(nonMono));
        m_samplesSinceLog = 0;
    });

    // 60 Hz display tick — the ONLY emitter of the high-rate change signals. The
    // decode hot path is on the I/O thread; this copies its latest snapshot into
    // the two HmUnits (it is their friend) and emits. Without it a burst reaching
    // the device's full ≈799.2 Hz internal rate — which happens in every session
    // containing motion (§6.6) — would post ~1,600 notifications a second at the
    // GUI thread, twice over for two units.
    m_displayTimer.setInterval(kDisplayTickMs);
    m_displayTimer.setSingleShot(false);
    connect(&m_displayTimer, &QTimer::timeout, this, [this]() {
        const HmSessionWorker::Snapshot snap = m_worker->snapshot();
        if (snap.seq == m_lastSeq)
            return;
        m_samplesSinceLog += snap.seq - m_lastSeq;
        m_totalSamples    += snap.seq - m_lastSeq;
        m_lastSeq          = snap.seq;

        HmUnit *const units[HM_UNIT_COUNT] = { m_lowerArm, m_palm };
        for (int u = 0; u < HM_UNIT_COUNT; ++u) {
            const HmSessionWorker::UnitState &s = snap.unit[u];
            HmUnit *unit = units[u];

            unit->m_quatW = s.qw; unit->m_quatX = s.qx;
            unit->m_quatY = s.qy; unit->m_quatZ = s.qz;
            // ⚠ No display-frame remap, unlike the Witmotion lane's X→X, Z→Y,
            // −Y→Z. That remap belongs to a frame convention this device does
            // not share and Phase D has not solved.
            unit->m_accelX = s.ax; unit->m_accelY = s.ay; unit->m_accelZ = s.az;
            unit->m_eulerRoll  = s.roll;
            unit->m_eulerPitch = s.pitch;
            unit->m_eulerYaw   = s.yaw;
            unit->m_angularVelocityDps = s.velDps;

            // --- Phase D: the anatomical frame -------------------------------
            //
            // TWO conditions, and both are needed. The DEVICE must have applied
            // its own calibration (before that the streamed quaternion carries
            // board placement, not anatomy), and a directed capture must have
            // SELECTED a frame candidate (before that we do not know which way
            // its axes point). Either one alone produces a quaternion that moves
            // convincingly and means nothing.
            const bool deviceCalibrated = (m_calibrationState == HM_CAL_CALIBRATED);
            const bool anat = deviceCalibrated && pinpoint::hm_frame::isSelected();

            if (anat != unit->m_anatCalibrated) {
                unit->m_anatCalibrated = anat;
                emit unit->anatCalibratedChanged();
            }

            if (anat) {
                unit->m_anatQuat = pinpoint::hm_frame::toAnatomical(
                    QQuaternion(s.qw, s.qx, s.qy, s.qz).normalized());
                unit->m_mountM   = pinpoint::hm_frame::mountM();
                // ⚠ A is IDENTITY for this lane, and that is not a stub. The
                // device referenced the pair at its own pose 0, so there is no
                // per-session world→anatomical solve left to do; what that pose
                // leaves behind is a constant offset from our neutral, which
                // wristRel's Address reference absorbs downstream.
                unit->m_alignA   = QQuaternion();
            } else {
                unit->m_anatQuat = QQuaternion();
                unit->m_mountM   = QQuaternion();
                unit->m_alignA   = QQuaternion();
            }

            // ⚠ PRONATION RATE COMES FROM THE LOWER-ARM UNIT ALONE. During
            // pronation both units rotate together — the wrist barely
            // articulates about this axis — so a difference of the two would
            // cancel most of the signal. The palm unit carries no such reading
            // and is left at zero rather than given a plausible one.
            const float pron = (u == 0 && anat)
                                   ? pinpoint::hm_frame::pronationRateDps(QVector3D(s.gx, s.gy, s.gz))
                                   : 0.0f;
            if (qAbs(pron - unit->m_pronationRateDps) > 0.5f) {
                unit->m_pronationRateDps = pron;
                emit unit->pronationRateDpsChanged();
            }

            emit unit->quatChanged();
            emit unit->accelChanged();
            if (qAbs(s.velDps - m_lastSentVelDps[u]) > 0.5f) {
                m_lastSentVelDps[u] = s.velDps;
                emit unit->angularVelocityDpsChanged();
            }
        }

        m_dataRateHz = snap.rateHz;
        if (qAbs(m_dataRateHz - m_lastSentRateHz) > 0.1) {
            m_lastSentRateHz = m_dataRateHz;
            emit dataRateHzChanged();
        }

        // ⚠ THE NUMBER THAT PROVES A CALIBRATION TOOK, AND IT IS ONLY INTERPRETABLE
        // AT REST WITH A STRAIGHT WRIST: ~15° uncalibrated, 0.4-0.8° once the
        // transform is applied, and the collapse to under 1° at the reference pose is
        // the evidence. ⚠ MEANINGLESS MID-MOTION — this same stream reads 170-180°
        // routinely while the wrist is moving (the library's five-swing fixture sits
        // in that band for 28 % of its samples), so a UI that shows it during the
        // raise is showing a fault that is not there. It is a rotation magnitude and
        // therefore independent of both units' unreconciled frames, which is exactly
        // what makes it usable before Phase D exists.
        //
        // Rate-limited like the rate and the angular velocities above: the tick runs
        // at 60 Hz and an unchanged value must not notify. The first real reading
        // always notifies, because the NaN it replaces compares false against
        // everything.
        const double rel = double(snap.relAngleNowDeg);
        const bool firstReading = std::isnan(m_relativeAngleDeg) && !std::isnan(rel);
        m_relativeAngleDeg = rel;
        if (firstReading || qAbs(rel - m_lastSentRelAngleDeg) > 0.1) {
            m_lastSentRelAngleDeg = rel;
            emit relativeAngleChanged();
        }
    });
    m_displayTimer.start();
}

HmInstance::~HmInstance()
{
    m_retryTimer.stop();
    m_logTimer.stop();
    m_displayTimer.stop();

    // The worker (and the transport that is its child, and the hm_session it
    // owns) lives on the I/O thread — destroy it there. The thread outlives the
    // instances (ImuManager joins it after deleting them), so this deferred
    // delete always runs, and the worker's destructor makes the
    // hm_session_destroy() call on the one thread allowed to make it.
    if (m_worker)
        m_worker->deleteLater();
}

std::vector<pinpoint::SourceId> HmInstance::sourceIds() const
{
    // ⚠ ORDER IS {lowerArm, palm} — the cable's own order (wire block 0 is the
    // lower arm, §6.3), which is also what sourceLabels() and every positional
    // consumer assume. A unit whose registration failed is SKIPPED rather than
    // represented by kInvalidSourceId, so the vector never hands a caller an id
    // it must remember to test.
    std::vector<pinpoint::SourceId> ids;
    ids.reserve(HM_UNIT_COUNT);
    const HmUnit *const units[HM_UNIT_COUNT] = { m_lowerArm, m_palm };
    for (const HmUnit *unit : units)
        if (unit->sourceId() != pinpoint::kInvalidSourceId)
            ids.push_back(unit->sourceId());
    return ids;
}

QStringList HmInstance::sourceLabels() const
{
    // Same order, same skips, same length as sourceIds() — the resource monitor
    // pairs them by position.
    QStringList labels;
    const HmUnit *const units[HM_UNIT_COUNT] = { m_lowerArm, m_palm };
    for (const HmUnit *unit : units)
        if (unit->sourceId() != pinpoint::kInvalidSourceId)
            labels.append(unit->unitLabel());
    return labels;
}

HmInstance::CaptureProvenance
HmInstance::captureProvenance(qint64 windowStartUs, qint64 windowEndUs) const
{
    // ⚠ A default-constructed value is NOT "clean" — every field's absent form says
    // "not measured" (state -1, configBits -1, no entries), which is what a device
    // with no worker honestly has to report. It must never read as "nothing clipped".
    if (!m_worker) return {};
    return m_worker->captureProvenance(windowStartUs, windowEndUs);
}

double HmInstance::skewUsMedian() const
{
    if (!m_worker) return std::numeric_limits<double>::quiet_NaN();
    const HmSessionWorker::SkewStats st = m_worker->skewStats();
    // ⚠ NaN, not 0, until a sample has been seen: 0 µs is a perfectly plausible
    // skew, so a default of 0 would be indistinguishable from a measurement.
    if (st.stored == 0) return std::numeric_limits<double>::quiet_NaN();
    return st.medianUs;
}

void HmInstance::start()
{
    if (m_attemptingConn) return;

    // A fresh attempt supersedes any pending backoff retry (this is also the
    // entry point the retry timer calls — stop() on an already-fired single-shot
    // is a harmless no-op).
    m_retryTimer.stop();

    // An explicit attempt clears the suppression. The flag only ever cancels a
    // PENDING automatic retry; a user who has pressed the button on the device,
    // or closed the vendor app, is entitled to try again without restarting the
    // application. The retry timer cannot reach here while suppressed, because
    // suppression stopped it.
    m_retrySuppressed = false;

    // Look up fresh from the enumerator at connection time — the platform handle
    // is refreshed by every re-scan and the stored one may be stale.
    QBluetoothDeviceInfo deviceInfo;
    for (const Device &dev : DeviceEnumerator::instance()->devices(DeviceType::Imu)) {
        if (dev.id == m_deviceId) {
            deviceInfo = dev.platformHandle.value<QBluetoothDeviceInfo>();
            break;
        }
    }

    if (!deviceInfo.isValid()) {
        appendLog(timestamp() + QStringLiteral("  ERROR: device not found in enumerator: ") + m_deviceId);
        setStateLabel(QStringLiteral("Not found"));
        return;
    }

    m_attemptingConn = true;
    m_connecting     = true;
    m_gattReachedThisAttempt = false;
    appendLog(timestamp() + QStringLiteral("  >>> Connecting to: ")
              + m_deviceDescription + QStringLiteral(" [") + m_deviceId + QStringLiteral("]"));

    // connectToDevice runs ON the I/O thread so the QLowEnergyController (and the
    // WinRT watchers behind it) are created with the right affinity.
#ifdef Q_OS_LINUX
    // On Linux, assign adapters round-robin across connections so multiple IMUs
    // can stream simultaneously without contending for the same HCI adapter.
    const QBluetoothAddress adapter = BleAdapterPool::instance()->nextAdapter();
    if (!adapter.isNull())
        appendLog(timestamp() + QStringLiteral("  BT adapter: ") + adapter.toString());
#else
    const QBluetoothAddress adapter;
#endif
    QMetaObject::invokeMethod(m_worker, [worker = m_worker, deviceInfo, adapter]() {
        worker->connectTo(deviceInfo, adapter);
    }, Qt::QueuedConnection);

    if (!m_busy) { m_busy = true; emit busyChanged(); }
}

void HmInstance::stop()
{
    m_retryTimer.stop();
    m_retryCount     = 0;
    m_connecting     = false;
    m_attemptingConn = false;

    // ⚠ THE PRODUCER STOP BARRIER, on the I/O thread and blocking. The worker
    // stops the drain timer, severs dataReceived, drops the link and calls
    // hm_session_close(); when this returns nothing the library owns can produce
    // another sample or event — structurally, because the library never pushes
    // and the host has stopped draining. Only after this may the caller pause
    // the EventBuffer and deregister the sources (Phase B).
    QMetaObject::invokeMethod(m_worker, [worker = m_worker]() {
        worker->shutdown();
    }, Qt::BlockingQueuedConnection);
}

void HmInstance::deregisterFromBuffer()
{
    // ⚠ LEGAL ONLY WITH THE BUFFER PAUSED AND NO SwingWindow LIVE — both asserted
    // by EventBuffer::deregisterSource(). The ordering is ImuManager's guarantee,
    // not ours to re-check: it calls stop() (the producer stop barrier, which also
    // nulls the worker's buffer pointer) and only then deregisterFromBuffer(),
    // with the buffer paused, in both ~ImuManager and setSelected(false).
    //
    // Idempotent: the ids are invalidated as they go, and deregisterSource() is
    // itself a no-op on an unknown id, so a second call does nothing.
    if (!m_eventBuffer) return;

    HmUnit *const units[HM_UNIT_COUNT] = { m_lowerArm, m_palm };
    for (HmUnit *unit : units) {
        if (unit->sourceId() == pinpoint::kInvalidSourceId) continue;
        m_eventBuffer->deregisterSource(unit->sourceId());
        unit->setSourceId(pinpoint::kInvalidSourceId);
    }
}

bool HmInstance::calibrationActive() const
{
    // ⚠ "A ROUTINE IS IN FLIGHT", WHICH IS NOT "SOMETHING IS CALIBRATED". Both
    // terminal phases are excluded and COMPLETE is one of them — a completed
    // routine can still leave the state at HM_CAL_UNKNOWN, which is what
    // calibrationState is for.
    return m_calibrationPhase != HM_CALP_IDLE
        && m_calibrationPhase != HM_CALP_COMPLETE
        && m_calibrationPhase != HM_CALP_ABORTED;
}

// ── The five entry points. Each is one queued hop and nothing else ───────────
//
// ⚠ QueuedConnection, NEVER BlockingQueuedConnection. The library's contract puts
// every hm_calibration_* call on the thread that owns the session, and blocking
// the GUI thread for one would freeze the guide animation that is PACING the
// athlete — the raise is watched continuously, so a stalled renderer is a failed
// calibration. hm_calibration_confirm_reference_pose() returns before its
// measurement exists in any case, so there is nothing a caller could usefully
// wait for.
//
// ⚠ AND THAT IS WHY NONE OF THEM RETURNS A STATUS. The hm_status the library
// produced cannot travel back across a queued hop; it arrives as
// calibrationCallRefused(status, call), which is the entire refusal channel. A UI
// that waited on a return value here would wait forever.
// ── Deferred history (Phase E) ───────────────────────────────────────────────
//
// ⚠ QueuedConnection for the reserve, exactly like the calibration entry points
// above and for the same reason: hm_history_reserve() is a hm_session_* call and
// must run on the thread that owns the session. It does not block and issues no
// radio traffic, so there is nothing the GUI thread could usefully wait for —
// and a refusal travels back as a log line, not as a return value.
void HmInstance::reserveHistory(qint64 impactUs, qint64 deadlineUs)
{
    if (!m_worker)
        return;
    QMetaObject::invokeMethod(m_worker,
                              [worker = m_worker, impactUs, deadlineUs]() {
                                  worker->reserveHistory(impactUs, deadlineUs);
                              },
                              Qt::QueuedConnection);
}

bool HmInstance::historyPending() const
{
    return m_worker && m_worker->historyPending();
}

HmInstance::HistoryResult HmInstance::takeHistoryResult()
{
    if (!m_worker)
        return {};
    return m_worker->takeHistoryResult();
}

void HmInstance::beginCalibration()
{
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::beginCalibration,
                              Qt::QueuedConnection);
}

void HmInstance::confirmHorizontal()
{
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::confirmHorizontal,
                              Qt::QueuedConnection);
}

void HmInstance::confirmRaise()
{
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::confirmRaise,
                              Qt::QueuedConnection);
}

void HmInstance::confirmReferencePose()
{
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::confirmReferencePose,
                              Qt::QueuedConnection);
}

void HmInstance::abortCalibration()
{
    QMetaObject::invokeMethod(m_worker, &HmSessionWorker::abortCalibration,
                              Qt::QueuedConnection);
}

void HmInstance::clearPresenceSurface()
{
    // The caller emits calibrationStateChanged() — this is only ever called as part
    // of a larger transition that has more to set.
    m_presenceAngleDeg    = std::numeric_limits<double>::quiet_NaN();
    m_poseSpreadMaxDeg    = std::numeric_limits<double>::quiet_NaN();
    m_presenceSamplesUsed = 0;
    m_presenceNotMeasured = false;
    m_anchor = ReferenceAnchor{};
}

void HmInstance::invalidateCalibration(int libraryState, const QString &why)
{
    // ⚠ RECONNECT IS NOT RESUME, AND THIS IS WHERE OUR UI MATCHES THAT INSTEAD OF
    // PAPERING OVER IT. §8.3 measured 0.70° immediately before dropping a link and
    // 18.80° at the same pose after reconnecting, strap untouched and never
    // removed. The library makes resume un-expressible — link-down drives every
    // subsequent sample to HM_CAL_UNCALIBRATED — so every last thing the routine
    // produced goes back to nothing here, including the reference anchor: an anchor
    // is a measurement of a transform that no longer exists.
    //
    // ⚠ AND NOTHING IS WRITTEN ANYWHERE. There is no save, no load, no "reuse last
    // session" and none may be added — see the declaration in the header for the
    // full reason. A persisted anchor or presence angle would be re-applied to a
    // device whose transform is gone, and the resulting wrist angles would be
    // plausible, permanently wrong and unfalsifiable from the recording. The
    // library refuses to express it; so do we.
    //
    // ⚠ libraryState IS READ, NOT CHOSEN. Link-down gives HM_CAL_UNCALIBRATED
    // ("there is demonstrably no transform"); a stream restart gives
    // HM_CAL_UNKNOWN ("I no longer know"), because whether a restart costs the
    // device its transform is untested where a disconnect demonstrably does. Those
    // are different claims and it is not ours to pick between them.
    //
    // Whether there was anything to lose decides only the WORDING. Both the state
    // reset and the signal are unconditional: a UI that misses one invalidation
    // goes on claiming a calibration that is gone, which is the failure this whole
    // path exists to prevent, and "we told you twice" costs nothing against it.
    const bool hadSomething = m_anchor.valid
                           || m_calibrationState == HM_CAL_CALIBRATED
                           || calibrationActive()
                           || !std::isnan(m_presenceAngleDeg);

    m_calibrationPhase       = HM_CALP_IDLE;
    m_calibrationState       = libraryState;
    m_calibrationAbortReason = HM_CAL_ABORT_NONE;
    clearPresenceSurface();
    emit calibrationStateChanged();
    emit calibrationInvalidated();

    if (hadSomething) {
        appendLog(timestamp()
                  + QStringLiteral("  Calibration lost — %1. The sensor's own transform "
                                   "does not survive this and cannot be restored: run "
                                   "the routine again before recording.").arg(why));
        ppWarn() << "[HmInstance]" << m_deviceId << "— calibration invalidated:" << why
                 << "— library state" << libraryState
                 << ". Nothing is persisted and nothing is resumed; the routine must be"
                    " re-run (§8.3 measured 0.70° → 18.80° at the same pose across a"
                    " plain disconnect).";
    } else {
        // Nothing had been calibrated, so saying "lost" would invent a loss. The
        // surface is still reset, because "nothing was calibrated" is exactly what
        // it now has to say.
        ppInfo() << "[HmInstance]" << m_deviceId
                 << "— calibration surface reset (" << why
                 << "); nothing had been calibrated. Library state" << libraryState;
    }
}

QString HmInstance::saveLog()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    const QString fileName = QStringLiteral("hackmotion_log_%1_%2.txt")
        .arg(QString(m_deviceId).replace(QStringLiteral(":"), QStringLiteral("")))
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = dir + QDir::separator() + fileName;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QStringLiteral("ERROR: could not write to %1").arg(path);

    QTextStream out(&f);
    for (const QString &line : std::as_const(m_logEntries))
        out << line << '\n';

    appendLog(timestamp() + QStringLiteral("  Log saved to ") + path);
    return path;
}

void HmInstance::resetStreamingState()
{
    m_logTimer.stop();
    if (m_dataRateHz != 0.0)    { m_dataRateHz = 0.0; emit dataRateHzChanged(); }
    if (m_batteryPercent != -1) { m_batteryPercent = -1; emit batteryPercentChanged(); }
    // ⚠ The UI must not offer "calibrate" against a link that has gone: the call
    // would be refused with HM_ERR_NO_STREAM. HM_EV_STREAM_STOPPED normally clears
    // this on its own, but a link that simply vanished may never produce one.
    if (m_streaming) { m_streaming = false; emit streamingChanged(); }
    // Back to NaN rather than left at its last value: this readout is only
    // interpretable at rest in a known pose, and the last value from a link that
    // has gone away is neither. NaN is the reading that cannot be misread.
    if (!std::isnan(m_relativeAngleDeg)) {
        m_relativeAngleDeg = std::numeric_limits<double>::quiet_NaN();
        emit relativeAngleChanged();
    }
    // ⚠ The CALIBRATION surface is deliberately NOT cleared here. It is driven from
    // the library's own HM_EV_LINK_DOWN, which is the only thing that also knows
    // WHICH state to move to — see invalidateCalibration().
}

int HmInstance::retryDelayMs(int attempt) const
{
    // Exponential backoff from kRetryBaseDelayMs (attempt 1 = base), capped at
    // kRetryMaxDelayMs: 2s, 4s, 8s, 16s, 30s(cap)…
    const long long d = static_cast<long long>(kRetryBaseDelayMs) << (attempt - 1);
    return static_cast<int>(qMin<long long>(d, kRetryMaxDelayMs));
}

void HmInstance::handleConnectFailure()
{
    // Exactly one call per failed connect attempt (whichever of the platform's
    // error/disconnect pair landed first consumed m_connecting in
    // onConnectionLost).
    if (m_retrySuppressed) {
        // The library already said a retry cannot help. Nothing is scheduled and
        // the reason has already been logged by the retryUseless handler.
        m_retryCount = 0;
        setStateLabel(QStringLiteral("Error"));
        if (m_busy) { m_busy = false; emit busyChanged(); }
        return;
    }

    // ⚠ AN ATTEMPT THAT NEVER REACHED GATT IS NOT RETRIED, AND THAT IS A
    // PROPERTY OF THIS DEVICE RATHER THAN A POLICY PREFERENCE.
    //
    // The wG3 advertises for only a FEW SECONDS after a physical button press
    // (§2.1), and if it has been asleep the first press only wakes it. So a
    // connect that never saw an advertisement failed because nothing was
    // advertising — and no amount of host-side retrying makes an asleep sensor
    // start. The Witmotion ladder (4 retries, 2/4/8/16 s backoff) is right for a
    // sensor that advertises continuously while powered; here it turns one
    // honest 20 s timeout into ~130 s of "Connecting…" / "Retrying…" that cannot
    // succeed, which reads as a hang and hides the ONE thing the user must do.
    //
    // A drop AFTER GATT came up is different in kind: the device was
    // demonstrably awake, so it keeps the full ladder below.
    if (!m_gattReachedThisAttempt) {
        m_retryCount = 0;
        setStateLabel(QStringLiteral("Not found"));
        if (m_busy) { m_busy = false; emit busyChanged(); }
        appendLog(timestamp()
                  + QStringLiteral("  No connection — the sensor never advertised."
                                   " It is asleep or switched off. Press its button,"
                                   " pause, press again, then Connect."));
        return;
    }

    if (m_retryCount < kMaxRetries) {
        ++m_retryCount;
        const int delayMs = retryDelayMs(m_retryCount);
        setStateLabel(QStringLiteral("Retrying %1/%2…").arg(m_retryCount).arg(kMaxRetries));
        if (!m_busy) { m_busy = true; emit busyChanged(); }   // still working
        appendLog(timestamp()
                  + QStringLiteral("  Link lost — auto-retry %1/%2 in %3 s")
                    .arg(m_retryCount).arg(kMaxRetries).arg((delayMs + 999) / 1000));
        m_retryTimer.start(delayMs);
    } else {
        m_retryCount = 0;
        setStateLabel(QStringLiteral("Error"));
        if (m_busy) { m_busy = false; emit busyChanged(); }
        appendLog(timestamp()
                  + QStringLiteral("  Connection failed — all %1 retries exhausted."
                                   " The wG3 advertises for only a few seconds after a"
                                   " button press: press, pause, press again, then retry.")
                    .arg(kMaxRetries));
    }
}

void HmInstance::onConnectionLost(bool fromError)
{
    m_attemptingConn = false;
    resetStreamingState();
    if (m_connected) { m_connected = false; emit imuConnectedChanged(); }

    if (m_connecting) {
        // An unresolved connect attempt failed. The platform pairs errorOccurred()
        // and disconnected() in EITHER order; whichever lands first consumes
        // m_connecting and owns the retry decision, so it is taken exactly once
        // (the ordering fix documented at imu_instance.h:236-243, replicated
        // rather than reinvented).
        m_connecting = false;
        handleConnectFailure();
    } else if (m_retryTimer.isActive()) {
        // The paired signal already scheduled a retry — this is the second half
        // of the pair. Leave the retry, the "Retrying…" label and busy intact.
        const int remainSec = (m_retryTimer.remainingTime() + 999) / 1000;
        appendLog(timestamp()
                  + QStringLiteral("  BLE %1 (post-failure cleanup) — retry %2/%3 still due in ~%4 s")
                    .arg(fromError ? QStringLiteral("error") : QStringLiteral("disconnected"))
                    .arg(m_retryCount).arg(kMaxRetries).arg(remainSec));
    } else {
        // A genuine disconnect outside any connect attempt (user disconnect,
        // mid-stream drop, or retries already exhausted).
        m_retryCount = 0;
        if (m_busy) { m_busy = false; emit busyChanged(); }
        setStateLabel(fromError ? QStringLiteral("Error") : QStringLiteral("Disconnected"));
    }
}

void HmInstance::onTransportState(int state)
{
    switch (static_cast<BleImuTransport::State>(state)) {
    case BleImuTransport::State::Disconnected:
        // ⚠ Whatever the display said, the device's calibration did not survive
        // this. The library drives every subsequent sample to HM_CAL_UNCALIBRATED
        // on link-down and the routine must be re-run (§8.3), and the UI is now
        // driven to match: the worker's HM_EV_LINK_DOWN handler emits
        // calibrationLost() and invalidateCalibration() empties the whole surface.
        //
        // ⚠ IT IS NOT DONE FROM HERE, and that is deliberate rather than lazy. This
        // handler knows only that the transport went away; the LIBRARY knows which
        // calibration state that leaves behind — UNCALIBRATED for a link-down,
        // UNKNOWN for a stream restart — and those are different claims. Driving it
        // from here would mean choosing one of them without the evidence.
        onConnectionLost(/*fromError=*/false);
        break;

    case BleImuTransport::State::Scanning:
        setStateLabel(QStringLiteral("Scanning…"));
        if (!m_busy) { m_busy = true; emit busyChanged(); }
        // The wG3 advertises for only a few seconds after a physical button
        // press, so a scan that finds nothing is usually a timing race rather
        // than an absent device.
        appendLog(timestamp() + QStringLiteral("  Scanning for the sensor — press its button if it is asleep…"));
        break;

    case BleImuTransport::State::Connecting:
        setStateLabel(QStringLiteral("Connecting…"));
        appendLog(timestamp() + QStringLiteral("  Connecting…"));
        break;

    case BleImuTransport::State::DiscoveringServices:
        setStateLabel(QStringLiteral("Discovering services…"));
        appendLog(timestamp() + QStringLiteral("  Discovering BLE services…"));
        break;

    case BleImuTransport::State::Ready:
        // GATT is up. The library's bring-up runs from here and announces itself
        // with HM_EV_READY, at which point the worker starts the stream.
        setStateLabel(QStringLiteral("Connected"));
        m_attemptingConn  = false;
        m_connecting      = false;
        m_retryCount      = 0;
        m_retrySuppressed = false;
        m_gattReachedThisAttempt = true;
        m_connected = true;
        m_busy      = false;
        emit imuConnectedChanged();
        emit busyChanged();
        m_totalSamples    = 0;
        m_samplesSinceLog = 0;
        m_logTimer.start(kLogIntervalMs);
        // The device vibrates when the link comes up (§9.5) — that is the user's
        // own confirmation, and worth saying so in the log.
        appendLog(timestamp() + QStringLiteral("  Link up — the sensor should have vibrated. Bringing the session up…"));
        break;

    case BleImuTransport::State::Error:
        // A connect-phase failure (m_connecting) or a mid-stream controller error
        // (m_connected) is a connection loss — route through the shared path so
        // the retry decision stays order-independent vs the paired disconnect.
        if (m_connecting || m_connected) {
            onConnectionLost(/*fromError=*/true);
        } else if (!m_retryTimer.isActive()) {
            m_attemptingConn = false;
            if (m_busy) { m_busy = false; emit busyChanged(); }
            setStateLabel(QStringLiteral("Error"));
        }
        break;
    }
}

QString HmInstance::timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const qint64 us_total = duration_cast<microseconds>(now.time_since_epoch()).count();
    const qint64 secs = us_total / 1'000'000;
    const int    frac = static_cast<int>(us_total % 1'000'000);
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
    return dt.toString(QStringLiteral("HH:mm:ss"))
           + QLatin1Char('.')
           + QString::number(frac).rightJustified(6, QLatin1Char('0'));
}

void HmInstance::appendLog(const QString &text)
{
    m_logEntries.append(text);
    emit logEntryAdded(text);
}

void HmInstance::setStateLabel(const QString &s)
{
    if (m_stateLabel == s) return;
    m_stateLabel = s;
    emit stateLabelChanged();
}
