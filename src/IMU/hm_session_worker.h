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

#include <QBluetoothAddress>
#include <QBluetoothDeviceInfo>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QTimer>

#include <wrist/wrist.h>

#include <atomic>
#include <deque>
#include <limits>
#include <vector>

// nowUs() is spelled inline below, so the buffer's clock has to be complete here
// rather than forward-declared: every timestamp the session maps is anchored to
// the SAME clock the rest of the capture uses, and that is the whole point.
#include "event_buffer.h"

#include "ble_imu_transport.h"
#include "hm_capture_provenance.h"  // the pure window arithmetic behind Phase B′
#include "hm_session_types.h"       // ReferenceAnchor / HistoryResult — what this measures

// ---------------------------------------------------------------------------
// HmSessionWorker — the I/O-thread resident
// ---------------------------------------------------------------------------
//
// ⚠ THE LIBRARY'S THREADING CONTRACT IS ABSOLUTE (session.h:6-21): every
// wr_session_* call must come from ONE thread for the session's whole life.
// libwrist contains no locks, no atomics and no threads, so this is not
// advice. Everything that touches the session — creation, bytes in, ticks,
// every drain, close and destroy — happens here, on ImuManager's shared IMU I/O
// thread. The only thing that crosses the boundary is snapshot(), which is a
// copy out from under a mutex, and the queued signals below.
//
// The host loop this implements is the one at the top of session.h:
//
//     on transport bytes ......... wr_session_on_bytes(s, p, n, now)
//     on link up/down ............ wr_session_on_link_up / _on_link_down
//     arm one timer to ........... wr_session_next_due_us(s)
//     when it fires .............. wr_session_tick(s, now)
//     then always ................ drain poll_writes / poll_events / poll_live
//
// That is the whole integration. The library never pushes; the host drains.
class HmSessionWorker : public QObject
{
    Q_OBJECT

public:
    // One unit's display state. Deliberately not wr_unit_sample: the display
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
        quint64   droppedLive = 0;   // wr_session_dropped_live — never silent
        UnitState unit[WR_UNIT_COUNT];

        // ── Ring-write accounting, copied out of the worker's own counters ────
        quint64 written[WR_UNIT_COUNT] = { 0, 0 };
        // Samples the ring never saw because they carried no mapped host time.
        // ⚠ NOT the obsolete "drop the first two seconds" rule: the clock fit
        // exists from the first live frame (brief §0 #1) and WR_SAMPLE_NO_FIT
        // fires only before it, so a non-zero count here means something is
        // wrong — which is exactly why it is counted rather than assumed zero.
        quint64 noFitSkipped = 0;
        // Samples whose host_time_us did not advance past the last one written to
        // that source, and the largest such backward step. MEASUREMENT ONLY — the
        // merger owns the clamping (see writeSample()).
        quint64 nonMonotonic[WR_UNIT_COUNT]  = { 0, 0 };
        qint64  maxBackStepUs[WR_UNIT_COUNT] = { 0, 0 };
        // skew_us provenance: min / MEDIAN / max of palm − lower_arm over the
        // session. Never applied to a timestamp — see HmInstance::skewUsMedian().
        //
        // ⚠ A MEDIAN, NOT A MEAN, AND THAT IS THE WHOLE POINT OF THIS FIELD. A
        // single record's tick difference is dominated by ±½-sample PAIRING JITTER
        // — libwrist measured 89 and 99 ticks on two consecutive records of
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
    // reserveHistory() must run ON the I/O thread (it is a wr_session_* call) and
    // is posted there by HmInstance. The other two are read from the GUI thread
    // while the gather polls, so they synchronise: an atomic flag and a
    // mutex-held result, the same shape snapshot() already uses.
    void reserveHistory(qint64 impactUs, qint64 deadlineUs);
    bool historyPending() const {
        return m_historyPending.load(std::memory_order_acquire);
    }
    pinpoint::hm::HistoryResult takeHistoryResult() {
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
                      const pinpoint::SourceId ids[WR_UNIT_COUNT]);

    // The reference-pose anchor of the most recent presence measurement, copied
    // out from under the same mutex snapshot() uses — so it is callable from any
    // thread. ⚠ EVENTED AND QUERIED BOTH, deliberately: the anchor cannot be
    // re-derived once the pose has passed (capturing it again from the live stream
    // would anchor on an instant that is only NEARLY the one measured, and
    // "nearly" becomes a fixed rotation error in every later reading), so the
    // signal below carries no payload and the value is fetched from here instead
    // of being marshalled through a metatype.
    pinpoint::hm::ReferenceAnchor referenceAnchor() const;

    // Window-scoped capture provenance (Phase B′). Callable from any thread; copies
    // out from under the same mutex snapshot() uses.
    pinpoint::hm::CaptureProvenance captureProvenance(qint64 windowStartUs,
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
    // refuses an out-of-order step with WR_ERR_INVALID_STATE, and duplicating that
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
    // wr_session_calibration_state() AT THE MOMENT THE EVENT WAS DRAINED. The
    // phase and the state answer different questions and neither is derivable from
    // the other — WR_CALP_COMPLETE with WR_CAL_UNKNOWN is the ordinary outcome of a
    // routine whose presence check was skipped — so the state is always read and
    // never inferred from the phase.
    void calibrationPhaseEvent(int phase, int previousPhase, int abortReason,
                               int libraryState, qint64 elapsedUs);
    // A presence measurement landed. The values are in referenceAnchor().
    void calibrationPresenceMeasured(int libraryState);
    // WR_WARN_PRESENCE_NOT_MEASURED: the check was ASKED FOR and could not be
    // taken. ⚠ Its own signal because the phase still reaches WR_CALP_COMPLETE and
    // "we could not check" must never end up in the same recording as "we checked
    // and it was fine". `samplesCollected` is how many arrived.
    void calibrationPresenceUnmeasured(int samplesCollected, int libraryState);
    // WR_WARN_CALIBRATION_ABSENT / _INDETERMINATE. Both arrive alongside a presence
    // measurement and only refine what the library knows — ABSENT drives the state
    // to UNCALIBRATED, INDETERMINATE leaves it exactly as it was — so this carries
    // the state and nothing else. Order against the presence event does not matter:
    // both re-read the library rather than deciding anything themselves.
    void calibrationStateRefreshed(int libraryState);
    // ⚠ THE CALIBRATION IS GONE. Link-down (always) or a stream restart.
    void calibrationLost(int libraryState, const QString &why);
    // The wr_status of a refused wr_calibration_* call, with the call's name. The
    // queued hop means this is the ONLY way a refusal can reach the GUI thread.
    void calibrationCallRefused(int status, const QString &call);

private:
    void onBytes(const QByteArray &data);
    void onTransportStateChanged(BleImuTransport::State state);

    // The shared body of the five calibration entry points: make the call, turn a
    // refusal into calibrationCallRefused(), then pump.
    void calibrationCall(wr_status (*fn)(wr_session *), const char *name);

    // Every path into the session ends here: drain what it produced, then
    // re-arm from the session's own idea of when it next wants attention.
    void pump();
    // Runs on the I/O thread only, from pump().
    void pollHistory();
    void rearm();
    void drainWrites();
    void drainEvents();
    void drainLive();
    void writeSample(const wr_sample &s);   // one wr_sample → two ring writes
    void handleEvent(const wr_event &ev);

    static wr_time_us nowUs() { return pinpoint::EventBuffer::nowMicros(); }

    // Batch sizes for the drains. wr_event is 280 bytes and wr_sample ~190, so
    // these are ~9 KB and ~12 KB of stack respectively — drained in a loop until
    // a poll comes back short, so a burst is never left sitting in a ring.
    static constexpr size_t kWriteBatch = 16;
    static constexpr size_t kEventBatch = 32;
    static constexpr size_t kLiveBatch  = 64;

    // Upper bound on how long the drain timer is allowed to sleep, however
    // distant wr_session_next_due_us() says the next deadline is. The keepalive
    // poll is 30 s away for most of a session, and a single no-op tick per
    // second is far cheaper than the failure mode this bounds: a host clock that
    // jumps, or a due time computed from one, cannot park the session for
    // minutes. wr_session_tick() is documented as cheap and idempotent when
    // nothing is due, which is exactly what these extra wakes hit.
    static constexpr int kMaxDueDelayMs = 1'000;

    wr_session      *m_session   = nullptr;
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
    pinpoint::SourceId     m_sourceIds[WR_UNIT_COUNT] =
        { pinpoint::kInvalidSourceId, pinpoint::kInvalidSourceId };

    // Cumulative counters. They live here rather than in m_snap because
    // drainLive() rebuilds the snapshot from scratch each pass and would
    // otherwise reset them; the snapshot carries a copy out.
    quint64 m_written[WR_UNIT_COUNT] = { 0, 0 };
    quint64 m_noFitSkipped = 0;

    // Per-unit last host time actually written, for the monotonicity measurement.
    // WR_TIME_UNKNOWN (INT64_MIN) is the "nothing written yet" sentinel and can
    // never be a real mapped time, so no first-sample special case is needed.
    wr_time_us m_lastWrittenUs[WR_UNIT_COUNT] = { WR_TIME_UNKNOWN, WR_TIME_UNKNOWN };
    quint64    m_nonMonotonic[WR_UNIT_COUNT]  = { 0, 0 };
    wr_time_us m_maxBackStepUs[WR_UNIT_COUNT] = { 0, 0 };

    // ⚠ THE VALUES ARE KEPT, BECAUSE A MEDIAN CANNOT BE STREAMED. libwrist's
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

    // WR_EV_CLOCK_DEGRADED application-log rate limiting — see handleEvent().
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

    void noteProvenance(const wr_sample &s);   // I/O thread; needs a mapped host time

    // ── Deferred history state, I/O thread unless noted ─────────────────────
    // The session's policy, kept from initialise() so reserveHistory() can hand
    // it to wr_history_request_around() — which is the only way the pre/post
    // roll fields set there reach a request.
    wr_session_policy m_policy{};
    uint64_t m_historyRequestId = 0;
    // Read from the GUI thread by HmInstance::historyPending(), so atomic rather
    // than mutex-held: it is polled every 50 ms during a gather.
    std::atomic<bool> m_historyPending{ false };
    // Written on the I/O thread, taken on the GUI thread — under m_mutex.
    pinpoint::hm::HistoryResult m_historyResult;

    mutable QMutex m_mutex;
    Snapshot       m_snap;
    // Guarded by m_mutex alongside m_snap, for the same reason: it is written on
    // the I/O thread as the presence event is drained and read on the GUI thread.
    // ⚠ LATEST ONLY, AND NEVER PERSISTED. A second measurement replaces the first
    // because a new calibration replaces the old one; there is no history of
    // attempts to compare, on purpose (§8.2 shows a comparison would prefer the
    // worst attempt available), and nothing here reaches a disk.
    pinpoint::hm::ReferenceAnchor m_anchor;
};
