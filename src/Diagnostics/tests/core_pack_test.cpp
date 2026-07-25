// Acceptance tests for the SHIPPED seed pack (src/Resources/diagnostics/core.json).
//
// The pack is not done when its characteristics load — it is done when the causal graph resolves in
// the right direction, the dominant causes concentrate, and no brand name has leaked into the
// content. Each of those is a way the pack can be wrong while looking entirely correct.
//
//   cmake --build build/analyzer-tests --target core_pack_test
//   ctest --test-dir build/analyzer-tests -R core_pack --output-on-failure

#include "../characteristic_pack.h"

#include <QFile>

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

int main()
{
    std::printf("core_pack_test\n");

    QFile f(QStringLiteral(PP_CORE_PACK_PATH));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("  [FAIL] cannot open %s\n", PP_CORE_PACK_PATH);
        return 1;
    }
    const QByteArray   raw  = f.readAll();
    const PackLoadResult res = loadPack(raw, QStringLiteral("core.json"));
    const CharacteristicPack &p = res.pack;

    // ── It loads and validates ──────────────────────────────────────────────────
    {
        if (!res.loaded)
            for (const ValidationIssue &i : res.report.withSeverity(IssueSeverity::Error))
                std::printf("        error: %s\n", qPrintable(i.message));
        check(res.loaded, "the shipped pack loads and validates with no errors");
        check(p.id == QStringLiteral("core"), "pack id is 'core'");
        check(p.schemaVersion == kPackSchemaVersion, "pack declares the current schema version");
    }

    // ── Content census ──────────────────────────────────────────────────────────
    int observable = 0, screened = 0, asserted = 0;
    for (const Condition &c : p.conditions) {
        if (c.observability == Observability::Observable) ++observable;
        if (c.confirmedBy == ConfirmedBy::Screened) ++screened;
        if (c.confirmedBy == ConfirmedBy::Asserted) ++asserted;
    }
    {
        check(observable >= 25, "at least 25 observable characteristics ship on day one");
        check(screened >= 10, "the screened cause library is seeded");
        check(asserted >= 5, "behavioural causes are seeded");
        check(p.edges.size() >= 75, "the causal graph is seeded, not a stub");
        std::printf("        (%d observable, %d screened, %d behavioural, %d edges, %d measures)\n",
                    observable, screened, asserted, int(p.edges.size()), int(p.measures.size()));
    }

    // ── Cause concentration ─────────────────────────────────────────────────────
    // The whole point of the model: a handful of latent causes explain most of the pack, and none of
    // them needs any capture hardware. If every characteristic had its own private cause, this would
    // be a restated fault list rather than a diagnosis.
    {
        std::map<int, QString, std::greater<int>> byCoverage;
        for (const Condition &c : p.conditions) {
            const int cov = coverageOf(p, c.id);
            if (cov > 0) byCoverage.insert({ cov, c.id });
        }

        auto coverage = [&](const char *id) { return coverageOf(p, QString::fromLatin1(id)); };
        check(coverage("poor_pelvic_disassociation") == 9, "poor pelvic disassociation explains 9");
        check(coverage("limited_thoracic_rotation") == 7, "limited thoracic rotation explains 7");
        check(coverage("limited_lead_hip_ir") == 6, "limited lead-hip internal rotation explains 6");
        check(coverage("limited_trail_hip_ir") == 5, "limited trail-hip internal rotation explains 5");
        check(coverage("poor_core_stability") == 5, "poor core stability explains 5");

        const int topFive = coverage("poor_pelvic_disassociation") + coverage("limited_thoracic_rotation")
                          + coverage("limited_lead_hip_ir") + coverage("limited_trail_hip_ir")
                          + coverage("poor_core_stability");
        check(topFive >= int(p.edges.size()) * 3 / 10,
              "five causes account for a substantial share of every causal edge");

        // Every dominant cause must be screen-backed — that is what makes the output actionable
        // without any capture hardware at all.
        bool allScreened = true;
        for (const char *id : { "poor_pelvic_disassociation", "limited_thoracic_rotation",
                                "limited_lead_hip_ir", "limited_trail_hip_ir", "poor_core_stability" }) {
            const Condition *c = p.condition(QString::fromLatin1(id));
            if (!c || c->confirmedBy != ConfirmedBy::Screened) allScreened = false;
        }
        check(allScreened, "every dominant cause is screen-backed");
    }

    // ── Edge orientation, structurally ──────────────────────────────────────────
    // The seed tables read effect-first while Edge is cause-first, so every row flips on
    // transcription. A coverage count CANNOT catch a mistake here — totals are identical under edge
    // reversal — so this is the assertion that guards the whole graph.
    {
        bool noneInverted = true;
        for (const Condition &c : p.conditions) {
            if (c.confirmedBy != ConfirmedBy::Screened) continue;
            if (!causesOf(p, c.id).isEmpty()) noneInverted = false;      // in-degree must be 0
            if (effectsOf(p, c.id).isEmpty()) noneInverted = false;      // out-degree must be > 0
        }
        check(noneInverted, "every screened cause has in-degree 0 and out-degree > 0");

        // Two spot checks in plain English, so a reader can see the orientation is right.
        check(effectsOf(p, QStringLiteral("limited_hip_extension"))
                  .contains(QStringLiteral("s_posture")),
              "limited hip extension CAUSES S-posture (not the reverse)");
        check(causesOf(p, QStringLiteral("early_extension"))
                  .contains(QStringLiteral("limited_lead_hip_ir")),
              "early extension is CAUSED BY limited lead-hip internal rotation");
    }

    // ── Every characteristic resolves to Live, or to a NAMED missing measure ────
    {
        int live = 0, planned = 0, noProducer = 0, notCapturable = 0;
        bool everyGapNamed = true;

        for (const Condition &c : p.conditions) {
            if (c.observability != Observability::Observable) continue;
            for (const QString &sid : c.detectedBy) {
                const Signal *s = p.signal(sid);
                if (!s) continue;
                for (const QString &mid : s->measures) {
                    const Measure *m = p.measure(mid);
                    if (!m) { everyGapNamed = false; continue; }
                    switch (m->status) {
                    case MeasureStatus::Live:          ++live; break;
                    case MeasureStatus::Planned:       ++planned; break;
                    case MeasureStatus::NoProducer:    ++noProducer; break;
                    case MeasureStatus::NotCapturable:
                        ++notCapturable;
                        if (m->gapReason.isEmpty()) everyGapNamed = false;
                        break;
                    }
                }
            }
        }
        std::printf("        (measure bindings: %d live, %d planned, %d no-producer, %d capture-gap)\n",
                    live, planned, noProducer, notCapturable);
        check(everyGapNamed, "every characteristic resolves to a real measure, gaps named");
        check(live > 0, "some characteristics are LIVE on day one, not all stubs");
    }

    // ── Provided measures bind to the catalogue, not to a parallel registry ─────
    {
        bool allBound = true;
        int  provided = 0;
        for (const Measure &m : p.measures) {
            if (m.kind != MeasureKind::Provided) continue;
            ++provided;
            if (m.metricKey.isEmpty()) allBound = false;
        }
        check(provided > 0 && allBound, "every Provided measure names a MetricCatalogue key");

        // The payoff of ranking series rather than reduced measures: one producer unblocks several
        // characteristics. pelvisSway carries sway, slide and hanging back at three different phases.
        int onPelvisSway = 0;
        for (const Measure &m : p.measures)
            if (m.metricKey == QStringLiteral("pelvisSway")) ++onPelvisSway;
        check(onPelvisSway == 3, "three characteristics sit on one pelvis-sway series (one producer)");
    }

    // ── Capture gaps are gaps, not roadmap items ────────────────────────────────
    {
        const Measure *thoracic = p.measure(QStringLiteral("m_thoracicCurve"));
        const Measure *lumbar   = p.measure(QStringLiteral("m_lumbarCurve"));
        check(thoracic && thoracic->status == MeasureStatus::NotCapturable,
              "C-posture's measure is a capture gap (no spinal keypoint exists)");
        check(lumbar && lumbar->status == MeasureStatus::NotCapturable,
              "S-posture's measure is a capture gap");
        check(thoracic && !thoracic->gapReason.isEmpty(), "the gap carries a reason for the UI");

        // Carried deliberately despite being unmeasurable: four conditions cite C-posture as a
        // cause, so dropping it would cost them their strongest explanation.
        check(coverageOf(p, QStringLiteral("c_posture")) >= 2,
              "C-posture is carried because other conditions depend on it");
    }

    // ── Screened and Behavioural causes never enter the roadmap ─────────────────
    // One row implying a producer that will never be built corrupts the artefact for every other
    // row, so this is a hard rule rather than a presentation choice.
    {
        bool clean = true;
        for (const Condition &c : p.conditions) {
            if (!isOutsideCaptureReach(c.confirmedBy)) continue;
            for (const QString &sid : c.detectedBy) {
                const Signal *s = p.signal(sid);
                if (s && !s->measures.isEmpty()) clean = false;   // it would land in the roadmap
            }
        }
        check(clean, "no Physical/Behavioural cause carries a measure that could reach the roadmap");
    }

    // ── Tail splits ─────────────────────────────────────────────────────────────
    {
        for (const char *axis : { "ball_position", "stance_width", "alignment", "ball_body_distance" })
            check(tailsOfAxis(p, QString::fromLatin1(axis)).size() == 2,
                  "a two-sided characteristic has both tails authored");

        // Both tails must sit on the same measure — that is what makes them tails.
        const QStringList ballTails = tailsOfAxis(p, QStringLiteral("ball_position"));
        check(ballTails.size() == 2 && ballTails.contains(QStringLiteral("ball_forward"))
                  && ballTails.contains(QStringLiteral("ball_back")),
              "ball position splits into forward and back");

        // Mark's case, wired at BOTH ends: ball forward opens the shoulders, ball back closes them.
        check(effectsOf(p, QStringLiteral("ball_forward")).contains(QStringLiteral("alignment_open")),
              "ball forward causes an open shoulder line");
        check(effectsOf(p, QStringLiteral("ball_back")).contains(QStringLiteral("alignment_closed")),
              "ball back causes a closed shoulder line");
        check(!effectsOf(p, QStringLiteral("ball_forward")).contains(QStringLiteral("alignment_closed")),
              "the tails are not cross-wired");
    }

    // ── C-posture's two routes have different remedies ──────────────────────────
    {
        const QStringList causes = causesOf(p, QStringLiteral("c_posture"));
        check(causes.contains(QStringLiteral("thoracic_kyphosis")), "C-posture has a physical cause");
        check(causes.contains(QStringLiteral("ball_too_far")), "C-posture has a setup-induced cause");

        const Condition *phys  = p.condition(QStringLiteral("thoracic_kyphosis"));
        const Condition *setup = p.condition(QStringLiteral("ball_too_far"));
        check(phys && phys->confirmedBy == ConfirmedBy::Screened, "the physical route is screened");
        check(setup && setup->confirmedBy == ConfirmedBy::Measured, "the setup route is measured");
    }

    // ── No brand names anywhere in the content ──────────────────────────────────
    // Several conditions are named with terms popularised by a commercial screening system. The
    // TERMS are common domain and stay; the ATTRIBUTION must not enter the repo in any form — not a
    // citation, not an author, not an explanatory note. Checked against the raw bytes so a comment
    // or a stray field cannot slip past.
    {
        const char *forbidden[] = { "titleist", "tpi", "trackman", "flightscope", "foresight",
                                    "k-vest", "kvest", "gears", "swingcatalyst", "boditrak",
                                    "performance institute", "certified" };
        bool clean = true;
        const QString lower = QString::fromUtf8(raw).toLower();
        for (const char *needle : forbidden) {
            if (lower.contains(QLatin1String(needle))) {
                std::printf("        brand token found: '%s'\n", needle);
                clean = false;
            }
        }
        check(clean, "no commercial organisation, product or certification body is named");
    }

    // ── Uncited content is honestly badged ──────────────────────────────────────
    {
        int proposed = 0;
        for (const Condition &c : p.conditions)
            if (c.provenance.tier == ProvenanceTier::Proposed) ++proposed;
        check(proposed > 0, "uncited content is tiered as proposed, not laundered");

        bool noFakeCitations = true;
        for (const Condition &c : p.conditions)
            if (c.provenance.tier != ProvenanceTier::Proposed && c.provenance.citation.isEmpty())
                noFakeCitations = false;
        check(noFakeCitations, "no condition claims a tier above proposed without a citation");
    }

    // ── Every condition says what it costs the golfer ───────────────────────────
    {
        bool allHaveConsequence = true;
        for (const Condition &c : p.conditions)
            if (c.consequence.text().isEmpty()) allHaveConsequence = false;
        check(allHaveConsequence, "every condition carries a plain-English consequence");
    }

    // ── The health list is populated, and it is warnings not errors ─────────────
    {
        const int warnings = res.report.warningCount();
        check(warnings > 0, "the health list has content (uncited tiers, single-tail axes)");
        check(res.report.errorCount() == 0, "nothing in the health list is an error");
        std::printf("        (%d health-list warnings)\n", warnings);
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
