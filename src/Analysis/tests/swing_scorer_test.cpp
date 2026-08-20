// Standalone test for SwingScorer. Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// Verifies the sign-independent scoring MACHINERY: deadband, bounded falloff,
// one-sided clamp, and weighted-geometric-mean aggregation (weakest-link).

#include "../swing_scorer.h"

#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label, int got = -999, int want = -999)
{
    if (got != -999) std::printf("  [%s] %-40s got %4d  want %4d\n", c ? "PASS" : "FAIL", label, got, want);
    else             std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// Build a Wrist metric series whose Impact phase-sample = `impactValue`.
static MetricSeries metric(const char *key, double impactValue)
{
    MetricSeries m;
    m.key = QString::fromLatin1(key); m.label = m.key; m.unit = QStringLiteral("°");
    m.t_us = { 0, 1 }; m.value = { 0.0, impactValue };
    m.phaseSamples.push_back({ Phase::Impact, 1, impactValue, QString() });
    return m;
}

static const ScoredMetric *sm(const ScoreBreakdown &b, const char *key)
{
    for (const ScoredMetric &m : b.metrics)
        if (m.key == QString::fromLatin1(key)) return &m;
    return nullptr;
}

int main()
{
    std::printf("=== SwingScorer (Wrist) ===\n");

    // 1. Every metric on its band centre → all sub-scores 100 → overall 100.
    {
        std::vector<MetricSeries> s = {
            metric("leadWristFlexExt", 15.0),   // mu 15
            metric("leadWristRadUln",   0.0),   // mu 0
            metric("forearmPronation",  0.0),   // mu 0
            metric("leadArmFlexion",    5.0),   // mu 5
        };
        ScoreBreakdown b = SwingScorer::score(s, 1);
        check(b.metrics.size() == 4, "4 metrics scored", int(b.metrics.size()), 4);
        check(b.overall == 100, "all-ideal -> overall 100", b.overall, 100);
        check(sm(b, "leadWristFlexExt") && sm(b, "leadWristFlexExt")->band == QStringLiteral("green"), "ideal band green");
    }

    // 2. Deadband: 1 sigma off still scores 100. (FE sigma 12 → 15+12 = 27.)
    {
        std::vector<MetricSeries> s = { metric("leadWristFlexExt", 27.0) };
        ScoreBreakdown b = SwingScorer::score(s, 1);
        check(sm(b, "leadWristFlexExt")->subScore == 100.0, "1 sigma within deadband -> 100");
    }

    // 3. One-sided: FE penalises BELOW mu only. Above mu (more flexion) stays 100;
    //    far below mu (extension/cupping) is penalised.
    {
        ScoreBreakdown hi = SwingScorer::score({ metric("leadWristFlexExt", 60.0) }, 1);  // way above mu
        check(sm(hi, "leadWristFlexExt")->subScore == 100.0, "one-sided: above mu clamped to 100");
        ScoreBreakdown lo = SwingScorer::score({ metric("leadWristFlexExt", -30.0) }, 1); // far below mu (z<-3)
        check(sm(lo, "leadWristFlexExt")->subScore <= 2.0, "one-sided: far below mu -> ~0");
        check(sm(lo, "leadWristFlexExt")->band == QStringLiteral("red"), "far-below band red");
    }

    // 4. Weakest-link: one catastrophic metric drags overall far below the arithmetic mean.
    {
        std::vector<MetricSeries> s = {
            metric("leadWristFlexExt", -30.0),  // far below mu (one-sided) -> ~1 (catastrophic)
            metric("leadWristRadUln",   0.0),   // 100
            metric("forearmPronation",  0.0),   // 100
            metric("leadArmFlexion",    5.0),   // 100
        };
        ScoreBreakdown b = SwingScorer::score(s, 1);
        // arithmetic mean ~55; geometric (weakest-link) must be far lower.
        check(b.overall < 30, "weakest-link drags overall far below arithmetic mean", b.overall, -999);
        check(b.perRegion.value(QStringLiteral("wrist")) < b.perRegion.value(QStringLiteral("arm")),
              "wrist region (with the fault) scores below arm region");
    }

    // 5. Unknown session type → empty breakdown, overall 0.
    {
        ScoreBreakdown b = SwingScorer::score({ metric("leadWristFlexExt", 15.0) }, 0);
        check(b.metrics.empty() && b.overall == 0, "unknown session -> empty/0");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
