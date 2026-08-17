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

// Wrist/arm positional calibration flow. Relocated verbatim from the session
// wizard's Panel 3 so the SAME state machine runs in two hosts:
//   • the start-session wizard  (layoutMode "full")
//   • the Wrist toolbar IMU panel (layoutMode "compact")
//
// The state machine (phases 0→1→2, the timer chain, stillness-gated capture,
// mount validation) lives on the internal `d` object; the BodyVizView guide and
// the status display live in the active layout Component (full or compact). The
// timers reach the loaded BodyVizView through `flow._bvv`.
//
// Auto-start is gated on `flow.visible && flow._autoStartGate` (replacing the
// wizard's `currentStep === stepCalibrate` guard) so the flow runs whether hosted
// by a wizard step or a panel that toggles it visible.
//
// ── TWO ROUTINES, BRANCHED AT THE TOP ──────────────────────────────────────────
// There are two state machines in this file, and which one runs is decided ONCE,
// from the DEVICE in slot A:
//
//   • Witmotion (`d.calibPhase` 0→1→2) — OUR routine. Arm-down + T-pose captures,
//     stillness-gated, then the abduction refinement and the mount-validation
//     gate. It calls ImuInstance-only methods (clearCalibration,
//     setNominalCalibration, refineMountAboutLongAxis, calibArmDown,
//     mountDeviationDeg, calibrationAngleValid), none of which exist on a
//     HackMotion — which is why the branch is at the top and not sprinkled
//     through the phases.
//
//   • HackMotion (`d.hmStep` 0→1→…→5) — the DEVICE's routine, driven through
//     libhackmotion: forearm horizontal → one continuous ~30° raise across the
//     chest → the device applies its own transform → reference pose → presence
//     check. The order is fixed and enforced BY THE LIBRARY; we issue markers and
//     read state. There is nothing to solve host-side, nothing to store, and no
//     mount check to run — so the refinement, the arm-down/T-pose captures and
//     the AngleWarning belong to the Witmotion routine ONLY.
//
// Both share the BodyVizView host, the status sub-components and the
// completed()/cancelled() signals. ⚠ Nothing about the HackMotion calibration is
// persisted, ever: a plain BLE disconnect destroys it (measured 0.70° → 18.80° at
// the same pose, strap untouched), the library makes resume un-expressible, and
// this UI matches that rather than papering over it.

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PinPointStudio

Item {
    id: flow

    // ── Config ────────────────────────────────────────────────────────────────
    property string layoutMode: "full"     // "full" (wizard) | "compact" (toolbar panel)
    property bool   showHeader: true        // step eyebrow + "Calibrate Sensors" title
    property string stepLabel:  qsTr("CALIBRATE")   // host-supplied eyebrow text

    // ── Outputs (read-only) — hosts bind to these ─────────────────────────────
    readonly property alias calibrationDone:   d.calibrationDone
    readonly property alias calibrationFailed: d.calibrationFailed
    readonly property alias mountFailed:       d.mountFailed
    readonly property alias phase:             d.calibPhase
    readonly property alias leadImu:           d.leadImu   // slot-A viz UNIT (or null)
    // Which routine is in play — hosts word their own copy from this.
    readonly property alias isHackMotion:      d.isHackMotion

    // ── Signals ─────────────────────────────────────────────────────────────
    signal completed()
    signal cancelled()      // compact-mode "Back/Cancel"

    // ── Internal wiring ───────────────────────────────────────────────────────
    // Auto-start gate: true between begin() and reset(). Combined with visibility
    // it replaces the wizard's currentStep guard.
    property bool _autoStartGate: false
    // The active layout's BodyVizView assigns itself here on load so the
    // flow-level timers can drive its guide animation across the Loader boundary.
    property var _bvv: null

    // ── API (host-driven entry points) ────────────────────────────────────────
    function begin()         { d._reset(); _autoStartGate = true }
    function reset()         { d._reset(); _autoStartGate = false }
    // Restore the completed state WITHOUT re-running — used on backward nav into
    // the wizard step, or when the lead IMU is already calibrated this session.
    // Consolidates the wizard's two former restore branches; both ended in the
    // same visible state (phase 2, progress full, done).
    function showCompleted() {
        // HackMotion: there is nothing host-side to restore — the DEVICE holds the
        // calibration and only its own state says whether one is live. So the
        // restore is a read of that state, never a replay of stored fields, and it
        // must not synthesise a Witmotion phase-2 completion.
        if (d.isHackMotion) {
            var dev = d.leadDevice
            d.hmStep          = (dev && dev.calibrationState === d.calCalibrated) ? 5 : 0
            d.calibrationDone = (d.hmStep === 5)
            _autoStartGate    = false
            return
        }
        var imu = d.leadImu
        if (imu !== null && imu.calibrated) {
            d.calibArmDownQuat  = imu.calibArmDown
            d.calibArmTPoseQuat = imu.calibArmTPose
        }
        d.calibPhase         = 2
        d.phase1AccumMs      = 0
        d.stableAccumMs      = d._captureHoldMs
        d.phaseProgress      = 1.0
        d._animStage         = ""
        d._animateLeadArm    = false
        d._leadArmTarget     = d.leadArmDownQuat
        d.calibrationFailed  = false
        d.mountFailed        = false
        d.mountFailMsg       = ""
        d._armDownCaptured   = true
        d._phase1MinHoldDone = true
        d.calibrationDone    = true
        _autoStartGate       = false
    }

    onCalibrationDoneChanged: if (calibrationDone) { calibCompleteTing.play(); flow.completed() }

    TingPlayer { id: calibCompleteTing; frequency: 4186.0 }  // C8 — two octaves above the ball ting

    // ── State machine + helpers (relocated from the wizard's calibPanel) ───────
    QtObject {
        id: d

        // phase 0 — user holds lead arm straight down; wait for IMU stable
        // phase 1 — animated guide: arm moves from down → T-pose
        // phase 2 — user raises arm to T-pose; hold to capture
        property int  calibPhase:    0
        property real phaseProgress: 0.0   // 0–1 for phase 2 hold timer

        // Captured reference quaternions from the lead-arm IMU.
        property var  calibArmDownQuat:  null   // phase 0 — arm relaxed at side
        property var  calibArmTPoseQuat: null   // phase 2 — arm raised to T-pose
        property bool calibrationDone:   false
        // Diagnostic — Euler angles at each calibration capture (cleared by _reset)
        property var calibArmDownEuler:  null   // { roll, pitch, yaw } at arm-down capture
        property var calibArmTPoseEuler: null   // { roll, pitch, yaw } at T-pose capture

        readonly property bool rightHanded: athleteController.currentHandedness !== "Left"

        // T-pose seed quaternions (match BodyPoseAdapter pre-seed values).
        readonly property quaternion tPoseQuat: d.rightHanded
            ? Qt.quaternion( 0.9948, -0.0105, -0.0011,  0.1012)   // left arm
            : Qt.quaternion( 0.9948, -0.0105,  0.0011, -0.1011)   // right arm

        // Arm-down: shoulder-local +Z = world -Y (verified by computing
        // shoulder.rotation * (0,0,1) for both left and right shoulders).
        // Rotating armNode's +Y to shoulder-local +Z = R_x(+90°) for both arms.
        // The mirror symmetry of the shoulder nodes means both use the same value.
        readonly property quaternion leadArmDownQuat:  Qt.quaternion(0.7071, 0.7071, 0, 0)
        readonly property quaternion trailArmDownQuat: Qt.quaternion(0.7071, 0.7071, 0, 0)

        // ── Slot resolution ───────────────────────────────────────────────────
        // ImuManager owns the placement lookup now (unit-keyed for a HackMotion,
        // whose two units are keyed "<deviceId>#lowerArm" / "#palm", so ONE
        // peripheral fills slots A and B). Walking imuDeviceList against a scalar
        // imuPlacement here would only re-implement that, wrongly.
        //
        // ⚠ instanceForSlot() returns the VIZ object — an HmUnit for a HackMotion
        // slot, an ImuInstance for a Witmotion one — while deviceForSlot() returns
        // the owning PERIPHERAL. Both answer to QML by name and they are DIFFERENT
        // OBJECTS for a HackMotion. Cube/viz and per-sensor quaternions are unit
        // work; calibration is device work.
        //
        // The three assigned `var` reads are required reactive dependencies:
        // instanceForSlot() is a Q_INVOKABLE, not a property, so without them
        // these bindings would resolve once and never re-evaluate when the
        // instance is created by the Connect button on the previous step or the
        // placement is edited. ⚠ A bare `imuManager.instances` statement is DROPPED
        // by the QML compiler and takes the dependency with it — it must be
        // ASSIGNED.
        readonly property QtObject leadImu: {
            var _dep  = imuManager.instances
            var _dep2 = appSettings.imuPlacement
            var _dep3 = imuManager.imuDeviceList
            return imuManager.instanceForSlot("A")
        }
        // Hand (B) and upper-arm (C) are OPTIONAL for the Wrist session. The precise
        // calibration + mount check run on whatever is connected (A is the anchor).
        readonly property QtObject slotB: {
            var _dep  = imuManager.instances
            var _dep2 = appSettings.imuPlacement
            var _dep3 = imuManager.imuDeviceList
            return imuManager.instanceForSlot("B")
        }
        readonly property QtObject slotC: {
            var _dep  = imuManager.instances
            var _dep2 = appSettings.imuPlacement
            var _dep3 = imuManager.imuDeviceList
            return imuManager.instanceForSlot("C")
        }

        // The PERIPHERAL behind slot A — the object every hm_calibration_* call
        // goes to. For a Witmotion it is the same ImuInstance leadImu resolves to;
        // for a HackMotion it is the HmInstance that OWNS leadImu.
        readonly property QtObject leadDevice: {
            var _dep  = imuManager.instances
            var _dep2 = appSettings.imuPlacement
            var _dep3 = imuManager.imuDeviceList
            return imuManager.deviceForSlot("A")
        }

        // Which routine runs. Taken from the device-list entry's `vendor` field —
        // the discriminator the settings panel already uses (ImusPanel.qml:88) and
        // the only one proven in this tree. The HmInstance object also duck-types
        // (it alone carries unitLowerArm / calibrationPhase), but a property-absence
        // probe would be a guess about QML's undefined-property behaviour, and
        // guessing wrong here silently runs OUR routine against a device that has
        // none of its methods.
        readonly property bool isHackMotion: {
            var _dep  = imuManager.instances
            var _dep2 = appSettings.imuPlacement
            var id    = imuManager.deviceIdForSlot("A")
            if (id === "") return false
            var list = imuManager.imuDeviceList
            for (var i = 0; i < list.length; ++i)
                if (list[i].id === id) return list[i].vendor === "hackmotion"
            return false
        }
        function _connectedSegs() {
            // [instance, armDownRef] for every connected segment; A always first.
            var out = []
            if (leadImu && leadImu.imuConnected) out.push([leadImu, _refA])
            if (slotB   && slotB.imuConnected)   out.push([slotB,   _refB])
            if (slotC   && slotC.imuConnected)   out.push([slotC,   _refC])
            return out
        }
        function _curQuat(i) { return i ? Qt.quaternion(i.quatW, i.quatX, i.quatY, i.quatZ) : null }
        // Small Y-rotation bringing the abducted-pose anatomical axis onto Z (= φ).
        function _phiFromAbduction(inst) {
            var q = inst.anatQuat
            var w = Math.min(1, Math.abs(q.scalar))
            var s = Math.sqrt(Math.max(0, 1 - w*w))
            if (s < 1e-4) return 0
            var sg = q.scalar >= 0 ? 1 : -1
            var mx = sg*q.x/s, mz = sg*q.z/s
            var pp = Math.atan2(mx, mz), pm = Math.atan2(-mx, -mz)
            var phi = Math.abs(pp) <= Math.abs(pm) ? pp : pm
            return phi * 180 / Math.PI
        }
        // Per-sensor arm-down reference quaternions (captured at phase-1 completion).
        property var _refA: null
        property var _refB: null
        property var _refC: null
        // Mount validation outcome (set at phase-2 completion).
        property bool   mountFailed: false
        property string mountFailMsg: ""

        // Phase 2: accumulate stable hold duration (target _captureHoldMs).
        property real stableAccumMs: 0.0

        // Stillness-gated capture tuning. Both capture phases (arm-down and
        // abduction) watch the IMU's instantaneous angular velocity and only
        // accumulate samples while the arm is held still; any motion above the
        // threshold resets the hold. The threshold is deliberately forgiving:
        // an arm held out at shoulder height sways/tremors more than a few °/s
        // (an earlier, tighter gate never settled → capture stalled), but stays
        // comfortably below mid-motion (30–100°/s+).
        readonly property real _stillThreshDps: 15.0   // deg/s — held-still ceiling
        readonly property real _captureHoldMs:  2000   // ms of continuous stillness

        // Quaternion samples accumulated during each stillness-held capture window.
        property var _phase1Samples: []
        property var _phase2Samples: []

        // Phase 1 accumulator.
        property real phase1AccumMs: 0.0

        function _quatSlerp(a, b, t) {
            var dot = a.scalar * b.scalar + a.x * b.x + a.y * b.y + a.z * b.z
            if (dot < 0) { b = Qt.quaternion(-b.scalar, -b.x, -b.y, -b.z); dot = -dot }
            if (dot > 0.9995) {
                var r = Qt.quaternion(a.scalar + t * (b.scalar - a.scalar),
                                      a.x     + t * (b.x     - a.x),
                                      a.y     + t * (b.y     - a.y),
                                      a.z     + t * (b.z     - a.z))
                var len = Math.sqrt(r.scalar*r.scalar + r.x*r.x + r.y*r.y + r.z*r.z)
                return Qt.quaternion(r.scalar/len, r.x/len, r.y/len, r.z/len)
            }
            var theta0    = Math.acos(dot)
            var sinTheta0 = Math.sin(theta0)
            var s0 = Math.sin((1 - t) * theta0) / sinTheta0
            var s1 = Math.sin(      t * theta0) / sinTheta0
            return Qt.quaternion(s0 * a.scalar + s1 * b.scalar,
                                 s0 * a.x     + s1 * b.x,
                                 s0 * a.y     + s1 * b.y,
                                 s0 * a.z     + s1 * b.z)
        }

        // Iterative slerp mean: slerp(acc, samples[i], 1/(i+1)) converges to
        // the uniform spherical mean when all samples cluster near each other.
        function _slerpAverage(samples) {
            if (samples.length === 0) return Qt.quaternion(1, 0, 0, 0)
            var acc = samples[0]
            for (var i = 1; i < samples.length; i++)
                acc = _quatSlerp(acc, samples[i], 1.0 / (i + 1))
            return acc
        }

        // Animation targets — driven imperatively by the phase timers below.
        property quaternion _leadArmTarget:  d.leadArmDownQuat
        // ⚠ THE HACKMOTION RAISE LIVES HERE, NOT IN _leadArmTarget. Its pose 0 and
        // pose 1 share one upper-arm rotation — the elbow must not move — so the
        // travel the device watches is a FOREARM rotation. The Witmotion routine
        // leaves this at identity, which is what it was hardcoded to before.
        property quaternion _leadForeArmTarget: Qt.quaternion(1, 0, 0, 0)
        property bool       _animateLeadArm: false
        // Which guide animation is in flight ("introUp"/"introDown"/"raise") —
        // the chain advances on the BodyVizView's leadArmAnimFinished() signal,
        // never on a parallel wall-clock timer (which keeps counting while a
        // stalled renderer shows nothing, running the chain ahead of the user).
        property string _animStage: ""
        // Set when arm-down is captured; prevents the phase-1 timer from
        // re-triggering captureTransitionTimer during the raise animation.
        property bool _armDownCaptured: false
        // True only after phase1MinHoldTimer fires — gives the user a 2s settle
        // window after phase 1 begins before the stillness-gated capture starts.
        property bool _phase1MinHoldDone: false

        // Set when the lead IMU disconnects mid-calibration.
        property bool calibrationFailed: false

        // ═══ HackMotion — the device-native routine ════════════════════════════
        //
        // Library enum values, from libhackmotion's hackmotion/event.h and
        // hackmotion/types.h. QML cannot see the C enums, so the integers are
        // named ONCE here rather than spelled at each comparison.
        // ⚠ QML forbids a property name beginning with a capital, so these cannot
        // carry the library's own spelling; it is in the trailing comment on every
        // line so the mapping stays greppable both ways.
        readonly property int calpIdle:            0   // HM_CALP_IDLE
        readonly property int calpAwaitHorizontal: 1   // HM_CALP_AWAIT_HORIZONTAL
        readonly property int calpMarkingPose0:    2   // HM_CALP_MARKING_POSE0
        readonly property int calpObservingRaise:  3   // HM_CALP_OBSERVING_RAISE
        readonly property int calpMarkingPose1:    4   // HM_CALP_MARKING_POSE1
        readonly property int calpApplying:        5   // HM_CALP_APPLYING
        readonly property int calpVerifying:       6   // HM_CALP_VERIFYING
        readonly property int calpComplete:        7   // HM_CALP_COMPLETE
        readonly property int calpAborted:         8   // HM_CALP_ABORTED

        readonly property int calUnknown:      0   // HM_CAL_UNKNOWN
        readonly property int calUncalibrated: 1   // HM_CAL_UNCALIBRATED
        readonly property int calCalibrated:   2   // HM_CAL_CALIBRATED
        readonly property int calLost:         3   // HM_CAL_LOST — never appears live

        // hm_calibration_abort_reason. Read off the header rather than any summary:
        // it has exactly SIX enumerators, so NO_RESULT — the last one — is 5.
        readonly property int abortNone:         0   // HM_CAL_ABORT_NONE
        readonly property int abortCaller:       1   // HM_CAL_ABORT_CALLER
        readonly property int abortRaiseTooSlow: 2   // HM_CAL_ABORT_RAISE_TOO_SLOW
        readonly property int abortStreamLost:   3   // HM_CAL_ABORT_STREAM_LOST
        readonly property int abortLinkLost:     4   // HM_CAL_ABORT_LINK_LOST
        readonly property int abortNoResult:     5   // HM_CAL_ABORT_NO_RESULT

        // hm_status, the few a calibration call can be refused with.
        readonly property int errInvalidState:  -2   // HM_ERR_INVALID_STATE
        readonly property int errLinkDown:     -12   // HM_ERR_LINK_DOWN
        readonly property int errNoStream:     -14   // HM_ERR_NO_STREAM
        readonly property int errBusy:         -18   // HM_ERR_BUSY

        // Our step in the choreography. The DEVICE's phase is the authority on
        // where the library is; this only distinguishes the sub-steps a phase
        // cannot — before begin, and the reference-pose wait against the
        // confirmation readout, both of which sit inside VERIFYING/COMPLETE.
        //   0 ready   1 pose 0 (horizontal)   2 raise   3 applying
        //   4 reference pose   5 confirmed    9 stopped, needs an explicit re-run
        property int  hmStep: 0
        // True between confirmReferencePose() and the presence values landing. The
        // call RETURNS BEFORE THE MEASUREMENT EXISTS, so this is the wait on the
        // measurement, not on the call.
        property bool hmAwaitingPresence: false
        property string hmFailMsg:  ""
        property string hmFailKind: "error"    // "error" | "warn"
        // The calibration was destroyed under us (link drop). Distinct from a
        // failed attempt: there is nothing to resume and nothing was stored.
        property bool hmInvalidated: false

        // The library's phase, mirrored as a declarative dependency so the
        // transitions are edge-triggered by the device rather than by our timers.
        readonly property int hmPhase: (isHackMotion && leadDevice !== null)
                                       ? leadDevice.calibrationPhase : calpIdle
        onHmPhaseChanged: _hmOnPhase(hmPhase)

        // Slot A changed KIND under us — a device was reassigned. Neither routine's
        // state means anything for the other, and a HackMotion "done" must NOT
        // survive into the Witmotion flow (or the reverse), so drop back to
        // uncalibrated and let the host start whichever routine now applies.
        onIsHackMotionChanged: {
            _hmResetState()
            calibPhase        = 0
            calibrationDone   = false
            calibrationFailed = false
        }

        // ── Pacing ────────────────────────────────────────────────────────────
        readonly property int _hmSettleMs:      2000   // pose-0 settle before `a2 00`
        // ⚠ THE GUIDE ANIMATION IS FUNCTIONAL, NOT DECORATIVE: the device watches
        // the raise CONTINUOUSLY from the instant it enters OBSERVING_RAISE, so the
        // animation paces the athlete and the elapsed time between the two markers
        // becomes OURS to control rather than the athlete's. 3000 ms + a 500 ms
        // settle lands the second marker at ~3.5 s, comfortably inside the
        // library's 6 s calibration_raise_limit_us default.
        //
        // ⚠ DO NOT RAISE THAT LIMIT AND DO NOT ADD A WALL-CLOCK FALLBACK that
        // confirms the raise anyway. If a stalled renderer overruns it the library
        // aborts with HM_CAL_ABORT_RAISE_TOO_SLOW, and a legible failure the coach
        // can repeat is worth more than a marker fired at an arm that never moved —
        // which is precisely the attempt that scores BEST on the presence check
        // (0.70°, §8.2). The device imposes no deadline of its own: one measured
        // attempt took 15.6 s and was still applied.
        readonly property int _hmRaiseAnimMs:   3000
        readonly property int _hmRaiseSettleMs:  500
        readonly property int _hmReturnAnimMs:  1500   // paced return to pose 0
        readonly property int _hmRefSettleMs:   1500   // stillness settle before the check
        // Bound on the wait for the presence measurement. The library averages up
        // to 64 live samples, and a resting wrist streams at ~25 Hz → ~2.6 s worst
        // case, so this is a ceiling with margin, not a policy.
        readonly property int _hmPresenceWaitMs: 6000

        // ── Did the raise actually happen? ────────────────────────────────────
        // ⚠ THE DEVICE WILL REPORT A CALIBRATION FOR AN ATTEMPT WHERE NOTHING MOVED,
        // AND THAT IS NOT A BUG WE CAN FIX IN THE LIBRARY. §8.2 measured the presence
        // check at 1.96° for the correct routine, 6.10° for a raise about the wrong
        // axis, and 0.70° — the BEST score of the three — for pose 1 marked without
        // moving at all. The check tests the ZEROING, which cannot fail; the raise is
        // what determines the anatomical axis, and the presence angle is blind to it.
        // So hm_session_calibration_state() reaches HM_CAL_CALIBRATED for a routine
        // the athlete never performed, and a UI that pings on that alone tells a coach
        // their sensor is calibrated when its frame is undetermined.
        //
        // The one thing that CAN see it is the stream we are already receiving. The
        // lower-arm unit sits on the forearm, which is the segment this routine
        // rotates, so the angular travel of its own reported orientation between the
        // two markers IS the raise. That is an independent measurement, not a
        // reinterpretation of the presence angle — and §8.2 names exactly this gap
        // ("the payload carries the one thing the presence check is blind to —
        // whether the raise happened at all, and about which axis").
        //
        // ⚠ IT GATES BEFORE `a2 01`, NOT AFTER. Below the threshold we ABORT instead
        // of marking pose 1: before 0x94 nothing has been applied, so the device is
        // left alone rather than given a transform we know is undetermined.
        property var  _hmRaiseStartQuat: null
        property real _hmRaiseTravelDeg: Number.NaN
        // §8.2's separable ~30° against its unseparable 4.2°/7.6° attempts. Sits
        // between the two populations with margin either way.
        readonly property real _hmMinRaiseTravelDeg: 15.0

        function _hmUnitQuat() {
            var u = leadImu
            if (!u) return null
            var q = Qt.quaternion(u.quatW, u.quatX, u.quatY, u.quatZ)
            // A unit that has not delivered a sample yet reads as identity; treat
            // that as "cannot measure" rather than as a real orientation.
            if (q.scalar === 1 && q.x === 0 && q.y === 0 && q.z === 0) return null
            return q
        }

        // Angle between two orientations, degrees. ⚠ Convention-blind by design —
        // this asks only HOW FAR, never about direction or axis, which is all the
        // "did it move" question needs.
        function _hmQuatAngleDeg(a, b) {
            var dot = Math.abs(a.scalar*b.scalar + a.x*b.x + a.y*b.y + a.z*b.z)
            return 2 * Math.acos(Math.min(1, dot)) * 180 / Math.PI
        }

        function _hmResetState() {
            // ⚠ hmStartTimer is NOT stopped here: its `running` is a binding on
            // hmStep, and the file's idiom is to leave binding-driven timers to
            // their bindings (introStartTimer and stabilityHoldTimer are omitted
            // from _reset() for the same reason).
            hmHorizontalSettleTimer.stop()
            hmRaiseConfirmTimer.stop()
            hmRefSettleTimer.stop()
            hmPresenceWaitTimer.stop()
            hmStep             = 0
            hmAwaitingPresence = false
            hmFailMsg          = ""
            hmFailKind         = "error"
            hmInvalidated      = false
            _hmRaiseStartQuat  = null
            _hmRaiseTravelDeg  = Number.NaN
        }

        // Cancel/Recalibrate mid-routine. ⚠ abortCalibration() ALWAYS works — it is
        // a local state reset and writes nothing. But at VERIFYING the device's
        // transform is ALREADY APPLIED and no command reverses it, so aborting
        // there DECLINES THE PRESENCE CHECK; it does not undo a calibration, and
        // the wording must not claim it did.
        function _hmAbortIfActive() {
            var dev = leadDevice
            if (!isHackMotion || !dev || !dev.calibrationActive) return
            var atVerifying = (dev.calibrationPhase === calpVerifying)
            dev.abortCalibration()
            if (atVerifying)
                _hmStop("warn", qsTr("Presence check declined. The sensor applied its transform "
                                     + "already and nothing reverses that — the calibration was NOT "
                                     + "undone, it is simply unverified. Re-run to check it."))
            else
                _hmStop("warn", qsTr("Calibration cancelled before the sensor applied anything."))
        }

        // Terminal state: the routine stopped and only an explicit Recalibrate
        // restarts it. calibrationFailed is reused so the shared StatusBadge and
        // the host gates read correctly without a second flag.
        function _hmStop(kind, msg) {
            // hmStartTimer is left to its binding (see _hmResetState) — hmStep 9
            // holds it off.
            hmHorizontalSettleTimer.stop()
            hmRaiseConfirmTimer.stop()
            hmRefSettleTimer.stop()
            hmPresenceWaitTimer.stop()
            hmStep             = 9
            hmAwaitingPresence = false
            hmFailKind         = kind
            hmFailMsg          = msg
            _animateLeadArm    = false
            _animStage         = ""
            calibrationFailed  = true
            calibrationDone    = false
        }

        // ── Step 1 — preconditions, then `hm_calibration_begin()` ─────────────
        // ⚠ hm_calibration_begin() returns HM_ERR_NO_STREAM when no stream is
        // running, and there is DELIBERATELY no AWAIT_STREAM phase in the library:
        // the device observes a continuous raise, which two static samples cannot
        // supply. Under our one-stream cycle the stream comes up just after connect
        // and stays open, so this is normally satisfied — but say plainly what is
        // missing when it is not. Do NOT queue a begin for later and do NOT retry.
        function _hmBegin() {
            var dev = leadDevice
            if (!isHackMotion || !dev) return
            if (!dev.imuConnected) {
                _hmStop("error", qsTr("The wrist sensor is not connected. Connect it on the IMUs "
                                      + "step, then tap Recalibrate."))
                return
            }
            if (!dev.streaming) {
                _hmStop("error", qsTr("The wrist sensor is connected but not streaming, and the "
                                      + "sensor has to WATCH the raise — it cannot calibrate from "
                                      + "two still poses. Wait for the stream, then tap Recalibrate."))
                return
            }
            hmFailMsg     = ""
            hmInvalidated = false
            hmStep        = 1
            // Pose the guide at pose 0 before the first marker. No animation: this
            // is the starting position, not a motion to follow.
            if (flow._bvv) {
                flow._bvv.resetArmAnimation(flow._bvv.hmCalUpperArmQuat,
                                            flow._bvv.hmCalForeArmPose0Quat)
                _animateLeadArm    = false
                _animStage         = ""
                _leadArmTarget     = flow._bvv.hmCalUpperArmQuat
                _leadForeArmTarget = flow._bvv.hmCalForeArmPose0Quat
            }
            // ⚠ Queued onto the I/O thread and returns NOTHING. A refusal arrives
            // only as calibrationCallRefused; the settle window that leads to
            // `a2 00` is started by the AWAIT_HORIZONTAL phase, not by this call.
            dev.beginCalibration()
        }

        // ── The library's phase transitions ───────────────────────────────────
        function _hmOnPhase(p) {
            var dev = leadDevice
            if (!isHackMotion || !dev) return

            if (p === calpAwaitHorizontal) {
                // The library is ready for `a2 00`. Give the athlete a settle
                // window and confirm from the timer — never from _hmBegin(), which
                // returns before the library has moved.
                hmStep = 1
                hmHorizontalSettleTimer.restart()

            } else if (p === calpObservingRaise) {
                // ⚠ THE DEVICE IS WATCHING FROM THIS INSTANT. OBSERVING_RAISE is
                // the signal to start the raise — not a timer of ours — so the
                // guide animation starts here and nowhere else.
                hmStep = 2
                // The device is watching from now, so this is the instant to anchor
                // the travel measurement against.
                _hmRaiseStartQuat = _hmUnitQuat()
                _hmRaiseTravelDeg = Number.NaN
                if (flow._bvv) {
                    // ⚠ The UPPER ARM DOES NOT MOVE — the elbow stays put and the
                    // forearm elevates. Anchor both segments, then change only the
                    // forearm target; BodyVizView's forearm onChanged handler is
                    // what starts the slerp, because the arm target is unchanged.
                    flow._bvv.resetArmAnimation(flow._bvv.hmCalUpperArmQuat,
                                                flow._bvv.hmCalForeArmPose0Quat)
                    flow._bvv.leadArmAnimDuration = _hmRaiseAnimMs
                    _animStage         = "hmRaise"
                    _animateLeadArm    = true
                    _leadForeArmTarget = flow._bvv.hmCalForeArmPose1Quat
                }

            } else if (p === calpApplying) {
                hmStep          = 3
                _animateLeadArm = false
                _animStage      = ""

            } else if (p === calpVerifying) {
                // ⚠ VERIFYING MEANS THE TRANSFORM IS ALREADY APPLIED AND THE
                // PRESENCE CHECK IS NOT YET MEASURED. It is not success: no tick,
                // no Continue, nothing written. It is the cue for the reference
                // pose, which is NOT OPTIONAL — skipping it leaves the recording at
                // HM_CAL_UNKNOWN, and it also yields the anchor Phase D needs.
                hmStep = 4
                if (flow._bvv) {
                    flow._bvv.resetArmAnimation(flow._bvv.hmCalUpperArmQuat,
                                                flow._bvv.hmCalForeArmPose1Quat)
                    flow._bvv.leadArmAnimDuration = _hmReturnAnimMs
                    _animStage         = "hmReturn"
                    _animateLeadArm    = true
                    _leadForeArmTarget = flow._bvv.hmCalForeArmPose0Quat
                } else {
                    hmRefSettleTimer.restart()
                }

            } else if (p === calpComplete) {
                // ⚠ COMPLETE is not calibrated, and HM_CAL_ABORT_CALLER is carried
                // on a transition to COMPLETE as well as to ABORTED — so
                // "abort_reason != NONE" is NOT "the routine failed". The verdict
                // is computed in _hmEvaluate() from the STATE.
                _hmEvaluate()

            } else if (p === calpAborted) {
                // RAISE_TOO_SLOW is "that took too long — try again", not a fault in
                // the sensor, so it reads as a warning and the others as errors.
                _hmStop(dev.calibrationAbortReason === abortRaiseTooSlow ? "warn" : "error",
                        _hmAbortText(dev.calibrationAbortReason))
            }
        }

        // ── The verdict ───────────────────────────────────────────────────────
        // Called on every calibration state change and on COMPLETE, and idempotent
        // by construction. ⚠ calibrationDone comes from calibrationState ===
        // HM_CAL_CALIBRATED and from nothing else: never from phase === COMPLETE
        // (the device applies its transform for every attempt, including rejected
        // ones), and never from the presence angle being small (it INVERTS — the
        // attempt with no axis information scored best).
        function _hmEvaluate() {
            var dev = leadDevice
            if (!isHackMotion || !dev) return
            if (calibrationDone || hmStep === 9) return
            // ⚠ A VERDICT IS ONLY READ FOR A ROUTINE WE HAVE DRIVEN PAST THE RAISE.
            // The device's phase, state and abort reason persist from the PREVIOUS
            // attempt until the library moves them, and a state change does arrive
            // while a fresh routine is still being begun — so evaluating at step
            // 0/1/2 would read last attempt's COMPLETE as this one's outcome and
            // kill a run that is going fine.
            if (hmStep < 3) return

            // ⚠ CALIBRATED is reachable only through a presence measurement, which
            // only happens after confirmReferencePose() — so requiring step 4 here
            // costs nothing and removes the last way a stale state could be read as
            // this attempt's success.
            if (hmStep >= 4 && dev.calibrationState === calCalibrated) {
                hmPresenceWaitTimer.stop()
                hmAwaitingPresence = false
                hmStep             = 5
                calibrationFailed  = false
                hmFailMsg          = ""
                calibrationDone    = true
                return
            }

            // Not calibrated. Anything before COMPLETE is still in flight.
            if (dev.calibrationPhase !== calpComplete) return

            if (hmStep < 4) {
                // Defensive: the routine finished without VERIFYING ever being
                // seen, so the reference pose was never offered. The library will
                // refuse a late confirm, so the honest outcome is a re-run.
                _hmStop("error", qsTr("The sensor finished before the reference pose could be "
                                      + "taken, so nothing checked the calibration. Tap "
                                      + "Recalibrate."))
                return
            }

            if (dev.calibrationAbortReason === abortCaller) {
                _hmStop("warn", qsTr("Presence check declined. The sensor's transform is applied "
                                     + "and nothing reverses that — it is unverified, not undone. "
                                     + "Re-run to check it."))
                return
            }
            // ⚠ THE MEASUREMENT NEVER HAPPENED. The library collected too few live
            // samples at the reference pose to average one (HM_WARN_PRESENCE_NOT_
            // MEASURED), and the phase reaches COMPLETE either way — so without this
            // flag a check that never ran would read as a success. Its own outcome,
            // distinct from "measured and passed" and from "declined".
            // ⚠ It also decides what presenceSamplesUsed MEANS on this path
            // (collected, not used), so the count is never shown without it.
            if (dev.presenceNotMeasured) {
                _hmStop("error", qsTr("Too few readings arrived at the reference pose to check the "
                                      + "calibration, so it is NOT confirmed — the sensor applied "
                                      + "its transform, but nothing verified it. Hold the first "
                                      + "position still and tap Recalibrate."))
                return
            }
            // presenceSamplesUsed > 0 proves the measurement LANDED (the presence
            // event may arrive after the phase event, so a bare state read here
            // would flash a false failure). With a measurement in hand and the
            // state still not CALIBRATED, the check did not pass.
            if (dev.presenceSamplesUsed > 0)
                _hmStop("error", qsTr("The check at the reference pose did not confirm a "
                                      + "calibration on the sensor. Re-seat nothing — just hold "
                                      + "the first position still and tap Recalibrate."))
            // Otherwise keep waiting; hmPresenceWaitTimer bounds it.
        }

        // The calibration is GONE. A plain BLE disconnect destroys it and the
        // library forces UNCALIBRATED on link-down, so this drives the flow back to
        // uncalibrated and asks for a re-run. ⚠ Nothing is resumed and nothing was
        // stored — there is deliberately no persistence to restore from.
        function _hmInvalidated() {
            if (!isHackMotion) return
            hmInvalidated = true
            _hmStop("error", qsTr("The sensor's calibration is gone — a dropped link destroys it "
                                  + "(measured 0.70° → 18.80° at the same pose with the strap "
                                  + "untouched). It cannot be resumed or restored. Re-run the "
                                  + "routine once the sensor is back."))
        }

        // ⚠ Every hm_calibration_* call is queued onto the I/O thread and returns
        // nothing; a refusal arrives ONLY here. NO_STREAM and BUSY must not collapse
        // into one generic error — they ask the coach for different things.
        function _hmRefused(status, call) {
            if (!isHackMotion) return
            if (status === errNoStream)
                _hmStop("error", qsTr("The sensor stopped streaming, so it cannot watch the raise "
                                      + "(%1 was refused). Tap Recalibrate once data is flowing.")
                                     .arg(call))
            else if (status === errBusy)
                _hmStop("warn", qsTr("The sensor is busy retrieving swing data (%1 was refused) — "
                                     + "try again in a moment.").arg(call))
            else if (status === errLinkDown)
                _hmStop("error", qsTr("The sensor's link went down (%1 was refused).").arg(call))
            else if (status === errInvalidState)
                _hmStop("error", qsTr("The sensor was not in a state to accept %1. Tap Recalibrate "
                                      + "to start the routine from the beginning.").arg(call))
            else
                _hmStop("error", qsTr("The sensor refused %1 (status %2).").arg(call).arg(status))
        }

        function _hmAbortText(reason) {
            if (reason === abortRaiseTooSlow)
                // Not a sensor fault, and not the athlete's either — the guide sets
                // the pace, so this reads as "repeat it", never as an error.
                return qsTr("That took too long between the two positions — the sensor needs one "
                            + "continuous raise. Tap Recalibrate and follow the guide.")
            if (reason === abortStreamLost)
                return qsTr("The sensor's data stream stopped part-way through, so the raise could "
                            + "not be watched. Tap Recalibrate once data is flowing.")
            if (reason === abortLinkLost)
                return qsTr("The sensor's link dropped part-way through. Reconnect it, then run the "
                            + "routine again — a dropped link destroys any calibration.")
            if (reason === abortNoResult)
                return qsTr("The sensor did not answer in time, so no calibration was applied. Tap "
                            + "Recalibrate.")
            if (reason === abortCaller)
                return qsTr("Calibration cancelled.")
            return qsTr("The calibration routine stopped before it finished. Tap Recalibrate.")
        }

        // Presentation helpers — every one of these is STATE, never a score.
        function _hmDeg(v) { return (v === undefined || isNaN(v)) ? "—" : v.toFixed(2) + "°" }
        function _hmStateText(s) {
            if (s === calCalibrated)   return qsTr("CALIBRATED")
            if (s === calUncalibrated) return qsTr("UNCALIBRATED")
            if (s === calLost)         return qsTr("LOST")
            return qsTr("NOT CHECKED")
        }

        function _reset() {
            if (isHackMotion) {
                // ⚠ Nothing to clear host-side — the device holds the calibration
                // and clearCalibration()/setNominalCalibration() do not exist on
                // it. An in-flight routine is aborted so a Recalibrate always
                // starts from a known state.
                _hmAbortIfActive()
                _hmResetState()
            } else if (leadImu) {
                leadImu.clearCalibration()
            }

            introReadyTimer.stop()
            phase1MinHoldTimer.stop()
            phase1HoldTimer.stop()
            captureTransitionTimer.stop()
            raiseReadyTimer.stop()
            _animStage         = ""
            calibPhase         = 0
            phase1AccumMs      = 0
            stableAccumMs      = 0
            _phase1Samples     = []
            _phase2Samples     = []
            phaseProgress      = 0.0
            _animateLeadArm    = false
            _leadArmTarget     = leadArmDownQuat
            _leadForeArmTarget = Qt.quaternion(1, 0, 0, 0)
            calibrationFailed  = false
            _armDownCaptured   = false
            _phase1MinHoldDone = false
            mountFailed        = false
            mountFailMsg       = ""
            _refA              = null
            _refB              = null
            _refC              = null
            calibArmDownQuat   = null
            calibArmTPoseQuat  = null
            calibrationDone    = false
            calibArmDownEuler  = null
            calibArmTPoseEuler = null
        }
    }

    // ⚠ Witmotion only, and the target is nulled rather than merely disabled for a
    // HackMotion: slot A resolves to an HmUnit there, which has no imuConnected at
    // all (connection state lives on the PERIPHERAL), and Connections warns when it
    // cannot find a signal on its target. The HackMotion equivalent is below, on
    // d.leadDevice.
    Connections {
        target:  d.isHackMotion ? null : d.leadImu
        enabled: d.calibPhase > 0 && !d.calibrationDone
        function onImuConnectedChanged() {
            var imu = d.leadImu
            if (imu && !imu.imuConnected)
                d.calibrationFailed = true
        }
    }

    // ── HackMotion — every path the library can hand us ────────────────────────
    // All four are reachable and each reads differently. ⚠ There is no return value
    // to check anywhere: the invokables are queued onto the I/O thread and return
    // nothing, so a refusal exists ONLY as calibrationCallRefused.
    Connections {
        target: d.isHackMotion ? d.leadDevice : null

        function onCalibrationCallRefused(status, call) { d._hmRefused(status, call) }

        // The calibration is gone — drive back to uncalibrated and ask for a re-run.
        function onCalibrationInvalidated() { d._hmInvalidated() }

        // Phase, state, presence angle, spread and sample count all notify through
        // this one signal. The verdict is recomputed rather than latched, because
        // the presence event and the phase event are separate library events and
        // may land in either order.
        function onCalibrationStateChanged() { d._hmEvaluate() }

        // A plain disconnect DESTROYS the calibration (§8.3). The library forces
        // UNCALIBRATED and emits calibrationInvalidated for the same event; this
        // path exists so the flow still regresses if the transport signal is the
        // only one that reaches us.
        function onImuConnectedChanged() {
            var dev = d.leadDevice
            if (dev && !dev.imuConnected && (d.hmStep > 0 || d.calibrationDone))
                d._hmInvalidated()
        }
    }

    // Phase 2 hold timer.
    Timer {
        id: stabilityHoldTimer
        interval: 100
        repeat:   true
        running:  flow.visible && flow._autoStartGate
                  && d.calibPhase === 2
                  && !d.calibrationDone
                  && !d.mountFailed
                  && d.leadImu !== null
        onTriggered: {
            var imu = d.leadImu
            if (!imu) return
            // Stillness-gated: only accumulate while the arm is held still;
            // motion resets the hold so the captured pose is genuinely static.
            if (imu.angularVelocityDps > d._stillThreshDps) {
                d._phase2Samples = []
                d.stableAccumMs  = 0
                d.phaseProgress  = 0.0
                return
            }
            d._phase2Samples = d._phase2Samples.concat(
                [Qt.quaternion(imu.quatW, imu.quatX, imu.quatY, imu.quatZ)])
            d.stableAccumMs += interval
            d.phaseProgress = Math.min(d.stableAccumMs / d._captureHoldMs, 1.0)
            if (d.stableAccumMs >= d._captureHoldMs) {
                d.calibArmTPoseQuat = d._slerpAverage(d._phase2Samples)

                // Abduction refinement + mount validation for EVERY connected segment.
                // Each sensor: refine its mounting about the long axis by φ (from the
                // abducted-pose anatomical orientation), then evaluate the two-part gate
                //   gravity check (gravΔ ≤ 25°, catches flip/upside-down) AND
                //   long-axis deviation (φ/strapΔ ≤ 15°, catches strap rotation).
                // PASS requires ALL connected segments to pass; any FAIL → re-seat.
                var segs = d._connectedSegs()
                var allPass = segs.length > 0
                var failNames = []
                var nameFor = function(inst) {
                    return inst === d.leadImu ? qsTr("forearm")
                         : inst === d.slotB    ? qsTr("hand")
                         : qsTr("upper arm")
                }
                for (var k = 0; k < segs.length; ++k) {
                    var s2 = segs[k][0], ref = segs[k][1]
                    if (!ref) continue
                    s2.refineMountAboutLongAxis(ref, d._phiFromAbduction(s2), false)
                    var ok = s2.mountDeviationDeg <= 15.0 && s2.mountGravityErrorDeg <= 25.0
                    if (!ok) { allPass = false; failNames.push(nameFor(s2)) }
                }

                if (allPass) {
                    d.mountFailed = false
                    d.mountFailMsg = ""
                    d.calibrationDone = true
                } else {
                    d.mountFailed = true
                    d.mountFailMsg = qsTr("Sensor mounted incorrectly (%1) — re-seat per the strap guide and tap Recalibrate.")
                        .arg(failNames.join(", "))
                    // Leave calibrationDone false; user must Recalibrate.
                }
            }
        }
    }

    // Phase 0: 3s after the flow becomes active AND the body model has fully
    // loaded, play the 3s rest→T-pose guide animation. The fullyLoaded gate
    // matters on Windows: the 19 GLB segments + first-draw shader compilation
    // can stall rendering for seconds, and a chain started against a stalled
    // renderer plays to nobody. Guard on visible+gate: this flow lives in a
    // StackLayout/Popup so it is instantiated up front — without the guard the
    // intro fires immediately and the whole capture chain runs in the
    // background.
    // ⚠ THE ONLY ENTRY POINT INTO THE WITMOTION CHAIN, hence the only place the
    // device branch has to be applied: with this gated off, calibPhase never leaves
    // 0 and neither phase-1 nor phase-2 timers can run.
    Timer {
        id: introStartTimer
        interval: 3000
        repeat:   false
        running:  !d.isHackMotion
                  && d.calibPhase === 0 && flow.visible && flow._autoStartGate
                  && flow._bvv !== null && flow._bvv.fullyLoaded
        onTriggered: {
            flow._bvv.resetArmAnimation(d.leadArmDownQuat)
            flow._bvv.leadArmAnimDuration = 3000
            d._animStage      = "introUp"
            d._animateLeadArm = true
            d._leadArmTarget  = d.tPoseQuat
        }
    }

    // Guide-animation completion chain: each stage advances when the guide has
    // actually FINISHED drawing (BodyVizView signals the slerp's end), so a
    // stalled renderer delays the chain instead of being outrun by it.
    Connections {
        target:  flow._bvv
        enabled: flow._autoStartGate
        function onLeadArmAnimFinished() {
            if (d._animStage === "introUp") {
                // Lower the arm back to rest at the same speed. _leadArmFrom is
                // reset to tPoseQuat so the return starts from the top.
                d._animStage = "introDown"
                flow._bvv.resetArmAnimation(d.tPoseQuat)
                flow._bvv.leadArmAnimDuration = 3000
                d._leadArmTarget = d.leadArmDownQuat
            } else if (d._animStage === "introDown") {
                d._animStage = ""
                introReadyTimer.start()   // 2s settle pause, then phase 1
            } else if (d._animStage === "raise") {
                d._animStage = ""
                raiseReadyTimer.start()   // 2s settle pause, then phase 2
            } else if (d._animStage === "hmRaise") {
                // ⚠ THE CHAIN ADVANCES HERE AND NOWHERE ELSE — never on a parallel
                // wall-clock timer, which keeps counting while a stalled renderer
                // shows nothing and would run the chain ahead of the athlete. The
                // stakes are higher on this branch than on the Witmotion one: the
                // DEVICE is measuring the very motion the guide is pacing, and a
                // marker fired at an arm that never moved is the attempt that scores
                // BEST on the presence check (0.70°, §8.2) while carrying no axis
                // information at all.
                d._animStage = ""
                hmRaiseConfirmTimer.start()   // short settle, then `a2 01`
            } else if (d._animStage === "hmReturn") {
                d._animStage      = ""
                d._animateLeadArm = false
                hmRefSettleTimer.start()      // stillness settle, then the check
            }
        }
    }

    // Post-intro settle pause complete: start phase 1.
    Timer {
        id: introReadyTimer
        interval: 2000
        repeat:   false
        onTriggered: {
            d._animateLeadArm    = false
            d._phase1MinHoldDone = false
            d.calibPhase         = 1
            phase1MinHoldTimer.start()
            // No hardware zeroing — orientation comes from our own Madgwick
            // fusion (device angle-zeroing is vestigial); the arm-down pose is
            // captured directly by phase1HoldTimer once the IMU is stable.
        }
    }

    Timer {
        id: phase1MinHoldTimer
        interval: 2000
        repeat:   false
        onTriggered: d._phase1MinHoldDone = true
        // No capture here — phase1HoldTimer handles it reactively.
    }

    // Phase 1 accumulator — same stillness-gated pattern as stabilityHoldTimer
    // (phase 2). The timer runs for the whole phase; each tick decides whether
    // to accumulate (arm held still) or reset the hold (arm moving), watching
    // imu.angularVelocityDps. _armDownCaptured gates it off once captured so it
    // can't re-trigger during the raise animation. _phase1MinHoldDone (set 2s
    // after phase 1 begins via phase1MinHoldTimer) gives the user a settle
    // window before the still-watch starts.
    Timer {
        id: phase1HoldTimer
        interval: 100
        repeat:   true
        running:  flow.visible && flow._autoStartGate
                  && d.calibPhase === 1
                  && d._phase1MinHoldDone
                  && !d._armDownCaptured
                  && d.leadImu !== null
        onTriggered: {
            var imu = d.leadImu
            if (!imu) return
            // Stillness-gated: only accumulate while the arm is held still;
            // motion resets the hold so the captured pose is genuinely static.
            if (imu.angularVelocityDps > d._stillThreshDps) {
                d._phase1Samples = []
                d.phase1AccumMs  = 0
                return
            }
            d._phase1Samples = d._phase1Samples.concat(
                [Qt.quaternion(imu.quatW, imu.quatX, imu.quatY, imu.quatZ)])
            d.phase1AccumMs += interval
            if (d.phase1AccumMs >= d._captureHoldMs) {
                d.calibArmDownQuat = d._slerpAverage(d._phase1Samples)
                // Quick-calibrate EVERY connected segment at arm-down: sets A so
                // anatQuat=identity here, with the fixed nominal mounting M, and runs
                // the gravity (flip) check. Each sensor's arm-down reference is stored
                // for the phase-2 abduction refinement. The hand, forearm and upper-arm
                // sensors are mounted COPLANAR (same strap orientation), so all three share
                // the arm nominal mount → handMount=false for all. (nominalHandMount() is a
                // non-coplanar dorsal placement we do not use.)
                d._refA = d._curQuat(d.leadImu)
                d._refB = d._curQuat(d.slotB)
                d._refC = d._curQuat(d.slotC)
                if (d.leadImu && d.leadImu.imuConnected)
                    d.leadImu.setNominalCalibration(d._refA, false)
                if (d.slotB && d.slotB.imuConnected)
                    d.slotB.setNominalCalibration(d._refB, false)
                if (d.slotC && d.slotC.imuConnected)
                    d.slotC.setNominalCalibration(d._refC, false)
                d._armDownCaptured = true  // stops timer via binding; must be after quat capture
                captureTransitionTimer.start()
            }
        }
    }

    // Phase 1 → raise: brief pause after arm-down captured, then play the
    // raise guide animation; phase 2 starts via the completion chain above.
    Timer {
        id: captureTransitionTimer
        interval: 800
        repeat:   false
        onTriggered: {
            flow._bvv.resetArmAnimation(d.leadArmDownQuat)
            flow._bvv.leadArmAnimDuration = 1500
            d._animStage      = "raise"
            d._animateLeadArm = true
            d._leadArmTarget  = d.tPoseQuat
        }
    }

    // Raise animation complete + settle pause, then start T-pose capture.
    Timer {
        id: raiseReadyTimer
        interval: 2000
        repeat:   false
        onTriggered: {
            d._animateLeadArm = false
            d.calibPhase      = 2
        }
    }

    // ══ HackMotion timers ══════════════════════════════════════════════════════
    // The chain is: this timer → beginCalibration() → [library: AWAIT_HORIZONTAL]
    // → settle → confirmHorizontal() → [library: OBSERVING_RAISE] → guide raise →
    // leadArmAnimFinished → settle → confirmRaise() → [library: APPLYING →
    // VERIFYING] → guide back to pose 0 → settle → confirmReferencePose() → wait on
    // the MEASUREMENT. Every arrow into the library is a queued call with no return
    // value, and every arrow out of it is a phase or state change.

    // Same fullyLoaded gate as the Witmotion intro: a chain started against a
    // stalled renderer plays to nobody, and here that renderer is pacing a motion
    // the device is timing. The short interval is a read window for the first
    // instruction, not a settle — the settle happens after the library is ready.
    Timer {
        id: hmStartTimer
        interval: 1500
        repeat:   false
        running:  d.isHackMotion
                  && d.hmStep === 0 && flow.visible && flow._autoStartGate
                  && flow._bvv !== null && flow._bvv.fullyLoaded
        onTriggered: d._hmBegin()
    }

    // Pose-0 settle, then `a2 00`. Started by the AWAIT_HORIZONTAL phase.
    Timer {
        id: hmHorizontalSettleTimer
        interval: d._hmSettleMs
        repeat:   false
        onTriggered: {
            var dev = d.leadDevice
            if (dev && d.hmStep === 1) dev.confirmHorizontal()
        }
    }

    // Raise complete (the GUIDE has finished drawing it) + a short settle, then
    // `a2 01`. ⚠ No wall-clock fallback: if this never fires because the renderer
    // stalled, the library aborts with RAISE_TOO_SLOW and the coach repeats the
    // routine, which is worth more than a marker fired at a motionless arm.
    Timer {
        id: hmRaiseConfirmTimer
        interval: d._hmRaiseSettleMs
        repeat:   false
        onTriggered: {
            var dev = d.leadDevice
            if (!dev || d.hmStep !== 2) return

            // ⚠ MEASURE THE TRAVEL BEFORE MARKING POSE 1 — see _hmRaiseTravelDeg.
            var now = d._hmUnitQuat()
            if (d._hmRaiseStartQuat !== null && now !== null) {
                d._hmRaiseTravelDeg = d._hmQuatAngleDeg(d._hmRaiseStartQuat, now)
                if (d._hmRaiseTravelDeg < d._hmMinRaiseTravelDeg) {
                    // Nothing has been applied yet, so abort rather than hand the
                    // device a transform whose axis we know is undetermined.
                    dev.abortCalibration()
                    d._hmStop("warn", qsTr("Your forearm only moved %1° — the sensor needs about "
                                           + "%2° to work out which way your wrist bends, and it "
                                           + "cannot tell on its own that the movement was missing. "
                                           + "Nothing was applied. Follow the guide and tap "
                                           + "Recalibrate.")
                                          .arg(Math.round(d._hmRaiseTravelDeg))
                                          .arg(Math.round(flow._bvv ? flow._bvv.hmCalRaiseDeg : 30)))
                    return
                }
            }
            // Could not measure (no live unit, or no sample yet): proceed and let the
            // presence check speak. ⚠ Deliberately permissive — a measurement we
            // failed to take must not make the device unusable.
            dev.confirmRaise()
        }
    }

    // Back at the reference pose and still, so the presence check may be measured.
    // ⚠ confirmReferencePose() RETURNS BEFORE THE MEASUREMENT EXISTS — HM_OK only
    // means the run started — so what follows is a wait on the presence values, not
    // on the call, bounded by hmPresenceWaitTimer.
    Timer {
        id: hmRefSettleTimer
        interval: d._hmRefSettleMs
        repeat:   false
        onTriggered: {
            var dev = d.leadDevice
            if (!dev || d.hmStep !== 4) return
            d.hmAwaitingPresence = true
            dev.confirmReferencePose()
            hmPresenceWaitTimer.restart()
        }
    }

    // The measurement never landed. Distinct from the library's own
    // presenceNotMeasured warning (which DID reach a verdict); this is the case
    // where nothing at all came back.
    Timer {
        id: hmPresenceWaitTimer
        interval: d._hmPresenceWaitMs
        repeat:   false
        onTriggered: {
            d._hmEvaluate()      // it may have landed in the same instant
            if (!d.calibrationDone && d.hmStep === 4)
                d._hmStop("error", qsTr("The sensor never reported a check at the reference pose, "
                                        + "so the calibration is NOT confirmed. Hold the first "
                                        + "position still and tap Recalibrate."))
        }
    }

    // ── Layout: full (wizard) vs compact (toolbar panel) ───────────────────────
    Loader {
        anchors.fill: parent
        sourceComponent: flow.layoutMode === "compact" ? compactLayout : fullLayout
    }

    // FULL — RowLayout: BodyVizView (fill) | status Column (sp(180), right).
    Component {
        id: fullLayout

        RowLayout {
            anchors.fill:    parent
            anchors.margins: Theme.sp(8)
            spacing:         Theme.sp(12)

            BodyVizView {
                id: calibBvvFull
                Layout.fillHeight:   true
                Layout.fillWidth:    true
                Layout.minimumWidth: Theme.sp(200)
                Component.onCompleted: flow._bvv = calibBvvFull

                poseSource:       null   // no camera input during calibration
                rightHanded:      d.rightHanded
                highlightLeadArm: true
                leadArmColor:     Theme.colorAccent

                useLeadArmOverride:      true
                leadArmOverrideRotation: d._leadArmTarget
                // ⚠ THE HACKMOTION RAISE IS THIS PROPERTY. Its two poses share one
                // upper-arm rotation (the elbow must not move), so the travel the
                // device watches is a forearm rotation and _leadForeArmTarget is
                // what changes between the markers. Identity — unchanged — for the
                // Witmotion routine. Derivation in BodyVizView.qml.
                leadForeArmOverrideRotation: d.isHackMotion ? d._leadForeArmTarget
                                                            : Qt.quaternion(1, 0, 0, 0)

                useTrailArmOverride:          true
                trailArmOverrideRotation:     d.trailArmDownQuat
                trailForeArmOverrideRotation: Qt.quaternion(1, 0, 0, 0)

                animateLeadArm: d._animateLeadArm

                // ⚠ THE HACKMOTION ROUTINE IS INVISIBLE FROM THE DEFAULT FRONT
                // CAMERA. Its pose 0 points the forearm forward, almost along that
                // camera's view axis, so both the pose and the 30° sweep collapse
                // into a stub twitching sideways. The Witmotion routine's arm-down →
                // T-pose happens in the frontal plane and reads perfectly from the
                // front, which is why only this branch moves the camera.
                useGuideCamera: d.isHackMotion
            }

            Column {
                Layout.preferredWidth: Theme.sp(360)
                Layout.fillHeight:     true
                spacing:               Theme.sp(16)
                Layout.topMargin:      Theme.sp(32)

                Text {
                    visible:            flow.showHeader
                    width:              parent.width
                    text:               flow.stepLabel
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingMicro
                    color:              Theme.colorText3
                }

                PpDisplayText {
                    visible:        flow.showHeader
                    width:          parent.width
                    text:           qsTr("Calibrate Sensors")
                    pixelSize:      Math.min(Theme.sp(18), Theme.fontSzDisplay)
                    wrapMode:       Text.WordWrap
                }

                StatusBadge   { width: implicitWidth }
                PhaseText      { width: parent.width }
                ProgressBar    { width: parent.width }
                StatusLabel    { width: parent.width }
                AngleWarning   { width: parent.width }
                NoImuWarning   { width: parent.width }
                MountFailText  { width: parent.width }

                // Device-native routine — mutually exclusive with the four above.
                HmPhaseText    { width: parent.width }
                HmStepBar      { width: parent.width }
                HmStatusLabel  { width: parent.width }
                HmReadouts     { width: parent.width }
                HmFailText     { width: parent.width }
                HmPhaseDNote   { width: parent.width }

                PpButton {
                    visible: d.calibPhase > 0 || d.calibrationDone
                             || d.calibrationFailed || d.mountFailed
                             || (d.isHackMotion && d.hmStep > 0)
                    label:   qsTr("↺  Recalibrate")
                    primary: false
                    onClicked: flow.begin()
                }
            }
        }
    }

    // COMPACT — stacked BodyVizView (~sp(220)) + status, scrolling in a Flickable,
    // with the action bar PINNED at the bottom so it is always visible even when a
    // short window clamps the popup. The whole-panel attention frame is drawn by the
    // host panel; here the action bar is a plain container whose main action (Cancel)
    // uses the attention colour.
    Component {
        id: compactLayout

        Item {
            anchors.fill: parent

            // Pinned, always-visible action bar.
            Item {
                id: actionBar
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                anchors.margins: Theme.sp(12)
                height: Theme.sp(54)

                RowLayout {
                    anchors { fill: parent; leftMargin: Theme.sp(10); rightMargin: Theme.sp(10) }
                    spacing: Theme.sp(8)
                    PpButton {
                        visible: d.calibPhase > 0 || d.calibrationDone
                                 || d.calibrationFailed || d.mountFailed
                                 || (d.isHackMotion && d.hmStep > 0)
                        label:   qsTr("↺  Recalibrate")
                        primary: false
                        onClicked: flow.begin()
                    }
                    Item { Layout.fillWidth: true }
                    PpButton {
                        label:     qsTr("Cancel")
                        attention: true
                        // ⚠ A HackMotion routine given up on must be aborted, or the
                        // library sits in it until its own limits expire. The abort
                        // ALWAYS works — but at VERIFYING it only DECLINES the
                        // presence check, so _hmAbortIfActive() words it that way
                        // rather than claiming the calibration was undone.
                        onClicked: { d._hmAbortIfActive(); flow.cancelled() }
                    }
                }
            }

            Flickable {
                anchors { left: parent.left; right: parent.right; top: parent.top; bottom: actionBar.top }
                contentWidth: width
                contentHeight: compactCol.implicitHeight
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: compactCol
                    width: parent.width
                    spacing: Theme.sp(12)

                    BodyVizView {
                        id: calibBvvCompact
                        Layout.fillWidth:       true
                        Layout.preferredHeight: Theme.sp(220)
                        Layout.leftMargin:      Theme.sp(12)
                        Layout.rightMargin:     Theme.sp(12)
                        Layout.topMargin:       Theme.sp(12)
                        Component.onCompleted: flow._bvv = calibBvvCompact

                        poseSource:       null
                        rightHanded:      d.rightHanded
                        highlightLeadArm: true
                        leadArmColor:     Theme.colorAccent

                        useLeadArmOverride:      true
                        leadArmOverrideRotation: d._leadArmTarget
                        // See the full layout above: for HackMotion the RAISE itself
                        // lives in this property; identity for the Witmotion routine.
                        leadForeArmOverrideRotation: d.isHackMotion ? d._leadForeArmTarget
                                                                    : Qt.quaternion(1, 0, 0, 0)

                        useTrailArmOverride:          true
                        trailArmOverrideRotation:     d.trailArmDownQuat
                        trailForeArmOverrideRotation: Qt.quaternion(1, 0, 0, 0)

                        animateLeadArm: d._animateLeadArm

                        // Same reason as the full layout — see the comment there.
                        useGuideCamera: d.isHackMotion
                    }

                    ColumnLayout {
                        Layout.fillWidth:    true
                        Layout.leftMargin:   Theme.sp(15)
                        Layout.rightMargin:  Theme.sp(15)
                        Layout.bottomMargin: Theme.sp(12)
                        spacing: Theme.sp(12)

                        StatusBadge  { Layout.alignment: Qt.AlignLeft }
                        PhaseText     { Layout.fillWidth: true }
                        ProgressBar   { Layout.fillWidth: true }
                        StatusLabel   { Layout.fillWidth: true }
                        AngleWarning  { Layout.fillWidth: true }
                        NoImuWarning  { Layout.fillWidth: true }
                        MountFailText { Layout.fillWidth: true }

                        // Device-native routine — mutually exclusive with the four above.
                        HmPhaseText   { Layout.fillWidth: true }
                        HmStepBar     { Layout.fillWidth: true }
                        HmStatusLabel { Layout.fillWidth: true }
                        HmReadouts    { Layout.fillWidth: true }
                        HmFailText    { Layout.fillWidth: true }
                        HmPhaseDNote  { Layout.fillWidth: true }
                    }
                }
            }
        }
    }

    // ── Shared status sub-components (used by both layouts) ────────────────────
    // Status uses the wizard's existing colours: colorAccent (calibrating),
    // colorGood (done), colorWarn/colorError (issues). The compact layout
    // additionally frames its pinned action bar with the attention colour to draw
    // the eye to the controls; the status indicators themselves do not.

    // ⚠ The badge is SHARED by both routines, so "calibrating" has to be true for a
    // HackMotion too — its calibPhase never leaves 0, and a routine mid-flight
    // reading "Pending" is the kind of quietly wrong state this flow exists to
    // avoid. `calibrationFailed` is set by both routines, so _failed needs nothing.
    component StatusBadge: Rectangle {
        readonly property bool _complete:    d.calibrationDone
        readonly property bool _calibrating: !d.calibrationDone
                                             && (d.calibPhase >= 1
                                                 || (d.isHackMotion && d.hmStep >= 1 && d.hmStep <= 4))
        readonly property bool _failed:      (d.calibrationFailed || d.mountFailed) && !d.calibrationDone

        implicitWidth:  statusBadgeLbl.implicitWidth + Theme.sp(16)
        implicitHeight: Theme.sp(22)
        height:         Theme.sp(22)
        radius:         Theme.sp(11)
        color: _complete    ? Theme.colorGoodLight
             : _calibrating ? Theme.colorAccentLight
             : _failed      ? Theme.colorErrorLight
             :                Theme.colorBg3
        border.color: _complete    ? Theme.colorGood
                    : _calibrating ? Theme.colorAccent
                    : _failed      ? Theme.colorError
                    :                Theme.colorBorderMid

        Text {
            id: statusBadgeLbl
            anchors.centerIn: parent
            text: parent._complete    ? qsTr("Complete")
                : parent._calibrating ? qsTr("Calibrating")
                : parent._failed      ? qsTr("Failed")
                :                       qsTr("Pending")
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzMicro
            color: parent._complete    ? Theme.colorGood
                 : parent._calibrating ? Theme.colorAccent
                 : parent._failed      ? Theme.colorError
                 :                       Theme.colorText3
        }
    }

    // ⚠ The next four are the WITMOTION routine's status display and say things that
    // are simply untrue of a HackMotion (T-pose, hold-to-capture, arm-down). Hidden
    // rather than reworded, because the device-native routine has its own set below.
    // An invisible child is skipped by both Column and ColumnLayout, so nothing is
    // left holding space.
    component PhaseText: Text {
        visible:    !d.isHackMotion
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        color:          Theme.colorText2
        text: {
            if (d.calibPhase === 0)
                return qsTr("Watch the guide — this shows the T-pose position you'll hold next.")
            if (d.calibPhase === 1) {
                if (d._armDownCaptured)
                    return qsTr("Follow the guide and raise your arm out to shoulder height.")
                return qsTr("Let your lead arm hang relaxed at your side and hold still.")
            }
            return qsTr("Hold your arm at shoulder height and keep it still — the bar fills while you hold steady.")
        }
    }

    component ProgressBar: Item {
        visible: !d.isHackMotion
        height:  Theme.sp(32)
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width:  parent.width
            height: Theme.sp(4)
            radius: Theme.sp(2)
            color:  Theme.colorBg3
            Rectangle {
                readonly property real fillFraction: {
                    if (d.calibrationDone)        return 1.0
                    if (d.calibPhase === 2)       return d.phaseProgress
                    if (d.calibPhase === 1 && !d._armDownCaptured)
                        return Math.min(d.phase1AccumMs / d._captureHoldMs, 1.0)
                    return 0.0
                }
                width:  parent.width * fillFraction
                height: parent.height
                radius: parent.radius
                color:  d.calibrationDone ? Theme.colorGood : Theme.colorAccent
                Behavior on width { NumberAnimation { duration: 150 } }
            }
        }
    }

    component StatusLabel: Row {
        visible: !d.isHackMotion
        spacing: Theme.sp(6)
        Rectangle {
            visible:      d.calibrationDone
            width:        Theme.sp(16)
            height:       Theme.sp(16)
            radius:       width / 2
            color:        "transparent"
            border.color: Theme.colorGood
            border.width: Theme.sp(1.5)
            y:            (statusLbl.implicitHeight - height) / 2
            Text {
                anchors.centerIn: parent
                text:           "✓"
                color:          Theme.colorGood
                font.pixelSize: Theme.sp(9)
                font.bold:      true
            }
        }
        Text {
            id: statusLbl
            readonly property bool _capturing:
                (d.calibPhase === 1 && d.phase1AccumMs > 0 && !d._armDownCaptured)
                || d.calibPhase === 2
            width:              parent.width - (d.calibrationDone ? Theme.sp(22) : 0)
            wrapMode:           Text.WordWrap
            font.family:        Theme.fontData
            font.pixelSize:     _capturing ? Theme.fontSzHeading : Theme.fontSzMicro
            font.bold:          _capturing
            font.letterSpacing: Theme.trackingData
            color: d.calibrationDone ? Theme.colorGood
                 : _capturing         ? Theme.colorAccent
                 :                       Theme.colorText3
            text: {
                if (d.calibrationDone)    return qsTr("CALIBRATION COMPLETE")
                if (d.calibPhase === 0)   return qsTr("WATCH THE GUIDE")
                if (d.calibPhase === 1) {
                    if (d._armDownCaptured) return qsTr("FOLLOW THE GUIDE")
                    var imu = d.leadImu
                    if (!imu || !imu.imuConnected) return qsTr("WAITING FOR SENSOR")
                    if (d.phase1AccumMs > 0)       return qsTr("HOLD STILL — CAPTURING")
                    return qsTr("HOLD STILL")
                }
                return qsTr("HOLD STILL — CAPTURING")
            }
            Behavior on font.pixelSize { NumberAnimation { duration: Theme.durationNormal } }
            Behavior on color           { ColorAnimation  { duration: Theme.durationNormal } }
        }
    }

    // Calibration angle warning — shown when arm-down→T-pose angle was outside
    // the expected ~90° range. ⚠ WITMOTION ONLY: it grades OUR two-pose capture,
    // and a HackMotion computes its calibration on-device from a different routine
    // with nothing host-side to grade. There is no equivalent to invent.
    component AngleWarning: Text {
        readonly property bool _show: {
            if (d.isHackMotion) return false
            var imu = d.leadImu
            return d.calibrationDone && imu !== null && imu.calibrated && !imu.calibrationAngleValid
        }
        visible:    _show
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorWarn
        text:           qsTr("The arm positions didn't look quite right — the angle between arm-down and T-pose was much less than expected. For best results, make sure your arm is fully raised to shoulder height during the T-pose step, then tap Recalibrate.")
    }

    component NoImuWarning: Text {
        visible:    d.leadImu === null
        wrapMode:   Text.WordWrap
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorWarn
        text:           qsTr("No wrist sensor assigned to slot A. Return to the IMUs step to assign one.")
    }

    // ⚠ WITMOTION ONLY, like the mount validation that sets it. A HackMotion
    // computes its own calibration on-device; there is no A/M to solve, nothing to
    // store, and no mount check to fill the gap with.
    component MountFailText: Text {
        visible:    d.mountFailed && !d.calibrationDone && !d.isHackMotion
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorError
        text:           d.mountFailMsg
    }

    // ── HackMotion status sub-components ──────────────────────────────────────
    // Every one of these is hidden unless the slot-A device is a HackMotion, so the
    // two routines' displays never overlap.

    component HmPhaseText: Text {
        visible:    d.isHackMotion
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzBody2
        color:          Theme.colorText2
        text: {
            if (d.hmStep === 0)
                return qsTr("The sensor calibrates itself from two positions. Rest your lead "
                            + "forearm horizontal with the wrist straight — the guide shows the "
                            + "position.")
            if (d.hmStep === 1)
                return qsTr("Rest your forearm horizontal, wrist straight, and hold still.")
            if (d.hmStep === 2)
                return qsTr("Now raise your forearm smoothly across your chest, following the "
                            + "guide. The sensor watches the whole movement, so keep it to one "
                            + "smooth motion.")
            if (d.hmStep === 3)
                return qsTr("The sensor is working out its own calibration.")
            if (d.hmStep === 4)
                return qsTr("Return to the first position — forearm horizontal, wrist straight — "
                            + "and hold still while the sensor checks itself.")
            if (d.hmStep === 5)
                return qsTr("Calibrated. Hold the first position a moment longer: with the wrist "
                            + "straight and at rest, the two sensor units should now read within "
                            + "about a degree of each other.")
            return qsTr("The routine stopped. Tap Recalibrate to run it again.")
        }
    }

    // Four steps, discrete. No indeterminate progress and no elapsed-time bar: the
    // pacing that matters is the guide animation's, and a second clock next to it
    // invites the coach to hurry a motion the device is measuring.
    component HmStepBar: Row {
        visible: d.isHackMotion && d.hmStep >= 1 && d.hmStep <= 5
        spacing: Theme.sp(4)
        Repeater {
            model: 4
            Rectangle {
                width:  Theme.sp(38)
                height: Theme.sp(4)
                radius: Theme.sp(2)
                color: d.calibrationDone            ? Theme.colorGood
                     : (d.hmStep > index + 1)       ? Theme.colorAccent
                     : (d.hmStep === index + 1)     ? Theme.colorAccentLight
                     :                                Theme.colorBg3
            }
        }
    }

    component HmStatusLabel: Text {
        visible:            d.isHackMotion
        wrapMode:           Text.WordWrap
        font.family:        Theme.fontData
        font.pixelSize:     (d.hmStep >= 1 && d.hmStep <= 4) ? Theme.fontSzHeading : Theme.fontSzMicro
        font.bold:          d.hmStep >= 1 && d.hmStep <= 4
        font.letterSpacing: Theme.trackingData
        color: d.calibrationDone   ? Theme.colorGood
             : d.hmStep === 9      ? (d.hmFailKind === "warn" ? Theme.colorWarn : Theme.colorError)
             : d.hmStep >= 1       ? Theme.colorAccent
             :                       Theme.colorText3
        text: {
            if (d.calibrationDone) return qsTr("CALIBRATION COMPLETE")
            if (d.hmStep === 9)    return qsTr("NOT CALIBRATED")
            if (d.hmStep === 0)    return qsTr("READY")
            if (d.hmStep === 1)    return d.hmPhase === d.calpMarkingPose0
                                          ? qsTr("MARKING POSITION 1") : qsTr("HOLD STILL")
            if (d.hmStep === 2)    return d.hmPhase === d.calpMarkingPose1
                                          ? qsTr("MARKING POSITION 2") : qsTr("FOLLOW THE GUIDE")
            // ⚠ APPLYING and VERIFYING are NOT success. VERIFYING in particular means
            // the transform is already applied and the check has NOT been taken — so
            // no tick and nothing that reads as a verdict.
            if (d.hmStep === 3)    return qsTr("APPLYING…")
            if (d.hmStep === 4)    return d.hmAwaitingPresence ? qsTr("CHECKING…")
                                                              : qsTr("HOLD STILL")
            return ""
        }
        Behavior on font.pixelSize { NumberAnimation { duration: Theme.durationNormal } }
        Behavior on color           { ColorAnimation  { duration: Theme.durationNormal } }
    }

    // One label/value line of the confirmation readout. Data font on the value so it
    // reads as an instrument reading rather than as a grade.
    component HmReadout: Row {
        id: hmReadoutRow
        property string label: ""
        property string value: ""
        property color  tint:  Theme.colorText2
        spacing: Theme.sp(8)
        Text {
            width:          Theme.sp(150)
            text:           hmReadoutRow.label
            wrapMode:       Text.WordWrap
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
        }
        Text {
            text:               hmReadoutRow.value
            font.family:        Theme.fontData
            font.pixelSize:     Theme.fontSzMicro
            font.letterSpacing: Theme.trackingData
            color:              hmReadoutRow.tint
        }
    }

    // The confirmation readout. ⚠ Nothing here is a score and nothing here ranks
    // attempts: the relative-angle COLLAPSE is what proves the calibration took,
    // the calibration STATE is the only value that says a check was taken and
    // passed, and the presence angle is shown as state with the hold's own evidence
    // beside it.
    component HmReadouts: Column {
        visible: d.isHackMotion && (d.hmStep === 4 || d.hmStep === 5)
        spacing: Theme.sp(6)

        // ⚠ ONLY INTERPRETABLE AT REST WITH A STRAIGHT WRIST — the same stream reads
        // 170-180° mid-motion — which is why it is shown only while the athlete is
        // being asked to hold the reference pose, and labelled that way.
        HmReadout {
            label: qsTr("Both units, at rest, wrist straight")
            value: d._hmDeg(d.leadDevice ? d.leadDevice.relativeAngleDeg : NaN)
            tint:  Theme.colorText
        }
        Text {
            width:          parent.width
            wrapMode:       Text.WordWrap
            lineHeight:     1.4
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            text: qsTr("About 15° before calibration, 0.4–0.8° after. The collapse is what shows "
                       + "the calibration took — while the arm is moving the same figure reads "
                       + "170–180° and means nothing.")
        }

        // ⚠ THE ONE FIGURE THAT SEES THE HALF THE PRESENCE CHECK CANNOT. The device
        // reports a calibration even when nothing moved (§8.2: a no-raise attempt
        // scored the BEST presence angle of three), so this is the evidence that the
        // routine was actually performed. Shown as measured travel, never as a score:
        // more than the routine's ~30° is not "better", it is a different motion.
        HmReadout {
            label: qsTr("Forearm movement we saw")
            value: d._hmDeg(d._hmRaiseTravelDeg)
            tint:  isNaN(d._hmRaiseTravelDeg) ? Theme.colorText3
                 : d._hmRaiseTravelDeg >= d._hmMinRaiseTravelDeg ? Theme.colorGood
                 : Theme.colorWarn
        }

        HmReadout {
            label: qsTr("Sensor calibration state")
            value: d._hmStateText(d.leadDevice ? d.leadDevice.calibrationState : 0)
            tint:  (d.leadDevice && d.leadDevice.calibrationState === d.calCalibrated)
                       ? Theme.colorGood : Theme.colorWarn
        }
        HmReadout {
            label: qsTr("Reference-pose check")
            value: d._hmDeg(d.leadDevice ? d.leadDevice.presenceAngleDeg : NaN)
        }
        HmReadout {
            label: qsTr("Pose hold (max spread)")
            value: d._hmDeg(d.leadDevice ? d.leadDevice.poseSpreadMaxDeg : NaN)
        }
        // ⚠ The count means different things on the two paths — samples USED when a
        // measurement was taken, samples merely COLLECTED when the library warned it
        // could not take one — so presenceNotMeasured is what says which, and the
        // number is never shown without it.
        HmReadout {
            label: qsTr("Readings averaged")
            value: {
                var dev = d.leadDevice
                if (!dev) return "—"
                if (dev.presenceNotMeasured)
                    return qsTr("%1 — too few to measure").arg(dev.presenceSamplesUsed)
                return dev.presenceSamplesUsed > 0 ? String(dev.presenceSamplesUsed) : "—"
            }
            tint: (d.leadDevice && d.leadDevice.presenceNotMeasured) ? Theme.colorError
                                                                    : Theme.colorText2
        }
        Text {
            width:          parent.width
            wrapMode:       Text.WordWrap
            lineHeight:     1.4
            font.family:    Theme.fontBody
            font.pixelSize: Theme.fontSzMicro
            color:          Theme.colorText3
            // ⚠ Said in the UI, not only in a comment, because the temptation to
            // recalibrate until the number drops is exactly what §8.2 measured as
            // choosing the WORST attempt: no raise at all scored 0.70°, the correct
            // routine 1.96°, a raise about the wrong axis 6.10°.
            text: qsTr("The check confirms the sensor is calibrated — it is not a quality score "
                       + "and a smaller number is not a better calibration, so do not re-run to "
                       + "chase it down.")
        }
    }

    component HmFailText: Text {
        visible:    d.isHackMotion && d.hmStep === 9 && d.hmFailMsg !== ""
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          d.hmFailKind === "warn" ? Theme.colorWarn : Theme.colorError
        text:           d.hmFailMsg
    }

    // ⚠ Said plainly rather than shown as a live avatar. The plan's table puts an
    // ArmVizView free-movement confirmation here, but ArmVizView reads
    // anatCalibrated + anatQuat and BOTH ARE STUBS ON AN HmUnit UNTIL PHASE D
    // (false and identity), so the segment would park at rest — theatre that reads
    // as a broken calibration. The relative-angle collapse above is the real
    // confirmation; this is the honest note about what is still missing.
    component HmPhaseDNote: Text {
        visible:    d.isHackMotion && d.hmStep === 5
        wrapMode:   Text.WordWrap
        lineHeight: 1.5
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzMicro
        color:          Theme.colorText3
        text: qsTr("Live arm tracking from this sensor is not available yet — the sensor reports "
                   + "angles in its own anatomical frame and the conversion to ours is still being "
                   + "solved. Calibration and recording work; the on-screen arm does not follow it "
                   + "yet.")
    }
}
