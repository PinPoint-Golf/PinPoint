// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hm_binding_recon_test — what a recorded wG3 swing is bound from when it is
// re-analysed, and what it refuses to be bound from.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests --target hm_binding_recon_test
//   ctest --test-dir build/analyzer-tests -R hm_binding_recon --output-on-failure
//
// A Witmotion binding is READ BACK: the exporter persists the session's A/M solve
// and the loader parses it. A wG3 binding cannot be, because there is no such
// solve — the device calibrates itself, so the exporter writes no alignA/mountM
// for that lane at all. It is RECONSTRUCTED instead, from two facts the recording
// does carry: the frame constant, and which unit the lane was.
//
// ⚠ THIS WAS THE SECOND BLOCKER, AND NOBODY HAD NAMED IT. The live binding was
// fixed first; re-analysis still hardcoded IMU_WitMotion for every replayed lane
// and threw the recorded instrument tag away. Fixing only the live path would have
// helped future captures and left the eleven recorded E3 fixture swings silent —
// their device.role is 0 and roleName empty precisely BECAUSE the live binding
// never ran when they were captured.
//
// ⚠ AND EVERY FAILURE HERE IS SILENT. Re-analysis of an unbound lane still
// succeeds, still writes a swing.json, and simply holds no wrist metric. A wrong
// ROLE binds the palm's stream to the forearm and publishes a wrist angle computed
// from the wrong two segments. A dropped `hackMotion` skips the conjugate at the
// composition site and inverts every sign while the curve still tracks the wrist
// (hm_conjugate_test). None of the three raises anything.
//
// Every input is a value read out of swing.json, so they are stated directly —
// no swing folder, no analyzer, no OpenCV.

#include "../hm_binding_recon.h"

#include <QQuaternion>

#include <cmath>
#include <cstdio>

using namespace pinpoint;
using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

const QString kDev = QStringLiteral("D4:22:CD:00:9B:1F");
const QString kLowerArmSerial = kDev + QStringLiteral("#lowerArm");
const QString kPalmSerial     = kDev + QStringLiteral("#palm");

constexpr SourceId kSource = 3;
constexpr int kCandidate   = 1;   // C2 — a real selection, so mountM is not identity

bool sameRotation(const QQuaternion &a, const QQuaternion &b)
{
    const float d = a.scalar() * b.scalar() + a.x() * b.x() + a.y() * b.y() + a.z() * b.z();
    return std::fabs(std::fabs(d) - 1.0f) < 1e-4f;
}

// A calibrated lower-arm lane — the ordinary case.
hm_binding::Result good(const QString &serial = kLowerArmSerial,
                        int calState = hm_binding::kCalibratedState)
{
    return hm_binding::reconstruct(kSource, serial, calState, kCandidate);
}

void test_the_unit_decides_the_segment()
{
    // ⚠ ROLE COMES FROM THE UNIT, NOT FROM sessionType. Which arm segment a wG3's
    // lower-arm board measured is a fact about the hardware. Reading the capture's
    // declared intent instead would make re-analysis depend on a flag the golfer
    // set, and would produce nothing for a capture recorded under any other session
    // type — which is most of what a library holds.
    const auto fore = good(kLowerArmSerial);
    check(fore.binding.has_value() && fore.binding->role == SegmentRole::LeadForearm,
          "#lowerArm -> LeadForearm");

    const auto hand = good(kPalmSerial);
    check(hand.binding.has_value() && hand.binding->role == SegmentRole::LeadHand,
          "#palm -> LeadHand");
}

void test_the_lane_is_marked_as_the_instrument_it_is()
{
    // Without this the conjugate is skipped downstream and every wrist sign
    // inverts, with the curve still tracking the wrist. There is no output that
    // shows it.
    const auto r = good();
    check(r.binding.has_value() && r.binding->hackMotion, "the binding is marked hackMotion");
}

void test_A_is_identity_and_M_is_the_frame_constant()
{
    // Both by definition rather than by solve: the device referenced the pair at
    // its own pose, so there is no per-session world->anatomical rotation to
    // recover, and the only constant needed is the Phase-D frame.
    const auto r = good();
    check(r.binding.has_value(), "bound");
    if (!r.binding) return;
    check(r.binding->alignA.isIdentity(), "A is identity");
    check(sameRotation(r.binding->mountM, hm_frame::mountM(kCandidate)),
          "M is the frame constant for the selected candidate");
    check(r.binding->source == kSource, "and it names the lane it was built for");
}

void test_the_frame_constant_follows_the_candidate()
{
    // Not pinned to one candidate: whichever Phase D selected is what a
    // re-analysis must use, or a swing re-analysed on a later build silently
    // reads through a different frame than the one it was measured under.
    for (int c = 0; c < hm_frame::kCandidateCount; ++c) {
        const auto r = hm_binding::reconstruct(kSource, kPalmSerial,
                                               hm_binding::kCalibratedState, c);
        char label[96];
        std::snprintf(label, sizeof(label), "candidate %d: M tracks the selection", c);
        check(r.binding.has_value() && sameRotation(r.binding->mountM, hm_frame::mountM(c)),
              label);
    }
}

void test_no_frame_candidate_means_no_binding_at_all()
{
    // ⚠ NOT a binding whose toAnatomical() returns identity. That would produce a
    // confident, plausible, meaningless wrist curve — the worst of the three
    // possible outcomes, because it is the only one that looks like data.
    const auto r = hm_binding::reconstruct(kSource, kLowerArmSerial,
                                           hm_binding::kCalibratedState, -1);
    check(!r.binding.has_value(), "no candidate -> no binding");
    check(r.refusal == hm_binding::Refusal::NoFrameCandidate, "and it says which fact was missing");

    const auto past = hm_binding::reconstruct(kSource, kLowerArmSerial,
                                              hm_binding::kCalibratedState,
                                              hm_frame::kCandidateCount);
    check(!past.binding.has_value(), "an out-of-range candidate is not selected either");
}

void test_a_serial_that_names_no_unit_is_not_guessed_at()
{
    // A lane tagged hackmotion whose serial carries no unit suffix cannot say which
    // segment it measured. Defaulting to lowerArm would bind a palm stream to the
    // forearm and publish an angle from the wrong pair.
    const auto bare = good(kDev);
    check(!bare.binding.has_value(), "a bare device id is not bound");
    check(bare.refusal == hm_binding::Refusal::SerialNamesNoUnit, "and says why");

    check(!good(kDev + QStringLiteral("#upperArm")).binding.has_value(),
          "an unrecognised suffix is not bound");
    check(!good(QString()).binding.has_value(), "an empty serial is not bound");
}

void test_the_device_calibration_state_is_carried_not_assumed()
{
    // ⚠ 2 == WR_CAL_CALIBRATED. Anything else means the lane was streaming BOARD
    // PLACEMENT rather than anatomy, and a binding that claimed calibrated would
    // publish the strap's position as a wrist measurement.
    const auto cal = good(kLowerArmSerial, hm_binding::kCalibratedState);
    check(cal.binding.has_value() && cal.binding->calibrated && cal.binding->anatCalibrated,
          "state 2 -> calibrated");

    for (const int state : { -1, 0, 1, 3 }) {
        const auto r = good(kLowerArmSerial, state);
        char label[96];
        std::snprintf(label, sizeof(label), "state %d -> NOT calibrated", state);
        check(r.binding.has_value() && !r.binding->calibrated && !r.binding->anatCalibrated,
              label);
    }
}

void test_an_uncalibrated_lane_is_still_BOUND()
{
    // The distinction that matters: uncalibrated is a binding the caller warns
    // about, not an absent one. Dropping it instead would lose the lane entirely
    // and lose the warning with it — the loader's "recorded UNCALIBRATED — results
    // unreliable" line only fires over bindings that exist.
    const auto r = good(kLowerArmSerial, 0);
    check(r.binding.has_value(), "an uncalibrated lane still produces a binding");
    check(r.refusal == hm_binding::Refusal::None, "which is not a refusal");
    check(r.binding && r.binding->role == SegmentRole::LeadForearm,
          "and it still knows which segment it measured");
}

void test_the_mount_residuals_stay_absent()
{
    // There is no mount solve for a device that calibrates itself, so there are no
    // residuals to report. Inventing zeros that read as "a perfect mount" would be
    // worse than leaving them at their defaults, which is what a corpus filter on
    // calibration provenance sees.
    const auto r = good();
    check(r.binding.has_value(), "bound");
    if (!r.binding) return;
    check(r.binding->mountDeviationDeg == 0.0 && r.binding->mountGravityErrorDeg == 0.0,
          "no fabricated mount residuals");
    check(r.binding->calibratedAtUtc.isEmpty() && r.binding->calibAgeSec < 0.0,
          "and no fabricated calibration timestamp");
}

void test_both_units_of_one_peripheral_reconstruct_independently()
{
    // The real shape of a wG3 capture: one peripheral, two lanes, two source ids,
    // two segments, one frame. This is the case the eleven E3 fixture swings are.
    const auto fore = hm_binding::reconstruct(0, kLowerArmSerial,
                                              hm_binding::kCalibratedState, kCandidate);
    const auto hand = hm_binding::reconstruct(1, kPalmSerial,
                                              hm_binding::kCalibratedState, kCandidate);
    check(fore.binding && hand.binding, "both lanes bind");
    if (!fore.binding || !hand.binding) return;
    check(fore.binding->role != hand.binding->role, "to two different segments");
    check(fore.binding->source != hand.binding->source, "from two different sources");
    check(sameRotation(fore.binding->mountM, hand.binding->mountM),
          "through one shared frame constant");
    check(fore.binding->hackMotion && hand.binding->hackMotion, "both marked as the instrument");
}

} // namespace

int main()
{
    std::printf("=== HackMotion binding reconstruction: rebuilt, not read back ===\n");

    test_the_unit_decides_the_segment();
    test_the_lane_is_marked_as_the_instrument_it_is();
    test_A_is_identity_and_M_is_the_frame_constant();
    test_the_frame_constant_follows_the_candidate();
    test_no_frame_candidate_means_no_binding_at_all();
    test_a_serial_that_names_no_unit_is_not_guessed_at();
    test_the_device_calibration_state_is_carried_not_assumed();
    test_an_uncalibrated_lane_is_still_BOUND();
    test_the_mount_residuals_stay_absent();
    test_both_units_of_one_peripheral_reconstruct_independently();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
