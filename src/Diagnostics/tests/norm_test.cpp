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
#include <limits>

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

        check(near(n.claimLo(), 2.2) && near(n.claimHi(), 3.0), "the claim is the old green band");
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

    std::printf("=== norm: the claim, and the Ideal band it is NOT ===\n");
    {
        Norm n = symmetric(10.0, 0.0);
        n.sigmaLo = 2.0;
        n.sigmaHi = 5.0;
        check(near(n.claimLo(), 8.0),  "claimLo = mu - sigmaLo");
        check(near(n.claimHi(), 15.0), "claimHi = mu + sigmaHi");

        // The claim is policy-free by construction — it reads no policy at all. The Ideal band is
        // the claim scaled by idealMaxZ, and the two are equal ONLY under `standard`. Asserting
        // both here is what stops a future author reading claimLo() as "the green edge".
        for (const GradePolicyPreset &p : gradePolicyPresets()) {
            const NormBandEdges e = bandEdgesOf(n, p.policy);
            check(near(e.idealLo, 10.0 - p.policy.idealMaxZ * 2.0)
                  && near(e.idealHi, 10.0 + p.policy.idealMaxZ * 5.0),
                  "the drawn Ideal band scales with idealMaxZ");
        }
        check(near(bandEdgesOf(n, gradePolicyByName(QStringLiteral("standard"))).idealLo,
                   n.claimLo()),
              "…and coincides with the claim under standard, which is why this hid for so long");
        check(!near(bandEdgesOf(n, gradePolicyByName(QStringLiteral("strict"))).idealLo,
                    n.claimLo()),
              "…and does not under strict");
    }

    std::printf("=== norm: the drawn Ideal edge is the graded Ideal edge, per preset ===\n");
    {
        // THE regression gate for the divergence. bandEdgesOf() drew mu +/- sigma while grade()
        // applied policy.idealMaxZ, so under `strict` a value at 0.9 sigma was inside the drawn
        // green band and graded Good — and ragOf(Good) is Amber. Green band, amber chip, one
        // number. The edge is now computed the same way on both paths, so they agree by
        // construction; this sweeps every preset at the edge and either side of it to prove it.
        //
        // mu = 1.40, sigmaLo = 0.08 is the exact float case norm.h's own comment calls out.
        const Norm rows[] = { symmetric(1.40, 0.08), symmetric(0.0, 5.0), symmetric(65.0, 10.0) };

        for (const GradePolicyPreset &p : gradePolicyPresets()) {
            for (const Norm &n : rows) {
                const NormBandEdges e = bandEdgesOf(n, p.policy);
                for (double eps : { 0.0, -1e-9, -1e-3 }) {
                    check(grade(e.idealLo - eps, n, p.policy) == Grade::Ideal,
                          "inside the drawn low Ideal edge grades Ideal");
                    check(grade(e.idealHi + eps, n, p.policy) == Grade::Ideal,
                          "inside the drawn high Ideal edge grades Ideal");
                }
                // And a value inside the drawn green band never carries an Amber chip — the
                // user-visible form of the same claim.
                check(ragOf(grade(e.idealLo, n, p.policy)) == PpRag::Green
                      && ragOf(grade(e.idealHi, n, p.policy)) == PpRag::Green
                      && ragOf(grade(n.mu, n, p.policy)) == PpRag::Green,
                      "a value inside the drawn green band is never Amber");
            }
        }
    }

    std::printf("=== norm: the grade policy presets nest ===\n");
    {
        // The engine depends on this and nothing asserted it: `onTail` compares against the Ideal
        // edge while `deviated` comes from grade(), so a preset with goodMaxZ < idealMaxZ would
        // admit a reading that is a deviation and on neither tail — a signal that can never fire,
        // for a reason nothing reports. It held only because three hand-written presets happened
        // to be ordered.
        for (const GradePolicyPreset &p : gradePolicyPresets())
            check(gradePolicyIsOrdered(p.policy), p.name);

        check(!gradePolicyIsOrdered(GradePolicy{ 2.0, 1.0, 3.0 }), "good below ideal is refused");
        check(!gradePolicyIsOrdered(GradePolicy{ 1.0, 3.0, 2.0 }), "watch below good is refused");
        check(!gradePolicyIsOrdered(GradePolicy{ 0.0, 2.0, 3.0 }), "a zero ideal cap is refused");
    }

    std::printf("=== norm: a FLOOR grades one tail ===\n");
    {
        // Smash factor, driver. There is no upper fault in this quantity: a golfer approaching
        // perfect energy transfer is not deviating from anything.
        const Norm n = symmetric(1.48, 0.05);
        const Shape f = Shape::Floor;

        check(grade(1.48, n, {}, f) == Grade::Ideal, "the aspiration itself is Ideal");
        check(grade(1.55, n, {}, f) == Grade::Ideal, "above it is Ideal — this was Good before");
        check(grade(9.99, n, {}, f) == Grade::Ideal, "…however far above; the tail does not grade");
        check(grade(1.43, n, {}, f) == Grade::Ideal, "one tolerance below is still Ideal");
        check(grade(1.40, n, {}, f) == Grade::Good,  "…two below is ordinary variation");
        check(grade(1.35, n, {}, f) == Grade::Watch, "…three below is a deviation");
        check(grade(1.30, n, {}, f) == Grade::Action, "…beyond that is Action");

        // z is 0 on the good side, not a positive distance. A 0-100 score built on this must not
        // reward overshooting a floor.
        check(near(normZ(1.48, n, f), 0.0), "z is 0 AT the aspiration");
        check(near(normZ(1.60, n, f), 0.0), "…and 0 above it, never a reward");
        check(near(normZ(1.43, n, f), -1.0), "…and the ordinary per-side z below it");

        // Continuity at mu: both formulations agree there, which is what makes the piecewise
        // definition a function rather than a pair of them.
        check(near(normZ(1.48 - 1e-12, n, f), 0.0, 1e-9) && near(normZ(1.48, n, f), 0.0),
              "continuous at mu, from both sides");

        // The good side is Ideal AT EVERY POLICY, by construction — there is no threshold for it
        // to fall outside of.
        for (const GradePolicyPreset &p : gradePolicyPresets())
            check(grade(2.0, n, p.policy, f) == Grade::Ideal, p.name);

        // The single computed edge obeys the float-edge doctrine exactly as a target's does.
        for (const GradePolicyPreset &p : gradePolicyPresets()) {
            const NormBandEdges e = bandEdgesOf(n, p.policy, -1.0, f);
            check(e.highOpen && !e.lowOpen, "a floor is open above and graded below");
            check(near(e.idealHi, n.mu) && near(e.watchHi, n.mu),
                  "…and its open-side edges are mu, never a sentinel");
            check(grade(e.idealLo, n, p.policy, f) == Grade::Ideal, "ON the drawn edge is Ideal");
            check(grade(e.idealLo + 1e-9, n, p.policy, f) == Grade::Ideal, "…and just inside");
            check(grade(e.watchLo - 1e-9, n, p.policy, f) == Grade::Action, "…just outside Watch");
        }

        // The SwingLab margin sweep widens the GRADED side only. Applied to the open side it would
        // push a number past the aspiration and invent a ceiling the norm does not hold.
        const NormBandEdges swept = bandEdgesOf(n, {}, 0.10, f);
        check(near(swept.watchLo, (n.mu - n.sigmaLo) - 0.10), "a margin sweep widens the low tail");
        check(near(swept.watchHi, n.mu) && swept.highOpen,
              "…and leaves the open tail at the aspiration");
    }

    std::printf("=== norm: a CEILING is the mirror ===\n");
    {
        // A magnitude whose domain is [0, inf) and whose ideal is zero.
        const Norm n = symmetric(0.0, 2.0);
        const Shape c = Shape::Ceiling;

        check(grade(0.0,  n, {}, c) == Grade::Ideal, "zero is Ideal");
        check(grade(-9.0, n, {}, c) == Grade::Ideal, "below is Ideal — the tail does not grade");
        check(grade(2.0,  n, {}, c) == Grade::Ideal, "one tolerance above is still Ideal");
        check(grade(4.0,  n, {}, c) == Grade::Good,  "…two above is ordinary variation");
        check(grade(6.0,  n, {}, c) == Grade::Watch, "…three above is a deviation");
        check(grade(6.1,  n, {}, c) == Grade::Action, "…beyond that is Action");

        check(near(normZ(-5.0, n, c), 0.0), "z is 0 on the good side");
        check(near(normZ(2.0, n, c), 1.0),  "…and the ordinary per-side z above it");

        const NormBandEdges e = bandEdgesOf(n, {}, -1.0, c);
        check(e.lowOpen && !e.highOpen, "a ceiling is open below and graded above");
        check(near(e.idealLo, n.mu) && near(e.watchLo, n.mu), "…with mu on the open side");
    }

    std::printf("=== norm: a one-sided monitor is a COMPLETE monitor ===\n");
    {
        // hasExplicitMonitor() wants both bounds on a Target and exactly one on a one-sided
        // measure. Answering the Target way for a floor would make grade() ignore a bound the
        // author wrote down — authored corridor, unauthored grading.
        Norm n      = symmetric(1.48, 0.05);
        n.monitorLo = 1.30;

        check(!n.hasExplicitMonitor(), "half a band is not a monitor on a Target");
        check(n.hasExplicitMonitor(Shape::Floor), "…and IS one on a Floor");

        // Monitor precedence survives on the graded tail: outside is Action, inside is capped at
        // Watch however many tolerances out.
        check(grade(1.29, n, {}, Shape::Floor) == Grade::Action, "below the monitor is Action");
        check(grade(1.31, n, {}, Shape::Floor) == Grade::Watch,
              "…and inside it is capped at Watch, though 1.31 is 3.4 tolerances out");
        check(grade(2.00, n, {}, Shape::Floor) == Grade::Ideal, "the open tail is untouched by it");
    }

    std::printf("=== norm: a reading nobody believes is not a grade ===\n");
    {
        // A driver smash of 1.62 is a mis-tracked ball, not a swing finding. Grading it in EITHER
        // direction would launder a capture fault into a confident diagnosis.
        Norm n        = symmetric(1.48, 0.05);
        n.plausibleHi = 1.56;

        check(grade(1.55, n, {}, Shape::Floor) == Grade::Ideal, "inside the cap, graded normally");
        check(grade(1.62, n, {}, Shape::Floor) == Grade::NotMeasured, "outside it, not graded");
        check(n.isImplausible(1.62) && !n.isImplausible(1.55), "…and the reading says which it is");

        // NotMeasured, never Action — and never a pass either. It is the third state.
        check(!isDeviation(grade(1.62, n, {}, Shape::Floor)),
              "an implausible reading is not a deviation");
        check(ragOf(grade(1.62, n, {}, Shape::Floor)) == PpRag::Grey, "…and renders Grey");

        // It outranks the monitor band, which is otherwise the strongest rule in grade().
        Norm m        = symmetric(10.0, 1.0);
        m.monitorLo   = 5.0;
        m.monitorHi   = 15.0;
        m.plausibleHi = 40.0;
        check(grade(20.0, m) == Grade::Action,      "past the monitor is Action");
        check(grade(50.0, m) == Grade::NotMeasured, "…but past belief is not graded at all");

        // Bounds may appear singly: a floor caps above and says nothing below.
        Norm lo        = symmetric(10.0, 1.0);
        lo.plausibleLo = 0.0;
        check(grade(-1.0, lo) == Grade::NotMeasured && grade(99.0, lo) == Grade::Action,
              "a lone low bound disbelieves below and still grades above");
    }

    std::printf("=== norm: a degenerate one-sided norm ===\n");
    {
        // Zero tolerance on the graded side admits only mu there; the open side is still open.
        // withinBand has no special case for this any more — the computed edge mu - t*0 == mu
        // gives the same answer through the same arithmetic as everything else.
        const Norm n = symmetric(1.0, 0.0);
        check(grade(1.0, n, {}, Shape::Floor) == Grade::Ideal,  "the centre grades Ideal");
        check(grade(5.0, n, {}, Shape::Floor) == Grade::Ideal,  "…so does the whole open side");
        check(grade(0.9, n, {}, Shape::Floor) == Grade::Action, "…and anything below is Action");
        check(grade(0.9, n) == Grade::Action && grade(1.1, n) == Grade::Action,
              "…while a degenerate TARGET refuses both sides, as it always did");
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

    // ── Saying what a corridor is, in words ─────────────────────────────────
    //
    // Six surfaces render a corridor as a sentence. The rule lives here so they cannot drift, and
    // is gated here because a phrase built inside a QML binding is a phrase nothing can test.
    std::printf("\nphrasing\n");
    {
        // FAITHFUL numbers. The pack is mostly authored at one decimal, which is why every surface
        // was fixed there; the ratios are not, and at one decimal smash factor's Ideal and Good
        // edges (1.43, 1.38) render as the same "1.4".
        check(normNumber(1.5)  == QLatin1String("1.5"),  "a one-decimal figure stays one decimal");
        check(normNumber(20.0) == QLatin1String("20.0"), "…and a whole number keeps its decimal");
        check(normNumber(1.48) == QLatin1String("1.48"), "a two-decimal figure is not rounded away");
        check(normNumber(0.05) == QLatin1String("0.05"), "…nor a small tolerance, where it doubles it");
        check(normNumber(1.48 - 0.05) == QLatin1String("1.43"),
              "…and float noise off a subtraction does not force a fourth decimal");
        // QStringLiteral, not QLatin1String: the dash is U+2014 and QLatin1String would compare its
        // three UTF-8 bytes as three Latin-1 characters.
        check(normNumber(std::numeric_limits<double>::quiet_NaN()) == QStringLiteral("—"),
              "a non-finite number is a dash, never a rendered NaN");
        check(normNumber(std::numeric_limits<double>::infinity()) == QStringLiteral("—"),
              "…infinity too, which is what an unguarded open edge used to produce");

        // A one-sided corridor has NO second bound, so naming one states a limit on the very side
        // the norm refuses to grade.
        check(rangePhrase(1.43, 1.53, 1.48, Shape::Target) == QLatin1String("1.43 to 1.53"),
              "a target corridor names both bounds");
        check(rangePhrase(1.43, 1.53, 1.48, Shape::Floor) == QLatin1String("at least 1.48"),
              "a floor names the aspiration and nothing above it");
        check(rangePhrase(1.43, 1.53, 1.48, Shape::Ceiling) == QLatin1String("no more than 1.48"),
              "a ceiling mirrors it");

        // mu, not the pair, on a one-sided norm — which is the whole reason mu is a parameter.
        // A CLAIM's high edge on a floor is mu + a tolerance nothing grades; a BAND's is already
        // collapsed onto mu. Both callers are correct without either knowing which it holds.
        check(rangePhrase(1.43, 1.48, 1.48, Shape::Floor)
                  == rangePhrase(1.43, 1.53, 1.48, Shape::Floor),
              "a collapsed band and an uncollapsed claim phrase identically on a floor");

        check(actionPhrase(1.33, 1.63, Shape::Target) == QLatin1String("action beyond 1.33 to 1.63"),
              "a target norm faults on both tails");
        check(actionPhrase(1.33, 1.48, Shape::Floor) == QLatin1String("action below 1.33"),
              "a floor faults below and never 'beyond' — the open tail has no edge to be beyond");
        check(actionPhrase(1.48, 1.63, Shape::Ceiling) == QLatin1String("action above 1.63"),
              "…and a ceiling above");

        // An implausible reading is a THIRD statement, not a grade and not an absence. The reading
        // is shown, because hiding it is what makes a mis-tracked ball look like a capture gap.
        check(implausibleLabel() != gradeLabel(Grade::NotMeasured),
              "'not believed' is not the same word as 'not measured'");
        check(implausibleLabel() != gradeLabel(Grade::Action),
              "…nor the same word as a swing fault");
        const QString note = implausibleNote(1.62, QStringLiteral("ratio"));
        check(note.contains(QLatin1String("1.62")), "the note SHOWS the reading");
        check(note.contains(QLatin1String("ratio")), "…in its unit");
        check(note.contains(QLatin1String("Check the capture")),
              "…and points at the capture rather than the swing");
        check(!implausibleNote(1.62, QString()).contains(QLatin1String("  ")),
              "a measure with no unit leaves no double space behind it");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
