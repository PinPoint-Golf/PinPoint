// Standalone characterization + property test for imu_calibration.h (the A·q·M
// anatomical solve) and the end-to-end calibration→wrist-angle math.
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// This is the Track-A keystone golden (docs/implementation/imu_rearchitecture.md §6 Phase 0.1): it
// freezes the calibration solve and the composed q_anat→angle numbers so any later
// frame-contract change that silently perturbs them fails loudly.
//
// SCOPE NOTE: the offline scored path composes q_anat as
//   (alignA * qRaw * mountM).normalized()        — imu_vision_fuser.cpp:72
// We freeze that EXACT expression here (section E), but compiling imu_vision_fuser.cpp
// itself drags in swing_window.h (Buffer). The literal live-vs-offline two-site
// agreement assertion (imu_instance.cpp:213 vs imu_vision_fuser.cpp:72) belongs with
// the Phase 1.3 unification, where both sites are touched.

#include "../../IMU/imu_calibration.h"
#include "../wrist_angles.h"

#include <QQuaternion>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace imu_calibration;
using pinpoint::analysis::WristAngles;
using pinpoint::analysis::wristFlexExtDeviation;
using pinpoint::analysis::wristRel;
using pinpoint::analysis::radToDeg;

static int g_fail = 0;

// --- assert helpers (sign-robust where rotations are compared) ----------------

static float quatDist(const QQuaternion &a, const QQuaternion &b)
{
    return std::max(std::max(std::abs(a.scalar() - b.scalar()), std::abs(a.x() - b.x())),
                    std::max(std::abs(a.y() - b.y()), std::abs(a.z() - b.z())));
}
// q and -q are the same rotation: accept whichever sign is closer.
static bool quatNear(const QQuaternion &g, const QQuaternion &w, float tol)
{
    const QQuaternion negW(-w.scalar(), -w.x(), -w.y(), -w.z());
    return std::min(quatDist(g, w), quatDist(g, negW)) <= tol;
}

static void checkQuat(const char *label, const QQuaternion &got, const QQuaternion &want, float tol)
{
    const bool ok = quatNear(got, want, tol);
    std::printf("  [%s] %-34s got (%.4f,%.4f,%.4f,%.4f)  want (%.4f,%.4f,%.4f,%.4f)\n",
                ok ? "PASS" : "FAIL", label,
                got.scalar(), got.x(), got.y(), got.z(),
                want.scalar(), want.x(), want.y(), want.z());
    if (!ok) ++g_fail;
}

static void checkNear(const char *label, double got, double want, double tol)
{
    const bool ok = std::abs(got - want) <= tol;
    std::printf("  [%s] %-34s got %9.4f  want %9.4f  (tol %.4g)\n",
                ok ? "PASS" : "FAIL", label, got, want, tol);
    if (!ok) ++g_fail;
}

static void checkBool(const char *label, bool got, bool want)
{
    const bool ok = (got == want);
    std::printf("  [%s] %-34s got %s  want %s\n",
                ok ? "PASS" : "FAIL", label, got ? "true" : "false", want ? "true" : "false");
    if (!ok) ++g_fail;
}

int main()
{
    const QQuaternion I;
    const QVector3D X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
    std::printf("=== imu_calibration.h characterization & keystone golden ===\n\n");

    // -- A. nominal mount constants are frozen (a frame change must not silently move them).
    std::printf("-- A. nominal mount constants --\n");
    checkQuat("nominalArmMount",  nominalArmMount(),  QQuaternion(0.5f, -0.5f, -0.5f, -0.5f), 1e-3f);
    checkQuat("nominalHandMount", nominalHandMount(), QQuaternion(0.4388f, 0.6054f, 0.4965f, -0.4409f), 1e-3f);

    // -- B. solveSegment reproduces nominalArmMount from the canonical arm-down inputs.
    //    Arm hanging: gravity-DOWN is sensor +X, long axis (distal) is sensor +X, flexion
    //    axis is sensor +Z (docs/reference/imu_axis_reference.md + imu_calibration.h:54-57). Hand-
    //    verified: this yields M=(0.5,-0.5,-0.5,-0.5)=nominalArmMount exactly.
    std::printf("\n-- B. solveSegment(arm-down) == nominalArmMount --\n");
    {
        const Alignment a = solveSegment(/*refRaw=*/I, /*down=*/X, /*long=*/X, /*flex=*/Z);
        checkQuat("M == nominalArmMount", a.M, nominalArmMount(), 1e-4f);
        checkNear("axisAngleDeg",         a.axisAngleDeg, 90.0, 0.5);
        checkBool("valid",                a.valid, true);

        // The opposite-sign down vector flips e_y → a DIFFERENT (mirrored) mount. Freeze it
        // too so the sign-handling branch (imu_calibration.h:127) can't silently change.
        const Alignment flip = solveSegment(I, -X, X, Z);
        checkQuat("M(down=-X) flipped",   flip.M, QQuaternion(0.5f, 0.5f, -0.5f, 0.5f), 1e-4f);
        checkBool("valid(flip)",          flip.valid, true);
    }

    // -- C. reference-pose identity: A·refRaw·M == I, by construction A=conj(refRaw·M).
    //    Holds for ANY refRaw, so this is a property test over several fixed orientations.
    std::printf("\n-- C. A·refRaw·M == I (reference-pose identity property) --\n");
    {
        const QQuaternion refs[] = {
            I,
            QQuaternion::fromAxisAndAngle(X, 37),
            QQuaternion::fromAxisAndAngle(Y, 110),
            QQuaternion::fromAxisAndAngle(QVector3D(1, 2, 3), 64),
            (QQuaternion::fromAxisAndAngle(Z, 25) * QQuaternion::fromAxisAndAngle(X, 80)).normalized(),
        };
        int idx = 0;
        for (const QQuaternion &refRaw : refs) {
            const Alignment a = solveSegment(refRaw, X, X, Z);
            const QQuaternion qAnatRef = (a.A * refRaw * a.M).normalized();
            char lbl[40];
            std::snprintf(lbl, sizeof lbl, "A·refRaw·M == I  [#%d]", idx++);
            checkQuat(lbl, qAnatRef, I, 1e-4f);
        }
    }

    // -- D. validity gate: axisAngleDeg must land in [60,120] (imu_calibration.h:138).
    std::printf("\n-- D. validity gate (axis orthogonality) --\n");
    {
        checkBool("parallel long∥flex → invalid", solveSegment(I, X, X, X).valid, false);
        checkBool("orthogonal long⊥flex → valid", solveSegment(I, X, X, Z).valid, true);
        // 45° apart: below the 60° floor → rejected.
        const QVector3D diag = QVector3D(1, 0, 1).normalized();
        const Alignment a45 = solveSegment(I, X, X, diag);
        checkNear("axisAngleDeg(45° case)", a45.axisAngleDeg, 45.0, 0.5);
        checkBool("45° apart → invalid",    a45.valid, false);
    }

    // -- E. KEYSTONE: a fixed (A,M,q_raw) pose → frozen q_anat (via the exact offline
    //    expression) → frozen wrist flexion/deviation. Inputs are deterministic; outputs
    //    are captured from current code. A change here means the calibration→angle math
    //    moved — exactly what Track A exists to catch.
    std::printf("\n-- E. keystone: (A,M,q_raw) → q_anat → wrist angles --\n");
    {
        const QQuaternion mFore = nominalArmMount();
        const QQuaternion mHand = nominalHandMount();
        const QQuaternion aFore = I, aHand = I;                 // world→anat (frame-dependent; fixed here)
        const QQuaternion qRawFore = QQuaternion::fromAxisAndAngle(X, 10);
        const QQuaternion qRawHand =
            (QQuaternion::fromAxisAndAngle(X, 10) * QQuaternion::fromAxisAndAngle(Z, 25)).normalized();

        // Exactly imu_vision_fuser.cpp:72.
        const QQuaternion qAnatFore = (aFore * qRawFore * mFore).normalized();
        const QQuaternion qAnatHand = (aHand * qRawHand * mHand).normalized();
        const QQuaternion qWristRel = wristRel(qAnatFore, qAnatHand, /*addr=*/I);
        const WristAngles wa = wristFlexExtDeviation(qWristRel);

        std::printf("  [DIAG] qAnatFore = (%.5f, %.5f, %.5f, %.5f)\n",
                    qAnatFore.scalar(), qAnatFore.x(), qAnatFore.y(), qAnatFore.z());
        std::printf("  [DIAG] qAnatHand = (%.5f, %.5f, %.5f, %.5f)\n",
                    qAnatHand.scalar(), qAnatHand.x(), qAnatHand.y(), qAnatHand.z());
        std::printf("  [DIAG] qWristRel = (%.5f, %.5f, %.5f, %.5f)\n",
                    qWristRel.scalar(), qWristRel.x(), qWristRel.y(), qWristRel.z());
        std::printf("  [DIAG] fe = %.4f°   rud = %.4f°\n", radToDeg(wa.feRad), radToDeg(wa.rudRad));

        // GOLDEN VALUES — captured from current code (frozen reference). A diff here
        // means the A·q·M composition or the wrist-angle extraction moved.
        checkQuat("qAnatFore frozen", qAnatFore, QQuaternion(0.54168f, -0.45452f, -0.45452f, -0.54168f), 1e-3f);
        checkQuat("qAnatHand frozen", qAnatHand, QQuaternion(0.47970f,  0.52741f,  0.64267f, -0.28054f), 1e-3f);
        checkQuat("qWristRel frozen", qWristRel, QQuaternion(-0.12003f, 0.02809f,  0.97935f,  0.16027f), 1e-3f);
        checkNear("fe frozen (deg)",  radToDeg(wa.feRad),  -5.6377, 0.02);
        checkNear("rud frozen (deg)", radToDeg(wa.rudRad), 17.8888, 0.02);
    }

    // -- F. toAnatomical() — the unified A·q·M helper (Phase 1.3) — matches the manual
    //    expression both call sites used (imu_instance.cpp:213, imu_vision_fuser.cpp:72).
    std::printf("\n-- F. toAnatomical() unified helper agreement --\n");
    {
        const QQuaternion A  = QQuaternion::fromAxisAndAngle(X, 37);
        const QQuaternion M  = nominalArmMount();
        const QQuaternion qs[] = {
            I,
            QQuaternion::fromAxisAndAngle(Z, 25),
            (QQuaternion::fromAxisAndAngle(Y, 70) * QQuaternion::fromAxisAndAngle(X, 12)).normalized(),
        };
        int idx = 0;
        for (const QQuaternion &qRaw : qs) {
            const QQuaternion viaHelper = toAnatomical(A, qRaw, M);
            const QQuaternion viaManual = (A * qRaw * M).normalized();
            char lbl[44];
            std::snprintf(lbl, sizeof lbl, "helper == (A·q·M).norm  [#%d]", idx++);
            checkQuat(lbl, viaHelper, viaManual, 1e-6f);
        }
        // The keystone qAnatFore (section E) is exactly toAnatomical(I, qRawFore, armMount).
        const QQuaternion qRawFore = QQuaternion::fromAxisAndAngle(X, 10);
        checkQuat("keystone via helper", toAnatomical(I, qRawFore, nominalArmMount()),
                  QQuaternion(0.54168f, -0.45452f, -0.45452f, -0.54168f), 1e-3f);
    }

    std::printf("\n=== %s (%d assert failures) ===\n",
                g_fail ? "FAILURES PRESENT" : "ALL ASSERTS PASS", g_fail);
    return g_fail ? 1 : 0;
}
