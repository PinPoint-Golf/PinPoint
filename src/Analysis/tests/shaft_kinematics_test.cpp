// Standalone tests for the minimal R6 double-pendulum club predictor
// (src/Analysis/shaft_kinematics.h — wrist-cock knot table, swing progress,
// branch/chirality signs, envelope, predicted rate). Pure std, no fixture.
//
//   cmake --build build/analyzer-tests --target shaft_kinematics_test
//   ctest --test-dir build/analyzer-tests -R shaft_kinematics --output-on-failure

#include "../shaft_kinematics.h"

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }
static double wrapd(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

int main()
{
    // ── knot pins: β̂/σ_β hit the table exactly at the knots ─────────────────
    std::printf("=== knot table ===\n");
    {
        check(near(betaHatDeg(0.00),   8.0, 1e-12), "beta(0.00) = 8");
        check(near(betaHatDeg(0.50),  92.0, 1e-12), "beta(0.50) = 92");
        check(near(betaHatDeg(0.60), 100.0, 1e-12), "beta(0.60) = 100 (peak lag)");
        check(near(betaHatDeg(0.90),   7.0, 1e-12), "beta(0.90) = 7");
        check(near(betaHatDeg(0.95), -27.0, 1e-12), "beta(0.95) = -27 (lead side)");
        check(near(betaHatDeg(1.00), -95.0, 1e-12), "beta(1.00) = -95");
        check(near(sigmaBetaDeg(0.50), 22.0, 1e-12), "sigma(0.50) = 22");
        check(near(sigmaBetaDeg(1.00), 30.0, 1e-12), "sigma(1.00) = 30");
        // linear interpolation between knots: s=0.25 is halfway 0.15→0.35
        check(near(betaHatDeg(0.25), 0.5 * (27.0 + 70.0), 1e-9), "beta(0.25) = midpoint 48.5");
        check(near(sigmaBetaDeg(0.25), 0.5 * (15.0 + 20.0), 1e-9), "sigma(0.25) = midpoint 17.5");
        // clamped outside [0,1]
        check(near(betaHatDeg(-0.5), 8.0, 1e-12) && near(betaHatDeg(1.5), -95.0, 1e-12),
              "s clamped to [0,1]");
    }

    // ── release flip: β̂ crosses zero between the 0.90 and 0.95 knots ────────
    std::printf("=== release sign flip ===\n");
    {
        check(betaHatDeg(0.905) > 0.0, "just after 0.90: still trail side (+)");
        check(betaHatDeg(0.945) < 0.0, "just before 0.95: lead side (-)");
        // the zero itself: 0.90 + 0.05·7/34
        const double s0 = 0.90 + 0.05 * 7.0 / 34.0;
        check(near(betaHatDeg(s0), 0.0, 1e-9), "interpolated zero at s = 0.90 + 0.05*7/34");
    }

    // ── swing progress: anchor mapping + clamps + degenerate anchors ─────────
    std::printf("=== swingProgress ===\n");
    {
        check(near(swingProgress(40, 40, 70, 109, 150), 0.0, 1e-12), "bs0 -> 0");
        check(near(swingProgress(70, 40, 70, 109, 150), 0.5, 1e-12), "top -> 0.5");
        check(near(swingProgress(109, 40, 70, 109, 150), 0.9, 1e-12), "impact -> 0.9");
        check(near(swingProgress(150, 40, 70, 109, 150), 1.0, 1e-12), "fin0 -> 1.0");
        check(near(swingProgress(55, 40, 70, 109, 150), 0.25, 1e-12), "backswing midpoint -> 0.25");
        check(near(swingProgress(0, 40, 70, 109, 150), 0.0, 1e-12), "pre-bs0 clamps to 0");
        check(near(swingProgress(169, 40, 70, 109, 150), 1.0, 1e-12), "post-fin0 clamps to 1");
        // degenerate (equal) anchors never divide by zero and stay bounded
        const double sDeg = swingProgress(50, 50, 50, 50, 50);
        check(sDeg >= 0.0 && sDeg <= 1.0, "degenerate anchors stay in [0,1]");
    }

    // ── branch/chirality: pred = φ + chir·β̂, wrapped [0,360) ────────────────
    std::printf("=== phiClubPredDeg ===\n");
    {
        check(near(phiClubPredDeg(90.0, 0.5, +1), 182.0, 1e-9), "chir=+1: 90 + 92 = 182");
        check(near(phiClubPredDeg(90.0, 0.5, -1), 358.0, 1e-9), "chir=-1: 90 - 92 wraps to 358");
        check(near(phiClubPredDeg(350.0, 0.0, +1), 358.0, 1e-9), "wrap stays in [0,360)");
        // post-release the branch has flipped: chir=+1 now puts the club on the
        // other side of the arm
        check(near(phiClubPredDeg(90.0, 1.0, +1), 355.0, 1e-9), "chir=+1 at s=1: 90 - 95 = 355");
    }

    // ── envelope contains truth under ±8° measured-arm noise ─────────────────
    std::printf("=== envelope ===\n");
    {
        bool contained = true;
        for (int chir : {-1, +1})
            for (double s = 0.0; s <= 1.0001; s += 0.05)
                for (double noise : {-8.0, -4.0, 0.0, 4.0, 8.0}) {
                    const double phiTrue = 137.0;                      // arbitrary
                    const double thTrue  = phiTrue + chir * betaHatDeg(s);
                    const KinEnvelope e  = envelope(s, phiTrue + noise, chir, 3.0);
                    if (std::abs(wrapd(thTrue - e.centerDeg)) > e.halfDeg) contained = false;
                }
        check(contained, "3σ envelope contains truth for all s, both chir, |φ noise| <= 8°");
        // half-width never degenerates to the full circle
        bool bounded = true;
        for (double s = 0.0; s <= 1.0001; s += 0.01)
            if (envelope(s, 0.0, 1, 3.0).halfDeg > 175.0) bounded = false;
        check(bounded, "half-width capped below the half-circle");
    }

    // ── predicted rate: wrap-aware finite difference ─────────────────────────
    std::printf("=== omegaPredDegPerS ===\n");
    {
        check(near(omegaPredDegPerS(10.0, 30.0, 0.01), 2000.0, 1e-6), "20° over 10 ms = 2000°/s");
        check(near(omegaPredDegPerS(350.0, 10.0, 0.1), 200.0, 1e-6), "wrap through 0: +200°/s");
        check(near(omegaPredDegPerS(10.0, 350.0, 0.1), -200.0, 1e-6), "wrap the other way: -200°/s");
        check(near(omegaPredDegPerS(10.0, 30.0, 0.0), 0.0, 1e-12), "dt <= 0 -> 0 (no fabricated rate)");
    }

    // ── mid-downswing rates from the table alone clear the wedge trigger ─────
    // With a realistically moving arm (−500°/s) and the table's release slope,
    // the predicted club rate must exceed the 720°/s trigger well before impact
    // — the whole point of predicting the fan before looking.
    std::printf("=== trigger sanity ===\n");
    {
        const int top = 0, impact = 32;                    // 32 frames at 150 fps
        const double fps = 150.0;
        double peak = 0.0;
        for (int f = top + 1; f < impact; ++f) {
            const double sA = swingProgress(f - 1, -60, top, impact, 60);
            const double sB = swingProgress(f + 1, -60, top, impact, 60);
            const double phiA = 160.0 - 110.0 * (f - 1 - top) / double(impact - top);
            const double phiB = 160.0 - 110.0 * (f + 1 - top) / double(impact - top);
            const double w = omegaPredDegPerS(phiClubPredDeg(phiA, sA, +1),
                                              phiClubPredDeg(phiB, sB, +1), 2.0 / fps);
            peak = std::max(peak, std::abs(w));
        }
        std::printf("  (peak predicted rate %.0f°/s)\n", peak);
        check(peak >= 720.0, "peak |ω̂| clears the 720°/s wedge trigger");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
