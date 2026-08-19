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
// The pure hm_unit_sample → ImuSample converter (src/IMU). It owns the m/s² → g
// conversion from the raw mg counts and passes gyro and the quaternion through
// verbatim; it is unit-agnostic, so both blocks go through the same call.
#include "hm_frame.h"
#include "hm_sample_convert.h"
#include "hm_unit_id.h"
// The skew median and the §10.3 half-split stability test — pure, and pure so that
// the statistic gating a "not stable" warning can be tested without hardware.
#include "hm_skew_stats.h"

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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>

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

// ⚠ COMPONENT ORDER, AND IT IS THE ONE PLACE IT CAN GO WRONG SILENTLY. Every
// quaternion the library hands out is a float[4] in (w, x, y, z) order
// (quat.h:30, sample.h:97); QQuaternion's constructor takes (scalar, x, y, z).
// So this mapping is index-for-index — and a transposition here would not throw,
// would not fail a norm check, and would land as a FIXED ROTATION ERROR in every
// reading Phase D ever produces from the anchor. It is spelled once, here.
//
// ⚠ NOTHING ELSE HAPPENS TO THE VALUE. No normalisation, no conjugation, no axis
// re-ordering: the streamed quaternion maps world→body (§6.7), the conjugate of
// what most IMU code assumes, and whether the solve feeds it raw or conjugated is
// a binary choice Phase D settles with a single-axis test rather than by
// inspection. Storing the library's measurement exactly as it arrived is what
// keeps that choice available.
static QQuaternion toQQuaternion(const float q[4])
{
    return QQuaternion(q[0], q[1], q[2], q[3]);
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
        // ⚠ The gyro VECTOR, not just the magnitude above. Phase D reads the
        // pronation rate off the lower-arm unit's component about the limb axis,
        // which a magnitude has already thrown away.
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;
    };

    struct Snapshot {
        // Monotonic count of live samples decoded since the session was created.
        // The GUI tick compares it against its own last value and does nothing
        // when it has not moved, so a still device costs no notifications at all.
        quint64   seq = 0;
        double    rateHz = 0.0;
        quint64   droppedLive = 0;   // hm_session_dropped_live — never silent
        UnitState unit[HM_UNIT_COUNT];

        // ── Ring-write accounting, copied out of the worker's own counters ────
        quint64 written[HM_UNIT_COUNT] = { 0, 0 };
        // Samples the ring never saw because they carried no mapped host time.
        // ⚠ NOT the obsolete "drop the first two seconds" rule: the clock fit
        // exists from the first live frame (brief §0 #1) and HM_SAMPLE_NO_FIT
        // fires only before it, so a non-zero count here means something is
        // wrong — which is exactly why it is counted rather than assumed zero.
        quint64 noFitSkipped = 0;
        // Samples whose host_time_us did not advance past the last one written to
        // that source, and the largest such backward step. MEASUREMENT ONLY — the
        // merger owns the clamping (see writeSample()).
        quint64 nonMonotonic[HM_UNIT_COUNT]  = { 0, 0 };
        qint64  maxBackStepUs[HM_UNIT_COUNT] = { 0, 0 };
        // skew_us provenance: min / MEDIAN / max of palm − lower_arm over the
        // session. Never applied to a timestamp — see HmInstance::skewUsMedian().
        //
        // ⚠ A MEDIAN, NOT A MEAN, AND THAT IS THE WHOLE POINT OF THIS FIELD. A
        // single record's tick difference is dominated by ±½-sample PAIRING JITTER
        // — libhackmotion measured 89 and 99 ticks on two consecutive records of
        // one capture against a session median of 59 (§10.3) — so the stable
        // 0.92 ms is a session-level figure that only appears after aggregating,
        // and a mean carries every one of those outliers into the answer.
        // ⚠ NO MEDIAN HERE. This snapshot is rebuilt on every drain — BLE notification
        // rate — and a median needs the stored values, so computing one per pass would
        // put an O(n) pass over 16 k samples on the hot path to answer a question
        // nothing reads at that rate. See HmSessionWorker::skewStats(), computed on
        // demand by the export path and the 10 s summary.
        quint64 skewCount     = 0;   // every sample seen
        qint32  skewMinUs     = 0;
        qint32  skewMaxUs     = 0;
        // Inter-unit relative angle in degrees — see writeSample() for the bands.
        // `Now` is the LATEST sample's value and is the one to read while holding a
        // pose; the session band behind it is polluted by every movement since the
        // link came up, which is the wrong shape for "hold still and read it".
        // ⚠ NaN, not 0, until a sample has been decoded: 0° is what a perfectly
        // applied calibration approaches, so a default of 0 would read as the best
        // possible number before anything had arrived. drainLive() never publishes a
        // snapshot without a sample, so the default is only ever seen by a reader
        // that ignores it anyway — which is the point of making it unmistakable.
        float   relAngleNowDeg = std::numeric_limits<float>::quiet_NaN();
        quint64 relAngleCount  = 0;
        double  relAngleSumDeg = 0.0;
        float   relAngleMinDeg = 0.0f;
        float   relAngleMaxDeg = 0.0f;
    };

    explicit HmSessionWorker(const QString &deviceId);
    ~HmSessionWorker() override;

    // Callable from any thread.
    Snapshot snapshot() const;

    // ── Deferred history (Phase E) ──────────────────────────────────────────
    // reserveHistory() must run ON the I/O thread (it is a hm_session_* call) and
    // is posted there by HmInstance. The other two are read from the GUI thread
    // while the gather polls, so they synchronise: an atomic flag and a
    // mutex-held result, the same shape snapshot() already uses.
    void reserveHistory(qint64 impactUs, qint64 deadlineUs);
    bool historyPending() const {
        return m_historyPending.load(std::memory_order_acquire);
    }
    HmInstance::HistoryResult takeHistoryResult() {
        QMutexLocker lk(&m_mutex);
        return std::move(m_historyResult);
    }

    // ⚠ CALLED FROM HmInstance's CONSTRUCTOR, BEFORE moveToThread(), and that is
    // what makes it safe without a lock: at that instant this object still has
    // the constructing thread's affinity, nothing else holds a reference to it,
    // and the moveToThread() + queued initialise() that follow establish the
    // happens-before for every later I/O-thread read. There is no attach point
    // later in the life cycle for the same reason there is no detach one: the
    // buffer pointer is only ever cleared from shutdown(), on the I/O thread.
    void attachBuffer(pinpoint::EventBuffer *buffer,
                      const pinpoint::SourceId ids[HM_UNIT_COUNT]);

    // The reference-pose anchor of the most recent presence measurement, copied
    // out from under the same mutex snapshot() uses — so it is callable from any
    // thread. ⚠ EVENTED AND QUERIED BOTH, deliberately: the anchor cannot be
    // re-derived once the pose has passed (capturing it again from the live stream
    // would anchor on an instant that is only NEARLY the one measured, and
    // "nearly" becomes a fixed rotation error in every later reading), so the
    // signal below carries no payload and the value is fetched from here instead
    // of being marshalled through a metatype.
    HmInstance::ReferenceAnchor referenceAnchor() const;

    // Window-scoped capture provenance (Phase B′). Callable from any thread; copies
    // out from under the same mutex snapshot() uses.
    HmInstance::CaptureProvenance captureProvenance(qint64 windowStartUs,
                                                    qint64 windowEndUs) const;

    // ── The inter-unit skew, as a median rather than a mean ───────────────────
    //
    // ⚠ COMPUTED ON DEMAND, NOT PER DRAIN. A median needs the stored values, so it
    // costs an O(n) pass; the callers are the export path and the 10 s summary, both
    // of which ask rarely. Callable from any thread.
    struct SkewStats {
        quint64 total  = 0;    // every sample seen this session
        quint64 stored = 0;    // …of which this many are behind the figures below.
                               // ⚠ stored < total is a TRUNCATION, and a truncation
                               // nobody reports reads as coverage.
        double  medianUs = 0.0;
        // ⚠ §10.3's stability claim, tested the way §10.3 tested it: the median of
        // the first half of the run minus the median of the second. That measured
        // IDENTICAL medians across the halves of a 238 s session, so a non-zero value
        // here is the offset genuinely moving — which is the thing Phase E/G may not
        // assume away. ⚠ The min/max SPREAD cannot do this job: single-record values
        // scatter by ±½-sample pairing jitter (~1250 µs at the device's internal
        // rate) on a perfectly healthy device, so a spread threshold fires on noise.
        double  halfSplitDeltaUs = 0.0;
        bool    halfSplitValid   = false;   // false when the run is too short to split
    };
    SkewStats skewStats() const;

    // ── I/O thread only ──────────────────────────────────────────────────────
    void initialise();          // create the session and the drain timer
    void connectTo(const QBluetoothDeviceInfo &device, const QBluetoothAddress &adapter);
    void shutdown();            // the stop barrier's I/O-thread half

    // ── The calibration routine — I/O thread only, reached by QueuedConnection ─
    //
    // ⚠ THE ORDER IS FIXED BY THE LIBRARY and is not enforced again here: it
    // refuses an out-of-order step with HM_ERR_INVALID_STATE, and duplicating that
    // check would give us a second, divergent state machine to keep in step with
    // the first. Every one of these therefore does the same three things — call,
    // surface a refusal, pump — and the library owns the sequencing.
    void beginCalibration();
    void confirmHorizontal();
    void confirmRaise();
    void confirmReferencePose();
    void abortCalibration();

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

    // ── Calibration, GUI-bound ───────────────────────────────────────────────
    //
    // ⚠ EVERY ONE OF THESE CARRIES `libraryState`, READ BACK FROM
    // hm_session_calibration_state() AT THE MOMENT THE EVENT WAS DRAINED. The
    // phase and the state answer different questions and neither is derivable from
    // the other — HM_CALP_COMPLETE with HM_CAL_UNKNOWN is the ordinary outcome of a
    // routine whose presence check was skipped — so the state is always read and
    // never inferred from the phase.
    void calibrationPhaseEvent(int phase, int previousPhase, int abortReason,
                               int libraryState, qint64 elapsedUs);
    // A presence measurement landed. The values are in referenceAnchor().
    void calibrationPresenceMeasured(int libraryState);
    // HM_WARN_PRESENCE_NOT_MEASURED: the check was ASKED FOR and could not be
    // taken. ⚠ Its own signal because the phase still reaches HM_CALP_COMPLETE and
    // "we could not check" must never end up in the same recording as "we checked
    // and it was fine". `samplesCollected` is how many arrived.
    void calibrationPresenceUnmeasured(int samplesCollected, int libraryState);
    // HM_WARN_CALIBRATION_ABSENT / _INDETERMINATE. Both arrive alongside a presence
    // measurement and only refine what the library knows — ABSENT drives the state
    // to UNCALIBRATED, INDETERMINATE leaves it exactly as it was — so this carries
    // the state and nothing else. Order against the presence event does not matter:
    // both re-read the library rather than deciding anything themselves.
    void calibrationStateRefreshed(int libraryState);
    // ⚠ THE CALIBRATION IS GONE. Link-down (always) or a stream restart.
    void calibrationLost(int libraryState, const QString &why);
    // The hm_status of a refused hm_calibration_* call, with the call's name. The
    // queued hop means this is the ONLY way a refusal can reach the GUI thread.
    void calibrationCallRefused(int status, const QString &call);

private:
    void onBytes(const QByteArray &data);
    void onTransportStateChanged(BleImuTransport::State state);

    // The shared body of the five calibration entry points: make the call, turn a
    // refusal into calibrationCallRefused(), then pump.
    void calibrationCall(hm_status (*fn)(hm_session *), const char *name);

    // Every path into the session ends here: drain what it produced, then
    // re-arm from the session's own idea of when it next wants attention.
    void pump();
    // Runs on the I/O thread only, from pump().
    void pollHistory();
    void rearm();
    void drainWrites();
    void drainEvents();
    void drainLive();
    void writeSample(const hm_sample &s);   // one hm_sample → two ring writes
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

    // ── The recording path. I/O-thread state throughout: the writes happen here
    // and shutdown() — which clears the pointer — is delivered here too, under a
    // BlockingQueuedConnection, so the stop barrier needs no producer-side lock
    // (ImuIoWorker takes one only because its detachBuffer() is called from the
    // GUI thread). One id per unit; kInvalidSourceId means that unit's
    // registration failed and its lane simply does not record.
    pinpoint::EventBuffer *m_buffer = nullptr;
    pinpoint::SourceId     m_sourceIds[HM_UNIT_COUNT] =
        { pinpoint::kInvalidSourceId, pinpoint::kInvalidSourceId };

    // Cumulative counters. They live here rather than in m_snap because
    // drainLive() rebuilds the snapshot from scratch each pass and would
    // otherwise reset them; the snapshot carries a copy out.
    quint64 m_written[HM_UNIT_COUNT] = { 0, 0 };
    quint64 m_noFitSkipped = 0;

    // Per-unit last host time actually written, for the monotonicity measurement.
    // HM_TIME_UNKNOWN (INT64_MIN) is the "nothing written yet" sentinel and can
    // never be a real mapped time, so no first-sample special case is needed.
    hm_time_us m_lastWrittenUs[HM_UNIT_COUNT] = { HM_TIME_UNKNOWN, HM_TIME_UNKNOWN };
    quint64    m_nonMonotonic[HM_UNIT_COUNT]  = { 0, 0 };
    hm_time_us m_maxBackStepUs[HM_UNIT_COUNT] = { 0, 0 };

    // ⚠ THE VALUES ARE KEPT, BECAUSE A MEDIAN CANNOT BE STREAMED. libhackmotion's
    // own reconciler keeps them for the same reason and caps the store the same
    // way. 16384 int32 is 64 KB and covers ~20 s of the device's full ≈799 Hz
    // internal rate, or hours of the 25 Hz live rate.
    //
    // ⚠ AND A TRUNCATION NOBODY REPORTS READS AS COVERAGE, which is why the total
    // is counted separately from the stored count: skewStored < skewCount says the
    // median is over a prefix of the session rather than the session.
    static constexpr size_t kMaxSkewSamples = 16384;
    std::vector<qint32> m_skewSamples;
    quint64 m_skewCount = 0;
    qint32  m_skewMinUs = 0;
    qint32  m_skewMaxUs = 0;

    // Inter-unit relative angle, degrees — the calibrated / uncalibrated / not-in-
    // the-mounting discriminator. See writeSample() for what the values mean.
    quint64 m_relAngleCount  = 0;
    double  m_relAngleSumDeg = 0.0;
    float   m_relAngleMinDeg = 0.0f;
    float   m_relAngleMaxDeg = 0.0f;

    // HM_EV_CLOCK_DEGRADED application-log rate limiting — see handleEvent().
    // The device log ring keeps every report regardless.
    bool     m_clockDegradedLogged   = false;
    uint64_t m_clockDegradedStreamId = 0;
    uint32_t m_clockDegradedP90Us    = 0;
    quint64  m_clockDegradedCount    = 0;

    // ── Capture provenance (Phase B′) ────────────────────────────────────────
    // ⚠ GUARDED BY m_mutex, like m_snap and m_anchor: written on the I/O thread as
    // samples are recorded, read from the export thread through captureProvenance().
    // The log itself is pure and does no locking of its own — see the note on its
    // class — so this mutex is the only one, rather than two with divergent ideas of
    // what they protect.
    pinpoint::hm::CaptureProvenanceLog m_provenance;

    void noteProvenance(const hm_sample &s);   // I/O thread; needs a mapped host time

    // ── Deferred history state, I/O thread unless noted ─────────────────────
    // The session's policy, kept from initialise() so reserveHistory() can hand
    // it to hm_history_request_around() — which is the only way the pre/post
    // roll fields set there reach a request.
    hm_session_policy m_policy{};
    uint64_t m_historyRequestId = 0;
    // Read from the GUI thread by HmInstance::historyPending(), so atomic rather
    // than mutex-held: it is polled every 50 ms during a gather.
    std::atomic<bool> m_historyPending{ false };
    // Written on the I/O thread, taken on the GUI thread — under m_mutex.
    HmInstance::HistoryResult m_historyResult;

    mutable QMutex m_mutex;
    Snapshot       m_snap;
    // Guarded by m_mutex alongside m_snap, for the same reason: it is written on
    // the I/O thread as the presence event is drained and read on the GUI thread.
    // ⚠ LATEST ONLY, AND NEVER PERSISTED. A second measurement replaces the first
    // because a new calibration replaces the old one; there is no history of
    // attempts to compare, on purpose (§8.2 shows a comparison would prefer the
    // worst attempt available), and nothing here reaches a disk.
    HmInstance::ReferenceAnchor m_anchor;
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

void HmSessionWorker::attachBuffer(pinpoint::EventBuffer *buffer,
                                   const pinpoint::SourceId ids[HM_UNIT_COUNT])
{
    m_buffer = buffer;
    for (int u = 0; u < HM_UNIT_COUNT; ++u)
        m_sourceIds[u] = ids[u];
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

    // ⚠ THE LIVE RING IS LEFT AT ITS RECOMMENDED DEFAULT ON PURPOSE, and that is
    // worth a line precisely because "there is no live rate ceiling" makes an
    // untouched default look like an oversight. HM_LIVE_RING_RECOMMENDED is
    // documented as sized from HOW OFTEN THE HOST DRAINS rather than from a rate,
    // and we drain on every notification AND every timer tick — which is the
    // condition it assumes. Raising it would buy nothing a faster drain does not
    // already give; hm_session_dropped_live() is the number that would say
    // otherwise, and it is in the 10 s summary.

    // ⚠ UNMODIFIED, and keepalive_period_us above all. §9.2: a silent connection
    // is dropped at exactly 5.0 minutes and an ACTIVE STREAM DOES NOT PREVENT IT
    // — only a host→device write resets the device's idle timer. For a golf
    // lesson that is mid-lesson with an athlete standing on a mat. There must be
    // no way for this integration to weaken it, so it does not touch it.
    //
    // ⚠ AND calibration_raise_limit_us STAYS AT ITS 6 s DEFAULT — a decision, not
    // an oversight, recorded here because "the raise took longer than 6 s" is
    // exactly the kind of report that gets 'fixed' by raising a limit. The guide
    // animation is what PACES the athlete, and it is set to ~3 s, so the default
    // carries a factor of two in margin over the motion it is timing. The limit is
    // a CLIENT policy — the device imposes no deadline of its own and one measured
    // attempt took 15.6 s and was still applied — which means the only thing
    // raising it can buy is the silent acceptance of a raise the athlete did not
    // actually perform smoothly. A stalled renderer that overruns must produce the
    // library's legible HM_CAL_ABORT_RAISE_TOO_SLOW instead, and the coach re-runs
    // a 3 s routine.
    cfg.policy = hm_session_policy_default();

    // ⚠ THE RETRIEVAL WINDOW IS SIZED FROM THE WINDOW WE KEEP, not from §7.6's
    // recommended 3 s / 1.5 s — because a pull costs about as long as the span
    // it asks for AND stalls the device's own sample counter for 90-99 % of that
    // (§0 row 3, six measured pulls), so every extra second asked for is a second
    // the NEXT shot is not being recorded.
    //
    // ShotProcessor freezes a 4 s trailing window (kWindowDuration) and the
    // analyser never reads further back than impact − 1.75 s (the onset clamp in
    // shaft_track_assembly.h). Measured across the 2026-08-18 studio swings, the
    // frozen window held 1.56-2.41 s after impact. So ±2 s covers everything that
    // is kept and everything that is read, and the default's extra 0.5 s of
    // pre-roll would be fetched, paid for in stall, and then discarded.
    cfg.policy.history_pre_roll_us  = hm_time_us(2'000'000);
    cfg.policy.history_post_roll_us = hm_time_us(2'000'000);

    // Kept so reserveHistory() can pass it to hm_history_request_around(): the
    // two fields above only exist if something reads them.
    m_policy = cfg.policy;

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

    // ⚠ AND MAKE THE RING BARRIER A PROPERTY OF THE CODE RATHER THAN OF THE CALL
    // ORDER. The two lines above already mean no drain can run after this returns,
    // so nothing below could write — but that argument depends on the timer and
    // the signal being the only two ways in. Nulling the pointer here means a
    // third way in, added later, still cannot reach a source that is about to be
    // deregistered. The drains further down this function run with it already
    // null, which is correct: they finish EVENT work, not sample work.
    m_buffer = nullptr;

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

HmInstance::ReferenceAnchor HmSessionWorker::referenceAnchor() const
{
    QMutexLocker lk(&m_mutex);
    return m_anchor;
}

void HmSessionWorker::calibrationCall(hm_status (*fn)(hm_session *), const char *name)
{
    const QString call = QString::fromLatin1(name);

    if (!m_session) {
        // Not a library refusal — there is nothing to refuse it. Reported through
        // the same channel so a UI has exactly one place to handle "that did not
        // happen", and HM_ERR_INVALID_STATE is the honest code for it.
        emit calibrationCallRefused(HM_ERR_INVALID_STATE, call);
        return;
    }

    const hm_status st = fn(m_session);

    // ⚠ THE RETURN VALUE CANNOT GO BACK TO THE CALLER. The GUI thread posted this
    // and moved on (see the Q_INVOKABLEs on HmInstance for why it must), so a
    // refusal is a signal or it is nothing.
    //
    // ⚠ AND A REFUSAL IS NEVER QUEUED FOR LATER OR RETRIED HERE. HM_ERR_NO_STREAM
    // has no waiting state to fall into by design — the device observes a
    // CONTINUOUS raise between the markers, so calibration is not a standalone
    // transaction — and HM_ERR_BUSY wants the coach to try again a second later
    // with the athlete still standing still, which is a decision for the UI and
    // not for a hidden retry that would fire while nobody was in the pose.
    if (st < HM_OK) {
        emit calibrationCallRefused(st, call);
        emit logLine(QStringLiteral("%1 refused: %2")
                         .arg(call, QString::fromLatin1(hm_status_str(st))));
    }

    // Unconditional, and on the refusal path too: pump() re-arms the drain timer
    // from hm_session_next_due_us(), which is the rule for EVERY call into the
    // session. On the success path it is also what gets the `a2 00` / `a2 01` write
    // out to the transport on this pass instead of waiting for a timer.
    pump();
}

void HmSessionWorker::beginCalibration()
{
    // ⚠ RETURNS HM_ERR_NO_STREAM IF NO STREAM IS RUNNING, and this does not check
    // first. Under the one-stream cycle the stream is opened on HM_EV_READY and
    // left open, so it is normally up — but a pre-check here would be a second
    // opinion about the library's own state, and the library's is the one that
    // decides. Surface the refusal; do not queue the call for later and do not
    // retry it.
    calibrationCall(&hm_calibration_begin, "hm_calibration_begin");
}

void HmSessionWorker::confirmHorizontal()
{
    calibrationCall(&hm_calibration_confirm_horizontal, "hm_calibration_confirm_horizontal");
}

void HmSessionWorker::confirmRaise()
{
    // ⚠ WHAT THIS MARKS IS THE END OF A MOTION THAT WAS WATCHED THROUGHOUT, not
    // the second of two poses. The device observes a continuous raise between the
    // two markers, which is why the guide animation is functional rather than
    // decorative: it paces the athlete so the motion the device sees is one smooth
    // sweep.
    calibrationCall(&hm_calibration_confirm_raise, "hm_calibration_confirm_raise");
}

void HmSessionWorker::confirmReferencePose()
{
    // ⚠ NOT OPTIONAL IN OUR FLOW, AND IT RETURNS BEFORE THE MEASUREMENT EXISTS.
    // HM_OK means the run has STARTED; the library then averages the next live
    // samples at the declared pose and reports HM_EV_CALIBRATION_PRESENCE, or
    // HM_WARN_PRESENCE_NOT_MEASURED when too few arrived. The phase reaches
    // HM_CALP_COMPLETE either way, so the outcome is the event and never this call.
    //
    // And this is the step that decides whether the recording can claim anything:
    // skip it and hm_session_calibration_state() stays HM_CAL_UNKNOWN — because the
    // device applies the transform for every `a2 01`, including attempts we would
    // reject, so "we issued the markers" is not evidence that calibration took. It
    // is also the only source of the reference anchor Phase D's frame solve needs.
    calibrationCall(&hm_calibration_confirm_reference_pose,
                    "hm_calibration_confirm_reference_pose");
}

void HmSessionWorker::abortCalibration()
{
    // ⚠ THE ONE EXEMPTION FROM THE HISTORY-BRACKET RULE: abort ALWAYS works,
    // bracket or no bracket. It writes nothing — it is a local state reset — so the
    // write quiet period does not apply, and a UI must always be able to cancel a
    // routine it has given up on. Refusing it would leave a stuck calibration with
    // no way out until a pull completed.
    //
    // ⚠ IT HAS TWO OUTCOMES AND ONLY ONE OF THEM IS "ABORTED". Before `0x94`
    // nothing has been applied and the routine ends at HM_CALP_ABORTED. At
    // HM_CALP_VERIFYING the transform is ALREADY APPLIED and no command reverses
    // that, so aborting there DECLINES THE PRESENCE CHECK: the phase goes to
    // HM_CALP_COMPLETE carrying HM_CAL_ABORT_CALLER, the angle stays NaN and the
    // state stays HM_CAL_UNKNOWN. Reporting that as aborted would tell a consumer
    // nothing happened to a stream whose frame had just changed underneath it.
    calibrationCall(&hm_calibration_abort, "hm_calibration_abort");
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

// ---------------------------------------------------------------------------
// Deferred history — reserve at detection, collect at the gather (Phase E)
// ---------------------------------------------------------------------------

void HmSessionWorker::reserveHistory(qint64 impactUs, qint64 deadlineUs)
{
    if (!m_session)
        return;

    if (m_historyPending.load(std::memory_order_acquire)) {
        // A previous shot's pull is still running. ⚠ THIS IS THE SECOND-BALL
        // CASE and it is the failure mode worth naming: the buffer holds ~7.5 s
        // and a pull costs ~4.5 s, so a second strike inside ~3 s can evict the
        // range the first one is still fetching. The library warns through
        // HM_EV_HISTORY_EVICTION_RISK; refusing here keeps the FIRST swing's
        // data rather than trading it for a partial second.
        emit logLine(QStringLiteral("history: reservation refused — a pull is "
                                    "already in flight (second ball too soon)"));
        return;
    }

    // ±2 s around the event, from the policy set in initialise() — sized to the
    // window ShotProcessor actually freezes rather than to §7.6's recommendation.
    // Passing the policy rather than NULL is what makes those fields reachable.
    hm_history_request req = hm_history_request_around(&m_policy, hm_time_us(impactUs));

    // ⚠ DEADLINE MUST BE PAST window.end_us OR THIS IS REFUSED AT THE CALL SITE
    // (HM_ERR_INVALID_ARG), not four seconds later. The caller orders it inside
    // the pipeline's own gather deadline so a slow pull materialises its own
    // block instead of being cancelled by our timeout.
    req.deadline_us = hm_time_us(deadlineUs);

    // ⚠ Re-request the holes. The device HOLES an over-wide request rather than
    // clamping it, and `a1` may be issued in place, so a partial result is
    // exactly the thing worth asking for again. Cost: each attempt stalls the
    // sample counter again, and self_recording_gap becomes the ENVELOPE over all
    // of them — which is why `attempts` is recorded beside it.
    req.refill_gaps  = true;
    req.max_attempts = 0;               // library default (3)

    // ⚠⚠ NO ALIGNMENT GATE. TAKE THE DATA AND SAY WHAT IT IS WORTH.
    //
    // This was 4167 µs — one 240 fps camera frame — on the reasoning that a trace
    // placed against video by more than a frame is worse than no trace. It was
    // wrong three times over, and the 2026-08-18 studio session is what proved it:
    // all five swings came back HM_HIST_REFUSED_ALIGNMENT, attempts 0, no radio
    // traffic, and the whole of Phase E never ran.
    //
    //  - The camera is 150 fps, not 240. A frame is 6,636 µs, so the limit was
    //    stricter than its OWN stated rationale.
    //  - It could never have passed. The link's fit reported a p90 residual of
    //    16,145 µs (median 6,520, max 49,219) over a 56 s baseline — that is
    //    Bluetooth notification jitter, measured at 20-25 ms back in Phase B,
    //    BEFORE this limit was chosen. A gate that cannot open is not a guard,
    //    it is the feature switched off.
    //  - It refused the deferred lane over an error the LIVE lane already carries
    //    ungated: both are dated through the same fit.
    //
    // Zero is the library's own default and its header says exactly why: "we could
    // not date these, and here they are anyway, clearly marked" — never "we could
    // not date these, so you get nothing". The block still carries every
    // sample_index, the per-unit device_time_us, coverage, density, and the fit
    // with its flags. Event-anchored analysis wants precisely that, and impact is
    // identifiable in both the wrist trace and the video independently.
    //
    // ⚠ This disables the QUALITY gate ONLY. A window on the far side of a
    // retrieval is still refused with the same status code, and must stay refused:
    // there the mapping does not exist, so the pull would hand back the WRONG
    // samples rather than undated ones. Do not read a future REFUSED_ALIGNMENT as
    // this line having come back.
    req.alignment_budget_us = 0;

    req.user_tag = uint64_t(impactUs);

    uint64_t id = 0;
    const hm_status st = hm_history_reserve(m_session, &req, &id);
    if (st < HM_OK) {
        // Every one of these is validated AT RESERVE rather than discovered at
        // the deadline: an unsatisfiable window, no fit at all, a window wider
        // than the gather area, or every slot taken.
        emit logLine(QStringLiteral("history: reserve refused: %1")
                         .arg(QString::fromLatin1(hm_status_str(st))));
        return;
    }

    m_historyRequestId = id;
    m_historyPending.store(true, std::memory_order_release);
    pump();   // the rule for EVERY call into the session: re-arm from next_due_us
}

void HmSessionWorker::pollHistory()
{
    if (!m_session || !m_historyPending.load(std::memory_order_acquire))
        return;

    hm_history_block *block = nullptr;
    const hm_status st = hm_history_collect(m_session, m_historyRequestId, &block);
    if (st == HM_PENDING)
        return;                       // still in flight — never blocks

    m_historyPending.store(false, std::memory_order_release);

    if (st < HM_OK || !block) {
        emit logLine(QStringLiteral("history: collect failed: %1")
                         .arg(QString::fromLatin1(hm_status_str(st))));
        return;
    }

    HmInstance::HistoryResult r;
    r.valid    = true;
    r.status   = block->status;
    r.attempts = block->attempts;
    r.coverageOverflowed = block->coverage_overflowed != 0;

    r.coverageFraction = block->coverage_fraction;
    r.density          = block->density;
    r.achievedHz       = block->achieved_hz;
    r.largestGapUs     = block->largest_gap_us;

    r.liveOverlapSamples    = block->live_overlap_samples;
    r.liveOverlapMismatches = block->live_overlap_mismatches;

    r.selfRecordingGapStartUs = qint64(block->self_recording_gap.start_us);
    r.selfRecordingGapEndUs   = qint64(block->self_recording_gap.end_us);
    r.requestedStartUs        = qint64(block->requested.start_us);
    r.requestedEndUs          = qint64(block->requested.end_us);

    r.delivered.reserve(block->delivered_count);
    for (size_t i = 0; i < block->delivered_count; ++i)
        r.delivered.emplace_back(qint64(block->delivered[i].start_us),
                                 qint64(block->delivered[i].end_us));

    r.gaps.reserve(block->gap_count);
    for (size_t i = 0; i < block->gap_count; ++i)
        r.gaps.push_back(HmInstance::HistoryResult::Gap{
            qint64(block->gaps[i].span.start_us),
            qint64(block->gaps[i].span.end_us),
            int(block->gaps[i].kind) });

    r.fit                = block->fit;
    r.calStateAtStart    = block->calibration.state_at_start;
    r.calStateAtEnd      = block->calibration.state_at_end;
    r.calSpansTransition = block->calibration.spans_transition != 0;
    r.presenceAngleDeg   = block->calibration.presence_angle_deg;
    r.configBits         = int(block->config.bits);

    r.tUs.reserve(block->sample_count);
    r.lowerArm.reserve(block->sample_count);
    r.palm.reserve(block->sample_count);
    for (size_t i = 0; i < block->sample_count; ++i) {
        const hm_sample &smp = block->samples[i];
        // ⚠ SAME GATE AS THE LIVE PATH, AND FOR THE SAME REASON: never fall back
        // to arrival time. For a retrieved block arrival time is meaningless
        // twice over — these samples arrived thousands at a time, seconds after
        // they were measured.
        if (smp.host_time_us == HM_TIME_UNKNOWN || (smp.flags & HM_SAMPLE_NO_FIT)) {
            ++r.noHostTimeSkipped;
            continue;
        }
        r.tUs.push_back(qint64(smp.host_time_us));
        r.lowerArm.push_back(pinpoint::hm::toImuSample(smp.lower_arm));
        r.palm.push_back(pinpoint::hm::toImuSample(smp.palm));
    }

    emit logLine(QStringLiteral("history: %1 — %2 samples, coverage %3, "
                                "density %4, achieved %5 Hz, largest gap %6 us, "
                                "overlap %7/%8 mismatched, attempts %9")
                     .arg(QString::fromLatin1(hm_history_status_name(
                              hm_history_status(block->status))))
                     .arg(r.tUs.size())
                     .arg(r.coverageFraction, 0, 'f', 3)
                     .arg(r.density, 0, 'f', 3)
                     .arg(r.achievedHz, 0, 'f', 1)
                     .arg(r.largestGapUs)
                     .arg(r.liveOverlapMismatches)
                     .arg(r.liveOverlapSamples)
                     .arg(r.attempts));

    // ⚠ AND INTO THE APP LOG, because logLine() above reaches only the device
    // panel in Settings. The 2026-08-18 studio session recorded five swings with
    // this whole path refusing on every one of them, and the only trace was a
    // status code inside a 28 MB swing.json — found days later, by reading it.
    // A retrieval that returns nothing has cost the shot its high-rate lane, and
    // that has to be legible while the athlete is still on the mat.
    if (block->status != HM_HIST_COMPLETE || r.tUs.empty()) {
        ppWarn() << "[HmInstance]" << m_deviceId << "— history retrieval"
                 << hm_history_status_name(hm_history_status(block->status))
                 << "—" << r.tUs.size() << "samples, coverage" << r.coverageFraction
                 << "attempts" << r.attempts;
    }

    // ⚠ EXACTLY ONCE, and only after everything above has been copied out: the
    // block owns its samples, its intervals and its gaps, and releasing it runs
    // the allocator. A second release is undefined exactly as a double free is.
    hm_history_block_release(block);

    QMutexLocker lk(&m_mutex);
    m_historyResult = std::move(r);
}

void HmSessionWorker::pump()
{
    if (!m_session) return;

    // Events first: HM_EV_READY starts the stream, and the write it queues is
    // then picked up by the very next drain rather than waiting for a timer.
    drainEvents();
    drainWrites();
    drainLive();
    // ⚠ AFTER drainLive(), not before. Collecting is what re-anchors the clock
    // fit (§6.1.1 — the sample counter stops while the device replays, so one
    // fit cannot span a pull), and the live samples drained above belong to the
    // fit as it stood BEFORE that re-anchor.
    pollHistory();
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
        //
        // ⚠ UNKNOWN, NOT UNCALIBRATED, AND THE DIFFERENCE IS READ RATHER THAN
        // ASSERTED. Whether a restart costs the device its transform is untested
        // where a disconnect demonstrably does, so the library says "I no longer
        // know" rather than "it is gone" — two different claims, and the state comes
        // from hm_session_calibration_state() precisely so we make neither of them
        // ourselves.
        ppWarn() << "[HmInstance]" << m_deviceId << "— stream restarted; calibration is no longer known";
        emit calibrationLost(int(hm_session_calibration_state(m_session)),
                             QStringLiteral("the stream restarted"));
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

        // ⚠ calibration_invalidated IS ALWAYS 1 AND IT IS NOT CHECKED, because a
        // check would imply the other branch exists. §8.3 measured 0.70°
        // immediately before dropping a link and 18.80° at the same pose after
        // reconnecting, strap untouched and never removed — RECONNECT IS NOT
        // RESUME. hm_session_on_link_down() has already driven the library's state
        // to UNCALIBRATED by the time this event surfaces, which is why the value
        // is read rather than assumed even here.
        emit calibrationLost(int(hm_session_calibration_state(m_session)),
                             QStringLiteral("the link went down"));
        break;
    }

    case HM_EV_WARNING: {
        // hm_event_format already renders the code; the ppWarn gets it into the
        // application log too, where a support bundle will find it.
        const hm_warning_code code = static_cast<hm_warning_code>(ev.u.warning.code);
        // ⚠ detail_i32 IS LOGGED FOR EVERY CODE, INCLUDING THE ONES WE DO NOT KNOW.
        // For most warnings it is a supporting number; for at least one it is the
        // ENTIRE content — HM_WARN_CALIBRATION_STATUS_FORM carries the status byte of
        // a `0x94` short form that has never been observed and whose values mean
        // nothing to anyone yet. If this application is ever the first to see one,
        // logging the name alone would discard the single byte that would let
        // somebody work out what it meant. Cheap here, unrecoverable if omitted.
        ppWarn() << "[HmInstance]" << m_deviceId << "— warning:"
                 << hm_warning_code_name(code)
                 << "detail" << ev.u.warning.detail_i32;

        // ⚠ THREE OF THESE ARE CALIBRATION OUTCOMES, NOT DIAGNOSTICS, and folding
        // them into the generic warning line would leave the UI claiming success
        // for two of them: the phase reaches HM_CALP_COMPLETE regardless.
        switch (code) {
        // ⚠ FIRST, AND DELIBERATELY NOT NEXT TO THE CALIBRATION CASES BELOW — two of
        // those share a fallthrough, and a case inserted between them silently
        // redirects the first one.
        //
        // ⚠ THIS IS NOT A DIAGNOSTIC. IT SAYS THIS DEVICE IS NOT THE ONE WE MODEL.
        // §6.3's record is a header followed by one block PER SENSOR, and the count
        // comes from the 0x84 reply's LENGTH (libhackmotion f89cea4 — reading it as a
        // byte gave the right answer on this hardware by coincidence). This fires
        // when that count is one the library's two-block layout cannot carry, which
        // means the whole two-lane model — two sources, "#lowerArm" and "#palm", one
        // wrist angle between them — does not describe what is on the arm.
        //
        // It reaches the coach's device log rather than only ppWarn, because the
        // alternative is a session that records two lanes of something else and looks
        // entirely normal doing it. detail_i32 is the count the device reported.
        case HM_WARN_SENSOR_COUNT_UNSUPPORTED:
            emit logLine(QStringLiteral("ERROR: this device reports %1 sensors. This "
                                       "integration models exactly two — a lower-arm "
                                       "unit and a palm unit — so the recorded lanes "
                                       "may not be what they claim. Do not trust a "
                                       "wrist angle from this session.")
                             .arg(ev.u.warning.detail_i32));
            ppError() << "[HmInstance]" << m_deviceId
                      << "— sensor count unsupported:" << ev.u.warning.detail_i32
                      << "— the two-lane model does not describe this device";
            break;

        case HM_WARN_PRESENCE_NOT_MEASURED:
            // The check was ASKED FOR and could not be taken — too few live samples
            // reached the run. detail_i32 is how many were collected. The angle
            // stays NaN and the state stays HM_CAL_UNKNOWN, so this is the only
            // thing that distinguishes "we could not check" from "we checked and it
            // was fine".
            emit calibrationPresenceUnmeasured(ev.u.warning.detail_i32,
                                               int(hm_session_calibration_state(m_session)));
            break;

        case HM_WARN_CALIBRATION_ABSENT:
            // The angle sits in the uncalibrated population (§8.2: 15.01° after a
            // power cycle, 18.80° after a plain disconnect) and the library has
            // driven the state to UNCALIBRATED. The routine ran and the transform is
            // absent or was lost — which is the ONE verdict this angle can carry,
            // because there the gap is an order of magnitude.
        case HM_WARN_CALIBRATION_INDETERMINATE:
            // The angle landed BETWEEN the two populations §8.2 measured. It is
            // evidence of neither state, so the library leaves the state exactly as
            // it was — and so do we. Guessing here is how a presence check becomes a
            // quality score.
            emit calibrationStateRefreshed(int(hm_session_calibration_state(m_session)));
            break;

        default:
            break;
        }
        break;
    }

    case HM_EV_CALIBRATION_PHASE: {
        const hm_calibration_phase_event &p = ev.u.calibration_phase;
        // ⚠ THE STATE IS RE-READ, NEVER INFERRED FROM THE PHASE. They are
        // deliberately different questions: the phase says where the routine is, the
        // state says what is known about the device's transform, and
        // HM_CALP_COMPLETE with HM_CAL_UNKNOWN is the ordinary outcome of a routine
        // whose presence check was skipped or could not be taken. Deriving one from
        // the other is the exact mistake the library's comment at HM_CALP_COMPLETE
        // warns against.
        emit calibrationPhaseEvent(p.phase, p.previous_phase, p.abort_reason,
                                   int(hm_session_calibration_state(m_session)),
                                   qint64(p.elapsed_us));
        break;
    }

    case HM_EV_CALIBRATION_PRESENCE: {
        const hm_calibration_presence_event &pe = ev.u.calibration_presence;

        HmInstance::ReferenceAnchor a;
        a.valid = true;
        // (2) THE AVERAGED POSE — the one a Phase D frame solve must use.
        a.qLowerArmMean   = toQQuaternion(pe.q_lower_arm_mean);
        a.qPalmMean       = toQQuaternion(pe.q_palm_mean);
        a.poseSpreadDeg[HM_UNIT_LOWER_ARM] = pe.pose_spread_deg[HM_UNIT_LOWER_ARM];
        a.poseSpreadDeg[HM_UNIT_PALM]      = pe.pose_spread_deg[HM_UNIT_PALM];
        // (1) The medoid record — kept for the angle and its provenance only.
        // ⚠ It is NOT an alternative anchor: it is selected on the RELATIVE
        // rotation, which is blind to a whole-arm movement carrying both units
        // together — precisely the motion that contaminates an absolute pose.
        a.qLowerArmMedoid = toQQuaternion(pe.q_lower_arm);
        a.qPalmMedoid     = toQQuaternion(pe.q_palm);
        a.sampleIndex     = pe.sample_index;
        a.skewUs          = pe.skew_us;
        a.samplesUsed     = pe.samples_used;
        a.relativeAngleDeg = pe.relative_angle_deg;
        {
            QMutexLocker lk(&m_mutex);
            m_anchor = a;
        }

        // ⚠ pe.state IS DELIBERATELY NOT THE VALUE CARRIED. It is the state at the
        // measurement; hm_session_calibration_state() is the state now, and a
        // warning refining it (ABSENT / INDETERMINATE) may already have been applied
        // by the time this event is drained. Reading the library twice costs nothing
        // and keeps one answer for the question rather than two.
        emit calibrationPresenceMeasured(int(hm_session_calibration_state(m_session)));
        break;
    }

    case HM_EV_CLOCK_DEGRADED: {
        // ⚠ EVERY ONE OF THESE STAYS IN THE DEVICE LOG RING (log is left true) —
        // only the APPLICATION log is rate-limited. Measured on this Mac: seven
        // reports in six minutes, all saying the same thing, which is how a
        // recurring condition crowds out the one-off lines a support bundle is
        // read for.
        //
        // And it says something worth saying once, properly. The residual spread
        // is BLE notification jitter (p90 ~20-25 ms here), not a device fault and
        // not something a retry or a reconnect changes. What it costs is the
        // PRECISION of the host times this lane writes into the buffer, which is
        // what an alignment against video would rest on — so it is a link-health
        // signal that belongs in provenance rather than a fault to act on. Note
        // what it does NOT touch: each unit's device_time_us is derived from the
        // frame alone, so it is unaffected by any fit state.
        const hm_clock_snapshot &clk = ev.u.clock;
        const bool  first  = !m_clockDegradedLogged || clk.stream_id != m_clockDegradedStreamId;
        // Re-report only on a material worsening, so a slow drift into a genuinely
        // bad link is still visible while a steady one is not repeated.
        const bool  worse  = clk.residual_p90_us > m_clockDegradedP90Us * 2;
        ++m_clockDegradedCount;
        if (first || worse) {
            m_clockDegradedLogged   = true;
            m_clockDegradedStreamId = clk.stream_id;
            m_clockDegradedP90Us    = clk.residual_p90_us;
            ppWarn() << "[HmInstance]" << m_deviceId
                     << "— clock fit degraded: link jitter p90"
                     << clk.residual_p90_us / 1000.0 << "ms, max"
                     << clk.residual_max_us / 1000.0 << "ms, rate"
                     << clk.fitted_rate_hz << "Hz over" << clk.observations
                     << "observations. Recorded sample times carry that much"
                        " precision against the video clock; per-unit device time"
                        " is unaffected. Further reports suppressed unless it doubles"
                        " (total so far" << m_clockDegradedCount << ").";
        }
        break;
    }

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

            // ⚠ EVERY POLLED SAMPLE IS WRITTEN, not only the newest. Keeping just
            // `latest` is right for the 60 Hz display and wrong for the ring,
            // because THE RING IS THE RECORDING: a burst at the device's full
            // ≈799.2 Hz arrives as tens of samples in one notification, and
            // decimating it here would lose most of a swing while every counter
            // in the system still read "no drops".
            writeSample(samples[i]);
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

        // The vector too, in the device's own axes and unremapped — the frame
        // constant is applied once, at the display tick, alongside the quaternion.
        s.gx = b.gyro_dps[0];
        s.gy = b.gyro_dps[1];
        s.gz = b.gyro_dps[2];
    }

    // The cumulative ring-write counters live on the worker (this snapshot is
    // rebuilt from scratch each pass) and are copied out here.
    next.noFitSkipped = m_noFitSkipped;
    next.skewCount    = m_skewCount;
    next.skewMinUs    = m_skewMinUs;
    next.skewMaxUs    = m_skewMaxUs;
    next.relAngleNowDeg = hm_relative_angle_deg(&latest);
    next.relAngleCount  = m_relAngleCount;
    next.relAngleSumDeg = m_relAngleSumDeg;
    next.relAngleMinDeg = m_relAngleMinDeg;
    next.relAngleMaxDeg = m_relAngleMaxDeg;
    for (int u = 0; u < HM_UNIT_COUNT; ++u) {
        next.written[u]       = m_written[u];
        next.nonMonotonic[u]  = m_nonMonotonic[u];
        next.maxBackStepUs[u] = m_maxBackStepUs[u];
    }

    QMutexLocker lk(&m_mutex);
    next.seq         = m_snap.seq + decoded;
    next.rateHz      = rateHz;
    // Non-zero means the host did not keep up and live samples were lost. It is
    // surfaced in the 10 s summary rather than swallowed.
    next.droppedLive = hm_session_dropped_live(m_session);
    m_snap = next;
}

// ⚠ CALLED ONLY FOR SAMPLES THAT ARE ACTUALLY RECORDED — past the capture gate and
// past the no-fit gate. Provenance that described samples the window does not
// contain would be worse than none: it would attribute a clip to a swing that never
// saw it.
void HmSessionWorker::noteProvenance(const hm_sample &s)
{
    // ⚠ pinned_mask, the PER-UNIT field, rather than HM_SAMPLE_PINNED, which is the
    // record-level summary of the same condition. Keying off the mask is what keeps
    // one saturation from being reported once per unit, and it is also the only form
    // that says WHICH channel clipped.
    QMutexLocker lk(&m_mutex);
    m_provenance.noteSample(qint64(s.host_time_us),
                            s.lower_arm.pinned_mask,
                            s.palm.pinned_mask,
                            (s.flags & HM_SAMPLE_QUAT_NORM_SUSPECT) != 0,
                            int(s.calibration),
                            int(s.config_bits));
}

HmInstance::CaptureProvenance
HmSessionWorker::captureProvenance(qint64 windowStartUs, qint64 windowEndUs) const
{
    QMutexLocker lk(&m_mutex);
    return m_provenance.inWindow(windowStartUs, windowEndUs);
}

HmSessionWorker::SkewStats HmSessionWorker::skewStats() const
{
    QMutexLocker lk(&m_mutex);

    SkewStats st;
    st.total  = m_skewCount;
    st.stored = quint64(m_skewSamples.size());
    if (m_skewSamples.empty())
        return st;

    // ⚠ m_skewSamples IS NEVER SORTED IN PLACE — the half-split needs arrival order.
    // Both helpers are in Imu/hm_skew_stats.h, pure, so the arithmetic behind the
    // "your sensor is not behaving" warning is testable without a device.
    st.medianUs = pinpoint::hm::skewMedianUs(m_skewSamples);

    const pinpoint::hm::HalfSplit hs = pinpoint::hm::skewHalfSplit(m_skewSamples);
    st.halfSplitDeltaUs = hs.deltaUs;
    st.halfSplitValid   = hs.valid;
    return st;
}

void HmSessionWorker::writeSample(const hm_sample &s)
{
    // ⚠ skew_us IS ACCUMULATED BEFORE ANY GATE, because it does not come from the
    // clock fit: it is the difference of the two units' own tick counters, so a
    // sample the ring rejects for having no host time still carries a valid one.
    // The exception is a configuration that ships no ticks at all (`5e` — §10.3
    // says it removes the counter and with it any skew measurement), where the
    // field would fold a meaningless 0 into the mean and make the export claim a
    // 0 µs skew it never measured. We ask for `7e`, so this branch should not be
    // reachable; it costs one test and it stops a silent lie if it ever is.
    if ((s.flags & (HM_SAMPLE_TICKS_MISSING | HM_SAMPLE_NOT_TIME_ALIGNABLE)) == 0) {
        if (m_skewCount == 0) {
            m_skewMinUs = s.skew_us;
            m_skewMaxUs = s.skew_us;
        } else {
            m_skewMinUs = qMin(m_skewMinUs, s.skew_us);
            m_skewMaxUs = qMax(m_skewMaxUs, s.skew_us);
        }
        if (m_skewSamples.size() < kMaxSkewSamples)
            m_skewSamples.push_back(s.skew_us);
        ++m_skewCount;
    }

    // ⚠ THE ONE NUMBER THAT SAYS WHETHER THE TWO UNITS ARE WHERE THEY SHOULD BE,
    // and it is accumulated before the capture gate because it describes the
    // DEVICE, not the recording. hm_relative_angle_deg() is the library's own
    // 2·acos|q_palm · q_arm| — deliberately convention-blind, which makes it
    // useless for anything needing a sign and exactly right here.
    //
    // ⚠ IT IS ONLY INTERPRETABLE IN A KNOWN POSE, and reading it in any other is
    // how it gets mistaken for a fault. Measured by replaying the library's own
    // fixtures through this same decode path — every sample in both is
    // UNCALIBRATED:
    //   tests/fixtures/smoke.hmwire   first 200 samples (still) mean 14.96°,
    //                                 matching §8.3's 15.01° for a straight wrist,
    //                                 then up to 173° once the hand moves
    //   tests/fixtures/swings.hmwire  modal band 170-180° (28 % of samples),
    //                                 max 180.00°, across five real golf swings
    // So the bands are:
    //   0.4-0.8°   calibration applied and holding (§8.3). The COLLAPSE to under 1°
    //              at the reference pose is what proves the routine took — which is
    //              why HmInstance publishes this as relativeAngleDeg for the flow to
    //              show at that one moment, and only at that moment.
    //   ~15°       UNCALIBRATED, AT REST, WRIST STRAIGHT — board placement, not
    //              anatomy. The only pose where a number carries a verdict, and the
    //              expected reading before the routine has been run.
    //   up to 180° ORDINARY for an uncalibrated pair away from that pose. It is NOT
    //              evidence of a mounting or decode fault: uncalibrated, this angle
    //              carries both boards' offsets and is not an anatomical quantity at
    //              all — which is the whole reason the device's own routine (Phase C,
    //              above) and the frame solve (Phase D) exist. Two live cubes sitting
    //              ~180° apart is what an uncalibrated pair LOOKS like, and is not a
    //              fault to chase.
    //
    // ⚠ IT IS NOT A CALIBRATION QUALITY SCORE — §8.2 measured a no-raise
    // calibration scoring BEST on it, because it tests the zeroing and is blind to
    // the axis. Its sound uses are this one and the presence check, both of which
    // ask "did calibration happen at all", where the gap is an order of magnitude.
    //
    // Samples whose decode may be misaligned are excluded: a suspect quaternion
    // norm makes the angle meaningless rather than merely noisy.
    if ((s.flags & HM_SAMPLE_QUAT_NORM_SUSPECT) == 0) {
        const float relDeg = hm_relative_angle_deg(&s);
        if (m_relAngleCount == 0) {
            m_relAngleMinDeg = relDeg;
            m_relAngleMaxDeg = relDeg;
        } else {
            m_relAngleMinDeg = qMin(m_relAngleMinDeg, relDeg);
            m_relAngleMaxDeg = qMax(m_relAngleMaxDeg, relDeg);
        }
        m_relAngleSumDeg += double(relDeg);
        ++m_relAngleCount;
    }

    if (!m_buffer || !m_buffer->isCapturing())
        return;

    // ⚠ NEVER FALL BACK TO ARRIVAL TIME. host_recv_us is one-sidedly late and is
    // what the fit is built FROM; mixing two timebases inside one source looks
    // fine and corrupts the capture. A sample with no mapped time is skipped and
    // counted instead — and the count matters, because the fit exists from the
    // first live frame (brief §0 #1), so HM_SAMPLE_NO_FIT arriving here is a
    // symptom rather than the expected start-up condition it once was.
    if (s.host_time_us == HM_TIME_UNKNOWN || (s.flags & HM_SAMPLE_NO_FIT)) {
        // ⚠ COUNTED TWICE, INTO TWO DIFFERENT AUDIENCES, and neither is redundant:
        // m_noFitSkipped feeds the 10 s device summary a coach's support bundle
        // carries, while the provenance log is what reaches swing.json so that a
        // window missing samples says so a year later. The path is unreachable after
        // the first live frame (brief §0 #1), so the lock costs nothing in practice.
        ++m_noFitSkipped;
        {
            QMutexLocker lk(&m_mutex);
            m_provenance.noteNoFitSkipped();
        }
        return;
    }

    // Past every gate, so this sample has a mapped host time and is about to be
    // recorded — which is exactly the set the provenance describes.
    noteProvenance(s);

    // ⚠ ONE SAMPLE, ONE INDEX, ONE MAPPED HOST TIME — SO BOTH BLOCKS GET THE SAME
    // STAMP, and the palm is NOT offset by skew_us. The fit maps index → host
    // time, and one record has one index; §10.3 states that whether the stable
    // 0.92 ms is real sampling skew or arbitrary phase between two free-running
    // counters cannot be told from the counters alone, so applying it here would
    // bake in an interpretation the library explicitly refuses to make. It is
    // carried as provenance instead (skewUsMean()), and the fit-independent
    // per-unit device_time_us is what Phase E/G anchor on when sub-millisecond
    // pairing genuinely matters.
    const hm_unit_sample *const blocks[HM_UNIT_COUNT] = { &s.lower_arm, &s.palm };

    for (int u = 0; u < HM_UNIT_COUNT; ++u) {
        const pinpoint::SourceId id = m_sourceIds[u];
        if (id == pinpoint::kInvalidSourceId)
            continue;   // this unit's registration failed — it does not record

        auto slot = m_buffer->acquireWriteSlot(id);
        if (!slot.valid || slot.capacity < sizeof(pinpoint::ImuSample))
            continue;

        // ⚠ MEASURED HERE, CLAMPED BY THE MERGER — NOT BOTH. host_time_us is
        // monotonic in sample_index within one clock fit and the samples arrive in
        // index order, so per-source monotonicity (event_buffer.cpp:84) is safe by
        // construction — but the library RE-ANCHORS the fit (design §6.1.1 at every
        // history pull, and the lower-envelope offset moves as observations arrive)
        // and nothing in clock.h promises the published host_time_us never steps
        // back across a re-anchor. So it is asserted by measurement rather than by
        // assumption. Clamping it locally would hide the event from the resource
        // monitor's monotonicity_violations counter — the one diagnostic a coach's
        // support bundle actually carries — so the merger stays the single site.
        //
        // Per unit rather than per device because the merger's state is per SOURCE.
        // The two counts move together while both lanes are registered (same stamp,
        // same order); they diverge exactly when one registration failed.
        if (s.host_time_us <= m_lastWrittenUs[u]) {
            ++m_nonMonotonic[u];
            const hm_time_us step = m_lastWrittenUs[u] - s.host_time_us;
            if (step > m_maxBackStepUs[u])
                m_maxBackStepUs[u] = step;
        }
        m_lastWrittenUs[u] = s.host_time_us;

        // The converter owns the m/s² → g conversion and is the single source of
        // truth for this lane's stored units; the quaternion goes in exactly as it
        // arrived (world→body, §6.7 — the convention question is Phase D's).
        const pinpoint::ImuSample smp = pinpoint::hm::toImuSample(*blocks[u]);
        std::memcpy(slot.data, &smp, sizeof smp);
        *slot.bytes_written = static_cast<uint32_t>(sizeof smp);
        *slot.timestamp_us  = s.host_time_us;
        m_buffer->publish(id, slot.sequence);
        ++m_written[u];

        // ⚠ ONE LINE PER LANE, THE FIRST TIME IT RECORDS, IN THE APPLICATION LOG.
        // Everything else about this lane lives in the device log ring, which has
        // no reachable UI — so without this, "two sources are registered" and "two
        // sources are recording" look identical from outside, and they are NOT the
        // same thing: writes gate on isCapturing(), which only becomes true when a
        // SESSION starts. A registered lane sitting at zero events in Settings is
        // correct behaviour, and this is the line that says when that changed.
        if (m_written[u] == 1) {
            ppInfo() << "[HmInstance]" << m_deviceId << "— recording started on the"
                     << hm_unit_name(static_cast<hm_unit>(u)) << "lane, source" << id;
        }
    }
}

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

#include "hm_instance.moc"
