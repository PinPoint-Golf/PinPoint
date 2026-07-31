// The corridor picture (src/Diagnostics/corridor_plot.{h,cpp}) — the distribution a norm claims,
// laid out over the readings a library holds.
//
// Geometry in C++ so it can be asserted at all; this suite is the reason that choice was made. Five
// things carry the weight, and each is invisible in the rendered picture if it is wrong:
//
//   1. The curve is SPLIT-normal. sigmaLo and sigmaHi are asymmetric by design, and a symmetric
//      bell would show tolerance the norm does not grant on the tight side.
//   2. The bands drawn are the bands that GRADE — bandEdgesOf(), policy included. norm.h records a
//      bug where a surface drew mu ± sigma while grade() applied policy.idealMaxZ; under `strict`
//      those differ, and the picture would be of a corridor the app does not use.
//   3. An ungraded tail is drawn as ungraded. A full bell over a Floor measure states a judgement
//      the norm does not make.
//   4. Implausible readings are counted APART, never graded. "We do not believe this number" and
//      "this swing was bad" are different statements.
//   5. The window contains the whole corridor AND every reading. Cropping either is a picture that
//      lies by omission, and the readings outside the corridor are the ones being looked for.
//
//   cmake --build build/analyzer-tests --target corridor_plot_test
//   ctest --test-dir build/analyzer-tests -R corridor_plot --output-on-failure

#include "corridor_plot.h"

#include <QCoreApplication>

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }

// A deliberately ASYMMETRIC norm — ball position is tolerated far more forward than back, which is
// exactly why the two sigmas exist.
static Norm makeNorm()
{
    Norm n;
    n.measureId = QStringLiteral("m_test");
    n.contextId = QStringLiteral("any");
    n.mu        = 30.0;
    n.sigmaLo   = 4.0;
    n.sigmaHi   = 12.0;
    n.unit      = QStringLiteral("% stance width");
    return n;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    std::printf("=== the curve is split-normal, not a bell ===\n");
    {
        const Norm n = makeNorm();

        check(near(splitNormalPeakNormalised(n.mu, n.mu, n.sigmaLo, n.sigmaHi), 1.0),
              "the peak is at mu, normalised to 1");

        // One sigma out on each side is the SAME height — that is what makes the join continuous —
        // but at very different distances, which is the whole point of two sigmas.
        const double lo = splitNormalPeakNormalised(n.mu - n.sigmaLo, n.mu, n.sigmaLo, n.sigmaHi);
        const double hi = splitNormalPeakNormalised(n.mu + n.sigmaHi, n.mu, n.sigmaLo, n.sigmaHi);
        check(near(lo, hi, 1e-9), "one tolerance out is the same height on both sides");
        check(near(lo, std::exp(-0.5), 1e-9), "and that height is exp(-1/2)");

        // The asymmetry itself: 4 units above mu is much closer to the peak than 4 units below,
        // because the high side is three times as tolerant.
        const double above = splitNormalPeakNormalised(n.mu + 4.0, n.mu, n.sigmaLo, n.sigmaHi);
        const double below = splitNormalPeakNormalised(n.mu - 4.0, n.mu, n.sigmaLo, n.sigmaHi);
        check(above > below, "the tolerant side falls away more slowly");

        // A degenerate sigma must not produce NaN — a corridor mid-authoring can legitimately have
        // one, and NaN crossing into QML poisons every binding that touches it.
        check(near(splitNormalPeakNormalised(5.0, 5.0, 0.0, 0.0), 1.0),
              "a zero tolerance is a spike at mu, not a NaN");
        check(near(splitNormalPeakNormalised(6.0, 5.0, 0.0, 0.0), 0.0),
              "and nothing away from it");
    }

    std::printf("=== the bands drawn are the bands that grade ===\n");
    {
        const Norm n = makeNorm();

        // Under a NON-default policy, drawn-equals-graded is a real claim rather than a coincidence:
        // `strict` moves every edge, and mu ± sigma would no longer be the ideal band.
        GradePolicy strict;
        strict.idealMaxZ = 0.5;
        strict.goodMaxZ  = 1.0;
        strict.watchMaxZ = 1.5;

        CorridorPlotOptions opt;
        opt.width  = 1000.0;
        opt.height = 100.0;

        const CorridorPlot p = layoutCorridorPlot(n, Shape::Target, {}, strict, opt);
        const NormBandEdges e = bandEdgesOf(n, strict, -1.0, Shape::Target);

        auto toX = [&](double v) { return (v - p.xMin) / (p.xMax - p.xMin) * p.width; };
        check(near(p.idealLoX, toX(e.idealLo), 1e-6), "the ideal low handle is bandEdgesOf's edge");
        check(near(p.idealHiX, toX(e.idealHi), 1e-6), "and the ideal high handle too");
        check(near(p.muX, toX(n.mu), 1e-6), "mu is where mu is");
        check(!near(e.idealHi, n.mu + n.sigmaHi, 1e-9),
              "and under strict that edge is NOT the norm's raw claim — so this proves the point");

        // Four bands each side of nothing: action | watch | ideal | watch | action, with the ideal
        // spanning the middle. Regions, not edges, and contiguous.
        check(p.bands.size() == 5, "five regions across a two-sided corridor");
        double runningX = p.bands.front().x;
        bool   contiguous = true;
        for (const CorridorBand &b : p.bands) {
            if (!near(b.x, runningX, 1e-6)) contiguous = false;
            runningX = b.x + b.w;
        }
        check(contiguous, "and they tile without a gap");
        check(p.bands.front().grade == Grade::Action && p.bands.back().grade == Grade::Action,
              "Action at both ends");
    }

    std::printf("=== an ungraded tail is drawn as ungraded ===\n");
    {
        Norm n   = makeNorm();
        n.mu     = 1.45;
        n.sigmaLo = 0.05;
        n.sigmaHi = 0.05;

        // Floor: higher is better, so only the LOW tail grades. A full bell here would state that
        // being well above the aspiration is a fault, which the norm does not say.
        const CorridorPlot p = layoutCorridorPlot(n, Shape::Floor, {}, {}, {});
        check(p.highOpen, "the high tail is flagged open");
        check(!p.lowOpen, "and the low tail still grades");

        bool actionAbove = false;
        for (const CorridorBand &b : p.bands)
            if (b.grade == Grade::Action && b.x > p.muX) actionAbove = true;
        check(!actionAbove, "there is no Action band above the aspiration");

        const CorridorPlot c = layoutCorridorPlot(n, Shape::Ceiling, {}, {}, {});
        check(c.lowOpen && !c.highOpen, "a Ceiling is the mirror of it");
    }

    std::printf("=== the counts are grade()'s counts ===\n");
    {
        Norm n = makeNorm();
        n.plausibleLo = 0.0;
        n.plausibleHi = 100.0;

        std::vector<double> values;
        for (int i = 0; i < 200; ++i) values.push_back(20.0 + i * 0.35);   // 20.0 … 89.65
        values.push_back(-5.0);      // outside the believed range
        values.push_back(140.0);     // and the other side

        const GradePolicy  policy;
        const CorridorPlot p = layoutCorridorPlot(n, Shape::Target, values, policy, {});

        check(p.implausible == 2, "the two unbelievable readings are counted apart");
        check(p.n == 200, "and are NOT among the graded ones");

        // Graded by the same function the app grades with, over the same values.
        int ideal = 0, good = 0, watch = 0, action = 0;
        for (double v : values) {
            if (n.isImplausible(v)) continue;
            switch (grade(v, n, policy, Shape::Target)) {
            case Grade::Ideal:  ++ideal;  break;
            case Grade::Good:   ++good;   break;
            case Grade::Watch:  ++watch;  break;
            case Grade::Action: ++action; break;
            case Grade::NotMeasured: break;
            }
        }
        check(p.ideal == ideal && p.good == good && p.watch == watch && p.action == action,
              "every band count matches grade() exactly");
        check(p.ideal + p.good + p.watch + p.action == p.n, "and they account for every reading");
    }

    std::printf("=== the window hides nothing ===\n");
    {
        Norm n = makeNorm();

        // A reading far outside the corridor is exactly what an author is looking for, so the
        // window has to reach it.
        std::vector<double> values = { 30.0, 31.0, 240.0, -90.0 };
        const CorridorPlot  p      = layoutCorridorPlot(n, Shape::Target, values, {}, {});

        check(p.xMin < -90.0, "the window reaches the lowest reading");
        check(p.xMax > 240.0, "and the highest");

        const NormBandEdges e = bandEdgesOf(n, {}, -1.0, Shape::Target);
        check(p.xMin < e.watchLo && p.xMax > e.watchHi,
              "and still contains the whole graded corridor");

        // Every reading reaches the rug, whatever the scatter does.
        check(int(p.rug.size()) == p.n, "the rug carries every reading");
    }

    std::printf("=== a thinned scatter says so ===\n");
    {
        const Norm n = makeNorm();

        std::vector<double> many;
        for (int i = 0; i < 5000; ++i) many.push_back(30.0 + (i % 40) * 0.5);

        CorridorPlotOptions opt;
        opt.maxSamplePoints = 100;

        const CorridorPlot p = layoutCorridorPlot(n, Shape::Target, many, {}, opt);
        check(p.n == 5000, "every reading is still counted");
        check(int(p.rug.size()) == 5000, "and still on the rug");
        check(int(p.samples.size()) <= 120, "the scatter is capped");
        check(p.truncated, "and REPORTED — a silent cap reads as the whole library");

        std::vector<double> few = { 30.0, 31.0, 29.0 };
        const CorridorPlot  q   = layoutCorridorPlot(n, Shape::Target, few, {}, opt);
        check(!q.truncated, "a small library is not reported as thinned");
    }

    std::printf("=== the finding is stated in words ===\n");
    {
        Norm n    = makeNorm();
        n.sigmaLo = 500.0;   // absurdly loose: everything will land Ideal
        n.sigmaHi = 500.0;

        std::vector<double> values;
        for (int i = 0; i < 50; ++i) values.push_back(30.0 + i * 0.1);

        const CorridorPlot p = layoutCorridorPlot(n, Shape::Target, values, {}, {});
        check(p.ideal == p.n, "a corridor this loose grades everything Ideal");
        check(!p.note.isEmpty(), "and the picture says so rather than leaving it to be noticed");
        check(p.note.contains(QStringLiteral("Ideal")), "naming the band it collapsed into");

        // Too few readings to judge by — a different statement from "this corridor is fine", and
        // the two must not look the same.
        const CorridorPlot few = layoutCorridorPlot(n, Shape::Target, { 30.0, 31.0 }, {}, {});
        check(!few.note.isEmpty() && !few.note.contains(QStringLiteral("Ideal")),
              "and too few readings reads as too few, not as a clean bill of health");

        // A corridor that discriminates says nothing at all.
        Norm tight   = makeNorm();
        tight.sigmaLo = 1.0;
        tight.sigmaHi = 1.0;
        std::vector<double> spread;
        for (int i = 0; i < 60; ++i) spread.push_back(20.0 + i * 0.5);
        const CorridorPlot ok = layoutCorridorPlot(tight, Shape::Target, spread, {}, {});
        check(ok.note.isEmpty(), "a corridor that tells swings apart is not commented on");
    }

    std::printf("=== an empty library still draws the claim ===\n");
    {
        const CorridorPlot p = layoutCorridorPlot(makeNorm(), Shape::Target, {}, {}, {});
        check(!p.curve.empty(), "the curve is drawn with no readings at all");
        check(p.bands.size() == 5, "and so are the bands");
        check(p.n == 0 && p.samples.empty() && p.rug.empty(), "there is simply nothing over it");
    }

    std::printf("=== a corridor is only as precise as what measures it ===\n");
    {
        // Whole degrees, whole percent, whole millimetres. Left ungoverned a drag stores 30.418273,
        // which reads as a measurement and is really where a pointer stopped.
        check(near(corridorPrecisionFor(QString::fromUtf8("°")).step, 1.0), "degrees are whole");
        check(corridorPrecisionFor(QString::fromUtf8("°")).decimals == 0, "and render without a point");
        check(near(corridorPrecisionFor(QStringLiteral("% stance width")).step, 1.0),
              "body-relative percentages are whole");
        check(near(corridorPrecisionFor(QStringLiteral("mm")).step, 1.0), "millimetres are whole");
        // Whole millimetres expressed in the unit the norm is actually stated in.
        check(near(corridorPrecisionFor(QStringLiteral("cm")).step, 0.1),
              "and a centimetre corridor still moves in whole millimetres");
        check(corridorPrecisionFor(QStringLiteral("cm")).decimals == 1, "rendering one place");

        check(near(corridorPrecisionFor(QStringLiteral(":1")).step, 0.1),
              "tempo moves in tenths, the way everyone quotes it");
        check(near(corridorPrecisionFor(QStringLiteral("rpm")).step, 50.0),
              "spin moves in 50s — finer is inside the hardware's own noise");

        // An unrecognised unit gets the FINEST setting, not a guess: rounding a quantity we do not
        // know could destroy a legitimate value, and that is the worse fault.
        check(corridorPrecisionFor(QStringLiteral("furlongs")).step < 0.02,
              "an unknown unit is left alone rather than rounded on a hunch");

        check(near(snapCorridorValue(30.418273, QString::fromUtf8("°")), 30.0), "a degree snaps whole");
        check(near(snapCorridorValue(30.6, QString::fromUtf8("°")), 31.0), "and rounds, not truncates");
        check(near(snapCorridorValue(-4.4, QString::fromUtf8("°")), -4.0), "negatives too");
        check(near(snapCorridorValue(12.34, QStringLiteral("cm")), 12.3), "a centimetre to the mm");
        check(near(snapCorridorValue(2874.0, QStringLiteral("rpm")), 2850.0), "spin to the 50");
    }

    std::printf("=== a frozen window makes the axis a ruler ===\n");
    {
        // Dragging mu normally moves the derived window, so the same pixel means a different value
        // from one frame to the next — the value shifts the window, which shifts the value. A caller
        // holding the window still is what makes a handle placeable at all.
        const Norm n = makeNorm();

        CorridorPlotOptions opt;
        opt.width  = 1000.0;
        opt.height = 100.0;

        const CorridorPlot a = layoutCorridorPlot(n, Shape::Target, {}, {}, opt);

        Norm moved = n;
        moved.mu   = n.mu + 40.0;
        const CorridorPlot b = layoutCorridorPlot(moved, Shape::Target, {}, {}, opt);
        check(!near(a.xMin, b.xMin) || !near(a.xMax, b.xMax),
              "moving mu does move the derived window — which is the problem being solved");

        // Frozen, the same pixel means the same value however far mu travels.
        opt.windowMin = a.xMin;
        opt.windowMax = a.xMax;
        const CorridorPlot fa = layoutCorridorPlot(n, Shape::Target, {}, {}, opt);
        const CorridorPlot fb = layoutCorridorPlot(moved, Shape::Target, {}, {}, opt);
        check(near(fa.xMin, fb.xMin) && near(fa.xMax, fb.xMax),
              "a supplied window is honoured whatever the corridor does");

        // And the curve still moves inside it — freezing the axis must not freeze the picture.
        check(!near(fa.muX, fb.muX), "the aspiration still travels across the frozen axis");

        // The round trip a drag depends on: pixel -> value -> pixel, under the frozen window.
        const double px    = 640.0;
        const double value = fa.xMin + (px / fa.width) * (fa.xMax - fa.xMin);
        Norm at            = n;
        at.mu              = value;
        const CorridorPlot fc = layoutCorridorPlot(at, Shape::Target, {}, {}, opt);
        check(near(fc.muX, px, 1e-6), "a value placed at a pixel lays out back at that pixel");
    }

    std::printf("=== the readings sit ON the curve being set ===\n");
    {
        const Norm n = makeNorm();

        CorridorPlotOptions opt;
        opt.width  = 1000.0;
        opt.height = 100.0;

        // One reading at the aspiration, one a tolerance out, one far into the tail.
        const std::vector<double> values = { n.mu, n.mu + n.sigmaHi, n.mu + 6.0 * n.sigmaHi };
        const CorridorPlot p = layoutCorridorPlot(n, Shape::Target, values, {}, opt);
        check(int(p.samples.size()) == 3, "every reading is placed");

        auto yFor = [&](double v) {
            const double x = (v - p.xMin) / (p.xMax - p.xMin) * p.width;
            for (const CorridorPoint &pt : p.samples)
                if (near(pt.x, x, 1e-6)) return pt.y;
            return -1.0;
        };

        // Height is the density the NORM gives that value — so a dot's y is decided by the corridor
        // being set, which is what makes the cloud show where the library falls ON that claim.
        check(near(yFor(n.mu), p.height - p.height * 1.0, 1e-6),
              "a reading at the aspiration sits at the peak");
        check(near(yFor(n.mu + n.sigmaHi), p.height - p.height * std::exp(-0.5), 1e-6),
              "one tolerance out sits where the curve is at exp(-1/2)");
        check(yFor(n.mu + 6.0 * n.sigmaHi) > yFor(n.mu + n.sigmaHi),
              "and one far out is stranded near the floor");

        // The asymmetry reaches the dots too: the same DISTANCE either side of mu is a different
        // height, because the tolerances differ. A symmetric bell would put these level.
        const std::vector<double> pair = { n.mu - 4.0, n.mu + 4.0 };
        const CorridorPlot        q    = layoutCorridorPlot(n, Shape::Target, pair, {}, opt);
        check(q.samples.size() == 2 && !near(q.samples[0].y, q.samples[1].y, 1e-6),
              "equal distances either side of mu sit at different heights");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
