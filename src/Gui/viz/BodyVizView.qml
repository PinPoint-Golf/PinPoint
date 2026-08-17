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
import QtQuick3D
import QtQuick3D.Helpers
import QtQuick3D.AssetUtils
import PinPointStudio

// BodyVizView — full Y-bot body assembled from individual bone-local GLBs.
//
// Each segment is extracted from ybot.glb by tools/extract_body_segments.py,
// which uses the GLTF inverse-bind matrices to place the joint at the origin
// of each segment's local coordinate system.
//
// Kinematic chain (all positions are parent-local, rotations are rest-pose):
//
//   hipsNode       ← pelvis, world y≈1.0
//     spineNode      (0, 0.099, -0.012)
//       spine1Node   (0, 0.117, 0)
//         spine2Node (0, 0.135, 0)
//           neckNode   (0, 0.150, 0.009)  [no mesh — no distinct Y-bot neck]
//             headNode (0, 0.103, 0.031)
//           leftShoulderNode  (+0.061, 0.091, 0.008)
//             leftArmNode     (0, 0.129, 0)
//               leftForeArmNode  (0, 0.274, 0)
//                 leftHandNode   (0, 0.276, 0)
//           rightShoulderNode (mirror)
//     leftUpLegNode  (+0.091, -0.067, 0)
//       leftLegNode  (0, 0.406, 0)   [+Y is bone-local down — UpLeg is 180° around Z]
//         leftFootNode (0, 0.421, 0)
//     rightUpLegNode (mirror)
//
// Rest-pose quaternions are the local-space rotations read from the bind matrices
// (see extract_body_segments.py diagnostic output for derivation).
// Near-identity rotations (<0.1° deviation) are left at default (identity) to
// avoid unnecessary computation.
//
// Phase 2 will add:
//   property QtObject poseSource  — drives bone rotations from a CameraInstance
//   BodyPoseAdapter               — 2D keypoints → per-bone quaternions
//   60 Hz Timer                   — animates rotations via slerp

Item {
    id: root

    // Face-on CameraInstance — drives per-bone animation when non-null.
    property QtObject poseSource: null

    // true when the camera delivers a horizontally mirrored image (typical webcam).
    // false for industrial cameras that deliver non-mirrored frames.
    property bool mirroredSource: true

    // BodyPoseAdapter converts keypoints → slerped per-bone quaternions at 60 Hz.
    BodyPoseAdapter {
        id: adapter
        poseSource:     root.poseSource
        mirroredSource: root.mirroredSource
    }

    // Set true to show a small sphere at every joint pivot — useful for
    // verifying that parent-local offsets are correct.
    property bool showJoints: false

    // ── Arm override properties — used by the calibration wizard step ─────────
    // When false (default), arm rotations are driven by the BodyPoseAdapter.
    // When true, the lead/trail arm rotations are overridden directly so the
    // calibration screen can pose the body independently of camera input.

    // true = left arm is lead arm (right-handed golfer), false = right arm is lead
    property bool rightHanded: true

    // Highlight the lead arm with a coloured overlay — used to indicate which
    // arm the user should position during calibration.
    property bool  highlightLeadArm: false
    property color leadArmColor:     "#5B9BD5"

    // Sensor-mount orientation markers on the lead-arm segments — a bright ball on
    // each bone's local +Z face, indicating where the IMU is mounted and exposing
    // bone roll. Mirrors ArmVizView's OrientationTab; sized 50% smaller here.
    property bool showOrientationTabs: true
    component OrientationTab: Node {
        id: tabRoot
        property real  along:    0.20
        property color tabColor: Theme.colorImuA
        property real  tabScale: 0.0003          // 50% of ArmVizView's 0.0006
        property vector3d tabDir: Qt.vector3d(0, 0, -1)   // -Z = back/outer face = sensor mount
        visible: root.showOrientationTabs && root.highlightLeadArm && root.rightHanded
        y: along
        Model {
            source:    "#Sphere"
            scale:     Qt.vector3d(tabRoot.tabScale, tabRoot.tabScale, tabRoot.tabScale)
            position:  Qt.vector3d(tabRoot.tabDir.x * 0.05, tabRoot.tabDir.y * 0.05, tabRoot.tabDir.z * 0.05)
            materials: PrincipledMaterial {
                baseColor: tabRoot.tabColor
                lighting:  PrincipledMaterial.NoLighting
            }
        }
    }

    // Lead arm override (calibration phase 0 = arm-down, phase 2 = T-pose guide)
    property bool       useLeadArmOverride:          false
    property quaternion leadArmOverrideRotation:     Qt.quaternion(1, 0, 0, 0)
    property quaternion leadForeArmOverrideRotation: Qt.quaternion(1, 0, 0, 0)

    // Trail arm override — keeps the trail arm in a natural relaxed hang
    // regardless of what the BodyPoseAdapter would otherwise compute.
    property bool       useTrailArmOverride:          false
    property quaternion trailArmOverrideRotation:     Qt.quaternion(1, 0, 0, 0)
    property quaternion trailForeArmOverrideRotation: Qt.quaternion(1, 0, 0, 0)

    // ── Device-native (HackMotion) calibration guide poses ────────────────────
    // The wG3 computes its own calibration ON-DEVICE from a two-pose routine
    // (libhackmotion specification.md §8.2), performed as ONE CONTINUOUS motion
    // because the device watches the whole travel rather than sampling two static
    // poses:
    //
    //   pose 0  upper arm hanging at the side but FLEXED FORWARD, elbow bent, the
    //           forearm horizontal and STRAIGHT ACROSS the body, palm DOWN
    //   pose 1  the forearm ELEVATED hmCalRaiseDeg, THE ELBOW STAYING PUT
    //
    // ⚠ THE RAISE MOVES THE FOREARM NODE, NOT THE UPPER ARM, and that is the whole
    // reason this file animates the forearm override at all. "Elbow in the same
    // position" fixes the upper arm: the elbow sits at the end of the upper-arm
    // bone, so ANY upper-arm rotation other than one about its own long axis moves
    // it. The upper-arm quaternion is therefore IDENTICAL in both poses and only
    // leadForeArmOverrideRotation changes — which is why _leadForeArmFrom/_To and
    // the forearm's own onChanged handler exist further down. ⚠ Without that
    // handler this guide would not animate AT ALL: the existing trigger is a change
    // in the upper-arm override, and here there isn't one.
    //
    // ⚠ WHY THE UPPER ARM IS FLEXED FORWARD, AND WHY THE FOREARM IS NOT. A forearm
    // held straight across a body whose upper arm hangs vertically passes THROUGH
    // the torso — the elbow sits at z ≈ -0.06, behind the belly's front surface.
    // Two ways out, and only one of them is the routine: tilt the FOREARM forward
    // (it then reads as pointing out diagonally, which is not "across the body"), or
    // flex the UPPER ARM forward so the elbow comes out in front and the forearm can
    // lie flat across the chest. The second is what a person actually does, so it is
    // what the guide does. hmCalUpperArmFwdDeg is the amount.
    //
    // ── DERIVATION ────────────────────────────────────────────────────────────
    // Built with the SAME composition the pose adapter already uses, rather than a
    // fresh one: body_pose_adapter.cpp:41-63 pre-bakes the constant parent chain
    // from this file's own rest quaternions, and body_pose_adapter.cpp:216 sets a
    // forearm from  m_tLeftForeArm = lArmW.conjugated() * faW  — i.e.
    //
    //       forearm_local = conj(upperArm_world) ⊗ forearm_world
    //
    // with  upperArm_world = spineRest ⊗ spine2Rest ⊗ shoulderRest ⊗ upperArm_local.
    // That chain reproduces the live node hierarchy's own sceneRotation to 1e-6, so
    // the constants here and the running scene agree.
    //
    // The upper arm is the hanging-down rotation pre-rotated in WORLD space about
    // +X by -hmCalUpperArmFwdDeg, which carries the bone from (0,-1,0) toward +Z
    // (forward) while leaving its roll relationship intact; the local quaternion is
    // then conj(parent) ⊗ that. forearm_world is the frame whose columns are the
    // bone's own axes: +Y the bone direction, -Z the DORSAL face (the side
    // OrientationTab marks and the wG3 sits on), +X = Y × Z. Pose 0 wants bone +Y
    // straight across the body — exactly (-1,0,0) for a left lead arm, no forward
    // component — with dorsal +Y (up), which is palm DOWN. Pose 1 rotates both by
    // hmCalRaiseDeg in the vertical plane containing the bone, so the palm stays
    // down relative to the forearm through the whole travel.
    //
    // ⚠ Checked against the live chain, not by eye. Shoulder at (0.188, 1.436,
    // -0.062); the flexed upper arm puts the elbow at (0.182, 1.226, 0.114) — in
    // FRONT of the torso. Pose 0's wrist is (-0.094, 1.226, 0.114): identical y and
    // z to the elbow, so the forearm is exactly horizontal and exactly across, and
    // it crosses the midline by 0.094. Pose 1 lifts the wrist to (-0.057, 1.364,
    // 0.114) — elbow unmoved, z unmoved, bone direction exactly hmCalRaiseDeg
    // higher, dorsal marker still above the palmar one.
    //
    // ⚠ 30° IS THE ROUTINE, NOT A MINIMUM. §8.2 separated a full ~30° travel
    // cleanly in the 0x94 payload (28.9° palm / 30.9° arm) and could not separate
    // a 4° one at all — but a BIGGER one is not "safer": it is a different routine
    // from the one the device's own maths expects.
    readonly property real hmCalRaiseDeg: 30
    // ⚠ A PRESENTATION CONSTANT, SET BY RENDERING, NOT FROM THE SPEC. Rendered at
    // 30/40/50°: at 30° the elbow is only at z ≈ 0.075 and the forearm still grazes
    // the belly; 40° puts it clearly in front with the pose still reading as a
    // relaxed arm. The device measures the RAISE and needs pose 0 only to be a pose
    // the athlete can repeat, so this is ours to choose for legibility.
    readonly property real hmCalUpperArmFwdDeg: 40

    // Upper arm — THE SAME IN BOTH POSES (see the ⚠ above), and unlike the
    // Witmotion flow's leadArmDownQuat this one IS handed: the forward flexion puts
    // non-zero y and z in it, so the (w,x,y,z) → (w,x,-y,-z) mirror is no longer a
    // no-op the way it is for a purely vertical hang.
    readonly property quaternion hmCalUpperArmQuat: root.rightHanded
        ? Qt.quaternion(0.6158076, 0.7131185, -0.2317729,  0.2419182)   // left arm  (lead when right-handed)
        : Qt.quaternion(0.6158076, 0.7131185,  0.2317729, -0.2419182)   // right arm (y,z mirrored)

    // Forearm, pose 0 — straight across the body, palm down.
    readonly property quaternion hmCalForeArmPose0Quat: root.rightHanded
        ? Qt.quaternion(0.1753510, 0.3046825,  0.6926323,  0.6298263)
        : Qt.quaternion(0.1753510, 0.3046825, -0.6926323, -0.6298263)

    // Forearm, pose 1 — elevated hmCalRaiseDeg, elbow unmoved.
    // ⚠ Baked rather than composed from hmCalRaiseDeg at runtime, because it depends
    // on hmCalUpperArmFwdDeg too (the forearm's local frame is relative to a flexed
    // upper arm); a "simplification" that rebuilt it from one angle would quietly
    // change the axis. Both constants are recorded above and the derivation is
    // reproducible from them.
    readonly property quaternion hmCalForeArmPose1Quat: root.rightHanded
        ? Qt.quaternion(0.2482337, 0.2489165,  0.5060204,  0.7876319)
        : Qt.quaternion(0.2482337, 0.2489165, -0.5060204, -0.7876319)

    // The HackMotion routine happens in a near-FRONTAL plane — the forearm lies
    // across the chest and elevates in place — so the default view DIRECTION
    // (face-on, down -Z) is the right one for it and no angle change is wanted.
    // What the default camera does lack is size: it frames a whole 1.8 m body from
    // 3.5 m, which leaves the forearm a few dozen pixels and its 30° lift hard to
    // read. So the guide camera keeps the direction and only moves closer.
    //
    // ⚠ CHECKED BY RENDERING, NOT ASSUMED. Both poses were rendered offscreen from
    // the app default, from this zoomed face-on, and from several 3/4 and overhead
    // angles; face-on is where "forearm horizontal across the chest" and "forearm
    // raised" are both unmistakable, and the dorsal/palmar marker pair reads
    // correctly (mount above, palm below). An earlier revision of this routine had
    // the forearm pointing FORWARD, almost along the view axis, and did need an
    // off-axis camera — that is no longer the routine.
    //
    // ⚠ SET IMPERATIVELY, NOT AS A BINDING, because OrbitCameraController writes
    // camera.position / camera.eulerRotation directly — one user drag would
    // destroy a binding here permanently and silently take the guide view with it.
    // Orbit input is disabled while the guide camera is active for the same reason.
    property bool useGuideCamera: false
    readonly property vector3d guideCameraPosition: root.rightHanded
        ? Qt.vector3d( 0.06, 1.28, 1.45)
        : Qt.vector3d(-0.06, 1.28, 1.45)     // mirrored for a left-handed lead arm

    readonly property vector3d defaultCameraPosition: Qt.vector3d(0, 0.9, 3.5)

    function _applyCameraView() {
        // Face-on either way, so the orientation is identity in both branches and
        // only the distance changes.
        camera.eulerRotation = Qt.vector3d(0, 0, 0)
        camera.position = root.useGuideCamera ? root.guideCameraPosition
                                              : root.defaultCameraPosition
    }
    onUseGuideCameraChanged: _applyCameraView()
    onRightHandedChanged:    if (root.useGuideCamera) _applyCameraView()

    // When true, animate leadArmOverrideRotation changes via slerp (1.5 s).
    // Used by the calibration wizard to animate the guide from arm-down to T-pose.
    // Implemented with FrameAnimation + JS slerp: a wall-clock NumberAnimation
    // skips to its end on the first frame after a render stall (on Windows the
    // first frames of the calibrate step can stall for seconds while D3D
    // compiles the 19 skinned-model pipelines — the guide visibly "jumped" to
    // T-pose). Per-frame advance with a clamped step makes stalls PAUSE the
    // guide instead of skipping it. Emits leadArmAnimFinished() when the slerp
    // completes so hosts can chain on actual completion, not wall-clock guesses.
    property bool animateLeadArm:     false
    property int  leadArmAnimDuration: 1500
    signal leadArmAnimFinished()

    // Internal slerp state — not part of the public API.
    property quaternion _leadArmFrom: Qt.quaternion(1, 0, 0, 0)
    property quaternion _leadArmTo:   Qt.quaternion(1, 0, 0, 0)
    property real       _leadArmP:    1.0   // linear progress 0 → 1 (frame-driven)
    property real       _leadArmT:    1.0   // eased (InOutCubic) copy of _leadArmP
    // The forearm's own from/to. ⚠ ONE CLOCK, TWO SEGMENTS: both slerps read
    // _leadArmT, so the upper arm and forearm always travel together and
    // leadArmAnimFinished() still means "the whole guide motion is done". Giving
    // the forearm its own progress would let a host chain on one segment while the
    // other was still moving — and the HackMotion routine chains on exactly that
    // signal to fire its second marker at the device.
    property quaternion _leadForeArmFrom: Qt.quaternion(1, 0, 0, 0)
    property quaternion _leadForeArmTo:   Qt.quaternion(1, 0, 0, 0)

    function _slerp(a, b, t) {
        var dot = a.scalar*b.scalar + a.x*b.x + a.y*b.y + a.z*b.z
        if (dot < 0) { b = Qt.quaternion(-b.scalar, -b.x, -b.y, -b.z); dot = -dot }
        dot = Math.min(dot, 1.0)
        if (dot > 0.9995) {
            return Qt.quaternion(a.scalar + t*(b.scalar - a.scalar),
                                 a.x      + t*(b.x      - a.x),
                                 a.y      + t*(b.y      - a.y),
                                 a.z      + t*(b.z      - a.z))
        }
        var theta0    = Math.acos(dot)
        var theta     = theta0 * t
        var sinTheta  = Math.sin(theta)
        var sinTheta0 = Math.sin(theta0)
        var s0 = Math.cos(theta) - dot * sinTheta / sinTheta0
        var s1 = sinTheta / sinTheta0
        return Qt.quaternion(s0*a.scalar + s1*b.scalar,
                             s0*a.x      + s1*b.x,
                             s0*a.y      + s1*b.y,
                             s0*a.z      + s1*b.z)
    }

    // ⚠ TAKES BOTH SEGMENTS, because the HackMotion routine animates the FOREARM
    // while the upper arm stays put. foreFromQ is optional so the Witmotion flow's
    // existing single-argument calls keep meaning what they meant; when it is
    // omitted the forearm's start state is left alone.
    //
    // ⚠ AND IT ANCHORS THE DESTINATIONS TO THE START, WHICH IS LOAD-BEARING.
    // _leadArmTo / _leadForeArmTo are only updated by the two onChanged handlers,
    // and those RETURN EARLY while animateLeadArm is false — which is exactly the
    // state a caller is in while posing a starting position. So without this, a
    // destination survives from whatever ran last, and the next animation
    // interpolates the un-retargeted segment towards a stale target.
    //
    // That is not hypothetical: it shipped for one build. The HackMotion raise
    // retargets ONLY the forearm, so the upper arm still held _leadArmTo = identity
    // — the T-pose — and the raise swung the whole arm out to the side while the
    // forearm kept its relative angle. Anchoring to == from here makes "a segment
    // nobody retargeted does not move" true by construction instead of by protocol.
    function resetArmAnimation(fromQ, foreFromQ) {
        _leadArmAnim.stop()
        _leadArmFrom = fromQ
        _leadArmTo   = fromQ
        if (foreFromQ !== undefined) {
            _leadForeArmFrom = foreFromQ
            _leadForeArmTo   = foreFromQ
        }
        _leadArmP    = 1.0
        _leadArmT    = 1.0
    }

    FrameAnimation {
        id: _leadArmAnim
        running: false
        onTriggered: {
            // Clamp a stalled frame to one ~30 fps step so shader-compile or
            // load hitches pause the guide rather than fast-forwarding it.
            const dt = Math.min(frameTime, 1 / 30)
            root._leadArmP = Math.min(1.0, root._leadArmP
                                           + dt * 1000 / root.leadArmAnimDuration)
            const p = root._leadArmP   // InOutCubic, as the old NumberAnimation
            root._leadArmT = p < 0.5 ? 4 * p * p * p
                                     : 1 - Math.pow(-2 * p + 2, 3) / 2
            if (root._leadArmP >= 1.0) {
                stop()
                root.leadArmAnimFinished()
            }
        }
    }

    // ⚠ EITHER SEGMENT CHANGING STARTS THE MOTION, and the forearm handler is not
    // optional garnish: in the HackMotion routine the UPPER ARM IS IDENTICAL in
    // both poses, so if only the upper-arm override could start a slerp, that
    // routine's guide would never animate at all — it would jump, the athlete
    // would get no pacing, and the device would be watching a motion nobody was
    // shown how to make.
    //
    // Both handlers re-anchor from the CURRENT interpolated value, so a target
    // changed mid-flight continues from where the guide actually is rather than
    // snapping. When both change in the same turn the second handler restarts a
    // clock the first already reset to zero, which is harmless — the from/to pairs
    // were both captured before either restart.
    function _restartArmSlerp() {
        root._leadArmP = 0.0
        root._leadArmT = 0.0
        _leadArmAnim.restart()
    }

    onLeadArmOverrideRotationChanged: {
        if (!root.animateLeadArm) return
        root._leadArmFrom = _leadArmT < 1.0
            ? root._slerp(root._leadArmFrom, root._leadArmTo, root._leadArmT)
            : root._leadArmFrom
        root._leadArmTo = root.leadArmOverrideRotation
        root._restartArmSlerp()
    }

    onLeadForeArmOverrideRotationChanged: {
        if (!root.animateLeadArm) return
        root._leadForeArmFrom = _leadArmT < 1.0
            ? root._slerp(root._leadForeArmFrom, root._leadForeArmTo, root._leadArmT)
            : root._leadForeArmFrom
        root._leadForeArmTo = root.leadForeArmOverrideRotation
        root._restartArmSlerp()
    }

    // ── Loading state ─────────────────────────────────────────────────────────
    // Each RuntimeLoader reports Success (2), Loading (1), or Error (3).
    // Tally successes so we can show a progress indicator until all 19 segments load.
    property int loadedCount:  0
    property int totalSegments: 19   // 13 body + 6 arm (LeftArm/ForeArm/Hand ×2)
    readonly property bool fullyLoaded: loadedCount >= totalSegments

    function onSegmentLoaded(status) {
        if (status === RuntimeLoader.Success) loadedCount++
    }

    // ── Debug joint sphere component ──────────────────────────────────────────
    component JointMarker: Model {
        visible: root.showJoints
        source:  "#Sphere"
        scale:   Qt.vector3d(0.008, 0.008, 0.008)
        materials: PrincipledMaterial { baseColor: "#FF4444"; metalness: 0; roughness: 0.5 }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 3D Scene
    // ─────────────────────────────────────────────────────────────────────────
    // QML Rectangle provides the background so its color exactly matches the
    // surrounding UI — View3D's clearColor undergoes tonemapping that causes a
    // visible mismatch when the 3D viewport is large (e.g. calibration panel).
    Rectangle {
        anchors.fill: parent
        color: Theme.colorBg
    }

    View3D {
        anchors.fill: parent

        environment: SceneEnvironment {
            backgroundMode:      SceneEnvironment.Transparent
            antialiasingMode:    SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        // ── Camera ────────────────────────────────────────────────────────────
        // Character is ~1.8 world units tall; orbit origin at mid-chest height.
        PerspectiveCamera {
            id: camera
            position:    Qt.vector3d(0, 0.9, 3.5)
            fieldOfView: 45
            clipNear:    0.01
            clipFar:     100.0
        }

        Node { id: orbitOrigin; position: Qt.vector3d(0, 0.9, 0) }

        OrbitCameraController {
            anchors.fill: parent
            origin: orbitOrigin
            camera: camera
            // ⚠ Off while the guide camera is showing. This controller writes
            // camera.position / camera.eulerRotation imperatively, so one drag
            // would move the athlete off the only viewpoint the routine is legible
            // from, with nothing to put them back.
            enabled: !root.useGuideCamera
        }

        // ── Lighting ──────────────────────────────────────────────────────────
        DirectionalLight {
            eulerRotation: Qt.vector3d(-45, 45, 0)
            brightness:    1.2
            color:         "#FFFFFF"
        }
        DirectionalLight {
            eulerRotation: Qt.vector3d(30, -60, 0)
            brightness:    0.4
            color:         Theme.colorAccentLight
        }
        PointLight {
            position:      Qt.vector3d(0, 2.0, 1.5)
            brightness:    0.5
            color:         Theme.colorAccent
            quadraticFade: 0.8
        }

        // ═════════════════════════════════════════════════════════════════════
        // Kinematic chain root — Hips (pelvis)
        // World position extracted from ybot.glb bind matrix.
        // ═════════════════════════════════════════════════════════════════════
        Node {
            id: hipsNode
            position: Qt.vector3d(0, 0.9979, 0)
            rotation: adapter.hipsRotation

            JointMarker {}
            RuntimeLoader {
                source: "qrc:/assets/body/body_Hips.glb"
                onStatusChanged: root.onSegmentLoaded(status)
            }

            // ── Spine chain ───────────────────────────────────────────────────
            Node {
                id: spineNode
                position: Qt.vector3d(0, 0.0992, -0.0123)
                rotation: Qt.quaternion(0.9982, -0.0607, 0, 0)

                JointMarker {}
                RuntimeLoader {
                    source: "qrc:/assets/body/body_Spine.glb"
                    onStatusChanged: root.onSegmentLoaded(status)
                }

                Node {
                    id: spine1Node
                    position: Qt.vector3d(0, 0.1173, 0)

                    JointMarker {}
                    RuntimeLoader {
                        source: "qrc:/assets/body/body_Spine1.glb"
                        onStatusChanged: root.onSegmentLoaded(status)
                    }

                    Node {
                        id: spine2Node
                        position: Qt.vector3d(0, 0.1346, 0)
                        rotation: Qt.quaternion(0.9983, 0.0577, 0, 0)

                        JointMarker {}
                        RuntimeLoader {
                            source: "qrc:/assets/body/body_Spine2.glb"
                            onStatusChanged: root.onSegmentLoaded(status)
                        }

                        // ── Neck → Head ───────────────────────────────────────
                        // No neck mesh — Y-bot has no distinct neck skin geometry.
                        // The Node is kept for Phase 2 head animation.
                        Node {
                            id: neckNode
                            position: Qt.vector3d(0, 0.1503, 0.0088)

                            JointMarker {}

                            Node {
                                id: headNode
                                position: Qt.vector3d(0, 0.1032, 0.0314)
                                visible:  adapter.headVisible
                                rotation: adapter.headRotation

                                JointMarker {}
                                RuntimeLoader {
                                    source: "qrc:/assets/body/body_Head.glb"
                                    onStatusChanged: root.onSegmentLoaded(status)
                                }
                            }
                        }

                        // ── Left shoulder → arm chain ─────────────────────────
                        Node {
                            id: leftShoulderNode
                            position: Qt.vector3d(0.0611, 0.0911, 0.0076)
                            rotation: Qt.quaternion(-0.4398, -0.4538, -0.5448, 0.5511)

                            JointMarker {}
                            RuntimeLoader {
                                source: "qrc:/assets/body/body_LeftShoulder.glb"
                                onStatusChanged: root.onSegmentLoaded(status)
                            }

                            Node {
                                id: leftArmNode
                                position: Qt.vector3d(0, 0.1292, 0)
                                visible:  adapter.leftArmVisible
                                rotation: {
                                    if (root.useLeadArmOverride && root.rightHanded) {
                                        return root.animateLeadArm && root._leadArmT < 1.0
                                            ? root._slerp(root._leadArmFrom, root._leadArmTo, root._leadArmT)
                                            : root.leadArmOverrideRotation
                                    }
                                    if (root.useTrailArmOverride && !root.rightHanded) return root.trailArmOverrideRotation
                                    return adapter.leftArmRotation
                                }

                                JointMarker {}
                                OrientationTab { along: 0.22; tabColor: Theme.colorImuC }   // upper arm = slot C — green
                                RuntimeLoader {
                                    source: "qrc:/assets/body/arm_LeftArm.glb"
                                    onStatusChanged: root.onSegmentLoaded(status)
                                }
                                // Highlight overlay — upper arm segment (right-handed lead arm)
                                Model {
                                    visible: root.highlightLeadArm && root.rightHanded
                                    source: "#Cylinder"
                                    position: Qt.vector3d(0, 0.137, 0)
                                    scale: Qt.vector3d(0.00025, 0.00274, 0.00025)
                                    materials: PrincipledMaterial {
                                        baseColor: root.leadArmColor
                                        opacity: 0.45
                                        alphaMode: PrincipledMaterial.Blend
                                        metalness: 0.0
                                        roughness: 0.5
                                    }
                                }

                                Node {
                                    id: leftForeArmNode
                                    position: Qt.vector3d(0, 0.274, 0)
                                    visible:  adapter.leftForeArmVisible
                                    rotation: {
                                        // Slerped on the SHARED clock while a guide
                                        // motion is in flight — the HackMotion routine's
                                        // raise lives entirely in this node.
                                        if (root.useLeadArmOverride  &&  root.rightHanded)
                                            return root.animateLeadArm && root._leadArmT < 1.0
                                                ? root._slerp(root._leadForeArmFrom, root._leadForeArmTo, root._leadArmT)
                                                : root.leadForeArmOverrideRotation
                                        if (root.useTrailArmOverride && !root.rightHanded) return root.trailForeArmOverrideRotation
                                        return adapter.leftForeArmRotation
                                    }

                                    JointMarker {}
                                    OrientationTab { along: 0.22; tabColor: Theme.colorImuA }   // forearm = slot A — red
                                    RuntimeLoader {
                                        source: "qrc:/assets/body/arm_LeftForeArm.glb"
                                        onStatusChanged: root.onSegmentLoaded(status)
                                    }
                                    // Highlight overlay — forearm segment
                                    Model {
                                        visible: root.highlightLeadArm && root.rightHanded
                                        source: "#Cylinder"
                                        position: Qt.vector3d(0, 0.138, 0)
                                        scale: Qt.vector3d(0.00022, 0.002761, 0.00022)
                                        materials: PrincipledMaterial {
                                            baseColor: root.leadArmColor
                                            opacity: 0.45
                                            alphaMode: PrincipledMaterial.Blend
                                            metalness: 0.0
                                            roughness: 0.5
                                        }
                                    }

                                    Node {
                                        id: leftHandNode
                                        position: Qt.vector3d(0, 0.2761, 0)
                                        visible:  adapter.leftForeArmVisible

                                        JointMarker {}
                                        OrientationTab { along: 0.10; tabColor: Theme.colorImuB }   // hand = slot B — yellow
                                        RuntimeLoader {
                                            source: "qrc:/assets/body/arm_LeftHand.glb"
                                            onStatusChanged: root.onSegmentLoaded(status)
                                        }
                                        // Highlight overlay — hand segment
                                        Model {
                                            visible: root.highlightLeadArm && root.rightHanded
                                            source: "#Sphere"
                                            position: Qt.vector3d(0, 0.05, 0)
                                            scale: Qt.vector3d(0.0005, 0.0007, 0.0004)
                                            materials: PrincipledMaterial {
                                                baseColor: root.leadArmColor
                                                opacity: 0.45
                                                alphaMode: PrincipledMaterial.Blend
                                                metalness: 0.0
                                                roughness: 0.5
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        // ── Right shoulder → arm chain ────────────────────────
                        Node {
                            id: rightShoulderNode
                            position: Qt.vector3d(-0.0611, 0.0911, 0.0076)
                            rotation: Qt.quaternion(0.4398, 0.4538, -0.5448, 0.5511)

                            JointMarker {}
                            RuntimeLoader {
                                source: "qrc:/assets/body/body_RightShoulder.glb"
                                onStatusChanged: root.onSegmentLoaded(status)
                            }

                            Node {
                                id: rightArmNode
                                position: Qt.vector3d(0, 0.1292, 0)
                                visible:  adapter.rightArmVisible
                                rotation: {
                                    if (root.useLeadArmOverride && !root.rightHanded) {
                                        return root.animateLeadArm && root._leadArmT < 1.0
                                            ? root._slerp(root._leadArmFrom, root._leadArmTo, root._leadArmT)
                                            : root.leadArmOverrideRotation
                                    }
                                    if (root.useTrailArmOverride && root.rightHanded) return root.trailArmOverrideRotation
                                    return adapter.rightArmRotation
                                }

                                JointMarker {}
                                RuntimeLoader {
                                    source: "qrc:/assets/body/arm_RightArm.glb"
                                    onStatusChanged: root.onSegmentLoaded(status)
                                }
                                // Highlight overlay — upper arm segment (left-handed lead arm)
                                Model {
                                    visible: root.highlightLeadArm && !root.rightHanded
                                    source: "#Cylinder"
                                    position: Qt.vector3d(0, 0.137, 0)
                                    scale: Qt.vector3d(0.00025, 0.00274, 0.00025)
                                    materials: PrincipledMaterial {
                                        baseColor: root.leadArmColor
                                        opacity: 0.45
                                        alphaMode: PrincipledMaterial.Blend
                                        metalness: 0.0
                                        roughness: 0.5
                                    }
                                }

                                Node {
                                    id: rightForeArmNode
                                    position: Qt.vector3d(0, 0.2741, 0)
                                    visible:  adapter.rightForeArmVisible
                                    rotation: {
                                        // Slerped on the shared clock — see the left arm.
                                        if (root.useLeadArmOverride  && !root.rightHanded)
                                            return root.animateLeadArm && root._leadArmT < 1.0
                                                ? root._slerp(root._leadForeArmFrom, root._leadForeArmTo, root._leadArmT)
                                                : root.leadForeArmOverrideRotation
                                        if (root.useTrailArmOverride &&  root.rightHanded) return root.trailForeArmOverrideRotation
                                        return adapter.rightForeArmRotation
                                    }

                                    JointMarker {}
                                    RuntimeLoader {
                                        source: "qrc:/assets/body/arm_RightForeArm.glb"
                                        onStatusChanged: root.onSegmentLoaded(status)
                                    }
                                    // Highlight overlay — forearm segment
                                    Model {
                                        visible: root.highlightLeadArm && !root.rightHanded
                                        source: "#Cylinder"
                                        position: Qt.vector3d(0, 0.138, 0)
                                        scale: Qt.vector3d(0.00022, 0.002741, 0.00022)
                                        materials: PrincipledMaterial {
                                            baseColor: root.leadArmColor
                                            opacity: 0.45
                                            alphaMode: PrincipledMaterial.Blend
                                            metalness: 0.0
                                            roughness: 0.5
                                        }
                                    }

                                    Node {
                                        id: rightHandNode
                                        position: Qt.vector3d(0, 0.2761, 0)
                                        visible:  adapter.rightForeArmVisible

                                        JointMarker {}
                                        RuntimeLoader {
                                            source: "qrc:/assets/body/arm_RightHand.glb"
                                            onStatusChanged: root.onSegmentLoaded(status)
                                        }
                                        // Highlight overlay — hand segment
                                        Model {
                                            visible: root.highlightLeadArm && !root.rightHanded
                                            source: "#Sphere"
                                            position: Qt.vector3d(0, 0.05, 0)
                                            scale: Qt.vector3d(0.0005, 0.0007, 0.0004)
                                            materials: PrincipledMaterial {
                                                baseColor: root.leadArmColor
                                                opacity: 0.45
                                                alphaMode: PrincipledMaterial.Blend
                                                metalness: 0.0
                                                roughness: 0.5
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ── Left leg chain ────────────────────────────────────────────────
            // UpLeg rotation ≈ 180° around Z: bone-local +Y points world-down.
            // Child positions are therefore along local +Y (= world -Y = downward).
            Node {
                id: leftUpLegNode
                position: Qt.vector3d(0.0912, -0.0666, -0.0006)
                visible:  adapter.leftUpLegVisible
                rotation: adapter.leftUpLegRotation

                JointMarker {}
                RuntimeLoader {
                    source: "qrc:/assets/body/body_LeftUpLeg.glb"
                    onStatusChanged: root.onSegmentLoaded(status)
                }

                Node {
                    id: leftLegNode
                    position: Qt.vector3d(0, 0.4060, 0)
                    visible:  adapter.leftLegVisible
                    rotation: adapter.leftLegRotation

                    JointMarker {}
                    RuntimeLoader {
                        source: "qrc:/assets/body/body_LeftLeg.glb"
                        onStatusChanged: root.onSegmentLoaded(status)
                    }

                    Node {
                        id: leftFootNode
                        position: Qt.vector3d(0, 0.4210, 0)
                        visible:  adapter.poseSource === null
                        rotation: Qt.quaternion(0.8408, 0.5405, 0.0144, 0.0250)

                        JointMarker {}
                        RuntimeLoader {
                            source: "qrc:/assets/body/body_LeftFoot.glb"
                            onStatusChanged: root.onSegmentLoaded(status)
                        }
                    }
                }
            }

            // ── Right leg chain ───────────────────────────────────────────────
            Node {
                id: rightUpLegNode
                position: Qt.vector3d(-0.0913, -0.0666, -0.0006)
                visible:  adapter.rightUpLegVisible
                rotation: adapter.rightUpLegRotation

                JointMarker {}
                RuntimeLoader {
                    source: "qrc:/assets/body/body_RightUpLeg.glb"
                    onStatusChanged: root.onSegmentLoaded(status)
                }

                Node {
                    id: rightLegNode
                    position: Qt.vector3d(0, 0.4060, 0)
                    visible:  adapter.rightLegVisible
                    rotation: adapter.rightLegRotation

                    JointMarker {}
                    RuntimeLoader {
                        source: "qrc:/assets/body/body_RightLeg.glb"
                        onStatusChanged: root.onSegmentLoaded(status)
                    }

                    Node {
                        id: rightFootNode
                        position: Qt.vector3d(0, 0.4210, 0)
                        visible:  adapter.poseSource === null
                        rotation: Qt.quaternion(0.8408, 0.5406, -0.0144, -0.0250)

                        JointMarker {}
                        RuntimeLoader {
                            source: "qrc:/assets/body/body_RightFoot.glb"
                            onStatusChanged: root.onSegmentLoaded(status)
                        }
                    }
                }
            }
        }
    }

    // ── Loading overlay ───────────────────────────────────────────────────────
    Rectangle {
        anchors.centerIn: parent
        visible:  !root.fullyLoaded
        width:    loadText.width + Theme.sp(24)
        height:   loadText.height + Theme.sp(16)
        color:    "#CC000000"
        radius:   Theme.sp(6)

        Text {
            id: loadText
            anchors.centerIn: parent
            color:          Theme.colorText2
            font.family:    Theme.fontData
            font.pixelSize: Theme.fontSzBody2
            text:           qsTr("Loading… %1 / %2").arg(root.loadedCount).arg(root.totalSegments)
        }
    }

    // ── Orbit hint ────────────────────────────────────────────────────────────
    Text {
        anchors { bottom: parent.bottom; right: parent.right; margins: Theme.sp(10) }
        text:           qsTr("Drag · Scroll to zoom")
        color:          Theme.colorText3
        font.family:    Theme.fontBody
        font.pixelSize: Theme.fontSzLabel
    }
}
