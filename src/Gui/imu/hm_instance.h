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

#include <QObject>
#include <QQuaternion>
#include <QStringList>
#include <QTimer>

#include <hackmotion/sample.h>      // hm_unit — the cable-fixed unit ordering

#include "device_enumerator.h"
#include "imu_device.h"
#include "types.h"

class HmSessionWorker;
class QThread;
namespace pinpoint { class EventBuffer; }

// ---------------------------------------------------------------------------
// HmUnit — one of the wG3's two sensor units, as the viz components see it
// ---------------------------------------------------------------------------
//
// The wG3 is ONE BLE peripheral carrying TWO sensor units on a cable: wire
// block 0 is the lower arm, block 1 is the palm (hm_unit / spec §6.3). The
// order is fixed by the wiring and is not configurable — a consumer that swaps
// them produces a plausible-looking wrist angle that is simply MIRRORED, which
// every plausibility check passes.
//
// WHY A SEPARATE QOBJECT PER UNIT. `ImuVizView.qml` and `ArmVizView.qml` bind
// to a *controller* object and resolve `quatW`, `accelX`, `anatQuat` and the
// rest by NAME through the metaobject. They neither know nor care what class
// answers. So the cheapest way to drive two live orientation cubes from one
// device is to hand the two views two objects that duck-type as an
// `ImuInstance`, rather than to teach the views about a device that has two of
// everything. That is what this class is: the property surface those two QML
// files actually read, and nothing else.
//
// ⚠ THERE IS NO "DEVICE ACCELERATION" HERE, AND THERE MUST NEVER BE ONE. The
// two units sit 3-8 cm apart on hand and forearm, so under rotation they are at
// different radii and their linear accelerations differ by roughly ω²r — the
// palm unit read 31-51 m/s² MORE than the lower-arm unit, consistently, across
// five golf swings (§6.4). They are SUPPOSED to disagree; a disagreement is not
// a fault and an average of the two is not a measurement of anything. Any
// quantity derived from linear acceleration names the unit it came from, which
// is exactly why this state lives per unit and there is no aggregate above it.
class HmUnit : public QObject
{
    Q_OBJECT

    // ── Identity ─────────────────────────────────────────────────────────────
    Q_PROPERTY(QString unitId    READ unitId    CONSTANT)
    Q_PROPERTY(QString unitLabel READ unitLabel CONSTANT)

    // ── What ImuVizView.qml reads ────────────────────────────────────────────
    Q_PROPERTY(float quatW  READ quatW  NOTIFY quatChanged)
    Q_PROPERTY(float quatX  READ quatX  NOTIFY quatChanged)
    Q_PROPERTY(float quatY  READ quatY  NOTIFY quatChanged)
    Q_PROPERTY(float quatZ  READ quatZ  NOTIFY quatChanged)
    Q_PROPERTY(float accelX READ accelX NOTIFY accelChanged)
    Q_PROPERTY(float accelY READ accelY NOTIFY accelChanged)
    Q_PROPERTY(float accelZ READ accelZ NOTIFY accelChanged)

    // Diagnostic readouts, same names ImuInstance uses so a panel can show
    // either kind without a branch.
    Q_PROPERTY(float eulerRoll  READ eulerRoll  NOTIFY quatChanged)
    Q_PROPERTY(float eulerPitch READ eulerPitch NOTIFY quatChanged)
    Q_PROPERTY(float eulerYaw   READ eulerYaw   NOTIFY quatChanged)
    Q_PROPERTY(float angularVelocityDps READ angularVelocityDps NOTIFY angularVelocityDpsChanged)

    // ── What ArmVizView.qml's quatApplyCalib() reads ──────────────────────────
    // Both paths of that function have to be well-defined for a HackMotion unit
    // or the binding throws: the anatomical path (anatCalibrated + anatQuat) and
    // the legacy single-factor fallback (calibrated + calibTransform).
    Q_PROPERTY(bool        anatCalibrated READ anatCalibrated NOTIFY anatCalibratedChanged)
    Q_PROPERTY(QQuaternion anatQuat       READ anatQuat       NOTIFY quatChanged)
    Q_PROPERTY(bool        calibrated     READ calibrated     NOTIFY calibratedChanged)
    Q_PROPERTY(QQuaternion calibTransform READ calibTransform NOTIFY calibratedChanged)

public:
    // deviceId is the enumerator's id for the peripheral; `unit` picks which of
    // its two blocks this object represents.
    HmUnit(const QString &deviceId, hm_unit unit, QObject *parent = nullptr);

    hm_unit unit()      const { return m_unit; }
    // ⚠ "<deviceId>#lowerArm" / "<deviceId>#palm", and the exact string matters:
    // it becomes the EventBuffer SourceDescriptor::identifier in Phase B and the
    // AppSettings::imuPlacement key in Phase C. Both are persisted, so it is
    // fixed here, once, rather than spelled again at each of those sites.
    QString unitId()    const { return m_unitId; }
    QString unitLabel() const { return m_unitLabel; }   // "Lower arm" / "Palm"

    float quatW() const { return m_quatW; }
    float quatX() const { return m_quatX; }
    float quatY() const { return m_quatY; }
    float quatZ() const { return m_quatZ; }
    // ⚠ m/s², gravity-removed LINEAR acceleration — it reads ≈0 at rest, and it
    // is not the same physical quantity a Witmotion lane's accel channel holds.
    // Displayed in its native units on purpose; Phase B converts to g for
    // ImuSample and records in provenance that the two are not comparable.
    float accelX() const { return m_accelX; }
    float accelY() const { return m_accelY; }
    float accelZ() const { return m_accelZ; }
    float eulerRoll()  const { return m_eulerRoll;  }
    float eulerPitch() const { return m_eulerPitch; }
    float eulerYaw()   const { return m_eulerYaw;   }
    // Gyro magnitude, straight from the device's own rate channel — not a
    // quaternion difference. There is nothing to estimate here.
    float angularVelocityDps() const { return m_angularVelocityDps; }

    // ⚠ PHASE A HAS NO ANATOMICAL FRAME, AND WILL NOT PRETEND OTHERWISE.
    // The device applies its own calibration in ITS anatomical convention
    // (§8.1); the constant per-unit rotation from that convention to ours is
    // solved empirically in Phase D and is the linchpin of the integration.
    // Until it exists, anatCalibrated is false and anatQuat is identity, so
    // ArmVizView parks the segment at rest rather than driving it with a frame
    // nobody has reconciled. Inventing a transform here would produce a display
    // that looks right and is mirrored — F3 in the integration brief.
    bool        anatCalibrated() const { return m_anatCalibrated; }
    QQuaternion anatQuat()       const { return m_anatQuat; }
    // Exists only so ArmVizView's fallback path is defined. There is no
    // host-side zeroing on this device, so this never becomes true.
    bool        calibrated()     const { return false; }
    QQuaternion calibTransform() const { return QQuaternion(1.0f, 0.0f, 0.0f, 0.0f); }

    // Session-metadata accessors, matching ImuInstance's so a future binding
    // writer can ask either kind. Identity until Phase D solves R_unit.
    QQuaternion alignA() const { return m_alignA; }
    QQuaternion mountM() const { return m_mountM; }

    // Phase B: the EventBuffer source this unit writes. Assigned at
    // registration and read back by HmInstance::sourceIds().
    pinpoint::SourceId sourceId() const { return m_sourceId; }
    void setSourceId(pinpoint::SourceId id) { m_sourceId = id; }

signals:
    void quatChanged();
    void accelChanged();
    void angularVelocityDpsChanged();
    void anatCalibratedChanged();
    void calibratedChanged();

private:
    friend class HmInstance;   // the 60 Hz display tick writes these members

    hm_unit m_unit;
    QString m_unitId;
    QString m_unitLabel;

    // ⚠ THE STREAMED QUATERNION MAPS WORLD → BODY (§6.7), the conjugate of what
    // most IMU code assumes. It is stored and displayed exactly as it arrives.
    // Phase A only feeds a cube, and the convention question belongs to Phase D
    // together with the frame solve — conjugating it here because the cube
    // "looks better" would bake a guess into the one place nothing checks it.
    float m_quatW = 1.0f, m_quatX = 0.0f, m_quatY = 0.0f, m_quatZ = 0.0f;
    float m_accelX = 0.0f, m_accelY = 0.0f, m_accelZ = 0.0f;
    float m_eulerRoll = 0.0f, m_eulerPitch = 0.0f, m_eulerYaw = 0.0f;
    float m_angularVelocityDps = 0.0f;

    bool        m_anatCalibrated = false;
    QQuaternion m_anatQuat{ 1.0f, 0.0f, 0.0f, 0.0f };
    QQuaternion m_alignA  { 1.0f, 0.0f, 0.0f, 0.0f };
    QQuaternion m_mountM  { 1.0f, 0.0f, 0.0f, 0.0f };

    pinpoint::SourceId m_sourceId = pinpoint::kInvalidSourceId;
};

// ---------------------------------------------------------------------------
// HmInstance — one HackMotion wG3, as ImuManager holds it
// ---------------------------------------------------------------------------
//
// A PEER of ImuInstance, not a subclass of it and not a subclass of ImuBase.
// The two share a shape — I/O-thread ownership of the link, a 60 Hz display
// tick that is the only emitter of the high-rate signals, retry with backoff,
// a log ring — and share almost no mechanism: libhackmotion owns framing,
// decode and fusion; the device owns calibration; and one peripheral produces
// two units. Both kinds are held through ImuDeviceBase, which is the
// device-kind-agnostic slice ImuManager actually calls.
//
// PHASE B SCOPE. This registers TWO EventBuffer sources — one per unit, keyed
// on HmUnit::unitId() — and every drained hm_sample becomes two ImuSample ring
// writes, both stamped with the sample's mapped host time. The display path
// (60 Hz tick, two orientation cubes) is unchanged and still shows only the
// newest sample; the RING gets every one of them, because the ring is the
// recording. What is still absent is an anatomical frame (Phase D), the
// calibration flow (Phase C) and the deferred history pull (Phase E).
class HmInstance : public ImuDeviceBase
{
    Q_OBJECT

    Q_PROPERTY(QString     stateLabel        READ stateLabel        NOTIFY stateLabelChanged)
    Q_PROPERTY(bool        imuConnected      READ imuConnected      NOTIFY imuConnectedChanged)
    Q_PROPERTY(bool        busy              READ busy              NOTIFY busyChanged)
    Q_PROPERTY(int         batteryPercent    READ batteryPercent    NOTIFY batteryPercentChanged)
    Q_PROPERTY(double      dataRateHz        READ dataRateHz        NOTIFY dataRateHzChanged)
    Q_PROPERTY(QString     deviceId          READ deviceId          CONSTANT)
    Q_PROPERTY(QString     deviceDescription READ deviceDescription NOTIFY deviceDescriptionChanged)
    Q_PROPERTY(QStringList logEntries        READ logEntries        CONSTANT)

    // The two units, as QObject* so QML can bind an ImuVizView's `controller`
    // straight onto one of them. CONSTANT: the objects live as long as the
    // instance does; their CONTENTS notify.
    Q_PROPERTY(QObject *unitLowerArm READ unitLowerArmObject CONSTANT)
    Q_PROPERTY(QObject *unitPalm     READ unitPalmObject     CONSTANT)

    // ⚠ THE PRESENCE ANGLE IS NOT A QUALITY SCORE AND IT INVERTS. §8.2 measured
    // the correct routine at 1.96°, a raise about the wrong axis at 6.10°, and
    // NO RAISE AT ALL at 0.70° — the attempt carrying no axis information scored
    // best, because this figure tests only the zeroing. A UI that ranks attempts
    // on it, or lets a coach "recalibrate until the number goes down", would
    // systematically prefer the worst calibration available. Its one sound use
    // is catching "calibration never happened or was lost", where the gap is an
    // order of magnitude. Show it as STATE, never as a score.
    //
    // Declared in Phase A so Phase C only has to fill them; NaN / HM_CALP_IDLE
    // until then.
    Q_PROPERTY(double presenceAngleDeg READ presenceAngleDeg NOTIFY calibrationStateChanged)
    Q_PROPERTY(int    calibrationPhase READ calibrationPhase NOTIFY calibrationStateChanged)

public:
    // ioThread is ImuManager's shared IMU I/O thread. Everything that touches
    // the hm_session lives there — see HmSessionWorker in the .cpp and the
    // threading contract at the top of hackmotion/session.h.
    explicit HmInstance(const Device &device,
                        pinpoint::EventBuffer *buffer,
                        QThread *ioThread,
                        QObject *parent = nullptr);
    ~HmInstance() override;

    // ── ImuDeviceBase ────────────────────────────────────────────────────────
    QString     deviceId()          const override { return m_deviceId; }
    QString     deviceDescription() const override { return m_deviceDescription; }
    QStringList logEntries()        const override { return m_logEntries; }

    QString stateLabel()     const override { return m_stateLabel; }
    bool    imuConnected()   const override { return m_connected; }
    bool    busy()           const override { return m_busy; }
    int     batteryPercent() const override { return m_batteryPercent; }
    double  dataRateHz()     const override { return m_dataRateHz; }

    std::vector<pinpoint::SourceId> sourceIds() const override;

    // ⚠ PARALLEL TO sourceIds() AND THE SAME LENGTH, including when a
    // registration failed and the vector is short. The resource monitor pairs
    // them positionally, so a labels list built independently of the ids would
    // mislabel every row of a partially-registered device rather than omit one.
    // The strings come from HmUnit::unitLabel(), so they are spelled once and
    // stay translated.
    QStringList sourceLabels() const override;

    // Mean of hm_sample.skew_us over every sample this session, NaN until one has
    // been seen. ⚠ PROVENANCE, NOT A CORRECTION: the two units' blocks are NOT
    // paired as simultaneous and this value is never applied to a timestamp. §10.3
    // measures a stable 59 ticks (0.92 ms) whose physical meaning is unresolved —
    // real sampling skew and arbitrary phase between two free-running counters
    // cannot be told apart from the counters alone — so the honest thing is to
    // carry it into swing.json and let the analysis decide. Read by the export
    // path; there is no provenance block to put it in yet (Phase E builds one).
    double skewUsMean() const;

    void start()               override;
    void stop()                override;
    void deregisterFromBuffer() override;

    // ── The two units ────────────────────────────────────────────────────────
    HmUnit  *unitLowerArm()       const { return m_lowerArm; }
    HmUnit  *unitPalm()           const { return m_palm; }
    QObject *unitLowerArmObject() const { return m_lowerArm; }
    QObject *unitPalmObject()     const { return m_palm; }

    double presenceAngleDeg() const { return m_presenceAngleDeg; }
    int    calibrationPhase() const { return m_calibrationPhase; }

    Q_INVOKABLE QString saveLog();

signals:
    void deviceDescriptionChanged();
    void calibrationStateChanged();

    // The negotiated ATT MTU is below HM_MIN_ATT_MTU (96), so the library
    // refuses to run. ⚠ Its own error, deliberately not folded into a generic
    // connect failure: everything up to this point SUCCEEDED, no Qt platform
    // lets an application request an MTU, and therefore nothing a retry does
    // can change the outcome. A UI that treats it as transient retries forever
    // against a wall; a UI that shows it as "connection failed" sends the coach
    // looking for a flat battery.
    void mtuRejected(int negotiated, int required);

private:
    static QString timestamp();
    void appendLog(const QString &text);
    void setStateLabel(const QString &s);
    void onTransportState(int state);            // BleImuTransport::State, as int
    void resetStreamingState();
    void onConnectionLost(bool fromError);
    void handleConnectFailure();
    int  retryDelayMs(int attempt) const;

    // The two sources are registered against this in the constructor and the
    // ids handed to the HmUnits; the worker holds the same pointer for the ring
    // writes. Null is legal and means "no recording" — every consumer already
    // handles an empty sourceIds().
    pinpoint::EventBuffer *m_eventBuffer = nullptr;

    // I/O-thread residents. The worker has no QObject parent (parenting would
    // fight moveToThread) and is destroyed with deleteLater onto the I/O thread,
    // which ImuManager joins after the instances are gone.
    QThread         *m_ioThread = nullptr;
    HmSessionWorker *m_worker   = nullptr;

    Device      m_device;
    QString     m_deviceId;
    QString     m_deviceDescription;
    QStringList m_logEntries;

    HmUnit *m_lowerArm = nullptr;
    HmUnit *m_palm     = nullptr;

    // Retry. m_connecting = "a connect attempt is in flight and its outcome is
    // unresolved". Whichever of the transport's error/disconnect pair lands
    // first consumes it and owns the retry decision, so the decision is taken
    // exactly once regardless of the order the platform emits them in — the
    // ordering fix documented at imu_instance.h:236-243, replicated rather than
    // reinvented.
    QTimer m_retryTimer;
    int    m_retryCount     = 0;
    bool   m_connecting     = false;
    bool   m_attemptingConn = false;
    // Set from HM_EV_LINK_DOWN's recovery advice when the library classifies the
    // drop as one no retry can fix — the device slept and needs a physical
    // button press, another application holds the single allowed connection, or
    // we powered it off ourselves. It only ever CANCELS a pending retry, which
    // is why the classification arriving a moment after the transport's own
    // disconnect signal is harmless.
    bool   m_retrySuppressed = false;
    // Did THIS connect attempt ever reach GATT? It decides whether a failure is
    // worth retrying at all, and the answer is device-specific rather than
    // stylistic — see handleConnectFailure(). Reset by start(), set by Ready.
    bool   m_gattReachedThisAttempt = false;

    // 60 Hz display tick — the ONLY emitter of the high-rate change signals.
    // The decode hot path is on the I/O thread; this copies the worker's latest
    // snapshot into the two HmUnits and emits. Without it a burst reaching the
    // device's full ≈799.2 Hz internal rate (which happens in every session
    // containing motion, §6.6) would post ~1,600 property notifications a
    // second at the GUI thread.
    QTimer  m_displayTimer;
    quint64 m_lastSeq        = 0;
    double  m_lastSentRateHz = 0.0;
    float   m_lastSentVelDps[HM_UNIT_COUNT] = { 0.0f, 0.0f };

    // 10 s log summary.
    QTimer  m_logTimer;
    quint64 m_totalSamples    = 0;
    quint64 m_samplesSinceLog = 0;
    quint64 m_lastDroppedLive = 0;
    // Totals as of the previous summary, so a GROWING count warns while a
    // historic one is merely reported. Both are expected to stay at zero for a
    // whole session; they are counted rather than assumed, which is the point.
    quint64 m_lastNoFitSkipped = 0;
    quint64 m_lastNonMonotonic = 0;
    // The skew figure and its spread reach the application log once each — the
    // device log ring carries the running numbers but has no reachable UI.
    bool    m_skewReported     = false;
    bool    m_skewSpreadWarned = false;
    bool    m_relAngleReported = false;

    QString m_stateLabel = QStringLiteral("Disconnected");
    bool    m_connected  = false;
    bool    m_busy       = false;
    int     m_batteryPercent = -1;
    double  m_dataRateHz     = 0.0;

    double m_presenceAngleDeg = 0.0;   // set to NaN in the constructor
    int    m_calibrationPhase = 0;     // HM_CALP_IDLE

    // ── Source-descriptor numbers, and why the two rate figures disagree ──────
    //
    // ⚠ kRingSizingRateHz IS A SIZING CEILING, NOT A CLAIM ABOUT THE RATE.
    // SourceDescriptor::computeSlotCount() is next-pow2(rate × window), so 800 ×
    // 5 s → 4,096 slots × 40 B = 160 KB per unit, 320 KB for the device. The live
    // rate is adaptive: 25 Hz at rest and 100 Hz in motion are two strong modes
    // of a continuum, and dense bursts reach index step 1 — the full ≈799.2 Hz
    // internal rate — in every session containing motion (§6.6). Sizing from an
    // assumed 100 Hz would give 512 slots, i.e. a ring holding 0.64 s of a dense
    // stretch, which silently overwrites the front of a five-second swing window
    // under exactly the conditions that matter most.
    //
    // ⚠ kExpectedInterarrivalUs (25 Hz) DELIBERATELY DISAGREES WITH IT, and the
    // two must not be "fixed" to match. That field feeds ONLY the stall watchdog
    // (EventBuffer::maybeRunWatchdog), which floors at 1 s anyway; declaring the
    // fast end there would flag a resting wrist — which genuinely does drop to
    // ~25 Hz — as a stalled source. Each number is right for its own job.
    // A skew spread wider than this means the ~0.92 ms offset is not the constant
    // §10.3 measured. Set well above that figure (and above Q14/tick quantisation)
    // so only a real departure trips it, not jitter around a stable value.
    static constexpr qint32   kSkewSpreadWarnUs       = 2'000;

    static constexpr uint32_t kRingSizingRateHz        = 800;
    static constexpr int      kSourceWindowMs          = 5'000;
    static constexpr int      kExpectedInterarrivalUs  = 40'000;

    static constexpr int kMaxRetries       = 4;
    static constexpr int kRetryBaseDelayMs = 2'000;
    static constexpr int kRetryMaxDelayMs  = 30'000;
    static constexpr int kLogIntervalMs    = 10'000;
    static constexpr int kDisplayTickMs    = 16;
};
