// Standalone test for the Tier-2 strengths ("Working well") + selection policy.
// Run: ctest --test-dir build/analyzer-tests -R strengths_test --output-on-failure
//
// A faulty-but-good-elsewhere swing surfaces the adjacent strengths; the curated default suppresses
// strengths on a swing with no fault to protect against; the relaxed flag surfaces them anyway.

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
static bool has(const std::vector<PpWristFinding> &v, const char *id)
{
    for (const PpWristFinding &f : v)
        if (f.id == QLatin1String(id)) return true;
    return false;
}
static int goodCount(const std::vector<PpWristFinding> &v)
{
    int n = 0;
    for (const PpWristFinding &f : v)
        if (f.severity == PpFindingSeverity::Good) ++n;
    return n;
}

int main()
{
    std::printf("=== Tier-2 strengths ===\n");
    NormBandProvider provider;

    // 1. Cast swing — set / face-rotation / width are all good, adjacent to the cast fault → surface.
    {
        std::printf("-- 1. strengths surface adjacent to a fault --\n");
        const auto f = WristAssessmentEngine::assess(makeCastSwing(), provider).findings;
        check(has(f, "set"),           "'wrist set well loaded' surfaces");
        check(has(f, "face_rotation"), "'face rotation on schedule' surfaces");
        check(has(f, "width"),         "'lead arm holds its width' surfaces");
        check(has(f, "cast"),          "the cast fault is still present");
    }

    // 2. Clean swing, curated default → no strengths (nothing to protect against) and no faults.
    {
        std::printf("-- 2. curated default: clean → none --\n");
        const auto f = WristAssessmentEngine::assess(makeCleanSwing(), provider).findings;
        check(goodCount(f) == 0, "curated policy suppresses strengths with no live fault");
    }

    // 3. Clean swing, relaxed policy → all three strengths.
    {
        std::printf("-- 3. relaxed policy: clean → 3 strengths --\n");
        WristAssessmentConfig cfg;
        cfg.rules.strengthsRequireAdjacentFault = false;
        const auto f = WristAssessmentEngine::assess(makeCleanSwing(), provider, cfg).findings;
        check(goodCount(f) == 3, "relaxed policy surfaces all clean strengths");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
