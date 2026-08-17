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

#include <hackmotion/hackmotion.h>

#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QtMath>

#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

// libhackmotion hands out UUIDs as 16 raw bytes and owns their formatting; Qt
// wants a string. Marshal through the library's own formatter rather than
// reordering the bytes here — the same division of labour device_enumerator.cpp
// uses in the opposite direction with hm_uuid_parse().
static QBluetoothUuid toQtUuid(const hm_uuid &uuid)
{
    char text[HM_UUID_STRING_SIZE];
    hm_uuid_format(&uuid, text, sizeof text);
    return QBluetoothUuid(QString::fromLatin1(text));
}

// Euler angles FOR DISPLAY ONLY, derived from the quaternion because the device
// streams no Euler triple of its own.
//
// ⚠ This is not an anatomical decomposition and must never be used as one. The
// streamed quaternion maps world→body (§6.7), so these are the angles of the
// world frame expressed in the sensor's, and the constant rotation between the
// device's anatomical convention and ours is unsolved until Phase D. They exist
// so a diagnostic panel can show the same three numbers for either device kind.
static void quatToEulerDeg(float w, float x, float y, float z,
                           float &rollDeg, float &pitchDeg, float &yawDeg)
{
    const float sinRoll = 2.0f * (w * x + y * z);
    const float cosRoll = 1.0f - 2.0f * (x * x + y * y);
    rollDeg = qRadiansToDegrees(std::atan2(sinRoll, cosRoll));

    // asin's domain is fragile against Q14 rounding, which can push the argument
    // a whisker past ±1 and return NaN for a perfectly ordinary pose.
    const float sinPitch = qBound(-1.0f, 2.0f * (w * y - z * x), 1.0f);
    pitchDeg = qRadiansToDegrees(std::asin(sinPitch));

    const float sinYaw = 2.0f * (w * z + x * y);
    const float cosYaw = 1.0f - 2.0f * (y * y + z * z);
    yawDeg = qRadiansToDegrees(std::atan2(sinYaw, cosYaw));
}

// ---------------------------------------------------------------------------
// HmSessionWorker — the I/O-thread resident
// ---------------------------------------------------------------------------
//
// ⚠ THE LIBRARY'S THREADING CONTRACT IS ABSOLUTE (session.h:6-21): every
// hm_session_* call must come from ONE thread for the session's whole life.
// libhackmotion contains no locks, no atomics and no threads, so this is not
// advice. Everything that touches the session — creation, bytes in, ticks,
// every drain, close and destroy — happens here, on ImuManager's shared IMU I/O
// thread. The only thing that crosses the boundary is snapshot(), which is a
// copy out from under a mutex, and the queued signals below.
//
// The host loop this implements is the one at the top of session.h:
//
//     on transport bytes ......... hm_session_on_bytes(s, p, n, now)
//     on link up/down ............ hm_session_on_link_up / _on_link_down
//     arm one timer to ........... hm_session_next_due_us(s)
//     when it fires .............. hm_session_tick(s, now)
//     then always ................ drain poll_writes / poll_events / poll_live
//
// That is the whole integration. The library never pushes; the host drains.
class HmSessionWorker : public QObject
{
    Q_OBJECT

public:
    // One unit's display state. Deliberately not hm_unit_sample: the display
    // needs a dozen floats and copying the full 190-byte sample under the
    // snapshot mutex for every GUI tick would be paying for the raw counts,
    // the tick counters and the pinned masks that nothing here reads.
    struct UnitState {
        float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        float roll = 0.0f, pitch = 0.0f, yaw = 0.0f;
        float velDps = 0.0f;
    };

    struct Snapshot {
        // Monotonic count of live samples decoded since the session was created.
        // The GUI tick compares it against its own last value and does nothing
        // when it has not moved, so a still device costs no notifications at all.
        quint64   seq = 0;
        double    rateHz = 0.0;
        quint64   droppedLive = 0;   // hm_session_dropped_live — never silent
        UnitState unit[HM_UNIT_COUNT];
    };

    explicit HmSessionWorker(const QString &deviceId);
    ~HmSessionWorker() override;

    // Callable from any thread.
    Snapshot snapshot() const;

    // ── I/O thread only ──────────────────────────────────────────────────────
    void initialise();          // create the session and the drain timer
    void connectTo(const QBluetoothDeviceInfo &device, const QBluetoothAddress &adapter);
    void shutdown();            // the stop barrier's I/O-thread half

signals:
    void logLine(const QString &text);
    void transportState(int state);              // BleImuTransport::State, as int
    void mtuTooSmall(int negotiated, int required);
    void deviceVersions(const QString &summary); // "fw 1.2 · proto 7.0 · hw 3.1"
    void batteryPercent(int percent);
    void streamingChanged(bool streaming);
    // The library classified the drop as one no retry can fix. Carries the
    // advice's own name so the log says WHY rather than just "not retrying".
    void retryUseless(const QString &adviceName);

private:
    void onBytes(const QByteArray &data);
    void onTransportStateChanged(BleImuTransport::State state);

    // Every path into the session ends here: drain what it produced, then
    // re-arm from the session's own idea of when it next wants attention.
    void pump();
    void rearm();
    void drainWrites();
    void drainEvents();
    void drainLive();
    void handleEvent(const hm_event &ev);

    static hm_time_us nowUs() { return pinpoint::EventBuffer::nowMicros(); }

    // Batch sizes for the drains. hm_event is 280 bytes and hm_sample ~190, so
    // these are ~9 KB and ~12 KB of stack respectively — drained in a loop until
    // a poll comes back short, so a burst is never left sitting in a ring.
    static constexpr size_t kWriteBatch = 16;
    static constexpr size_t kEventBatch = 32;
    static constexpr size_t kLiveBatch  = 64;

    // Upper bound on how long the drain timer is allowed to sleep, however
    // distant hm_session_next_due_us() says the next deadline is. The keepalive
    // poll is 30 s away for most of a session, and a single no-op tick per
    // second is far cheaper than the failure mode this bounds: a host clock that
    // jumps, or a due time computed from one, cannot park the session for
    // minutes. hm_session_tick() is documented as cheap and idempotent when
    // nothing is due, which is exactly what these extra wakes hit.
    static constexpr int kMaxDueDelayMs = 1'000;

    hm_session      *m_session   = nullptr;
    BleImuTransport *m_transport = nullptr;   // child of this worker — migrates with it
    QTimer          *m_dueTimer  = nullptr;   // child of this worker, for the same reason

    QString m_deviceId;
    bool    m_linkUp    = false;
    bool    m_localStop = false;   // we asked for the disconnect, so classify it as ours

    // Rolling 1 s rate window, mirroring ImuIoWorker's. One entry per decoded
    // sample: a dense burst at the device's full ≈799.2 Hz internal rate (§6.6)
    // makes this ~800 entries, which is a few kilobytes of deque and O(1) either
    // end — the same shape of cost the Witmotion lane already pays at 100 Hz.
    std::deque<qint64> m_liveMs;

    mutable QMutex m_mutex;
    Snapshot       m_snap;
};

HmSessionWorker::HmSessionWorker(const QString &deviceId)
    : m_deviceId(deviceId)
{
    // Both of these are QObject CHILDREN rather than value members, so that
    // moveToThread() carries them onto the I/O thread with us. moveToThread
    // migrates children and nothing else; a QTimer value member would keep GUI
    // affinity and start()ing it from the I/O thread trips "Timers cannot be
    // started from another thread" (the same trap ble_imu_transport.cpp:33-39
    // documents for its connect watchdog).
    m_dueTimer = new QTimer(this);
    m_dueTimer->setSingleShot(true);
    connect(m_dueTimer, &QTimer::timeout, this, [this]() {
        if (!m_session) return;
        hm_session_tick(m_session, nowUs());
        pump();
    });

    // ⚠ The wG3 runs its whole protocol — commands out, stream and replies in —
    // through ONE bidirectional characteristic, and the data characteristic does
    // not sit under the service's UUID base, so the fragment form BleImuTransport
    // uses for Witmotion cannot express this device at all. Explicit UUIDs,
    // matched by equality, with a null write so it resolves onto the notify
    // characteristic. HM_MIN_ATT_MTU is a diagnosis rather than a knob: no Qt
    // platform lets an application request an MTU.
    const BleImuTransport::UuidConfig uuids = BleImuTransport::explicitUuids(
        toQtUuid(HM_UUID_TRANSPARENT_UART_SERVICE),
        toQtUuid(HM_UUID_DATA_CHARACTERISTIC),
        QBluetoothUuid(),
        HM_MIN_ATT_MTU);

    m_transport = new BleImuTransport(uuids, this);

    // The transport and this worker share a thread, so every one of these is a
    // direct call on the I/O thread — the session is only ever touched from here.
    connect(m_transport, &BleImuTransport::dataReceived,
            this,        &HmSessionWorker::onBytes);
    connect(m_transport, &BleImuTransport::stateChanged,
            this,        &HmSessionWorker::onTransportStateChanged);

    connect(m_transport, &BleImuTransport::mtuTooSmall, this,
            [this](int negotiated, int required) {
        // The transport's own gate fires before the library ever sees the link.
        // Both are wired up because they can fail independently: this one on
        // UuidConfig::minAttMtu, HM_EV_MTU_REJECTED on the value we pass into
        // hm_session_on_link_up().
        emit mtuTooSmall(negotiated, required);
    });
    connect(m_transport, &BleImuTransport::mtuChanged, this, [this](int mtu) {
        emit logLine(QStringLiteral("ATT MTU negotiated: %1 bytes (device needs %2)")
                         .arg(mtu).arg(HM_MIN_ATT_MTU));
    });
    connect(m_transport, &BleImuTransport::errorOccurred, this, [this](const QString &msg) {
        emit logLine(QStringLiteral("ERROR: ") + msg);
    });
    connect(m_transport, &BleImuTransport::diagnosticInfo, this, [this](const QString &msg) {
        emit logLine(QStringLiteral("[diag] ") + msg);
    });
}

HmSessionWorker::~HmSessionWorker()
{
    // Runs on the I/O thread: HmInstance deleteLater()s us onto it, and
    // ImuManager joins the thread only after the instances are gone, so the
    // deferred delete always runs. hm_session_destroy() is a hm_session_* call
    // like any other and obeys the same one-thread contract.
    if (m_session) {
        hm_session_destroy(m_session);
        m_session = nullptr;
    }
}

void HmSessionWorker::initialise()
{
    if (m_session) return;

    hm_session_config cfg = hm_session_config_default();

    // ⚠ ONE LINE NOW, NOTHING AT RUNTIME. The digest ring defaults to OFF, and
    // with it off a Phase-E history block reports live_overlap_samples == 0 —
    // which means NO EVIDENCE that the retrieved span agrees with the live one,
    // not agreement. A consumer stitching [live prefix] + [retrieved span] +
    // [live suffix] into one lane depends on that agreement, and a seam where it
    // does not hold looks exactly like a real wrist movement. The pointer stays
    // NULL so the library allocates it inside its single create-time allocation.
    cfg.memory.digest_ring_capacity = HM_DIGEST_RING_RECOMMENDED;

    // ⚠ UNMODIFIED, and keepalive_period_us above all. §9.2: a silent connection
    // is dropped at exactly 5.0 minutes and an ACTIVE STREAM DOES NOT PREVENT IT
    // — only a host→device write resets the device's idle timer. For a golf
    // lesson that is mid-lesson with an athlete standing on a mat. There must be
    // no way for this integration to weaken it, so it does not touch it.
    cfg.policy = hm_session_policy_default();

    // The observed 0x7e. Under it the RELATIVE angle stayed within 0.58° over
    // five minutes while the two units individually drifted 1.95° and 1.13°
    // (§6.2) — and the relative rotation is the entire point of the device.
    cfg.stream_config = hm_stream_config_default();

    // ⚠ NEVER A MAC. macOS CoreBluetooth never exposes one, so the enumerator's
    // id is a per-host UUID there and an address elsewhere; the library only
    // ever labels events and log lines with it.
    const QByteArray idUtf8 = m_deviceId.toUtf8();
    const int idLen = qMin(static_cast<int>(idUtf8.size()), HM_DEVICE_ID_MAX - 1);
    std::memcpy(cfg.device_id, idUtf8.constData(), size_t(idLen));
    cfg.device_id[idLen] = '\0';

    const hm_status st = hm_session_create(&cfg, &m_session);
    if (st < HM_OK) {
        m_session = nullptr;
        emit logLine(QStringLiteral("ERROR: hm_session_create failed: %1")
                         .arg(QString::fromLatin1(hm_status_str(st))));
        ppError() << "[HmInstance]" << m_deviceId << "— hm_session_create failed:"
                  << hm_status_str(st);
        return;
    }

    // hm_version_string() rather than the HM_VERSION_STRING macro: the log
    // should name the library that was LINKED, not the header we compiled with.
    emit logLine(QStringLiteral("libhackmotion session created (%1)")
                     .arg(QString::fromLatin1(hm_version_string())));
    pump();
}

void HmSessionWorker::connectTo(const QBluetoothDeviceInfo &device,
                                const QBluetoothAddress &adapter)
{
    m_localStop = false;
    m_transport->connectToDevice(device, adapter);
}

void HmSessionWorker::shutdown()
{
    // ⚠ THE PRODUCER STOP BARRIER, run under a BlockingQueuedConnection so that
    // when HmInstance::stop() returns nothing the library owns can produce
    // another sample or event. That is structural rather than a promise: the
    // library never pushes, the host drains, and after this the queues are
    // sealed (session.h §2.11).
    m_localStop = true;
    if (m_dueTimer)
        m_dueTimer->stop();

    // Sever the data path first, so no notification arriving during teardown can
    // reach a session that is about to close.
    if (m_transport)
        QObject::disconnect(m_transport, &BleImuTransport::dataReceived, this, nullptr);

    if (m_session && hm_session_is_streaming(m_session)) {
        // Best effort only, and deliberately not waited on: session.h asks the
        // host to write the `83` from stop_stream() for a clean stop, but a stop
        // barrier must never block on a device reply. The link teardown below
        // may well overtake the write — in which case nothing is worse than not
        // having tried, because a disconnect ends the stream anyway.
        const hm_status st = hm_session_stop_stream(m_session);
        if (st < HM_OK)
            emit logLine(QStringLiteral("Stream stop refused: %1")
                             .arg(QString::fromLatin1(hm_status_str(st))));
        drainWrites();
    }

    // Drives onTransportStateChanged(Disconnected) synchronously on this thread,
    // which is what tells the session the link went down — and it is ours, so it
    // is classified HM_LINK_DOWN_LOCAL_REQUEST.
    if (m_transport)
        m_transport->disconnectFromDevice();

    if (m_session) {
        hm_session_close(m_session);
        // ⚠ CLOSE FINISHES WORK, IT DOES NOT DISCARD IT. Events (and, from Phase
        // E, history blocks) stay collectable until destroy(), so one last drain
        // puts the closing classification in the log ring rather than dropping it.
        drainEvents();
    }
}

HmSessionWorker::Snapshot HmSessionWorker::snapshot() const
{
    QMutexLocker lk(&m_mutex);
    return m_snap;
}

void HmSessionWorker::onBytes(const QByteArray &data)
{
    if (!m_session || data.isEmpty()) return;

    // ⚠ ONE CALL, ONE NOTIFICATION. This payload goes in exactly as the
    // transport delivered it: never buffered, never concatenated with the last
    // one, never split. The protocol has no length field, no sequence number and
    // no checksum (§3), so a coalesced byte stream cannot be resynchronised —
    // there is nothing to synchronise TO — and the library does not try. It
    // raises HM_WARN_TRAILING_BYTES on the first frame that is not a whole
    // number of records, which is how a coalescing transport announces itself.
    hm_session_on_bytes(m_session,
                        reinterpret_cast<const uint8_t *>(data.constData()),
                        size_t(data.size()),
                        nowUs());
    pump();
}

void HmSessionWorker::onTransportStateChanged(BleImuTransport::State state)
{
    emit transportState(static_cast<int>(state));

    if (!m_session) return;

    switch (state) {
    case BleImuTransport::State::Ready: {
        // GATT is up and notifications are enabled — the link the library cares
        // about exists from here.
        const int mtu = m_transport->mtu();
        if (mtu <= 0) {
            // ⚠ 0 means "unknown, proceed", which the library treats as a warning
            // (HM_WARN_MTU_UNKNOWN) rather than a failure. Passing a wrong number
            // instead would be a real failure: it is the only thing standing
            // between a 93-byte frame and a truncation that parses as garbage.
            emit logLine(QStringLiteral("ATT MTU not reported by this platform — "
                                        "proceeding unchecked"));
        }
        m_linkUp = true;
        hm_session_on_link_up(m_session, mtu > 0 ? mtu : 0, nowUs());
        pump();
        break;
    }

    case BleImuTransport::State::Disconnected:
    case BleImuTransport::State::Error: {
        if (!m_linkUp) break;   // the attempt never reached GATT; there was no link
        m_linkUp = false;

        // Qt reports THAT the link went away, not why: a supervision timeout, a
        // remote close and an adapter hiccup all arrive as the same
        // disconnected(). So only the two facts we genuinely hold are asserted —
        // that we asked for it, or that the controller errored — and everything
        // else is UNKNOWN. The library classifies far better than a guess would,
        // from how long the link was idle and whether the device is still
        // advertising, and a fabricated cause would poison exactly that.
        const hm_link_down_cause cause =
            m_localStop ? HM_LINK_DOWN_LOCAL_REQUEST
                        : (state == BleImuTransport::State::Error
                               ? HM_LINK_DOWN_TRANSPORT_ERROR
                               : HM_LINK_DOWN_UNKNOWN);

        // ⚠ This ALWAYS invalidates calibration, deliberately. §8.3 measured
        // 0.70° immediately before dropping a link and 18.80° at the same pose
        // after reconnecting, strap untouched — a reconnect that resumed as
        // calibrated would be the worst bug this integration could ship.
        hm_session_on_link_down(m_session, cause, nowUs());
        pump();
        break;
    }

    case BleImuTransport::State::Scanning:
    case BleImuTransport::State::Connecting:
    case BleImuTransport::State::DiscoveringServices:
        break;
    }
}

void HmSessionWorker::pump()
{
    if (!m_session) return;

    // Events first: HM_EV_READY starts the stream, and the write it queues is
    // then picked up by the very next drain rather than waiting for a timer.
    drainEvents();
    drainWrites();
    drainLive();
    rearm();
}

void HmSessionWorker::rearm()
{
    if (!m_session || !m_dueTimer) return;

    // ⚠ RE-ARMED AFTER EVERY CALL INTO THE SESSION, not only after ticks: bytes
    // arriving, a link coming up and a stream starting all move the next
    // deadline, and the session is the only thing that knows where it went.
    const hm_time_us due = hm_session_next_due_us(m_session);
    if (due == HM_TIME_NEVER) {
        // ⚠ "Nothing pending" — stop the timer. Not "fire now": HM_TIME_NEVER is
        // INT64_MAX, so an unsigned or clamped subtraction reads it as due in the
        // past and spins the drain loop at full tilt.
        m_dueTimer->stop();
        return;
    }

    const hm_time_us delayUs = due - nowUs();
    const int delayMs = delayUs <= 0
        ? 0
        : int(qMin<hm_time_us>((delayUs + 999) / 1000, kMaxDueDelayMs));
    m_dueTimer->start(delayMs);
}

void HmSessionWorker::drainWrites()
{
    // ⚠ poll_writes() returns nothing at all while a history bracket is open
    // (Phase E). That is the deliberate write quiet period, not a stall — the
    // `a1` that opened the bracket already reset the device's idle timer — so
    // there is nothing here to watchdog and nothing to "recover".
    hm_write_request reqs[kWriteBatch];
    for (;;) {
        const size_t n = hm_session_poll_writes(m_session, reqs, kWriteBatch);
        for (size_t i = 0; i < n; ++i) {
            const QByteArray payload(reinterpret_cast<const char *>(reqs[i].data),
                                     int(reqs[i].length));
            // The library decides per write whether it wants an acknowledgement;
            // the characteristic supports both modes, so honour it rather than
            // letting the transport pick from the properties alone.
            m_transport->writeToDevice(payload, reqs[i].without_response != 0);
        }
        if (n < kWriteBatch) break;
    }
}

void HmSessionWorker::drainEvents()
{
    hm_event evs[kEventBatch];
    for (;;) {
        const size_t n = hm_session_poll_events(m_session, evs, kEventBatch);
        for (size_t i = 0; i < n; ++i)
            handleEvent(evs[i]);
        if (n < kEventBatch) break;
    }
}

void HmSessionWorker::handleEvent(const hm_event &ev)
{
    // ⚠ PERSONAL DATA NEVER REACHES THE LOG RING. HM_EV_IDENTITY carries the
    // device's MAC and serial, which name a specific unit and a specific owner;
    // hm_event_is_sensitive() identifies exactly those events so a log sink does
    // not have to think about it. They are dropped here rather than redacted,
    // because a log line saying an identity arrived is worth nothing to a coach.
    if (hm_event_is_sensitive(&ev))
        return;

    const hm_event_type type = static_cast<hm_event_type>(ev.type);

    char text[192];
    hm_event_format(&ev, text, sizeof text, /*include_identifiers=*/false);
    QString line = QString::fromUtf8(text);
    bool log = true;

    switch (type) {
    case HM_EV_READY: {
        // ONE STREAM, OPENED ONCE, LEFT OPEN (api-request B8): calibration needs
        // it open, the clock fit needs it continuous, and history works in place,
        // so nothing ever requires stopping it. Phase A wants the cubes moving.
        const hm_status st = hm_session_start_stream(m_session);
        if (st < HM_OK)
            line += QStringLiteral("  — start_stream failed: %1")
                        .arg(QString::fromLatin1(hm_status_str(st)));
        break;
    }

    case HM_EV_MTU_REJECTED:
        // The library's own gate, on the value we passed to on_link_up().
        emit mtuTooSmall(ev.u.mtu, HM_MIN_ATT_MTU);
        break;

    case HM_EV_DEVICE_INFO: {
        const hm_device_info &info = ev.u.device_info;
        if (info.valid & HM_INFO_VERSIONS) {
            emit deviceVersions(QStringLiteral("fw %1.%2 · proto %3.%4 · hw %5.%6")
                                    .arg(info.firmware_major).arg(info.firmware_minor)
                                    .arg(info.protocol_major).arg(info.protocol_minor)
                                    .arg(info.hardware_major).arg(info.hardware_minor));
        }
        break;
    }

    case HM_EV_BATTERY:
        emit batteryPercent(int(ev.u.battery.percent));
        break;

    case HM_EV_STREAM_STARTED:
        // ⚠ The first 0x90 FRAME, not the `a0 01 7e` acknowledgement — data is
        // genuinely flowing by the time this arrives.
        emit streamingChanged(true);
        break;

    case HM_EV_STREAM_STOPPED:
        emit streamingChanged(false);
        break;

    case HM_EV_STREAM_RESTARTED:
        // ⚠ Reported, never silent (api-request B8): a restart drops
        // HM_CAL_CALIBRATED to HM_CAL_UNKNOWN, so a calibration does not survive
        // it and the routine has to be re-run.
        ppWarn() << "[HmInstance]" << m_deviceId << "— stream restarted; calibration is no longer known";
        break;

    case HM_EV_LINK_DOWN: {
        const hm_recovery_advice advice =
            static_cast<hm_recovery_advice>(ev.u.link_down.advice);
        line += QStringLiteral("  [%1 → %2]")
                    .arg(QString::fromLatin1(hm_link_down_cause_name(
                             static_cast<hm_link_down_cause>(ev.u.link_down.cause))))
                    .arg(QString::fromLatin1(hm_recovery_advice_name(advice)));

        // Three advices mean a retry cannot help at any interval: the device
        // slept and only a physical button press wakes it, another application
        // holds the one connection the device accepts, or we powered it off
        // ourselves. Retrying against any of them burns battery, occupies the
        // adapter and shows the user a spinner for a problem only they can fix.
        if (advice == HM_RECOVER_NEEDS_BUTTON_PRESS
            || advice == HM_RECOVER_NEEDS_OTHER_APP_CLOSED
            || advice == HM_RECOVER_DO_NOT_RETRY) {
            emit retryUseless(QString::fromLatin1(hm_recovery_advice_name(advice)));
        }
        break;
    }

    case HM_EV_WARNING:
        // hm_event_format already renders the code; the ppWarn gets it into the
        // application log too, where a support bundle will find it.
        ppWarn() << "[HmInstance]" << m_deviceId << "— warning:"
                 << hm_warning_code_name(static_cast<hm_warning_code>(ev.u.warning.code));
        break;

    case HM_EV_CLOCK_DEGRADED:
    case HM_EV_DEVICE_ERROR:
        ppWarn() << "[HmInstance]" << m_deviceId << "—" << hm_event_type_name(type)
                 << ":" << text;
        break;

    case HM_EV_CLOCK_UPDATED:
        // Periodic housekeeping at 1 Hz. It would fill the log ring a coach
        // reads with the one event that never needs acting on; HM_EV_CLOCK_DEGRADED
        // is the half that matters and is logged above.
        log = false;
        break;

    default:
        break;
    }

    if (log)
        emit logLine(line);
}

void HmSessionWorker::drainLive()
{
    hm_sample samples[kLiveBatch];
    quint64   decoded = 0;
    bool      haveLatest = false;
    hm_sample latest{};

    const qint64 nowMs = nowUs() / 1000;

    for (;;) {
        const size_t n = hm_session_poll_live(m_session, samples, kLiveBatch);
        for (size_t i = 0; i < n; ++i) {
            // Rolling 1 s rate window, on ARRIVAL rather than on the sample's
            // mapped host time: the mapped time is what the clock fit says, and
            // before the fit has observations it is not available at all — while
            // what this number is for is "is data flowing, and how fast".
            m_liveMs.push_back(nowMs);
        }
        if (n > 0) {
            decoded += n;
            latest = samples[n - 1];
            haveLatest = true;
        }
        if (n < kLiveBatch) break;
    }

    while (!m_liveMs.empty() && m_liveMs.front() < nowMs - 1000)
        m_liveMs.pop_front();

    if (!haveLatest)
        return;

    const qint64 winMs = m_liveMs.size() > 1 ? nowMs - m_liveMs.front() : 0;
    const double rateHz = winMs > 0 ? double(m_liveMs.size()) * 1000.0 / double(winMs)
                                    : 0.0;

    // Only the newest sample reaches the display; everything in between was
    // already counted into the rate. ⚠ The hot path must not emit a signal per
    // sample — a burst at the device's full ≈799.2 Hz would post ~1,600 property
    // notifications a second at the GUI thread — so nothing is emitted from here
    // at all. The 60 Hz display tick on the GUI thread reads this snapshot.
    Snapshot next;
    const hm_unit_sample *const blocks[HM_UNIT_COUNT] = { &latest.lower_arm, &latest.palm };
    for (int u = 0; u < HM_UNIT_COUNT; ++u) {
        const hm_unit_sample &b = *blocks[u];
        UnitState &s = next.unit[u];

        // ⚠ EXACTLY AS IT ARRIVES. q_world_to_body is the conjugate of what most
        // IMU code assumes (§6.7) and is stored unconjugated and unremapped. The
        // convention question belongs to Phase D together with the frame solve;
        // "the cube looks better this way" is precisely the reasoning that bakes
        // in a guess nothing downstream can check.
        s.qw = b.q_world_to_body[0];
        s.qx = b.q_world_to_body[1];
        s.qy = b.q_world_to_body[2];
        s.qz = b.q_world_to_body[3];

        // Gravity-removed LINEAR acceleration, m/s², in its native units and its
        // native axes. The two units sit at different radii and are SUPPOSED to
        // disagree under rotation by ~ω²r — that is not a fault, which is why
        // this state is per unit and there is no aggregate above it.
        s.ax = b.linear_accel_mps2[0];
        s.ay = b.linear_accel_mps2[1];
        s.az = b.linear_accel_mps2[2];

        quatToEulerDeg(s.qw, s.qx, s.qy, s.qz, s.roll, s.pitch, s.yaw);

        // The device's own rate channel, not a quaternion difference: there is
        // nothing to estimate here.
        s.velDps = std::sqrt(b.gyro_dps[0] * b.gyro_dps[0]
                           + b.gyro_dps[1] * b.gyro_dps[1]
                           + b.gyro_dps[2] * b.gyro_dps[2]);
    }

    QMutexLocker lk(&m_mutex);
    next.seq         = m_snap.seq + decoded;
    next.rateHz      = rateHz;
    // Non-zero means the host did not keep up and live samples were lost. It is
    // surfaced in the 10 s summary rather than swallowed.
    next.droppedLive = hm_session_dropped_live(m_session);
    m_snap = next;
}

// ---------------------------------------------------------------------------
// HmUnit
// ---------------------------------------------------------------------------

HmUnit::HmUnit(const QString &deviceId, hm_unit unit, QObject *parent)
    : QObject(parent)
    , m_unit(unit)
    // ⚠ NOT TRANSLATED, and spelled once here: this string becomes the
    // EventBuffer SourceDescriptor::identifier in Phase B and the
    // AppSettings::imuPlacement key in Phase C, and both are persisted. A
    // localised identifier would silently split one device's history in two the
    // first time the application ran in another language.
    , m_unitId(deviceId + (unit == HM_UNIT_PALM ? QStringLiteral("#palm")
                                                : QStringLiteral("#lowerArm")))
    // Display only, so this one IS translated.
    , m_unitLabel(unit == HM_UNIT_PALM ? tr("Palm") : tr("Lower arm"))
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
    // anything has been measured.
    m_presenceAngleDeg = std::numeric_limits<double>::quiet_NaN();
    m_calibrationPhase = HM_CALP_IDLE;

    // Phase B registers two EventBuffer sources here and hands each id to its
    // HmUnit. Nothing is registered in Phase A on purpose — see the header.
    Q_UNUSED(m_eventBuffer)

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
        // Only while the link is up: a stream stopping BECAUSE the link went
        // away must not overwrite "Disconnected" with "Connected".
        if (!m_connected) return;
        setStateLabel(streaming ? QStringLiteral("Streaming") : QStringLiteral("Connected"));
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
        appendLog(timestamp()
            + QStringLiteral("  Data: %1 samples total  (+%2 in last 10s)  %3 Hz avg%4%5")
                .arg(m_totalSamples)
                .arg(m_samplesSinceLog)
                .arg(m_dataRateHz, 0, 'f', 1)
                .arg(bat)
                .arg(dropped));
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
    // Empty in Phase A, and empty on purpose: registering a source nothing ever
    // writes to would put a permanently silent lane in the data viewer and a
    // stale binding candidate in the wizard. Phase B registers two.
    return {};
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
    // Phase B fills this in, once there are sources to deregister. Nothing is
    // registered in Phase A, so there is nothing to undo.
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
        // this. The library drives every subsequent sample to
        // HM_CAL_UNCALIBRATED on link-down and the routine must be re-run (§8.3);
        // Phase C is what surfaces that in the UI.
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

#include "hm_instance.moc"
