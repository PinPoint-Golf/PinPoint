// Standalone tests for the P7 club-at-ball impact geometry
// (src/Analysis/impact_geom.h): the theta==theta_ball crossing detector, the
// anchor-vs-geometry decision, and the P6-window knock-on through
// locatePTimes. Pure 1-D math, no fixture, no Qt/OpenCV. Synthetic profiles
// with hand-placed crossings.
//
//   cmake --build build/analyzer-tests --target impact_geom_test
//   ctest --test-dir build/analyzer-tests -R impact_geom --output-on-failure

#include "../impact_geom.h"
#include "../shaft_positions.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// Timebase: 200 Hz (dt = 5000 µs), 101 frames. Window landmarks: top 40,
// finish 90. Grip fixed at (100,100), ball at (200,200) ⇒ θ_ball = +45° at
// every frame, so e(f) = θ(f) − 45 and crossings are hand-placeable on θ.
static constexpr int     kNf   = 101;
static constexpr int     kTopF = 40, kFinF = 90;
static constexpr int64_t kDt   = 5000;
static constexpr double  kBx = 200.0, kBy = 200.0;

static std::vector<int64_t> times()
{
    std::vector<int64_t> t(kNf);
    for (int f = 0; f < kNf; ++f) t[f] = int64_t(f) * kDt;
    return t;
}
static std::vector<double> gripX() { return std::vector<double>(kNf, 100.0); }
static std::vector<double> gripY() { return std::vector<double>(kNf, 100.0); }

// θ(f) = 45 + slopeDegPerFrame·(f − crossF): e ramps through 0 at crossF,
// armed both sides for |f − crossF| > hyst/slope.
static std::vector<double> ramp(double crossF, double slopeDegPerFrame)
{
    std::vector<double> th(kNf);
    for (int f = 0; f < kNf; ++f) th[f] = 45.0 + slopeDegPerFrame * (double(f) - crossF);
    return th;
}

int main()
{
    const std::vector<int64_t> t  = times();
    const std::vector<double>  gx = gripX();
    const std::vector<double>  gy = gripY();
    ImpactGeomConfig cfg;

    // ── (1) basic crossing: fixed ball, sub-frame instant ─────────────────────
    {
        std::printf("(1) basic crossing\n");
        const ImpactGeomResult r = locateImpactGeom(t, ramp(60.0, 2.0), gx, gy,
                                                    kBx, kBy, kTopF, kFinF, cfg);
        check(r.found, "found");
        check(std::llabs(r.tUs - 60 * kDt) <= kDt / 2, "sub-frame instant within half a frame of f60");
        check(r.frame == 60, "at-or-before frame is f60");

        // Mid-frame crossing: zero of e sits at f=60.5 exactly.
        const ImpactGeomResult h = locateImpactGeom(t, ramp(60.5, 2.0), gx, gy,
                                                    kBx, kBy, kTopF, kFinF, cfg);
        check(h.found && std::llabs(h.tUs - int64_t(60.5 * kDt)) <= 500,
              "half-frame crossing interpolates to f60.5");
        check(h.frame == 60, "half-frame crossing's at-or-before frame is f60");
    }

    // ── (2) moving grip: θ_ball(t) genuinely time-varying ─────────────────────
    {
        std::printf("(2) moving grip\n");
        // Grip slides right 2 px/frame; θ(f) tracks the true θ_ball(f) + 2(f−60)
        // so the zero of e still sits exactly at f60.
        std::vector<double> mgx(kNf), th(kNf);
        for (int f = 0; f < kNf; ++f) {
            mgx[f] = 100.0 + 2.0 * f;
            const double tb = std::atan2(kBy - 100.0, kBx - mgx[f]) * 180.0 / 3.14159265358979323846;
            th[f] = tb + 2.0 * (double(f) - 60.0);
        }
        const ImpactGeomResult r = locateImpactGeom(t, th, mgx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(r.found, "found with a moving grip");
        check(std::llabs(r.tUs - 60 * kDt) <= kDt / 2, "instant still lands at f60");
    }

    // ── (3) hysteresis: a sub-band wiggle never arms ──────────────────────────
    {
        std::printf("(3) hysteresis\n");
        std::vector<double> th(kNf);
        for (int f = 0; f < kNf; ++f)
            th[f] = 45.0 + 5.0 * std::sin(0.7 * f);   // |e| ≤ 5 < hystDeg 8
        const ImpactGeomResult r = locateImpactGeom(t, th, gx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(!r.found, "sub-hysteresis wiggle is not a crossing");
    }

    // ── (4) the ±180 seam: θ_ball+180 must never fire ─────────────────────────
    {
        std::printf("(4) wrap seam\n");
        // θ sweeps from −80 to −280 (≡ +80 past the anti-ball direction −135):
        // e runs −125 → +35 THROUGH the seam (−180 ≡ +180), never through 0.
        std::vector<double> th(kNf);
        for (int f = 0; f < kNf; ++f) th[f] = -80.0 - 200.0 * (double(f) - 41.0) / 49.0;
        const ImpactGeomResult r = locateImpactGeom(t, th, gx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(!r.found, "a transit through theta_ball+180 (the seam) does not fire");

        // Genuine crossing on a profile that ALSO passes the seam first:
        // e: +35 → seam → −125 …continues… → crosses 0 at f80 from below.
        std::vector<double> th2(kNf);
        for (int f = 0; f < kNf; ++f) {
            if (f <= 65) th2[f] = 80.0 + 200.0 * (double(f) - 41.0) / 24.0;   // e +35 → +∼235 (wraps)
            else         th2[f] = 45.0 - 4.0 * (80.0 - double(f));            // e ramps −60 → 0 @80 → +40
        }
        const ImpactGeomResult r2 = locateImpactGeom(t, th2, gx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(r2.found && std::llabs(r2.tUs - 80 * kDt) <= kDt,
              "the genuine 0-transit after a seam pass is found at f80");

        // Representation independence: the long-path DP ending −270 ≡ +90.
        std::vector<double> th3 = ramp(60.0, 2.0);
        for (double &v : th3) v -= 360.0;
        const ImpactGeomResult r3 = locateImpactGeom(t, th3, gx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(r3.found && std::llabs(r3.tUs - 60 * kDt) <= kDt / 2,
              "theta − 360 gives the identical crossing");
    }

    // ── (5) gate window ───────────────────────────────────────────────────────
    {
        std::printf("(5) gate window\n");
        const ImpactGeomResult r = locateImpactGeom(t, ramp(30.0, 2.0), gx, gy,
                                                    kBx, kBy, kTopF, kFinF, cfg);
        check(!r.found, "a crossing before top is outside the window");
        const ImpactGeomResult r2 = locateImpactGeom(t, ramp(96.0, 2.0), gx, gy,
                                                     kBx, kBy, kTopF, kFinF, cfg);
        check(!r2.found, "a crossing after finish is outside the window");
    }

    // ── (6) NaN gap: the instant interpolates across missing frames ──────────
    {
        std::printf("(6) NaN gap\n");
        std::vector<double> th = ramp(60.0, 2.0);
        for (int f = 55; f <= 65; ++f) th[f] = std::numeric_limits<double>::quiet_NaN();
        const ImpactGeomResult r = locateImpactGeom(t, th, gx, gy, kBx, kBy, kTopF, kFinF, cfg);
        check(r.found, "found across an 11-frame NaN gap");
        check(std::llabs(r.tUs - 60 * kDt) <= kDt,
              "instant interpolates inside the gap, not at its far edge");
    }

    // ── (7) decision matrix ───────────────────────────────────────────────────
    {
        std::printf("(7) decision\n");
        ImpactGeomResult geo;
        geo.found = true; geo.tUs = 60 * kDt; geo.frame = 60;

        // Corroborating anchor (f62 = 10 ms away), retime off: kept verbatim.
        ImpactDecision d = decideImpactFrame(true, 62, geo, t, kTopF, cfg);
        check(d.applied == kImpactGeomKept && d.frame == 62 && d.tUs == 62 * kDt,
              "corroborated anchor kept (retime off)");

        // Retime on: frame (the window bound) stays, the instant moves.
        ImpactGeomConfig rcfg = cfg; rcfg.retime = true;
        d = decideImpactFrame(true, 62, geo, t, kTopF, rcfg);
        check(d.applied == kImpactGeomRetimed && d.frame == 62 && d.tUs == geo.tUs,
              "corroborated anchor retimed sub-frame, frame kept");

        // Implausible anchor (f85 = 125 ms away > overrideUs 100 ms): overridden.
        d = decideImpactFrame(true, 85, geo, t, kTopF, cfg);
        check(d.applied == kImpactGeomOverride && d.frame == 60 && d.tUs == geo.tUs,
              "implausible anchor overridden with the geometry");

        // No anchor: adopted.
        d = decideImpactFrame(false, -1, geo, t, kTopF, cfg);
        check(d.applied == kImpactGeomAdopted && d.frame == 60 && d.tUs == geo.tUs,
              "absent anchor adopts the geometry");

        // No geometry: anchor stands, even an implausible-looking one.
        d = decideImpactFrame(true, 85, ImpactGeomResult{}, t, kTopF, cfg);
        check(d.applied == kImpactGeomKept && d.frame == 85, "no geometry ⇒ abstain, anchor kept");

        // Clamp: a geometry frame at/below top clamps to top+1.
        ImpactGeomResult early; early.found = true; early.tUs = kTopF * kDt; early.frame = kTopF;
        d = decideImpactFrame(false, -1, early, t, kTopF, cfg);
        check(d.frame == kTopF + 1, "adopted frame clamps to top+1");
    }

    // ── (8) default pins (flip commit updates these) ─────────────────────────
    {
        std::printf("(8) default pins\n");
        const ImpactGeomConfig def;
        check(def.enabled == false, "enabled defaults false (dark at merge)");
        check(def.retime == false, "retime defaults false (dark at merge)");
        check(def.hystDeg == 8.0 && def.maxStepDeg == 120.0 && def.overrideUs == 100000,
              "tuning defaults pinned");
    }

    // ── (9) P6-window knock-on through locatePTimes ──────────────────────────
    // The gross-late failure shape: a coverage gap around impact maps the
    // anchor deep into the follow-through. θ crosses horizontal at f85 on the
    // downswing (the true P6) and again at f105 in the through-swing; a stale
    // impact bound of f110 admits the through-swing transit as "last crossing"
    // (p6LastCrossing default) — the corrected bound (f88, at-or-before the
    // geometric impact) restores delivery.
    {
        std::printf("(9) locatePTimes knock-on\n");
        const int     nf2 = 121;
        std::vector<int64_t> t2(nf2);
        for (int f = 0; f < nf2; ++f) t2[f] = int64_t(f) * kDt;
        std::vector<double> th(nf2), phi(nf2, std::numeric_limits<double>::quiet_NaN());
        for (int f = 0; f < nf2; ++f) {
            if (f <= 60)       th[f] = -80.0;                                   // parked (pre-window)
            else if (f <= 85)  th[f] = -80.0 + 80.0 * double(f - 60) / 25.0;    // → 0 @85 (P6)
            else if (f <= 95)  th[f] = 80.0 * double(f - 85) / 10.0;            // → +80 @95 (impact ~f88)
            else if (f <= 115) th[f] = 80.0 - 160.0 * double(f - 95) / 20.0;    // → 0 @105 (through) → −80
            else               th[f] = -80.0;
        }
        PositionsConfig pcfg;   // hysteresisDeg 8, p6LastCrossing default ON
        const std::vector<PTime> stale =
            locatePTimes(t2, th, phi, 5, 60, 110, 118, pcfg);
        const std::vector<PTime> fixed =
            locatePTimes(t2, th, phi, 5, 60, 88, 118, pcfg);
        const PTime *p6s = nullptr, *p6f = nullptr;
        for (const PTime &p : stale) if (p.p == 6) p6s = &p;
        for (const PTime &p : fixed) if (p.p == 6) p6f = &p;
        check(p6s && std::llabs(p6s->tUs - 105 * kDt) <= kDt,
              "stale impact bound admits the through-swing transit as P6");
        check(p6f && std::llabs(p6f->tUs - 85 * kDt) <= kDt,
              "corrected impact bound restores the delivery P6 at f85");
    }

    std::printf("%s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
