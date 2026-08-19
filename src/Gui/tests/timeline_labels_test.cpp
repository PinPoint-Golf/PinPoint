/*
 * Copyright (c) 2026 Mark Liversedge (liversedge@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

// Characterization of TimelineLabels — the pure, font-free maths behind the Transit
// timeline. stationLayout() is exercised indirectly (it composes distribute()); the
// font-measuring path needs a QGuiApplication and is left to the visual/manual check.
//
// positionLayout() shares that same font-measuring path (QFontMetricsF sizes each
// "Pn" label) but its non-overlap/sort/filter behaviour is exactly what the P1..P8
// timeline layer depends on, so it IS worth exercising directly here. A headless
// QGuiApplication (QT_QPA_PLATFORM=offscreen, no display needed) is bootstrapped in
// main() just for this — self-contained, so `ctest`/direct-run both work with no
// external env setup, and it does not change any of the other (still font-free) tests.

#include "timeline_labels.h"

#include <QGuiApplication>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <tuple>
#include <utility>

static int g_fail = 0;

static void checkTrue(const char *label, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_fail;
}
static void checkNear(const char *label, double got, double want, double tol = 1e-6)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("  [%s] %-32s got %10.4f  want %10.4f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static void checkEqI(const char *label, long long got, long long want)
{
    const bool ok = got == want;
    std::printf("  [%s] %-32s got %8lld  want %8lld\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static void checkStr(const char *label, const QString &got, const char *want)
{
    const bool ok = got == QString::fromLatin1(want);
    std::printf("  [%s] %-32s got %-16s want %-16s\n", ok ? "PASS" : "FAIL", label,
                got.toLocal8Bit().constData(), want);
    if (!ok) ++g_fail;
}

static QVariantList vl(std::initializer_list<double> xs)
{
    QVariantList l;
    for (double x : xs) l.append(x);
    return l;
}
static QVariantList phases(std::initializer_list<std::pair<int, qint64>> ps)
{
    QVariantList l;
    for (const auto &p : ps) {
        QVariantMap m;
        m.insert(QStringLiteral("phase"), p.first);
        m.insert(QStringLiteral("t_us"), QVariant::fromValue<qint64>(p.second));
        l.append(m);
    }
    return l;
}

// {p, t_us, source, conf} tuples — the fields positionLayout() reads from each
// club.positions entry (grip/head/theta/… pass through untouched, not needed here).
static QVariantList positionsList(
    std::initializer_list<std::tuple<int, qint64, int, double>> ps)
{
    QVariantList l;
    for (const auto &p : ps) {
        QVariantMap m;
        m.insert(QStringLiteral("p"),      std::get<0>(p));
        m.insert(QStringLiteral("t_us"),   QVariant::fromValue<qint64>(std::get<1>(p)));
        m.insert(QStringLiteral("source"), std::get<2>(p));
        m.insert(QStringLiteral("conf"),   std::get<3>(p));
        l.append(m);
    }
    return l;
}

int main(int argc, char **argv)
{
    // Headless-safe: offscreen needs no display. Only set the fallback when the
    // caller (ctest, a dev shell, ...) hasn't already picked a platform.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    TimelineLabels t;
    std::printf("=== TimelineLabels ===\n\n");

    // ── distribute: no overlap → passthrough ──────────────────────────────────
    std::printf("-- distribute: no overlap (passthrough) --\n");
    {
        const QVariantList r = t.distribute(vl({10, 50, 90}), vl({8, 8, 8}), 4, 0, 100);
        checkNear("c0", r.at(0).toDouble(), 10);
        checkNear("c1", r.at(1).toDouble(), 50);
        checkNear("c2", r.at(2).toDouble(), 90);
    }

    // ── distribute: overlapping centres pushed to >= gap clearance ─────────────
    std::printf("-- distribute: overlap pushed apart --\n");
    {
        // sizes 10, gap 2 → minimum centre spacing = 5 + 2 + 5 = 12.
        const QVariantList r = t.distribute(vl({50, 52, 54}), vl({10, 10, 10}), 2, 0, 200);
        checkNear("c0", r.at(0).toDouble(), 50);
        checkNear("c1", r.at(1).toDouble(), 62);
        checkNear("c2", r.at(2).toDouble(), 74);
        const double g1 = r.at(1).toDouble() - r.at(0).toDouble();
        const double g2 = r.at(2).toDouble() - r.at(1).toDouble();
        checkTrue("gap0>=12", g1 >= 12 - 1e-9);
        checkTrue("gap1>=12", g2 >= 12 - 1e-9);
        checkTrue("monotonic", r.at(0).toDouble() <= r.at(1).toDouble()
                                && r.at(1).toDouble() <= r.at(2).toDouble());
    }

    // ── distribute: clamp to upper bound ───────────────────────────────────────
    std::printf("-- distribute: clamp to hi --\n");
    {
        const QVariantList r = t.distribute(vl({195, 198}), vl({10, 10}), 2, 0, 200);
        checkNear("c1 == hi-h", r.at(1).toDouble(), 195);   // 200 - 5
        checkNear("c0 == c1-12", r.at(0).toDouble(), 183);
    }

    // ── distribute: clamp to lower bound ───────────────────────────────────────
    std::printf("-- distribute: clamp to lo --\n");
    {
        const QVariantList r = t.distribute(vl({2, 5}), vl({10, 10}), 2, 0, 200);
        checkNear("c0 == lo+h", r.at(0).toDouble(), 5);     // 0 + 5
        checkNear("c1 == c0+12", r.at(1).toDouble(), 17);
    }

    // ── positionLayout: empty/absent input ⇒ empty list ────────────────────────
    std::printf("-- positionLayout: empty input --\n");
    {
        checkTrue("empty positions -> empty list",
                  t.positionLayout(QVariantList(), 0, 1000000, true, 300, 4,
                                   QStringLiteral("Arial"), 10).isEmpty());
    }

    // ── positionLayout: a lone position keeps center == tick frac position ─────
    std::printf("-- positionLayout: lone position --\n");
    {
        const QVariantList p = positionsList({ {4, 500000, 0, 0.9} });
        const QVariantList r = t.positionLayout(p, 0, 1000000, true, 300, 4,
                                                QStringLiteral("Arial"), 10);
        checkTrue("lone -> one entry", r.size() == 1);
        const QVariantMap m = r.at(0).toMap();
        const double frac      = m.value(QStringLiteral("frac")).toDouble();
        const double rawCenter = frac * 300.0;
        checkEqI("lone p", m.value(QStringLiteral("p")).toLongLong(), 4);
        checkStr("lone label", m.value(QStringLiteral("label")).toString(), "P4");
        checkNear("lone center == raw tick frac position",
                  m.value(QStringLiteral("center")).toDouble(), rawCenter);
        checkTrue("lone -> no elbow", !m.value(QStringLiteral("elbow")).toBool());
    }

    // ── positionLayout: clustered P5/P6/P7 solved to non-overlapping centers ───
    std::printf("-- positionLayout: clustered downswing positions --\n");
    {
        // Span is 1,000,000us; P5/P6/P7 sit within ~1% of the span of each other —
        // exactly the downswing clustering the solver exists to spread apart. P99
        // (out-of-range p) must be dropped; the survivors must come back sorted by
        // t_us regardless of input order.
        const QVariantList p = positionsList({
            {7, 508000, 0, 0.7},
            {99, 300000, 0, 0.5},   // out-of-range p — dropped
            {5, 500000, 0, 0.8},
            {6, 505000, 1, 0.9},
        });
        const QVariantList r = t.positionLayout(p, 0, 1000000, true, 300, 4,
                                                QStringLiteral("Arial"), 10);
        checkTrue("drops out-of-range p", r.size() == 3);
        if (r.size() == 3) {
            const QVariantMap m0 = r.at(0).toMap();
            const QVariantMap m1 = r.at(1).toMap();
            const QVariantMap m2 = r.at(2).toMap();
            checkEqI("sorted: p5 first",  m0.value(QStringLiteral("p")).toLongLong(), 5);
            checkEqI("sorted: p6 second", m1.value(QStringLiteral("p")).toLongLong(), 6);
            checkEqI("sorted: p7 third",  m2.value(QStringLiteral("p")).toLongLong(), 7);
            checkStr("p5 label", m0.value(QStringLiteral("label")).toString(), "P5");
            checkStr("p6 label", m1.value(QStringLiteral("label")).toString(), "P6");
            checkStr("p7 label", m2.value(QStringLiteral("label")).toString(), "P7");
            checkEqI("p6 source passthrough", m1.value(QStringLiteral("source")).toLongLong(), 1);

            const double c0 = m0.value(QStringLiteral("center")).toDouble();
            const double c1 = m1.value(QStringLiteral("center")).toDouble();
            const double c2 = m2.value(QStringLiteral("center")).toDouble();
            checkTrue("centers within [lo,hi]",
                      c0 >= 0.0 && c0 <= 300.0 && c1 >= 0.0 && c1 <= 300.0
                      && c2 >= 0.0 && c2 <= 300.0);
            checkTrue("centers non-overlapping (monotonic, separated)", c0 < c1 && c1 < c2);
        }
    }

    // ── positionLayout: P10 (Finish) kept, p>10 dropped ───────────────────────
    std::printf("-- positionLayout: P10 kept, out-of-range dropped --\n");
    {
        const QVariantList p = positionsList({
            {1,  100000, 0, 0.9},
            {10, 950000, 0, 0.8},   // Finish — must survive with label "P10"
            {11, 300000, 0, 0.5},   // out-of-range p — dropped
        });
        const QVariantList r = t.positionLayout(p, 0, 1000000, true, 300, 4,
                                                QStringLiteral("Arial"), 10);
        checkTrue("P10 kept, P11 dropped -> two entries", r.size() == 2);
        if (r.size() == 2) {
            checkStr("first label P1", r.at(0).toMap().value(QStringLiteral("label")).toString(), "P1");
            checkEqI("second p == 10", r.at(1).toMap().value(QStringLiteral("p")).toLongLong(), 10);
            checkStr("second label P10",
                     r.at(1).toMap().value(QStringLiteral("label")).toString(), "P10");
        }
    }

    // ── activeStation ──────────────────────────────────────────────────────────
    std::printf("-- activeStation --\n");
    {
        const QVariantList p = phases({{0, 0}, {1, 180000}, {2, 620000}, {5, 950000}});
        checkEqI("before first -> -1", t.activeStation(p, -1), -1);
        checkEqI("at 0 -> 0", t.activeStation(p, 0), 0);
        checkEqI("mid -> 1", t.activeStation(p, 500000), 1);
        checkEqI("on station -> that", t.activeStation(p, 620000), 2);
        checkEqI("after last -> last", t.activeStation(p, 2000000), 3);
        checkEqI("empty -> -1", t.activeStation(QVariantList(), 100), -1);
    }

    // ── valueAtNearest ─────────────────────────────────────────────────────────
    std::printf("-- valueAtNearest --\n");
    {
        const QVariantList tt = vl({0, 100, 200, 300});
        const QVariantList vv = vl({10, 20, 30, 40});
        checkNear("before -> first", t.valueAtNearest(tt, vv, -50), 10);
        checkNear("after  -> last",  t.valueAtNearest(tt, vv, 400), 40);
        checkNear("140 -> 20 (100 nearer)", t.valueAtNearest(tt, vv, 140), 20);
        checkNear("160 -> 30 (200 nearer)", t.valueAtNearest(tt, vv, 160), 30);
        checkNear("150 tie -> lower (20)",  t.valueAtNearest(tt, vv, 150), 20);
        checkNear("empty -> 0", t.valueAtNearest(QVariantList(), QVariantList(), 100), 0);
    }

    // ── snap ───────────────────────────────────────────────────────────────────
    std::printf("-- snap --\n");
    {
        const QVariantList p = phases({{0, 0}, {1, 180000}, {2, 620000}, {5, 950000}});
        checkEqI("within tol -> station", t.snap(p, 185000, 20000), 180000);
        checkEqI("outside tol -> unchanged", t.snap(p, 300000, 20000), 300000);
        checkEqI("near last -> last", t.snap(p, 960000, 20000), 950000);
    }

    // ── phase names ─────────────────────────────────────────────────────────────
    std::printf("-- phase names --\n");
    {
        checkStr("full 0", t.phaseFullName(0), "Address");
        checkStr("full 5", t.phaseFullName(5), "Impact");
        checkStr("full 11", t.phaseFullName(11), "Follow-through");
        checkStr("full 12", t.phaseFullName(12), "Shaft-parallel back");
        checkStr("full 13", t.phaseFullName(13), "Arm-parallel down");
        checkStr("full 14", t.phaseFullName(14), "Shaft-parallel through");
        checkStr("full oob", t.phaseFullName(15), "P15");
        // The compact tag is the COACHING P-POSITION wherever the phase is one. "SPB" and
        // "APD" were this codebase's own jargon for P2 and P5; a golfer reads the numbers.
        // Every P-position, pinned against the mapping the Phase enum documents, because a
        // silent renumbering here would relabel the axis, the chips, the brush and the
        // wrist grid all at once and every one of them would still look plausible.
        checkStr("P1  = Address",              t.phaseShortTag(0),  "P1");
        checkStr("P2  = ShaftParallelBack",    t.phaseShortTag(12), "P2");
        checkStr("P3  = MidBackswing",         t.phaseShortTag(8),  "P3");
        checkStr("P4  = Top",                  t.phaseShortTag(2),  "P4");
        checkStr("P5  = ArmParallelDown",      t.phaseShortTag(13), "P5");
        checkStr("P6  = Delivery",             t.phaseShortTag(9),  "P6");
        checkStr("P7  = Impact",               t.phaseShortTag(5),  "P7");
        checkStr("P8  = ShaftParallelThrough", t.phaseShortTag(14), "P8");
        checkStr("P9  = FollowThrough",        t.phaseShortTag(11), "P9");
        checkStr("P10 = Finish",               t.phaseShortTag(7),  "P10");

        // The five the P-system does not name carry NO tag. The mnemonics they used to
        // carry (TKW/TRN/DWN/REL/SPD) are gone by decision: a chart mixing the shared
        // vocabulary with a private one reads as neither.
        checkStr("Takeaway   untagged", t.phaseShortTag(1),  "");
        checkStr("Transition untagged", t.phaseShortTag(3),  "");
        checkStr("Downswing  untagged", t.phaseShortTag(4),  "");
        checkStr("Release    untagged", t.phaseShortTag(6),  "");
        checkStr("Max speed  untagged", t.phaseShortTag(10), "");

        // hasPositionTag is the predicate anything that PAIRS tags must ask first — the
        // difference between a "P4→P6" chip and a "→P6" one.
        checkTrue("P-position has a tag",  t.hasPositionTag(0)  && t.hasPositionTag(5));
        checkTrue("Takeaway has none",     !t.hasPositionTag(1));
        checkTrue("Max speed has none",    !t.hasPositionTag(10));
        checkTrue("out of range has none", !t.hasPositionTag(99) && !t.hasPositionTag(-1));

        checkStr("tag oob", t.phaseShortTag(99), "P99");
    }

    std::printf("\n=== %s (%d assert failures) ===\n",
                g_fail ? "FAILURES PRESENT" : "ALL ASSERTS PASS", g_fail);
    return g_fail ? 1 : 0;
}
