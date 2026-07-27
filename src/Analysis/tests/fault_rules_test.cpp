// Standalone golden test for the Tier-2 fault rules (F1–F8).
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests -j4
//   ctest --test-dir build/analyzer-tests -R fault_rules_test --output-on-failure
//
// Each labelled fault fixture → the expected finding id + severity; the clean fixture → no faults.
// This is the regression net: any change to bands/rules/sampling that alters a labelled swing's
// finding must break this loudly.

#include "../reference_bands.h"
#include "../wrist_assessment_engine.h"
#include "../wrist_assessment_fixtures.h"

#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static const PpWristFinding *find(const std::vector<PpWristFinding> &v, const char *id)
{
    for (const PpWristFinding &f : v)
        if (f.id == QLatin1String(id)) return &f;
    return nullptr;
}

static std::vector<PpWristFinding> assessFindings(const IWristAngleSource &src)
{
    NormBandProvider provider;
    return WristAssessmentEngine::assess(src, provider).findings;
}

static void expectFault(const IWristAngleSource &src, const char *id, PpFindingSeverity sev,
                        const char *label)
{
    const auto f = assessFindings(src);
    const PpWristFinding *hit = find(f, id);
    check(hit != nullptr, label);
    if (hit)
        check(hit->severity == sev, (std::string(label) + " severity").c_str());
}

int main()
{
    std::printf("=== Tier-2 fault rules ===\n");

    expectFault(makeCastSwing(),        "cast",           PpFindingSeverity::Fault, "cast → Fault");
    expectFault(makeFlipSwing(),        "flip",           PpFindingSeverity::Fault, "flip → Fault");
    expectFault(makeOverRotationSwing(),"over_rotation",  PpFindingSeverity::Fault, "over-rotation → Fault");
    expectFault(makeHoldingOffSwing(),  "holding_off",    PpFindingSeverity::Watch, "holding-off → Watch");
    expectFault(makeChickenWingSwing(), "chicken_wing",   PpFindingSeverity::Watch, "chicken-wing → Watch");
    // Open face at the top with clean downstream → suppressed to Watch (see relationship_rules_test).
    expectFault(makeOpenFaceTopSwing(), "open_face_top",  PpFindingSeverity::Watch, "open-face-at-top → Watch (suppressed)");

    // Clean swing → no faults (strengths are suppressed too when there's nothing to protect against).
    {
        std::printf("-- clean → no faults --\n");
        const auto f = assessFindings(makeCleanSwing());
        int faults = 0;
        for (const PpWristFinding &x : f)
            if (x.severity != PpFindingSeverity::Good) ++faults;
        check(faults == 0, "clean swing emits no fault findings");
    }

    // The cast fixture's set is still well-loaded → the cast Fault is reported on the right positions.
    {
        std::printf("-- contributing positions --\n");
        const auto f = assessFindings(makeCastSwing());
        const PpWristFinding *cast = find(f, "cast");
        check(cast && !cast->positions.empty(), "cast carries contributing positions");
        check(cast && cast->dofs.size() == 1 && cast->dofs[0] == PpJointDof::LeadWristRadUln,
              "cast is a lead-wrist radial-ulnar fault");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
