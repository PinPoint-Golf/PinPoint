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

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import PinPointStudio

Item {
    id: root

    // ── Public API ────────────────────────────────────────────────────────────

    property int sessionType: 0

    signal cancelled()
    signal sessionStartRequested(int sessionType, var goals)
    signal cameraRecalibrateRequested()
    signal navigateToSettings(int panelIndex)   // settingsPanelCameras / settingsPanelImus

    // ScreenSettings sub-panel indices for navigateToSettings.
    readonly property int settingsPanelCameras: 3
    readonly property int settingsPanelImus:    4

    // Re-entry point after an external stereo calibration completes (see the
    // cameraRecalibrateRequested handler in Main.qml).
    function reopenAtTriangulate() {
        currentStep = stepTriangulate
        stepStates  = ["done", "done", "pending", "pending", "pending", "pending", "pending"]
    }

    // ── Step state ────────────────────────────────────────────────────────────

    // Named step indices (panels live in a StackLayout at these indices). Use
    // these everywhere instead of hardcoded numbers so the wizard stays robust.
    //   Goals(0) Cameras(1) Triangulate(2) BallCal(3) IMUs(4) Calibrate(5) Confirm(6) Ready(7)
    // Triangulate appears only once both camera perspectives are connected
    // (hasTriangulateStep); BallCal always shows and gates Continue on a live
    // detected ball (Skip is an escape hatch); Calibrate + Confirm are wrist-only
    // (hasCalibrateStep); all others run every session. (Zero-G / gyro-bias is
    // performed in the WitMotion app, not here.)
    readonly property int stepGoals:       0
    readonly property int stepCameras:     1
    readonly property int stepTriangulate: 2
    readonly property int stepBallCal:     3
    readonly property int stepImus:        4
    readonly property int stepCalibrate:   5
    readonly property int stepConfirm:     6
    readonly property int stepReady:       7
    readonly property int stepCount:       8

    property int currentStep: 0
    // stepCount entries, indexed by the named step constants above.
    property var stepStates: ["pending", "pending", "pending", "pending", "pending", "pending", "pending", "pending"]

    // Per-session IMU exclusion lives in ImuManager (imuManager.sessionImuExcluded)
    // so the wizard and every toolbar IMU panel share one list — same pattern as
    // cameras. Re-seeded from appSettings.imuExcluded each time the wizard opens
    // (see onVisibleChanged); never written back to settings (per-session choice).

    // Set by the Confirm-Tracking "Recalibrate" link before navigating back to
    // the Calibrate step, so onCurrentStepChanged forces a fresh IMU calibration
    // instead of retaining the previous "complete" state (backward navigation).
    property bool _forceImuRecalibrate: false

    // True only for Wrist Motion — enables the Calibrate + Confirm steps.
    readonly property bool hasCalibrateStep: sessionType === SessionController.Wrist

    // True once both a face-on AND a down-the-line camera are connected — only
    // then is there a camera pair to triangulate. Dynamic by design: the step
    // appears the moment the second perspective connects, whether via the
    // wizard's Connect or the toolbar camera panel.
    readonly property bool hasTriangulateStep: {
        var fo = false, dtl = false
        var list = cameraManager.cameraList
        for (var i = 0; i < list.length; ++i) {
            if (!list[i].selected) continue
            if (list[i].perspective === CameraInstance.FaceOn)          fo  = true
            else if (list[i].perspective === CameraInstance.DownTheLine) dtl = true
        }
        return fo && dtl
    }

    // First CONNECTED face-on camera instance (reactive on instances) and the
    // ball-detection state — drives the BallCal step, its footer hint/Skip, and
    // the Ready summary row. The face-on instance whose live presence the v2
    // temporal detector reports.
    readonly property QtObject ballFaceOnInst: {
        var insts = cameraManager.instances
        var fo = faceOnList
        for (var i = 0; i < fo.length; ++i)
            for (var j = 0; j < insts.length; ++j)
                if (insts[j].cameraKey === fo[i].cameraKey)
                    return insts[j]
        return null
    }
    // v2: the ball step gates on LIVE detection (a placed ball), not a saved
    // calibration profile. ballPresent is the instance's smoothed present flag.
    readonly property bool ballReady: ballFaceOnInst !== null && ballFaceOnInst.ballPresent

    // Ordered indices of the steps visible for the current session type and
    // camera state — the single source of truth for navigation (goNext/goBack),
    // the tab strip, and the "STEP x OF N" numbering.
    readonly property var visibleSteps: {
        var s = [stepGoals, stepCameras]
        if (hasTriangulateStep) s.push(stepTriangulate)
        s.push(stepBallCal)
        s.push(stepImus)
        if (hasCalibrateStep) { s.push(stepCalibrate); s.push(stepConfirm) }
        s.push(stepReady)
        return s
    }

    // Display number per step index, for the "STEP x OF N" eyebrows and tab pips.
    readonly property var stepNumbers: {
        var nums = [], vs = visibleSteps
        for (var i = 0; i < vs.length; ++i) nums[vs[i]] = i + 1
        return nums
    }

    // Terminal step index and total visible steps for the "STEP x OF N" eyebrows.
    readonly property int lastStep:   stepReady
    readonly property int totalSteps: visibleSteps.length

    // Index of the visible step preceding idx (tab-strip connector lines).
    function prevVisibleStep(idx) {
        var vs = visibleSteps, p = stepGoals
        for (var i = 0; i < vs.length; ++i) {
            if (vs[i] >= idx) break
            p = vs[i]
        }
        return p
    }

    // Mark the current step and advance to the next visible step.
    // mark: "done" | "skipped". Completing an unfinished Calibrate step is
    // blocked (Skip is the explicit way past it) — keeps the toolbar ‹/›
    // arrows consistent with the wizard footer.
    function goNext(mark) {
        if (currentStep >= lastStep) return
        if (currentStep === stepCalibrate && mark === "done" && !calibFlow.calibrationDone) return
        if (currentStep === stepBallCal && mark === "done" && !ballReady) return  // gate on live detection
        var arr = stepStates.slice()
        arr[currentStep] = mark
        stepStates = arr
        var vs = visibleSteps
        for (var i = 0; i < vs.length; ++i)
            if (vs[i] > currentStep) { currentStep = vs[i]; return }
    }

    // Step back to the previous visible step, resetting the current one.
    function goBack() {
        if (currentStep <= 0) return
        var arr = stepStates.slice()
        arr[currentStep] = "pending"
        stepStates = arr
        var vs = visibleSteps
        for (var i = vs.length - 1; i >= 0; --i)
            if (vs[i] < currentStep) { currentStep = vs[i]; return }
    }

    // Fresh wizard for a new session of the given type (ScreenHome entry point).
    function reset(type) {
        sessionType = type
        currentStep = stepGoals
        var arr = []
        for (var i = 0; i < stepCount; ++i) arr.push("pending")
        stepStates = arr
    }

    // Release every device the setup flow may have connected — called when the
    // user abandons the wizard (Cancel, or ‹ backing out of the first step; see
    // Main.qml). Mirrors the End Session teardown in PpSessionToolbar: cameras
    // stop + deselect, IMUs disconnect (BLE battery), and capture intent goes
    // off, which also releases the microphone (shot-detection audio follows
    // captureIntent). The paced IMU connect queue must be aborted first: the
    // wizard stays instantiated in Main.qml's StackLayout, so a running queue
    // would otherwise keep connecting devices after the wizard is dismissed.
    function releaseDevices() {
        imuConnectTimer.stop()
        imusCol._connecting   = false
        imusCol._connectQueue = []
        cameraManager.stopCapture()
        cameraManager.disconnectAll()
        imuManager.disconnectAll()
    }

    // The Calibrate / Confirm steps host a full 3D visualisation that should use
    // the entire viewport width, unlike the text-led steps which are clamped to a
    // comfortable reading column (Theme.contentWidth).
    readonly property bool isVizStep: currentStep === stepCalibrate || currentStep === stepConfirm

    // ── Positional calibration (Panel 3) ──────────────────────────────────────
    // The calibration state machine, timers, helpers, BodyVizView guide, status
    // column and completion ting now live in ImuCalibrationFlow.qml (calibFlow,
    // hosted by Panel 3). The wizard reads calibFlow.calibrationDone for gating
    // and drives it via begin() / showCompleted() from onCurrentStepChanged.

    // Track the previous step so we know the direction of navigation.
    property int _prevStep: -1

    onCurrentStepChanged: {
        _prevStep   = currentStep

        if (currentStep === root.stepCalibrate) {
            // "Recalibrate" from Confirm Tracking: force a fresh IMU calibration
            // regardless of navigation direction (otherwise the restore branch
            // below would restore the previous "complete" state).
            if (root._forceImuRecalibrate) {
                root._forceImuRecalibrate = false
                calibFlow.begin()
                return
            }

            // Restore the completed state only if the lead IMU actually carries a
            // calibration from this session — true whether we arrived forward
            // (already calibrated) or backward from a later step. The Skip button
            // advances past Calibrate to Confirm WITHOUT calibrating, so backward
            // nav must not assume completion; doing so flipped calibrationDone
            // false→true, dinging the success chime and falsely marking
            // calibration done.
            var imu = calibFlow.leadImu
            if (imu !== null && imu.calibrated)
                calibFlow.showCompleted()
            else
                calibFlow.begin()
        }
    }

    // ── Session type data ─────────────────────────────────────────────────────

    readonly property var sessionTypes: [
        { icon: "◑", name: qsTr("Swing Analysis"),
          requiredCameras: 2, requiredImus: 3, railIndex: 1, comingSoon: true },
        { icon: "⌖", name: qsTr("Wrist Motion"),
          requiredCameras: 1, requiredImus: 2, railIndex: 2 },
        { icon: "⇅", name: qsTr("Ground Forces"),
          requiredCameras: 2, requiredImus: 3, railIndex: 3, comingSoon: true },
        { icon: "✦", name: qsTr("AI Coach"),
          requiredCameras: 2, requiredImus: 3, railIndex: 4, comingSoon: true }
    ]

    // ── IMU placement requirements per session type ───────────────────────────
    // Indexed 0–3 to match sessionTypes above. Edit here to change requirements.
    // slot: label shown in the row ("A"/"B"/"C")
    // placement: where on the body the sensor is mounted
    // required: false = optional slot, shown with muted styling

    readonly property var imuRequirements: [
        // 0 — Swing Analysis
        [
            { slot: "A", placement: qsTr("Thorax"),       required: true  },
            { slot: "B", placement: qsTr("Lumbar spine"), required: true  },
            { slot: "C", placement: qsTr("T12 junction"), required: true  }
        ],
        // 1 — Wrist Motion
        [
            { slot: "A", placement: qsTr("Forearm"),      required: true  },
            { slot: "B", placement: qsTr("Hand"),         required: true  },
            { slot: "C", placement: qsTr("Upper arm"),    required: false }
        ],
        // 2 — Ground Forces
        [
            { slot: "A", placement: qsTr("Lead thigh"),   required: true  },
            { slot: "B", placement: qsTr("Trail thigh"),  required: true  },
            { slot: "C", placement: qsTr("Lumbar spine"), required: true  }
        ],
        // 3 — AI Coach (same placements as Swing Analysis)
        [
            { slot: "A", placement: qsTr("Thorax"),       required: true  },
            { slot: "B", placement: qsTr("Lumbar spine"), required: true  },
            { slot: "C", placement: qsTr("T12 junction"), required: true  }
        ]
    ]

    // ── Goal chip data — indexed 0–3 to match sessionTypes ──────────────────
    // Edit here to change goals per session type without touching UI code.

    readonly property var goalDefsByType: [
        // 0 — Swing Analysis
        [
            { key: "generalAssessment", name: qsTr("General assessment"),  sub: qsTr("CHECK WHERE I'M AT · IDENTIFY AREAS TO IMPROVE") },
            { key: "kinematicSequence", name: qsTr("Kinematic sequence"),  sub: qsTr("SEGMENT VELOCITY ORDER")    },
            { key: "xFactor",           name: qsTr("X-factor"),            sub: qsTr("HIP–SHOULDER SEPARATION")   },
            { key: "swingTempo",        name: qsTr("Swing tempo"),          sub: qsTr("BACK : DOWN RATIO")         },
            { key: "earlyExtension",    name: qsTr("Early extension"),      sub: qsTr("HIP SWAY DETECTION")        },
            { key: "clubPath",          name: qsTr("Club path"),            sub: qsTr("IN-OUT TREND")              },
            { key: "wristAngles",       name: qsTr("Wrist angles"),         sub: qsTr("FLEXION AT IMPACT")         }
        ],
        // 1 — Wrist Motion
        [
            { key: "generalAssessment", name: qsTr("General assessment"),  sub: qsTr("CHECK WHERE I'M AT · IDENTIFY AREAS TO IMPROVE") },
            { key: "wristAngleTop",       name: qsTr("Wrist angle at the top"),    sub: qsTr("FLAT / BOWED / CUPPED AT TOP")     },
            { key: "impactConditions",    name: qsTr("Impact conditions"),         sub: qsTr("FLEXION / EXTENSION AT CONTACT")   },
            { key: "wristAngleSequence",  name: qsTr("Wrist angle sequence"),      sub: qsTr("TRANSITION & ARC PROFILE")         },
            { key: "trailWristExtension", name: qsTr("Trail wrist extension"),     sub: qsTr("SCOOP / FLIP DETECTION")           }
        ],
        // 2 — Ground Forces
        [
            { key: "generalAssessment", name: qsTr("General assessment"),  sub: qsTr("CHECK WHERE I'M AT · IDENTIFY AREAS TO IMPROVE") },
            { key: "kinematicSequence", name: qsTr("Kinematic sequence"),  sub: qsTr("SEGMENT VELOCITY ORDER")    },
            { key: "xFactor",           name: qsTr("X-factor"),            sub: qsTr("HIP–SHOULDER SEPARATION")   },
            { key: "swingTempo",        name: qsTr("Swing tempo"),          sub: qsTr("BACK : DOWN RATIO")         },
            { key: "earlyExtension",    name: qsTr("Early extension"),      sub: qsTr("HIP SWAY DETECTION")        },
            { key: "clubPath",          name: qsTr("Club path"),            sub: qsTr("IN-OUT TREND")              },
            { key: "wristAngles",       name: qsTr("Wrist angles"),         sub: qsTr("FLEXION AT IMPACT")         }
        ],
        // 3 — AI Coach
        [
            { key: "generalAssessment", name: qsTr("General assessment"),  sub: qsTr("CHECK WHERE I'M AT · IDENTIFY AREAS TO IMPROVE") },
            { key: "kinematicSequence", name: qsTr("Kinematic sequence"),  sub: qsTr("SEGMENT VELOCITY ORDER")    },
            { key: "xFactor",           name: qsTr("X-factor"),            sub: qsTr("HIP–SHOULDER SEPARATION")   },
            { key: "swingTempo",        name: qsTr("Swing tempo"),          sub: qsTr("BACK : DOWN RATIO")         },
            { key: "earlyExtension",    name: qsTr("Early extension"),      sub: qsTr("HIP SWAY DETECTION")        },
            { key: "clubPath",          name: qsTr("Club path"),            sub: qsTr("IN-OUT TREND")              },
            { key: "wristAngles",       name: qsTr("Wrist angles"),         sub: qsTr("FLEXION AT IMPACT")         }
        ]
    ]

    readonly property var curGoalDefs: goalDefsByType[sessionType]

    property var  selectedGoals:   []
    property bool goalsInteracted: false

    // ── TODO stubs for camera properties not yet on cameraManager ───────────────

    property bool _todo_stereoCalibrationValid: false  // TODO: bind to real property
    property bool _todo_triangulationValid:     false  // TODO: bind to real property

    // ── IMU detail string when connected ──────────────────────────────────────

    readonly property string imuDetail: {
        var insts = imuManager.instances // rebind on connection changes
        if (insts.length === 0) return ""
        // Show detail from the first connected instance.
        var inst = null
        for (var i = 0; i < insts.length; ++i) {
            if (insts[i].imuConnected) { inst = insts[i]; break }
        }
        if (!inst) return ""
        var parts = []
        var hz = inst.dataRateHz
        if (hz > 0) parts.push(Math.round(hz) + " Hz")
        var rate = inst.outputRateHz
        if (rate > 0) parts.push(qsTr("configured %1 Hz").arg(rate))
        var bat = inst.batteryPercent
        if (bat >= 0) parts.push(qsTr("battery %1%").arg(bat))
        if (insts.length > 1) parts.push(qsTr("%1 IMUs").arg(insts.length))
        return parts.join(" · ")
    }

    // ── Convenience ───────────────────────────────────────────────────────────

    readonly property var curType:    sessionTypes[sessionType]
    readonly property var curImuReqs: imuRequirements[sessionType]

    // All cameraList entries assigned to a given perspective — any number of
    // cameras may share one (e.g. two face-on cameras). Reads directly from
    // cameraList which includes the persisted perspective field, so this works
    // whether or not cameras are currently selected.
    readonly property var faceOnList: {
        var out = [], list = cameraManager.cameraList
        for (var i = 0; i < list.length; ++i)
            if (list[i].perspective === CameraInstance.FaceOn) out.push(list[i])
        return out
    }

    readonly property var dtlList: {
        var out = [], list = cameraManager.cameraList
        for (var i = 0; i < list.length; ++i)
            if (list[i].perspective === CameraInstance.DownTheLine) out.push(list[i])
        return out
    }

    // Every available camera participates in this session — capture is not
    // limited by view assignment. Perspective only gates the ANALYSIS minimums
    // (camsAllConnected: ≥1 face-on, plus ≥1 down-the-line for two-camera
    // session types — requiredCameras is a MINIMUM, not a maximum) and the
    // Triangulate step.
    readonly property var sessionCamList: cameraManager.cameraList

    function camDetail(d) {
        if (!d) return ""
        var sn  = d.serialNumber ? qsTr("SN: ") + d.serialNumber : ""
        var res = (d.maxWidth && d.maxHeight) ? d.maxWidth + " × " + d.maxHeight : ""
        var ifc = d.interface || ""
        return [d.alias || d.description, sn, ifc, res].filter(function(s){ return s !== "" }).join(" · ")
    }

    // True when at least one of the triangulation pair's cameras (face-on or
    // down-the-line — unassigned cameras don't triangulate) is marked fixed in
    // place — stereo calibration and triangulation become optional steps then.
    readonly property bool anyFixedCamera: {
        var fixed = appSettings.cameraFixedInPlace
        var cams  = faceOnList.concat(dtlList)
        for (var i = 0; i < cams.length; ++i)
            if (fixed[cams[i].cameraKey]) return true
        return false
    }

    // Every ENABLED session camera is connected AND the session minimum is met
    // (≥1 face-on connected; ≥1 down-the-line too for two-camera sessions).
    // Reactive via cameraList (selection changes fire cameraListChanged).
    // Mirrors imusAllConnected.
    readonly property bool camsAllConnected: {
        var cams = sessionCamList
        var fo = 0, dtl = 0
        for (var i = 0; i < cams.length; ++i) {
            if (!cams[i].sessionEnabled) continue
            if (!cams[i].selected) return false
            if (cams[i].perspective === CameraInstance.FaceOn)           ++fo
            else if (cams[i].perspective === CameraInstance.DownTheLine) ++dtl
        }
        if (fo === 0) return false
        if (curType.requiredCameras === 2 && dtl === 0) return false
        return true
    }

    // (A slot→imuDeviceList-row helper lived here and had no callers anywhere in
    // the tree, in this file or outside it. Phase C's migration would have had to
    // rename it as well — imuManager now has its own deviceForSlot() — so it was
    // removed instead. The rows that DO need this resolve it inline from
    // imuManager.deviceIdForSlot(); see the CheckRow delegate's _dev below.)

    // All required IMU slots assigned AND their devices present. The presence
    // half restores what the pre-Phase-C walk of imuDeviceList enforced for
    // free: a placement entry whose sensor is dead, at home, or simply not
    // discovered this session must not satisfy the step — the coach would sail
    // past it into a Connect that can never succeed. (imuDeviceList is read in
    // the binding, so re-scans re-evaluate this.)
    readonly property bool imusOk: {
        // deviceIdForSlot() is Q_INVOKABLE, not reactive on its own — this read
        // is the explicit dependency that used to come for free from walking
        // the placement map directly.
        var _dep = appSettings.imuPlacement
        var list = imuManager.imuDeviceList
        var reqs = root.curImuReqs
        for (var i = 0; i < reqs.length; ++i) {
            if (!reqs[i].required) continue
            var id = imuManager.deviceIdForSlot(reqs[i].slot)
            if (id === "") return false
            var present = false
            for (var j = 0; j < list.length; ++j)
                if (list[j].id === id) { present = true; break }
            if (!present) return false
        }
        return reqs.some(function(r) { return r.required })
    }

    // True when every required IMU slot has a connected instance.
    // Reactive: imuManager.instances fires instancesChanged() whenever any
    // ImuInstance.imuConnected changes (see imu_manager.cpp lambda at createInstance).
    readonly property bool imusAllConnected: {
        var _dep  = imuManager.instances     // reactive dependency
        var _dep2 = appSettings.imuPlacement // deviceForSlot() is Q_INVOKABLE, not reactive on its own
        var reqs = root.curImuReqs
        for (var i = 0; i < reqs.length; ++i) {
            if (!reqs[i].required) continue
            // Connect state is a DEVICE property (imu_manager.h ⚠) — deviceForSlot(),
            // NOT instanceForSlot(), which for a HackMotion slot hands back the
            // per-unit HmUnit and has no imuConnected of its own.
            var dev = imuManager.deviceForSlot(reqs[i].slot)
            if (!dev || !dev.imuConnected) return false
        }
        return reqs.some(function(r) { return r.required })
    }

    // Every unmet requirement — drives the Ready panel content.
    // Reactive: re-evaluates when hardware state, step states, or placements change.
    // Each entry: { text: string, panel: int }
    // panel >= 0 renders a clickable "→ Open … settings" link that navigates
    // to that Settings sub-panel (settingsPanelCameras / settingsPanelImus)
    // and returns here via ‹. panel = -1 means no actionable settings link.
    readonly property var readinessIssues: {
        var issues = []

        // Camera requirements — session capture starts on wizard completion, so
        // a camera that is assigned but disabled/unconnected is an issue too,
        // not just a missing assignment. Per-perspective ladder: assigned →
        // enabled → connected; several cameras may share a perspective, the
        // minimum is one connected each.
        if (stepStates[stepCameras] === "skipped") {
            issues.push({ text: qsTr("Cameras skipped — no video will be captured this session"), panel: -1 })
        } else {
            // View-assignment minimums — analysis needs ≥1 face-on (+ ≥1
            // down-the-line for two-camera session types).
            if (faceOnList.length === 0)
                issues.push({ text: qsTr("Face-on camera not assigned"), panel: settingsPanelCameras })
            if (curType.requiredCameras === 2 && dtlList.length === 0)
                issues.push({ text: qsTr("Down-the-line camera not assigned"), panel: settingsPanelCameras })
            // Connection — every enabled camera should be streaming by now.
            var cams = sessionCamList
            var anyEnabled = false
            for (var c = 0; c < cams.length; ++c) {
                if (!cams[c].sessionEnabled) continue
                anyEnabled = true
                if (!cams[c].selected)
                    issues.push({ text: qsTr("%1 not connected — press Connect in the cameras step")
                                            .arg(cams[c].alias || cams[c].description), panel: -1 })
            }
            if (cams.length > 0 && !anyEnabled)
                issues.push({ text: qsTr("All cameras disabled for this session"), panel: -1 })
            // Pair checks only matter once there is a camera pair to triangulate,
            // and only when the cameras are not fixed in place.
            if (hasTriangulateStep && !anyFixedCamera) {
                if (!_todo_stereoCalibrationValid)
                    issues.push({ text: qsTr("Stereo calibration not confirmed — use Recalibrate in the triangulation step"), panel: -1 })
                if (!_todo_triangulationValid)
                    issues.push({ text: qsTr("Triangulation not confirmed"), panel: -1 })
            }
        }

        // IMU requirements — one entry per unassigned required slot, plus calibration
        if (stepStates[stepImus] === "skipped") {
            issues.push({ text: qsTr("Motion sensors skipped — no movement data will be captured"), panel: -1 })
        } else {
            var reqs = curImuReqs
            // deviceIdForSlot() is Q_INVOKABLE, not reactive on its own — keep
            // an explicit read of imuPlacement so this whole readinessIssues
            // binding still re-evaluates when placement changes, the way it
            // used to for free by reading the map directly. imuDeviceList is
            // read for the presence check and is reactive on its own.
            var _dep = appSettings.imuPlacement
            var list = imuManager.imuDeviceList
            for (var i = 0; i < reqs.length; ++i) {
                if (!reqs[i].required) continue
                var id = imuManager.deviceIdForSlot(reqs[i].slot)
                if (id === "") {
                    issues.push({ text: qsTr("IMU %1 — %2 not assigned")
                                            .arg(reqs[i].slot).arg(reqs[i].placement),
                                  panel: settingsPanelImus })
                    continue
                }
                // Assigned, but the sensor was not found this session — the
                // pre-Phase-C wizard called this "not assigned", which sent the
                // coach to re-do a placement that was fine. Say what is wrong.
                var present = false
                for (var j = 0; j < list.length; ++j)
                    if (list[j].id === id) { present = true; break }
                if (!present)
                    issues.push({ text: qsTr("IMU %1 — %2 assigned but the sensor was not found. Power it on and Scan.")
                                            .arg(reqs[i].slot).arg(reqs[i].placement),
                                  panel: settingsPanelImus })
            }
            if (root.hasCalibrateStep && !calibFlow.calibrationDone)
                issues.push({ text: qsTr("Sensor position calibration not completed — return to the Calibrate step"), panel: -1 })
        }

        return issues
    }

    readonly property bool fullyReady: readinessIssues.length === 0

    readonly property int contentWidth: Theme.contentWidth(width)

    onVisibleChanged: {
        if (visible) {
            var saved = appSettings.sessionGoalsByType[sessionType.toString()]
            selectedGoals   = (saved && saved.length > 0) ? saved.slice() : []
            goalsInteracted = false
            // Session enablement is manager-owned (shared with the toolbar
            // panels and video tiles). A new session inherits the global
            // enablement afresh — disabling also disconnects a connected device.
            var camList = cameraManager.cameraList
            for (var i = 0; i < camList.length; ++i)
                cameraManager.setSessionCameraEnabled(camList[i].cameraKey,
                    appSettings.cameraExcluded.indexOf(camList[i].cameraKey) < 0)
            var devList = imuManager.imuDeviceList
            for (var j = 0; j < devList.length; ++j)
                imuManager.setSessionImuEnabled(devList[j].id,
                    appSettings.imuExcluded.indexOf(devList[j].id) < 0)
        }
    }

    // ── Page layout ───────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Header ────────────────────────────────────────────────────────────

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: hdrInner.implicitHeight + Theme.sp(40)

            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width:  root.contentWidth
                height: parent.height

                Column {
                    id: hdrInner
                    anchors {
                        left:           parent.left
                        right:          closeBtn.left
                        verticalCenter: parent.verticalCenter
                        rightMargin:    Theme.sp(16)
                    }
                    spacing: Theme.sp(6)

                    Row {
                        spacing: Theme.sp(8)
                        Text {
                            text:           root.curType.icon
                            font.pixelSize: Theme.sp(20)
                            color:          Theme.colorText2
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text:           root.curType.name
                            font.family:    Theme.fontDisplay
                            font.italic:    Theme.fontDisplayItalic
                            font.weight: Theme.fontDisplayWeight
                            font.pixelSize: Math.min(Theme.sp(22), Theme.fontSzDisplay)
                            color:          Theme.colorText
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    Text {
                        width:          parent.width
                        text:           qsTr("Before you step up, let's make sure everything is ready. We'll confirm your goals, check your cameras, and connect your sensors — it only takes a moment.")
                        font.family:    Theme.fontBody
                        font.weight:    Theme.fontBodyWeight
                        font.pixelSize: Theme.fontSzBody2
                        color:          Theme.colorText2
                        wrapMode:       Text.WordWrap
                        lineHeight:     1.65
                    }

                    Text {
                        text:               athleteController.currentName !== ""
                                                ? athleteController.currentName.toUpperCase()
                                                : qsTr("NO ATHLETE")
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingMicro
                        color:              Theme.colorText3
                    }
                }

                PpButton {
                    id: closeBtn
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    label:     qsTr("Cancel")
                    onClicked: root.cancelled()
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid }

        // ── Tab strip ─────────────────────────────────────────────────────────

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sp(56)

            Row {
                anchors.centerIn: parent
                spacing: 0

                Repeater {
                    // stepCount tabs always present; conditional steps (Triangulate,
                    // Calibrate, Confirm) hide via visibleSteps. Indices match the
                    // named step constants.
                    model: [qsTr("Goals"), qsTr("Cameras"), qsTr("Triangulate"), qsTr("Ball"), qsTr("IMUs"), qsTr("Calibrate"), qsTr("Confirm"), qsTr("Ready")]

                    delegate: Row {
                        id: tabRow
                        required property string modelData
                        required property int    index
                        spacing: 0
                        visible: root.visibleSteps.indexOf(tabRow.index) !== -1

                        Item {
                            visible: tabRow.index > 0
                            width:   Theme.sp(28); height: Theme.sp(32)

                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left:  parent.left
                                anchors.right: parent.right
                                height: 1
                                color: root.stepStates[root.prevVisibleStep(tabRow.index)] === "done"
                                           ? Theme.colorGood : Theme.colorBorderMid
                                Behavior on color { ColorAnimation { duration: Theme.durationNormal } }
                            }
                        }

                        Column {
                            spacing: Theme.sp(4)

                            readonly property string pipState: {
                                if (tabRow.index === root.currentStep) return "current"
                                var s = root.stepStates[tabRow.index]
                                if (s === "done")    return "done"
                                if (s === "skipped") return "skipped"
                                return "future"
                            }

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: Theme.sp(16); height: Theme.sp(16)
                                radius: Theme.sp(8)
                                color: {
                                    var s = parent.pipState
                                    if (s === "current") return Theme.colorAccent
                                    if (s === "done")    return Theme.colorGoodLight
                                    if (s === "skipped") return Theme.colorWarnLight
                                    return Theme.colorBg3
                                }
                                border.width: 1
                                border.color: {
                                    var s = parent.pipState
                                    if (s === "current") return Theme.colorAccent
                                    if (s === "done")    return Theme.colorGood
                                    if (s === "skipped") return Theme.colorWarn
                                    return Theme.colorBorderMid
                                }
                                Behavior on color        { ColorAnimation { duration: Theme.durationNormal } }
                                Behavior on border.color { ColorAnimation { duration: Theme.durationNormal } }

                                Text {
                                    anchors.centerIn: parent
                                    text: {
                                        var s = parent.parent.pipState
                                        if (s === "done")    return "✓"
                                        if (s === "skipped") return "⚠"
                                        // Hidden tabs have no number — they're invisible anyway.
                                        return String(root.stepNumbers[tabRow.index] || "")
                                    }
                                    font.family:    Theme.fontData
                                    font.pixelSize: Theme.fontSzMicro
                                    color: {
                                        var s = parent.parent.pipState
                                        if (s === "current") return Theme.dark ? Theme.colorBg : "#FFFFFF"
                                        if (s === "done")    return Theme.colorGood
                                        if (s === "skipped") return Theme.colorWarn
                                        return Theme.colorText3
                                    }
                                }
                            }

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text:           tabRow.modelData
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzMicro
                                color: {
                                    var s = parent.pipState
                                    if (s === "current") return Theme.colorAccent
                                    if (s === "done")    return Theme.colorGood
                                    if (s === "skipped") return Theme.colorWarn
                                    return Theme.colorText3
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid }

        // ── Body — Flickable so tall panels don't clip ────────────────────────

        Flickable {
            Layout.fillWidth:  true
            Layout.fillHeight: true
            contentWidth:  width
            // Viz steps fill the viewport exactly (no scroll padding); text-led
            // steps add a little bottom breathing room. The Math.max keeps content
            // scrollable if it ever exceeds the viewport (e.g. a very short window).
            contentHeight: Math.max(height, bodyPanel.implicitHeight + (root.isVizStep ? 0 : Theme.sp(40)))
            clip:          true

            Item {
                id: bodyPanel
                anchors.horizontalCenter: parent.horizontalCenter
                // Viz steps span the full viewport; text-led steps stay in the
                // centred reading column.
                width:          root.isVizStep ? parent.width : root.contentWidth
                implicitHeight: bodyStack.implicitHeight

                StackLayout {
                    id: bodyStack
                    width:        parent.width
                    currentIndex: root.currentStep

                    // ── Panel 0: Goals ────────────────────────────────────────

                    Item {
                        implicitHeight: goalsCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: goalsCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(16)

                            StepIntro {
                                width:   parent.width
                                eyebrow: qsTr("STEP %1 OF %2 · GOALS").arg(root.stepNumbers[root.stepGoals]).arg(root.totalSteps)
                                heading: qsTr("What are you working on today?")
                                body:    qsTr("Pick the areas you want to focus on this session. Pinpoint will highlight these metrics in the live view and lead with them in your summary. We've picked up where you left off — change anything you like and we'll remember it next time.")
                            }

                            GridLayout {
                                width:         parent.width
                                columns:       2
                                columnSpacing: Theme.sp(8)
                                rowSpacing:    Theme.sp(8)

                                Repeater {
                                    model: root.curGoalDefs

                                    delegate: Rectangle {
                                        id: chip
                                        required property var modelData
                                        required property int index

                                        Layout.fillWidth:    true
                                        Layout.preferredHeight: chipCol.implicitHeight + Theme.sp(36)
                                        radius: Theme.radius

                                        readonly property bool sel: root.selectedGoals.indexOf(modelData.key) !== -1
                                        readonly property bool showLast: !root.goalsInteracted
                                                                         && sel
                                                                         && root.selectedGoals.length > 0
                                                                         && root.selectedGoals[0] === modelData.key

                                        // Multi-select goal card — chip/tile language: selected shows the
                                        // accent wash + accent border; hover (unselected) fades a faint bg
                                        // fill in (alpha-ramped → no flash) and eases the border to accentMid.
                                        // Pairs with the PpPressable scale-grow / press-dip below.
                                        color:        sel                  ? Theme.colorAccentLight
                                                    : goalMa.containsMouse ? Theme.colorBg2
                                                    :                        Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0)
                                        border.width: 1
                                        border.color: sel                  ? Theme.colorAccent
                                                    : goalMa.containsMouse ? Theme.colorAccentMid
                                                    :                        Theme.colorBorderMid
                                        Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                                        Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                        Column {
                                            id: chipCol
                                            anchors {
                                                left: parent.left; right: parent.right
                                                leftMargin: Theme.sp(14); rightMargin: Theme.sp(14)
                                                verticalCenter: parent.verticalCenter
                                            }
                                            spacing: Theme.sp(5)

                                            Row {
                                                spacing: Theme.sp(6)
                                                Text {
                                                    text:           chip.modelData.name
                                                    font.family:    Theme.fontBody
                                                    font.pixelSize: Theme.fontSzBody2
                                                    color:          chip.sel ? Theme.colorAccent : Theme.colorText
                                                }
                                                Text {
                                                    visible:        chip.showLast
                                                    text:           qsTr("↩ last session")
                                                    font.family:    Theme.fontData
                                                    font.pixelSize: Theme.fontSzMicro
                                                    color:          Theme.colorText3
                                                    anchors.verticalCenter: parent.verticalCenter
                                                }
                                            }
                                            Text {
                                                width:              parent.width
                                                text:               chip.modelData.sub
                                                font.family:        Theme.fontData
                                                font.pixelSize:     Theme.fontSzMicro
                                                font.letterSpacing: Theme.trackingData
                                                color:              Theme.colorText3
                                                elide:              Text.ElideRight
                                            }
                                        }

                                        PpPressable {
                                            id: goalMa
                                            onClicked: {
                                                root.goalsInteracted = true
                                                var k   = chip.modelData.key
                                                var arr = root.selectedGoals.slice()
                                                var i   = arr.indexOf(k)
                                                if (i === -1) arr.push(k)
                                                else          arr.splice(i, 1)
                                                root.selectedGoals = arr
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ── Panel 1: Cameras ──────────────────────────────────────

                    Item {
                        implicitHeight: camsCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: camsCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(16)

                            // True when at least one enabled, assigned camera is not yet
                            // connected — i.e. there is something for "Connect" to do.
                            // Drives the wizard footer button (Connect ↔ Continue).
                            // Reactive via cameraList: selection AND session-enablement
                            // changes both fire cameraListChanged.
                            readonly property bool canConnect: {
                                var cams = root.sessionCamList
                                for (var i = 0; i < cams.length; ++i)
                                    if (cams[i].sessionEnabled && !cams[i].selected)
                                        return true
                                return false
                            }

                            // Connect every enabled, not-yet-selected session camera, then
                            // start the capture pipelines so the row thumbnails stream
                            // (same path as PpCameraPanel's Connect). Cameras connect
                            // synchronously — no paced queue needed (that's a BLE thing).
                            function startConnect() {
                                var cams = root.sessionCamList
                                for (var i = 0; i < cams.length; ++i)
                                    if (cams[i].sessionEnabled && !cams[i].selected)
                                        cameraManager.setSelected(cams[i].index, true)
                                if (!cameraManager.isRecording && cameraManager.anySelected)
                                    cameraManager.startAll()
                            }

                            // One row per available camera — capture is not limited by
                            // view assignment. Grouped face-on, down-the-line, then
                            // unassigned/other. A placeholder row appears for any
                            // REQUIRED view with no assignment so the "ASSIGN IN
                            // SETTINGS" failure state still shows.
                            readonly property var camRows: {
                                var rows = []
                                var fo = root.faceOnList, dtl = root.dtlList
                                if (fo.length === 0) rows.push({ persp: CameraInstance.FaceOn, data: null })
                                for (var i = 0; i < fo.length; ++i)
                                    rows.push({ persp: CameraInstance.FaceOn, data: fo[i] })
                                if (root.curType.requiredCameras === 2 && dtl.length === 0)
                                    rows.push({ persp: CameraInstance.DownTheLine, data: null })
                                for (var j = 0; j < dtl.length; ++j)
                                    rows.push({ persp: CameraInstance.DownTheLine, data: dtl[j] })
                                var list = cameraManager.cameraList
                                for (var k = 0; k < list.length; ++k)
                                    if (list[k].perspective !== CameraInstance.FaceOn
                                            && list[k].perspective !== CameraInstance.DownTheLine)
                                        rows.push({ persp: list[k].perspective, data: list[k] })
                                return rows
                            }

                            StepIntro {
                                width:   parent.width
                                eyebrow: qsTr("STEP %1 OF %2 · CAMERAS").arg(root.stepNumbers[root.stepCameras]).arg(root.totalSteps)
                                heading: qsTr("Connecting your cameras")
                                body:    root.curType.requiredCameras === 1
                                    ? qsTr("Every connected camera records this session alongside your IMU data. Enable and connect the cameras below — analysis uses the face-on view, so make sure at least one camera is assigned to it.")
                                    : qsTr("Every connected camera records this session. Analysis needs two views — face-on and down the line — to reconstruct your movement in 3D; triangulation follows once both are streaming.")
                            }

                            Column {
                                width: parent.width
                                spacing: 0

                                // Per-camera rows — same pattern as the IMU slot rows:
                                // enable toggle (manager-owned session enablement), live
                                // status chip, and a streaming thumbnail once connected.
                                // One row per assigned camera; perspectives can repeat.
                                Repeater {
                                    model: camsCol.camRows

                                    delegate: CheckRow {
                                        required property var modelData

                                        width: parent.width

                                        // cameraList entry for this row (null = placeholder
                                        // for a required, unassigned view). Reactive:
                                        // cameraListChanged fires on selection and
                                        // session-enablement changes.
                                        readonly property var _data: modelData.data

                                        label: {
                                            if (!_data)
                                                return modelData.persp === CameraInstance.FaceOn
                                                    ? qsTr("Face-on camera") : qsTr("Down-the-line camera")
                                            var name = _data.alias || _data.description
                                            if (modelData.persp === CameraInstance.FaceOn)
                                                return qsTr("Face-on camera — %1").arg(name)
                                            if (modelData.persp === CameraInstance.DownTheLine)
                                                return qsTr("Down-the-line camera — %1").arg(name)
                                            return qsTr("Camera — %1").arg(name)
                                        }

                                        // Live instance for the thumbnail — reactive on
                                        // cameraManager.instances (PpCameraPanel pattern).
                                        readonly property QtObject _inst: {
                                            var insts = cameraManager.instances
                                            if (!_data) return null
                                            for (var i = 0; i < insts.length; ++i)
                                                if (insts[i].cameraKey === _data.cameraKey)
                                                    return insts[i]
                                            return null
                                        }

                                        readonly property bool _enabled:
                                            _data !== null && _data.sessionEnabled
                                        readonly property bool _connected:
                                            _data !== null && _data.selected

                                        // Session-local enable toggle — manager-owned so the
                                        // toolbar panel and video tiles share it. Disabling a
                                        // connected camera also disconnects it (C++ side).
                                        showToggle:    _data !== null
                                        toggleChecked: _enabled
                                        onToggled: (v) => {
                                            if (_data) cameraManager.setSessionCameraEnabled(_data.cameraKey, v)
                                        }

                                        disabled:    _data !== null && !_enabled
                                        subDisabled: _data
                                            ? qsTr("%1 · DISABLED — WON'T CONNECT").arg(_data.alias || _data.description)
                                            : ""

                                        ok:   _enabled && _connected
                                        warn: _enabled && !_connected

                                        // Unassigned cameras record fine — nudge that analysis
                                        // won't use them until a view is assigned ("Other" is
                                        // a deliberate assignment, no nudge).
                                        subOk:   modelData.persp === CameraInstance.None
                                                 ? root.camDetail(_data) + qsTr(" · NO VIEW ASSIGNED")
                                                 : root.camDetail(_data)
                                        subWarn: _data
                                            ? (_data.alias || _data.description) + qsTr(" — PRESS CONNECT")
                                            : ""
                                        subFail: modelData.persp === CameraInstance.FaceOn
                                            ? qsTr("NOT FOUND — ASSIGN FACE-ON PERSPECTIVE IN SETTINGS → CAMERAS")
                                            : qsTr("NOT FOUND — ASSIGN DOWN-THE-LINE PERSPECTIVE IN SETTINGS → CAMERAS")

                                        chipText:      !_enabled ? "" : (_connected ? qsTr("Connected") : "")
                                        chipColor:     Theme.colorGoodLight
                                        chipTextColor: Theme.colorGood

                                        // Streaming thumbnail once the instance exists.
                                        thumbInstance: _inst
                                    }
                                }

                            }


                            // Settings deep-link
                            Text {
                                text:           qsTr("→ Open camera settings")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorAccent
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root.navigateToSettings(root.settingsPanelCameras)
                                }
                            }
                        }
                    }

                    // ── Panel 2: Triangulate ──────────────────────────────────
                    // Only reachable when both camera perspectives are connected
                    // (hasTriangulateStep). Hosts the same stereo-calibration
                    // STUB PpCameraPanel's calibrate mode uses; the real ChArUco
                    // capture flow drops in here when the pipeline lands (TODO).

                    Item {
                        implicitHeight: triangulateCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: triangulateCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(16)

                            StepIntro {
                                width:   parent.width
                                eyebrow: qsTr("STEP %1 OF %2 · TRIANGULATION").arg(root.stepNumbers[root.stepTriangulate] || "").arg(root.totalSteps)
                                heading: qsTr("Triangulating your cameras")
                                body:    qsTr("With a face-on and a down-the-line camera connected, Pinpoint can reconstruct your movement in 3D. Stereo calibration solves where the cameras sit relative to each other; triangulation confirms the result. If your setup hasn't moved since last time, you're good to go.")
                            }

                            Column {
                                width: parent.width
                                spacing: 0

                                CheckRow {
                                    width:        parent.width
                                    ok:           root._todo_stereoCalibrationValid
                                    optional:     root.anyFixedCamera
                                    label:        qsTr("Stereo calibration")
                                    subOk:        qsTr("Calibration valid")
                                    subFail:      root.anyFixedCamera
                                                      ? qsTr("OPTIONAL — CAMERAS ARE FIXED IN PLACE")
                                                      : qsTr("CALIBRATION NEEDED")
                                    showRecal:    true
                                    recalEnabled: root.faceOnList.length > 0 && root.dtlList.length > 0
                                    onRecalibrate: root.cameraRecalibrateRequested()
                                }
                                CheckRow {
                                    width:    parent.width
                                    ok:       root._todo_triangulationValid
                                    optional: root.anyFixedCamera
                                    label:    qsTr("Triangulation")
                                    subOk:    qsTr("Baseline confirmed")
                                    subFail:  root.anyFixedCamera
                                                  ? qsTr("OPTIONAL — CAMERAS ARE FIXED IN PLACE")
                                                  : qsTr("NOT CONFIRMED")
                                }
                            }

                            // Stereo calibration flow — in-panel STUB until the
                            // real pipeline lands; shared with PpCameraPanel.
                            CameraCalibrationFlow {
                                width:  parent.width
                                height: Theme.sp(300)
                                layoutMode: "full"
                                showHeader: false
                            }
                        }
                    }

                    // ── Panel 3: Ball Detection (v2 temporal detector) ────────
                    // Always in the flow (visibleSteps). The user frames the
                    // hitting area, presses Learn to seed the empty-mat baseline
                    // (relearnBallBaseline → Option A), then places a ball; the
                    // step stays live (ballPresent, with a ting) and GATES Continue
                    // on a detected ball. Skip is an escape hatch. No calibration
                    // profile — the temporal detector self-calibrates at runtime.

                    Item {
                        implicitHeight: ballCalCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: ballCalCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(16)

                            StepIntro {
                                width:   parent.width
                                eyebrow: qsTr("STEP %1 OF %2 · BALL DETECTION").arg(root.stepNumbers[root.stepBallCal]).arg(root.totalSteps)
                                heading: qsTr("Set up ball detection")
                                body:    qsTr("Pinpoint learns what your empty hitting area looks like, then detects the ball against it — no calibration needed. Frame the hitting area, empty the mat and press Learn, then place a ball to confirm it's detected.")
                            }

                            // Cameras step skipped / face-on not connected.
                            Text {
                                width: parent.width
                                visible: root.ballFaceOnInst === null
                                text: qsTr("No face-on camera connected — go back to the Cameras step and connect one, or skip ball detection.")
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color: Theme.colorWarn
                            }

                            RowLayout {
                                width: parent.width
                                visible: root.ballFaceOnInst !== null
                                spacing: Theme.sp(20)

                                Loader {
                                    id: ballStepFrame
                                    Layout.preferredWidth: Theme.sp(420)
                                    Layout.preferredHeight: Theme.sp(270)
                                    Layout.alignment: Qt.AlignTop
                                    // Only live while this step is showing — the
                                    // wizard stays instantiated in the StackLayout.
                                    active: root.currentStep === root.stepBallCal
                                            && root.ballFaceOnInst !== null
                                    onLoaded: {
                                        var inst = root.ballFaceOnInst
                                        if (inst && inst.roi.width <= 0)
                                            cameraManager.setBallRoi(inst, Qt.rect(0.40, 0.55, 0.20, 0.30))
                                    }
                                    sourceComponent: PpCameraFrame {
                                        instance: root.ballFaceOnInst
                                        showPoseOverlay:      false
                                        showHittingArea:      true
                                        roiEditable:          true
                                        showPerspectiveBadge: false
                                        showStatsOverlay:     false
                                        showReplayOverlay:    false
                                    }
                                }

                                ColumnLayout {
                                    id: ballStepCol
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    spacing: Theme.sp(12)

                                    // True for ~the seed window after pressing Learn (or a
                                    // ROI drag, which also re-seeds) — shows "Learning…".
                                    property bool learning: false

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("1. Empty the hitting area and position the box over where the ball sits at address — drag to move, corners to resize, drag outside to redraw.\n2. Press Learn.\n3. Place a ball — it's detected below.")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        color: Theme.colorText2
                                    }

                                    // Learn the empty-mat baseline (Option A).
                                    PpButton {
                                        label:   qsTr("Learn hitting area")
                                        primary: true
                                        onClicked: {
                                            ballStepCol.learning = true
                                            learnTimer.restart()
                                            cameraManager.relearnBallBaseline(root.ballFaceOnInst)
                                        }
                                    }
                                    Timer {
                                        id: learnTimer
                                        interval: 2000
                                        onTriggered: ballStepCol.learning = false
                                    }

                                    // Live present/absent badge. The instance tings on the
                                    // present transition (ballPresent), so no QML sound here.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: Theme.sp(4)
                                        spacing: Theme.sp(8)
                                        Rectangle {
                                            width: Theme.sp(11); height: width; radius: width / 2
                                            color: ballStepCol.learning ? Theme.colorWarn
                                                 : root.ballReady        ? Theme.colorGood
                                                                         : Theme.colorText3
                                        }
                                        Text {
                                            text: ballStepCol.learning ? qsTr("Learning the mat…")
                                                : root.ballReady        ? qsTr("Ball detected")
                                                                        : qsTr("No ball — place one in the hitting area")
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzBody
                                            color: root.ballReady ? Theme.colorGood : Theme.colorText2
                                        }
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("Continue is enabled once a ball is detected.")
                                        wrapMode: Text.WordWrap
                                        font.family: Theme.fontData
                                        font.pixelSize: Theme.fontSzMicro
                                        color: Theme.colorText3
                                    }
                                }
                            }
                        }
                    }

                    // ── Panel 4: IMUs ─────────────────────────────────────────

                    Item {
                        implicitHeight: imusCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: imusCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(16)

                            // ── Connect queue state ───────────────────────────
                            property var  _connectQueue: []
                            property int  _connectIdx:   0
                            property bool _connecting:   false

                            // The imuDeviceList row for every device assigned to ANY slot
                            // (required or optional) and enabled this session — DEDUPED by
                            // device id. Phase C unit-keyed placement means a single
                            // HackMotion wG3 answers to BOTH slot A and slot B at once
                            // (deviceIdForSlot("A") === deviceIdForSlot("B")'s device), so
                            // the naive per-slot loop this replaced would enumerate the
                            // same peripheral twice — canConnect and startConnect share
                            // this helper precisely so neither one re-introduces that.
                            function assignedDevices() {
                                var _dep = appSettings.imuPlacement   // deviceIdForSlot() is Q_INVOKABLE, not reactive on its own
                                var reqs = root.curImuReqs
                                var list = imuManager.imuDeviceList
                                var seen = ({})
                                var out  = []
                                for (var i = 0; i < reqs.length; ++i) {
                                    var devId = imuManager.deviceIdForSlot(reqs[i].slot)
                                    if (!devId || seen[devId]) continue
                                    for (var j = 0; j < list.length; ++j) {
                                        if (list[j].id === devId && list[j].sessionEnabled) {
                                            seen[devId] = true
                                            out.push(list[j])
                                            break
                                        }
                                    }
                                }
                                return out
                            }

                            // True when at least one enabled, assigned sensor is not yet
                            // connected — i.e. there is something for "Connect" to do.
                            // Drives the wizard footer button (Connect ↔ Continue).
                            readonly property bool canConnect: {
                                var _dep = imuManager.instances   // reactive
                                if (_connecting) return false
                                var devs = assignedDevices()
                                for (var i = 0; i < devs.length; ++i) {
                                    var inst = imuManager.instanceFor(devs[i].id)
                                    if (!inst || !inst.imuConnected) return true
                                }
                                return false
                            }

                            // Build the list of assigned-but-unconnected device INDICES
                            // (one per PERIPHERAL, thanks to assignedDevices()'s dedup —
                            // queuing the same wG3 twice would connect it twice with a
                            // 2 s gap, a real double-connect, not a cosmetic glitch), then
                            // connect them sequentially with a 2-second gap between each
                            // to give BlueZ time to reset its GATT state.
                            function startConnect() {
                                var devs  = assignedDevices()
                                var queue = []
                                for (var i = 0; i < devs.length; ++i) {
                                    var inst = imuManager.instanceFor(devs[i].id)
                                    if (!inst || !inst.imuConnected) queue.push(devs[i].index)
                                }
                                if (queue.length === 0) return
                                _connectQueue = queue
                                _connectIdx   = 0
                                _connecting   = true
                                imuManager.setSelected(queue[0], true)
                                _connectIdx   = 1
                                if (_connectIdx < queue.length)
                                    imuConnectTimer.start()
                                else
                                    _connecting = false
                            }

                            Timer {
                                id: imuConnectTimer
                                interval: 2000
                                repeat:   false
                                onTriggered: {
                                    var queue = imusCol._connectQueue
                                    var idx   = imusCol._connectIdx
                                    if (idx < queue.length) {
                                        imuManager.setSelected(queue[idx], true)
                                        imusCol._connectIdx = idx + 1
                                        if (imusCol._connectIdx < queue.length)
                                            imuConnectTimer.restart()
                                        else
                                            imusCol._connecting = false
                                    } else {
                                        imusCol._connecting = false
                                    }
                                }
                            }

                            StepIntro {
                                width:   parent.width
                                eyebrow: qsTr("STEP %1 OF %2 · MOTION SENSORS").arg(root.stepNumbers[root.stepImus]).arg(root.totalSteps)
                                heading: qsTr("Attaching your motion sensors")
                                body:    qsTr("Switch each sensor on before stepping up. The positions below are specific to this session type — attach them in the right place and pair each one. Assign placements in Settings → IMUs if you haven't already.")
                            }

                            // ── Scan / Connect header ─────────────────────────
                            RowLayout {
                                width: parent.width
                                spacing: Theme.sp(8)

                                Text {
                                    Layout.fillWidth: true
                                    text: {
                                        var n = imuManager.imuDeviceList.length
                                        return n === 0
                                            ? qsTr("NO DEVICES FOUND")
                                            : n === 1 ? qsTr("1 DEVICE FOUND")
                                                      : qsTr("%1 DEVICES FOUND").arg(n)
                                    }
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingMicro
                                    color: imuManager.imuDeviceList.length > 0
                                               ? Theme.colorText3 : Theme.colorWarn
                                }

                                // Scan button — mirrors ImusPanel.qml exactly
                                Rectangle {
                                    id: imuWizScanBtn
                                    // Bound to the manager's authoritative scan
                                    // state — NOT a local 30 s timer. The window
                                    // is 90 s with HackMotion enabled, and a
                                    // local clock that ran out early re-offered
                                    // a Scan tap that the enumerator's re-entry
                                    // guard silently swallowed.
                                    readonly property bool scanning: imuManager.imuScanActive

                                    implicitWidth:  imuWizScanMeasure.implicitWidth + Theme.sp(24)
                                    implicitHeight: Theme.sp(28)
                                    radius: Theme.radius
                                    color:  scanning ? Theme.colorAccentLight
                                                     : (imuWizScanArea.containsMouse ? Theme.colorBg2
                                                            : Qt.rgba(Theme.colorBg2.r, Theme.colorBg2.g, Theme.colorBg2.b, 0))
                                    border.width: 1
                                    border.color: scanning ? Theme.colorAccent
                                                           : (imuWizScanArea.containsMouse ? Theme.colorAccentMid : Theme.colorBorderStrong)
                                    Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                                    Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                                    Text {
                                        id: imuWizScanMeasure
                                        visible: false
                                        text: qsTr("Scanning…")
                                        font.family: Theme.fontBody; font.pixelSize: Theme.fontSzBody2; font.weight: Theme.fontBodyWeight
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        text:           imuWizScanBtn.scanning ? qsTr("Scanning…") : qsTr("Scan")
                                        font.family:    Theme.fontBody
                                        font.pixelSize: Theme.fontSzBody2
                                        font.weight:    Theme.fontBodyWeight
                                        color:          imuWizScanBtn.scanning ? Theme.colorAccent : Theme.colorText2
                                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                                    }
                                    PpPressable {
                                        id: imuWizScanArea
                                        onClicked: imuManager.rescanImu()
                                    }
                                }

                            }

                            Column {
                                width: parent.width
                                spacing: 0

                                // Per-slot rows driven by the imuRequirements table. A slot is
                                // "found" when imuManager.deviceIdForSlot(slot) resolves to a
                                // device (Phase C unit-keyed placement — see _dev below). One
                                // HackMotion wG3 answers to BOTH slot A and slot B, so this
                                // Repeater renders it as two rows of the SAME device rather
                                // than one filled row and one empty one.
                                Repeater {
                                    model: root.curImuReqs

                                    delegate: CheckRow {
                                        required property var modelData
                                        required property int index

                                        width:    parent.width
                                        optional: !modelData.required
                                        label:    qsTr("IMU %1 — %2").arg(modelData.slot).arg(modelData.placement)

                                        // imuDeviceList row for the device occupying this slot
                                        // (null = unassigned). Resolved via
                                        // imuManager.deviceIdForSlot() — the canonical
                                        // unit-keyed placement resolver (imu_manager.h) — so a
                                        // HackMotion wG3 correctly answers to BOTH slot A and
                                        // slot B from the SAME device id; the two rows must
                                        // both read as filled, not just the anchor slot.
                                        // imuManager.instances is a reactive dependency: any
                                        // imuConnected change fires instancesChanged() which
                                        // re-evaluates these bindings; appSettings.imuPlacement
                                        // is read explicitly too because deviceIdForSlot() is a
                                        // Q_INVOKABLE and not reactive on its own.
                                        readonly property var _dev: {
                                            var _dep  = imuManager.instances
                                            var _dep2 = appSettings.imuPlacement
                                            var devId = imuManager.deviceIdForSlot(modelData.slot)
                                            if (!devId) return null
                                            var list = imuManager.imuDeviceList
                                            for (var i = 0; i < list.length; ++i)
                                                if (list[i].id === devId) return list[i]
                                            return null
                                        }

                                        // Live DEVICE instance. Connect state (stateLabel,
                                        // imuConnected) is a DEVICE property, so this is
                                        // imuManager.deviceForSlot() — NOT instanceForSlot(),
                                        // which for a HackMotion slot would hand back the
                                        // per-unit HmUnit and has no stateLabel/imuConnected of
                                        // its own. Re-evaluated when imuManager.instances
                                        // changes; once non-null, QML tracks _inst.stateLabel
                                        // and _inst.imuConnected directly for fine-grained
                                        // reactivity.
                                        property QtObject _inst: {
                                            var _dep  = imuManager.instances
                                            var _dep2 = appSettings.imuPlacement
                                            return imuManager.deviceForSlot(modelData.slot)
                                        }

                                        // "Lower arm" / "Palm" when this slot is one half of a
                                        // HackMotion wG3 (Phase C unit-keyed placement); "" for
                                        // a Witmotion, whose device IS the segment. Appended to
                                        // the sub-text below so slot A and slot B — which now
                                        // legitimately share one _dev — read as the two units of
                                        // ONE peripheral rather than as a naming collision a
                                        // coach would read as a misconfiguration.
                                        readonly property string _unitLabel:
                                            imuManager.unitLabelForSlot(modelData.slot)

                                        // QML tracks these on _inst directly: stateLabelChanged()
                                        // and imuConnectedChanged() on the instance update them.
                                        readonly property string _stateLabel: _inst ? _inst.stateLabel    : ""
                                        readonly property bool   _connected:  _inst ? _inst.imuConnected  : false

                                        // Excluded = disabled for connection THIS SESSION only.
                                        // Manager-owned (imuManager.sessionImuExcluded) so the
                                        // toolbar IMU panels share it; never written back to
                                        // settings (per-session choice). Reactive via the
                                        // sessionEnabled field on the imuDeviceList entry.
                                        readonly property bool _excluded:
                                            _dev !== null && !_dev.sessionEnabled

                                        disabled:      _excluded
                                        subDisabled:   _dev
                                            ? qsTr("%1 · DISABLED — WON'T CONNECT").arg(_dev.alias || _dev.description)
                                            : qsTr("DISABLED — WON'T CONNECT")
                                        // Toggle shown once a device is assigned to this slot.
                                        showToggle:    _dev !== null
                                        toggleChecked: !_excluded
                                        onToggled: (v) => {
                                            // setSessionImuEnabled is keyed on the DEVICE id, not
                                            // the slot. For a HackMotion, slot A's row and slot
                                            // B's row share one _dev.id, so toggling EITHER
                                            // switch enables/disables the WHOLE peripheral — both
                                            // rows will reflect it (they both read sessionEnabled
                                            // off the same imuDeviceList entry via _dev above).
                                            // That is CORRECT, one physical peripheral, not a
                                            // bug — the two rows are meant to move together.
                                            // Disabling a selected/connected device also
                                            // disconnects it (manager side).
                                            if (_dev) imuManager.setSessionImuEnabled(_dev.id, v)
                                        }

                                        ok:   !_excluded && _connected
                                        warn: !_excluded && _dev !== null && !_connected

                                        // RHS chip: appears as soon as an instance exists and
                                        // updates in real-time as the connection progresses.
                                        // Suppressed while excluded (the dimmed row + toggle say it all).
                                        chipText: {
                                            if (_excluded || !_inst) return ""
                                            if (_connected) return qsTr("Connected")
                                            if (_stateLabel === "Error" || _stateLabel === "Not found")
                                                return qsTr("Connection Failed")
                                            return qsTr("Connecting")
                                        }
                                        chipColor: {
                                            if (!_inst) return "transparent"
                                            if (_connected) return Theme.colorGoodLight
                                            if (_stateLabel === "Error" || _stateLabel === "Not found")
                                                return Theme.colorErrorLight
                                            return Theme.colorWarnLight
                                        }
                                        chipTextColor: {
                                            if (!_inst) return Theme.colorText3
                                            if (_connected) return Theme.colorGood
                                            if (_stateLabel === "Error" || _stateLabel === "Not found")
                                                return Theme.colorError
                                            return Theme.colorWarn
                                        }

                                        subOk: {
                                            if (!_dev) return ""
                                            var parts = [_dev.alias || _dev.description, _dev.transport, _dev.id]
                                                            .filter(function(s){ return s && s !== "" })
                                            if (_unitLabel !== "") parts.push(_unitLabel)
                                            return parts.join(" · ")
                                        }

                                        subWarn: {
                                            if (!_dev) return ""
                                            var suffix = _unitLabel !== "" ? (" (" + _unitLabel + ")") : ""
                                            return (_dev.alias || _dev.description) + suffix + qsTr(" — PRESS CONNECT")
                                        }

                                        subFail: {
                                            var noDevices = imuManager.imuDeviceList.length === 0
                                            if (noDevices)
                                                return modelData.required
                                                    ? qsTr("NO DEVICES FOUND — CLICK SCAN")
                                                    : qsTr("OPTIONAL — NO DEVICES FOUND")
                                            return modelData.required
                                                ? qsTr("NOT ASSIGNED — OPEN SETTINGS → IMUS")
                                                : qsTr("OPTIONAL — NOT ASSIGNED")
                                        }
                                    }
                                }

                            }

                            // Settings deep-link
                            Text {
                                text:           qsTr("→ Open IMU settings")
                                font.family:    Theme.fontBody
                                font.pixelSize: Theme.fontSzBody2
                                color:          Theme.colorAccent
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape:  Qt.PointingHandCursor
                                    onClicked:    root.navigateToSettings(root.settingsPanelImus)
                                }
                            }
                        }
                    }

                    // ── Panel 5: Calibrate Sensors ───────────────────────────
                    // Only reachable for Wrist Motion (hasCalibrateStep).
                    // The panel fills the available height — no Flickable wrapper.

                    Item {
                        // Panel 4 hosts the extracted calibration flow (full
                        // layout). Fill the visible viewport height, gated on the
                        // active step so the full-viewport height does not inflate
                        // the StackLayout on the text-led steps.
                        implicitHeight: root.isVizStep ? Math.max(Theme.sp(480), bodyStack.parent.parent.height) : 0

                        ImuCalibrationFlow {
                            id: calibFlow
                            anchors.fill: parent
                            layoutMode:   "full"
                            showHeader:   true
                            stepLabel:    qsTr("STEP %1 OF %2 · CALIBRATE").arg(root.stepNumbers[root.stepCalibrate] || "").arg(root.totalSteps)
                            // The wizard advances via the footer; nothing required here.
                            onCompleted: {}
                        }
                    }

                    // ── Panel 6: Confirm Tracking ─────────────────────────────
                    // Only reachable for wrist motion sessions (hasCalibrateStep).

                    Item {
                        // Gated on the active step so this panel's full-viewport
                        // height does not inflate the StackLayout on other steps.
                        implicitHeight: root.isVizStep ? Math.max(Theme.sp(480), bodyStack.parent.parent.height) : 0

                        ColumnLayout {
                            anchors.fill:    parent
                            anchors.margins: Theme.sp(8)
                            spacing:         Theme.sp(12)

                            Column {
                                Layout.fillWidth:    true
                                Layout.maximumWidth: root.contentWidth
                                Layout.topMargin:    Theme.sp(32)
                                spacing:             Theme.sp(8)

                                Text {
                                    text:               qsTr("STEP %1 OF %2 · CONFIRM TRACKING").arg(root.stepNumbers[root.stepConfirm] || "").arg(root.totalSteps)
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingMicro
                                    color:              Theme.colorText3
                                }

                                PpDisplayText {
                                    width:          parent.width
                                    text:           qsTr("Check your sensor")
                                    pixelSize:      Math.min(Theme.sp(22), Theme.fontSzDisplay)
                                    wrapMode:       Text.WordWrap
                                }

                                Text {
                                    width:          parent.width
                                    text:           qsTr("Move your arm slowly in all directions. The 3D model should track your movement precisely. If it doesn't, go back and recalibrate.")
                                    font.family:    Theme.fontBody
                                    font.weight:    Theme.fontBodyWeight
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText2
                                    lineHeight:     1.65
                                    wrapMode:       Text.WordWrap
                                }
                            }

                            // Live tracking — the arm avatar is driven entirely by the
                            // calibrated sensors (anatQuat), resolved per slot inside
                            // ArmVizView. Move your arm and confirm the model follows.
                            ArmVizView {
                                id: confirmArmViz
                                Layout.fillWidth:  true
                                Layout.fillHeight: true

                                // ── Live lead-wrist metrics (vs neutral) ──────────────────
                                // Added as a CHILD of the ArmVizView instance — ArmVizView.qml
                                // is never modified. Read-only check; '—' until that slot's
                                // sensors are calibrated (roll needs the optional upper-arm IMU).
                                Rectangle {
                                    anchors { top: parent.top; right: parent.right; margins: Theme.sp(10) }
                                    width:   metricsCol.width  + Theme.sp(20)
                                    height:  metricsCol.height + Theme.sp(14)
                                    radius:  Theme.radius
                                    color:   Qt.alpha(Theme.colorBg2, 0.85)
                                    border.width: 1
                                    border.color: Theme.colorBorderMid

                                    Column {
                                        id: metricsCol
                                        anchors.centerIn: parent
                                        spacing: Theme.sp(6)

                                        Row {
                                            spacing: Theme.sp(10)
                                            Text {
                                                text: qsTr("Bow / cup"); width: Theme.sp(54)
                                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                                                color: Theme.colorText3
                                            }
                                            Text {
                                                text: liveWrist.bowLabel
                                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                                                color: liveWrist.bowValid ? Theme.colorText : Theme.colorText3
                                            }
                                        }
                                        Row {
                                            spacing: Theme.sp(10)
                                            Text {
                                                text: qsTr("Hinge"); width: Theme.sp(54)
                                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                                                color: Theme.colorText3
                                            }
                                            Text {
                                                text: liveWrist.hingeLabel
                                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                                                color: liveWrist.bowValid ? Theme.colorText : Theme.colorText3
                                            }
                                        }
                                        Row {
                                            spacing: Theme.sp(10)
                                            Text {
                                                text: liveWrist.rollTitle; width: Theme.sp(54)
                                                font.family: Theme.fontBody; font.pixelSize: Theme.fontSzLabel
                                                color: Theme.colorText3
                                            }
                                            Text {
                                                text: liveWrist.rollLabel
                                                font.family: Theme.fontData; font.pixelSize: Theme.fontSzBody2
                                                color: liveWrist.rollValid ? Theme.colorText : Theme.colorText3
                                            }
                                        }
                                    }
                                }

                                // Run the live computer only while this step is showing.
                                Binding {
                                    target:   liveWrist
                                    property: "active"
                                    value:    root.currentStep === root.stepConfirm
                                }
                            }

                            // Recalibrate affordance — returns to the Calibrate step and forces
                            // a fresh IMU capture (root._forceImuRecalibrate), used when tracking
                            // looks wrong. Calibration itself happens there, not on this screen.
                            Row {
                                Layout.fillWidth:    true
                                Layout.bottomMargin: Theme.sp(8)
                                spacing:             Theme.sp(6)

                                Text {
                                    text:           qsTr("Not tracking your movement?")
                                    font.family:    Theme.fontBody
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText3
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    id:             recalLink
                                    text:           qsTr("Recalibrate")
                                    font.family:    Theme.fontBody
                                    font.weight:    Theme.fontBodyWeight
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorAccent
                                    anchors.verticalCenter: parent.verticalCenter
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape:  Qt.PointingHandCursor
                                        onClicked: {
                                            var arr = root.stepStates.slice()
                                            arr[root.stepCalibrate] = "pending"
                                            arr[root.stepConfirm]   = "pending"
                                            root.stepStates = arr
                                            root._forceImuRecalibrate = true
                                            root.currentStep = root.stepCalibrate
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ── Panel 7: Ready ────────────────────────────────────────


                    Item {
                        implicitHeight: readyCol.implicitHeight + Theme.sp(32)

                        Column {
                            id: readyCol
                            anchors { left: parent.left; right: parent.right; top: parent.top; topMargin: Theme.sp(32) }
                            spacing: Theme.sp(20)

                            Column {
                                width: parent.width
                                spacing: Theme.sp(8)

                                Text {
                                    text:               qsTr("STEP %1 OF %2 · READY").arg(root.stepNumbers[root.stepReady]).arg(root.totalSteps)
                                    font.family:        Theme.fontData
                                    font.pixelSize:     Theme.fontSzMicro
                                    font.letterSpacing: Theme.trackingMicro
                                    color:              Theme.colorText3
                                }
                                Text {
                                    width:          parent.width
                                    text:           root.fullyReady
                                                        ? qsTr("You're good to go")
                                                        : qsTr("Not quite ready")
                                    font.family:    Theme.fontDisplay
                                    font.italic:    Theme.fontDisplayItalic
                                    font.weight: Theme.fontDisplayWeight
                                    font.pixelSize: Math.min(Theme.sp(22), Theme.fontSzDisplay)
                                    color:          root.fullyReady ? Theme.colorText : Theme.colorWarn
                                    wrapMode:       Text.WordWrap
                                }
                                Text {
                                    width:          parent.width
                                    text:           root.fullyReady
                                        ? qsTr("Everything checked out. The moment you take your address and make your first swing, Pinpoint starts capturing. There's nothing else to press.")
                                        : qsTr("A few things couldn't be confirmed before this session. You can use ← Back to sort them out, or start anyway — Pinpoint will capture what it can, though some analysis may be limited or missing from your results.")
                                    font.family:    Theme.fontBody
                                    font.weight:    Theme.fontBodyWeight
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          Theme.colorText2
                                    wrapMode:       Text.WordWrap
                                    lineHeight:     1.65
                                }
                            }

                            // Issue list — visible only when something is unmet
                            Column {
                                visible: !root.fullyReady
                                width:   parent.width
                                spacing: Theme.sp(6)

                                Repeater {
                                    model: root.readinessIssues
                                    delegate: Row {
                                        required property var modelData
                                        required property int index
                                        width:   parent.width
                                        spacing: Theme.sp(8)

                                        Text {
                                            text:           "⚠"
                                            font.family:    Theme.fontBody
                                            font.pixelSize: Theme.fontSzBody2
                                            color:          Theme.colorWarn
                                            anchors.top:    parent.top
                                            anchors.topMargin: Theme.sp(1)
                                        }

                                        Column {
                                            width:   parent.width - Theme.sp(8) - Theme.sp(16)
                                            spacing: Theme.sp(3)

                                            Text {
                                                width:          parent.width
                                                text:           modelData.text
                                                font.family:    Theme.fontBody
                                                font.weight:    Theme.fontBodyWeight
                                                font.pixelSize: Theme.fontSzBody2
                                                color:          Theme.colorText2
                                                wrapMode:       Text.WordWrap
                                                lineHeight:     1.4
                                            }

                                            Text {
                                                visible:        modelData.panel >= 0
                                                text:           modelData.panel === root.settingsPanelCameras
                                                                    ? qsTr("→ Open camera settings")
                                                                    : qsTr("→ Open IMU settings")
                                                font.family:    Theme.fontBody
                                                font.pixelSize: Theme.fontSzBody2
                                                color:          linkMa.containsMouse
                                                                    ? Theme.colorText : Theme.colorAccent
                                                Behavior on color { ColorAnimation { duration: Theme.durationFast } }

                                                MouseArea {
                                                    id:           linkMa
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape:  Qt.PointingHandCursor
                                                    onClicked:    root.navigateToSettings(modelData.panel)
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Summary table
                            Rectangle {
                                width: parent.width
                                height: summaryRows.implicitHeight
                                radius: Theme.radius
                                color:  "transparent"
                                border.width: 1
                                border.color: Theme.colorBorderMid

                                Column {
                                    id: summaryRows
                                    width: parent.width

                                    SummaryRow {
                                        width: parent.width
                                        rowLabel: qsTr("Goals")
                                        rowValue: root.selectedGoals.length > 0
                                                      ? qsTr("%1 selected · saved to profile").arg(root.selectedGoals.length)
                                                      : qsTr("None — defaulting to %1").arg(root.curGoalDefs[0].name)
                                        good: true
                                    }
                                    Rectangle { width: parent.width; height: 1; color: Theme.colorBorderMid }
                                    SummaryRow {
                                        width: parent.width
                                        rowLabel: qsTr("Cameras")
                                        good: root.stepStates[root.stepCameras] !== "skipped" && root.camsAllConnected
                                        rowValue: {
                                            if (root.stepStates[root.stepCameras] === "skipped")
                                                return qsTr("Skipped — no video capture")
                                            if (root.camsAllConnected) {
                                                var cams = root.sessionCamList, n = 0
                                                for (var i = 0; i < cams.length; ++i)
                                                    if (cams[i].sessionEnabled && cams[i].selected) ++n
                                                return n === 1 ? qsTr("1 camera connected")
                                                               : qsTr("%1 cameras connected").arg(n)
                                            }
                                            if (root.faceOnList.length === 0)
                                                return root.curType.requiredCameras === 2 && root.dtlList.length === 0
                                                    ? qsTr("Neither camera view assigned")
                                                    : qsTr("Face-on camera not assigned")
                                            if (root.curType.requiredCameras === 2 && root.dtlList.length === 0)
                                                return qsTr("Down-the-line camera not assigned")
                                            return qsTr("Not connected — press Connect in the cameras step")
                                        }
                                    }
                                    Rectangle {
                                        visible: root.hasTriangulateStep
                                        width: parent.width; height: 1; color: Theme.colorBorderMid
                                    }
                                    SummaryRow {
                                        visible: root.hasTriangulateStep
                                        width: parent.width
                                        rowLabel: qsTr("Triangulation")
                                        good: root._todo_triangulationValid || root.anyFixedCamera
                                        rowValue: root._todo_triangulationValid
                                                      ? qsTr("Baseline confirmed")
                                                      : root.anyFixedCamera
                                                          ? qsTr("Optional — cameras fixed in place")
                                                          : qsTr("Not confirmed")
                                    }
                                    Rectangle { width: parent.width; height: 1; color: Theme.colorBorderMid }
                                    SummaryRow {
                                        width: parent.width
                                        rowLabel: qsTr("IMUs")
                                        good: root.stepStates[root.stepImus] !== "skipped" && root.imusOk
                                        rowValue: root.stepStates[root.stepImus] === "skipped"
                                                      ? qsTr("Skipped — no motion data")
                                                      : root.imusOk
                                                          ? qsTr("%1 sensors assigned").arg(root.curImuReqs.filter(function(r){ return r.required }).length)
                                                          : qsTr("Some sensors not assigned")
                                    }
                                    Rectangle { width: parent.width; height: 1; color: Theme.colorBorderMid }
                                    SummaryRow {
                                        width: parent.width
                                        rowLabel: qsTr("Ball detection")
                                        good: root.ballReady
                                        rowValue: root.ballReady
                                                      ? qsTr("Ball detected")
                                                      : root.stepStates[root.stepBallCal] === "skipped"
                                                          ? qsTr("Skipped")
                                                          : qsTr("No ball detected")
                                    }
                                }
                            }

                            // Notice box
                            Rectangle {
                                width: parent.width
                                height: noticeText.implicitHeight + Theme.sp(20)
                                radius: Theme.radius
                                color:  root.fullyReady ? Theme.colorGoodLight : Theme.colorWarnLight
                                border.width: 1
                                border.color: root.fullyReady ? Theme.colorGood : Theme.colorWarn

                                Text {
                                    id: noticeText
                                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: Theme.sp(12) }
                                    text: root.fullyReady
                                              ? qsTr("Everything's set up and ready to go. Step up when you like — Pinpoint will start capturing the moment you take your address.")
                                              : qsTr("Starting with an incomplete setup is fine — partial data is often still useful. For the full picture though, it's worth coming back once the hardware is sorted. Your results will thank you for it.")
                                    font.family:    Theme.fontBody
                                    font.weight:    Theme.fontBodyWeight
                                    font.pixelSize: Theme.fontSzBody2
                                    color:          root.fullyReady ? Theme.colorGood : Theme.colorWarn
                                    wrapMode:       Text.WordWrap
                                    lineHeight:     1.5
                                }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.colorBorderMid }

        // ── Footer ────────────────────────────────────────────────────────────

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.sp(64)

            Item {
                anchors.horizontalCenter: parent.horizontalCenter
                width:  root.contentWidth
                height: parent.height

                RowLayout {
                    anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
                    spacing: Theme.sp(8)

                    // Hint text
                    Text {
                        Layout.fillWidth: true
                        text: {
                            var s = root.currentStep
                            if (s === root.stepGoals) {
                                var n = root.selectedGoals.length
                                if (n === 0) return qsTr("No goals selected — defaulting to %1").arg(root.curGoalDefs[0].name)
                                return n === 1 ? qsTr("1 goal selected") : qsTr("%1 goals selected").arg(n)
                            }
                            if (s === root.stepCameras) {
                                if (root.camsAllConnected)
                                    return qsTr("All cameras connected")
                                if (camsCol.canConnect)
                                    return qsTr("Cameras assigned — tap Connect to start them")
                                return root.faceOnList.length === 0 ? qsTr("Face-on camera not assigned")
                                     : (root.curType.requiredCameras === 2 && root.dtlList.length === 0)
                                           ? qsTr("Down-the-line camera not assigned")
                                           : qsTr("Camera disabled — enable it to capture video this session")
                            }
                            if (s === root.stepTriangulate) return root._todo_triangulationValid
                                ? qsTr("Triangulation confirmed")
                                : root.anyFixedCamera
                                    ? qsTr("Optional — cameras are fixed in place")
                                    : qsTr("Stereo calibration isn't available yet — skip to continue")
                            if (s === root.stepBallCal) return root.ballReady
                                ? qsTr("Ball detected")
                                : qsTr("Learn the hitting area, then place a ball — or skip ball detection")
                            if (s === root.stepImus) return root.imusAllConnected
                                ? qsTr("All required sensors connected")
                                : root.imusOk
                                    ? qsTr("All required sensors assigned — tap Connect to pair them")
                                    : qsTr("Some sensors not assigned — or skip to start without motion data")
                            if (s === root.stepCalibrate) return calibFlow.calibrationDone
                                ? qsTr("Calibration complete")
                                : qsTr("Follow the guide to calibrate your sensors — or skip to continue without calibration")
                            return ""
                        }
                        font.family:        Theme.fontData
                        font.pixelSize:     Theme.fontSzMicro
                        font.letterSpacing: Theme.trackingData
                        color: {
                            var s = root.currentStep
                            if (s === root.stepCameras) return root.camsAllConnected ? Theme.colorGood : Theme.colorWarn
                            if (s === root.stepTriangulate)
                                return (root._todo_triangulationValid || root.anyFixedCamera)
                                           ? Theme.colorGood : Theme.colorWarn
                            if (s === root.stepBallCal) return root.ballReady ? Theme.colorGood : Theme.colorWarn
                            if (s === root.stepImus)    return root.imusAllConnected ? Theme.colorGood : Theme.colorWarn
                            if (s === root.stepCalibrate) return calibFlow.calibrationDone ? Theme.colorGood : Theme.colorWarn
                            return Theme.colorText3
                        }
                        elide: Text.ElideRight
                    }

                    // ← Back
                    PpButton {
                        visible: root.currentStep > 0
                        label:   qsTr("← Back")
                        primary: false
                        onClicked: root.goBack()
                    }

                    // Skip →
                    PpButton {
                        visible: (root.currentStep === root.stepCameras && !root.camsAllConnected)
                                 || (root.currentStep === root.stepTriangulate && !root._todo_triangulationValid)
                                 || (root.currentStep === root.stepBallCal && !root.ballReady)
                                 || (root.currentStep === root.stepImus && !root.imusAllConnected)
                                 || (root.currentStep === root.stepCalibrate && !calibFlow.calibrationDone)
                        label:   qsTr("Skip →")
                        primary: false
                        onClicked: root.goNext("skipped")
                    }

                    // Primary action
                    Rectangle {
                        id: primaryBtn
                        // IMU step: the primary button doubles as "Connect" until every
                        // ENABLED sensor is connected, then becomes "Continue". Keeps the
                        // bottom-right button the single "keep progressing" control.
                        // "Ready to continue" = required slots all connected AND nothing
                        // enabled left to connect (imusCol.canConnect false). Using
                        // canConnect — not just imusAllConnected — means re-enabling an
                        // optional sensor mid-page correctly reverts the button to "Connect".
                        readonly property bool imuConnectMode:
                            root.currentStep === root.stepImus
                            && !(root.imusAllConnected && !imusCol.canConnect)

                        // Cameras step: same Connect ↔ Continue pattern. Cameras
                        // connect synchronously, so there is no "Connecting…"
                        // phase — the mode is simply "something left to connect".
                        readonly property bool camConnectMode:
                            root.currentStep === root.stepCameras && camsCol.canConnect

                        implicitWidth:  primaryLbl.implicitWidth + Theme.sp(28)
                        implicitHeight: Theme.sp(38)
                        radius: Theme.radius
                        readonly property color _primaryBase: root.currentStep < root.lastStep
                                   ? Theme.colorAccent
                                   : (root.fullyReady ? Theme.colorGood : Theme.colorWarn)
                        color: primaryArea.containsMouse ? Qt.lighter(_primaryBase, 1.08) : _primaryBase
                        Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                        // Dim only when there's nothing actionable: Calibrate not done, or
                        // IMU connect-mode with nothing left to connect (and not mid-connect).
                        // Uses primaryArea.pressed for press feedback — imperative opacity
                        // assignments would destroy this binding on first press.
                        opacity: {
                            var blocked = (root.currentStep === root.stepCalibrate && !calibFlow.calibrationDone)
                                       || (root.currentStep === root.stepBallCal && !root.ballReady)
                                       || (primaryBtn.imuConnectMode && !imusCol.canConnect && !imusCol._connecting)
                            if (blocked) return 0.4
                            return primaryArea.pressed ? 0.8 : 1.0
                        }
                        Behavior on opacity { NumberAnimation { duration: Theme.durationNormal } }

                        Text {
                            id: primaryLbl
                            anchors.centerIn: parent
                            text: primaryBtn.camConnectMode
                                      ? qsTr("Connect")
                                      : primaryBtn.imuConnectMode
                                      ? (imusCol._connecting ? qsTr("Connecting…") : qsTr("Connect"))
                                      : (root.currentStep < root.lastStep
                                            ? qsTr("Continue →")
                                            : (root.fullyReady ? qsTr("▶  Start session") : qsTr("▶  Start anyway")))
                            font.family:    Theme.fontBody
                            font.pixelSize: Theme.fontSzBody
                            color: Theme.dark ? Theme.colorBg : "#FFFFFF"
                        }

                        PpPressable {
                            id: primaryArea
                            hoverScale: 1.0   // full-width CTA — keep press-dip only
                            onClicked: {
                                // Camera connect-mode: connect instead of advancing.
                                if (primaryBtn.camConnectMode) {
                                    camsCol.startConnect()
                                    return
                                }
                                // IMU connect-mode: trigger connection instead of advancing.
                                if (primaryBtn.imuConnectMode) {
                                    if (!imusCol._connecting && imusCol.canConnect) imusCol.startConnect()
                                    return
                                }
                                if (root.currentStep < root.lastStep) {
                                    // goNext gates an incomplete Calibrate step internally.
                                    root.goNext("done")
                                } else if (!root.curType.comingSoon) {
                                    // Defensive: Swing/GRF/Coach are not startable yet
                                    // (the home picker already blocks reaching here).
                                    var goals = root.selectedGoals.length > 0
                                                    ? root.selectedGoals
                                                    : [root.curGoalDefs[0].key]
                                    root.sessionStartRequested(root.sessionType, goals)
                                }
                            }
                        }

                        // Traveling-light frame while the CTA is connecting devices.
                        // Cameras connect synchronously (brief), IMUs span the paced
                        // BLE queue + each sensor's connect window. Light hue to read
                        // against the filled accent/good/warn background.
                        PpConnectingFrame {
                            anchors.fill: parent
                            radius:  parent.radius
                            color:   Theme.dark ? Theme.colorBg : "#FFFFFF"
                            running: (root.currentStep === root.stepCameras && cameraManager.anyConnecting)
                                  || (root.currentStep === root.stepImus
                                      && (imusCol._connecting || imuManager.anyConnecting))
                        }
                    }
                }
            }
        }
    }

    // ── Inline helper components ──────────────────────────────────────────────

    component StepIntro: Column {
        property string eyebrow: ""
        property string heading: ""
        property string body:    ""
        spacing: Theme.sp(8)

        Text {
            text:               eyebrow
            font.family:        Theme.fontData
            font.pixelSize:     Theme.fontSzMicro
            font.letterSpacing: Theme.trackingMicro
            color:              Theme.colorText3
        }
        PpDisplayText {
            width:          parent.width
            text:           heading
            pixelSize:      Math.min(Theme.sp(20), Theme.fontSzDisplay)
            wrapMode:       Text.WordWrap
        }
        Text {
            width:          parent.width
            text:           body
            font.family:    Theme.fontBody
            font.weight:    Theme.fontBodyWeight
            font.pixelSize: Theme.fontSzBody2
            color:          Theme.colorText2
            wrapMode:       Text.WordWrap
            lineHeight:     1.65
        }
        Rectangle {
            width: parent.width; height: 1
            color: Theme.colorBorderMid
        }
    }

    // Small on/off toggle — mirrors the TogglePill in ImusPanel.qml so the
    // wizard's per-IMU enable switch looks and behaves identically to Settings.
    component TogglePill: Rectangle {
        id: pill
        property bool checked: false
        signal toggled(bool value)

        width:  Theme.sp(34)
        height: Theme.sp(18)
        radius: Theme.sp(9)
        color:  pill.checked ? Theme.colorAccent : Theme.colorBg3
        Behavior on color { ColorAnimation { duration: Theme.durationFast } }

        Rectangle {
            width:  Theme.sp(12)
            height: Theme.sp(12)
            radius: Theme.sp(6)
            color:  "white"
            anchors.verticalCenter: parent.verticalCenter
            x: pill.checked ? parent.width - width - Theme.sp(3) : Theme.sp(3)
            Behavior on x { NumberAnimation { duration: 120 } }
        }

        MouseArea {
            anchors.fill: parent
            cursorShape:  Qt.PointingHandCursor
            onClicked:    pill.toggled(!pill.checked)
        }
    }

    component StatusCircle: Rectangle {
        property bool ok:    false
        property bool warn:  false   // amber — found but not ready (e.g. assigned but not connected)
        property bool error: false   // red   — hard failure (not found / not assigned)
        width: Theme.sp(17); height: Theme.sp(17); radius: Theme.sp(9)
        color:        ok    ? Theme.colorGoodLight  :
                      warn  ? Theme.colorWarnLight  :
                      error ? Theme.colorErrorLight : "transparent"
        border.width: 1
        border.color: ok    ? Theme.colorGood  :
                      warn  ? Theme.colorWarn  :
                      error ? Theme.colorError : Theme.colorBorderMid
        Text {
            anchors.centerIn: parent
            visible:        parent.ok
            text:           "✓"
            font.pixelSize: Theme.sp(10)
            color:          Theme.colorGood
        }
    }

    component CheckRow: Item {
        id: cr
        property bool   ok:            false
        property bool   warn:          false  // amber — found but not connected/ready
        property bool   optional:      false  // true = muted colour when hard-failing
        property string label:         ""
        property string subOk:         ""
        property string subWarn:       ""    // shown when warn && !ok
        property string subFail:       ""
        property bool   showRecal:     false
        property bool   recalEnabled:  false
        // Disabled state — muted/greyed, neutral circle, shows subDisabled.
        property bool   disabled:      false
        property string subDisabled:   ""
        // Optional on/off toggle on the right (e.g. enable an IMU for connection).
        property bool   showToggle:    false
        property bool   toggleChecked: true
        signal toggled(bool value)
        // RHS status chip — shown when chipText is non-empty
        property string chipText:      ""
        property color  chipColor:     "transparent"
        property color  chipTextColor: Theme.colorText3
        signal recalibrate()
        // Live camera thumbnail (wizard camera rows) — the row grows to fit it.
        property QtObject thumbInstance: null

        height: thumbInstance !== null ? Theme.sp(76) : Theme.sp(52)
        Behavior on height { NumberAnimation { duration: Theme.durationFast } }

        Rectangle {
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right }
            height: 1; color: Theme.colorBorder
        }

        RowLayout {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
            spacing: Theme.sp(10)

            StatusCircle {
                opacity: cr.disabled ? 0.45 : 1.0
                ok:    !cr.disabled && cr.ok
                warn:  !cr.disabled && !cr.ok && cr.warn
                error: !cr.disabled && !cr.ok && !cr.warn && !cr.optional
            }

            Column {
                Layout.fillWidth: true
                spacing: Theme.sp(2)
                opacity: cr.disabled ? 0.45 : 1.0
                Text {
                    text:           cr.label
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzBody2
                    color:          Theme.colorText
                }
                Text {
                    width:              parent.width
                    text:               cr.disabled ? cr.subDisabled
                                                     : (cr.ok ? cr.subOk : (cr.warn ? cr.subWarn : cr.subFail))
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData
                    color:              cr.disabled ? Theme.colorText3
                                               : (cr.ok   ? Theme.colorGood
                                               : (cr.warn ? Theme.colorWarn
                                                          : (cr.optional ? Theme.colorText3
                                                                         : Theme.colorError)))
                    elide:              Text.ElideRight
                }
            }

            // Live thumbnail preview — streams as soon as the camera connects.
            // All overlays off; PpCameraFrame handles the RGB/Bayer split and
            // the subscribe/unsubscribe lifecycle. Loader-gated so the many
            // non-camera CheckRows don't each carry a video item.
            Loader {
                active:  cr.thumbInstance !== null
                visible: active
                Layout.preferredWidth:  Theme.sp(108)
                Layout.preferredHeight: Theme.sp(62)
                Layout.alignment:       Qt.AlignVCenter
                sourceComponent: PpCameraFrame {
                    instance: cr.thumbInstance
                    showPoseOverlay:      false
                    showHittingArea:      false
                    showPerspectiveBadge: false
                    showStatsOverlay:     false
                }
            }

            // Per-row enable toggle (e.g. include/exclude an IMU for connection).
            Row {
                visible:          cr.showToggle
                spacing:          Theme.sp(6)
                Layout.alignment: Qt.AlignVCenter
                Text {
                    text:           qsTr("Enable")
                    font.family:    Theme.fontData
                    font.pixelSize: Theme.fontSzMicro
                    color:          Theme.colorText3
                    anchors.verticalCenter: parent.verticalCenter
                }
                TogglePill {
                    checked:                cr.toggleChecked
                    anchors.verticalCenter: parent.verticalCenter
                    onToggled:              (v) => cr.toggled(v)
                }
            }

            PpButton {
                visible:   cr.showRecal
                label:     qsTr("Recalibrate")
                primary:   false
                enabled:   cr.recalEnabled
                onClicked: cr.recalibrate()
            }

            // Status chip — e.g. "Connecting", "Connection Failed", "Connected"
            Rectangle {
                visible:        cr.chipText !== ""
                implicitWidth:  chipLbl.implicitWidth + Theme.sp(14)
                implicitHeight: Theme.sp(20)
                radius:         Theme.sp(4)
                color:          cr.chipColor
                border.width:   1
                border.color:   cr.chipTextColor
                Behavior on color        { ColorAnimation { duration: Theme.durationFast } }
                Behavior on border.color { ColorAnimation { duration: Theme.durationFast } }

                Text {
                    id: chipLbl
                    anchors.centerIn:   parent
                    text:               cr.chipText
                    font.family:        Theme.fontData
                    font.pixelSize:     Theme.fontSzMicro
                    font.letterSpacing: Theme.trackingData
                    color:              cr.chipTextColor
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }
            }
        }
    }

    component SummaryRow: Item {
        id: sr
        property string rowLabel: ""
        property string rowValue: ""
        property bool   good:     true
        height: Theme.sp(44)

        RowLayout {
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: Theme.sp(14) }
            spacing: Theme.sp(10)

            Rectangle {
                width: Theme.sp(6); height: Theme.sp(6); radius: Theme.sp(3)
                color: sr.good ? Theme.colorGood : Theme.colorWarn
            }
            Text {
                Layout.fillWidth: true
                text:           sr.rowLabel
                font.family:    Theme.fontBody
                font.pixelSize: Theme.fontSzBody2
                color:          Theme.colorText
            }
            Text {
                text:               sr.rowValue
                font.family:        Theme.fontData
                font.pixelSize:     Theme.fontSzMicro
                font.letterSpacing: Theme.trackingData
                color:              sr.good ? Theme.colorGood : Theme.colorWarn
            }
        }
    }
}
