// Standalone test for finding confidence — orthogonal to severity, source- and segmentation-aware,
// with low-confidence demotion (not dropping). Design §7.2.
// Run: ctest --test-dir build/analyzer-tests -R confidence_test --output-on-failure

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

// Drop the segmentation confidence of every present checkpoint to `conf`.
static FixtureWristAngleSource withSegConf(FixtureWristAngleSource src, float conf)
{
    PpSwingPositionTimeline tl = src.timeline();
    for (int p = 0; p < kNumPos; ++p)
        if (tl.positions[p].present) tl.positions[p].conf = conf;
    src.setTimeline(tl);
    return src;
}

int main()
{
    std::printf("=== Tier-2 confidence ===\n");
    NormBandProvider provider;

    // 1. Confidence is a fraction of the rule base (data quality < 1 pulls it down), in (0, ruleBase).
    {
        std::printf("-- 1. composition --\n");
        const PpWristAssessmentResult r = WristAssessmentEngine::assess(makeCastSwing(), provider);
        const PpWristFinding *cast = find(r.findings, "cast");
        check(cast && cast->confidence > 0.0f && cast->confidence < 0.85f, "cast confidence ∈ (0, ruleBase)");
    }

    // 2. Higher segmentation confidence → higher finding confidence.
    {
        std::printf("-- 2. segmentation-aware --\n");
        const PpWristAssessmentResult rHi = WristAssessmentEngine::assess(withSegConf(makeCastSwing(), 0.95f), provider);
        const PpWristAssessmentResult rLo = WristAssessmentEngine::assess(withSegConf(makeCastSwing(), 0.40f), provider);
        check(find(rHi.findings, "cast")->confidence > find(rLo.findings, "cast")->confidence,
              "confidence rises with segmentation confidence");
    }

    // 3. Below the floor → demoted (lowConfidence flag), never dropped.
    {
        std::printf("-- 3. demotion --\n");
        const PpWristAssessmentResult rNorm = WristAssessmentEngine::assess(makeCastSwing(), provider);
        const PpWristAssessmentResult rWeak = WristAssessmentEngine::assess(withSegConf(makeCastSwing(), 0.20f), provider);
        const PpWristFinding *normal = find(rNorm.findings, "cast");
        const PpWristFinding *weak   = find(rWeak.findings, "cast");
        check(normal && !normal->lowConfidence, "normal-confidence cast not demoted");
        check(weak && weak->lowConfidence, "weak-segmentation cast demoted (not dropped)");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
