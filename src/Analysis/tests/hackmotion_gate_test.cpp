// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// hackmotion_gate_test — the requirement axis that decides whether an hm.* rung
// is offered at all.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests --target hackmotion_gate_test
//   ctest --test-dir build/analyzer-tests -R hackmotion_gate --output-on-failure
//
// Phase F added a `hackMotion` axis to MetricRequirement, three descriptors that
// route through it, and one line in metric_provider.h that reads it:
//
//     if (req.hackMotion && !ctx.hasHackMotion) missing << "a HackMotion wrist sensor";
//
// metric_catalogue_test covers the DECLARATION — that the three descriptors exist,
// carry the right type and group, and bring the catalogue to its stated size. It
// never sets ShotContext::hasHackMotion, so the gate itself has never run under a
// test. This file is the other half.
//
// ⚠ THE TWO FAILURE DIRECTIONS ARE BOTH SILENT AND BOTH BAD.
//
// A gate stuck CLOSED hides a measurement the golfer paid for: the wG3 is on the
// wrist, the lane is recorded, the metric is produced — and the directory says
// "needs a HackMotion wrist sensor" about the sensor they are wearing.
//
// A gate stuck OPEN is worse, because it does not read as an error at all. The
// hm.* keys are the MEASURED rungs; a shot without a wG3 offers them, they
// resolve to nothing, and the panels that prefer them over our own estimate go
// quiet — the same silence the four hand-rolled key loops used to produce, from
// the other end. See series_key_ladder_test.
//
// ⚠ AND THE AXIS MUST STAY SEPARATE FROM imuRoles. That list says which SEGMENTS
// were bound and is blind to what bound them: a wG3 and a pair of Witmotions both
// report LeadForearm + LeadHand. Gating on roles would offer the measured rungs
// to a Witmotion rig, which is exactly what the separate axis exists to prevent —
// so the "roles alone are not enough" case is pinned explicitly below.

#include "metric_catalogue.h"
#include "launch_monitor_reading.h"

#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

// The three measured rungs, and the bare keys they shadow.
const char *const kHmKeys[] = { "hm.leadWristFlexExt", "hm.leadWristRadUln", "hm.forearmRotation" };
const char *const kBareKeys[] = { "leadWristFlexExt", "leadWristRadUln", "forearmRotation" };
constexpr int kHmKeyCount = 3;

// A wrist shot with the lead forearm and hand bound — what BOTH instruments look
// like from imuRoles alone, which is the whole reason the axis is separate.
ShotContext wristShot(bool hackMotion)
{
    ShotContext c;
    c.sessionType   = 1;                                   // Wrist
    c.imuRoles      = { SegmentRole::LeadForearm, SegmentRole::LeadHand };
    c.tier          = ReconstructionTier::Mono3DPlusImu;
    c.hasHackMotion = hackMotion;
    return c;
}

void test_the_measured_rungs_need_the_device()
{
    const MetricCatalogue cat = makeMetricCatalogue();
    const ShotContext witmotion = wristShot(false);

    for (int i = 0; i < kHmKeyCount; ++i) {
        const MetricAvailability a = cat.resolve(QString::fromLatin1(kHmKeys[i]), witmotion);
        char label[128];
        std::snprintf(label, sizeof(label), "%s Unavailable without a wG3", kHmKeys[i]);
        check(a.state == MetricAvailability::Unavailable, label);

        // ...and it says WHY, in the words a golfer would use. An Unavailable with
        // the wrong reason sends someone to buy a camera for a wrist sensor.
        std::snprintf(label, sizeof(label), "%s says which sensor is missing", kHmKeys[i]);
        check(a.reason.contains(QStringLiteral("HackMotion")), label);
    }
}

void test_the_measured_rungs_are_offered_when_it_is_worn()
{
    const MetricCatalogue cat = makeMetricCatalogue();
    const ShotContext wg3 = wristShot(true);

    for (int i = 0; i < kHmKeyCount; ++i) {
        const MetricAvailability a = cat.resolve(QString::fromLatin1(kHmKeys[i]), wg3);
        char label[128];
        std::snprintf(label, sizeof(label), "%s Measured with a wG3", kHmKeys[i]);
        check(a.state == MetricAvailability::Measured, label);
        std::snprintf(label, sizeof(label), "%s routes through the device rung", kHmKeys[i]);
        check(a.routeId == QStringLiteral("hackMotion"), label);
    }
}

void test_bound_roles_alone_do_not_open_the_gate()
{
    // ⚠ THE ASSERTION THAT KEEPS THE AXIS SEPARATE. Both shots below carry
    // LeadForearm + LeadHand; only one carries the instrument. If a future
    // simplification folded hasHackMotion into imuRoles, this is what fails —
    // otherwise a three-Witmotion rig would silently claim the measured rungs.
    const MetricCatalogue cat = makeMetricCatalogue();
    ShotContext everyRole;
    everyRole.sessionType = 1;
    everyRole.imuRoles    = { SegmentRole::LeadForearm, SegmentRole::LeadHand,
                              SegmentRole::LeadUpperArm, SegmentRole::Thorax,
                              SegmentRole::Pelvis };
    everyRole.tier            = ReconstructionTier::Stereo3D;
    everyRole.hasFaceOn       = true;
    everyRole.hasClubTrack    = true;
    everyRole.hasBallTrack    = true;
    everyRole.hasLaunchMonitor = true;
    everyRole.hasHackMotion   = false;   // the one thing missing

    for (int i = 0; i < kHmKeyCount; ++i) {
        char label[160];
        std::snprintf(label, sizeof(label),
                      "%s still Unavailable on a fully-instrumented shot without the wG3",
                      kHmKeys[i]);
        check(cat.resolve(QString::fromLatin1(kHmKeys[i]), everyRole).state
                  == MetricAvailability::Unavailable, label);
    }
}

void test_our_own_estimates_are_untouched_by_the_flag()
{
    // The bare keys are OUR estimate, from our own forearm-and-hand IMUs. They must
    // resolve identically whichever instrument is present — the flag names a new
    // capability, it does not withdraw an existing one. (A wG3 capture emits no
    // bare wrist keys, but that is the PRODUCER's choice; the catalogue still says
    // the shot could carry them.)
    const MetricCatalogue cat = makeMetricCatalogue();
    for (int i = 0; i < 2; ++i) {   // leadWristFlexExt, leadWristRadUln — the two-sensor pair
        const auto without = cat.resolve(QString::fromLatin1(kBareKeys[i]), wristShot(false));
        const auto with    = cat.resolve(QString::fromLatin1(kBareKeys[i]), wristShot(true));
        char label[128];
        std::snprintf(label, sizeof(label), "%s resolves the same either way", kBareKeys[i]);
        check(without.state == with.state && without.routeId == with.routeId, label);
        std::snprintf(label, sizeof(label), "%s is Measured from our own IMUs", kBareKeys[i]);
        check(with.state == MetricAvailability::Measured, label);
    }
}

void test_forearm_rotation_is_produced_by_either_vendor()
{
    // The bare `forearmRotation` is a segment axial rotation from the lead-forearm
    // binding ALONE, for either vendor and whether or not an upper arm is mounted.
    // It is the one wrist-group metric a two-sensor rig can produce that
    // forearmPronation cannot stand in for, so it must not have picked up the
    // device requirement from its hm. twin.
    const MetricCatalogue cat = makeMetricCatalogue();
    ShotContext foreOnly;
    foreOnly.sessionType = 1;
    foreOnly.imuRoles    = { SegmentRole::LeadForearm };
    foreOnly.tier        = ReconstructionTier::Mono3DPlusImu;

    check(cat.resolve(QStringLiteral("forearmRotation"), foreOnly).state
              == MetricAvailability::Measured,
          "forearmRotation Measured from a lone forearm IMU, no wG3");
    check(cat.resolve(QStringLiteral("hm.forearmRotation"), foreOnly).state
              == MetricAvailability::Unavailable,
          "...while its measured twin still needs the device");
}

void test_the_directory_hides_the_rungs_a_shot_cannot_have()
{
    // availableOnly is what the metric directory renders. The three rungs appear
    // for a wG3 shot and not for a Witmotion one — the same gate, read through the
    // query path rather than resolve().
    const MetricCatalogue cat = makeMetricCatalogue();
    MetricQuery q;
    q.group         = QStringLiteral("Wrist & forearm");
    q.availableOnly = true;

    const ShotContext wm  = wristShot(false);
    const ShotContext wg3 = wristShot(true);

    auto countHm = [&](const ShotContext &ctx) {
        int n = 0;
        for (const MetricDescriptor *d : cat.query(q, &ctx))
            if (d->key.startsWith(QStringLiteral("hm."))) ++n;
        return n;
    };

    check(countHm(wm) == 0, "no hm. rungs in the directory without the device");
    check(countHm(wg3) == kHmKeyCount, "all three appear with it");

    // The wG3 shot is a superset, not a different set: nothing our own sensors
    // offer is withdrawn by plugging one in.
    check(cat.query(q, &wg3).size() == cat.query(q, &wm).size() + size_t(kHmKeyCount),
          "the device adds three and removes none");
}

void test_the_requirement_reads_the_same_at_the_seam()
{
    // missingForRequirement() is the shared reader every route walks through.
    // Exercised directly so a change there is caught even if no descriptor happened
    // to route through it.
    MetricRequirement req;
    req.hackMotion = true;

    ShotContext without;
    ShotContext with;
    with.hasHackMotion = true;

    const QStringList missing = missingForRequirement(req, without);
    check(missing.size() == 1 && missing.first().contains(QStringLiteral("HackMotion")),
          "a hackMotion requirement is missing exactly one thing without the device");
    check(missingForRequirement(req, with).isEmpty(), "and nothing with it");

    // A requirement that does NOT ask for it is not gated by it either way.
    MetricRequirement plain;
    check(missingForRequirement(plain, without).isEmpty()
              && missingForRequirement(plain, with).isEmpty(),
          "an unrelated requirement ignores the axis");
}

} // namespace

int main()
{
    std::printf("=== HackMotion requirement axis: the gate, not the declaration ===\n");

    test_the_measured_rungs_need_the_device();
    test_the_measured_rungs_are_offered_when_it_is_worn();
    test_bound_roles_alone_do_not_open_the_gate();
    test_our_own_estimates_are_untouched_by_the_flag();
    test_forearm_rotation_is_produced_by_either_vendor();
    test_the_directory_hides_the_rungs_a_shot_cannot_have();
    test_the_requirement_reads_the_same_at_the_seam();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
