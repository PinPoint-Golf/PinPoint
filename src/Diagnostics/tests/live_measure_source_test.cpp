// THE FIRST TIME THE PACK MEETS A REAL SWING.
//
// Everything upstream of this file is dark by design. characteristic_engine_test feeds synthetic
// values; norm_measure_source_test feeds a fake norm set; measure_sample_test reduces a
// hand-written phase grid. Each proves one link. NONE of them can fail if the shipped content is
// wrong about what a recorded swing carries, and that is precisely the failure this stage exists to
// find: 109 measures declare themselves `live`, and until something read a swing off disk nobody
// could say how many of them a capture actually answers.
//
// So this test is a KNOWN-GROUPS validation, not a unit test. The fixtures are real swings copied
// out of the corpus, the values asserted against them were computed independently from the
// JSON, and the coverage numbers are OBSERVED and pinned — they are a measurement of the pipeline,
// and the reason to pin them is that a silent drop from 30 assessable conditions to 3 looks exactly
// like a healthy run.
//
// ── The fixtures, and what each is for ─────────────────────────────────────────────────────────
//
//   rich_7iron      2026-07-10 Wrist_02 / swing_0001 — 39 metrics, 5 segmented phases, club 7 IRON.
//                   The best capture in the corpus, and therefore the ceiling on what today's
//                   producers can answer.
//   lm_7iron        2026-08-04 Wrist_05 / swing_0025 — 25 lm.* metrics from a GCQuad, ONE phase
//                   (impact), no body metrics at all. The launch-monitor half of the vocabulary.
//   sparse_noclub   2026-07-08 Wrist_01 / swing_0002 — one metric, four phases, and no review block
//                   so no declared club. The degradation case.
//   lm_partial      2026-08-04 Wrist_05 / swing_0004 — the SAME monitor on the same session
//                   reporting 13 of its 25 fields, which is what makes "your monitor did not report
//                   this" a real state rather than a branch invented to fill out an enum. 8 KB, so
//                   it is cheaper to carry than to argue about.
//
// rich_7iron is the ONE fixture that is not byte-for-byte its original: `analysis.pose2d` was
// dropped, taking it from 33 MB to 1.5 MB. Nothing in this chain reads pose2d — buildPhaseGrid()
// consumes `analysis.metrics` and `analysis.phases` and readPhaseGrid() the identity fields — and a
// 33 MB blob of per-frame landmarks in the source tree is a cost every clone pays forever. Every
// metric, every phase, every sample is verbatim.
//
// The fixtures are COPIED TO A TEMPORARY DIRECTORY before use, because readPhaseGrid() writes a
// swing_phasegrid.json sidecar beside the document it parses and a test must not leave artefacts in
// the source tree. It also means the sidecar write/reload path is exercised on real data.
//
//   cmake --build build/analysis-tests --target live_measure_source_test
//   ctest --test-dir build/analysis-tests -R live_measure_source --output-on-failure

#include "../live_measure_source.h"
#include "../norm_provider.h"
#include "../pack_provider.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <memory>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

// A value spot-check, printed with both numbers so a failure says what moved rather than that
// something did.
static void checkValue(const LiveMeasureSource &src, const char *measureId, double want,
                       double tol = 1e-6)
{
    const std::optional<IMeasureValueSource::Value> v = src.value(QLatin1String(measureId));
    if (!v) {
        std::printf("  [FAIL] %s did not read (%s)\n", measureId,
                    qPrintable(src.missingReason(QLatin1String(measureId))));
        ++g_fail;
        return;
    }
    const bool ok = near(v->value, want, tol);
    std::printf("  [%s] %-24s = %.9g (expected %.9g)\n", ok ? "PASS" : "FAIL", measureId,
                v->value, want);
    if (!ok) ++g_fail;
}

static void checkMissing(const LiveMeasureSource &src, const char *measureId, MissingKind want)
{
    const std::optional<IMeasureValueSource::Value> v = src.value(QLatin1String(measureId));
    const MissingKind got = src.missingKind(QLatin1String(measureId));
    const bool ok = !v.has_value() && got == want;
    std::printf("  [%s] %-24s absent: \"%s\"\n", ok ? "PASS" : "FAIL", measureId,
                qPrintable(src.missingReason(QLatin1String(measureId))));
    if (!ok) ++g_fail;
}

// ── Fixtures ────────────────────────────────────────────────────────────────

#ifndef PP_LIVE_SWINGS_DIR
#  define PP_LIVE_SWINGS_DIR "."
#endif

// Copy <source>/<name>/swing.json to <tmp>/live_swings/<name>/swing.json.
//
// The `live_swings` level is kept because readPhaseGrid() derives the session id from the swing
// directory's PARENT — that is the swing library's shape, and flattening it here would make every
// fixture report a session id this test invented.
static QString stageFixture(const QTemporaryDir &tmp, const char *name)
{
    const QString dst = QDir(tmp.path()).filePath(QLatin1String("live_swings/") + QLatin1String(name));
    if (!QDir().mkpath(dst))
        return QString();
    const QString src = QDir(QLatin1String(PP_LIVE_SWINGS_DIR))
                            .filePath(QLatin1String(name) + QLatin1String("/swing.json"));
    if (!QFile::copy(src, QDir(dst).filePath(QStringLiteral("swing.json"))))
        return QString();
    return dst;
}

// ── The full-stack pass, per fixture ────────────────────────────────────────

struct Coverage {
    int findings    = 0;   // conditions detect() asked at all
    int assessable  = 0;   // Fired or NotFired
    int fired       = 0;
    int unavailable = 0;
    int measures    = 0;   // live measures that produced a value
};

static Coverage runFixture(const QString &dir, const CharacteristicPack &pack,
                           const std::shared_ptr<const INormProvider> &norms, const char *label)
{
    std::printf("\n── %s ──────────────────────────────────────\n", label);

    const LiveMeasureSource src(dir, pack);
    const LiveDetection     d = detectForSwing(src, pack, norms);

    Coverage cov;
    cov.findings = int(d.result.findings.size());

    // (a) NO FABRICATED VALUE. Every finding is either an assessment carrying the reading that
    // produced it, or Unavailable carrying nothing at all. A NotFired finding with no evidence
    // would be the module's founding mistake — a clean bill of health nobody measured.
    bool shapeOk = true;
    for (const Finding &f : d.result.findings) {
        switch (f.state) {
        case FindingState::Fired:    ++cov.fired; [[fallthrough]];
        case FindingState::NotFired: ++cov.assessable;
            if (!f.evidence.hasEvidence) {
                shapeOk = false;
                std::printf("      %s assessed with no evidence\n", qPrintable(f.conditionId));
            }
            break;
        case FindingState::Unavailable: ++cov.unavailable;
            if (f.evidence.hasEvidence) {
                shapeOk = false;
                std::printf("      %s unavailable but carries evidence\n", qPrintable(f.conditionId));
            }
            break;
        }
    }

    for (const Measure &m : pack.measures)
        if (m.status == MeasureStatus::Live && src.value(m.id).has_value())
            ++cov.measures;

    std::printf("  club %s -> context %s (session %s)\n", qPrintable(d.club),
                qPrintable(d.contextId), qPrintable(src.sessionId()));
    std::printf("  COVERAGE: %d of 152 conditions assessable on %s"
                "  (%d evaluated, %d fired, %d unavailable)\n",
                cov.assessable, label, cov.findings, cov.fired, cov.unavailable);
    std::printf("  MEASURES: %d of 109 live measures resolved\n", cov.measures);

    check(shapeOk, "every finding is assessed WITH evidence or unavailable WITHOUT it");
    check(cov.findings == 125,
          "detect() asked all 125 non-latent, non-withdrawn conditions (the shipped pack binds none)");

    // (d) Where a norm resolved, the corridor on the evidence is a real band and z is finite.
    bool corridorsOk = true;
    for (const Finding &f : d.result.findings) {
        if (!f.evidence.hasEvidence || !f.evidence.hasCorridor)
            continue;
        const MeasureEvidence &e = f.evidence;
        // Open on exactly one side is a one-sided measure and legal; open on both is not a band.
        const bool sane = !(e.lowOpen && e.highOpen) && e.corridorLo <= e.corridorHi
                          && std::isfinite(e.z) && std::isfinite(e.value);
        if (!sane) {
            corridorsOk = false;
            std::printf("      %s: [%g,%g] z=%g\n", qPrintable(f.conditionId), e.corridorLo,
                        e.corridorHi, e.z);
        }
    }
    check(corridorsOk, "every graded finding carries a sane corridor and a finite z");

    return cov;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    std::printf("live_measure_source_test\n");

    // THE SHIPPED CONTENT, and deliberately NOT makeCharacteristicPackProvider() /
    // sharedNormProvider(). Those assemble the developer's own packs and norm sets from
    // AppDataLocation on top of core, which would make the pinned coverage numbers below depend on
    // whose machine the suite ran on. The resource providers read core and nothing else.
    const std::unique_ptr<ICharacteristicPackProvider> packProv = makeResourcePackProvider();
    const CharacteristicPack                          &pack     = packProv->pack();
    const std::shared_ptr<const INormProvider> norms{ makeResourceNormProvider().release() };

    std::printf("\ncontent\n");
    check(pack.conditions.size() == 152, "the shipped pack carries 152 conditions");
    check(pack.measures.size() == 130, "…and 130 measures");
    check(!norms->norms().norms.empty(), "the shipped norm set loaded");

    QTemporaryDir tmp;
    check(tmp.isValid(), "a temporary directory for the fixture copies");
    if (!tmp.isValid()) { std::printf("\nFAILURES\n"); return 1; }

    const QString rich    = stageFixture(tmp, "rich_7iron");
    const QString lm      = stageFixture(tmp, "lm_7iron");
    const QString sparse  = stageFixture(tmp, "sparse_noclub");
    const QString lmPart  = stageFixture(tmp, "lm_partial");
    check(!rich.isEmpty() && !lm.isEmpty() && !sparse.isEmpty() && !lmPart.isEmpty(),
          "the fixture swings staged out of the source tree");
    if (rich.isEmpty() || lm.isEmpty() || sparse.isEmpty() || lmPart.isEmpty()) {
        std::printf("\nFAILURES\n");
        return 1;
    }

    // ── Known values, computed independently from the JSON ──────────────────
    //
    // Each figure below was derived by hand from the fixture's own `metrics[]` and `phases[]`,
    // applying the conventions measure_sample.h states: a ±15 ms windowed median about each phase
    // instant, falling back to the metric's own phaseSamples entry where the curve is empty. They
    // are the arithmetic the code is SUPPOSED to do, not a recording of what it did.
    std::printf("\nrich_7iron: known values\n");
    {
        const LiveMeasureSource src(rich, pack);

        check(src.club() == QLatin1String("7 IRON"), "the club is read off the swing doc");
        check(src.contextId() == QLatin1String("iron_7"), "…and resolves to the iron_7 context node");
        check(!src.hasLaunchMonitor(), "no launch monitor on this capture");
        check(src.grid().metrics.size() == 39, "39 metrics reached the phase grid");

        // At P1, straight off the metric's phaseSamples entry — ballPosition ships with an EMPTY
        // curve, so this is the fallback path measure_sample.h argues for.
        checkValue(src, "m_ballPosition", 45.485331989515046);
        checkValue(src, "m_stanceWidth",  84.31632042149685);

        // At P7, from the windowed median of a 745-sample curve.
        checkValue(src, "m_impactShaftLean",     41.00000000000002);
        checkValue(src, "m_clubheadSpeedImpact", 73.8041484205068);

        // Summary metric, empty curve, labelled at impact.
        checkValue(src, "m_tempoRatio", 3.0696358098669494);

        // Delta P1 -> P4.
        checkValue(src, "m_headSwayBack", -2.6973052058455864);

        // THE ONE THAT PROVES THE SPANS ARE REAL. An anchored Extremum over P1..P4: the pelvis
        // reaches -28.60 somewhere between the top and address, while the P4 ENDPOINT reads only
        // -24.08. A reduction over phase values alone would miss 4.5% of stance width, and the
        // signed-deviation contract makes the answer (peak - anchor) rather than |peak - anchor|.
        checkValue(src, "m_pelvisSwayBack", -28.601046538397142);
        checkValue(src, "m_pelvisSwayImpact", 17.063654641156035);
        checkValue(src, "m_pelvisSwayFinish", 20.78817002111798);
    }

    std::printf("\nlm_7iron: known values\n");
    {
        const LiveMeasureSource src(lm, pack);
        check(src.hasLaunchMonitor(), "the lm.* metrics are seen as launch-monitor data");
        check(src.contextId() == QLatin1String("iron_7"), "club 7 IRON -> iron_7");

        // Straight passthroughs of the GCQuad's own readings, labelled at impact.
        checkValue(src, "m_lmClubPath",    -0.494054);
        checkValue(src, "m_lmAttackAngle", -2.730836);
        checkValue(src, "m_smashFactor",    1.388462623429449);
    }

    std::printf("\nsparse_noclub: known values\n");
    {
        const LiveMeasureSource src(sparse, pack);
        check(src.grid().metrics.size() == 1, "exactly one metric reached the grid");
        // No review block, so the club falls to the house-wide DRIVER stub — see
        // LiveMeasureSource::club() for why this seam cannot tell that from a declared driver.
        check(src.club() == QLatin1String("DRIVER"), "an undeclared club reads as the DRIVER stub");
        checkValue(src, "m_impactShaftLean", 163.63672117342375);
    }

    // ── The missing-reason vocabulary ───────────────────────────────────────
    std::printf("\nwhy a measure did not read\n");
    {
        const LiveMeasureSource rich_(rich, pack);
        const LiveMeasureSource lm_(lm, pack);
        const LiveMeasureSource part_(lmPart, pack);

        checkMissing(rich_, "m_notAMeasureAtAll", MissingKind::UnknownMeasure);

        // An lm.* measure on a capture with no monitor, and the same measure on a capture whose
        // monitor simply did not report that field. The distinction is the difference between
        // "connect a launch monitor" and "yours did not answer this one", and it is the only pair
        // in the vocabulary where a golfer's next action differs.
        checkMissing(rich_, "m_smashFactor", MissingKind::NoLaunchMonitor);
        checkMissing(part_, "m_lmClubPath",  MissingKind::LaunchMonitorFieldAbsent);
        check(part_.hasLaunchMonitor() && part_.grid().metrics.size() == 13,
              "…and lm_partial really is a monitor reporting 13 of its 25 fields");

        // A body metric on the launch-monitor-only capture: the producer exists, this shot has none.
        checkMissing(lm_, "m_ballPosition", MissingKind::MetricNotProduced);

        // The metric IS here; the reducer wants P5 and P6, which this swing never segmented. This is
        // the reason that would otherwise be invisible — it looks identical to a missing producer
        // from outside, and it is the one a golfer can act on by re-recording.
        //
        // rich_ is a static copied swing.json fixture, so this stays a valid PhaseNotSegmented
        // pin regardless of segmenter changes upstream. But note: as of the 2026-08-09 segmenter
        // change, the segmenter DOES now emit P5 (ArmParallelDown) on forearm-bound swings — this
        // fixture predates that and simply has no P5/P6 ticks recorded. When the fixture is
        // regenerated from a current segmenter run, re-check whether it still lacks P5/P6 (and
        // still exercises PhaseNotSegmented) or whether this assertion needs to flip to a
        // different missing-measure fixture.
        checkMissing(rich_, "m_lagAngleDown", MissingKind::PhaseNotSegmented);

        // (c) Nothing that has no producer reports a number. Zero would be a value the golfer's
        // ledger would happily plot.
        int planned = 0, wrong = 0;
        for (const Measure &m : pack.measures) {
            if (m.status == MeasureStatus::Live)
                continue;
            ++planned;
            if (rich_.value(m.id).has_value()) {
                ++wrong;
                std::printf("      %s (%s) produced a value\n", qPrintable(m.id),
                            qPrintable(measureStatusName(m.status)));
            }
        }
        std::printf("  %d measures are not live on the richest capture in the corpus\n", planned);
        // 20 -> 19: m_transitionPlaneShift (planned, `swingPlane` delta p4->p5, down-the-line) was
        // retired unbuilt and replaced by m_transitionPlaneDelta, which is LIVE — it has a producer
        // (shaft_plane.h via ShaftPlaneStage). Worth knowing WHY that one is live rather than
        // planned, because it reads oddly next to a measure that is deliberately normless: this
        // check equates "not live" with "nothing produces it", and a measure that DOES produce a
        // number while awaiting real norms would break that equation if it were parked as planned.
        check(planned == 19, "19 shipped measures have no producer yet");
        check(wrong == 0, "…and not one of them produced a value");
    }

    // ── The full stack, per fixture ─────────────────────────────────────────
    const Coverage cRich   = runFixture(rich,   pack, norms, "rich_7iron");
    const Coverage cLm     = runFixture(lm,     pack, norms, "lm_7iron");
    const Coverage cSparse = runFixture(sparse, pack, norms, "sparse_noclub");

    // THE NUMBERS BELOW ARE OBSERVED, NOT AUTHORED. They are what the shipped pack and norm set
    // answer on these three swings today, pinned so that a producer landing, a corridor being
    // authored, or a metric key drifting shows up as a deliberate edit here rather than as silence.
    // Raising them is the point of the next several stages; lowering one without saying so is the
    // regression this test exists to catch.
    std::printf("\nthe measurement\n");
    check(cRich.assessable > 0 && cLm.assessable > 0, "the two rich captures assess something at all");
    // 51 → 53 on 2026-08-12: `top` and `sky`, and this fixture has NO launch monitor at all — so
    // it is worth saying why they became answerable on a camera-only swing. Both are conjunctions
    // (detection: all), and one conjunct that was assessed and did not fire settles an AND whatever
    // the unreadable terms would have said. This capture has no strike height and never will, but
    // it has an attack angle, and an attack angle of -4° is not the upward strike a top requires.
    // "Definitely not a top" is a real answer, and the conjunction reaches it from evidence no
    // single one of its terms could.
    check(cRich.assessable == 53, "rich_7iron: 53 of 152 conditions assessable (observed)");
    check(cRich.measures   == 38, "rich_7iron: 38 of 109 live measures resolved (observed)");
    // 12 → 14 on 2026-08-09: sig_launchLow/sig_launchHigh moved onto m_lmLaunchAngle (the
    // measured key this fixture actually carries), so launch_low and launch_high became
    // assessable on an LM-only capture.
    //
    // 14 → 17 on 2026-08-12: thin, chunk and shank stopped being asserted. All three are now
    // threshold signals on the two face-impact readings this fixture already carried, so nothing
    // new had to be captured for them to become answerable — the numbers were always there and the
    // pack simply had no signal reading them. The one finding that fires on this swing is `thin`:
    // 14.1 mm below centre, past the panel's own 10 mm boundary.
    //
    // NOT 19. `pull_hook` and `push_slice` are measured too now, but they read `m_compoundMiss`,
    // which is DERIVED at write time rather than read off the device — and this fixture is a
    // verbatim corpus copy written by an older build, so its metrics array does not contain the
    // key. That is the honest state of every swing already on disk: they gain the two conditions
    // when the reading is next folded in, not before. Regenerating the fixture to paper over it
    // would hide exactly the thing worth knowing.
    // 17 → 21 and 25 → 26 on 2026-08-12, and the single extra MEASURE is the whole story. It is
    // `m_attackAngle`, which now prefers `lm.attackAngle` and falls back to our projected
    // `attackAngle` (Measure::preferKeys). This fixture is device-only: it has never had a bare
    // `attackAngle` and could not have one, so before the ladder the measure resolved nothing —
    // and `attack_too_steep` and `attack_too_shallow` were therefore unanswerable on a swing whose
    // launch monitor had reported the attack angle outright, -2.73°, sitting in the document read
    // by nothing. Those two conditions plus `top` and `sky` are the four.
    check(cLm.assessable   == 21, "lm_7iron: 21 of 152 conditions assessable (observed)");
    check(cLm.measures     == 26, "lm_7iron: 26 of 109 live measures resolved (observed)");
    check(cSparse.assessable == 2, "sparse_noclub: 2 of 152 conditions assessable (observed)");
    check(cSparse.measures   == 1, "sparse_noclub: 1 of 109 live measures resolved (observed)");
    check(cRich.assessable > cSparse.assessable,
          "a richer capture assesses strictly more than a degraded one");

    // ── Known groups: the verdict must match the arithmetic ─────────────────
    //
    // Four conditions whose numbers can be worked out on paper from the fixture and the shipped
    // norm. This is the part that cannot be satisfied by a pipeline that runs cleanly and grades
    // everything wrong.
    std::printf("\nknown groups: the verdict against the arithmetic\n");
    {
        const LiveMeasureSource src(rich, pack);
        const LiveDetection     d = detectForSwing(src, pack, norms);

        // m_impactShaftLean = 41.0°, graded at `iron` (inherited by iron_7): mu 8, sigmaHi 6, so
        // the Ideal band tops out at 14 and Action begins at 26. 41 is 5.5 tolerances out on the
        // high side — the high tail fires, the low tail cannot.
        const Finding *hi = d.result.find(QStringLiteral("excessive_shaft_lean"));
        const Finding *lo = d.result.find(QStringLiteral("insufficient_shaft_lean"));
        check(hi && hi->state == FindingState::Fired,
              "41° of shaft lean fires excessive_shaft_lean (5.5 sigma above an iron's 8°)");
        check(lo && lo->state == FindingState::NotFired,
              "…and the same reading does NOT fire the low tail of the same axis");
        check(hi && hi->evidence.hasEvidence && near(hi->evidence.value, 41.0, 1e-9),
              "the finding carries the 41° that produced it");
        check(hi && hi->evidence.hasCorridor && near(hi->evidence.corridorHi, 14.0),
              "…and the corridor it was tested against");

        // m_ballPosition = 45.49% of stance width, graded at `iron`: mu 33, sigma 10. That is
        // OUTSIDE the Ideal band (23..43) and well inside Good (13..53). THE SIGNAL MUST NOT FIRE.
        // Ideal is |z| <= 1, so a detector that fired here would flag a third of any population on
        // every characteristic in the library — the single failure norm_measure_source_test names
        // first and the one only real data can demonstrate.
        const Finding *back = d.result.find(QStringLiteral("ball_back"));
        check(back && back->state == FindingState::NotFired,
              "a ball position outside Ideal but inside Good does NOT fire ball_back");
        check(back && back->evidence.hasEvidence && back->evidence.z > 1.0 && back->evidence.z < 2.0,
              "…and the NotFired finding still records how far out it sat (1 < z < 2)");

        // m_pelvisSwayBack = -28.60% of stance width against mu -5, sigma 7.5: 3.15 tolerances
        // below, past the Watch edge at -27.5, so Action on the low tail.
        const Finding *sway = d.result.find(QStringLiteral("sway"));
        check(sway && sway->state == FindingState::Fired,
              "28.6% of sway off the ball fires `sway` (3.1 sigma below a -5% norm)");
        check(sway && sway->direction == Direction::Low, "…on the low tail, which is the one authored");
    }
    {
        // m_smashFactor = 1.3885 on a 7 iron, a FLOOR: mu 1.38, so anything at or above the
        // aspiration is Ideal by construction and the deficit signal cannot fire. The arithmetic
        // matters because a floor graded as a target would call this reading a fault for being
        // 0.008 above the mean.
        const LiveMeasureSource src(lm, pack);
        const LiveDetection     d = detectForSwing(src, pack, norms);

        const Finding *smash = d.result.find(QStringLiteral("smash_deficit"));
        check(smash && smash->state == FindingState::NotFired,
              "a smash of 1.389 against an iron floor of 1.38 does NOT fire smash_deficit");
        check(smash && smash->evidence.hasCorridor && smash->evidence.highOpen,
              "…and the evidence reports the open high tail rather than a fabricated upper edge");
    }

    std::printf("\n%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
