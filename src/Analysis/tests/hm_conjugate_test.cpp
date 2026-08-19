// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_conjugate_test — the one conjugate, at the one site, and the reason nothing
// downstream can notice when it is missing.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests --target hm_conjugate_test
//   ctest --test-dir build/analyzer-tests -R hm_conjugate --output-on-failure
//
// Synthetic quaternions only — no hardware, no capture.
//
// A wG3 streams WORLD -> BODY. Our pipeline composes A·q_raw·M expecting
// body -> world (wrist_angles.h's qFore⁻¹·qHand only typechecks that way). The
// contract is therefore
//
//     q_anat = q_hm* (x) R_ph          (hm_frame.h)
//
// which is exactly imu_calibration::toAnatomical(A = identity, q_raw = q_hm*,
// M = R_ph) — the shared helper, with the raw quaternion ALREADY conjugated. So
// the conjugate belongs at the composition site (imu_vision_fuser.cpp), told by
// ImuSegmentBinding::hackMotion, and nowhere else.
//
// ⚠ THIS IS DISTINCT FROM hm_frame_test, WHICH PINS THE FRAME. That test asks
// which of the four candidate rotations is right and proves cross-talk cannot
// choose. This one asks whether the COMPOSITION SITE holds up its end of the
// contract, and it holds for every candidate — it is a statement about our
// plumbing, not about the device's axes.
//
// ⚠ AND THE REASON IT NEEDS PINNING IS SECTION C. The angle is IDENTICAL either
// way. Not approximately; provably, for every input. A dropped conjugate produces
// a quaternion that tracks the wrist convincingly with every decomposed sign
// inverted, and there is no plausibility check anywhere downstream that can tell
// the two apart. It was settled once by re-analysing a fixture swing with the
// conjugate disabled and reading which output looked like a golf swing. That
// experiment cannot be re-run in CI; this can.

#include "../../IMU/hm_frame.h"
#include "../../IMU/imu_calibration.h"
#include "../wrist_angles.h"

#include <QQuaternion>
#include <QVector3D>

#include <cmath>
#include <cstdio>

using namespace pinpoint;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

constexpr double kDeg = 180.0 / M_PI;

// q and −q are the same rotation (the double cover), so compare on the rotation.
bool sameRotation(const QQuaternion &a, const QQuaternion &b, float tol = 1e-4f)
{
    const float d = a.scalar() * b.scalar() + a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
    return std::fabs(std::fabs(d) - 1.0f) < tol;
}

// Rotation magnitude in degrees, on [0, 180].
double angleDeg(const QQuaternion &q)
{
    const QQuaternion n = q.normalized();
    const QVector3D v(n.x(), n.y(), n.z());
    return 2.0 * std::atan2(double(v.length()), std::fabs(double(n.scalar()))) * kDeg;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) < tol; }

QQuaternion q(float deg, float x, float y, float z)
{
    return QQuaternion::fromAxisAndAngle(QVector3D(x, y, z), deg).normalized();
}

// A spread of device attitudes — nothing special about them beyond being
// non-trivial and off every axis, so an identity or axis-aligned coincidence
// cannot carry a test.
const QQuaternion kAttitudes[] = {
    q(37.0f, 1.0f, 0.0f, 0.0f),
    q(-64.0f, 0.0f, 1.0f, 0.0f),
    q(115.0f, 0.0f, 0.0f, 1.0f),
    q(80.0f, 0.6f, -0.5f, 0.3f),
    q(-160.0f, -0.2f, 0.9f, 0.4f),
    q(12.0f, 0.3f, 0.3f, 0.9f),
};
constexpr int kAttitudeCount = int(sizeof(kAttitudes) / sizeof(kAttitudes[0]));

// ── A. The composition site owes exactly what hm_frame.h promises ────────────

void test_the_shared_helper_with_a_conjugated_raw_IS_the_contract()
{
    // What imu_vision_fuser does for a hackMotion binding, spelled out: A is
    // identity, the raw quaternion is conjugated first, M is the frame constant.
    // If that ever stops equalling hm_frame::toAnatomical, one of the two moved.
    for (int c = 0; c < hm_frame::kCandidateCount; ++c) {
        bool all = true;
        for (const QQuaternion &qHm : kAttitudes) {
            const QQuaternion viaFuser =
                imu_calibration::toAnatomical(QQuaternion(),          // A = identity
                                              qHm.conjugated(),       // the conjugate, here
                                              hm_frame::mountM(c));   // M = R_ph
            const QQuaternion viaContract = hm_frame::toAnatomical(qHm, c);
            all = all && sameRotation(viaFuser, viaContract);
        }
        char label[96];
        std::snprintf(label, sizeof(label),
                      "candidate %d: A=I, q_hm*, M=R_ph == hm_frame contract", c);
        check(all, label);
    }
}

void test_A_is_identity_by_design_not_by_accident()
{
    // The device applies its own calibration and streams the result, so there is no
    // per-session world->anatomical solve for a wG3 and A must stay identity. A
    // non-identity A here would be a fabricated calibration.
    const QQuaternion qHm = kAttitudes[3];
    const QQuaternion withA =
        imu_calibration::toAnatomical(q(20.0f, 0.0f, 1.0f, 0.0f), qHm.conjugated(),
                                      hm_frame::mountM(0));
    check(!sameRotation(withA, hm_frame::toAnatomical(qHm, 0)),
          "a non-identity A would change the answer (so identity is load-bearing)");
}

// ── B. Conjugating twice is conjugating not at all ───────────────────────────

void test_the_conjugate_must_not_also_live_at_storage_time()
{
    // hm_sample_convert.h stores the streamed quaternion VERBATIM and says at
    // length why it refuses to conjugate. That refusal is correct: doing it in two
    // places is doing it in none. Pinned as an inequality, because the failure is a
    // well-meant "defensive" second conjugate that restores the original.
    const QQuaternion qHm = kAttitudes[4];
    const QQuaternion twice =
        imu_calibration::toAnatomical(QQuaternion(), qHm.conjugated().conjugated(),
                                      hm_frame::mountM(0));
    check(!sameRotation(twice, hm_frame::toAnatomical(qHm, 0)),
          "conjugating at storage AND here gives the wrong answer");
    check(sameRotation(twice,
                       imu_calibration::toAnatomical(QQuaternion(), qHm, hm_frame::mountM(0))),
          "...specifically, the un-conjugated answer");
}

// ── C. ⚠ WHY NOTHING DOWNSTREAM CAN CATCH IT ─────────────────────────────────

// The two candidate anatomical streams for one unit: with the conjugate (correct)
// and without it (the failure being guarded against).
struct Pair { QQuaternion right, wrong; };
Pair both(const QQuaternion &qHm, int c)
{
    return { imu_calibration::toAnatomical(QQuaternion(), qHm.conjugated(), hm_frame::mountM(c)),
             imu_calibration::toAnatomical(QQuaternion(), qHm, hm_frame::mountM(c)) };
}

// The wrist relative, with its Address reference held at IDENTITY. Deliberate: on
// a real swing that reference is itself built from this same composition, so it
// would move with the bug and confuse what is being measured. Pinning it isolates
// the composition, which is what this file is about.
QQuaternion rel(const QQuaternion &fore, const QQuaternion &hand)
{
    return analysis::wristRel(fore, hand, QQuaternion());
}

void test_the_wrist_ANGLE_is_identical_either_way()
{
    // ⚠ THE CENTRAL FACT, AND IT IS EXACT RATHER THAN APPROXIMATE.
    //
    //   right = R*·q_f·q_h*·R      wrong = R*·q_f*·q_h·R
    //
    // and q_f·q_h* = q_f·(q_f*·q_h)*·q_f* — a conjugation of the other one's
    // inverse. Conjugation preserves rotation angle and so does inversion, so the
    // magnitudes agree for EVERY pair of attitudes. No magnitude check, no
    // range gate, no "is this plausible for a wrist" test anywhere downstream can
    // separate a correct pipeline from a mirrored one. That is why the conjugate
    // has to be pinned at the site rather than inferred from an output.
    for (int c = 0; c < hm_frame::kCandidateCount; ++c) {
        bool all = true;
        for (int i = 0; i < kAttitudeCount; ++i) {
            for (int j = 0; j < kAttitudeCount; ++j) {
                const Pair fore = both(kAttitudes[i], c);
                const Pair hand = both(kAttitudes[j], c);
                const QQuaternion relRight = rel(fore.right, hand.right);
                const QQuaternion relWrong = rel(fore.wrong, hand.wrong);
                all = all && near(angleDeg(relRight), angleDeg(relWrong), 1e-3);
            }
        }
        char label[96];
        std::snprintf(label, sizeof(label),
                      "candidate %d: the wrist angle is convention-blind (all %dx%d pairs)",
                      c, kAttitudeCount, kAttitudeCount);
        check(all, label);
    }
}

void test_but_every_decomposed_SIGN_inverts()
{
    // With the forearm at the device's own reference pose — which is not a
    // contrivance, it is the pose the wG3's calibration establishes and the one
    // Address is referenced to — the identity collapses to
    //
    //     wrong = right*
    //
    // exactly. A conjugate is the same rotation about the opposite axis, so every
    // signed component of the decomposition negates while its magnitude does not.
    // That is the whole failure in one line: bowed reads as cupped, ulnar reads as
    // radial, and the curve still looks like a wrist.
    const int c = 0;
    const QQuaternion foreAtRef;                     // q_hm = identity: the reference pose
    const Pair fore = both(foreAtRef, c);

    // A pure flexion of the hand about the wrist's flexion axis (Z in the
    // anatomical frame), and a pure ulnar deviation about X. Single-axis, because
    // that is where "the signs negate" is exact rather than merely typical — and it
    // is also the shape of a directed acceptance capture ("bowed, then returned").
    struct Motion { const char *what; QQuaternion qHm; };
    const Motion motions[] = {
        { "flexion",   q(25.0f, 0.0f, 0.0f, 1.0f) },
        { "deviation", q(18.0f, 1.0f, 0.0f, 0.0f) },
    };

    for (const Motion &m : motions) {
        const Pair hand = both(m.qHm, c);
        const QQuaternion relRight = rel(fore.right, hand.right);
        const QQuaternion relWrong = rel(fore.wrong, hand.wrong);

        check(sameRotation(relWrong, relRight.conjugated()),
              "the un-conjugated relative IS the conjugate of the correct one");

        const analysis::WristAngles a = analysis::wristFlexExtDeviation(relRight);
        const analysis::WristAngles b = analysis::wristFlexExtDeviation(relWrong);

        char label[128];
        std::snprintf(label, sizeof(label), "%s: bow/cup sign inverts (%.2f deg -> %.2f deg)",
                      m.what, a.feRad * kDeg, b.feRad * kDeg);
        check(near(a.feRad, -b.feRad, 1e-4), label);
        std::snprintf(label, sizeof(label), "%s: hinge sign inverts (%.2f deg -> %.2f deg)",
                      m.what, a.rudRad * kDeg, b.rudRad * kDeg);
        check(near(a.rudRad, -b.rudRad, 1e-4), label);

        // And the magnitudes are untouched — restating section C on the numbers a
        // reader would actually see on a chart.
        std::snprintf(label, sizeof(label), "%s: and the magnitudes are unchanged", m.what);
        check(near(std::fabs(a.feRad), std::fabs(b.feRad), 1e-4)
                  && near(std::fabs(a.rudRad), std::fabs(b.rudRad), 1e-4), label);
    }
}

void test_the_flag_is_not_a_no_op()
{
    // ImuSegmentBinding::hackMotion selects between two genuinely different
    // compositions. If a future simplification made them agree, the flag could be
    // deleted — and this is the assertion that would have to be deleted with it.
    for (int c = 0; c < hm_frame::kCandidateCount; ++c) {
        const Pair p = both(kAttitudes[3], c);
        char label[96];
        std::snprintf(label, sizeof(label),
                      "candidate %d: conjugated and un-conjugated differ", c);
        check(!sameRotation(p.right, p.wrong), label);
    }
}

// ── D. The asymmetry that makes it easy to walk past ─────────────────────────

void test_the_VECTOR_path_needs_no_conjugate()
{
    // ⚠ THE ORIENTATION PATH NEEDS A CONJUGATE AND THE VECTOR PATH DOES NOT, WHICH
    // IS EXACTLY WHY THE MISSING ONE SURVIVED REVIEW. The fuser rotates measured
    // inertials by mountM.conjugated(); for a wG3 that is R_ph* = frameMap(c) —
    // character for character what hm_frame::pronationRateDps already applies. The
    // device's gyro shares its quaternion's frame and its calibration re-references
    // both together, so only the constant map is needed there.
    //
    // Pinned so that "the vector path is fine, therefore the orientation path is
    // fine" cannot be reasoned to a second time, and so that a change to mountM's
    // definition breaks here rather than silently unrotating every gyro.
    const QVector3D gyro(120.0f, -450.0f, 65.0f);   // °/s, a plausible mid-downswing rate
    for (int c = 0; c < hm_frame::kCandidateCount; ++c) {
        const QVector3D viaFuser = hm_frame::mountM(c).conjugated().rotatedVector(gyro);
        const QVector3D viaFrame = hm_frame::frameMap(c).rotatedVector(gyro);
        char label[96];
        std::snprintf(label, sizeof(label),
                      "candidate %d: mountM* == frameMap on a vector", c);
        check((viaFuser - viaFrame).length() < 1e-3f, label);

        // ...and it is the same rotation pronationRateDps reads its answer from.
        std::snprintf(label, sizeof(label),
                      "candidate %d: and its y component IS the pronation rate", c);
        check(near(double(viaFuser.y()), double(hm_frame::pronationRateDps(gyro, c)), 1e-3),
              label);
    }
}

void test_an_unselected_candidate_parks_rather_than_guessing()
{
    // No frame selected ⇒ identity out of hm_frame::toAnatomical and 0 out of
    // pronationRateDps, so an unreconciled lane drives nothing. The alternative —
    // falling back to some default candidate — would publish a confident wrong sign.
    check(!hm_frame::isSelected(-1) && !hm_frame::isSelected(hm_frame::kCandidateCount),
          "out-of-range candidates are not selected");
    check(hm_frame::toAnatomical(kAttitudes[0], -1).isIdentity(),
          "an unselected candidate yields identity, not a guess");
    check(near(double(hm_frame::pronationRateDps(QVector3D(1.0f, 2.0f, 3.0f), -1)), 0.0, 1e-9),
          "and no pronation rate");
}

} // namespace

int main()
{
    std::printf("=== HackMotion conjugate: one site, and invisible when missing ===\n");

    test_the_shared_helper_with_a_conjugated_raw_IS_the_contract();
    test_A_is_identity_by_design_not_by_accident();
    test_the_conjugate_must_not_also_live_at_storage_time();
    test_the_wrist_ANGLE_is_identical_either_way();
    test_but_every_decomposed_SIGN_inverts();
    test_the_flag_is_not_a_no_op();
    test_the_VECTOR_path_needs_no_conjugate();
    test_an_unselected_candidate_parks_rather_than_guessing();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
