// Standalone tests for the norm value types and the grade rule (src/Diagnostics/norm.h).
//
// Two behaviours carry the whole design and are tested hardest:
//
//   1. z is computed PER SIDE, so an asymmetric norm grades asymmetrically. A norm that tolerated
//      the same deviation either way could not express ball position at all.
//   2. An explicit monitor band DOMINATES the z-derived outer edge in both directions — outside it
//      is always Action, inside it is never Action. That is the rule that makes the 39 corridors
//      migrated out of reference_bands.cpp reproduce byte-identically, and every case below that
//      looks fussy is there because it is a case the migration actually hits.
//
//   cmake --build build/analyzer-tests --target norm_test
//   ctest --test-dir build/analyzer-tests -R norm_test --output-on-failure

#include "../norm.h"

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

static Norm symmetric(double mu, double sigma)
{
    Norm n;
    n.measureId = QStringLiteral("m_test");
    n.contextId = QStringLiteral("full_swing");
    n.mu        = mu;
    n.sigmaLo   = sigma;
    n.sigmaHi   = sigma;
    return n;
}

int main()
{
    std::printf("=== norm: z per side ===\n");
    {
        Norm n   = symmetric(10.0, 2.0);
        n.sigmaLo = 1.0;   // tight below, loose above — ball position's shape

        check(near(normZ(10.0, n), 0.0), "at the centre, z is zero");
        check(near(normZ(12.0, n), 1.0), "one sigmaHi above -> z = +1");
        check(near(normZ(9.0,  n), -1.0), "one sigmaLo below -> z = -1");
        check(near(normZ(8.0,  n), -2.0), "two sigmaLo below -> z = -2");
        // The point of the asymmetry: the same absolute deviation grades differently either way.
        check(std::fabs(normZ(8.0, n)) > std::fabs(normZ(12.0, n)),
              "equal absolute deviation is worse on the tighter side");
    }

    std::printf("=== norm: degenerate tolerance ===\n");
    {
        Norm n = symmetric(5.0, 0.0);
        check(near(normZ(5.0, n), 0.0), "zero sigma: the centre itself is still z = 0");
        check(std::isinf(normZ(5.1, n)), "zero sigma: anything off-centre is infinitely far");
        check(grade(5.1, n) == Grade::Action, "zero sigma: off-centre grades Action, not a crash");
    }

    std::printf("=== norm: grade boundaries (no explicit monitor) ===\n");
    {
        const Norm n = symmetric(0.0, 1.0);

        check(grade(0.0,  n) == Grade::Ideal,  "centre -> Ideal");
        check(grade(1.0,  n) == Grade::Ideal,  "exactly 1 sigma -> Ideal (inclusive)");
        check(grade(-1.0, n) == Grade::Ideal,  "exactly -1 sigma -> Ideal (inclusive)");
        check(grade(1.5,  n) == Grade::Good,   "between 1 and 2 sigma -> Good");
        check(grade(2.0,  n) == Grade::Good,   "exactly 2 sigma -> Good (inclusive)");
        check(grade(2.5,  n) == Grade::Watch,  "between 2 and 3 sigma -> Watch");
        check(grade(3.0,  n) == Grade::Watch,  "exactly 3 sigma -> Watch (inclusive)");
        check(grade(3.01, n) == Grade::Action, "beyond 3 sigma -> Action");
        check(grade(-3.01, n) == Grade::Action, "beyond -3 sigma -> Action");
    }

    std::printf("=== norm: grade policy is honoured ===\n");
    {
        const Norm n = symmetric(0.0, 1.0);
        GradePolicy strict;
        strict.idealMaxZ = 0.5;
        strict.goodMaxZ  = 1.0;
        strict.watchMaxZ = 1.5;

        check(grade(0.75, n, strict) == Grade::Good,   "a tighter policy narrows Ideal");
        check(grade(1.25, n, strict) == Grade::Watch,  "a tighter policy narrows Good");
        check(grade(2.0,  n, strict) == Grade::Action, "a tighter policy reaches Action sooner");
        // The same value under the default policy is two bands better — the policy is genuinely
        // pack-wide and not baked into the norm.
        check(grade(2.0, n) == Grade::Good, "the same value under the default policy is Good");
    }

    std::printf("=== norm: explicit monitor band dominates ===\n");
    {
        // kRadUln P1 as migrated: green +/-3 with a 5.0 margin. sigma = 3, monitor = +/-8, and 3
        // sigma would be 9 — so the monitor band is TIGHTER than the z-derived outer edge and must
        // win, or values between 8 and 9 would grade Watch where the old classifier said Red.
        Norm n     = symmetric(0.0, 3.0);
        n.monitorLo = -8.0;
        n.monitorHi = 8.0;

        check(n.hasExplicitMonitor(), "both monitor bounds set -> explicit");
        check(grade(0.0, n) == Grade::Ideal,   "centre -> Ideal");
        check(grade(3.0, n) == Grade::Ideal,   "green edge (1 sigma) -> Ideal");
        check(grade(4.0, n) == Grade::Good,    "inside the monitor band, 1.33 sigma -> Good");
        check(grade(7.0, n) == Grade::Watch,   "inside the monitor band, 2.33 sigma -> Watch");
        check(grade(8.0, n) == Grade::Watch,   "exactly at the monitor edge -> Watch, not Action");
        check(grade(8.5, n) == Grade::Action,  "past the monitor edge -> Action even at 2.83 sigma");
        check(grade(-8.5, n) == Grade::Action, "past the lower monitor edge -> Action");
    }

    std::printf("=== norm: monitor band caps Action from inside ===\n");
    {
        // The other direction of the same rule. A norm whose monitor band is WIDER than 3 sigma
        // must not grade Action inside it — the old classifier called everything inside amber
        // Amber, however many tolerances out it was.
        Norm n      = symmetric(0.0, 1.0);
        n.monitorLo = -10.0;
        n.monitorHi = 10.0;

        check(grade(5.0, n) == Grade::Watch,
              "5 sigma but inside a wide monitor band -> capped at Watch");
        check(grade(10.0, n) == Grade::Watch, "at the wide monitor edge -> Watch");
        check(grade(10.5, n) == Grade::Action, "past the wide monitor edge -> Action");
    }

    std::printf("=== norm: asymmetric monitor (the tempo shape) ===\n");
    {
        // The tempo corridor is the case that cannot be expressed by any global z policy: green
        // 2.2..3.0 with amber 1.8..3.6 needs a low margin of 0.4 and a high margin of 0.6.
        Norm n      = symmetric(2.6, 0.4);
        n.monitorLo = 1.8;
        n.monitorHi = 3.6;

        check(near(n.idealLo(), 2.2) && near(n.idealHi(), 3.0), "ideal band is the old green band");
        check(grade(2.6, n) == Grade::Ideal,  "centre -> Ideal");
        check(grade(2.2, n) == Grade::Ideal,  "green lower edge -> Ideal");
        check(grade(3.0, n) == Grade::Ideal,  "green upper edge -> Ideal");
        // The two amber edges land in DIFFERENT bands, because the margins are asymmetric while
        // the tolerance is not: 1.8 is exactly 2 sigma below (Good), 3.6 is 2.5 sigma above
        // (Watch). Both are inside the monitor band, so neither is Action — and both collapse to
        // Amber under the legacy 3-band RAG, which is what parity requires. This is the documented
        // consequence of grading a 3-band authored corridor on a 4-band scale: Good and Watch both
        // fall inside the old amber.
        check(grade(1.8, n) == Grade::Good,   "amber lower edge is exactly 2 sigma -> Good");
        check(grade(3.6, n) == Grade::Watch,  "amber upper edge is 2.5 sigma -> Watch");
        check(grade(1.8, n) != Grade::Action && grade(3.6, n) != Grade::Action,
              "inside the monitor band, neither amber edge reaches Action");
        check(grade(1.7, n) == Grade::Action, "below amber -> Action");
        check(grade(3.7, n) == Grade::Action, "above amber -> Action");
        // Asymmetric margins really are asymmetric: 0.4 below, 0.6 above.
        check(near(n.mu - *n.monitorLo, 0.8) && near(*n.monitorHi - n.mu, 1.0),
              "the monitor band is asymmetric about the centre");
    }

    std::printf("=== norm: ideal band edges ===\n");
    {
        Norm n = symmetric(10.0, 0.0);
        n.sigmaLo = 2.0;
        n.sigmaHi = 5.0;
        check(near(n.idealLo(), 8.0),  "idealLo = mu - sigmaLo");
        check(near(n.idealHi(), 15.0), "idealHi = mu + sigmaHi");
    }

    std::printf("=== norm: NotMeasured is never a pass ===\n");
    {
        // The enum's contract, asserted so a future refactor cannot quietly fold NotMeasured into
        // the passing bands. "We could not assess this" and "this is fine" are different claims.
        check(!isDeviation(Grade::NotMeasured), "NotMeasured is not a deviation");
        check(!isDeviation(Grade::Ideal) && !isDeviation(Grade::Good), "Ideal/Good are not deviations");
        check(isDeviation(Grade::Watch) && isDeviation(Grade::Action), "Watch/Action are deviations");
        check(Grade::NotMeasured != Grade::Ideal && Grade::NotMeasured != Grade::Good,
              "NotMeasured is its own band, distinct from every passing grade");
    }

    std::printf("=== norm: enum spellings round-trip ===\n");
    {
        const NormSource sources[] = { NormSource::Heuristic, NormSource::Seated,
                                       NormSource::Literature, NormSource::Imported };
        bool ok = true;
        for (NormSource s : sources) {
            NormSource back{};
            ok = ok && normSourceFromName(normSourceName(s), back) && back == s;
        }
        check(ok, "every NormSource round-trips through its JSON token");

        const Grade grades[] = { Grade::Ideal, Grade::Good, Grade::Watch, Grade::Action,
                                 Grade::NotMeasured };
        ok = true;
        for (Grade g : grades) {
            Grade back{};
            ok = ok && gradeFromName(gradeName(g), back) && back == g;
        }
        check(ok, "every Grade round-trips through its JSON token");

        NormSource dummy{};
        check(!normSourceFromName(QStringLiteral("nonsense"), dummy),
              "an unknown source token is refused, not silently defaulted");

        check(gradeLabel(Grade::Ideal)  == QLatin1String("Ideal")  &&
              gradeLabel(Grade::Good)   == QLatin1String("Good")   &&
              gradeLabel(Grade::Watch)  == QLatin1String("Watch")  &&
              gradeLabel(Grade::Action) == QLatin1String("Action") &&
              gradeLabel(Grade::NotMeasured) == QLatin1String("Not measured"),
              "the user-facing labels are the agreed words");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
