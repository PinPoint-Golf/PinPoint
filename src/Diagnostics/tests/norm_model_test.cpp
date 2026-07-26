// NormModel — the QML façade over measures, norms and contexts
// (src/Gui/characteristics/norm_model.{h,cpp}), run against the SHIPPED content.
//
// The façade's whole job is that QML holds no rules, which means every rule it hides has to be
// asserted here or it is asserted nowhere. Four carry the weight, and each exists because getting
// it wrong is invisible in the rendered page:
//
//   1. measureForMetricAtPhase() — the join stage 9 re-points metric_catalog.cpp at. A reducer is
//      where a phase lives, so an `at` measure and a Δ-from-address measure can both name P4 on
//      one metric; returning the wrong one attaches a corridor to a different number and the page
//      still renders perfectly.
//   2. The norms-by-context list shows EVERY context that resolves, not only those with their own
//      row, and marks which is which. A list of authored rows only would leave nine of thirteen
//      contexts looking ungraded when they are graded by inheritance.
//   3. The Watch edge shown is the edge that GRADES: an explicit monitor dominates the z-derived
//      one, exactly as grade() applies it. Showing the z edge while grading on the monitor would
//      render a corridor the app does not use.
//   4. hasNorm is tri-state, and "no" is the interesting case — a corridor signal that cannot
//      fire. A bool filter cannot express it.
//
//   cmake --build build/analyzer-tests --target norm_model_test
//   ctest --test-dir build/analyzer-tests -R norm_model --output-on-failure

#include "norm_model.h"

#include "../characteristic_pack.h"

#include <QCoreApplication>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static QVariantMap rowFor(const QVariantList &rows, const char *id)
{
    for (const QVariant &v : rows)
        if (v.toMap().value(QStringLiteral("id")).toString() == QLatin1String(id)) return v.toMap();
    return {};
}

static QVariantMap normRowFor(const QVariantList &norms, const char *contextId)
{
    for (const QVariant &v : norms)
        if (v.toMap().value(QStringLiteral("askedContextId")).toString() == QLatin1String(contextId))
            return v.toMap();
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    NormModel        m;

    std::printf("=== the shipped content is reachable ===\n");
    {
        check(m.measureCount() > 60, "the pack's measures are loaded");
        check(m.normCount() > 50, "the norm set is loaded");
        check(m.normedMeasureCount() > 0 && m.normedMeasureCount() <= m.measureCount(),
              "some but not all measures resolve to a norm at the default context");
        check(m.contexts().size() >= 13, "the context tree is loaded");

        // The LAYERS, not the assembled set. "merged" is an implementation word and must never
        // reach a user: they need to see the shipped set as its own thing, because that is what
        // their own overrides will override.
        const QVariantList sets = m.normSets();
        check(sets.size() == 1, "one norm set today — the shipped core set");
        const QVariantMap core = sets.value(0).toMap();
        check(core.value(QStringLiteral("label")).toString() == QLatin1String("core"),
              "the layer names itself, not the merge that assembled it");
        check(core.value(QStringLiteral("readOnly")).toBool(), "the shipped set is read-only");
        check(core.value(QStringLiteral("normCount")).toInt() == m.normCount(),
              "the layer's row count accounts for the whole assembled set");
        std::printf("        (%d measures, %d norms, %d normed, %lld contexts)\n",
                    m.measureCount(), m.normCount(), m.normedMeasureCount(),
                    static_cast<long long>(m.contexts().size()));
    }

    std::printf("=== the context tree renders in tree order, with depth ===\n");
    {
        const QVariantList ctx = m.contexts();
        check(!ctx.isEmpty() && ctx.first().toMap().value(QStringLiteral("id")).toString()
                                    == QLatin1String("any"),
              "'any' is the first root");
        check(ctx.first().toMap().value(QStringLiteral("depth")).toInt() == 0, "a root is depth 0");

        // full_swing is a child of any; driver a child of full_swing. Depth is what the UI indents
        // by, so an off-by-one here flattens the whole list into a lie about the hierarchy.
        int depthFull = -1, depthDriver = -1;
        bool defaultMarked = false;
        for (const QVariant &v : ctx) {
            const QVariantMap r = v.toMap();
            const QString    id = r.value(QStringLiteral("id")).toString();
            if (id == QLatin1String("full_swing")) {
                depthFull     = r.value(QStringLiteral("depth")).toInt();
                defaultMarked = r.value(QStringLiteral("isDefault")).toBool();
            }
            if (id == QLatin1String("driver")) depthDriver = r.value(QStringLiteral("depth")).toInt();
        }
        check(depthFull == 1, "full_swing is depth 1");
        check(depthDriver == 2, "driver is depth 2");
        check(defaultMarked, "full_swing is flagged as the default context");
    }

    std::printf("=== the metric -> measure join ===\n");
    {
        // An `at` measure and a Δ measure both name P4 on leadWristFlexExt. The absolute reading
        // is what a corridor keyed on a phase means, so `at` must win outright.
        const QVariantMap top = m.measureForMetricAtPhase(QStringLiteral("leadWristFlexExt"),
                                                          int(Phase::Top));
        check(top.value(QStringLiteral("found")).toBool(), "leadWristFlexExt at P4 resolves");
        check(top.value(QStringLiteral("measureId")).toString() == QLatin1String("m_leadWristAtTop"),
              "an `at` measure beats a Δ measure on the same phase");
        check(!top.value(QStringLiteral("deltaFromAddress")).toBool(),
              "an `at` reading is not flagged Δ-from-address");

        // No `at` measure exists for P2, so the Δ-from-address cell measure answers, and the flag
        // has to say so — a consumer plots the corridor against the wrong curve without it.
        const QVariantMap p2 = m.measureForMetricAtPhase(QStringLiteral("leadWristRadUln"),
                                                         int(Phase::ShaftParallelBack));
        check(p2.value(QStringLiteral("found")).toBool(), "leadWristRadUln at P2 resolves");
        check(p2.value(QStringLiteral("measureId")).toString()
                  == QLatin1String("m_leadWristRadUln_p2"),
              "the Δ-from-address cell measure answers where no `at` measure exists");
        check(p2.value(QStringLiteral("deltaFromAddress")).toBool(),
              "a Δ-from-address measure is flagged as such");
        check(p2.value(QStringLiteral("unit")).toString() == QString::fromUtf8("°"),
              "the measure's own unit comes back with it");

        // An extremum is not a reading at a phase. m_lagAngleDown is the lowest lag angle between
        // P5 and P6; returning it for either would attach a corridor to a different number.
        check(!m.measureForMetricAtPhase(QStringLiteral("lagAngle"), int(Phase::Delivery))
                   .value(QStringLiteral("found")).toBool(),
              "an extremum measure never answers a phase query");

        check(!m.measureForMetricAtPhase(QStringLiteral("noSuchMetric"), int(Phase::Impact))
                   .value(QStringLiteral("found")).toBool(),
              "an unknown metric reports not-found");
        check(!m.measureForMetricAtPhase(QStringLiteral("leadWristFlexExt"), 9999)
                   .value(QStringLiteral("found")).toBool(),
              "an int that is not a Phase enumerator reports not-found rather than casting");
    }

    std::printf("=== the pack-level join is the same join ===\n");
    {
        // The algorithm lives in the pack layer so C++ callers (stage 9) reach it without a QML
        // façade. If the two ever disagree, the page and the pipeline grade differently.
        const CharacteristicPack &pack = makeCharacteristicPackProvider()->pack();
        const Measure *direct = measureForMetricAtPhase(pack, QStringLiteral("leadWristFlexExt"),
                                                        Phase::Top);
        check(direct && direct->id == QLatin1String("m_leadWristAtTop"),
              "the pack-level function returns the same measure the façade does");
        check(measureForMetricAtPhase(pack, QString(), Phase::Top) == nullptr,
              "an empty metric key matches nothing");
    }

    std::printf("=== the directory ===\n");
    {
        const QVariantList all = m.measures();
        check(all.size() == m.measureCount(), "an unfiltered query returns every measure");

        const QVariantMap bp = rowFor(all, "m_ballPosition");
        check(!bp.isEmpty(), "ball position is in the directory");
        check(bp.value(QStringLiteral("hasNorm")).toBool(), "ball position resolves to a norm");
        check(bp.value(QStringLiteral("defaultContextId")).toString()
                  == QLatin1String("full_swing"),
              "a row summarises against the default context");
        check(!bp.value(QStringLiteral("normInherited")).toBool(),
              "ball position has its OWN full-swing row, so nothing is inherited");
        check(bp.value(QStringLiteral("ownNormCount")).toInt() == 5,
              "ball position carries five authored rows (full swing + four clubs)");
        check(bp.value(QStringLiteral("weakProvenance")).toBool(),
              "a heuristic norm is flagged weak");
        check(!bp.value(QStringLiteral("highMeans")).toString().isEmpty(),
              "the direction sentence travels with the row");
        check(bp.value(QStringLiteral("group")).toString() == QLatin1String("Feet & stance"),
              "the row is grouped by the metric catalogue's own group");

        // hasNorm is tri-state. "no" is the interesting filter — a measure nothing can grade.
        const QVariantMap yes{ { QStringLiteral("hasNorm"), QStringLiteral("yes") } };
        const QVariantMap no { { QStringLiteral("hasNorm"), QStringLiteral("no") } };
        const int nYes = m.measures(yes).size();
        const int nNo  = m.measures(no).size();
        check(nYes > 0 && nNo > 0, "both sides of the has-norm filter are non-empty");
        check(nYes + nNo == all.size(), "the two sides partition the directory exactly");
        check(nYes == m.normedMeasureCount(), "the census agrees with the filter");

        const QVariantMap live{ { QStringLiteral("status"), QStringLiteral("live") } };
        const QVariantList liveRows = m.measures(live);
        check(!liveRows.isEmpty() && liveRows.size() < all.size(), "the status filter narrows");
        for (const QVariant &v : liveRows)
            if (v.toMap().value(QStringLiteral("status")).toString() != QLatin1String("live")) {
                check(false, "the status filter admits only that status");
                break;
            }

        const QVariantMap search{ { QStringLiteral("search"), QStringLiteral("wrist") } };
        check(m.measures(search).size() > 0, "search matches on label");

        const QVariantMap none{ { QStringLiteral("group"), QStringLiteral("Not a group") } };
        check(m.measures(none).isEmpty(), "an unknown group returns nothing, not everything");
    }

    std::printf("=== norms by context: inheritance is visible, not implied ===\n");
    {
        const QVariantMap d = m.measureDetail(QStringLiteral("m_ballPosition"));
        check(!d.isEmpty(), "the detail page resolves");
        check(d.value(QStringLiteral("label")).toString().contains(QLatin1String("Ball position")),
              "the detail carries the measure's label");
        check(!d.value(QStringLiteral("highMeans")).toString().isEmpty(),
              "highMeans is on the detail page — this is where a sign convention surfaces");

        const QVariantList norms = d.value(QStringLiteral("norms")).toList();

        // Five authored rows, but MORE than five contexts resolve: the archetypes and every
        // partial-swing node inherit from full swing. That difference is the whole point of a
        // tree, and a list that showed only authored rows would hide it.
        check(norms.size() > 5, "more contexts resolve than have their own row");

        const QVariantMap driver = normRowFor(norms, "driver");
        check(!driver.isEmpty(), "driver resolves");
        check(driver.value(QStringLiteral("own")).toBool(), "driver has its OWN corridor");
        check(qFuzzyCompare(driver.value(QStringLiteral("mu")).toDouble(), 5.0),
              "the driver corridor is the driver's, not full swing's");

        const QVariantMap bowed = normRowFor(norms, "archetype_bowed");
        check(!bowed.isEmpty(), "an archetype context resolves by inheritance");
        check(bowed.value(QStringLiteral("inherited")).toBool(), "and is marked inherited");
        check(bowed.value(QStringLiteral("contextId")).toString() == QLatin1String("full_swing"),
              "the inherited row names WHERE it came from, so the UI need not guess");
        check(qFuzzyCompare(bowed.value(QStringLiteral("mu")).toDouble(), 30.0),
              "an inherited row shows the ancestor's numbers");

        // Depth is what the list indents by; a child must sit deeper than its parent.
        check(normRowFor(norms, "full_swing").value(QStringLiteral("depth")).toInt()
                  < driver.value(QStringLiteral("depth")).toInt(),
              "children indent deeper than their parent");
    }

    std::printf("=== the band edges shown are the band edges that grade ===\n");
    {
        // A migrated wrist-grid row: explicit monitor bounds, which DOMINATE the z-derived edge.
        const QVariantMap n = m.normAt(QStringLiteral("m_leadWristFlexExt_p1"),
                                       QStringLiteral("full_swing"));
        check(n.value(QStringLiteral("found")).toBool(), "the P1 cell norm resolves");
        check(n.value(QStringLiteral("explicitMonitor")).toBool(), "it carries explicit bounds");
        check(qFuzzyCompare(n.value(QStringLiteral("watchLo")).toDouble(), -10.0)
                  && qFuzzyCompare(n.value(QStringLiteral("watchHi")).toDouble(), 10.0),
              "the Watch edge is the monitor bound, not 3 sigma");
        check(qFuzzyCompare(n.value(QStringLiteral("idealLo")).toDouble(), -5.0)
                  && qFuzzyCompare(n.value(QStringLiteral("idealHi")).toDouble(), 5.0),
              "the Ideal band is mu +/- sigma");

        // The z-derived case, and the policy actually moving it. Ideal never moves — it is the
        // norm's own tolerance — but Good and Watch are policy-derived and must follow.
        const QVariantMap std_ = m.normAt(QStringLiteral("m_stanceWidth"),
                                          QStringLiteral("driver"));
        check(std_.value(QStringLiteral("found")).toBool(), "the driver stance-width norm resolves");
        check(!std_.value(QStringLiteral("explicitMonitor")).toBool(), "it has no explicit bounds");
        check(qFuzzyCompare(std_.value(QStringLiteral("watchHi")).toDouble(), 115.0 + 3.0 * 10.0),
              "the Watch edge is 3 sigma under the standard policy");

        m.setGradePolicy(QStringLiteral("strict"));
        const QVariantMap strict = m.normAt(QStringLiteral("m_stanceWidth"),
                                            QStringLiteral("driver"));
        check(qFuzzyCompare(strict.value(QStringLiteral("watchHi")).toDouble(), 115.0 + 2.25 * 10.0),
              "a stricter policy pulls the Watch edge in");
        check(qFuzzyCompare(strict.value(QStringLiteral("idealHi")).toDouble(),
                            std_.value(QStringLiteral("idealHi")).toDouble()),
              "but the Ideal band does not move — it is the norm's tolerance, not the policy's");

        // A monitor-bearing norm must be immune to the policy on its outer edge, or migrated
        // content would silently re-band the moment someone changed a setting.
        const QVariantMap strictMon = m.normAt(QStringLiteral("m_leadWristFlexExt_p1"),
                                               QStringLiteral("full_swing"));
        check(qFuzzyCompare(strictMon.value(QStringLiteral("watchHi")).toDouble(), 10.0),
              "an explicit monitor bound ignores the grade policy entirely");

        m.setGradePolicy(QStringLiteral("standard"));
        check(m.gradePolicy() == QLatin1String("standard"), "the policy round-trips");
        m.setGradePolicy(QStringLiteral("nonsense"));
        check(m.gradePolicy() == QLatin1String("standard"),
              "an unknown policy name lands on standard rather than persisting as itself");
        check(m.gradePolicies().size() == 3, "three presets are offered");
    }

    std::printf("=== the two ways a measure can be ungradeable read differently ===\n");
    {
        // A capture gap and a missing corridor are not the same statement, and merging them turns
        // "we cannot assess this" into "nobody has got round to it".
        const QVariantList gaps = m.measures(
            QVariantMap{ { QStringLiteral("status"), QStringLiteral("notCapturable") } });
        for (const QVariant &v : gaps) {
            const QVariantMap dd = m.measureDetail(v.toMap().value(QStringLiteral("id")).toString());
            if (dd.value(QStringLiteral("availability")).toString().isEmpty()) {
                check(false, "a capture gap always says why");
                break;
            }
        }
        std::printf("        (%lld capture gaps)\n", static_cast<long long>(gaps.size()));

        const QVariantList noNorm = m.measures(
            QVariantMap{ { QStringLiteral("hasNorm"), QStringLiteral("no") } });
        bool sawUngraded = false;
        for (const QVariant &v : noNorm) {
            const QVariantMap dd = m.measureDetail(v.toMap().value(QStringLiteral("id")).toString());
            if (dd.value(QStringLiteral("status")).toString() != QLatin1String("live")) continue;
            sawUngraded = true;
            check(dd.value(QStringLiteral("availability")).toString().contains(
                      QLatin1String("no norm")),
                  "a live measure with no norm says so, rather than looking fine");
            break;
        }
        if (!sawUngraded)
            std::printf("        (no live measure is missing a norm)\n");

        check(m.measureDetail(QStringLiteral("m_notAThing")).isEmpty(),
              "an unknown measure id returns an empty map, so a stale link lands on the catalogue");
    }

    std::printf("=== used-by is the blast radius, not just its size ===\n");
    {
        const QVariantMap d = m.measureDetail(QStringLiteral("m_ballPosition"));
        const QVariantList usedBy = d.value(QStringLiteral("usedBy")).toList();
        check(!usedBy.isEmpty(), "ball position is used by at least one characteristic");
        check(!usedBy.first().toMap().value(QStringLiteral("label")).toString().isEmpty(),
              "each user names itself, so an author sees WHAT they would change");
        check(rowFor(m.measures(), "m_ballPosition").value(QStringLiteral("usedBy")).toInt()
                  == usedBy.size(),
              "the row's count and the detail's list agree");
    }

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail,
                g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
