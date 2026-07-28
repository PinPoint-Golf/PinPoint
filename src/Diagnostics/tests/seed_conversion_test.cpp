// The seed conversion, gated on the SHIPPED content rather than on a fixture.
//
// m_smashFactor is the first and only one-sided measure PinPoint ships. Everything the shapes work
// built is machinery; this is the one place that machinery meets real content, and the assertions
// here are about what a golfer's number now grades as.
//
// ⚠ The measure is `externalDevice` — it needs a launch monitor — so none of this is verifiable on
// a live swing today. That is why the behavioural delta is the gate: it is the only thing that can
// say the conversion did what it was for.
//
// This file deliberately does NOT pin mu or sigma. Those are heuristics and are expected to move
// when a corpus re-seats them; pinning them would fail the first time somebody did the right thing.
// It pins the SHAPE of the answer: which band a value lands in, and which side of the corridor is
// open. The one number it does pin is a plausibility cap, because a cap is a physical claim rather
// than a fitted one — it moves only if the physics was wrong.
//
//   cmake --build build/analyzer-tests --target seed_conversion_test
//   ctest --test-dir build/analyzer-tests -R seed_conversion --output-on-failure

#include "../characteristic.h"
#include "../norm.h"
#include "../norm_provider.h"
#include "../pack_provider.h"

#include <QCoreApplication>

#include <cstdio>
#include <memory>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::printf("seed_conversion_test\n");

    const std::unique_ptr<ICharacteristicPackProvider> packProv = makeCharacteristicPackProvider();
    const std::shared_ptr<const INormProvider>         norms    = sharedNormProvider();
    const CharacteristicPack                          &pack     = packProv->pack();

    const Measure *m = pack.measure(QStringLiteral("m_smashFactor"));
    check(m != nullptr, "the shipped pack carries m_smashFactor");
    if (!m) { std::printf("FAILURES\n"); return 1; }

    // ── The measure ─────────────────────────────────────────────────────────
    std::printf("\nthe measure\n");
    check(m->shape == Shape::Floor, "m_smashFactor ships as a FLOOR");
    check(!m->highMeans.isEmpty(),
          "…and says what a high value means, which is what the shape line reads out");

    // The one-sided family is ten measures: m_smashFactor, plus the nine converted by the
    // unwatched-tails pass. Not a number pinned for its own sake — the reason for each conversion
    // is recorded in the norms.json header, and an ELEVENTH shape appearing without that
    // conversation is the thing this catches. The count is updated deliberately when a measure is
    // converted, never loosened to a `>=`: the claim is that these are the shapes somebody argued
    // for, not that there are some.
    int oneSided = 0;
    for (const Measure &mm : pack.measures)
        if (shapeIsOneSided(mm.shape)) ++oneSided;
    check(oneSided == 10, "…and one of exactly ten one-sided measures in the shipped pack");

    // ── The rows ────────────────────────────────────────────────────────────
    //
    // Every context, because a cap is per-context by design — smash is bounded by LOFT, so a driver,
    // an iron and a wedge cap differently while all three are the same one-sided quantity.
    std::printf("\nthe rows\n");
    const char *contexts[] = { "any", "driver", "iron", "wedge" };
    for (const char *cid : contexts) {
        const Norm *n = norms->norms().find(QStringLiteral("m_smashFactor"),
                                            QString::fromLatin1(cid));
        const QString what = QStringLiteral("%1: ").arg(QLatin1String(cid));

        if (!n) { check(false, qPrintable(what + QStringLiteral("has a row"))); continue; }
        check(true, qPrintable(what + QStringLiteral("has a row")));
        check(n->plausibleHi.has_value(),
              qPrintable(what + QStringLiteral("caps ABOVE — the open tail is open up to "
                                               "plausibility, never to infinity")));
        check(!n->plausibleLo.has_value(),
              qPrintable(what + QStringLiteral("and says nothing below, because a bound may "
                                               "appear singly")));
        // sigmaHi must equal sigmaLo on a one-sided row — validateNormsAgainst refuses otherwise,
        // and the ungraded side's tolerance is a number nothing reads.
        check(qFuzzyCompare(1.0 + n->sigmaLo, 1.0 + n->sigmaHi),
              qPrintable(what + QStringLiteral("holds its two tolerances equal")));
        check(!n->monitorHi.has_value(),
              qPrintable(what + QStringLiteral("carries no monitor bound on the open tail")));
        check(!n->citation.isEmpty(),
              qPrintable(what + QStringLiteral("explains its cap — the physics, in the citation")));
    }

    // Loft orders the caps: more loft sends more of the strike into spin and launch and less into
    // ball speed. A RELATIONSHIP, not four pinned numbers, so re-seating cannot break it without
    // breaking the physics it rests on.
    {
        const auto cap = [&norms](const char *cid) {
            const Norm *n = norms->norms().find(QStringLiteral("m_smashFactor"),
                                                QString::fromLatin1(cid));
            return (n && n->plausibleHi.has_value()) ? *n->plausibleHi : 0.0;
        };
        check(cap("driver") > cap("iron") && cap("iron") > cap("wedge"),
              "the caps fall with loft: driver > iron > wedge");
        check(qFuzzyCompare(cap("any"), cap("driver")),
              "…and the unknown-club row caps at the across-club ceiling, so a shot whose club we "
              "do not know still rejects an impossible reading");
    }

    // ── The behavioural delta — THE GATE ────────────────────────────────────
    //
    // What actually changes for a golfer, on the driver row. Before the conversion this measure was
    // a symmetric corridor, because readNorm mirrors sigmaLo into sigmaHi: a golfer approaching
    // perfect energy transfer was graded AWAY from centre for being too efficient.
    std::printf("\nwhat changed for a golfer\n");
    {
        const Norm *n = norms->norms().find(QStringLiteral("m_smashFactor"),
                                            QStringLiteral("driver"));
        check(n != nullptr, "the driver row resolves");
        if (n) {
            const GradePolicy std;                      // the shipped default

            // 1.55 — comfortably above mu. As a target norm this graded Good, i.e. an amber chip
            // for an excellent strike. There is no upper fault in smash factor.
            check(grade(1.55, *n, std, Shape::Floor) == Grade::Ideal,
                  "a driver smash of 1.55 grades IDEAL — the strike is efficient, and efficiency "
                  "has no upper fault");
            check(grade(1.55, *n, std, Shape::Target) == Grade::Good,
                  "…where the same number under the OLD two-sided reading was Good, which is the "
                  "defect this conversion exists to remove");

            // 1.62 — not a swing finding at all. Grading it in EITHER direction would launder a
            // capture fault into a confident diagnosis.
            check(grade(1.62, *n, std, Shape::Floor) == Grade::NotMeasured,
                  "1.62 is not graded: it is a mis-tracked ball, not a swing");
            check(n->isImplausible(1.62),
                  "…and it says so through the flag, so a surface can distinguish 'not believed' "
                  "from 'not measured'");
            check(!isDeviation(grade(1.62, *n, std, Shape::Floor)),
                  "…and it is emphatically not a finding");

            // 1.30 — a genuinely poor strike, and unchanged. The graded tail is an ordinary
            // corridor and the conversion must not have loosened it.
            check(grade(1.30, *n, std, Shape::Floor) == Grade::Action,
                  "1.30 is still Action — the graded tail is untouched");
            check(grade(1.30, *n, std, Shape::Target) == Grade::Action,
                  "…exactly as it was before");

            // The far end of the open tail, up to the cap: all one answer. A future 0-100 score
            // built on z must not climb past the aspiration and invent a target nobody set.
            check(grade(1.50, *n, std, Shape::Floor) == Grade::Ideal
                      && grade(1.56, *n, std, Shape::Floor) == Grade::Ideal,
                  "everything from the aspiration up to the cap grades the same: Ideal");
            check(normZ(1.56, *n, Shape::Floor) == 0.0,
                  "…and z is 0 across all of it, so nothing rewards overshooting a floor");

            const NormBandEdges e = bandEdgesOf(*n, std, -1.0, Shape::Floor);
            check(e.highOpen && !e.lowOpen, "the corridor is open ABOVE and graded below");
            check(qFuzzyCompare(1.0 + e.idealHi, 1.0 + n->mu),
                  "…and the open side's numeric edge is mu, the aspiration, never a sentinel");
        }
    }

    // ── The pack still validates ────────────────────────────────────────────
    //
    // The referential validator is where every shape rule lives, and shipped content going in
    // clean is the difference between a rule that runs and a rule that was only ever tested.
    std::printf("\nthe shipped set still loads clean\n");
    {
        const ValidationReport rep = validateNormsAgainst(norms->norms(), pack, norms->contexts());
        int errors = 0;
        for (const ValidationIssue &i : rep.issues)
            if (i.severity == IssueSeverity::Error) {
                ++errors;
                std::printf("      %s: %s\n", qPrintable(i.code), qPrintable(i.message));
            }
        check(errors == 0, "the shipped norm set validates against the shipped pack, shapes and all");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
