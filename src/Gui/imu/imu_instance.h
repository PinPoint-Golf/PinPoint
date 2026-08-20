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

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QQuaternion>
#include <QStringList>
#include <QTimer>
#include <QVector3D>

#include "device_enumerator.h"
#include "imu_device.h"
#include "types.h"
#include "wt9011dcl_ble.h"

class ImuIoWorker;
class QThread;
namespace pinpoint { class EventBuffer; }

// Manages the BLE connection lifecycle and live data for a single IMU device.
// Created and owned by ImuManager; one instance per selected device.
//
// Inherits ImuDeviceBase (see imu_device.h) so ImuManager can hold this and
// an HmInstance through one pointer type. Everything below that isn't marked
// `override` is Witmotion-specific and stays off the base on purpose.
class ImuInstance : public ImuDeviceBase
{
    Q_OBJECT

    Q_PROPERTY(QString stateLabel     READ stateLabel     NOTIFY stateLabelChanged)
    Q_PROPERTY(bool    imuConnected   READ imuConnected   NOTIFY imuConnectedChanged)
    Q_PROPERTY(bool    busy           READ busy           NOTIFY busyChanged)
    Q_PROPERTY(float   quatW          READ quatW          NOTIFY quatChanged)
    Q_PROPERTY(float   quatX          READ quatX          NOTIFY quatChanged)
    Q_PROPERTY(float   quatY          READ quatY          NOTIFY quatChanged)
    Q_PROPERTY(float   quatZ          READ quatZ          NOTIFY quatChanged)
    Q_PROPERTY(float   eulerRoll      READ eulerRoll      NOTIFY quatChanged)
    Q_PROPERTY(float   eulerPitch     READ eulerPitch     NOTIFY quatChanged)
    Q_PROPERTY(float   eulerYaw       READ eulerYaw       NOTIFY quatChanged)
    Q_PROPERTY(float   accelX         READ accelX         NOTIFY accelChanged)
    Q_PROPERTY(float   accelY         READ accelY         NOTIFY accelChanged)
    Q_PROPERTY(float   accelZ         READ accelZ         NOTIFY accelChanged)
    Q_PROPERTY(int     outputRateHz   READ outputRateHz   NOTIFY outputRateHzChanged)
    Q_PROPERTY(double  dataRateHz     READ dataRateHz     NOTIFY dataRateHzChanged)
    Q_PROPERTY(int     batteryPercent  READ batteryPercent  NOTIFY batteryPercentChanged)
    Q_PROPERTY(int     gimbalDropCount READ gimbalDropCount NOTIFY gimbalDropCountChanged)
    Q_PROPERTY(float angularVelocityDps READ angularVelocityDps NOTIFY angularVelocityDpsChanged)
    Q_PROPERTY(QString     deviceId          READ deviceId          CONSTANT)
    Q_PROPERTY(QString     deviceDescription READ deviceDescription CONSTANT)
    Q_PROPERTY(QStringList logEntries        READ logEntries        CONSTANT)
    Q_PROPERTY(bool        calibrated         READ calibrated         NOTIFY calibratedChanged)
    Q_PROPERTY(QQuaternion calibArmDown       READ calibArmDown       NOTIFY calibratedChanged)
    Q_PROPERTY(QQuaternion calibArmTPose      READ calibArmTPose      NOTIFY calibratedChanged)
    Q_PROPERTY(QQuaternion calibTransform     READ calibTransform     NOTIFY calibratedChanged)
    Q_PROPERTY(bool        calibrationAngleValid READ calibrationAngleValid NOTIFY calibratedChanged)
    Q_PROPERTY(bool zeroing READ zeroing NOTIFY zeroingChanged)
    // Anatomical orientation q_anat = A * q_raw * M (see imu_calibration.h). The
    // single transform all consumers use; identity-passthrough until calibrated.
    Q_PROPERTY(QQuaternion anatQuat       READ anatQuat       NOTIFY quatChanged)
    Q_PROPERTY(bool        anatCalibrated READ anatCalibrated NOTIFY anatCalibratedChanged)
    // Angle between the solved mounting and the expected nominal (precise mode);
    // a large value means the sensor is mis-seated. 0 until a functional solve.
    Q_PROPERTY(double      mountDeviationDeg READ mountDeviationDeg NOTIFY anatCalibratedChanged)
    // Gravity-direction check at arm-down: angle between M^-1*(arm-down accel) and
    // anatomical "up" (0,-1,0). Large means the sensor is flipped / upside-down
    // (which the long-axis-rotation deviation above is blind to). Set by
    // setNominalCalibration. 0 until calibrated.
    Q_PROPERTY(double      mountGravityErrorDeg READ mountGravityErrorDeg NOTIFY anatCalibratedChanged)

public:
    // ioThread is the ImuManager-owned shared IMU I/O thread: the BLE driver
    // and the per-packet hot path (ImuIoWorker) live there, never on the GUI
    // thread (docs/implementation/imu_io_thread_impl.md).
    explicit ImuInstance(const Device &device,
                         pinpoint::EventBuffer *buffer,
                         QThread *ioThread,
                         QObject *parent = nullptr);
    ~ImuInstance() override;

    // Identification (C++ callers and ResourceMonitor)
    QString            deviceId()          const override { return m_deviceId; }
    QString            deviceDescription() const override { return m_deviceDescription; }
    QStringList        logEntries()        const override { return m_logEntries; }
    // ⚠ Scalar, kept alongside sourceIds() below. shot_processor.cpp and other
    // files outside this task's ownership call this directly and are not to be
    // touched — see imu_manager.cpp's brief. Do not remove.
    pinpoint::SourceId sourceId()          const { return m_imuSourceId; }
    std::vector<pinpoint::SourceId> sourceIds() const override
    {
        if (m_imuSourceId == pinpoint::kInvalidSourceId) return {};
        return { m_imuSourceId };
    }

    // State
    QString stateLabel()     const override { return m_stateLabel; }
    bool    imuConnected()   const override { return m_connected; }
    bool    busy()           const override { return m_busy; }
    float   quatW()          const { return m_quatW; }
    float   quatX()          const { return m_quatX; }
    float   quatY()          const { return m_quatY; }
    float   quatZ()          const { return m_quatZ; }
    float   eulerRoll()      const { return m_eulerRoll;  }
    float   eulerPitch()     const { return m_eulerPitch; }
    float   eulerYaw()       const { return m_eulerYaw;   }
    float   accelX()         const { return m_accelX; }
    float   accelY()         const { return m_accelY; }
    float   accelZ()         const { return m_accelZ; }
    int     outputRateHz()      const { return m_outputRateHz; }
    double  dataRateHz()        const override { return m_dataRateHz; }
    int     batteryPercent()    const override { return m_batteryPercent; }
    int     gimbalDropCount()   const { return m_gimbalDropCount; }
    float   angularVelocityDps() const { return m_angularVelocityDps; }
    bool        calibrated()          const { return m_calibrated; }
    bool        calibrationAngleValid() const { return m_calibrationAngleValid; }
    bool        zeroing()       const { return m_zeroing; }
    QQuaternion calibArmDown()  const { return m_calibArmDown; }
    QQuaternion calibArmTPose() const { return m_calibArmTPose; }
    QQuaternion calibTransform() const { return m_calibTransform; }
    QQuaternion anatQuat()      const { return m_anatQuat; }
    bool        anatCalibrated() const { return m_anatCalibrated; }
    double      mountDeviationDeg() const { return m_mountDeviationDeg; }
    double      mountGravityErrorDeg() const { return m_mountGravityErrorDeg; }
    QQuaternion alignA()        const { return m_alignA; }   // session-metadata accessors
    QQuaternion mountM()        const { return m_mountM; }
    // Wallclock instant of the last successful anatomical calibration (invalid
    // until calibrated; cleared with the calibration). Persisted into saved
    // shots so SwingLab can filter a corpus by calibration freshness.
    QDateTime   calibratedAtUtc() const { return m_calibratedAtUtc; }

    // The two-part mount-gate thresholds (see .claude/calibration.md): gravity
    // catches flips, long-axis deviation catches strap rotation. BOTH must pass.
    static constexpr double kMountDeviationMaxDeg    = 15.0;
    static constexpr double kMountGravityErrorMaxDeg = 25.0;
    // Composite "is calibrated" — anatomical transform valid AND both mount
    // checks within threshold. This is what saved shots record as `calibrated`.
    bool fullyCalibrated() const
    {
        return m_anatCalibrated
            && m_mountDeviationDeg    <= kMountDeviationMaxDeg
            && m_mountGravityErrorDeg <= kMountGravityErrorMaxDeg;
    }

    // BLE chain delay between the physical impact and host arrival of the
    // sample that carries it (connection interval + stack). A measured-ish
    // placeholder until P4 auto-calibration; passed into the worker's
    // detector config and recorded in swing.json, never hard-coded in math.
    static constexpr qint64 kImuBleLatencyUs = 30'000;

    // ⚠ A SIZING CEILING AT REGISTRATION, NOT A CLAIM ABOUT THE RATE — the rate
    // is not known until the device connects and setOutputRateHz() runs.
    // SourceDescriptor::computeSlotCount() is next-pow2(rate × window), and it
    // runs ONCE, at registerSource(). Sizing from the 100 Hz default gave 512
    // slots, i.e. 2.56 s of ring on a device configured for 200 Hz — half the
    // five-second swing window, silently overwritten from the front. Sizing from
    // the fastest selectable rate costs 1,024 × 40 B = 40 KB and is always enough.
    // Once the rate IS known, setOutputRateHz() corrects the descriptor to the
    // negotiated cadence via EventBuffer::updateSourceFormat (ring memory is
    // already allocated, so the correction is metadata only) — the same
    // register-provisional-then-correct pattern CameraInstance uses for
    // resolution. That correction is what the offline re-fusion readers
    // (imu_refusion_check.h, imu_vision_fuser.cpp) integrate at, and it must
    // match ImuBase::fuseRawImu's nominal dt exactly or parity fails for reasons
    // that have nothing to do with the data.
    static constexpr uint32_t kRingSizingRateHz = 200;

    // Lifecycle — called by ImuManager
    void start()               override; // begin BLE connection
    void stop()                override; // disconnect and cancel any retry
    void deregisterFromBuffer() override; // call while EventBuffer is paused

    // Select the local orientation-fusion algorithm (Madgwick / ESKF). Forwards
    // to the device driver, which applies the swap on its packet-consumer thread.
    void setOrientationFilter(OrientationFilterType type);

    // Impact-detector sensitivity (shot detection P1): threshold scale applied
    // to every accel/gyro gate — >1 = less sensitive. Set by ImuManager from
    // AppSettings::swingDetectionSensitivity (Low 1.5 / Medium 1.0 / High 0.7).
    void setImpactSensitivity(float thresholdScale);

    // QML-invokable per-device actions
    Q_INVOKABLE void    zeroOrientation();
    Q_INVOKABLE QString saveLog() override;
    Q_INVOKABLE void    setOutputRateHz(int hz);
    Q_INVOKABLE void    setCalibration(const QQuaternion &armDown, const QQuaternion &tPose);
    Q_INVOKABLE void    clearCalibration();

    // Functional anatomical calibration (see imu_calibration.h). Derives A and M
    // from the arm-down reference plus the forearm long axis (twist swing) and
    // elbow flexion axis (elbow swing), all in the sensor frame.
    Q_INVOKABLE void    setFunctionalCalibration(const QQuaternion &refRaw,
                                                 const QVector3D   &gravityDownSensor,
                                                 const QVector3D   &longAxisSensor,
                                                 const QVector3D   &flexAxisSensor,
                                                 bool               handMount = false);
    Q_INVOKABLE void    clearFunctionalCalibration();

    // Mandated-mount calibration ("quick" mode): the mounting M is the known
    // nominal (imu_calibration::nominalArmMount), so a single arm-down reference
    // is enough — A aligns the reference pose to identity. δM fine-tune (precise
    // mode) comes later.
    Q_INVOKABLE void    setNominalCalibration(const QQuaternion &refRaw, bool handMount = false);

    // Precise refinement (δM fine-tune): apply a small rotation about the segment
    // long axis to the nominal mounting, correcting strap-slop. phiDeg is computed
    // by the caller from a second pose (arm abducted) — see ScreenSessionWizard.
    // Keeps the validated nominal; bounded → cannot flip the frame.
    Q_INVOKABLE void    refineMountAboutLongAxis(const QQuaternion &refRaw,
                                                 double phiDeg, bool handMount = false);

    Q_INVOKABLE void    beginZeroing();

    // beginRawDump/endRawDump: stream every packet's RAW euler/accel/gyro/quaternion
    // to ~/pinpoint_imu_raw.log between begin/end, for offline analysis.
    Q_INVOKABLE void    beginRawDump(const QString &tag);
    Q_INVOKABLE void    endRawDump();

signals:
    // stateLabelChanged / imuConnectedChanged / busyChanged / batteryPercentChanged /
    // dataRateHzChanged / logEntryAdded now live on ImuDeviceBase — declaring them
    // here too would be a hard moc error (duplicate signal in the hierarchy).
    // Every `emit` of these below is unchanged; it just resolves to the inherited
    // member, and existing `connect(inst, &ImuInstance::imuConnectedChanged, …)`
    // call sites still compile against it.
    void quatChanged();
    void accelChanged();
    void outputRateHzChanged();
    void gimbalDropCountChanged();
    void angularVelocityDpsChanged();
    void calibratedChanged();
    void anatCalibratedChanged();
    void zeroingChanged();
    void zeroingConfirmed();
    void zeroingFailed();

    // IMU impact auto-trigger (shot detection P1). estImpactUs is the
    // back-dated true-impact estimate in EventBuffer::nowMicros() domain
    // (peak arrival − kImuBleLatencyUs); confidence derives from swing energy
    // (never peak g — the ±16 g full scale clips real strikes).
    void impactDetected(qint64 estImpactUs, float confidence);

private:
    static QString timestamp();
    void appendLog(const QString &text);
    void setStateLabel(const QString &s);
    void onStateChanged(WT9011DCL_BLE::State s);

    // Connection-failure / loss handling. BlueZ emits errorOccurred() and
    // disconnected() (in either order) for a failed connect; both funnel through
    // onConnectionLost(), and the first to arrive consumes m_connecting so the
    // retry decision is made exactly once regardless of signal ordering.
    void resetStreamingState();          // stop live timers, clear rate/battery
    void onConnectionLost(bool fromError);
    void handleConnectFailure();         // schedule a backoff retry, or give up
    int  retryDelayMs(int attempt) const;

    pinpoint::EventBuffer *m_eventBuffer  = nullptr;
    pinpoint::SourceId     m_imuSourceId  = pinpoint::kInvalidSourceId;

    // I/O-thread residents (no QObject parent — parenting would fight
    // moveToThread): destroyed via deleteLater onto the I/O thread, which
    // ImuManager joins after the instances are gone.
    QThread       *m_ioThread          = nullptr;
    WT9011DCL_BLE *m_imu               = nullptr;
    ImuIoWorker   *m_worker            = nullptr;
    Device         m_device;
    QString        m_deviceId;
    QString        m_deviceDescription;
    QStringList    m_logEntries;

    // Retry. m_connecting = "a connect attempt is in flight and its outcome is
    // unresolved" — set in start(), cleared on Ready or by the first failure
    // signal of the connect attempt. This is the single source of truth the
    // retry decision keys off (it is NOT mutated by both the Error and
    // Disconnected handlers, which was the old m_inConnectPhase ordering bug).
    QTimer m_retryTimer;
    int    m_retryCount     = 0;
    bool   m_connecting     = false;
    bool   m_attemptingConn = false;

    // Battery polling
    QTimer m_batteryTimer;
    int    m_batteryRetries = 0;
    static constexpr int kMaxBatteryRetries = 3;

    // Gimbal drop counter polling — fires while connected so the count updates
    // even when all packets are being dropped (no quaternionUpdated to piggyback on).
    QTimer m_gimbalPollTimer;
    static constexpr int kGimbalPollIntervalMs = 200;

    // 60 Hz display tick: copies the worker's LiveSample snapshot into the
    // QML-facing members and emits the change signals — the GUI sees ZERO
    // per-packet events (the hot path lives on the I/O thread).
    QTimer  m_displayTimer;
    quint64 m_lastSeq        = 0;     // last snapshot seq consumed
    quint64 m_seqBase        = 0;     // seq at connection (per-connection totals)
    float   m_lastSentVelDps = 0.0f;
    double  m_lastSentRateHz = 0.0;

    // State
    QString m_stateLabel = QStringLiteral("Disconnected");
    bool    m_connected  = false;
    bool    m_busy       = false;

    // IMU data
    float m_quatW = 1.0f, m_quatX = 0.0f, m_quatY = 0.0f, m_quatZ = 0.0f;
    float m_accelX = 0.0f, m_accelY = 0.0f, m_accelZ = 0.0f;
    // Most recent Euler angles from the device — diagnostic use only.
    float m_eulerRoll  = 0.0f;
    float m_eulerPitch = 0.0f;
    float m_eulerYaw   = 0.0f;
    int   m_outputRateHz   = 100;
    int   m_batteryPercent  = -1;
    int   m_gimbalDropCount = 0;

    // Tick-refreshed copies of the worker's snapshot (members so the QML
    // property reads and the calibration flow's 100 ms polls stay cheap).
    double m_dataRateHz = 0.0;
    float  m_angularVelocityDps = 0.0f;

    // Raw packet streaming state for beginRawDump/endRawDump (off by default).
    // While active, a per-packet queued connection feeds the GUI (diagnostic
    // mode only — zero hot-path cost when off).
    bool        m_rawDump = false;
    QString     m_rawDumpTag;
    QStringList m_rawDumpLines;
    QMetaObject::Connection m_rawDumpConn;

    // Zeroing confirmation — settle-timer approach (no hardware ACK on WT901)
    // Future devices that emit ImuBase::zeroingConfirmed() take precedence over settle.
    QTimer m_zeroSettleTimer;   // 500 ms single-shot: soft confirmation (50 frames at 100 Hz)
    QTimer m_zeroConfirmTimer;  // 30 s single-shot: overall deadline / failure
    bool   m_zeroing = false;

    // Session calibration — arm-down and T-pose reference quaternions.
    // Set by the session wizard; valid only for the life of this instance.
    bool        m_calibrated             = false;
    bool        m_calibrationAngleValid  = true;
    QQuaternion m_calibArmDown  { 1.0f, 0.0f, 0.0f, 0.0f };
    QQuaternion m_calibArmTPose { 1.0f, 0.0f, 0.0f, 0.0f };
    // Derived from setCalibration() — maps raw sensor quaternion to anatomical world frame.
    // Identity until calibrated.
    QQuaternion m_calibTransform { 1.0f, 0.0f, 0.0f, 0.0f };

    // Functional anatomical calibration: q_anat = A * q_raw * M (see imu_calibration.h).
    bool        m_anatCalibrated = false;
    QQuaternion m_alignA  { 1.0f, 0.0f, 0.0f, 0.0f };   // fusion-world -> anatomical-world
    QQuaternion m_mountM  { 1.0f, 0.0f, 0.0f, 0.0f };   // anatomical-body -> sensor-body
    QQuaternion m_anatQuat{ 1.0f, 0.0f, 0.0f, 0.0f };   // cached A * q_raw * M
    double      m_mountDeviationDeg = 0.0;              // |solved M vs nominal| (precise mode)
    double      m_mountGravityErrorDeg = 0.0;          // arm-down gravity vs anatomical up (flip check)
    QDateTime   m_calibratedAtUtc;                     // wallclock of last calibration (invalid = never)

    // Log throttle — summary every 10 s
    QTimer  m_logTimer;
    quint64 m_totalRecords    = 0;
    quint64 m_recordsSinceLog = 0;

    // Auto-retry: a few attempts with exponential backoff (2s, 4s, 8s, …) capped
    // at kRetryMaxDelayMs. The old single 30 s retry was too thin/slow for a
    // flaky BLE link (the documented real-world failure mode).
    static constexpr int kMaxRetries       = 4;
    static constexpr int kRetryBaseDelayMs = 2'000;
    static constexpr int kRetryMaxDelayMs  = 30'000;
    static constexpr int kLogIntervalMs    = 10'000;
};
