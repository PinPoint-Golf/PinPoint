// Standalone tests for the launch monitor ball flight model (src/Analysis/
// lm_flight_path.h): the drag + Magnus integration against the design brief's reference
// shot, the asymmetry that makes the curve worth drawing at all, and the normalisation
// that pins the drawn endpoints to the device's reported ones. Pure — no Qt, no OpenCV,
// no fixture. Own main()/check() macros.
//
//   cmake --build build/analysis-tests --target lm_flight_path_test
//   ctest --test-dir build/analysis-tests -R lm_flight_path --output-on-failure

#include "../lm_flight_path.h"

#include <algorithm>
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
static void checkNear(double got, double want, double tol, const char *label)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("  [%s] %s (got %.4f, want %.4f ± %.4f)\n",
                ok ? "PASS" : "FAIL", label, got, want, tol);
    if (!ok) ++g_fail;
}

int main()
{
    std::printf("lm_flight_path_test\n");

    // ── The reference shot ──────────────────────────────────────────────────────
    //
    // The design brief's own row, and the reason the coefficients are what they are.
    // A change to LmFlightModel that does not move these is a change to a comment; a
    // change that does move them needs a better source than the one that set them.
    const LmLaunch ref{ 118.1, 13.2, -1.3, 4686.0, 4.7 };
    {
        const LmFlightIntegration f = lmIntegrateFlight(ref);
        check(f.has, "reference shot flies");
        checkNear(f.carryYd,   165.8, 0.6, "reference carry (device says 166.6)");
        checkNear(f.apexFt,     64.2, 0.6, "reference apex (device says 63)");
        checkNear(f.descentDeg, 37.4, 0.4, "reference descent (device says 37.3)");
        checkNear(f.offlineYd,  1.63, 0.1, "reference offline from a 4.7 deg axis");

        // The shape claim the whole card rests on. If the apex ever drifts to the
        // middle, the drawing has stopped being a model and gone back to being an arch.
        check(f.apexFraction > 0.55 && f.apexFraction < 0.70,
              "apex sits in 0.55-0.70 of carry for a normal iron");
        check(f.descentDeg > f.launchDeg + 15.0,
              "descent is markedly steeper than launch");
    }

    // ── x is monotonic ──────────────────────────────────────────────────────────
    //
    // The ball never travels backwards downrange. Cheap to assert and it is the first
    // symptom of an integration that has gone unstable at a larger step.
    {
        const LmFlightIntegration f = lmIntegrateFlight(ref);
        bool mono = true;
        for (size_t i = 1; i < f.points.size(); ++i)
            if (f.points[i].x < f.points[i - 1].x - 1e-9) mono = false;
        check(mono, "downrange distance is monotonic");
        check(f.points.size() > 100, "the polyline has enough points to draw");
    }

    // ── Drag-free, spin-free: an exact parabola ─────────────────────────────────
    //
    // The integrator's own correctness check, separated from the aerodynamics. With no
    // drag and no lift the answer is closed-form, so the apex must land at exactly half
    // the carry and the ball must come down at the angle it left at.
    //
    // NOTE this is NOT the brief's "zero spin gives a symmetric parabola" test, which
    // cannot hold as written: a zero-spin ball still has drag, and drag alone already
    // breaks the symmetry (see the case below). Symmetry is a property of the vacuum
    // case, and asserting it of the zero-spin case would have been asserting a bug.
    {
        LmFlightModel vac;
        vac.cd0 = 0.0; vac.cdS = 0.0; vac.clS = 0.0; vac.clS2 = 0.0; vac.clMax = 0.0;
        const LmFlightIntegration f = lmIntegrateFlight({ 118.1, 13.2, 0.0, 0.0, 0.0 }, vac);
        check(f.has, "vacuum shot flies");
        checkNear(f.apexFraction, 0.5,  0.005, "vacuum apex at half the carry");
        checkNear(f.descentDeg,   13.2, 0.10,  "vacuum descent equals launch");
    }

    // ── Zero spin, with drag: where the asymmetry actually comes from ───────────
    //
    // Drag alone already breaks the symmetry — it steepens the descent and nudges the
    // apex past the midpoint — but only slightly. It is LIFT that carries the apex out
    // to the ~63% the card is built around. Asserting both halves separately is what
    // stops the lift term being quietly dropped as "close enough": without it the curve
    // would still lean the right way and would lean nowhere near far enough.
    {
        const LmFlightIntegration drag = lmIntegrateFlight({ 118.1, 13.2, 0.0, 0.0, 0.0 });
        const LmFlightIntegration full = lmIntegrateFlight(ref);
        check(drag.has, "zero-spin shot flies");
        check(drag.apexFraction > 0.5, "drag alone leans the apex past the midpoint");
        check(drag.apexFraction < full.apexFraction - 0.05,
              "but lift is what carries it out to 63%");
        check(drag.descentDeg > 13.2, "drag alone still steepens the descent");
        check(std::fabs(drag.offlineYd) < 0.01, "no spin, no sideways");
        check(drag.carryYd < 120.0, "no lift, far less carry");
    }

    // ── Spin axis drives the side, and its sign ─────────────────────────────────
    {
        const LmFlightIntegration r = lmIntegrateFlight({ 118.1, 13.2, 0.0, 4686.0,  12.0 });
        const LmFlightIntegration l = lmIntegrateFlight({ 118.1, 13.2, 0.0, 4686.0, -12.0 });
        check(r.offlineYd > 0.0, "a right-tilted axis finishes right");
        check(l.offlineYd < 0.0, "a left-tilted axis finishes left");
        checkNear(r.offlineYd, -l.offlineYd, 0.05, "the two mirror each other");
    }

    // ── Normalisation lands the endpoints exactly ───────────────────────────────
    //
    // The contract with the card: the SHAPE is the model's, the ENDPOINTS are the
    // device's. Every one of these is exact, not approximate — a drawn ball that lands
    // a pixel off the carry label it sits under is the defect this replaced.
    {
        const LmFlightIntegration raw = lmIntegrateFlight(ref);
        const LmFlightPath p = lmNormalisedPath(raw, 166.6, 63.0, 5.1, 181.9);
        check(p.has, "normalised path exists");

        checkNear(p.carryYd,   166.6, 1e-9, "carry is the reported carry");
        checkNear(p.apexFt,     63.0, 1e-9, "apex is the reported apex");
        checkNear(p.offlineYd,   5.1, 1e-9, "offline is the reported offline");
        checkNear(p.totalYd,   181.9, 1e-9, "total is the reported total");

        // 166.6 / 181.9 — the trajectory ends where the roll begins, on one shared axis.
        checkNear(p.carryFraction, 166.6 / 181.9, 1e-6, "carry fraction of total");
        checkNear(p.finish.x, 1.0, 1e-9, "the roll finishes at x = 1");
        checkNear(p.finish.y, 0.0, 1e-9, "the roll finishes on the ground");

        double maxY = 0.0, minY = 1.0;
        bool inBand = true;
        for (const LmPoint &q : p.points) {
            maxY = std::max(maxY, q.y);
            minY = std::min(minY, q.y);
            if (q.x < -1e-9 || q.x > 1.0 + 1e-9 || std::fabs(q.z) > 1.0 + 1e-9)
                inBand = false;
        }
        checkNear(maxY, 1.0, 1e-9, "apex normalises to exactly 1");
        check(minY >= -1e-9, "nothing dips below the ground");
        check(inBand, "every point is inside the normalised box");

        // The landing's lateral position, taken back out of normalised space, is the
        // reported offline. This is the assertion that catches a lateral scale applied
        // to the wrong extent.
        checkNear(p.landing.z * p.lateralExtentYd, 5.1, 1e-6, "landing z decodes to 5.1 yd");
        checkNear(p.apexFractionOfCarry, raw.apexFraction, 1e-12, "apex fraction survives");
        checkNear(p.apexAtX, p.carryFraction * raw.apexFraction, 1e-12, "apex x is consistent");

        // The residual the brief asks to be surfaced rather than hidden.
        checkNear(p.residualOfflineYd, 5.1 - raw.offlineYd, 1e-9, "offline residual is kept");
        check(p.residualOfflineYd > 2.0, "the reference shot's residual is the real one");

        // Tangents: leaving shallower than it arrives, in the drawn space.
        check(p.launchTangent.y > 0.0,  "launch tangent climbs");
        check(p.landingTangent.y < 0.0, "landing tangent descends");
    }

    // ── A dead-straight model shot still draws a curved-out finish ──────────────
    //
    // Zero spin axis means the model finishes on the target line, but the device may
    // still report an offline (wind, or a soft axis reading). Dividing by the model's
    // zero would be the obvious bug; the fallback is a linear drift, which is the only
    // defensible line through "started straight, finished 5 yd right".
    {
        const LmFlightIntegration raw = lmIntegrateFlight({ 118.1, 13.2, 0.0, 4686.0, 0.0 });
        check(std::fabs(raw.offlineYd) < 1e-6, "model shot is dead straight");
        const LmFlightPath p = lmNormalisedPath(raw, 166.6, 63.0, 5.0, 181.9);
        check(p.has, "straight model + offset report still normalises");
        checkNear(p.landing.z * p.lateralExtentYd, 5.0, 1e-6, "landing still decodes to 5.0 yd");
        bool finite = true;
        for (const LmPoint &q : p.points)
            if (!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)) finite = false;
        check(finite, "no division by a zero lateral extent");
    }

    // ── Missing reports fall back to the model, never to zero ───────────────────
    {
        const LmFlightIntegration raw = lmIntegrateFlight(ref);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const LmFlightPath p = lmNormalisedPath(raw, nan, nan, nan, nan);
        check(p.has, "a device that reported no distances still draws");
        checkNear(p.carryYd, raw.carryYd, 1e-9, "carry falls back to the model's");
        checkNear(p.apexFt,  raw.apexFt,  1e-9, "apex falls back to the model's");
        checkNear(p.totalYd, p.carryYd,   1e-9, "no total means no roll");
        checkNear(p.carryFraction, 1.0,   1e-9, "and the trajectory fills the axis");
    }

    // ── Nothing in, nothing out ─────────────────────────────────────────────────
    //
    // The same rule as the reductions header: absence is not zero. A shot the monitor
    // missed must produce no path, so the card draws no flight rather than a flat line
    // that claims the ball never left the ground.
    {
        check(!lmIntegrateFlight({ 0.0, 13.2, 0.0, 4686.0, 4.7 }).has, "no ball speed, no path");
        check(!lmIntegrateFlight({ 118.1, 0.0, 0.0, 4686.0, 4.7 }).has, "no launch angle, no path");
        check(!lmIntegrateFlight({ 118.1, -3.0, 0.0, 4686.0, 4.7 }).has, "negative launch, no path");
        check(!lmNormalisedPath(LmFlightIntegration{}, 166.6, 63.0, 5.1, 181.9).has,
              "no integration, no normalised path");
    }

    std::printf("%s\n", g_fail == 0 ? "OK" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
