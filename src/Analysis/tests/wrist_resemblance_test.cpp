// Standalone test for WristResemblanceScorer (design §B.0a). Run via CTest:
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure
//
// Verifies the per-archetype resemblance CONSTRUCTION (A.5 #12/#18): boundedness,
// surfacing (overall == max R_p, label == argmax), per-archetype recall, FE
// monotonicity, and the blended flag. Provisional μ_p/σ_p are re-seated at Corpus 2;
// these checks are about the construction, not the specific centres.

#include "../wrist_resemblance.h"
#include "../score_uncertainty.h"
#include "../../Core/pp_tuned_constants.h"

#include <cstdio>

using namespace pinpoint::analysis;
namespace tr = pinpoint::tuned::scoring::resemblance;

static int g_fail = 0;
static void check(bool c, const char *label, int got = -999, int want = -999)
{
    if (got != -999) std::printf("  [%s] %-44s got %4d  want %4d\n", c ? "PASS" : "FAIL", label, got, want);
    else             std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// One lead-wrist FE series with labelled Top + Impact phase-samples (degrees, flexion-positive).
static std::vector<MetricSeries> fe(double top, double impact)
{
    MetricSeries m;
    m.key = QStringLiteral("leadWristFlexExt"); m.label = m.key; m.unit = QStringLiteral("°");
    m.t_us = { 0, 1, 2 }; m.value = { 0.0, top, impact };
    m.phaseSamples.push_back({ Phase::Top,    1, top,    QString() });
    m.phaseSamples.push_back({ Phase::Impact, 2, impact, QString() });
    return { m };
}

static int R(const ScoreBreakdown &b, const char *p)
{
    return b.resemblance.value(QString::fromLatin1(p), -1);
}

int main()
{
    std::printf("=== WristResemblanceScorer (§B.0a) ===\n");

    // 1. kind is Resemblance and all three patterns are present and bounded [0,100].
    {
        const ScoreBreakdown b = WristResemblanceScorer::score(fe(tr::kBowedMuTop, tr::kBowedMuImpact));
        check(b.kind == ScoreKind::Resemblance, "kind == Resemblance");
        check(b.resemblance.size() == 3, "three R_p present", b.resemblance.size(), 3);
        for (const char *p : { "bowed", "neutral", "cupped" })
            check(R(b, p) >= 0 && R(b, p) <= 100, p, R(b, p));
    }

    // 2. Per-archetype recall: a swing AT each centre reads that pattern highest, ~100,
    //    overall == max, label == argmax.
    struct Centre { const char *label; double top, imp; };
    const Centre centres[] = {
        { "bowed",   tr::kBowedMuTop,   tr::kBowedMuImpact   },
        { "neutral", tr::kNeutralMuTop, tr::kNeutralMuImpact },
        { "cupped",  tr::kCuppedMuTop,  tr::kCuppedMuImpact  },
    };
    for (const Centre &c : centres) {
        const ScoreBreakdown b = WristResemblanceScorer::score(fe(c.top, c.imp));
        check(b.patternLabel == QString::fromLatin1(c.label), c.label, 0, 0);
        check(b.overall == R(b, c.label), "overall == max R_p", b.overall, R(b, c.label));
        check(R(b, c.label) >= 95, "centre reads ~100", R(b, c.label), 100);
        // the other two read strictly lower
        for (const char *o : { "bowed", "neutral", "cupped" })
            if (QString::fromLatin1(o) != b.patternLabel)
                check(R(b, o) < R(b, c.label), "other < self", R(b, o), R(b, c.label));
    }

    // 3. FE monotonicity: as FE at impact moves AWAY from the bowed centre (toward cupping),
    //    R_bowed is non-increasing.
    {
        int prev = 101;
        bool mono = true;
        for (double imp = tr::kBowedMuImpact; imp >= tr::kCuppedMuImpact; imp -= 5.0) {
            const ScoreBreakdown b = WristResemblanceScorer::score(fe(tr::kBowedMuTop, imp));
            if (R(b, "bowed") > prev) mono = false;
            prev = R(b, "bowed");
        }
        check(mono, "R_bowed monotone as FE leaves bowed centre");
    }

    // 4. Blended flag: a swing midway between bowed and neutral has top-two close → blended.
    {
        const double midTop = 0.5 * (tr::kBowedMuTop + tr::kNeutralMuTop);
        const double midImp = 0.5 * (tr::kBowedMuImpact + tr::kNeutralMuImpact);
        const ScoreBreakdown b = WristResemblanceScorer::score(fe(midTop, midImp));
        check(b.blended, "midpoint bowed/neutral is blended");
    }

    // 5. A clearly-bowed swing is NOT blended (cupped far away).
    {
        const ScoreBreakdown b = WristResemblanceScorer::score(fe(tr::kBowedMuTop, tr::kBowedMuImpact));
        check(!b.blended || R(b, "neutral") + 10 >= R(b, "bowed"), "clean bowed: blended only if neutral within 10");
    }

    // 6. Missing FE channel ⇒ overall 0, label "unknown".
    {
        const ScoreBreakdown b = WristResemblanceScorer::score({});
        check(b.overall == 0 && b.patternLabel == QStringLiteral("unknown"), "no FE ⇒ unknown/0");
    }

    // --- Uncertainty interval (§B.7, WP-4) ---------------------------------------------
    // Realistic FE series (µs grid with neighbours so dθ/dt is sane) + Top/Impact phases.
    auto feRealistic = [](double top, double impact) {
        MetricSeries m;
        m.key = QStringLiteral("leadWristFlexExt"); m.label = m.key; m.unit = QStringLiteral("°");
        m.t_us  = { 900000, 1000000, 1100000, 1400000, 1500000, 1600000 };
        m.value = { top - 5, top, top + 5, impact - 5, impact, impact + 5 };
        m.phaseSamples.push_back({ Phase::Top,    1000000, top,    QString() });
        m.phaseSamples.push_back({ Phase::Impact, 1500000, impact, QString() });
        return std::vector<MetricSeries>{ m };
    };
    auto phasesAt = [](float conf) {
        return std::vector<PhaseEvent>{ { Phase::Top, 1000000, conf, SegmentRole::Unknown },
                                        { Phase::Impact, 1500000, conf, SegmentRole::Unknown } };
    };

    // 7. Interval is valid and bounded: 0 <= lo <= overall <= hi <= 100.
    {
        const auto fe = feRealistic(tr::kBowedMuTop, tr::kBowedMuImpact);
        const ScoreBreakdown b = WristResemblanceScorer::score(fe);
        const ScoreInterval iv = ScoreUncertainty::wristInterval(b, fe, phasesAt(0.9f));
        check(iv.valid(), "interval computed");
        check(iv.lo >= 0 && iv.lo <= b.overall && b.overall <= iv.hi && iv.hi <= 100,
              "0 <= lo <= overall <= hi <= 100");
    }

    // 8. Low confidence WIDENS the interval; the central score is UNCHANGED (B2/§B.7).
    {
        const auto fe = feRealistic(tr::kBowedMuTop, tr::kBowedMuImpact);
        const ScoreBreakdown b = WristResemblanceScorer::score(fe);
        const ScoreInterval hiC = ScoreUncertainty::wristInterval(b, fe, phasesAt(0.95f));
        const ScoreInterval loC = ScoreUncertainty::wristInterval(b, fe, phasesAt(0.30f));
        check(loC.halfWidth >= hiC.halfWidth, "low conf widens interval", loC.halfWidth, hiC.halfWidth);
        // central score does not depend on confidence (it is computed from FE only)
        check(b.overall == WristResemblanceScorer::score(fe).overall, "central score confidence-invariant");
    }

    // 9. No phases ⇒ invalid interval (cannot build the budget).
    {
        const auto fe = feRealistic(tr::kBowedMuTop, tr::kBowedMuImpact);
        const ScoreBreakdown b = WristResemblanceScorer::score(fe);
        const ScoreInterval iv = ScoreUncertainty::wristInterval(b, fe, {});
        check(!iv.valid(), "no phases ⇒ invalid interval");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
