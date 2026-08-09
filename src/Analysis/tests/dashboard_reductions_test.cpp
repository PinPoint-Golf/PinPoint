// Standalone tests for the corridor-bar reduction (src/Analysis/
// dashboard_reductions.h): the corridor bar's value→x domain, two-sided and
// one-sided. Pure, header-only — no OpenCV, no fixture. Own main()/check() macros.
//
//   cmake --build build/analyzer-tests --target dashboard_reductions_test
//   ctest --test-dir build/analyzer-tests -R dashboard_reductions --output-on-failure

#include "../dashboard_reductions.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

int main()
{
    std::printf("dashboard_reductions_test\n");

    // ── barDomain: the horizontal corridor bar's value→x domain ─────────────────
    //
    // Gated in BOTH directions throughout: every one-sided assertion has a two-sided
    // counterpart proving the new rule does NOT reach the 105 measures that are still
    // ordinary corridors. The two-sided numbers are pinned exactly, because "unchanged"
    // is the whole claim on that side.
    {
        // Two-sided, the shipped case: amber 10..20 padded 12% each side. Bit-identical
        // to what NormativeBar computed in QML before this existed.
        const BarDomain d = barDomain(/*gLo*/12, /*gHi*/18, /*aLo*/10, /*aHi*/20,
                                      false, false, 15.0, true);
        check(d.valid, "two-sided corridor → valid domain");
        check(near(d.lo, 10.0 - 10.0 * 0.12, 1e-9), "two-sided lo = amberLo − 12% of the amber span");
        check(near(d.hi, 20.0 + 10.0 * 0.12, 1e-9), "two-sided hi = amberHi + 12% of the amber span");

        // The reading does not widen a CLOSED side — it clamps to the track, as always.
        const BarDomain far = barDomain(12, 18, 10, 20, false, false, 900.0, true);
        check(near(far.lo, d.lo, 1e-9) && near(far.hi, d.hi, 1e-9),
              "a wild reading never moves a closed edge (the marker clamps instead)");
    }
    {
        // A FLOOR — the live shape: m_smashFactor @ driver, mu 1.48, sigmaLo 0.05 under
        // the standard policy. greenHi and amberHi both collapse to mu, which is the
        // aspiration and not a bound.
        const double mu = 1.48;
        const BarDomain d = barDomain(/*gLo*/1.43, /*gHi*/mu, /*aLo*/1.33, /*aHi*/mu,
                                      /*lowOpen*/false, /*highOpen*/true, 1.46, true);
        check(d.valid, "floor → valid domain");
        check(near(d.lo, 1.33 - 0.15 * 0.12, 1e-9),
              "the GRADED side of a floor keeps the ordinary pad");
        check(d.hi > mu + 1e-6, "the open side runs PAST the aspiration, not up to it");
        check(near(d.hi, mu + 0.15 * 0.35, 1e-9), "…by 35% of the graded span");

        // The defect this stage exists to fix: an Ideal reading above mu must sit inside
        // the domain with room to spare, not pinned to the last pixel of the track.
        const BarDomain ideal = barDomain(1.43, mu, 1.33, mu, false, true, 1.55, true);
        check(ideal.hi > 1.55, "a reading above the aspiration is INSIDE the domain, not clamped");
        check(near(ideal.hi, 1.55 + 0.15 * 0.35, 1e-9),
              "…and the open side anchors past the reading when the reading is the furthest");
        const BarDomain further = barDomain(1.43, mu, 1.33, mu, false, true, 1.62, true);
        check(further.hi > ideal.hi,
              "a further reading pushes the open edge further — it never stops tracking");
        check(near(further.lo, ideal.lo, 1e-9),
              "…while the graded edge stays put, so the corridor does not shrink to a sliver");
    }
    {
        // The CEILING mirror, which the bar could not express at all before.
        const double mu = 2.0;
        const BarDomain d = barDomain(/*gLo*/mu, /*gHi*/3.0, /*aLo*/mu, /*aHi*/6.0,
                                      /*lowOpen*/true, /*highOpen*/false, 1.2, true);
        check(d.valid, "ceiling → valid domain");
        check(near(d.hi, 6.0 + 4.0 * 0.12, 1e-9), "the graded side of a ceiling keeps the pad");
        check(d.lo < 1.2, "the open side runs past a reading BELOW the aspiration");
        check(near(d.lo, 1.2 - 4.0 * 0.35, 1e-9), "…by 35% of the graded span, mirrored");

        const BarDomain atMu = barDomain(mu, 3.0, mu, 6.0, true, false, 2.5, true);
        check(near(atMu.lo, mu - 4.0 * 0.35, 1e-9),
              "a reading inside the corridor leaves the aspiration as the anchor");
    }
    {
        // Degenerate ladder — amber → green → value±1 — and it must survive one-sided.
        const BarDomain g = barDomain(/*gLo*/4, /*gHi*/6, /*aLo*/0, /*aHi*/0, false, false, 5.0, true);
        check(near(g.lo, 4.0 - 2.0 * 0.12, 1e-9) && near(g.hi, 6.0 + 2.0 * 0.12, 1e-9),
              "degenerate amber falls back to the green band");

        const BarDomain v = barDomain(0, 0, 0, 0, false, false, 7.0, true);
        check(near(v.lo, 6.0 - 2.0 * 0.12, 1e-9) && near(v.hi, 8.0 + 2.0 * 0.12, 1e-9),
              "no corridor at all falls back to value±1");

        const BarDomain gOpen = barDomain(4, 6, 0, 0, false, true, 5.0, true);
        check(near(gOpen.hi, 6.0 + 2.0 * 0.35, 1e-9),
              "the green-band fallback still opens its ungraded side");

        // Zero-sigma: every edge collapses onto mu. The span floor keeps the domain from
        // inverting, and a bar with hi <= lo is what a divide-by-zero looks like on screen.
        const BarDomain z = barDomain(1.48, 1.48, 1.48, 1.48, false, true, 1.48, true);
        check(!z.valid || z.hi > z.lo, "a zero-sigma one-sided norm never inverts its domain");
    }
    {
        // No reading, and non-finite readings. A corridor with the marker hidden
        // (hasValue false) must still scale on the corridor alone.
        const BarDomain noVal = barDomain(12, 18, 10, 20, false, false, 0.0, false);
        check(near(noVal.lo, 10.0 - 10.0 * 0.12, 1e-9) && near(noVal.hi, 20.0 + 10.0 * 0.12, 1e-9),
              "hasValue=false scales to the corridor alone");
        const BarDomain openNoVal = barDomain(1.43, 1.48, 1.33, 1.48, false, true, 0.0, false);
        check(near(openNoVal.hi, 1.48 + 0.15 * 0.35, 1e-9),
              "…and a floor with no reading still opens past its aspiration");

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const BarDomain nanVal = barDomain(1.43, 1.48, 1.33, 1.48, false, true, nan, true);
        check(near(nanVal.hi, openNoVal.hi, 1e-9),
              "a NaN reading is ignored rather than poisoning the domain");

        // Nothing finite anywhere ⇒ invalid, so the caller hides the bands instead of
        // drawing one pinned to the left edge (which is what _fx() does with a NaN).
        check(!barDomain(nan, nan, nan, nan, false, false, nan, true).valid,
              "no corridor and no finite reading → invalid, nothing to draw");
        check(!barDomain(0, 0, 0, 0, false, false, nan, true).valid,
              "an empty corridor with a NaN reading → invalid");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
