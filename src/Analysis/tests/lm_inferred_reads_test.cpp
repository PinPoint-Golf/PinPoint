// Standalone tests for the launch monitor panel's two INFERRED reads (src/Analysis/
// lm_inferred_reads.h): all nine flight-shape windows, both severity tiers and both
// routes into the severe one, every threshold boundary, left-handed mirroring of the
// gloss, and the rule that a missing input yields NO read rather than a default one.
// Pure — needs QtCore for QString, nothing else. Own main()/check() macros.
//
// These are the only two claims on the whole panel that the device did not make, so
// they are the two that most need a test standing behind every word they print.
//
//   cmake --build build/analysis-tests --target lm_inferred_reads_test
//   ctest --test-dir build/analysis-tests -R lm_inferred_reads --output-on-failure

#include "../lm_inferred_reads.h"

#include <cstdio>
#include <limits>
#include <optional>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void checkStr(const QString &got, const char *want, const char *label)
{
    const bool ok = got == QString::fromUtf8(want);
    std::printf("  [%s] %s (got \"%s\", want \"%s\")\n",
                ok ? "PASS" : "FAIL", label, qPrintable(got), want);
    if (!ok) ++g_fail;
}

// The shape of one shot, right-handed unless said otherwise. Curve-free by default:
// with no carry and no offline there is no measured curvature, so severity falls to the
// spin axis alone.
static LmFlightShape shape(double startDir, double axis, double f2p = 0.0,
                           bool leftHanded = false)
{
    return lmFlightShape(startDir, axis, f2p,
                         std::optional<double>{}, std::optional<double>{}, leftHanded);
}

// A shot that also flew: carry and offline present, so the curvature is measured.
static LmFlightShape flown(double startDir, double axis, double f2p,
                           double carry, double offline, bool leftHanded = false)
{
    return lmFlightShape(startDir, axis, f2p, carry, offline, leftHanded);
}

int main()
{
    std::printf("lm_inferred_reads_test\n");

    // ── The nine windows ────────────────────────────────────────────────────────
    //
    // Every cell of the 3 x 3, named. The grid on the card has nine lights and each one
    // has to be reachable, or a golfer will hit a shot the panel has no word for.
    //
    // Driven by the SPIN AXIS, which is what bends the ball — see the header for the 25
    // shots that established it and the nine the old face-to-path model got wrong.
    {
        checkStr(shape(-3.0, -4.0).name, "Pull–draw",  "left window, left curve");
        checkStr(shape(-3.0,  0.0).name, "Pull",       "left window, no curve");
        checkStr(shape(-3.0,  4.0).name, "Pull–fade",  "left window, right curve");
        checkStr(shape( 0.0, -4.0).name, "Draw",       "centre window, left curve");
        checkStr(shape( 0.0,  0.0).name, "Straight",   "centre window, no curve");
        checkStr(shape( 0.0,  4.0).name, "Fade",       "centre window, right curve");
        checkStr(shape( 3.0, -4.0).name, "Push–draw",  "right window, left curve");
        checkStr(shape( 3.0,  0.0).name, "Push",       "right window, no curve");
        checkStr(shape( 3.0,  4.0).name, "Push–fade",  "right window, right curve");
    }

    // ── The grid indices agree with the words ───────────────────────────────────
    //
    // The lit cell and the label are read together; if they can disagree, one of them
    // is lying and the reader cannot tell which.
    {
        const LmFlightShape s = shape(-3.0, 4.0);
        check(s.windowIdx == 0 && s.curveIdx == 2, "Pull-fade lights the left/fade cell");
        const LmFlightShape c = shape(0.0, 0.0);
        check(c.windowIdx == 1 && c.curveIdx == 1, "Straight lights the centre cell");
        const LmFlightShape p = shape(3.0, -4.0);
        check(p.windowIdx == 2 && p.curveIdx == 0, "Push-draw lights the right/draw cell");
    }

    // ── The axis leads, and face-to-path does not overrule it ───────────────────
    //
    // The correction this model exists for. Face-to-path is a cause of the axis, not of
    // the flight, and on the two shots in the session where they disagreed in sign the
    // ball went the axis's way both times.
    {
        // swing_0012: face 2.9 deg CLOSED, axis 2.3 deg right, ball curved 2.1 yd RIGHT.
        // The old model called this a draw — the exact opposite of what happened.
        checkStr(flown(-2.5, 2.3, -2.9, 190.3, -6.1).name, "Pull–fade",
                 "a closed face with a right-tilted axis still fades");
        // swing_0018: same disagreement, smaller. Under the axis threshold, so straight.
        checkStr(flown(-0.6, 1.0, -1.4, 150.2, -0.4).name, "Straight",
                 "…and below the threshold it is simply straight");
        // swing_0009: face-to-path exactly 1.0, axis 16.3, 25 yd of curve. The old model
        // called this dead straight because 1.0 was not greater than 1.0.
        checkStr(flown(1.8, 16.3, 1.0, 193.2, 31.5).name, "Push–slice",
                 "a square-ish face with a steep axis is not a straight ball");
        // swing_0013 and swing_0020: gentle axis, real curve, called Straight before.
        checkStr(flown(-0.3, 4.8, 0.7, 150.8, 5.5).name, "Fade",
                 "a 4.8 deg axis curving 6 yd is a fade, not a straight ball");
        checkStr(flown(-0.7, 3.5, -0.0, 148.5, 2.6).name, "Fade",
                 "…and so is a 3.5 deg one");
    }

    // ── Severity is measured, not assumed ───────────────────────────────────────
    //
    // Fade against slice is a question about how much the ball curved, so it is answered
    // with how much the ball curved. Curvature is the finish LESS where the ball would
    // have finished flying straight down its own start line — which is why a big pull
    // that never bent is not a hook.
    {
        // swing_0011 and swing_0007: 23-24 yd of curve, steep axes.
        checkStr(flown(0.6, 13.0, 2.1, 204.2, 26.1).name, "Slice", "a 24 yd curve is a slice");
        checkStr(flown(-2.7, 15.7, 3.2, 185.9, 14.9).name, "Pull–slice",
                 "…and starting left does not make it a fade");
        // swing_0010: axis only 7.8, but 14.2 yd of curve on a 178 yd carry — the
        // measured curvature promotes it where the axis alone would not have.
        checkStr(flown(-8.7, -7.8, -3.8, 177.8, -41.5).name, "Pull–hook",
                 "measured curvature promotes a draw to a hook");
        // swing_0021: started 3.1 deg left and finished 10.7 yd left having barely bent.
        // Offline says "miles left"; curvature says "straight", and curvature is right.
        checkStr(flown(-3.1, -0.7, -0.9, 167.0, -10.7).name, "Pull",
                 "a pull that never curved is a pull, not a hook");
        // swing_0001: an 8.5 deg axis that bent only 3.6 deg on a short carry. Under both
        // thresholds, so it stays a fade — and it stays one whether or not the device
        // reported the distances, which is what makes the two routes one boundary.
        checkStr(shape(0.8, 8.5).name, "Fade", "axis under 10 with no distances is a fade");
        checkStr(flown(0.8, 8.5, 1.3, 125.8, 9.8).name, "Fade",
                 "…and the measured curvature agrees");
    }

    // ── Boundaries, exactly on the threshold ────────────────────────────────────
    //
    // Every comparison is strict, so a reading sitting exactly on a threshold stays in
    // the calmer category. That is the right way round: a golfer told "straight" at
    // exactly 1.0 deg has been told something defensible, and one told "pull" has not.
    {
        checkStr(shape(-1.0, 0.0).name, "Straight", "start exactly -1.0 is still straight");
        checkStr(shape( 1.0, 0.0).name, "Straight", "start exactly +1.0 is still straight");
        checkStr(shape(-1.001, 0.0).name, "Pull",   "just past -1.0 is a pull");
        checkStr(shape( 1.001, 0.0).name, "Push",   "just past +1.0 is a push");

        checkStr(shape(0.0, -2.0).name, "Straight", "axis exactly -2.0 is still straight");
        checkStr(shape(0.0,  2.0).name, "Straight", "axis exactly +2.0 is still straight");
        checkStr(shape(0.0,  2.001).name, "Fade",   "just past +2.0 is a fade");
        checkStr(shape(0.0, -2.001).name, "Draw",   "just past -2.0 is a draw");

        // The axis severity route uses >=.
        checkStr(shape(0.0,  9.999).name, "Fade",  "axis just under 10 does not promote");
        checkStr(shape(0.0, 10.0).name,   "Slice", "axis exactly 10 does promote");
        checkStr(shape(0.0, -10.0).name,  "Hook",  "…and mirrors");

        // The curvature route also uses >=. 100 yd carry: 4.5 deg is 7.87 yd of bend.
        const double atThreshold = 100.0 * std::tan(4.5 * kDegToRadIr);
        checkStr(flown(0.0, 3.0, 0.0, 100.0, atThreshold * 0.99).name, "Fade",
                 "curvature just under 4.5 deg stays a fade");
        checkStr(flown(0.0, 3.0, 0.0, 100.0, atThreshold * 1.01).name, "Slice",
                 "curvature past 4.5 deg is a slice");
    }

    // ── Curvature is not offline ────────────────────────────────────────────────
    //
    // The distinction the whole severity tier rests on. Same finish, same axis, two very
    // different shots: one started straight and bent 20 yards, the other started 7 deg
    // right and bent almost nothing.
    {
        const LmFlightShape bent  = flown(0.0, 6.0, 2.0, 160.0, 20.0);
        const LmFlightShape aimed = flown(7.0, 6.0, 2.0, 160.0, 20.0);
        check(bent.hasCurve && aimed.hasCurve, "both carry a measured curvature");
        check(std::abs(bent.curveYd - 20.0) < 0.01, "a straight start makes finish = curve");
        check(aimed.curveYd < 1.0, "…while starting right accounts for nearly all of it");
        checkStr(bent.name,  "Slice",      "the one that bent 20 yd is a slice");
        checkStr(aimed.name, "Push–fade",  "the one that was aimed there is not");
    }

    // ── Left-handed: the gloss mirrors, the reading does not ────────────────────
    //
    // The rule from src/Metrics/metric_descriptor.h. A ball that started left and
    // curved right is a pull-fade for a right-hander and a push-draw for a left-hander:
    // same ball, same numbers, different words. The EVIDENCE keeps saying "left",
    // because that is still where it went.
    {
        const LmFlightShape r = flown(-1.3, 4.7, 1.6, 166.6, 5.1, false);
        const LmFlightShape l = flown(-1.3, 4.7, 1.6, 166.6, 5.1, true);
        checkStr(r.name, "Pull–fade", "right-handed reference shot");
        checkStr(l.name, "Push–draw", "the same shot, left-handed");
        check(r.windowIdx == 0 && l.windowIdx == 2, "the grid column mirrors with the name");
        check(r.curveIdx  == 2 && l.curveIdx  == 0, "the grid row mirrors with the name");
        check(r.evidence == l.evidence, "the evidence is the reading, so it does not mirror");
        check(l.evidence.contains(QStringLiteral("left")),
              "a left-hander's evidence still says the ball started left");
        check(std::abs(r.curveYd - l.curveYd) < 1e-9,
              "curvature is a measurement, so it does not mirror either");

        // Severity mirrors too: a left-hander's big curve is a hook, not a slice.
        checkStr(flown(0.6, 13.0, 2.1, 204.2, 26.1, true).name, "Hook",
                 "a right-handed slice is a left-hander's hook");
    }

    // ── The evidence line, to the character ─────────────────────────────────────
    //
    // This string is the audit trail for a claim the device did not make, and a future
    // export has to be able to quote it verbatim. It is a design deliverable, not a
    // debug print, so it is asserted exactly.
    //
    // IT CITES WHAT DECIDED THE NAME. It used to lead with face-to-path, which the model
    // no longer classifies on — an audit trail quoting a number the read did not use is
    // a justification, not a reason.
    {
        // The reference shot: started 1.3 left, so a straight ball finishes 3.8 yd left;
        // it finished 5.1 yd right, so it bent 8.9 yd.
        const LmFlightShape s = flown(-1.3, 4.7, 1.6, 166.6, 5.1);
        checkStr(s.evidence,
                 "start 1.3° left · axis 4.7° right · curved 8.9 yd right",
                 "reference evidence line");

        const LmFlightShape n = flown(2.0, -4.0, -2.5, 150.0, -8.0);
        checkStr(n.evidence,
                 "start 2.0° right · axis 4.0° left · curved 13.2 yd left",
                 "a left-curving shot");

        // A reading that rounds to zero gets no side word — it has no side.
        const LmFlightShape z = flown(0.0, 0.0, 0.0, 150.0, 0.0);
        checkStr(z.evidence, "start 0.0° · axis 0.0° · curved 0.0 yd",
                 "zero readings carry no direction");

        // No distances: the curve clause falls back to the finish, which is all that is
        // known, and the read still happens off the axis.
        const LmFlightShape noDist = lmFlightShape(-1.3, 4.7, 1.6,
                                                   std::optional<double>{}, 5.1);
        checkStr(noDist.evidence, "start 1.3° left · axis 4.7° right · finishes 5.1 yd right",
                 "no carry, so the finish stands in for the curve");
        check(noDist.has && !noDist.hasCurve, "…and it says it has no curvature");

        // No axis at all: the fallback names face-to-path, and says so.
        const LmFlightShape noAxis = lmFlightShape(-1.3, std::optional<double>{}, 1.6,
                                                   166.6, 5.1);
        checkStr(noAxis.evidence,
                 "start 1.3° left · face +1.6° to path · curved 8.9 yd right",
                 "with no axis the evidence cites what it did use");
        checkStr(noAxis.name, "Pull–fade", "…and the fallback still reads the shot");
    }

    // ── No read rather than a default one ───────────────────────────────────────
    {
        const std::optional<double> none;
        check(!lmFlightShape(none, 4.7, 1.6, 166.6, 5.1).has, "no start direction, no read");
        check(!lmFlightShape(-1.3, none, none, 166.6, 5.1).has,
              "neither axis nor face-to-path, no read");
        check(!lmFlightShape(none, none, none, none, none).has, "nothing at all, no read");
        check(lmFlightShape(-1.3, 4.7, none, none, none).has,
              "the axis alone is enough to name a shape");
        check(lmFlightShape(-1.3, none, 1.6, none, none).has,
              "…and so is face-to-path, as the fallback");
        const double nan = std::numeric_limits<double>::quiet_NaN();
        check(!lmFlightShape(nan, 4.7, 1.6, 166.6, 5.1).has, "a NaN is an absence, not a zero");
        check(!lmFlightShape(-1.3, nan, nan, 166.6, 5.1).has, "…on either curve signal");
    }

    // ── Strike quality ──────────────────────────────────────────────────────────
    //
    // The brief's reference strike: 3 mm toe, 6 mm low. Inside the centre horizontally,
    // outside it vertically, and not far enough out to be thin — so LOW.
    {
        const LmStrikeRead s = lmStrikeQuality(3.0, -6.0, 1.40, 1.39);
        check(s.has, "reference strike reads");
        checkStr(s.name, "Low", "reference strike is low on the face");
        checkStr(s.evidence, "smash +0.01 vs μ", "reference strike context");
    }

    // ── Every strike category ───────────────────────────────────────────────────
    {
        const std::optional<double> no;
        checkStr(lmStrikeQuality( 0.0,  0.0, no, no).name, "Flushed", "dead centre");
        checkStr(lmStrikeQuality( 2.0, -2.0, no, no).name, "Flushed", "inside the centre box");
        checkStr(lmStrikeQuality( 8.0,  1.0, no, no).name, "Toe",     "horizontal, toe side");
        checkStr(lmStrikeQuality(-8.0,  1.0, no, no).name, "Heel",    "horizontal, heel side");
        checkStr(lmStrikeQuality( 0.0,  6.0, no, no).name, "High",    "vertical, mid tier, high");
        checkStr(lmStrikeQuality( 0.0, -6.0, no, no).name, "Low",     "vertical, mid tier, low");
        checkStr(lmStrikeQuality( 0.0, 12.0, no, no).name, "Fat",     "vertical, severe, high");
        checkStr(lmStrikeQuality( 0.0,-12.0, no, no).name, "Thin",    "vertical, severe, low");
    }

    // ── Strike boundaries ───────────────────────────────────────────────────────
    {
        const std::optional<double> no;
        checkStr(lmStrikeQuality(3.0,  3.0, no, no).name, "Flushed",
                 "exactly 3 mm in both axes is still flushed");
        checkStr(lmStrikeQuality(3.001, 3.0, no, no).name, "Toe",
                 "a hair past 3 mm horizontally is a toe strike");
        checkStr(lmStrikeQuality(0.0, 10.0, no, no).name, "High",
                 "exactly 10 mm vertical is still the mild tier");
        checkStr(lmStrikeQuality(0.0, 10.001, no, no).name, "Fat",
                 "just past 10 mm vertical is fat");
        // A tie between the two deviations goes to the vertical: it is the miss that
        // costs distance and the one a golfer can feel.
        checkStr(lmStrikeQuality(6.0, -6.0, no, no).name, "Low",
                 "an equal miss in both axes is named by the vertical");
        checkStr(lmStrikeQuality(6.0, -5.9, no, no).name, "Toe",
                 "the larger deviation names it once they differ");
    }

    // ── Smash is context, never a vote ──────────────────────────────────────────
    //
    // The same face location classifies identically whatever the smash did. If smash
    // were allowed in, a fast swing's toe strike would read as centred — the opposite
    // of what the card exists to show.
    {
        const std::optional<double> no;
        const LmStrikeRead good = lmStrikeQuality(9.0, 0.0, 1.50, 1.30);
        const LmStrikeRead bad  = lmStrikeQuality(9.0, 0.0, 1.10, 1.30);
        checkStr(good.name, "Toe", "a toe strike with a great smash is still a toe strike");
        checkStr(bad.name,  "Toe", "and so is one with a poor smash");
        checkStr(good.evidence, "smash +0.20 vs μ", "smash shown as context, above the mean");
        checkStr(bad.evidence,  "smash −0.20 vs μ", "smash shown as context, below the mean");
        check(lmStrikeQuality(9.0, 0.0, no, no).evidence.isEmpty(),
              "no session mean, no smash context");
    }

    // ── No read rather than a default one ───────────────────────────────────────
    {
        const std::optional<double> none;
        check(!lmStrikeQuality(none, -6.0, none, none).has, "no strike location, no read");
        check(!lmStrikeQuality(3.0, none, none, none).has,  "no strike height, no read");
        const double nan = std::numeric_limits<double>::quiet_NaN();
        check(!lmStrikeQuality(nan, -6.0, none, none).has,  "a NaN is an absence, not a zero");
    }

    // ── Tuning is honoured ──────────────────────────────────────────────────────
    //
    // The thresholds are coach-set config, so a coach changing them has to change the
    // reads. A test that only ever exercised the defaults would not notice a function
    // that had quietly hard-coded them back in.
    {
        LmInferenceTuning t;
        t.startT = 3.0; t.curveT = 5.0; t.centreMm = 8.0;
        checkStr(lmFlightShape(-2.0, 4.0, 0.0, std::optional<double>{},
                               std::optional<double>{}, false, t).name,
                 "Straight", "a wider window and a wider axis forgive the same shot");

        // The two severity routes are independently tunable, which is what lets a coach
        // trust one device's axis more than another's.
        LmInferenceTuning strict;
        strict.severeAxisT = 5.0;
        checkStr(lmFlightShape(0.0, 6.0, 0.0, std::optional<double>{},
                               std::optional<double>{}, false, strict).name,
                 "Slice", "a lower axis threshold promotes sooner");
        LmInferenceTuning lenient;
        lenient.severeCurveDeg = 20.0;
        checkStr(lmFlightShape(-8.7, -7.8, -3.8, 177.8, -41.5, false, lenient).name,
                 "Pull–draw", "…and a higher curvature threshold stops promoting");

        checkStr(lmStrikeQuality(6.0, -6.0, std::optional<double>{}, std::optional<double>{}, t).name,
                 "Flushed", "a wider centre forgives the same strike");
    }

    std::printf("%s\n", g_fail == 0 ? "OK" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
