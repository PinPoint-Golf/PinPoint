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

#include "hm_capture_provenance.h"  // the pure window arithmetic behind Phase B′

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

    // ⚠ "<deviceId>#lowerArm" / "<deviceId>#palm", and the exact string matters:
    // it is the EventBuffer SourceDescriptor::identifier (Phase B) and the
    // AppSettings::imuPlacement key (Phase C). Both are PERSISTED, so the
    // spelling is fixed here, once, rather than repeated at each of those sites —
    // two spellings would not fail, they would silently orphan a device's
    // placement and its recorded lanes.
    //
    // Static, because both persisted uses need the string for a device with no
    // live HmUnit: ImuManager resolves and migrates placement keys for a wG3 that
    // has been enumerated but never connected. Ask for the string; do not build a
    // throwaway HmUnit to read one off, and do not respell the suffix.
    static QString unitIdFor(const QString &deviceId, hm_unit unit);
    static QString unitLabelFor(hm_unit unit);           // "Lower arm" / "Palm"

    hm_unit unit()      const { return m_unit; }
    QString unitId()    const { return m_unitId; }
    QString unitLabel() const { return m_unitLabel; }

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

    // ⚠ NEITHER PHASE A NOR PHASE C HAS AN ANATOMICAL FRAME, AND NEITHER WILL
    // PRETEND OTHERWISE. ⚠ THIS IS UNCHANGED BY CALIBRATION, which is the trap:
    // once the routine below succeeds the device applies its own transform and the
    // cubes start moving sensibly, and it becomes very tempting to conclude that
    // the frame is now known. It is not. The device calibrates into ITS anatomical
    // convention (§8.1), which the specification never defines; the constant
    // per-unit rotation from that convention to ours is solved empirically in
    // Phase D and is the linchpin of the integration.
    // Until it exists, anatCalibrated is false and anatQuat is identity, so
    // ArmVizView parks the segment at rest rather than driving it with a frame
    // nobody has reconciled. Inventing a transform here — or conjugating the
    // streamed quaternion because a cube "looks better" — would produce a display
    // that looks right and is mirrored: F3 in the integration brief.
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
// recording.
//
// PHASE C SCOPE. The device's own calibration routine, driven from QML through
// the five Q_INVOKABLEs below and observed through the calibration properties.
// ⚠ THE ORDER IS FIXED BY THE LIBRARY and there is no way to express any other:
//
//   connect → start stream → beginCalibration() → confirmHorizontal()
//           → confirmRaise() → (device applies, `0x94`) → confirmReferencePose()
//
// Three things about that sequence are counter-intuitive enough to be worth
// stating here rather than only at the call sites:
//
//   - `0x94` IS NOT A VERDICT. The device applies the transform for every
//     `a2 01`, including attempts an application goes on to reject, so its
//     arrival tells us the frame under the stream CHANGED and nothing about
//     whether it changed correctly. Success is never inferred from it.
//   - THE REFERENCE-POSE STEP IS NOT OPTIONAL. HM_CALP_COMPLETE means the
//     device applied a transform; only a passed presence measurement makes
//     hm_session_calibration_state() read HM_CAL_CALIBRATED. Skipping it leaves
//     every recorded sample flagged HM_CAL_UNKNOWN — and it is also the step
//     that yields the reference anchor Phase D's frame solve needs.
//   - RECONNECT IS NOT RESUME. §8.3 measured 0.70° immediately before dropping
//     a link and 18.80° at the same pose after reconnecting, strap untouched.
//     Link-down and a stream restart both drive this whole surface back to
//     nothing (see invalidateCalibration()), and there is deliberately NO
//     persistence of any of it — see the comment on that function.
//
// What is still absent is an anatomical frame (Phase D) and the deferred
// history pull (Phase E).
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
    // NaN until a presence measurement lands, and driven back to NaN by every
    // invalidation — a stale angle from the previous attempt sitting on screen is
    // the shortest path to the ranking this must not permit.
    Q_PROPERTY(double presenceAngleDeg READ presenceAngleDeg NOTIFY calibrationStateChanged)
    Q_PROPERTY(int    calibrationPhase READ calibrationPhase NOTIFY calibrationStateChanged)

    // ⚠ THE PHASE AND THE STATE ARE TWO DIFFERENT QUESTIONS AND NEITHER IS
    // DERIVABLE FROM THE OTHER. `calibrationPhase` (hm_calibration_phase) is
    // WHERE THE ROUTINE IS; `calibrationState` (hm_calibration_state) is WHAT IS
    // KNOWN ABOUT THE DEVICE'S TRANSFORM. HM_CALP_COMPLETE with
    // HM_CAL_UNKNOWN is an ordinary, expected combination — the device applied
    // something and nobody checked it — which is exactly the mistake the
    // library's own comment at HM_CALP_COMPLETE warns against. So this is read
    // back from hm_session_calibration_state() on every phase event rather than
    // inferred here.
    Q_PROPERTY(int calibrationState READ calibrationState NOTIFY calibrationStateChanged)

    // hm_calibration_abort_reason. ⚠ NOT A SYNONYM FOR "ABORTED":
    // HM_CAL_ABORT_CALLER is carried on a transition to COMPLETE as well, because
    // aborting at HM_CALP_VERIFYING declines the presence check on a transform the
    // device HAS ALREADY APPLIED. Read it with the phase, never instead of it.
    Q_PROPERTY(int calibrationAbortReason READ calibrationAbortReason NOTIFY calibrationStateChanged)

    // Evidence about the HOLD, not about the calibration: the largest angular
    // distance between any sample of the presence run and the run's mean, taken as
    // the worse of the two units. ⚠ A mean without a spread is an estimate without
    // evidence — this is what says whether the athlete held the reference pose or
    // drifted through it, and Phase D looks at it before trusting the anchor. Show
    // it BESIDE the presence figure and never as a quality number. NaN until
    // measured.
    Q_PROPERTY(double poseSpreadMaxDeg    READ poseSpreadMaxDeg    NOTIFY calibrationStateChanged)
    Q_PROPERTY(int    presenceSamplesUsed READ presenceSamplesUsed NOTIFY calibrationStateChanged)

    // ⚠ "WE COULD NOT CHECK", WHICH IS NOT "WE CHECKED AND IT WAS FINE" AND NOT
    // "WE DECLINED TO CHECK". HM_WARN_PRESENCE_NOT_MEASURED means the reference
    // pose was asked for and too few live samples reached the run to average, and
    // the phase reaches HM_CALP_COMPLETE anyway — so a UI keyed only on the phase
    // would claim success. Latched for the current attempt and cleared when the
    // next routine begins. (A DECLINED check — abort at VERIFYING — leaves this
    // false and puts HM_CAL_ABORT_CALLER in calibrationAbortReason instead.)
    Q_PROPERTY(bool presenceNotMeasured READ presenceNotMeasured NOTIFY calibrationStateChanged)

    // A routine is in flight: the phase is neither IDLE, COMPLETE nor ABORTED.
    // The UI uses it to keep the guide on screen and the Calibrate button out of
    // reach; it is NOT a claim that anything has been calibrated.
    Q_PROPERTY(bool calibrationActive READ calibrationActive NOTIFY calibrationStateChanged)

    // ⚠ THE UI MUST NOT OFFER "CALIBRATE" WITHOUT THIS. hm_calibration_begin()
    // returns HM_ERR_NO_STREAM when no stream is running and there is deliberately
    // no AWAIT_STREAM phase to fall into — the device observes a CONTINUOUS raise
    // between the two markers, which two static samples cannot supply, so
    // calibration is not a standalone transaction. Under our one-stream cycle the
    // stream is up from just after HM_EV_READY and stays up, so this is normally
    // true whenever the device is connected — which is a reason to bind it, not a
    // reason to assume it.
    Q_PROPERTY(bool streaming READ streaming NOTIFY streamingChanged)

    // hm_relative_angle_deg() of the newest sample — the readout that PROVES a
    // calibration took, because it is a rotation magnitude and therefore
    // independent of both units' unreconciled frames.
    //
    // ⚠ IT IS ONLY INTERPRETABLE AT REST WITH A STRAIGHT WRIST, where ~15° means
    // uncalibrated and 0.4-0.8° means the transform is applied and holding. In any
    // other pose it is meaningless: the same stream reads 170-180° routinely while
    // the wrist is moving (the library's own five-swing fixture sits in that band
    // for 28 % of its samples), and a UI that shows this number mid-motion is
    // showing a fault that is not there. NaN before the first sample.
    Q_PROPERTY(double relativeAngleDeg READ relativeAngleDeg NOTIFY relativeAngleChanged)

public:
    // ── The reference-pose anchor, kept for Phase D's frame solve ─────────────
    //
    // Every field of hm_calibration_presence_event that cannot be re-derived once
    // the pose has passed. C++ only: Phase D consumes it, QML has no business
    // with it, and nothing here derives anything from it — the solve is Phase D's
    // and this is the raw measurement the library already took.
    //
    // ⚠ BOTH FORMS ARE KEPT ON PURPOSE, because they answer different questions:
    //
    //   the MEAN   — the averaged pose, and the ONLY one a frame solve may use.
    //                A person holding a declared pose still wobbles 0.5-2°, which
    //                is one to two orders larger than Q14 quantisation (~0.007°),
    //                and averaging the run is what removes it.
    //   the MEDOID — one real measured pair, for the angle and its provenance.
    //                ⚠ IT MUST NOT BE USED FOR THE SOLVE: it is selected on the
    //                RELATIVE rotation, which is blind to a whole-arm movement
    //                carrying both units together — precisely the motion that
    //                contaminates an ABSOLUTE pose. However centrally it is
    //                chosen, it still holds whatever the athlete was doing at
    //                that instant.
    //
    // `valid` is the gate and the only gate. A quaternion has no NaN idiom, so
    // there is no in-band sentinel: every other field is meaningless when it is
    // false.
    struct ReferenceAnchor {
        bool        valid = false;
        // (2) THE AVERAGED POSE — what a Phase D frame solve must use.
        QQuaternion qLowerArmMean, qPalmMean;
        // Per unit, [lowerArm, palm]. Check this before trusting the mean.
        float       poseSpreadDeg[2] = { 0.0f, 0.0f };
        // (1) The medoid record — for the angle and its provenance only.
        QQuaternion qLowerArmMedoid, qPalmMedoid;
        quint32     sampleIndex = 0;   // which record the medoid pair came from
        qint32      skewUs      = 0;   // palm − lower_arm for THAT record
        quint8      samplesUsed = 0;
        float       relativeAngleDeg = 0.0f;
    };

    // Latest measurement, invalid until one lands and driven back to invalid by
    // every invalidation.
    ReferenceAnchor referenceAnchor() const { return m_anchor; }

    // ── Capture provenance (Phase B′) ────────────────────────────────────────
    //
    // ⚠ WHY THIS EXISTS. A HackMotion reading reaches swing.json as ten floats
    // (pinpoint::ImuSample) plus a host timestamp. The NUMBERS survive intact —
    // accel is raw counts × 0.001 exactly, the quaternion is i16/16384 exact in
    // float32 — but every field of hm_sample that says WHETHER TO TRUST THEM
    // stops at writeSample(). Two of those are load-bearing and neither can be
    // reconstructed from the recording afterwards:
    //
    //   CALIBRATION STATE — the device applies its own transform, and sample.h is
    //     explicit that it "is not recoverable later, so if the recording does not
    //     carry this flag the mistake is permanent and invisible". Pre-calibration
    //     quaternions are valid geometry and anatomically meaningless (§8.1 puts
    //     the raw mounting offset at 11-15° at a straight wrist).
    //   PINNING — int16 fields SATURATE rather than wrap, so a clipped peak is a
    //     plausible flat top rather than a fault, and nothing else in the protocol
    //     reports it (§6.4: a struck swing reaches 53-58 % of full scale, a
    //     deliberate wrist flick 83 %).
    //
    // ⚠ SPLIT BY HOW THE DATA BEHAVES, NOT BY HOW IMPORTANT IT IS. Calibration
    // state cannot change inside a swing — the link drop that would change it also
    // ends the stream — so it is carried as a SPAN. Pinning can land on any
    // individual sample, so it is carried as TIMESTAMPED ENTRIES a window selects
    // from: a session total would answer "did this session ever clip", which is
    // not the question the analysis of one swing asks.
    // ⚠ THE TYPES AND THE WINDOW ARITHMETIC LIVE IN THE PURE HEADER
    // (Imu/hm_capture_provenance.h), so the rules deciding what a swing is allowed
    // to claim about itself are testable without a device, a session or a BLE link.
    // These aliases exist so call sites keep saying HmInstance::… rather than
    // reaching past this class for a type it hands out.
    using SampleException   = pinpoint::hm::SampleException;
    using CalibrationSpan   = pinpoint::hm::CalibrationSpan;
    using CaptureProvenance = pinpoint::hm::CaptureProvenance;
    static constexpr quint8 kSampleLevel = pinpoint::hm::kSampleLevel;

    // Window-scoped. Callable from any thread; the worker copies out under the
    // same mutex snapshot() uses.
    CaptureProvenance captureProvenance(qint64 windowStartUs, qint64 windowEndUs) const;

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

    double presenceAngleDeg()      const { return m_presenceAngleDeg; }
    int    calibrationPhase()      const { return m_calibrationPhase; }
    int    calibrationState()      const { return m_calibrationState; }
    int    calibrationAbortReason() const { return m_calibrationAbortReason; }
    double poseSpreadMaxDeg()      const { return m_poseSpreadMaxDeg; }
    int    presenceSamplesUsed()   const { return m_presenceSamplesUsed; }
    bool   presenceNotMeasured()   const { return m_presenceNotMeasured; }
    bool   calibrationActive()     const;
    bool   streaming()             const { return m_streaming; }
    double relativeAngleDeg()      const { return m_relativeAngleDeg; }

    // ── The routine, driven from QML ─────────────────────────────────────────
    //
    // ⚠ EVERY ONE OF THESE IS MARSHALLED ONTO THE I/O THREAD AND RETURNS
    // IMMEDIATELY, SO NONE OF THEM CAN REPORT A REFUSAL BY RETURNING ONE. The
    // library's threading contract puts every hm_calibration_* call on the one
    // thread that owns the session, and the hop is a QueuedConnection rather than
    // a BlockingQueuedConnection deliberately: a calibration call must never block
    // the GUI thread, whose renderer is PACING the athlete through a raise the
    // device watches continuously. hm_calibration_confirm_reference_pose() is
    // documented as returning BEFORE the measurement exists in any case, so there
    // is nothing useful to wait for.
    //
    // ⚠ A UI THAT WAITS ON A RETURN VALUE HERE WILL HANG. The hm_status the
    // library produced reaches the GUI thread ONLY as
    // calibrationCallRefused(status, call) — that signal is the entire refusal
    // channel. Everything else arrives as a phase or presence event.
    Q_INVOKABLE void beginCalibration();
    Q_INVOKABLE void confirmHorizontal();
    Q_INVOKABLE void confirmRaise();
    Q_INVOKABLE void confirmReferencePose();
    Q_INVOKABLE void abortCalibration();

    Q_INVOKABLE QString saveLog() override;

signals:
    void deviceDescriptionChanged();
    void calibrationStateChanged();
    void streamingChanged();
    void relativeAngleChanged();

    // ⚠ THE ONLY WAY A REFUSED CALIBRATION CALL IS REPORTED — see the
    // Q_INVOKABLEs above for why a return value cannot serve. `call` is the
    // function name so a log line says WHICH step was refused, and `status` is
    // carried through raw rather than folded into a generic error because the
    // three reachable values want three different things from the coach:
    //
    //   HM_ERR_NO_STREAM     the stream is not running. Nothing to wait for and
    //                        nothing to retry — there is no AWAIT_STREAM phase on
    //                        purpose. Reconnect.
    //   HM_ERR_BUSY          a history bracket is open (Phase E). The presence
    //                        check is measured FROM live samples and a retrieval
    //                        suspends them, so the right answer is "try again in a
    //                        second" — the athlete is standing still either way.
    //                        ⚠ Not reachable today; Phase E makes it reachable,
    //                        which is why it is carried legibly now rather than
    //                        discovered then.
    //   HM_ERR_INVALID_STATE no routine is running (abort), or the step is out of
    //                        order. A UI bug, not a device condition.
    void calibrationCallRefused(int status, const QString &call);

    // ⚠ THE CALIBRATION IS GONE AND THE COACH MUST RE-RUN THE ROUTINE. Emitted on
    // link-down (which ALWAYS invalidates — §8.3 measured 0.70° → 18.80° at the
    // same pose across a plain disconnect, strap untouched) and on a stream
    // restart. RECONNECT IS NOT RESUME, the library makes resume un-expressible,
    // and this signal is how our UI matches that rather than papering over it.
    void calibrationInvalidated();

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

    // ⚠ DRIVES THE WHOLE CALIBRATION SURFACE BACK TO NOTHING and says so. Phase
    // → IDLE, state → whatever the LIBRARY now reports (UNCALIBRATED after a
    // link-down, UNKNOWN after a stream restart — read, never guessed), presence
    // angle and pose spread → NaN, samples → 0, the reference anchor → invalid,
    // then calibrationInvalidated().
    //
    // ⚠ AND IT STORES NOTHING, ANYWHERE. There is no save, no load and no "reuse
    // last session", and none may ever be added. §8.3: a calibration is lost by a
    // power cycle, by remounting AND by a plain disconnect, and the library
    // deliberately ships no hm_calibration_save()/_load() because such a
    // convenience produces confidently wrong data with no error anywhere. Anything
    // persisted here would be re-applied to a device whose transform is gone, and
    // the resulting wrist angles would be plausible, permanently wrong, and
    // unfalsifiable from the recording. The absence is the feature.
    void invalidateCalibration(int libraryState, const QString &why);
    // Clears just the presence half — called when a NEW routine begins, so the
    // previous attempt's angle cannot sit on screen next to the new one and invite
    // the ranking §8.2 shows would prefer the worst attempt available.
    void clearPresenceSurface();

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
    double  m_lastSentRelAngleDeg = 0.0;

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

    // ── The calibration surface. Written only from the worker's queued
    // calibration signals and from invalidateCalibration(); the enums live in
    // <hackmotion/event.h> and are held as int here so this header stays the
    // light one it is. Every default is established in the constructor, where
    // that header is in scope, rather than spelled twice.
    double m_presenceAngleDeg = 0.0;   // set to NaN in the constructor
    int    m_calibrationPhase = 0;     // HM_CALP_IDLE
    int    m_calibrationState = 0;     // HM_CAL_UNKNOWN
    int    m_calibrationAbortReason = 0;   // HM_CAL_ABORT_NONE
    double m_poseSpreadMaxDeg = 0.0;   // set to NaN in the constructor
    int    m_presenceSamplesUsed = 0;
    bool   m_presenceNotMeasured = false;
    bool   m_streaming = false;
    // ⚠ NaN, not 0. Zero degrees is the reading a perfectly applied calibration
    // approaches, so a default of 0 would claim the best possible number before a
    // single sample has arrived.
    double m_relativeAngleDeg = 0.0;   // set to NaN in the constructor

    ReferenceAnchor m_anchor;

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
