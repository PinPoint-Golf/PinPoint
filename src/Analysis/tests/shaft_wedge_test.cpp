// Standalone tests for the R8-T1 blur-wedge plateau measure
// (src/Analysis/shaft_wedge.h — contiguous-run finder, energy-weighted
// circular centroid, absolute-threshold honesty, arm-smear veto). Pure std,
// synthetic score rows, no fixture.
//
//   cmake --build build/analyzer-tests --target shaft_wedge_test
//   ctest --test-dir build/analyzer-tests -R shaft_wedge --output-on-failure

#include "../shaft_wedge.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static double wrapd(double d)
{
    d = std::fmod(d + 180.0, 360.0);
    if (d < 0) d += 360.0;
    return d - 180.0;
}

// An envelope arc [center-half, center+half] at 1° steps, flat floor value.
static std::vector<float> arcDeg(double centerDeg, int halfBins)
{
    std::vector<float> deg;
    for (int d = -halfBins; d <= halfBins; ++d) {
        double a = std::fmod(centerDeg + d, 360.0);
        if (a < 0) a += 360.0;
        deg.push_back(float(a));
    }
    return deg;
}

int main()
{
    const WedgeConfig cfg;                       // defaults: threshScale 0.5, minSpanDeg 2
    const double kFloor = 20.0;                  // S1 evAbsFloor reference -> thresh 10
    const double kArmVeto = 12.0;                // cfg.armVetoDeg convention
    const double kPhiFar = 200.0;                // arm at 20° — far from every plateau below

    // ── symmetric plateau: centroid at the middle, width = span ─────────────
    std::printf("=== plateau centroid ===\n");
    {
        const std::vector<float> deg = arcDeg(90.0, 30);       // 60..120
        std::vector<float> raw(deg.size(), 5.f);
        for (size_t j = 0; j < deg.size(); ++j)
            if (std::abs(wrapd(double(deg[j]) - 90.0)) <= 2.0) raw[j] = 30.f;   // 88..92
        const WedgeCandidate c = measureWedge(raw, {}, deg, kPhiFar, kArmVeto, kFloor, cfg);
        check(c.ok, "symmetric plateau found");
        check(c.ok && std::abs(wrapd(c.centroidDeg - 90.0)) <= 1.5, "centroid within ±1.5° of 90");
        check(c.ok && std::abs(c.widthDeg - 4.0) <= 1e-9, "width = 4° (5 bins)");
    }

    // ── asymmetric energy pulls the centroid toward the heavy side ──────────
    {
        const std::vector<float> deg = arcDeg(90.0, 30);
        std::vector<float> raw(deg.size(), 0.f);
        for (size_t j = 0; j < deg.size(); ++j) {
            const double o = wrapd(double(deg[j]) - 90.0);
            if (o >= -2.0 && o <= 2.0) raw[j] = float(30.0 + 10.0 * o);   // 10..50 across 88..92
        }
        const WedgeCandidate c = measureWedge(raw, {}, deg, kPhiFar, kArmVeto, kFloor, cfg);
        check(c.ok && c.centroidDeg > 90.0 && c.centroidDeg < 92.0,
              "energy-weighted centroid shifts toward the brighter flank");
    }

    // ── honesty: flat row / no absolute floor / sub-span all yield nothing ──
    std::printf("=== honesty (never fabricate) ===\n");
    {
        const std::vector<float> deg = arcDeg(90.0, 30);
        const std::vector<float> flat(deg.size(), 5.f);        // everywhere below thresh
        check(!measureWedge(flat, {}, deg, kPhiFar, kArmVeto, kFloor, cfg).ok,
              "flat (drowned) row ⇒ no candidate");

        std::vector<float> raw(deg.size(), 5.f);
        for (size_t j = 25; j <= 35; ++j) raw[j] = 30.f;
        check(!measureWedge(raw, {}, deg, kPhiFar, kArmVeto, 0.0, cfg).ok,
              "absFloorRef <= 0 (no absolute anchor) ⇒ no candidate");

        std::vector<float> narrow(deg.size(), 5.f);
        narrow[30] = 30.f; narrow[31] = 30.f;                  // span 1° < minSpanDeg 2
        check(!measureWedge(narrow, {}, deg, kPhiFar, kArmVeto, kFloor, cfg).ok,
              "sub-minSpan plateau (line-like) ⇒ no candidate");
    }

    // ── arm-smear veto: a plateau at φ+180 is the forearm, not the club ─────
    std::printf("=== arm-smear veto ===\n");
    {
        const std::vector<float> deg = arcDeg(90.0, 30);
        std::vector<float> raw(deg.size(), 5.f);
        for (size_t j = 0; j < deg.size(); ++j)
            if (std::abs(wrapd(double(deg[j]) - 90.0)) <= 2.0) raw[j] = 30.f;
        // φ = 270 ⇒ arm smear direction = 90 — exactly the plateau
        check(!measureWedge(raw, {}, deg, 270.0, kArmVeto, kFloor, cfg).ok,
              "centroid within armVetoDeg of φ+180 ⇒ rejected");
        // the same plateau with the arm elsewhere is accepted
        check(measureWedge(raw, {}, deg, kPhiFar, kArmVeto, kFloor, cfg).ok,
              "same plateau, arm elsewhere ⇒ accepted");
    }

    // ── dif channel alone can carry the plateau (either-channel test) ───────
    std::printf("=== channels ===\n");
    {
        const std::vector<float> deg = arcDeg(90.0, 30);
        const std::vector<float> raw(deg.size(), 2.f);         // raw drowned
        std::vector<float> dif(deg.size(), 2.f);
        for (size_t j = 0; j < deg.size(); ++j)
            if (std::abs(wrapd(double(deg[j]) - 90.0)) <= 2.0) dif[j] = 25.f;
        const WedgeCandidate c = measureWedge(raw, dif, deg, kPhiFar, kArmVeto, kFloor, cfg);
        check(c.ok && std::abs(wrapd(c.centroidDeg - 90.0)) <= 1.5,
              "dif-only plateau found at the same centroid");
    }

    // ── two plateaus: the higher-energy run wins ────────────────────────────
    std::printf("=== best-run selection ===\n");
    {
        const std::vector<float> deg = arcDeg(90.0, 40);       // 50..130
        std::vector<float> raw(deg.size(), 0.f);
        for (size_t j = 0; j < deg.size(); ++j) {
            const double a = double(deg[j]);
            if (a >= 60 && a <= 64)  raw[j] = 15.f;            // weak fan
            if (a >= 110 && a <= 116) raw[j] = 40.f;           // strong fan
        }
        const WedgeCandidate c = measureWedge(raw, {}, deg, kPhiFar, kArmVeto, kFloor, cfg);
        check(c.ok && std::abs(wrapd(c.centroidDeg - 113.0)) <= 1.5,
              "higher-energy plateau wins the frame");
    }

    // ── wrap: an arc spanning 0° centres correctly ──────────────────────────
    std::printf("=== wrap ===\n");
    {
        const std::vector<float> deg = arcDeg(0.0, 20);        // 340..20
        std::vector<float> raw(deg.size(), 0.f);
        for (size_t j = 0; j < deg.size(); ++j)
            if (std::abs(wrapd(double(deg[j]) - 0.0)) <= 2.0) raw[j] = 30.f;   // 358..2
        const WedgeCandidate c = measureWedge(raw, {}, deg, kPhiFar, kArmVeto, kFloor, cfg);
        check(c.ok && std::abs(wrapd(c.centroidDeg - 0.0)) <= 1.5,
              "plateau straddling 0/360 centres at 0");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
