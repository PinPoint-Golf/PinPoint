// Standalone tests for the session diagnostics ledger (src/Analysis/diagnostic_ledger.h):
// the statistical primitives against hand-computed values, every tier and gate boundary,
// the stage ratchet, all five link grades with both of their caps, chain extraction over a
// planted corpus, the anti-churn devices, serialisation, one dedicated test per never-do
// rule in the design brief, and a parity block that replays the design mock's own nine
// fires arrays. Pure — no OpenCV, no fixture, no pack. Own main()/check(). Header-only, so
// nothing is compiled alongside it.
//
//   cmake --build build/analysis-tests --target diagnostic_ledger_test
//   ctest --test-dir build/analysis-tests -R diagnostic_ledger --output-on-failure

#include "../diagnostic_ledger.h"
#include "synthetic_session_corpus.h"

#include <QJsonDocument>

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace pinpoint::analysis;
namespace cp = pinpoint::analysis::corpus;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// ── Session builders ────────────────────────────────────────────────────────────
//
// A plan is the mock's own encoding — 1 fired, 0 clean, −1 not assessable — so a fires
// array can be pasted straight out of `Session Dashboard.dc.html`. `zs` overrides the
// default z where a test cares about the magnitude (trend, bookends, moved-together);
// otherwise a firing sits at 3.0 sigma and a clean shot at 0.5, either side of the ±2
// corridor every row here is graded against.
struct Plan {
    QString id;
    std::vector<int> states;
    std::vector<double> zs;
    int  dir = 1;            // direction on every firing
    int  flipAfter = -1;     // firings at or past this ordinal carry −dir instead
};

static Plan plan(const char *id, std::vector<int> states)
{
    Plan p;
    p.id = QString::fromLatin1(id);
    p.states = std::move(states);
    return p;
}

static std::vector<ShotRecord> sessionOf(const std::vector<Plan> &plans)
{
    size_t n = 0;
    for (const Plan &p : plans) n = std::max(n, p.states.size());

    std::vector<ShotRecord> shots;
    shots.reserve(n);
    std::vector<int> firingOrdinal(plans.size(), 0);

    for (size_t i = 0; i < n; ++i) {
        ShotRecord s;
        s.shotId = int(i) + 1;
        s.club = QStringLiteral("7i");
        s.timestampMs = qint64(1'700'000'000'000LL) + qint64(i) * 45'000LL;
        for (size_t k = 0; k < plans.size(); ++k) {
            const Plan &p = plans[k];
            ConditionRow r;
            r.conditionId      = p.id;
            r.drivingMeasureId = p.id + QStringLiteral(".measure");
            r.corridorLo = -2.0;
            r.corridorHi =  2.0;
            const int v = i < p.states.size() ? p.states[i] : cp::kNotAssessable;
            const bool haveZ = i < p.zs.size();
            if (v == cp::kFired) {
                r.state = ShotState::Fired;
                const int ord = firingOrdinal[k]++;
                r.direction = (p.flipAfter >= 0 && ord >= p.flipAfter) ? -p.dir : p.dir;
                r.z = (haveZ ? p.zs[i] : 3.0) * (r.direction < 0 ? -1.0 : 1.0);
            } else if (v == cp::kClean) {
                r.state = ShotState::Clean;
                r.z = haveZ ? p.zs[i] : 0.5;
            } else {
                r.state = ShotState::NotAssessable;
                r.notAssessableReason = QStringLiteral("ball not tracked");
                r.z = std::numeric_limits<double>::quiet_NaN();
            }
            r.value = r.z;
            s.rows.push_back(std::move(r));
        }
        shots.push_back(std::move(s));
    }
    return shots;
}

// Warm-up handling OFF — the design mock has none (see shotWeight()'s upstream note), and
// most gate boundaries below are stated in raw counts. The weighting gets its own section.
static LedgerOptions flatOpts()
{
    LedgerOptions o;
    o.warmUpWeight = 1.0;
    return o;
}

static const ConditionLedger &led(const std::vector<ConditionLedger> &ls, const char *id)
{
    static ConditionLedger empty;
    const ConditionLedger *l = ledgerFor(ls, QString::fromLatin1(id));
    return l ? *l : empty;
}

// ── The design mock's nine conditions, verbatim ─────────────────────────────────
//
// `Session Dashboard.dc.html`, `_sdConditions()`, lines ~1634–1690. Pasted rather than
// paraphrased: this block's entire job is to fail if our arithmetic and the design's ever
// part company.
static std::vector<Plan> mockPlans()
{
    std::vector<Plan> ps = {
        plan("transition_rush", {1,1,1,1,1, 1,1,1,1,0, 0,0,0,0}),
        plan("casting",         {0,1,-1,1,1, 1,1,1,1,0, 0,1,-1,0}),
        plan("shaft_lean",      {1,1,1,-1,1, 1,1,1,1,1, 1,-1,1,1}),
        plan("scooping",        {0,1,1,1,0, 1,1,0,1,1, 0,1,0,1}),
        plan("out_to_in",       {1,0,1,1,-1, 1,1,0,1,1, 0,1,-1,1}),
        plan("face_roll",       {0,1,1,1,0, 1,1,0,1,1, 0,1,1,0}),
        plan("deceleration",    {0,0,1,0,0, 0,0,0,1,0, 0,0,0,0}),
        plan("head_sway",       {0,1,0,0,0, 1,0,0,0,0, 0,1,0,0}),
        plan("bent_lead_arm",   {0,0,-1,0,0, 0,1,0,0,0, 0,0,-1,0})
    };
    // face_roll is the mock's dispersion case: nine firings, five open and four shut, so
    // agreement lands at 0.56 and the direction claim is suppressed.
    for (Plan &p : ps) if (p.id == QStringLiteral("face_roll")) p.flipAfter = 5;
    return ps;
}

int main()
{
    std::printf("diagnostic_ledger_test\n");
    const LedgerOptions O = flatOpts();

    // ── Wilson score lower bound ────────────────────────────────────────────────
    //
    // The whole reason the gate can be one number. 8-of-10 and 1-of-2 have raw rates of
    // 0.80 and 0.50 — close enough that a raw-rate gate needs a second "and n must be…"
    // rule bolted on. Their lower bounds are 0.49 and 0.09, which is not close at all.
    {
        check(near(wilsonLowerBound(0, 0), 0.0, 1e-12), "no assessable shots is no evidence, not a NaN");
        check(near(wilsonLowerBound(0, 10), 0.0, 1e-12), "never fired bottoms out at zero");
        check(near(wilsonLowerBound(8, 10), 0.4901568, 1e-6), "8 of 10 — hand-computed");
        check(near(wilsonLowerBound(1, 2),  0.0945287, 1e-6), "1 of 2 — hand-computed");
        check(wilsonLowerBound(8, 10) > wilsonLowerBound(1, 2),
              "…and 8 of 10 outranks 1 of 2, which the raw rate barely does");
        check(near(wilsonLowerBound(10, 10), 0.7224598, 1e-6), "10 of 10 is still not 1.0");

        // Same rate, less evidence: the bound has to fall, or warm-up weighting means
        // nothing (the weighting works by shrinking n as well as k).
        check(wilsonLowerBound(4, 5) < wilsonLowerBound(8, 10), "same rate, fewer shots, lower bound");
        check(near(wilsonLowerBound(4, 5), 0.3755283, 1e-6), "…and the fractional path agrees");

        // Never negative. The gate compares against 0.30 and a bound below zero is not a
        // rate anyone can be shown.
        bool everNegative = false;
        for (int k = 0; k <= 20; ++k)
            for (int n = k; n <= 20; ++n)
                if (n > 0 && wilsonLowerBound(k, n) < 0.0) everNegative = true;
        check(!everNegative, "the bound is clamped at zero everywhere");
    }

    // ── Theil–Sen: one wild shot must not move the arrow ────────────────────────
    {
        check(near(theilSenSlope(std::vector<double>{ 1, 2, 3, 4, 5 }), 1.0, 1e-12),
              "a clean ramp has the ramp's slope");
        check(near(theilSenSlope(std::vector<double>{ 5, 4, 3, 2, 1 }), -1.0, 1e-12),
              "…and the other way round");
        // The shank. Least squares on this series returns ≈ 12; the median of pairwise
        // slopes returns the slope of the five swings that were not a shank.
        check(near(theilSenSlope(std::vector<double>{ 1, 2, 3, 4, 5, 100 }), 1.0, 1e-12),
              "one wild point does not move the Theil–Sen slope");
        check(near(theilSenSlope(std::vector<double>{ 1, 2, 3, 4, 5, 6 }), 1.0, 1e-12),
              "…and the un-wild version agrees, so it is not luck");
        // A drift with the shank in the middle, which is the real session shape.
        const std::vector<double> drift{ 4.0, 3.8, 3.6, 9.0, 3.2, 3.0, 2.8, 2.6 };
        check(near(theilSenSlope(drift), -0.2, 1e-12),
              "an interior outlier does not invert an improving trend");
        check(near(theilSenSlope(std::vector<double>{ 7.0 }), 0.0, 1e-12),
              "one point is a slope of zero, not a NaN");
    }

    // ── Kendall τ-b, ties and all ───────────────────────────────────────────────
    {
        const std::vector<double> x{ 1, 2, 3, 4, 5 };
        check(near(kendallTau(x, x), 1.0, 1e-12), "perfect concordance is +1");
        check(near(kendallTau(x, std::vector<double>{ 5, 4, 3, 2, 1 }), -1.0, 1e-12),
              "perfect discordance is −1");
        // Hand case with a tie in y: C−D = 2, n0 = 3, tiesY = 1
        //   τ-b = 2 / sqrt(3 · 2) = 0.816496580927726
        check(near(kendallTau(std::vector<double>{ 1, 2, 3 }, std::vector<double>{ 1, 1, 2 }),
                   0.8164965809, 1e-9),
              "τ-b divides by the un-tied pairs, not by every pair");
        // τ-a on the same data would be 2/3 = 0.667 — the quantised-measure failure the
        // -b correction exists for.
        check(!near(kendallTau(std::vector<double>{ 1, 2, 3 }, std::vector<double>{ 1, 1, 2 }),
                    2.0 / 3.0, 1e-6),
              "…and is therefore NOT τ-a");
        check(near(kendallTau(x, std::vector<double>{ 2, 2, 2, 2, 2 }), 0.0, 1e-12),
              "a fully tied variable has no ordering to detect");
        check(near(kendallTau(std::vector<double>{ 1 }, std::vector<double>{ 1 }), 0.0, 1e-12),
              "one point is zero, not a divide");

        check(kendallTauPValue(1.0, 5) < 0.05, "a perfect 5-point monotone series is significant");
        check(kendallTauPValue(0.2, 5) > 0.05, "a weak 5-point tau is not");
        check(near(kendallTauPValue(0.0, 9), 1.0, 1e-12), "tau of zero is p = 1");
    }

    // ── Fisher's exact, one-sided in the AUTHORED direction ─────────────────────
    //
    // The tail summed is the upper one, P(X ≥ a). That is a choice, and these three
    // tables are what make it visible: the perfectly-associated table is tiny, the
    // perfectly ANTI-associated table is 1.0, and a two-sided test could not tell them
    // apart. The edge says "upstream makes downstream more likely"; the test asks exactly
    // that and nothing else.
    {
        // 8/1/1/4: 45/2002 + 1/2002 = 46/2002
        check(near(fisherExactOneSided(8, 1, 1, 4), 46.0 / 2002.0, 1e-12),
              "8·1·1·4 matches the hand-summed hypergeometric tail");
        // 1/1/1/1: 4/6 + 1/6 = 5/6
        check(near(fisherExactOneSided(1, 1, 1, 1), 5.0 / 6.0, 1e-12),
              "a dead-level table is nowhere near significant");
        // 5/0/0/5: only x = 5 survives → 1/C(10,5)
        check(near(fisherExactOneSided(5, 0, 0, 5), 1.0 / 252.0, 1e-12),
              "perfect co-firing is 1 in 252");
        // 0/5/5/0: the whole distribution is at or above a = 0
        check(near(fisherExactOneSided(0, 5, 5, 0), 1.0, 1e-12),
              "perfect ANTI-association gives p = 1 in the authored tail");
        check(near(fisherExactOneSided(3, 0, 3, 0), 1.0, 1e-12),
              "an empty column is no evidence of coupling");
        check(near(fisherExactOneSided(0, 0, 0, 0), 1.0, 1e-12), "an empty table is p = 1");
        check(fisherExactOneSided(5, 0, 0, 5) < fisherExactOneSided(4, 1, 1, 4),
              "…and the tail is monotone in a");
    }

    // ── Tier gates, at the boundary ─────────────────────────────────────────────
    //
    // Both halves required, neither sufficient. These are stated against the gate
    // function rather than against a session, so the boundary is EXACT rather than
    // approached — a session that happened to land on 0.30 would be a coincidence, and a
    // coincidence is not a test.
    {
        check(conditionTier(3, 1, 0.30, O) == Tier::Pattern,
              "wilson exactly at 0.30 is a pattern (>=, not >)");
        check(conditionTier(3, 1, 0.2999999, O) == Tier::Watching,
              "…and a hair under it is not");
        check(conditionTier(2, 2, 0.9, O) == Tier::Watching,
              "2 assessable shots cannot be a pattern however hard they fired");
        check(conditionTier(3, 3, 0.9, O) == Tier::Pattern, "3 assessable shots can");
        check(conditionTier(3, 0, 0.0, O) == Tier::Clean, "no firings at all is clean");
        check(conditionTier(0, 0, 0.0, O) == Tier::Clean, "and so is a session that never looked");
        check(conditionTier(14, 1, 0.01, O) == Tier::Watching,
              "one shank in fourteen stays watching forever — the outlier discard");

        // The floor moves when it is injected. Rule 4.
        LedgerOptions relaxed = O;
        relaxed.minAssessableForPattern = 2;
        check(conditionTier(2, 2, 0.9, relaxed) == Tier::Pattern, "the floor is injected");
        LedgerOptions strict = O;
        strict.patternWilsonLb = 0.50;
        check(conditionTier(3, 1, 0.30, strict) == Tier::Watching, "and so is the bound");
    }

    // ── Direction agreement, at the 0.70 gate ───────────────────────────────────
    {
        // Ten firings: seven one way, three the other. Exactly 0.70.
        std::vector<Plan> ps{ plan("split", { 1,1,1,1,1, 1,1,1,1,1 }) };
        ps[0].flipAfter = 7;
        const auto sevenOfTen = conditionLedgers(sessionOf(ps), O);
        check(near(led(sevenOfTen, "split").directionAgreement, 0.70, 1e-12),
              "seven of ten firings agree — exactly the gate");
        check(led(sevenOfTen, "split").directionClaimed,
              "at exactly 0.70 the direction is still claimed (suppression is < 0.70)");

        ps[0].flipAfter = 6;
        const auto sixOfTen = conditionLedgers(sessionOf(ps), O);
        check(near(led(sixOfTen, "split").directionAgreement, 0.60, 1e-12), "six of ten is 0.60");
        check(!led(sixOfTen, "split").directionClaimed, "…which suppresses the direction claim");
        check(led(sixOfTen, "split").tier == Tier::Pattern,
              "…while remaining a pattern: inconsistency is a finding, not an absence");

        // An unsigned condition has nothing to disagree about, and must not be reported
        // as dispersion it never showed.
        std::vector<Plan> un{ plan("unsigned", { 1,1,1,1 }) };
        un[0].dir = 0;
        const auto u = conditionLedgers(sessionOf(un), O);
        check(near(led(u, "unsigned").directionAgreement, 1.0, 1e-12),
              "no signed firings is agreement 1.0, not 0.0");
        check(led(u, "unsigned").modalDirection == 0, "…and no modal direction to claim");
    }

    // ── Recency: sinceLastFiring, resolving, freshThisShot, recurrence ──────────
    {
        // Resolving at EXACTLY the window. 7 of 12 clears the bound; the last firing is
        // five assessable shots back.
        const auto five = conditionLedgers(
            sessionOf({ plan("r5", { 1,1,1,1,1,1,1, 0,0,0,0,0 }) }), O);
        check(led(five, "r5").tier == Tier::Pattern, "7 of 12 is a pattern");
        check(led(five, "r5").sinceLastFiring == 5, "five assessable shots since the last firing");
        check(led(five, "r5").resolving, "…which is exactly the resolving window");

        const auto four = conditionLedgers(
            sessionOf({ plan("r4", { 1,1,1,1,1,1,1,1, 0,0,0,0 }) }), O);
        check(led(four, "r4").sinceLastFiring == 4, "four shots since");
        check(!led(four, "r4").resolving, "…which is not");

        // NotAssessable shots do not count towards the recovery. "We did not look" is not
        // evidence the fault has gone (rule 1, in its least obvious guise).
        const auto na = conditionLedgers(
            sessionOf({ plan("rna", { 1,1,1,1,1,1,1, 0,0,-1,-1,-1 }) }), O);
        check(led(na, "rna").sinceLastFiring == 2,
              "occluded shots since a firing do not count as recovery");
        check(!led(na, "rna").resolving, "…so the condition is not called resolving on them");

        const auto never = conditionLedgers(sessionOf({ plan("clean", { 0,0,0,0 }) }), O);
        check(led(never, "clean").sinceLastFiring == -1, "never fired is −1, not 0");
        check(!led(never, "clean").resolving, "…and never resolving");

        // freshThisShot: Pattern now, not Pattern two shots ago. The window is counted in
        // SHOTS, not assessable shots — it is about what the golfer has seen since, and
        // they saw every ball whether the capture did or not.
        const auto fresh = conditionLedgers(sessionOf({ plan("f", { 1,1,1 }) }), O);
        check(led(fresh, "f").tier == Tier::Pattern && led(fresh, "f").freshThisShot,
              "crossing into Pattern on this shot is fresh");
        const auto stale = conditionLedgers(sessionOf({ plan("f", { 1,1,1,1,1 }) }), O);
        check(led(stale, "f").tier == Tier::Pattern && !led(stale, "f").freshThisShot,
              "…and two shots later it is not");
        const auto edge2 = conditionLedgers(sessionOf({ plan("f", { 1,1,1,1 }) }), O);
        check(led(edge2, "f").freshThisShot,
              "the window is inclusive: crossed one shot ago is still fresh");
        LedgerOptions wide = O;
        wide.freshWindow = 4;
        check(conditionLedgers(sessionOf({ plan("f", { 1,1,1,1,1 }) }), wide)[0].freshThisShot,
              "…and the window is injected");

        // The caption.
        const auto rec = conditionLedgers(
            sessionOf({ plan("c", { 0,1,-1,1,1, 1,1,1,1,0, 0,1,-1,0 }) }), O);
        check(led(rec, "c").recurrence == QStringLiteral("8 of 12 measurable shots"),
              "the recurrence caption is the count, verbatim");
        check(led(rec, "c").latest == ShotState::Clean, "the latest state is the last shot's");
    }

    // ── Warm-up weighting (design §5.4) ─────────────────────────────────────────
    //
    // Down-weighted, never excluded: the swings happened and the ticks are drawn. What
    // changes is how hard they seed a PATTERN CLAIM, which is the thing an unrepresentative
    // first three balls would poison.
    {
        const auto shots = sessionOf({ plan("w", { 1,1,1,1,1, 0,0,0 }) });

        const auto flat = conditionLedgers(shots, O);
        check(led(flat, "w").fired == 5 && led(flat, "w").assessable == 8, "5 of 8 either way");
        check(near(led(flat, "w").wilsonLower, 0.3057379, 1e-6), "unweighted bound, hand-computed");
        check(led(flat, "w").tier == Tier::Pattern, "…which clears the gate");

        LedgerOptions warm = O;
        warm.warmUpShots  = 3;
        warm.warmUpWeight = 0.5;
        const auto weighted = conditionLedgers(shots, warm);
        check(near(led(weighted, "w").effFired, 3.5, 1e-12), "three warm-up firings weigh 1.5");
        check(near(led(weighted, "w").effAssessable, 6.5, 1e-12), "…of an effective 6.5 shots");
        check(led(weighted, "w").fired == 5 && led(weighted, "w").assessable == 8,
              "the RAW counts are untouched — the caption still says 5 of 8");
        check(led(weighted, "w").wilsonLower < led(flat, "w").wilsonLower,
              "the weighted bound is lower");
        check(led(weighted, "w").tier == Tier::Watching,
              "…far enough to hold a warm-up-seeded pattern back to watching");
        check(led(weighted, "w").recurrence == QStringLiteral("5 of 8 measurable shots"),
              "and the golfer is still told about all five swings");

        // The declared flag reaches shots the first-N rule does not.
        auto declared = shots;
        declared[6].warmUp = true;
        LedgerOptions none = O;
        none.warmUpShots = 0;
        none.warmUpWeight = 0.5;
        check(near(conditionLedgers(declared, none)[0].effAssessable, 7.5, 1e-12),
              "a declared warm-up shot is down-weighted on its own");
    }

    // ── Trend: ordinal, on |z|, over assessable shots only ──────────────────────
    {
        // Fires throughout, departure shrinking. |z| is the quantity, because the SIGN of
        // z is which side of a two-sided corridor the swing fell and a golfer oscillating
        // between the two is not improving.
        Plan imp = plan("imp", { 1,1,1,1,1,1,1,1 });
        imp.zs = { 4.0, 3.8, 3.6, 3.4, 3.2, 3.0, 2.8, 2.6 };
        const auto a = conditionLedgers(sessionOf({ imp }), O);
        check(led(a, "imp").trendPoints == 8, "eight assessable points");
        check(led(a, "imp").trendKnown && led(a, "imp").trend == Trend::Improving,
              "a shrinking departure is improving");
        check(near(led(a, "imp").trendSlope, -0.2, 1e-12), "…at the planted slope");
        check(near(led(a, "imp").trendTau, -1.0, 1e-12), "…with a perfect tau");

        Plan wor = imp;
        wor.id = QStringLiteral("wor");
        wor.zs = { 2.6, 2.8, 3.0, 3.2, 3.4, 3.6, 3.8, 4.0 };
        const auto b = conditionLedgers(sessionOf({ wor }), O);
        check(led(b, "wor").trendKnown && led(b, "wor").trend == Trend::Worsening,
              "a growing departure is worsening");

        // The shank in the middle of an improving run.
        Plan shank = imp;
        shank.id = QStringLiteral("shank");
        shank.zs = { 4.0, 3.8, 3.6, 9.0, 3.2, 3.0, 2.8, 2.6 };
        const auto c = conditionLedgers(sessionOf({ shank }), O);
        check(near(led(c, "shank").trendSlope, -0.2, 1e-12),
              "one wild swing does not flip the session's arrow");

        // Below the floor there is no verdict, not a weak one.
        Plan few = plan("few", { 1,1,1,1 });
        few.zs = { 4.0, 3.5, 3.0, 2.5 };
        const auto d = conditionLedgers(sessionOf({ few }), O);
        check(led(d, "few").trendPoints == 4 && !led(d, "few").trendKnown,
              "four points is below the trend floor");
        check(led(d, "few").trend == Trend::Stable, "…and reads stable, not improving");

        // Flat is stable, and stable is not "unknown dressed up".
        Plan flat = plan("flat", { 1,1,1,1,1,1 });
        flat.zs = { 3.0, 3.0, 3.0, 3.0, 3.0, 3.0 };
        const auto e = conditionLedgers(sessionOf({ flat }), O);
        check(!led(e, "flat").trendKnown && led(e, "flat").trend == Trend::Stable,
              "a flat series is stable");

        // Occluded shots contribute no point at all (rule 1 again).
        Plan gap = plan("gap", { 1,-1,1,-1,1,1,1,1 });
        gap.zs = { 4.0, 9.9, 3.6, 9.9, 3.2, 3.0, 2.8, 2.6 };
        check(conditionLedgers(sessionOf({ gap }), O)[0].trendPoints == 6,
              "an occluded shot contributes no trend point");
    }

    // ── Stage machine and its one-way ratchet ───────────────────────────────────
    {
        const std::vector<LinkEvidence> noLinks;

        const auto cold = conditionLedgers(sessionOf({ plan("a", { 1,1 }) }), O);
        check(sessionStage(Stage::Cold, cold, noLinks, false, O) == Stage::Cold,
              "two assessable shots is Cold");

        const auto forming = conditionLedgers(
            sessionOf({ plan("a", { 1,1,1 }), plan("b", { 1,1,1 }) }), O);
        check(sessionStage(Stage::Cold, forming, noLinks, false, O) == Stage::Forming,
              "three assessable shots is Forming");

        // Established follows the DESIGN DOCUMENT, not the mock: the gate is the LINK
        // GRADE (>= Coherent), not the mere existence of an authored edge.
        std::vector<LinkEvidence> weak(1);
        weak[0].from = QStringLiteral("a");
        weak[0].to   = QStringLiteral("b");
        weak[0].grade = LinkGrade::PresentTogether;
        check(sessionStage(Stage::Cold, forming, weak, false, O) == Stage::Forming,
              "two patterns joined only at Present together stay Forming");

        std::vector<LinkEvidence> strong = weak;
        strong[0].grade = LinkGrade::Coherent;
        check(sessionStage(Stage::Cold, forming, strong, false, O) == Stage::Established,
              "…and reach Established at Coherent");

        // A link to something that is not a pattern is not two linked patterns.
        const auto oneOnly = conditionLedgers(
            sessionOf({ plan("a", { 1,1,1 }), plan("b", { 0,0,0 }) }), O);
        check(sessionStage(Stage::Cold, oneOnly, strong, false, O) == Stage::Forming,
              "a Coherent link to a clean condition establishes nothing");

        // The ratchet. An occluded shot must not walk the panel back to a card list.
        check(sessionStage(Stage::Established, cold, noLinks, false, O) == Stage::Established,
              "Established never drops back to Cold");
        check(sessionStage(Stage::Established, forming, weak, false, O) == Stage::Established,
              "…nor to Forming");
        check(sessionStage(Stage::Forming, cold, noLinks, false, O) == Stage::Forming,
              "Forming never drops back to Cold");

        // Closing is an event, not a threshold.
        check(sessionStage(Stage::Cold, cold, noLinks, true, O) == Stage::Closing,
              "the caller sets Closing at session end");
        check(sessionStage(Stage::Closing, forming, strong, false, O) == Stage::Closing,
              "…and a closed session does not re-open");
    }

    // ── Link grades ─────────────────────────────────────────────────────────────
    {
        const NodeSpec early = cp::liveNode("up", 2);
        const NodeSpec late  = cp::liveNode("down", 6);
        const EdgeSpec e     = cp::edge("up", "down");

        // Conditionally dependent: 12 pairs, a=6 b=1 c=1 d=4 ⇒ 36/792 = 0.04545 ≤ 0.05.
        {
            const auto ls = conditionLedgers(sessionOf({
                plan("up",   { 1,1,1,1,1,1,1, 0,0,0,0,0 }),
                plan("down", { 1,1,1,1,1,1,0, 0,0,0,0,1 }) }), O);
            const LinkEvidence ev = gradeLink(e, early, late,
                                              ledgerFor(ls, QStringLiteral("up")),
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.pairs == 12, "twelve paired shots");
            check(ev.a == 6 && ev.b == 1 && ev.c == 1 && ev.d == 4, "the 2×2 is as planted");
            check(ev.fisherTested && near(ev.fisherP, 36.0 / 792.0, 1e-12),
                  "Fisher exact, hand-computed");
            check(ev.grade == LinkGrade::ConditionallyDependent, "…and the grade is dependence");
        }

        // Refusal on too few pairs. The table is perfect; seven pairs is still seven.
        {
            const auto ls = conditionLedgers(sessionOf({
                plan("up",   { 1,1,1,1,1,0,0 }),
                plan("down", { 1,1,1,1,1,0,0 }) }), O);
            const LinkEvidence ev = gradeLink(e, early, late,
                                              ledgerFor(ls, QStringLiteral("up")),
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.pairs == 7 && !ev.fisherTested, "seven pairs is below the min-pairs floor");
            check(ev.grade == LinkGrade::Coherent,
                  "…so a perfectly co-fired pair stops at Coherent");

            LedgerOptions lower = O;
            lower.minPairsForDependence = 7;
            check(gradeLink(e, early, late, ledgerFor(ls, QStringLiteral("up")),
                            ledgerFor(ls, QStringLiteral("down")), FocusSplit(), lower).grade
                  == LinkGrade::ConditionallyDependent,
                  "…and the floor is injected");
        }

        // Range restriction: upstream fires on every measurable shot, so there is no
        // variance left to covary with. Capped at Coherent, and flagged.
        {
            const auto ls = conditionLedgers(sessionOf({
                plan("up",   { 1,1,1,1,1,1,1,1,1,1,1,1 }),
                plan("down", { 1,1,1,1,1,1,1, 0,0,0,0,0 }) }), O);
            check(ledgerFor(ls, QStringLiteral("up"))->rangeRestricted,
                  "firing on every measurable shot is range restriction");
            const LinkEvidence ev = gradeLink(e, early, late,
                                              ledgerFor(ls, QStringLiteral("up")),
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.grade == LinkGrade::Coherent && ev.rangeRestricted,
                  "…and it caps the link at Coherent however many pairs there are");
            check(!ev.fisherTested, "the 2×2 is not even attempted");
        }

        // Present together: both present, but the edge runs backwards through the swing.
        {
            const auto ls = conditionLedgers(sessionOf({
                plan("up",   { 1,1,1,1,1,1,1, 0,0,0,0,0 }),
                plan("down", { 1,1,1,1,1,1,0, 0,0,0,0,1 }) }), O);
            const NodeSpec backwards = cp::liveNode("up", 8);
            const LinkEvidence ev = gradeLink(e, backwards, late,
                                              ledgerFor(ls, QStringLiteral("up")),
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.grade == LinkGrade::PresentTogether && !ev.coherent,
                  "an edge running backwards through the swing is not coherent");
        }

        // Present together, the mock's own case: an asserted ghost feeding a live
        // pattern. Neither endpoint was observed firing together, so nothing above
        // Present together is available.
        {
            const auto ls = conditionLedgers(sessionOf({ plan("down", { 1,1,1,1,1,1,1,0,0,0,0,0 }) }), O);
            const NodeSpec ghost = cp::assertedGhost("up", 4);
            const LinkEvidence ev = gradeLink(e, ghost, late, nullptr,
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.grade == LinkGrade::PresentTogether, "an asserted ghost is present, not coherent");
        }

        // Unanchored: a planned-but-unbuilt ghost asserts nothing.
        {
            const auto ls = conditionLedgers(sessionOf({ plan("down", { 1,1,1,1,1,1,1,0,0,0,0,0 }) }), O);
            const NodeSpec planned = cp::plannedGhost("up", 4);
            check(gradeLink(e, planned, late, nullptr, ledgerFor(ls, QStringLiteral("down")),
                            FocusSplit(), O).grade == LinkGrade::Unanchored,
                  "a merely-planned ghost anchors nothing");
        }

        // Moved together: the n-of-1 quasi-experiment, and the only grade that needs a
        // DECLARATION rather than a statistic.
        {
            Plan up   = plan("up",   { 1,1,1,1,1,1,1, 0,0,0,0,0 });
            up.zs     = { 4.0,4.2,3.8,4.1,3.9, 3.2,3.0, 1.0,0.8,0.6,0.9,0.7 };
            Plan down = plan("down", { 1,1,1,1,1,1,1, 0,0,0,0,0 });
            down.zs   = { 3.5,3.7,3.3,3.6,3.4, 2.8,2.6, 0.9,0.7,0.5,0.8,0.6 };
            const auto ls = conditionLedgers(sessionOf({ up, down }), O);
            const ConditionLedger *lu = ledgerFor(ls, QStringLiteral("up"));
            const ConditionLedger *ld = ledgerFor(ls, QStringLiteral("down"));

            FocusSplit focus;
            focus.declared = true;
            focus.baselineEnd = 4;                     // shots 1–5 baseline, 6–12 after
            const LinkEvidence ev = gradeLink(e, early, late, lu, ld, focus, O);
            check(ev.baselineN == 5 && ev.interventionN == 5, "a full 5 + 5 window either side");
            check(ev.movedTogether && ev.grade == LinkGrade::MovedTogether,
                  "both endpoints improved across the declared change");

            // No declaration, no quasi-experiment. The passage of time is not a treatment.
            const LinkEvidence undeclared = gradeLink(e, early, late, lu, ld, FocusSplit(), O);
            check(undeclared.grade == LinkGrade::ConditionallyDependent && !undeclared.movedTogether,
                  "without a focus contract the grade stops at dependence");

            // A declaration too late to have five baseline shots behind it.
            FocusSplit shallow;
            shallow.declared = true;
            shallow.baselineEnd = 2;
            const LinkEvidence thin = gradeLink(e, early, late, lu, ld, shallow, O);
            check(thin.baselineN == 3 && !thin.movedTogether,
                  "a three-shot baseline is not a baseline");

            // The window is injected.
            LedgerOptions narrow = O;
            narrow.movedTogetherWindow = 3;
            check(gradeLink(e, early, late, lu, ld, shallow, narrow).grade == LinkGrade::MovedTogether,
                  "…and the window is injected");
        }

        // Direction incoherence blocks Coherent even with the phases in order.
        {
            std::vector<Plan> ps{ plan("up",   { 1,1,1,1,1,1,1, 0,0,0,0,0 }),
                                  plan("down", { 1,1,1,1,1,1,0, 0,0,0,0,1 }) };
            ps[1].dir = -1;                             // opposite modal sense
            const auto ls = conditionLedgers(sessionOf(ps), O);
            const LinkEvidence ev = gradeLink(e, early, late,
                                              ledgerFor(ls, QStringLiteral("up")),
                                              ledgerFor(ls, QStringLiteral("down")),
                                              FocusSplit(), O);
            check(ev.grade == LinkGrade::PresentTogether,
                  "opposite directions on a same-sense edge are not coherent");

            const EdgeSpec inverse = cp::edge("up", "down", -1);
            check(gradeLink(inverse, early, late, ledgerFor(ls, QStringLiteral("up")),
                            ledgerFor(ls, QStringLiteral("down")), FocusSplit(), O).grade
                  >= LinkGrade::Coherent,
                  "…and are coherent on an edge the pack authored as inverse");
        }
    }

    // ── Chain extraction, over a planted corpus ─────────────────────────────────
    //
    // The mock's own two chains: a fully live spine, and one that is mostly unmeasurable.
    // Chain B is the reason every honesty device in this file exists and it is not
    // simplified away here either.
    {
        cp::CorpusSpec spec;
        spec.shots = 14;
        spec.warmUpShots = 3;
        for (const Plan &p : mockPlans()) {
            cp::PlantedCondition c;
            c.id = p.id;
            c.explicitStates = p.states;
            if (p.flipAfter >= 0) c.modalFirings = p.flipAfter;
            spec.conditions.push_back(std::move(c));
        }
        spec.nodes = {
            cp::liveNode("transition_rush", 2), cp::liveNode("casting", 6),
            cp::liveNode("shaft_lean", 7),      cp::liveNode("scooping", 7),
            cp::liveNode("out_to_in", 6),       cp::liveNode("face_roll", 1),
            cp::liveNode("deceleration", 6),    cp::liveNode("head_sway", 3),
            cp::liveNode("bent_lead_arm", 4),
            cp::screenedRoot("pelvic_disassoc", false, 0),
            cp::plannedGhost("pelvis_slow", 3),
            cp::assertedGhost("over_the_top", 5),
            cp::assertedGhost("hips_stall", 6),
            cp::outcomeNode("slice", "miss.slice", 9)
        };
        spec.edges = {
            cp::edge("transition_rush", "casting", 1, 3),
            cp::edge("casting", "shaft_lean", 1, 3),
            cp::edge("shaft_lean", "scooping", 1, 3),
            cp::edge("pelvic_disassoc", "pelvis_slow", 1, 2),
            cp::edge("pelvis_slow", "over_the_top", 1, 2),
            cp::edge("over_the_top", "out_to_in", 1, 2),
            cp::edge("out_to_in", "slice", 1, 2),
            cp::edge("hips_stall", "scooping", 1, 1)      // the rival parent
        };

        const cp::Corpus c = cp::generate(spec);
        const auto ls = conditionLedgers(c.shots, O);
        const auto links = gradeLinks(c.nodes, c.edges, ls, c.focus, O);
        const ChainRails rails = extractChains(c.nodes, c.edges, ls, links, O);

        auto chainOf = [&](const char *head) -> const Chain * {
            for (const Chain &ch : rails.chains)
                if (!ch.nodes.empty() && ch.nodes.front().id == QString::fromLatin1(head)) return &ch;
            return nullptr;
        };
        auto idsOf = [](const Chain &ch) {
            std::vector<QString> v;
            for (const ChainNode &n : ch.nodes) v.push_back(n.id);
            return v;
        };

        const Chain *A = chainOf("transition_rush");
        check(A != nullptr, "the live spine is extracted");
        if (A) {
            const std::vector<QString> want{ QStringLiteral("transition_rush"),
                                             QStringLiteral("casting"),
                                             QStringLiteral("shaft_lean"),
                                             QStringLiteral("scooping") };
            check(idsOf(*A) == want, "…in the pack's authored order");
            bool allLive = true;
            for (const ChainNode &n : A->nodes)
                if (n.kind != ChainNodeKind::LiveCard) allLive = false;
            check(allLive, "…and every node on it is a live card");
            check(A->nodes[2].hasLink && A->nodes[2].link.rangeRestricted,
                  "shaft_lean fires on every measurable shot, so its outgoing link is capped");
        }

        const Chain *B = chainOf("pelvic_disassoc");
        check(B != nullptr, "the mostly-unmeasurable chain is extracted, not simplified away");
        if (B) {
            const std::vector<QString> want{ QStringLiteral("pelvic_disassoc"),
                                             QStringLiteral("pelvis_slow"),
                                             QStringLiteral("over_the_top"),
                                             QStringLiteral("out_to_in"),
                                             QStringLiteral("slice") };
            check(idsOf(*B) == want, "…with its ghosts in place, never silently bridged");
            check(B->nodes[0].kind == ChainNodeKind::ScreenedRoot, "the root is a screened root");
            check(B->nodes[1].kind == ChainNodeKind::Ghost, "the planned measure is a ghost");
            check(B->nodes[2].kind == ChainNodeKind::Ghost, "the asserted node is a ghost");
            check(B->nodes[3].kind == ChainNodeKind::LiveCard, "the launch-monitor node is live");
            check(B->nodes[4].kind == ChainNodeKind::Outcome, "the declared miss is the outcome");
            check(B->nodes[0].hasLink && B->nodes[0].link.grade == LinkGrade::Unanchored
                  && B->nodes[0].link.screenOutstanding,
                  "…and the unentered screen leaves the chain unanchored");
        }

        // The unchained pattern. face_roll is a pattern the pack authors no edge for; it
        // gets its own line and is never forced onto a rail to tidy the picture.
        check(led(ls, "face_roll").tier == Tier::Pattern, "face_roll reaches pattern tier");
        bool faceUnchained = false;
        for (const QString &id : rails.unchainedPatterns)
            if (id == QStringLiteral("face_roll")) faceUnchained = true;
        check(faceUnchained, "…and is reported on the unchained line");

        // The rival parent, named and explicitly not adjudicated.
        const RivalParent *rp = nullptr;
        for (const RivalParent &r : rails.rivals)
            if (r.childId == QStringLiteral("scooping")) rp = &r;
        check(rp != nullptr, "two authored parents for a fired node are disclosed");
        if (rp) {
            check(rp->chosenParentId == QStringLiteral("shaft_lean"),
                  "…the better-evidenced parent is chosen");
            check(rp->rivalParentId == QStringLiteral("hips_stall"), "…the rival is named");
            check(!rp->adjudicated, "…and an unmeasurable rival is explicitly not adjudicated");
        }

        // The stage this session reaches.
        check(sessionStage(Stage::Cold, ls, links, false, O) == Stage::Established,
              "the mock session reaches Established");

        // Coverage, as the honesty line states it.
        const Coverage cov = sessionCoverage(ls, 140);
        check(cov.measurable == 9 && cov.total == 140,
              "coverage counts conditions with at least one assessable shot");
    }

    // ── Rank-band hysteresis ────────────────────────────────────────────────────
    //
    // The golfer must never watch the app change its mind mid-thought (B8). A one-band
    // swap is not a decision; a two-band jump is.
    {
        const std::vector<QString> prev{ QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };

        // b edges past a. One band — nothing moves.
        const std::vector<RankedCondition> nudge{ { QStringLiteral("a"), 0.50 },
                                                  { QStringLiteral("b"), 0.51 },
                                                  { QStringLiteral("c"), 0.10 } };
        check(hystereticOrder(nudge, prev, 1) == prev, "a one-band swap does not reorder the rail");

        // c jumps two bands. It moves, and takes the band with it.
        const std::vector<RankedCondition> jump{ { QStringLiteral("a"), 0.50 },
                                                 { QStringLiteral("b"), 0.20 },
                                                 { QStringLiteral("c"), 0.99 } };
        const std::vector<QString> moved{ QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("b") };
        check(hystereticOrder(jump, prev, 1) == moved, "a two-band jump does reorder it");

        // Band width zero is "no hysteresis at all", which is the control case.
        check(hystereticOrder(nudge, prev, 0)
              == std::vector<QString>{ QStringLiteral("b"), QStringLiteral("a"), QStringLiteral("c") },
              "band width zero sorts strictly by score");

        // A new condition enters at its true rank rather than at the end.
        const std::vector<RankedCondition> entrant{ { QStringLiteral("a"), 0.50 },
                                                    { QStringLiteral("b"), 0.40 },
                                                    { QStringLiteral("c"), 0.10 },
                                                    { QStringLiteral("d"), 0.99 } };
        check(hystereticOrder(entrant, prev, 1).front() == QStringLiteral("d"),
              "a new top-ranked condition enters at the top");

        // Ties break on id, so two equal scores do not shuffle between frames.
        const std::vector<RankedCondition> tied{ { QStringLiteral("b"), 0.5 },
                                                 { QStringLiteral("a"), 0.5 } };
        check(hystereticOrder(tied, {}, 1)
              == std::vector<QString>{ QStringLiteral("a"), QStringLiteral("b") },
              "equal scores order deterministically");
    }

    // ── Driver-footer debounce ──────────────────────────────────────────────────
    {
        const auto steady = sessionOf({ plan("a", { 1,1,1,1,1,1 }) });
        check(patternSetStableShots(steady, O) == 4, "the pattern set has held for four shots");
        check(driverFooterEligible(steady, O), "…so the driver footer may appear");

        const auto churn = sessionOf({ plan("a", { 1,1,1,1,1,1 }),
                                       plan("b", { -1,-1,-1,1,1,1 }) });
        check(patternSetStableShots(churn, O) == 1, "a pattern crossing on the last shot resets it");
        check(!driverFooterEligible(churn, O), "…and the footer stays away — better absent than flickering");

        LedgerOptions eager = O;
        eager.driverDebounceShots = 1;
        check(driverFooterEligible(churn, eager), "the debounce is injected");
    }

    // ── Session bookends ────────────────────────────────────────────────────────
    {
        Plan p = plan("bk", { 0,1,1,0,1,-1 });
        p.zs   = { 0.5, 3.0, -4.5, 1.0, 2.0, 99.0 };
        const auto shots = sessionOf({ p });
        const Bookends bk = conditionBookends(shots, QStringLiteral("bk"));
        check(bk.has, "the bookends exist");
        check(bk.worstShotId == 3, "worst is max |z| — shot 3");
        check(bk.bestShotId == 1, "best is min |z| — shot 1");
        check(near(bk.medianZ, 1.0, 1e-12), "the session median z");
        check(bk.representativeShotId == 4, "most representative is the shot closest to it");
        check(!conditionBookends(shots, QStringLiteral("nope")).has,
              "a condition with no assessable shots has no bookends");
        // The occluded shot's z is 99 and must not be the worst swing of the session.
        check(bk.worstShotId != 6, "an occluded shot is never a bookend");
    }

    // ── Serialisation round-trip ────────────────────────────────────────────────
    {
        cp::CorpusSpec spec;
        spec.shots = 12;
        spec.seed = 4242;
        cp::PlantedCondition a; a.id = QStringLiteral("a"); a.rate = 0.7;
        a.naRuns = { { 4, 2 } };
        cp::PlantedCondition b; b.id = QStringLiteral("b"); b.coFireParent = QStringLiteral("a");
        b.rate = 0.3;
        spec.conditions = { a, b };
        const cp::Corpus c = cp::generate(spec);

        LedgerOptions custom = O;
        custom.patternWilsonLb = 0.42;
        custom.minPairsForDependence = 6;

        const QJsonObject obj = toJson(c.shots, custom);
        check(obj.value(QStringLiteral("schemaVersion")).toInt() == kDiagSchemaVersion,
              "the document carries its schema version");

        // Through actual bytes, not just through the object — a QJsonDocument that will
        // not survive its own serialiser is not persistence.
        const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        LedgerOptions back;
        const auto rebuilt = fromJsonDocument(QJsonDocument::fromJson(bytes), &back);

        check(rebuilt.size() == c.shots.size(), "every shot survives the round trip");
        check(near(back.patternWilsonLb, 0.42, 1e-12) && back.minPairsForDependence == 6,
              "…and so do the gates it was read under");

        bool rowsMatch = true;
        for (size_t i = 0; i < rebuilt.size() && rowsMatch; ++i) {
            if (rebuilt[i].shotId != c.shots[i].shotId) rowsMatch = false;
            if (rebuilt[i].warmUp != c.shots[i].warmUp) rowsMatch = false;
            if (rebuilt[i].timestampMs != c.shots[i].timestampMs) rowsMatch = false;
            if (rebuilt[i].rows.size() != c.shots[i].rows.size()) { rowsMatch = false; break; }
            for (size_t k = 0; k < rebuilt[i].rows.size(); ++k) {
                const ConditionRow &x = rebuilt[i].rows[k], &y = c.shots[i].rows[k];
                if (x.conditionId != y.conditionId || x.state != y.state
                    || x.direction != y.direction || !near(x.z, y.z, 1e-9)
                    || x.notAssessableReason != y.notAssessableReason) rowsMatch = false;
            }
        }
        check(rowsMatch, "every field of every row survives it too");

        // The reductions are pure, so re-reducing the rebuilt rows must give the identical
        // panel. This is what makes review mode a re-render rather than a cache.
        const auto before = conditionLedgers(c.shots, custom);
        const auto after  = conditionLedgers(rebuilt, custom);
        bool same = before.size() == after.size();
        for (size_t i = 0; i < before.size() && same; ++i)
            same = before[i].id == after[i].id && before[i].tier == after[i].tier
                && before[i].fired == after[i].fired && before[i].assessable == after[i].assessable
                && near(before[i].wilsonLower, after[i].wilsonLower, 1e-12)
                && before[i].recurrence == after[i].recurrence;
        check(same, "…and re-reducing the rebuilt rows gives the identical ledger");

        // A NotAssessable reason is never dropped: the review strip prints it where the
        // corridor would go, and a blank there reads as a bug.
        bool reasonsKept = true;
        for (const ShotRecord &s : rebuilt)
            for (const ConditionRow &r : s.rows)
                if (r.state == ShotState::NotAssessable && r.notAssessableReason.isEmpty())
                    reasonsKept = false;
        check(reasonsKept, "every not-assessable row still carries its reason");

        // An unknown state string reads as "we did not look", never as clean.
        check(shotStateFromString(QStringLiteral("gibberish")) == ShotState::NotAssessable,
              "an unrecognised state degrades to NotAssessable, not to Clean");
    }

    // ── Never-do rule 1 — no percentage the n does not support ──────────────────
    //
    // Recurrence is a COUNT, always. There is no percentage-formatting function in the
    // header to reach for, and the caption is built here rather than in the panel so an
    // export and the screen cannot invent one independently.
    {
        const auto ls = conditionLedgers(sessionOf({ plan("p", { 1,1,-1,1,0,1 }) }), O);
        const ConditionLedger &l = led(ls, "p");
        check(l.recurrence == QStringLiteral("4 of 5 measurable shots"), "recurrence is a count");
        check(!l.recurrence.contains(QLatin1Char('%')), "…and carries no percent sign");
        check(!l.recurrence.contains(QStringLiteral("0.")), "…and no decimal rate either");
        check(recurrenceText(0, 0) == QStringLiteral("0 of 0 measurable shots"),
              "even the empty case is a count, not a division");
        // The bound exists and is used as an ORDINAL GATE. It is deliberately not part of
        // any string this file produces.
        check(l.wilsonLower > 0.0 && !l.recurrence.contains(QStringLiteral("bound")),
              "the Wilson bound gates the tier and never reaches the caption");
    }

    // ── Never-do rule 2 — NotAssessable is never clean ──────────────────────────
    {
        // The same four firings, once against ten clean shots and once against ten
        // occluded ones. If NotAssessable were quietly read as clean these would agree.
        const auto asClean = conditionLedgers(
            sessionOf({ plan("x", { 1,1,1,1, 0,0,0,0,0,0,0,0,0,0 }) }), O);
        const auto asNa = conditionLedgers(
            sessionOf({ plan("x", { 1,1,1,1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1 }) }), O);
        check(led(asClean, "x").assessable == 14 && led(asNa, "x").assessable == 4,
              "occluded shots leave the denominator");
        check(led(asClean, "x").fired == led(asNa, "x").fired, "…without touching the numerator");
        check(led(asNa, "x").wilsonLower > led(asClean, "x").wilsonLower,
              "…so the evidence is stronger, not weaker, for not having looked");
        check(led(asClean, "x").tier == Tier::Watching && led(asNa, "x").tier == Tier::Pattern,
              "…and the two sessions reach different tiers, which is the whole point");

        // A condition nobody could measure at all is Clean by tier and INVISIBLE to
        // coverage — the two facts a caller must not conflate.
        const auto blind = conditionLedgers(sessionOf({ plan("blind", { -1,-1,-1,-1 }) }), O);
        check(led(blind, "blind").assessable == 0 && led(blind, "blind").fired == 0,
              "a never-measured condition counts nothing");
        check(sessionCoverage(blind, 140).measurable == 0,
              "…and is not counted as covered");
        check(led(blind, "blind").run.size() == 4
              && led(blind, "blind").run[0] == ShotState::NotAssessable,
              "…while still holding a tick per shot, so the run is drawn and never gapped");

        // A missing ROW is the same as an explicit NotAssessable row: absence of a
        // reading is absence of a reading however it arrived.
        auto shots = sessionOf({ plan("y", { 1,1,1,1 }) });
        shots[2].rows.clear();
        check(conditionLedgers(shots, O)[0].assessable == 3,
              "a shot with no row for a condition is not assessable for it");

        // And the paired 2×2 inherits the rule.
        const auto pairs = conditionLedgers(sessionOf({
            plan("u", { 1,1,1,1,1,1,1,1,-1,-1 }),
            plan("v", { 1,1,1,1,1,1,1,1, 1, 1 }) }), O);
        const LinkEvidence ev = gradeLink(cp::edge("u", "v"), cp::liveNode("u", 1),
                                          cp::liveNode("v", 2),
                                          ledgerFor(pairs, QStringLiteral("u")),
                                          ledgerFor(pairs, QStringLiteral("v")),
                                          FocusSplit(), O);
        check(ev.pairs == 8, "a pair where either end was occluded is not a pair");
        check(ev.a + ev.b + ev.c + ev.d == ev.pairs, "…and the 2×2 sums to the pairs, exactly");
    }

    // ── Never-do rule 3 — no unauthored edge, and no dropped pattern ────────────
    {
        cp::CorpusSpec spec;
        spec.shots = 12;
        spec.conditions = {
            cp::exact("a", { 1,1,1,1,1,1,1,0,0,0,0,0 }),
            cp::exact("b", { 1,1,1,1,1,1,0,0,0,0,0,1 }),
            cp::exact("lonely", { 1,1,1,1,1,1,1,1,0,0,0,0 })
        };
        spec.nodes = { cp::liveNode("a", 1), cp::liveNode("b", 2), cp::liveNode("lonely", 3) };
        spec.edges = { cp::edge("a", "b") };            // NOTHING authored to `lonely`

        const cp::Corpus c = cp::generate(spec);
        const auto ls = conditionLedgers(c.shots, O);
        const auto links = gradeLinks(c.nodes, c.edges, ls, c.focus, O);
        const ChainRails rails = extractChains(c.nodes, c.edges, ls, links, O);

        // Every emitted link is an authored edge. `lonely` co-fires with both of the
        // others almost perfectly, which is exactly the coupling a miner would report.
        bool allAuthored = true;
        for (const LinkEvidence &l : rails.links) {
            bool found = false;
            for (const EdgeSpec &e : c.edges)
                if (e.from == l.from && e.to == l.to) found = true;
            if (!found) allAuthored = false;
        }
        check(allAuthored && rails.links.size() == 1,
              "only authored edges are ever emitted, however well two conditions covary");

        bool lonelyOnRail = false;
        for (const Chain &ch : rails.chains)
            for (const ChainNode &n : ch.nodes)
                if (n.id == QStringLiteral("lonely")) lonelyOnRail = true;
        check(!lonelyOnRail, "an unauthored pattern is never spliced onto a chain");

        check(led(ls, "lonely").tier == Tier::Pattern, "…it is still a pattern");
        bool reported = false;
        for (const QString &id : rails.unchainedPatterns)
            if (id == QStringLiteral("lonely")) reported = true;
        check(reported, "…and it is still reported, on its own line");

        // An edge naming a node the caller did not describe is skipped, not invented.
        auto strays = c.edges;
        strays.push_back(cp::edge("a", "nobody_described_this"));
        check(gradeLinks(c.nodes, strays, ls, c.focus, O).size() == 1,
              "an edge to an undescribed node is dropped, never guessed at");
    }

    // ── Never-do rule 4 — a screened root is not a finding until it is entered ──
    {
        const auto ls = conditionLedgers(sessionOf({
            plan("child", { 1,1,1,1,1,1,1,1,1,1,1,1 }) }), O);
        const ConditionLedger *child = ledgerFor(ls, QStringLiteral("child"));

        // Every configuration that could conceivably promote it: perfect co-firing, a
        // declared focus, a low min-pairs floor, an inverse edge, any phase order.
        FocusSplit focus;
        focus.declared = true;
        focus.baselineEnd = 5;
        LedgerOptions permissive = O;
        permissive.minPairsForDependence = 1;
        permissive.movedTogetherWindow   = 1;
        permissive.fisherAlpha           = 1.0;

        bool everPromoted = false;
        for (int type : { -1, 0, 1 }) {
            for (int phase : { 0, 5, 9 }) {
                const NodeSpec root = cp::screenedRoot("root", false, phase);
                const LinkEvidence ev = gradeLink(cp::edge("root", "child", type),
                                                  root, cp::liveNode("child", 6),
                                                  nullptr, child, focus, permissive);
                if (ev.grade != LinkGrade::Unanchored) everPromoted = true;
                if (!ev.screenOutstanding) everPromoted = true;
            }
        }
        check(!everPromoted, "an unentered screened root never exceeds Unanchored");

        // The screen is what promotes it, and doing it once is enough.
        const LinkEvidence entered = gradeLink(cp::edge("root", "child"),
                                               cp::screenedRoot("root", true, 0),
                                               cp::liveNode("child", 6), nullptr, child, focus, O);
        check(entered.grade == LinkGrade::PresentTogether && !entered.screenOutstanding,
              "entering the screen anchors it");
        check(!nodePresent(cp::screenedRoot("root", false), nullptr),
              "…and an unentered root is not 'present' at all");

        // The chain still draws it, as a root with its CTA — never omitted, never a finding.
        const std::vector<NodeSpec> nodes{ cp::screenedRoot("root", false, 0), cp::liveNode("child", 6) };
        const std::vector<EdgeSpec> edges{ cp::edge("root", "child") };
        const auto links = gradeLinks(nodes, edges, ls, FocusSplit(), O);
        const ChainRails rails = extractChains(nodes, edges, ls, links, O);
        check(rails.chains.size() == 1 && rails.chains[0].nodes[0].kind == ChainNodeKind::ScreenedRoot,
              "the root is drawn as a screened root, not omitted");
    }

    // ── Never-do rule 5 — the fault profile cannot reach a tier ─────────────────
    //
    // STRUCTURAL, and stated as such. conditionLedgers() takes shots and gates; there is
    // no third parameter through which prior sessions' prevalence could arrive, and there
    // is no field on LedgerOptions for one. The profile biases RANKING, which lives in
    // hystereticOrder() and reaches nothing here. What can be asserted at runtime is the
    // consequence: identical rows produce identical tiers, and every ranking bias the
    // caller can express leaves them untouched.
    {
        const auto shots = sessionOf({ plan("a", { 1,1,1,1,1,0,0,0 }),
                                       plan("b", { 0,1,0,0,1,0,0,0 }) });
        const auto once  = conditionLedgers(shots, O);
        const auto twice = conditionLedgers(shots, O);
        bool identical = once.size() == twice.size();
        for (size_t i = 0; i < once.size() && identical; ++i)
            identical = once[i].tier == twice[i].tier
                     && near(once[i].wilsonLower, twice[i].wilsonLower, 0.0)
                     && once[i].recurrence == twice[i].recurrence;
        check(identical, "identical rows produce bit-identical tiers");

        // Rank `b` far above `a` — a warm-started panel's strongest possible prior — and
        // then re-reduce. Nothing about the evidence has moved.
        const std::vector<RankedCondition> biased{ { QStringLiteral("b"), 99.0 },
                                                   { QStringLiteral("a"), 0.01 } };
        // Band width 0 so the hysteresis does not mask the point: the ranking is free to
        // move, and it is the ONLY thing that moves.
        const auto order = hystereticOrder(biased, { QStringLiteral("a"), QStringLiteral("b") }, 0);
        check(order.front() == QStringLiteral("b"), "a prior CAN reorder the presentation");
        const auto after = conditionLedgers(shots, O);
        check(after[0].tier == Tier::Pattern && after[1].tier == Tier::Watching,
              "…and cannot touch the tiers, the counts or the corridors");
        check(led(after, "b").fired == 2, "a down-ranked condition still counts every firing");
    }

    // ── Never-do rule 6 — cadence cannot gate the engine ───────────────────────
    //
    // BY CONSTRUCTION: there is no cadence parameter in this header. Not on
    // LedgerOptions, not on conditionLedgers(), not on gradeLink(). A quiet panel and a
    // loud one call the same function with the same arguments, so "the ledger keeps
    // accumulating at full rate" is not a rule the code has to remember — it is the only
    // thing the code can do. What follows is the observable consequence.
    {
        const auto shots = sessionOf({ plan("a", { 1,1,1,1,1,1,1,0,0,0,0,0 }) });

        // Every prefix accumulates, including the ones a Bandwidth panel would have said
        // nothing about (the shots where no pattern fired).
        std::vector<int> firedByShot;
        for (int n = 1; n <= int(shots.size()); ++n) {
            const std::vector<ShotRecord> prefix(shots.begin(), shots.begin() + n);
            firedByShot.push_back(conditionLedgers(prefix, O)[0].fired);
        }
        const std::vector<int> want{ 1,2,3,4,5,6,7,7,7,7,7,7 };
        check(firedByShot == want, "the ledger accumulates on every shot, quiet or not");

        // The quiet tail — five shots on which nothing fired — still moves the ledger.
        const auto atSeven = conditionLedgers(
            std::vector<ShotRecord>(shots.begin(), shots.begin() + 7), O);
        const auto atTwelve = conditionLedgers(shots, O);
        check(atSeven[0].assessable == 7 && atTwelve[0].assessable == 12,
              "…including the shots a bandwidth cadence would surface nothing for");
        check(atTwelve[0].sinceLastFiring == 5 && atTwelve[0].resolving,
              "…which is how a fault becomes 'resolving' during the quiet");
        check(atTwelve[0].wilsonLower < atSeven[0].wilsonLower,
              "…and how a rate falls when nothing is being said");
    }

    // ── Parity with the design mock ────────────────────────────────────────────
    //
    // The nine fires arrays out of `Session Dashboard.dc.html`, at N = 14, with warm-up
    // weighting DISABLED because the mock has none (shotWeight()'s upstream note). If this
    // block fails, our arithmetic and the published design have parted company and one of
    // them needs regenerating — the brief asks to be told which.
    {
        const auto ls = conditionLedgers(sessionOf(mockPlans()), O);

        check(led(ls, "transition_rush").tier == Tier::Pattern, "mock: rushed transition is a pattern");
        check(led(ls, "casting").tier         == Tier::Pattern, "mock: casting is a pattern");
        check(led(ls, "shaft_lean").tier      == Tier::Pattern, "mock: not enough shaft lean is a pattern");
        check(led(ls, "scooping").tier        == Tier::Pattern, "mock: scooping is a pattern");
        check(led(ls, "out_to_in").tier       == Tier::Pattern, "mock: path out-to-in is a pattern");
        check(led(ls, "face_roll").tier       == Tier::Pattern, "mock: face rolled open is a pattern");

        check(led(ls, "deceleration").tier  == Tier::Watching, "mock: deceleration is watching");
        check(led(ls, "head_sway").tier     == Tier::Watching, "mock: head sway is watching");
        check(led(ls, "bent_lead_arm").tier == Tier::Watching, "mock: bent lead arm is watching");

        int patterns = 0;
        for (const ConditionLedger &l : ls) if (l.tier == Tier::Pattern) ++patterns;
        check(patterns == 6, "mock: six patterns over fourteen shots");

        check(led(ls, "transition_rush").recurrence == QStringLiteral("9 of 14 measurable shots"),
              "mock recurrence: rushed transition 9 of 14");
        check(led(ls, "casting").recurrence == QStringLiteral("8 of 12 measurable shots"),
              "mock recurrence: casting 8 of 12");
        check(led(ls, "shaft_lean").recurrence == QStringLiteral("12 of 12 measurable shots"),
              "mock recurrence: shaft lean 12 of 12");
        check(led(ls, "scooping").recurrence == QStringLiteral("9 of 14 measurable shots"),
              "mock recurrence: scooping 9 of 14");
        check(led(ls, "out_to_in").recurrence == QStringLiteral("9 of 12 measurable shots"),
              "mock recurrence: path out-to-in 9 of 12");
        check(led(ls, "face_roll").recurrence == QStringLiteral("9 of 14 measurable shots"),
              "mock recurrence: face roll 9 of 14");
        check(led(ls, "deceleration").recurrence == QStringLiteral("2 of 14 measurable shots"),
              "mock recurrence: deceleration 2 of 14");
        check(led(ls, "head_sway").recurrence == QStringLiteral("3 of 14 measurable shots"),
              "mock recurrence: head sway 3 of 14");
        check(led(ls, "bent_lead_arm").recurrence == QStringLiteral("1 of 12 measurable shots"),
              "mock recurrence: bent lead arm 1 of 12");

        // The mock's own dispersion case, and the reason the gate exists.
        check(near(led(ls, "face_roll").directionAgreement, 5.0 / 9.0, 1e-9),
              "mock: face roll agrees 5 of 9 — the published 56%");
        check(!led(ls, "face_roll").directionClaimed,
              "…below the 70% gate, so it is dispersion and not a direction");
        check(led(ls, "transition_rush").directionClaimed, "…while the others keep their direction");

        // Shaft lean fires on every measurable swing — "no variance to covary with".
        check(led(ls, "shaft_lean").rangeRestricted, "mock: shaft lean is range restricted");
        check(!led(ls, "casting").rangeRestricted, "…and casting is not");

        // Recency, as the mock's card prints it.
        check(led(ls, "transition_rush").sinceLastFiring == 5 && led(ls, "transition_rush").resolving,
              "mock: rushed transition is resolving, none in the last 5");
        check(led(ls, "shaft_lean").sinceLastFiring == 0, "mock: shaft lean fired on the latest shot");

        // The bound, hand-checked against the mock's own `_sdWilson`.
        check(near(led(ls, "casting").wilsonLower, 0.3906176, 1e-6),
              "mock: casting's lower bound matches _sdWilson");
        check(near(led(ls, "deceleration").wilsonLower, 0.0400931, 1e-6),
              "mock: deceleration's does too, well below the gate");
    }

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
