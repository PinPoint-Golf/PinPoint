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

// The shape of one shot, right-handed unless said otherwise.
static LmFlightShape shape(double startDir, double f2p, double axis = 0.0,
                           bool leftHanded = false)
{
    return lmFlightShape(startDir, f2p, axis, std::optional<double>{}, leftHanded);
}

int main()
{
    std::printf("lm_inferred_reads_test\n");

    // ── The nine windows ────────────────────────────────────────────────────────
    //
    // Every cell of the 3 x 3, named. The grid on the card has nine lights and each one
    // has to be reachable, or a golfer will hit a shot the panel has no word for.
    {
        checkStr(shape(-3.0, -3.0).name, "Pull–draw",  "left window, left curve");
        checkStr(shape(-3.0,  0.0).name, "Pull",       "left window, no curve");
        checkStr(shape(-3.0,  3.0).name, "Pull–fade",  "left window, right curve");
        checkStr(shape( 0.0, -3.0).name, "Draw",       "centre window, left curve");
        checkStr(shape( 0.0,  0.0).name, "Straight",   "centre window, no curve");
        checkStr(shape( 0.0,  3.0).name, "Fade",       "centre window, right curve");
        checkStr(shape( 3.0, -3.0).name, "Push–draw",  "right window, left curve");
        checkStr(shape( 3.0,  0.0).name, "Push",       "right window, no curve");
        checkStr(shape( 3.0,  3.0).name, "Push–fade",  "right window, right curve");
    }

    // ── The grid indices agree with the words ───────────────────────────────────
    //
    // The lit cell and the label are read together; if they can disagree, one of them
    // is lying and the reader cannot tell which.
    {
        const LmFlightShape s = shape(-3.0, 3.0);
        check(s.windowIdx == 0 && s.curveIdx == 2, "Pull-fade lights the left/fade cell");
        const LmFlightShape c = shape(0.0, 0.0);
        check(c.windowIdx == 1 && c.curveIdx == 1, "Straight lights the centre cell");
        const LmFlightShape p = shape(3.0, -3.0);
        check(p.windowIdx == 2 && p.curveIdx == 0, "Push-draw lights the right/draw cell");
    }

    // ── Severity: two routes in, and the axis can only promote ──────────────────
    {
        checkStr(shape(0.0, -5.0).name, "Hook",  "face-to-path past severe is a hook");
        checkStr(shape(0.0,  5.0).name, "Slice", "face-to-path past severe is a slice");
        // The axis corroborates: it turns a draw into a hook...
        checkStr(shape(0.0, -2.0,  -10.0).name, "Hook", "a steep axis promotes a draw");
        checkStr(shape(0.0,  2.0,   10.0).name, "Slice", "a steep axis promotes a fade");
        // ...but it can never invent a curve the face never showed. A ball hit with a
        // square face does not become a slice because the axis reading was noisy.
        checkStr(shape(0.0,  0.0,   14.0).name, "Straight", "a steep axis alone is no curve");
        checkStr(shape(0.0,  0.5,   14.0).name, "Straight", "nor with a sub-threshold face");
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

        checkStr(shape(0.0, -1.0).name, "Straight", "face exactly -1.0 is still straight");
        checkStr(shape(0.0,  1.0).name, "Straight", "face exactly +1.0 is still straight");
        checkStr(shape(0.0,  1.001).name, "Fade",   "just past +1.0 is a fade");

        checkStr(shape(0.0,  4.0).name, "Fade",     "face exactly +4.0 is still a fade");
        checkStr(shape(0.0,  4.001).name, "Slice",  "just past +4.0 is a slice");
        checkStr(shape(0.0, -4.0).name, "Draw",     "face exactly -4.0 is still a draw");
        checkStr(shape(0.0, -4.001).name, "Hook",   "just past -4.0 is a hook");

        // The axis route uses >=, per the brief's "or |spin axis| >= 10 deg".
        checkStr(shape(0.0, 2.0,  9.999).name, "Fade",  "axis just under 10 does not promote");
        checkStr(shape(0.0, 2.0, 10.0).name,   "Slice", "axis exactly 10 does promote");
    }

    // ── Left-handed: the gloss mirrors, the reading does not ────────────────────
    //
    // The rule from src/Metrics/metric_descriptor.h. A ball that started left and
    // curved right is a pull-fade for a right-hander and a push-draw for a left-hander:
    // same ball, same numbers, different words. The EVIDENCE keeps saying "left",
    // because that is still where it went.
    {
        const LmFlightShape r = lmFlightShape(-1.3, 1.6, 4.7, 5.1, false);
        const LmFlightShape l = lmFlightShape(-1.3, 1.6, 4.7, 5.1, true);
        checkStr(r.name, "Pull–fade", "right-handed reference shot");
        checkStr(l.name, "Push–draw", "the same shot, left-handed");
        check(r.windowIdx == 0 && l.windowIdx == 2, "the grid column mirrors with the name");
        check(r.curveIdx  == 2 && l.curveIdx  == 0, "the grid row mirrors with the name");
        check(r.evidence == l.evidence, "the evidence is the reading, so it does not mirror");
        check(l.evidence.contains(QStringLiteral("left")),
              "a left-hander's evidence still says the ball started left");
    }

    // ── The evidence line, to the character ─────────────────────────────────────
    //
    // This string is the audit trail for a claim the device did not make, and a future
    // export has to be able to quote it verbatim. It is a design deliverable, not a
    // debug print, so it is asserted exactly.
    {
        const LmFlightShape s = lmFlightShape(-1.3, 1.6, 4.7, 5.1);
        checkStr(s.evidence,
                 "start 1.3° left · face +1.6° to path · finishes 5.1 yd right",
                 "reference evidence line");

        // Negative face-to-path keeps its sign, as a typographic minus.
        const LmFlightShape n = lmFlightShape(2.0, -2.5, 0.0, -8.0);
        checkStr(n.evidence,
                 "start 2.0° right · face −2.5° to path · finishes 8.0 yd left",
                 "signed face, left finish");

        // A reading that rounds to zero gets no side word — it has no side.
        const LmFlightShape z = lmFlightShape(0.0, 0.0, 0.0, 0.0);
        checkStr(z.evidence,
                 "start 0.0° · face 0.0° to path · finishes 0.0 yd",
                 "zero readings carry no direction");

        // Offline is optional context; without it the line simply stops.
        const LmFlightShape noOff = lmFlightShape(-1.3, 1.6, 4.7, std::optional<double>{});
        checkStr(noOff.evidence, "start 1.3° left · face +1.6° to path",
                 "no offline, no finish clause");
        check(noOff.has, "a missing offline does not suppress the read");
    }

    // ── No read rather than a default one ───────────────────────────────────────
    {
        const std::optional<double> none;
        check(!lmFlightShape(none, 1.6, 4.7, 5.1).has,   "no start direction, no read");
        check(!lmFlightShape(-1.3, none, 4.7, 5.1).has,  "no face-to-path, no read");
        check(!lmFlightShape(none, none, none, none).has, "nothing at all, no read");
        check(lmFlightShape(-1.3, 1.6, none, none).has,
              "the spin axis only corroborates, so its absence is survivable");
        const double nan = std::numeric_limits<double>::quiet_NaN();
        check(!lmFlightShape(nan, 1.6, 4.7, 5.1).has, "a NaN is an absence, not a zero");
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
        t.startT = 3.0; t.curveT = 3.0; t.centreMm = 8.0;
        checkStr(lmFlightShape(-2.0, 2.0, 0.0, std::optional<double>{}, false, t).name,
                 "Straight", "a wider window forgives the same shot");
        checkStr(lmStrikeQuality(6.0, -6.0, std::optional<double>{}, std::optional<double>{}, t).name,
                 "Flushed", "a wider centre forgives the same strike");
    }

    std::printf("%s\n", g_fail == 0 ? "OK" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
