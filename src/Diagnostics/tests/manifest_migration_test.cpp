// Stage 9's gate: the corridors the metric surfaces used to get from the MANIFEST now come from the
// NORM SET, through corridorForMetricAtPhase() — and nothing a surface used to draw went missing on
// the way.
//
// `MetricCatalogue::corridor()` is gone, along with `MetricNormative` and the one inline corridor the
// manifest carried. Everything MetricDetail, PpBandRail and the two dashboard zones render now
// resolves (metric, phase) → measure → norm. That join can fail silently in a way no compiler
// notices: a measure whose reducer names a phase the metric does not declare simply finds nothing,
// the corridor vanishes, and the tile renders as though the metric never had a band. So the metrics
// that HAD corridors are enumerated here, by hand, and each one must still produce one.
//
//   cmake --build build/analyzer-tests --target manifest_migration_test
//   ctest --test-dir build/analyzer-tests -R manifest_migration --output-on-failure

#include "../metric_corridor.h"
#include "../pack_provider.h"
#include "../../Metrics/metric_catalogue.h"

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
static void checkNear(const char *label, double got, double want)
{
    const bool ok = std::fabs(got - want) <= 1e-9;
    std::printf("  [%s] %-52s got %9.4f  want %9.4f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

int main()
{
    const std::unique_ptr<ICharacteristicPackProvider> packProv = makeCharacteristicPackProvider();
    const std::shared_ptr<const INormProvider>         norms    = sharedNormProvider();
    const CharacteristicPack                         &pack     = packProv->pack();
    const MetricCatalogue                              cat      = makeMetricCatalogue();
    const QString                                      kFull    = QStringLiteral("full_swing");

    check(!pack.measures.empty(), "the shipped characteristic pack loaded");
    check(!norms->norms().norms.empty(), "the shipped norm set loaded");

    // ── The tempo corridor, which was the manifest's only inline one ─────────
    //
    // ⚠ THESE FOUR NUMBERS ARE A MIGRATION PIN, and the only one left after stage 9 deleted the
    // wrist-table parity gate. They are the corridor the manifest inlined (green 2.2–3.0, amber
    // 1.8–3.6) and they license the deletion of that inline block. They will fail when tempo is
    // legitimately re-centred from a corpus — ledger C9 — and that is when this section goes, not
    // when the numbers are edited to match.
    std::printf("=== the tempo corridor survived the conversion ===\n");
    {
        const std::optional<MetricCorridor> c =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("tempoRatio"), Phase::Impact, kFull);
        check(c.has_value(), "tempoRatio resolves a corridor at Impact");
        if (c) {
            checkNear("green lo", c->greenLo, 2.2);
            checkNear("green hi", c->greenHi, 3.0);
            checkNear("amber lo", c->amberLo, 1.8);
            checkNear("amber hi", c->amberHi, 3.6);
            check(!c->deltaFromAddress, "the tempo corridor is absolute, not Δ-from-address");
            check(c->measureId == QLatin1String("m_tempoRatio"),
                  "it resolved through the m_tempoRatio measure");
        }

        // The dashboard Verdict tile asks for phase 5 by number, and renders only when BOTH a sample
        // and a corridor resolve there. Asserted as an int because that is how it crosses into QML.
        check(static_cast<int>(Phase::Impact) == 5,
              "Impact is still phase 5 — the number the Verdict tile passes");

        // The measure reads the phase its PRODUCER labels. It asked for P4 until stage 9 (ledger
        // C20) while tempo_metrics emits its phaseSample at P7, so the measure resolved unavailable
        // on every swing AND the join found nothing at Impact — the corridor would simply have
        // disappeared from two live surfaces.
        const Measure *m = pack.measure(QStringLiteral("m_tempoRatio"));
        check(m != nullptr && m->reducer.kind == ReducerKind::At
                  && m->reducer.anchor.value_or(Phase::Address) == Phase::Impact,
              "m_tempoRatio reads at P7 — the phase its producer labels");
        check(!corridorForMetricAtPhase(pack, *norms, QStringLiteral("tempoRatio"),
                                        Phase::Top, kFull).has_value(),
              "…and nothing at the Top, which the metric does not declare");
    }

    // ── The provisional-basis prose was not lost ────────────────────────────
    //
    // The manifest's contextNote was the only record of WHY the tempo band is provisional (the
    // published figures are measured Takeaway→Top where this metric is Address→Top). Stage 9 moved
    // it into the norm's citation. Deleting the note without moving it would have thrown away the
    // reason and left four numbers that look authoritative.
    std::printf("=== the reason the tempo band is provisional travelled with it ===\n");
    {
        const NormResolution res = norms->resolve(QStringLiteral("m_tempoRatio"), kFull);
        check(res.found(), "the tempo norm resolves");
        if (res.found()) {
            const QString cite = res.norm->citation;
            check(!cite.trimmed().isEmpty(), "the tempo norm carries a citation");
            check(cite.contains(QStringLiteral("Takeaway"), Qt::CaseInsensitive)
                      && cite.contains(QStringLiteral("Address"), Qt::CaseInsensitive),
                  "it states the basis mismatch that makes the band provisional");
            check(res.norm->source == NormSource::Heuristic && normIsWeak(*res.norm),
                  "and it reads as an authored figure, not a finding");
        }
    }

    // ── Every metric that had a corridor still has one ──────────────────────
    //
    // The five DOF metrics delegated to the compiled band table through `.normative.dof`. They now
    // resolve through their (DOF, position) cell measures. Enumerated by hand rather than derived,
    // because "derive the list from the thing under test" is how a gate passes on an empty set.
    std::printf("=== the DOF metrics still band at their declared phases ===\n");
    {
        const char *kDofMetrics[] = { "leadWristFlexExt", "leadWristRadUln", "forearmPronation",
                                      "leadArmFlexion",   "trailWristFlexExt" };
        for (const char *key : kDofMetrics) {
            const MetricDescriptor *d = cat.descriptor(QLatin1String(key));
            if (d == nullptr) {
                check(false, key);
                continue;
            }
            check(!d->phases.empty(), key);
            int resolved = 0;
            for (Phase p : d->phases)
                if (corridorForMetricAtPhase(pack, *norms, d->key, p, kFull).has_value())
                    ++resolved;
            std::printf("      %-18s %d of %zu declared phases carry a corridor\n",
                        key, resolved, d->phases.size());
            check(resolved == int(d->phases.size()),
                  "every declared phase of this metric resolves a corridor");
        }
    }

    // ── The absolute reading wins at a phase both measures name ─────────────
    //
    // A corridor keyed on a PHASE means the absolute reading there, which is why
    // measureForMetricAtPhase prefers `at` over `delta`. leadWristFlexExt is the case that exists:
    // the Δ-from-address cell measure and m_leadWristAtImpact both name P7. This CHANGED what the
    // detail page draws at Impact for that metric — it used to be the Δ corridor — and the change is
    // the intent, not a side effect.
    std::printf("=== absolute beats delta where both name the phase ===\n");
    {
        const std::optional<MetricCorridor> atTop =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("leadWristFlexExt"), Phase::Top, kFull);
        const std::optional<MetricCorridor> atImpact =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("leadWristFlexExt"), Phase::Impact, kFull);

        check(atImpact.has_value() && !atImpact->deltaFromAddress
                  && atImpact->measureId == QLatin1String("m_leadWristAtImpact"),
              "at Impact the ABSOLUTE reading wins, and the corridor is marked absolute");

        // At the Top the preferred measure is `m_leadWristAtTop` — absolute, and carrying NO norm.
        // The corridor must fall through to the Δ-from-address cell measure beside it rather than
        // vanishing: that cell has carried a band since v1, and this is the case that caught the
        // first version of corridorForMetricAtPhase() taking the winner and stopping.
        check(pack.measure(QStringLiteral("m_leadWristAtTop")) != nullptr
                  && !norms->resolve(QStringLiteral("m_leadWristAtTop"), kFull).found(),
              "m_leadWristAtTop exists and still has no norm — the premise of the next assertion");
        check(atTop.has_value() && atTop->deltaFromAddress
                  && atTop->measureId == QLatin1String("m_leadWristFlexExt_p4"),
              "…so the Top corridor falls through to the Δ-from-address cell, and says it is a Δ");
        check(atTop && atImpact && (atTop->greenLo != atImpact->greenLo),
              "the two corridors are genuinely different numbers — the flag is load-bearing");
    }

    // ── Context resolution reaches the corridors ────────────────────────────
    //
    // What the migration was FOR: a club-dependent corridor is a context, not a note. stanceWidth and
    // ballPosition carry per-club rows, and the same call in a different context must return
    // different numbers — otherwise the tree is decoration.
    std::printf("=== a per-club corridor resolves per club ===\n");
    {
        for (const char *key : { "stanceWidth", "ballPosition" }) {
            const std::optional<MetricCorridor> full =
                corridorForMetricAtPhase(pack, *norms, QLatin1String(key), Phase::Address, kFull);
            const std::optional<MetricCorridor> driver =
                corridorForMetricAtPhase(pack, *norms, QLatin1String(key), Phase::Address,
                                         QStringLiteral("driver"));
            check(full.has_value() && driver.has_value(), key);
            if (full && driver) {
                check(!full->inherited, "the full-swing row is its own, not inherited");
                check(!driver->inherited, "the driver row is its own too");
                check(driver->greenLo != full->greenLo || driver->greenHi != full->greenHi,
                      "and it says something different from the general one");
            }
        }

        // The two ways a context can be "not specific": an EMPTY one is a shot that declared
        // nothing, and falls back to the default; an UNRECOGNISED one resolves to nothing rather
        // than being quietly graded against a corridor that was never meant for it. Both directions
        // are asserted because collapsing them is the tempting simplification.
        const std::optional<MetricCorridor> unset =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("stanceWidth"), Phase::Address,
                                     QString());
        check(unset.has_value(), "a shot that declares no context gets the default corridor");
        const std::optional<MetricCorridor> bogus =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("stanceWidth"), Phase::Address,
                                     QStringLiteral("hovercraft"));
        check(!bogus.has_value(), "an unrecognised context yields NO corridor, never a fallback");
    }

    // ── The grade policy reaches the corridor ───────────────────────────────
    //
    // For a norm with no explicit monitor band the Watch edge IS the policy, and stance width and
    // ball position — both drawn on the dashboard Setup zone — are exactly that shape. If the façade
    // resolved corridors under the default while the user had chosen Strict, the dashboard would draw
    // a corridor the app does not grade on.
    std::printf("=== the grade policy moves the amber edge, where it may ===\n");
    {
        const GradePolicy strict = gradePolicyByName(QStringLiteral("strict"));
        const GradePolicy loose  = gradePolicyByName(QStringLiteral("lenient"));
        check(strict.watchMaxZ < loose.watchMaxZ, "strict really is tighter than lenient");

        const std::optional<MetricCorridor> s =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("stanceWidth"), Phase::Address,
                                     kFull, strict);
        const std::optional<MetricCorridor> l =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("stanceWidth"), Phase::Address,
                                     kFull, loose);
        check(s.has_value() && l.has_value(), "stanceWidth resolves under both policies");
        if (s && l) {
            check(s->amberLo > l->amberLo && s->amberHi < l->amberHi,
                  "a z-derived Watch edge tightens under a stricter policy");
            check(s->greenLo == l->greenLo && s->greenHi == l->greenHi,
                  "…and the Ideal band does not move: it is the norm's own claim");
        }

        // The tempo norm states its monitor bounds, so the policy must NOT move its amber edge.
        const std::optional<MetricCorridor> t =
            corridorForMetricAtPhase(pack, *norms, QStringLiteral("tempoRatio"), Phase::Impact,
                                     kFull, strict);
        check(t.has_value() && std::fabs(t->amberLo - 1.8) < 1e-9,
              "an explicit monitor band is the norm's claim and ignores the policy");
    }

    // ── Nothing is invented where there is nothing ──────────────────────────
    std::printf("=== no measure or no norm ⇒ no corridor ===\n");
    {
        check(!corridorForMetricAtPhase(pack, *norms, QStringLiteral("clubheadSpeed"),
                                        Phase::Impact, kFull).has_value(),
              "clubheadSpeed has no defensible band yet and gets no corridor");
        check(!corridorForMetricAtPhase(pack, *norms, QStringLiteral("kinematicSequence"),
                                        Phase::Impact, kFull).has_value(),
              "a Sequence metric has no corridor");
        check(!corridorForMetricAtPhase(pack, *norms, QStringLiteral("nope"),
                                        Phase::Impact, kFull).has_value(),
              "an unknown metric key gets no corridor");
        check(!corridorForMetricAtPhase(pack, *norms, QStringLiteral("leadWristFlexExt"),
                                        Phase::Finish, kFull).has_value(),
              "a phase no measure reads gets no corridor");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
