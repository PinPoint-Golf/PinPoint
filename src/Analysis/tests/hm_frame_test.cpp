// Phase D — HackMotion anatomical frame reconciliation.
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.0/macos
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// Synthetic quaternions only — NO HARDWARE, NO CAPTURE. Everything here is a
// property of the two frames and holds whichever candidate a capture eventually
// selects.
//
// Four sections, and sections C and D are the load-bearing ones:
//
//   A. the similarity identity  q_rel_pps = R_ph* ⊗ q_rel_hm ⊗ R_ph
//   B. ⚠ the ANGLE cannot distinguish the two composition orders
//   C. ⚠ CROSS-TALK CANNOT SELECT THE CANDIDATE — all four score zero
//   D. the (flexion, deviation) sign pair CAN — it is unique across the four
//
// B, C and D exist to stop three different "simplifications", each of which
// leaves a pipeline that looks entirely correct and is mirrored. B and C in
// particular assert that a check does NOT work, which is an unusual thing to
// pin and exactly why it needs pinning: a reader who does not know that
// cross-talk is blind here will reach for it as the acceptance test, because
// Phase D's own brief originally said to.

#include "../../IMU/hm_frame.h"
#include "../wrist_angles.h"

#include <QQuaternion>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <cstdio>

using pinpoint::analysis::WristAngles;
using pinpoint::analysis::radToDeg;
using pinpoint::analysis::wristFlexExtDeviation;
namespace hmf = pinpoint::hm_frame;

static int g_fail = 0;

static void checkNear(const char *label, double got, double want, double tol)
{
    const bool ok = std::abs(got - want) <= tol;
    std::printf("  [%s] %-52s got %9.4f  want %9.4f\n",
                ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

static void checkTrue(const char *label, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_fail;
}

// q and -q are the same rotation; compare on the closer sign.
static float quatDist(const QQuaternion &a, const QQuaternion &b)
{
    return std::max(std::max(std::abs(a.scalar() - b.scalar()), std::abs(a.x() - b.x())),
                    std::max(std::abs(a.y() - b.y()), std::abs(a.z() - b.z())));
}
static bool quatNear(const QQuaternion &g, const QQuaternion &w, float tol)
{
    const QQuaternion negW(-w.scalar(), -w.x(), -w.y(), -w.z());
    return std::min(quatDist(g, w), quatDist(g, negW)) <= tol;
}

// The convention-blind scalar wrist angle: 2·acos|q_a · q_b|, in degrees.
static double scalarAngleDeg(const QQuaternion &q)
{
    return 2.0 * radToDeg(std::acos(std::min(1.0f, std::abs(q.scalar()))));
}

// The device's anatomical axes, as this header understands them: Y is the limb
// axis, X carries flexion/extension, Z carries deviation.
static const QVector3D kHmFlexionAxis  (1.0f, 0.0f, 0.0f);
static const QVector3D kHmLimbAxis     (0.0f, 1.0f, 0.0f);
static const QVector3D kHmDeviationAxis(0.0f, 0.0f, 1.0f);

int main()
{
    std::printf("\n=== Phase D — HackMotion frame reconciliation ===\n");

    // -- A. the similarity identity ------------------------------------------
    //
    // Composing each unit through toAnatomical and taking our own relative
    // rotation must equal conjugating the device's relative rotation by the
    // constant. Both routes are used in the tree — the first by the live path
    // (per-unit anatQuat, then wristRel), the second by anything reasoning about
    // the pair directly — and they must not be allowed to drift apart.
    std::printf("\n-- A. q_rel_pps = R_ph* ⊗ q_rel_hm ⊗ R_ph, both routes agree --\n");
    for (int c = 0; c < hmf::kCandidateCount; ++c) {
        const QQuaternion Rph = hmf::mountM(c);

        // Two arbitrary, deliberately unlovely world->body orientations.
        const QQuaternion qArm  = QQuaternion::fromAxisAndAngle(QVector3D(0.3f, 0.7f, -0.2f), 41.0f)
                                      .normalized();
        const QQuaternion qPalm = QQuaternion::fromAxisAndAngle(QVector3D(-0.5f, 0.1f, 0.8f), 63.0f)
                                      .normalized();

        // Route 1: per-unit anatomical, then our own wrist relative.
        const QQuaternion viaUnits = hmf::wristRelFromHm(qArm, qPalm, c);

        // Route 2: the device's relative rotation, conjugated by the constant.
        // ⚠ q_rel_hm is q_arm ⊗ q_palm*, NOT the library's q_palm ⊗ q_arm*.
        const QQuaternion qRelHm = (qArm * qPalm.conjugated()).normalized();
        const QQuaternion viaRel = (Rph.conjugated() * qRelHm * Rph).normalized();

        char label[96];
        std::snprintf(label, sizeof(label), "%s  two routes agree", hmf::kCandidates[c].name);
        checkTrue(label, quatNear(viaUnits, viaRel, 1e-5f));
    }

    // -- B. ⚠ the ANGLE is blind to composition order -------------------------
    //
    // The plan calls for this explicitly: a test asserting that the angle check
    // CANNOT distinguish the two orders, so nobody later "simplifies" the real
    // acceptance test away in favour of it.
    std::printf("\n-- B. ⚠ the scalar angle CANNOT distinguish the two orders --\n");
    {
        // (i) An arbitrary pair. The angle is identical; the decomposition is not.
        const QQuaternion qArm  = QQuaternion::fromAxisAndAngle(QVector3D(0.2f, -0.6f, 0.5f), 37.0f)
                                      .normalized();
        const QQuaternion qPalm = QQuaternion::fromAxisAndAngle(QVector3D(0.9f, 0.2f, -0.1f), 52.0f)
                                      .normalized();

        const QQuaternion forward = (qArm * qPalm.conjugated()).normalized();
        const QQuaternion reverse = (qPalm * qArm.conjugated()).normalized();

        checkNear("angle is IDENTICAL under both orders",
                  scalarAngleDeg(reverse), scalarAngleDeg(forward), 1e-4);
        checkTrue("...though the two orders ARE different rotations",
                  !quatNear(forward, reverse, 1e-3f));

        const WristAngles wgf = wristFlexExtDeviation(forward);
        const WristAngles wgr = wristFlexExtDeviation(reverse);
        checkTrue("...and the decomposition differs, invisibly to the angle",
                  std::abs(radToDeg(wgf.feRad) - radToDeg(wgr.feRad)) > 1.0);

        // (ii) A pure single-axis motion — the acceptance-capture case. HERE the
        // reversal is an exact sign flip, which is what makes a DIRECTED motion
        // able to settle the order and an undirected one unable to.
        //
        // ⚠ The exactness is specific to a single-axis rotation. Case (i) shows a
        // general rotation does NOT simply negate: our decomposition is a ZXY
        // Cardan extraction, and inverting a general rotation reorders the
        // sequence rather than negating each term. So a capture that wanders off
        // its axis does not produce a cleanly inverted reading it could be
        // recognised by — it produces a wrong one that looks like a small error.
        for (int c = 0; c < hmf::kCandidateCount; ++c) {
            const QQuaternion Rph = hmf::mountM(c);
            const QQuaternion qFlexHm =
                QQuaternion::fromAxisAndAngle(kHmFlexionAxis, 20.0f).normalized();

            const QQuaternion fwd = (Rph.conjugated() * qFlexHm * Rph).normalized();
            const QQuaternion rev = (Rph.conjugated() * qFlexHm.conjugated() * Rph).normalized();

            char label[128];
            std::snprintf(label, sizeof(label), "%s  single-axis: angle unchanged",
                          hmf::kCandidates[c].name);
            checkNear(label, scalarAngleDeg(rev), scalarAngleDeg(fwd), 1e-3);

            std::snprintf(label, sizeof(label), "%s  single-axis: flexion INVERTS",
                          hmf::kCandidates[c].name);
            checkNear(label, radToDeg(wristFlexExtDeviation(rev).feRad),
                      -radToDeg(wristFlexExtDeviation(fwd).feRad), 1e-3);
        }
    }

    // -- C. ⚠ CROSS-TALK CANNOT SELECT THE CANDIDATE --------------------------
    //
    // The finding that reshaped this phase. Drive a pure rotation about each of
    // the device's anatomical axes and decompose through every candidate. If
    // cross-talk could select, some candidate would score badly. None does.
    std::printf("\n-- C. ⚠ every candidate scores ZERO cross-talk — so it cannot select --\n");
    {
        const float kDeg = 20.0f;
        struct Motion { const char *name; QVector3D axis; };
        const Motion motions[3] = {
            { "flexion   (about device X)", kHmFlexionAxis },
            { "deviation (about device Z)", kHmDeviationAxis },
            { "pronation (about device Y)", kHmLimbAxis },
        };

        for (int c = 0; c < hmf::kCandidateCount; ++c) {
            const QQuaternion Rph = hmf::mountM(c);
            for (const Motion &m : motions) {
                const QQuaternion qRelHm = QQuaternion::fromAxisAndAngle(m.axis, kDeg).normalized();
                const QQuaternion qRelPps = (Rph.conjugated() * qRelHm * Rph).normalized();
                const WristAngles w = wristFlexExtDeviation(qRelPps);

                const double fe  = radToDeg(w.feRad);
                const double rud = radToDeg(w.rudRad);

                // The SECONDARY channel of each motion is the cross-talk. It is
                // zero for every candidate, which is precisely the problem.
                const double crossTalk = (m.axis == kHmFlexionAxis)     ? std::abs(rud)
                                       : (m.axis == kHmDeviationAxis)   ? std::abs(fe)
                                                                        : std::max(std::abs(fe),
                                                                                   std::abs(rud));
                char label[128];
                std::snprintf(label, sizeof(label), "%s  %s cross-talk",
                              hmf::kCandidates[c].name, m.name);
                checkNear(label, crossTalk, 0.0, 1e-3);
            }
        }
        std::printf("      ^ all zero. A capture accepted on cross-talk alone has\n"
                    "        selected nothing — see section D for what does.\n");
    }

    // -- D. the sign signature IS unique — this is the selector ---------------
    //
    // Each candidate's (flexion, deviation) sign pair under a known DIRECTED
    // motion. Uniqueness across the four is what makes two labelled motions
    // sufficient, and it is what the capture protocol depends on.
    std::printf("\n-- D. the (flexion, deviation) sign pair is unique across candidates --\n");
    {
        const float kDeg = 20.0f;
        int seen[4][2];

        for (int c = 0; c < hmf::kCandidateCount; ++c) {
            const QQuaternion Rph = hmf::mountM(c);

            const QQuaternion qFlex = QQuaternion::fromAxisAndAngle(kHmFlexionAxis, kDeg).normalized();
            const QQuaternion qDev  = QQuaternion::fromAxisAndAngle(kHmDeviationAxis, kDeg).normalized();
            const QQuaternion qPron = QQuaternion::fromAxisAndAngle(kHmLimbAxis, kDeg).normalized();

            const WristAngles wf = wristFlexExtDeviation((Rph.conjugated() * qFlex * Rph).normalized());
            const WristAngles wd = wristFlexExtDeviation((Rph.conjugated() * qDev  * Rph).normalized());

            const int sFlex = radToDeg(wf.feRad)  > 0 ? +1 : -1;
            const int sDev  = radToDeg(wd.rudRad) > 0 ? +1 : -1;

            // The pronation sign comes off the gyro route, not the decomposition
            // — the wrist barely articulates about this axis, so the relative
            // rotation is the wrong place to read it.
            const QVector3D gyro(0.0f, 100.0f, 0.0f);       // +100 °/s about the device limb axis
            const int sPron = hmf::pronationRateDps(gyro, c) > 0 ? +1 : -1;
            Q_UNUSED(qPron);

            seen[c][0] = sFlex;
            seen[c][1] = sDev;

            char label[128];
            std::snprintf(label, sizeof(label), "%s  flexion sign", hmf::kCandidates[c].name);
            checkNear(label, sFlex, hmf::kCandidates[c].flexionSign, 0.0);
            std::snprintf(label, sizeof(label), "%s  deviation sign", hmf::kCandidates[c].name);
            checkNear(label, sDev, hmf::kCandidates[c].deviationSign, 0.0);
            std::snprintf(label, sizeof(label), "%s  pronation sign", hmf::kCandidates[c].name);
            checkNear(label, sPron, hmf::kCandidates[c].pronationSign, 0.0);
        }

        bool unique = true;
        for (int a = 0; a < hmf::kCandidateCount; ++a)
            for (int b = a + 1; b < hmf::kCandidateCount; ++b)
                if (seen[a][0] == seen[b][0] && seen[a][1] == seen[b][1]) unique = false;
        checkTrue("two directed motions select a candidate uniquely", unique);
    }

    // -- E. the unselected state is honest ------------------------------------
    //
    // Until a capture has selected, a HackMotion lane must report NO frame — not
    // a plausible default. Identity here is what parks the segment.
    std::printf("\n-- E. an unselected candidate yields identity, not a guess --\n");
    {
        const int unset = pinpoint::tuned::hmframe::kCandidateUnset;
        checkTrue("isSelected(unset) is false", !hmf::isSelected(unset));
        const QQuaternion q = QQuaternion::fromAxisAndAngle(QVector3D(1, 2, 3), 44.0f).normalized();
        checkTrue("toAnatomical(unset) is identity", quatNear(hmf::toAnatomical(q, unset),
                                                              QQuaternion(), 1e-6f));
        checkNear("pronationRateDps(unset) is zero",
                  hmf::pronationRateDps(QVector3D(0.0f, 100.0f, 0.0f), unset), 0.0, 1e-6);
    }

    std::printf("\n=== %s (%d failure%s) ===\n\n",
                g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
