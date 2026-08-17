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

// ArmVizView — rigid Y-bot arm segments driven by IMU quaternions.
//
// Each arm (upper arm / forearm / hand) is extracted from ybot.glb as a
// standalone bone-local GLB.  Bone-local means the joint sits at the segment's
// origin, so rotating a Node around that origin produces the correct pivot.
//
// Kinematic chain (per arm):
//   armNode  (positioned at shoulder joint)
//     └─ arm segment Model
//     └─ elbowNode  (at Qt.vector3d(0, 0.274, 0) in arm-local space)
//          └─ forearm segment Model
//          └─ wristNode  (at Qt.vector3d(0, 0.2761, 0) in forearm-local space)
//               └─ hand segment Model
//
// Rotation math (all quaternions world-space from IMUs):
//   armNode.rotation     = armRestQuat * imuUpperArm
//   elbowNode.rotation   = imuUpperArm.inv * imuForearm
//   wristNode.rotation   = imuForearm.inv  * imuHand
//
// IMU slot assignment (Wrist Motion session):
//   Slot A → Wrist  (forearm near wrist) — the calibrated IMU
//   Slot B → Hand   (back of hand)
//   Slot C → Upper arm (optional)
//
// Instances are resolved by slot assignment, not by position in
// imuManager.instances, because instances() iterates a QMap in device-ID order.
//
// Handedness — derived from athleteController.currentHandedness:
//   "Right" (default) → Left arm driven  (lead arm for right-handed golfer)
//   "Left"            → Right arm driven (lead arm for left-handed golfer)

Item {
    id: root

    readonly property bool rightHanded: athleteController.currentHandedness !== "Left"

    // Framing target: the middle of the forearm in the arm-down rest pose.
    // Shoulder sits at y≈1.436; the arm hangs straight down so the forearm spans
    // elbow (y≈1.162) → wrist (y≈0.886), giving a midpoint at y≈1.024. x/z follow
    // the shoulder joint. The camera looks straight on (-Z) at this point.
    readonly property vector3d armCenter: Qt.vector3d(rightHanded ? 0.1876 : -0.1876, 1.024, -0.062)

    // Distance pulls the whole arm (shoulder → hand) into frame with a small
    // margin, matching the breathing room BodyVizView leaves. With a 45° vertical
    // FOV this shows ±~0.50 m about armCenter — shoulder lands just inside the top.
    readonly property real camDistance: 1.2

    onRightHandedChanged: {
        camera.position      = Qt.vector3d(root.armCenter.x, root.armCenter.y, root.armCenter.z + root.camDistance)
        camera.eulerRotation = Qt.vector3d(0, 0, 0)
    }

    // ── Per-slot IMU bindings ─────────────────────────────────────────────────
    // Resolved by SLOT, not by walking imuDeviceList against a scalar placement
    // map — imuManager.instanceForSlot() is the one canonical resolver (Phase C
    // unit-keyed placement: a Witmotion's key is still its bare device id, but a
    // HackMotion's key is "<deviceId>#lowerArm" / "<deviceId>#palm", so a single
    // wG3 can answer to BOTH slot A and slot B). instanceForSlot() returns the
    // per-UNIT object a viz binding wants — an HmUnit for a HackMotion slot, an
    // ImuInstance for a Witmotion one — never the owning peripheral, which is
    // what quatApplyCalib() below actually reads (quatW/X/Y/Z, anatCalibrated,
    // anatQuat, calibrated, calibTransform all live on the unit, not the device).
    // `var _dep = imuManager.instances` AND `var _dep2 = appSettings.imuPlacement`
    // force re-evaluation when either changes: instanceForSlot() is a
    // Q_INVOKABLE, not a reactive property, so neither dependency is picked up
    // automatically.
    //
    // ⚠ PHASE C HAS NO ANATOMICAL FRAME for a HackMotion unit. HmUnit::
    // anatCalibrated() is false and anatQuat() is identity until Phase D solves
    // the per-unit constant rotation (hm_instance.h:127-134) — quatApplyCalib()
    // already handles that correctly by parking an uncalibrated segment at rest,
    // so a HackMotion-driven slot just sits still rather than showing a live
    // (but wrong/mirrored) avatar. That is correct and honest, not a bug: do not
    // "fix" it here by inventing a transform.
    readonly property QtObject imuSlotA: {   // Wrist (forearm) — calibrated
        var _dep  = imuManager.instances
        var _dep2 = appSettings.imuPlacement
        return imuManager.instanceForSlot("A")
    }
    readonly property QtObject imuSlotB: {   // Hand
        var _dep  = imuManager.instances
        var _dep2 = appSettings.imuPlacement
        return imuManager.instanceForSlot("B")
    }
    readonly property QtObject imuSlotC: {   // Upper arm (optional)
        var _dep  = imuManager.instances
        var _dep2 = appSettings.imuPlacement
        return imuManager.instanceForSlot("C")
    }

    // ── Quaternion helpers ────────────────────────────────────────────────────

    // Hamilton product of two quaternions (a × b).
    // QML quaternion value type uses .scalar (not .w) for the W component.
    function quatMul(a, b) {
        return Qt.quaternion(
            a.scalar*b.scalar - a.x*b.x - a.y*b.y - a.z*b.z,
            a.scalar*b.x + a.x*b.scalar + a.y*b.z - a.z*b.y,
            a.scalar*b.y - a.x*b.z + a.y*b.scalar + a.z*b.x,
            a.scalar*b.z + a.x*b.y - a.y*b.x + a.z*b.scalar
        )
    }

    // Conjugate = inverse for unit quaternions.
    function quatInv(q) { return Qt.quaternion(q.scalar, -q.x, -q.y, -q.z) }

    // Map a raw IMU quaternion to the segment's anatomical orientation.
    // Prefers the functional calibration (q_anat = A·q_raw·M, computed in C++ and
    // exposed as anatQuat — identity at the reference pose). Falls back to the
    // legacy single-factor calibTransform, then to raw if uncalibrated.
    // IMPORTANT: QML quaternion uses .scalar for W — .w returns undefined.
    function quatApplyCalib(imuInst, raw) {
        // Uncalibrated or absent segments sit at REST (identity contribution) rather
        // than passing through raw sensor data — otherwise an uncalibrated upper-arm
        // or hand sensor drives its segment to an arbitrary orientation (the
        // "zig-zag"/collapse). Only a calibrated segment moves.
        if (!imuInst) return Qt.quaternion(1, 0, 0, 0)
        if (imuInst.anatCalibrated) {
            var a = imuInst.anatQuat
            return Qt.quaternion(a.scalar, a.x, a.y, a.z)
        }
        if (!imuInst.calibrated) return Qt.quaternion(1, 0, 0, 0)
        var cal = Qt.quaternion(imuInst.calibTransform.scalar,
                                imuInst.calibTransform.x,
                                imuInst.calibTransform.y,
                                imuInst.calibTransform.z)
        return quatMul(cal, raw)
    }

    // Bone rest-pose quaternion: the world orientation of the segment when its
    // input quaternion is identity (the arm-down reference, where anatQuat == I).
    // The segment must hang straight down (world -Y).
    //
    // Solved numerically (Wahba fit) so the anatomical frame renders to the correct
    // model-world directions at the validated verify poses: arm-down → world -Y,
    // abduction → world -X (to the side), flexion → world -Z (forward; the avatar
    // faces -Z, away from the camera). This R0 maps the fixed anatomical frame to
    // the model world, so it is a reusable model constant, not session-specific.
    // (Right arm not yet re-derived — the lead arm for a right-handed athlete is the left.)
    // Corrected 2026-06-01 with the world-axis gizmo: the previous value rendered
    // both motions ~45° off about vertical (abduction landed 45° behind +X, flexion
    // midway between +X and +Z). R0_new = R(−45°,Y)·R0_old → abduction → +X (lateral),
    // flexion → +Z (anterior), arm hangs −Y. (Verified against the logged poses.)
    // Residual back-of-hand roll is handled by restRollDeg.
    // R0 is factored EXPLICITLY into the shared world→scene BASIS CHANGE composed with a
    // per-GLB REST OFFSET (the part that makes each segment hang). Canonical basis change:
    // pinpoint::viz::worldToScene() (src/Gui/viz_frame.h), a fixed Rx(-90°); viz_frame_test
    // pins that worldToScene·restOffset reproduces the old hand-tuned R0 constants, so the
    // avatar is unchanged. (docs/design/imu_frame_contract.md §6; imu_rearchitecture.md §3.4c.)
    //   left-handed lead  → restOffset = identity → R0 IS the pure basis change.
    //   right-handed lead → restOffset is the back-of-hand/forearm GLB rest rotation.
    readonly property quaternion worldToScene:    Qt.quaternion(0.70710678, -0.70710678, 0, 0)
    readonly property quaternion leftRestOffset:  Qt.quaternion(0.71184, -0.67833, 0.16334, -0.08089)
    readonly property quaternion rightRestOffset: Qt.quaternion(1, 0, 0, 0)
    readonly property quaternion leftRestQuat:  quatMul(worldToScene, leftRestOffset)
    readonly property quaternion rightRestQuat: quatMul(worldToScene, rightRestOffset)

    // Constant roll about each segment's long axis (bone-local +Y), correcting the
    // rendered rest FACING. The IMUs all share the strap convention, so at the
    // arm-down reference every segment is rolled by the same amount about its long
    // axis; one correction applies to all. Applied as a POST-multiply on each
    // segment's world orientation (W = R0·anat·rollFix), with the relative joints
    // conjugated by rollFix so motion planes are unchanged — only the roll shifts.
    // Long-axis roll bringing the back-of-hand (sensor face) onto +X (lateral).
    // −99° balances rest (~5° off) and flexion (~8°) — derived from the logged poses
    // against the corrected leftRestQuat. (Flexion can't go below ~7° by roll alone:
    // a flex pose carries unavoidable upper-arm rotation.)
    property real restRollDeg: -99
    readonly property quaternion rollFix: {
        var h = root.restRollDeg * Math.PI / 360.0   // half-angle
        return Qt.quaternion(Math.cos(h), 0, Math.sin(h), 0)   // about local +Y
    }
    readonly property quaternion rollFixInv: Qt.quaternion(root.rollFix.scalar, 0, -root.rollFix.y, 0)

    // Diagnostic orientation marker — a bright tab offset toward the segment's
    // local +Z (anatomical anterior) near its distal end. Because it sits OFF the
    // long axis it orbits visibly as the bone rolls, exposing roll on the otherwise
    // rotationally-symmetric arm segments. Add/remove via `showOrientationTabs`.
    property bool showOrientationTabs: true
    component OrientationTab: Node {
        id: tabRoot
        property real  along:    0.20            // distance up the bone (local +Y)
        property color tabColor: Theme.colorImuA
        property real  tabScale: 0.0006
        // Which local face the marker sits on. Empirically: +Z = palm (medial),
        // +X = posterior; the IMU straps on the back/outer face = opposite the palm
        // = local -Z (watch-like, away from the thigh). Tunable while we confirm.
        property vector3d tabDir: Qt.vector3d(0, 0, -1)
        visible: root.showOrientationTabs
        y: along
        Model {
            source:    "#Sphere"
            scale:     Qt.vector3d(tabRoot.tabScale, tabRoot.tabScale, tabRoot.tabScale)
            position:  Qt.vector3d(tabRoot.tabDir.x * 0.05, tabRoot.tabDir.y * 0.05, tabRoot.tabDir.z * 0.05)
            materials: PrincipledMaterial {
                baseColor: tabRoot.tabColor       // id-qualified — `parent.tabColor` silently failed → white
                lighting:  PrincipledMaterial.NoLighting
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // 3D Scene
    // ─────────────────────────────────────────────────────────────────────────
    View3D {
        id: view3D
        anchors.fill: parent

        environment: SceneEnvironment {
            clearColor:          Theme.colorBg
            backgroundMode:      SceneEnvironment.Color
            antialiasingMode:    SceneEnvironment.MSAA
            antialiasingQuality: SceneEnvironment.High
        }

        // Camera framed on the single lead arm.
        PerspectiveCamera {
            id: camera
            position:      Qt.vector3d(root.armCenter.x, root.armCenter.y, root.armCenter.z + root.camDistance)
            eulerRotation: Qt.vector3d(0, 0, 0)
            fieldOfView:   45
            clipNear:      0.01
            clipFar:       50.0
        }

        Node { id: orbitOrigin; position: root.armCenter }

        OrbitCameraController {
            anchors.fill: parent
            origin: orbitOrigin
            camera: camera
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
            position:      Qt.vector3d(0, 1.8, 1.2)
            brightness:    0.5
            color:         Theme.colorAccent
            quadraticFade: 0.8
        }

        // ── Active (lead) arm — driven by IMUs ────────────────────────────────
        //
        // Each rotation binding accesses imuSlot*.quatW/X/Y/Z directly so QML's
        // binding engine registers them as live dependencies.  Intermediate
        // `property quaternion` values are not used — see comment above.
        Node {
            id: armNode
            position: root.rightHanded ? Qt.vector3d( 0.1876, 1.4357, -0.0617)
                                       : Qt.vector3d(-0.1876, 1.4356, -0.0617)
            // rotation = armRestQuat * slotC(upperArm)
            rotation: {
                var raw = root.imuSlotC
                    ? Qt.quaternion(root.imuSlotC.quatW, root.imuSlotC.quatX, root.imuSlotC.quatY, root.imuSlotC.quatZ)
                    : Qt.quaternion(1, 0, 0, 0)
                var ua = root.quatApplyCalib(root.imuSlotC, raw)
                // W_upper = R0 · ua · rollFix
                var w = root.quatMul(root.rightHanded ? root.leftRestQuat : root.rightRestQuat, ua)
                return root.quatMul(w, root.rollFix)
            }

            RuntimeLoader {
                source: root.rightHanded ? "qrc:/assets/body/arm_LeftArm.glb"
                                         : "qrc:/assets/body/arm_RightArm.glb"
            }
            OrientationTab { along: 0.22; tabColor: Theme.colorImuC }   // upper arm = slot C — green

            Node {
                position: Qt.vector3d(0, 0.274, 0)
                // rotation = slotC(upperArm).inv * slotA(wrist/forearm, calibrated)
                rotation: {
                    var rawUa = root.imuSlotC
                        ? Qt.quaternion(root.imuSlotC.quatW, root.imuSlotC.quatX, root.imuSlotC.quatY, root.imuSlotC.quatZ)
                        : Qt.quaternion(1, 0, 0, 0)
                    var rawFa = root.imuSlotA
                        ? Qt.quaternion(root.imuSlotA.quatW, root.imuSlotA.quatX, root.imuSlotA.quatY, root.imuSlotA.quatZ)
                        : Qt.quaternion(1, 0, 0, 0)
                    var ua = root.quatApplyCalib(root.imuSlotC, rawUa)
                    var fa = root.quatApplyCalib(root.imuSlotA, rawFa)
                    // rollFix⁻¹ · (ua⁻¹·fa) · rollFix  — keeps W_fore = R0·fa·rollFix
                    var rel = root.quatMul(root.quatInv(ua), fa)
                    return root.quatMul(root.rollFixInv, root.quatMul(rel, root.rollFix))
                }

                RuntimeLoader {
                    source: root.rightHanded ? "qrc:/assets/body/arm_LeftForeArm.glb"
                                             : "qrc:/assets/body/arm_RightForeArm.glb"
                }
                OrientationTab { along: 0.22; tabColor: Theme.colorImuA }   // forearm = slot A — red

                Node {
                    position: Qt.vector3d(0, 0.2761, 0)
                    // rotation = slotA(wrist/forearm).inv * slotB(hand)
                    rotation: {
                        var rawFa = root.imuSlotA
                            ? Qt.quaternion(root.imuSlotA.quatW, root.imuSlotA.quatX, root.imuSlotA.quatY, root.imuSlotA.quatZ)
                            : Qt.quaternion(1, 0, 0, 0)
                        var rawHa = root.imuSlotB
                            ? Qt.quaternion(root.imuSlotB.quatW, root.imuSlotB.quatX, root.imuSlotB.quatY, root.imuSlotB.quatZ)
                            : Qt.quaternion(1, 0, 0, 0)
                        var fa = root.quatApplyCalib(root.imuSlotA, rawFa)
                        var ha = root.quatApplyCalib(root.imuSlotB, rawHa)
                        // rollFix⁻¹ · (fa⁻¹·ha) · rollFix  — keeps W_hand = R0·ha·rollFix
                        var rel = root.quatMul(root.quatInv(fa), ha)
                        return root.quatMul(root.rollFixInv, root.quatMul(rel, root.rollFix))
                    }

                    RuntimeLoader {
                        source: root.rightHanded ? "qrc:/assets/body/arm_LeftHand.glb"
                                                 : "qrc:/assets/body/arm_RightHand.glb"
                    }
                    OrientationTab { along: 0.10; tabColor: Theme.colorImuB }   // hand = slot B — yellow
                }
            }
        }

    }

    // ── IMU assignment legend ─────────────────────────────────────────────────
    Column {
        anchors { bottom: parent.bottom; left: parent.left; margins: Theme.sp(10) }
        spacing: Theme.sp(4)

        Repeater {
            model: [
                qsTr("Slot A · Wrist"),
                qsTr("Slot B · Hand"),
                qsTr("Slot C · Upper arm")
            ]
            delegate: Row {
                spacing: Theme.sp(6)
                // ⚠ CONNECTEDNESS IS A DEVICE PROPERTY, SO THIS ASKS THE DEVICE.
                // The imuSlotA/B/C bindings above hold the per-UNIT object, which for
                // a HackMotion is an HmUnit — and an HmUnit has no imuConnected at
                // all. Reading it there yields `undefined`, and `inst !== null &&
                // undefined` is `undefined` rather than false, which Qt reports as
                // "Unable to assign [undefined] to bool" once per evaluation and
                // leaves the dot stuck at its default. deviceForSlot() returns the
                // peripheral, which is what "is there a sensor live on this slot"
                // actually means for either device kind.
                property bool live: {
                    var _dep  = imuManager.instances
                    var _dep2 = appSettings.imuPlacement
                    var slot  = index === 0 ? "A" : index === 1 ? "B" : "C"
                    var dev   = imuManager.deviceForSlot(slot)
                    return dev !== null && dev.imuConnected === true
                }
                Rectangle {
                    width: Theme.sp(6); height: Theme.sp(6); radius: Theme.sp(3)
                    anchors.verticalCenter: parent.verticalCenter
                    color: parent.live ? Theme.colorGood : Theme.colorText3
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }
                Text {
                    text:           modelData
                    color:          parent.live ? Theme.colorText2 : Theme.colorText3
                    font.family:    Theme.fontBody
                    font.pixelSize: Theme.fontSzLabel
                    Behavior on color { ColorAnimation { duration: Theme.durationFast } }
                }
            }
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
