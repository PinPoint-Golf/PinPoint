// Standalone test for Tier-1 banding via WristAssessmentEngine::assess().
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests -j4
//   ctest --test-dir build/analyzer-tests -R tier1_banding_test --output-on-failure
//
// Verifies the per-cell RAG matrix: P1 → Ref, in-band → Green, margin → Amber, outside → Red,
// gap/indeterminate/no-corridor → Grey, and that un-instrumented DOFs are absent (present=false,
// all Grey). The demo (mockup) swing is spot-checked on BAND-CONSISTENT cells — the mockup's own
// rag[] arrays were hand-authored and are NOT reproducible from a clean band rule, so they are not
// asserted verbatim.

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

static PpRag rag(const PpWristAssessmentResult &r, PpJointDof d, PpSwingPosition p)
{
    return r.row(d).cells[static_cast<int>(p)].rag;
}

int main()
{
    std::printf("=== Tier-1 banding (assess) ===\n");
    NormBandProvider provider;

    // Clean swing → every assessable cell is Green; P1 is Ref; trail-wrist P8 (no corridor) is Grey.
    {
        std::printf("-- 1. clean swing → all Green --\n");
        const PpWristAssessmentResult r = WristAssessmentEngine::assess(makeCleanSwing(), provider);
        bool allGreen = true;
        for (PpJointDof d : { PpJointDof::LeadWristRadUln, PpJointDof::LeadWristFlexExt,
                              PpJointDof::LeadForearmRot, PpJointDof::TrailWristFlexExt,
                              PpJointDof::LeadElbowFlex }) {
            for (int p = 1; p < kNumPos; ++p) {           // skip P1 (Ref)
                const PpRag g = rag(r, d, static_cast<PpSwingPosition>(p));
                const bool gapP8 = (d == PpJointDof::TrailWristFlexExt && p == 7);
                if (gapP8) { if (g != PpRag::Grey) allGreen = false; }
                else if (g != PpRag::Green) allGreen = false;
            }
        }
        check(allGreen, "clean: all assessable cells Green (trail P8 Grey)");
        check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P1) == PpRag::Ref, "P1 is Ref");
        check(rag(r, PpJointDof::TrailWristFlexExt, PpSwingPosition::P8) == PpRag::Grey,
              "trail wrist P8 (no corridor) → Grey");
    }

    // Un-instrumented DOF → present=false, every cell Grey (P1 still Ref).
    {
        std::printf("-- 2. un-instrumented DOF --\n");
        const PpWristAssessmentResult r = WristAssessmentEngine::assess(makeCleanSwing(), provider);
        const PpDofRow &row = r.row(PpJointDof::TrailForearmRot);
        check(!row.present, "trail forearm rotation not present");
        bool greyAfterRef = true;
        for (int p = 1; p < kNumPos; ++p)
            if (row.cells[p].rag != PpRag::Grey) greyAfterRef = false;
        check(greyAfterRef, "un-instrumented DOF → Grey cells");
    }

    // Demo (mockup) swing — band-consistent spot checks (NOT the hand-authored mockup rag[]).
    {
        std::printf("-- 3. demo swing spot checks --\n");
        const PpWristAssessmentResult r = WristAssessmentEngine::assess(makeMockupDemoSwing(), provider);
        // radUln @P6: Δ 14 vs green [20,40] amber [15,45] → below amber → Red.
        check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P6) == PpRag::Red,
              "demo radUln P6 (lag dumped) → Red");
        // radUln @P3: Δ 25 vs green [18,38] → Green.
        check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P3) == PpRag::Green,
              "demo radUln P3 → Green");
        // flexExt @P3: Δ −8 vs green [−7,11] amber [−12,16] → just below green → Amber.
        check(rag(r, PpJointDof::LeadWristFlexExt, PpSwingPosition::P3) == PpRag::Amber,
              "demo flexExt P3 → Amber");
        // trail wrist @P8 is a gap → Grey.
        check(rag(r, PpJointDof::TrailWristFlexExt, PpSwingPosition::P8) == PpRag::Grey,
              "demo trail wrist P8 → Grey");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
