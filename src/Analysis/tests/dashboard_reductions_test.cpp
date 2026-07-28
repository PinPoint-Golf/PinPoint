// Standalone tests for the dashboard band-rail reductions (src/Analysis/
// dashboard_reductions.h): the phase-keyed sample↔corridor join, the value→y
// domain (including the one-sided speeds case and the always-present 0 line),
// and playhead interpolation with endpoint clamping. Pure, header-only — no
// OpenCV, no fixture. Own main()/check() macros.
//
//   cmake --build build/analyzer-tests --target dashboard_reductions_test
//   ctest --test-dir build/analyzer-tests -R dashboard_reductions --output-on-failure

#include "../dashboard_reductions.h"

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static RailSample smp(int phase, int64_t t, double v, const char *band = "")
{
    RailSample s;
    s.phase = phase;
    s.tUs   = t;
    s.value = v;
    s.band  = QString::fromLatin1(band);
    return s;
}
static RailCorridor cor(int phase, double gLo, double gHi, double aLo, double aHi,
                        bool highOpen = false, bool lowOpen = false)
{
    RailCorridor c;
    c.phase = phase;
    c.greenLo = gLo; c.greenHi = gHi;
    c.amberLo = aLo; c.amberHi = aHi;
    c.highOpen = highOpen; c.lowOpen = lowOpen;
    return c;
}

int main()
{
    std::printf("dashboard_reductions_test\n");

    // ── railCorridorAt: phase-keyed lookup, nullptr when absent ─────────────────
    {
        const std::vector<RailCorridor> cs = { cor(0, -5, 5, -10, 10), cor(5, 10, 20, 5, 25) };
        check(railCorridorAt(cs, 5) != nullptr && near(railCorridorAt(cs, 5)->greenLo, 10.0, 1e-9),
              "corridor lookup finds phase 5");
        check(railCorridorAt(cs, 3) == nullptr, "corridor lookup = nullptr for an uncorridored phase");
        check(railCorridorAt({}, 0) == nullptr, "corridor lookup = nullptr with no corridors");
    }

    // ── railCheckpoints: join by phase, order by time, every sample survives ─────
    {
        // Samples deliberately out of time order; phase 3 has no corridor; the
        // corridor for phase 9 has no sample and must NOT produce a point.
        const std::vector<RailSample>   ss = { smp(5, 200000, 18.0, "green"),
                                               smp(0,      0,  1.0, "green"),
                                               smp(3, 100000, -4.0, "red") };
        const std::vector<RailCorridor> cs = { cor(0, -5, 5, -10, 10),
                                               cor(5, 10, 20, 5, 25),
                                               cor(9,  0,  1,  0,  2) };
        const auto pts = railCheckpoints(ss, cs);

        check(pts.size() == 3, "one point per measured sample (uncorridored sample kept)");
        check(pts[0].phase == 0 && pts[1].phase == 3 && pts[2].phase == 5,
              "points ordered by time, not input order");
        check(pts[0].hasCorridor && near(pts[0].greenHi, 5.0, 1e-9),
              "phase 0 sample carries its corridor");
        check(!pts[1].hasCorridor, "phase 3 sample has no corridor → hasCorridor false");
        check(pts[2].hasCorridor && near(pts[2].amberHi, 25.0, 1e-9),
              "phase 5 sample carries its corridor");
        check(pts[2].band == QString("green"), "band carried through the join");
        // A corridor with no measurement must not invent a checkpoint.
        for (const RailPoint &p : pts) check(p.phase != 9, "corridor without a sample is dropped");
    }
    {
        const auto pts = railCheckpoints({ smp(-1, 0, 1.0) }, {});
        check(pts.empty(), "sample with phase < 0 is rejected");
        check(railCheckpoints({}, { cor(0, 0, 1, 0, 2) }).empty(),
              "no samples → no points (rail collapses)");
    }

    // ── railRange: spans dots + corridor bounds + 0, padded ─────────────────────
    {
        // Values 12..18, corridor 5..25 ⇒ raw span [0,25] (0 line included), pad 8%.
        std::vector<RailPoint> pts = railCheckpoints({ smp(0, 0, 12.0), smp(5, 100000, 18.0) },
                                                     { cor(5, 10, 20, 5, 25) });
        const RailRange r = railRange(pts);
        check(r.valid, "range valid for a populated rail");
        check(near(r.lo, 0.0 - 25.0 * 0.08, 1e-9), "lo = 0 (reference line) minus pad");
        check(near(r.hi, 25.0 + 25.0 * 0.08, 1e-9), "hi = corridor amberHi plus pad");
    }
    {
        // Negative excursion must widen the domain downward past 0.
        std::vector<RailPoint> pts = railCheckpoints({ smp(0, 0, -30.0), smp(5, 100000, 4.0) }, {});
        const RailRange r = railRange(pts);
        check(r.lo < -30.0 && r.hi > 0.0, "negative values widen below 0, 0 still inside");
    }
    {
        // ── The open tail's edge belongs IN the domain ──────────────────────────
        //
        // railRange used to take a `oneSided` bool and drop the upper bounds, because
        // one-sidedness was decided by string-matching a unit and the bound it dropped
        // was a two-sided norm's aspirational high edge. Both halves of that are gone.
        // A one-sided corridor's open edge is `mu` — the ASPIRATION — and hiding the
        // number the golfer is aiming at is the opposite of what the rail is for.
        //
        // So a floor at mu = 100 keeps 100 in the domain, and openness changes only how
        // the ribbon is painted.
        std::vector<RailPoint> pts =
            railCheckpoints({ smp(5, 0, 85.0) },
                            { cor(5, /*gLo*/95, /*gHi*/100, /*aLo*/85, /*aHi*/100, true) });
        const RailRange r = railRange(pts);
        check(r.hi > 100.0 && r.hi < 120.0,
              "the aspiration is in the domain, padded — not dropped, not crushing the trace");
        check(pts.front().highOpen && !pts.front().lowOpen,
              "…and openness travels on the checkpoint, per side, for the ribbon to read");
    }
    {
        // The ceiling mirror, which had no representation at all before: the old code
        // substituted the high edges and never the low ones.
        std::vector<RailPoint> pts =
            railCheckpoints({ smp(5, 0, 3.0) },
                            { cor(5, 0, 2, 0, 6, false, true) });
        check(pts.front().lowOpen && !pts.front().highOpen, "a ceiling is open BELOW");
        const RailRange r = railRange(pts);
        check(r.valid && r.hi > 6.0, "…and its graded high edge still bounds the domain");
    }
    {
        // Degenerate: a single value equal to 0 ⇒ widen to ±1 rather than divide by zero.
        std::vector<RailPoint> pts = railCheckpoints({ smp(0, 0, 0.0) }, {});
        const RailRange r = railRange(pts);
        check(r.valid && r.hi > r.lo, "degenerate span widens instead of collapsing");
        check(!railRange({}).valid, "empty rail → invalid range");
    }

    // ── interpolateAtUs: linear between samples, clamped outside ────────────────
    {
        const std::vector<int64_t> t = { 0, 100000, 200000 };
        const std::vector<double>  v = { 0.0, 10.0, 30.0 };

        check(near(interpolateAtUs(t, v,      0),  0.0, 1e-9), "exact first sample");
        check(near(interpolateAtUs(t, v, 100000), 10.0, 1e-9), "exact interior sample");
        check(near(interpolateAtUs(t, v, 200000), 30.0, 1e-9), "exact last sample");
        check(near(interpolateAtUs(t, v,  50000),  5.0, 1e-9), "midpoint of first segment");
        check(near(interpolateAtUs(t, v, 150000), 20.0, 1e-9), "midpoint of second segment");
        check(near(interpolateAtUs(t, v, -99999),  0.0, 1e-9), "clamped below the span");
        check(near(interpolateAtUs(t, v, 999999), 30.0, 1e-9), "clamped above the span");

        check(std::isnan(interpolateAtUs({}, {}, 0)), "empty curve → NaN");
        check(near(interpolateAtUs({ 5000 }, { 7.0 }, 0), 7.0, 1e-9), "single sample → that value");
        // Mismatched array lengths must not read past the shorter one.
        check(near(interpolateAtUs(t, { 0.0, 10.0 }, 150000), 10.0, 1e-9),
              "ragged arrays clamp to the shorter length");
        // Duplicate timestamps must not divide by zero. lower_bound guarantees
        // tUs[lo] < us <= tUs[hi], so an interior duplicate can never be the divisor;
        // a duplicated FINAL stamp is taken by the endpoint clamp, last value winning.
        check(near(interpolateAtUs({ 0, 100000, 100000 }, { 0.0, 5.0, 9.0 }, 100000), 9.0, 1e-9),
              "duplicated final stamp → last value (endpoint clamp)");
        check(near(interpolateAtUs({ 0, 50000, 50000, 100000 }, { 0.0, 5.0, 9.0, 12.0 }, 50000),
                   5.0, 1e-9),
              "interior duplicate resolves without dividing by zero");
        check(near(interpolateAtUs({ 0, 50000, 50000, 100000 }, { 0.0, 5.0, 9.0, 12.0 }, 75000),
                   10.5, 1e-9),
              "segment after an interior duplicate interpolates from the later sample");
    }

    // ── orderScoreSegments: weakest first, stable on ties ──────────────────────
    {
        const auto out = orderScoreSegments({ { QStringLiteral("tempo"),    80 },
                                              { QStringLiteral("wrist"),    41 },
                                              { QStringLiteral("rotation"), 62 } });
        check(out.size() == 3, "every bucket survives");
        check(out[0].label == QString("wrist"),    "weakest first");
        check(out[1].label == QString("rotation"), "then the middle");
        check(out[2].label == QString("tempo"),    "strongest last");
    }
    {
        // Equal values must order by label, not by insertion — otherwise the
        // breakdown reshuffles between swings that happen to score the same.
        const auto a = orderScoreSegments({ { QStringLiteral("zebra"), 50 },
                                            { QStringLiteral("alpha"), 50 } });
        const auto b = orderScoreSegments({ { QStringLiteral("alpha"), 50 },
                                            { QStringLiteral("zebra"), 50 } });
        check(a[0].label == QString("alpha") && a[1].label == QString("zebra"),
              "ties order by label");
        check(a[0].label == b[0].label && a[1].label == b[1].label,
              "tie order is independent of input order");
    }
    check(orderScoreSegments({}).empty(), "no buckets → no segments (donut hides them)");

    // ── orientationLabel: square inside the corridor, wherever it sits ──────────
    {
        check(orientationLabel(0.0,  -3.0, 3.0) == QString("square"), "inside corridor → square");
        check(orientationLabel(5.0,  -3.0, 3.0) == QString("open"),   "above greenHi → open");
        check(orientationLabel(-5.0, -3.0, 3.0) == QString("closed"), "below greenLo → closed");
        check(orientationLabel(-3.0, -3.0, 3.0) == QString("square"), "corridor bounds are inclusive");
        check(orientationLabel(3.0,  -3.0, 3.0) == QString("square"), "corridor bounds are inclusive (hi)");
        // A corridor deliberately off-zero: 2..8 open-at-address is CORRECT, so a
        // value of 5 must read square and a value of 0 must read closed.
        check(orientationLabel(5.0, 2.0, 8.0) == QString("square"),
              "off-zero corridor: inside is square, not open");
        check(orientationLabel(0.0, 2.0, 8.0) == QString("closed"),
              "off-zero corridor: below it is closed even though the value is 0");
        check(orientationLabel(1.0, 3.0, 3.0).isEmpty(), "degenerate corridor → empty");
        check(orientationLabel(1.0, 5.0, 2.0).isEmpty(), "reversed corridor → empty");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
