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

// ChartMetrics::seriesGroups — the chart's METRICS preset combo.
//
// What is actually at risk here is not the bucketing loop, which is short, but the JOIN: the
// presets are the catalogue's `.group` field, so the manifest and the chart are now coupled, and a
// descriptor that loses its group (or a producer that emits a key the manifest never declared)
// changes what the combo offers without touching a line of chart code. That is the point of the
// design — one grouping vocabulary, not two — and it is the reason it needs a test that reads the
// REAL catalogue rather than a fixture.
//
// So: a synthetic series list, the shipped manifest, and assertions about where keys land.

#include "chart_metrics.h"

#include "../../Metrics/metric_catalogue.h"

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <cstdio>

static int g_fail = 0;

static void checkTrue(const char *label, bool ok)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_fail;
}
static void checkEqI(const char *label, long long got, long long want)
{
    const bool ok = got == want;
    std::printf("  [%s] %-42s got %5lld  want %5lld\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static void checkEqD(const char *label, double got, double want)
{
    const bool ok = std::fabs(got - want) < 1e-6;
    std::printf("  [%s] %-42s got %8.3f  want %8.3f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static void checkStr(const char *label, const QString &got, const char *want)
{
    const bool ok = got == QString::fromUtf8(want);
    std::printf("  [%s] %-42s got \"%s\"  want \"%s\"\n", ok ? "PASS" : "FAIL", label,
                qPrintable(got), want);
    if (!ok) ++g_fail;
}

// A series with a real curve of `n` samples — what the chart can draw.
static QVariantMap curve(const char *key, int n = 4)
{
    QVariantList t, v;
    for (int i = 0; i < n; ++i) { t.append(qlonglong(i) * 1000); v.append(double(i)); }
    return QVariantMap{ { QStringLiteral("key"),   QString::fromLatin1(key) },
                        { QStringLiteral("t_us"),  t },
                        { QStringLiteral("value"), v } };
}

// A setup scalar — empty curve, one phaseSample. foot_metrics/tempo_metrics/club_delivery all
// emit this shape, and the chart has nothing to plot for it.
static QVariantMap scalar(const char *key)
{
    return QVariantMap{ { QStringLiteral("key"),   QString::fromLatin1(key) },
                        { QStringLiteral("t_us"),  QVariantList{} },
                        { QStringLiteral("value"), QVariantList{} } };
}

// The group a key landed in, or "" when no group claimed it.
static QString groupOf(const QVariantList &groups, const char *key)
{
    for (const QVariant &g : groups) {
        const QVariantMap m = g.toMap();
        if (m.value(QStringLiteral("keys")).toStringList().contains(QString::fromLatin1(key)))
            return m.value(QStringLiteral("group")).toString();
    }
    return {};
}

int main()
{
    ChartMetrics cm;

    // ── An empty / unplottable swing offers no presets at all ─────────────────────
    {
        std::printf("seriesGroups — nothing to plot\n");
        checkEqI("empty list", cm.seriesGroups({}).size(), 0);
        // Scalars only: every group would be empty, so the combo is not offered. A one-sample
        // series is a point, not a trace, and the plot skips it — so it must not count either.
        const QVariantList onlyScalars{ scalar("stanceWidth"), scalar("tempoRatio"),
                                        curve("leadWristFlexExt", 1) };
        checkEqI("scalars + a 1-sample curve", cm.seriesGroups(onlyScalars).size(), 0);
    }

    // ── The join: keys land in their catalogue group ──────────────────────────────
    {
        std::printf("seriesGroups — catalogue grouping\n");
        const QVariantList series{
            curve("leadWristFlexExt"), curve("leadWristRadUln"), curve("trailWristFlexExt"),
            curve("pelvisRotation"),   curve("xFactor"),
            curve("clubheadSpeed"),    curve("handSpeed"),   curve("lagAngle"),
            curve("headSway"),
            scalar("tempoRatio"), scalar("stanceWidth"),      // must not create groups of their own
        };
        const QVariantList g = cm.seriesGroups(series);

        checkStr("lead wrist",   groupOf(g, "leadWristFlexExt"), "Wrist & forearm");
        checkStr("trail wrist",  groupOf(g, "trailWristFlexExt"), "Wrist & forearm");
        checkStr("pelvis turn",  groupOf(g, "pelvisRotation"),   "Body rotation");
        checkStr("x-factor",     groupOf(g, "xFactor"),          "Body rotation");
        checkStr("clubhead spd", groupOf(g, "clubheadSpeed"),    "Club & speed");
        // lagAngle sits with the speeds rather than in its own group — the read a coach makes.
        checkStr("lag angle",    groupOf(g, "lagAngle"),         "Club & speed");
        checkStr("head sway",    groupOf(g, "headSway"),         "Head");

        // The scalars are absent entirely — not an empty group, not an "Other".
        checkStr("tempo scalar dropped",  groupOf(g, "tempoRatio"),  "");
        checkStr("stance scalar dropped", groupOf(g, "stanceWidth"), "");

        // Four groups for nine curves, none of them larger than the ≤5 the panel can read.
        checkEqI("group count", g.size(), 4);
        long long total = 0, biggest = 0;
        for (const QVariant &v : g) {
            const long long n = v.toMap().value(QStringLiteral("keys")).toStringList().size();
            total += n;
            if (n > biggest) biggest = n;
        }
        checkEqI("every curve placed", total, 9);
        checkTrue("no group over 5", biggest <= 5);
    }

    // ── Manifest order, so the chart and the Metric Library agree ─────────────────
    {
        std::printf("seriesGroups — ordering\n");
        // Declared in the manifest as Wrist & forearm … Body rotation … Club & speed, and the
        // result must follow that regardless of the order the analyzer appended the series in.
        const QVariantList series{ curve("clubheadSpeed"), curve("pelvisRotation"),
                                   curve("leadWristFlexExt") };
        const QVariantList g = cm.seriesGroups(series);
        checkEqI("three groups", g.size(), 3);
        checkStr("first",  g.at(0).toMap().value(QStringLiteral("group")).toString(), "Wrist & forearm");
        checkStr("second", g.at(1).toMap().value(QStringLiteral("group")).toString(), "Body rotation");
        checkStr("third",  g.at(2).toMap().value(QStringLiteral("group")).toString(), "Club & speed");
    }

    // ── The split of the old "Spine & pelvis" ─────────────────────────────────────
    {
        std::printf("seriesGroups — spine/pelvis split\n");
        // Ten members made one unreadable group. Angles went one way, translations the other;
        // hipLineTilt is an angle by name but a pelvis reading in the frontal plane, so it went
        // with sway and lift, which is what it is read against.
        const QVariantList series{ curve("spineSideBend"), curve("secondaryAxisTilt"),
                                   curve("pelvisSway"), curve("pelvisLift"),
                                   curve("hipLineTilt"), curve("thoraxLateralDrift") };
        const QVariantList g = cm.seriesGroups(series);
        // Two GROUPS, plus the cross-cutting "Plumb Bob" preset, which this set trips because it
        // carries both pelvisSway and hipLineTilt. groupOf() answers with the first entry a key
        // appears in, so the group assertions below are unaffected: a preset is additive and never
        // moves a metric out of the group it is filed under.
        checkEqI("two groups plus one preset", g.size(), 3);
        checkStr("side bend",  groupOf(g, "spineSideBend"),      "Spine & tilt");
        checkStr("axis tilt",  groupOf(g, "secondaryAxisTilt"),  "Spine & tilt");
        checkStr("sway",       groupOf(g, "pelvisSway"),         "Pelvis & lateral");
        checkStr("lift",       groupOf(g, "pelvisLift"),         "Pelvis & lateral");
        checkStr("hip line",   groupOf(g, "hipLineTilt"),        "Pelvis & lateral");
        checkStr("thx drift",  groupOf(g, "thoraxLateralDrift"), "Pelvis & lateral");
        checkStr("the preset comes last",
                 g.at(g.size() - 1).toMap().value(QStringLiteral("group")).toString(), "Plumb Bob");
    }

    // ── Cross-cutting presets ────────────────────────────────────────────────────
    {
        std::printf("seriesGroups — cross-cutting presets\n");

        // A metric has one `group` and the presets above are derived from it, so a coaching read
        // that spans groups — the plumb bob is the hip centre over the stance READ WITH the tilt of
        // the hip line — could only exist by taking hipLineTilt out of the group it belongs in.
        // MetricDescriptor::presets is the additive answer, and this is what it must do.
        {
            const QVariantList series{ curve("plumbBobDistance"), curve("hipLineTilt"),
                                       curve("pelvisSway"), curve("leadWristFlexExt") };
            const QVariantList g = cm.seriesGroups(series);
            QStringList presetKeys;
            bool found = false;
            for (const QVariant &v : g) {
                const QVariantMap m = v.toMap();
                if (m.value(QStringLiteral("group")).toString() == QLatin1String("Plumb Bob")) {
                    presetKeys = m.value(QStringLiteral("keys")).toStringList();
                    found = true;
                }
            }
            checkTrue("the preset is offered", found);
            // Manifest order, the same rule the groups follow, so the chart and the Metric Library
            // sequence the same metrics the same way.
            checkStr("its members, in manifest order", presetKeys.join(','),
                     "pelvisSway,hipLineTilt,plumbBobDistance");
            // And every member still answers with its own group.
            checkStr("hip tilt keeps its group", groupOf(g, "hipLineTilt"), "Pelvis & lateral");
            checkStr("plumb bob keeps its group", groupOf(g, "plumbBobDistance"), "Pelvis & lateral");
            // The preset sits after the groups and before Other.
            checkStr("presets follow the groups",
                     g.at(g.size() - 1).toMap().value(QStringLiteral("group")).toString(),
                     "Plumb Bob");
        }

        // ⚠ ONE MEMBER IS NOT A PRESET. A preset exists to put several curves on screen together;
        // with one member it duplicates a legend chip and pads the combo with an entry that says
        // nothing its group does not. It is also how the preset disappears honestly on a swing
        // where the ball was never found — no ruler, no plumb-bob curve, and a "Plumb Bob" preset
        // holding only a hip angle would be a preset lying about its own name.
        {
            const QVariantList series{ curve("hipLineTilt"), curve("leadWristFlexExt") };
            const QVariantList g = cm.seriesGroups(series);
            bool found = false;
            for (const QVariant &v : g)
                if (v.toMap().value(QStringLiteral("group")).toString() == QLatin1String("Plumb Bob"))
                    found = true;
            checkTrue("a single member does not make a preset", !found);
            checkStr("…and the metric is still reachable in its group",
                     groupOf(g, "hipLineTilt"), "Pelvis & lateral");
        }
    }

    // ── A key the manifest never declared stays reachable ─────────────────────────
    {
        std::printf("seriesGroups — uncatalogued keys\n");
        // A series added to the pipeline before the manifest should be awkward to find, not
        // invisible: dropping it would take a real measurement off the chart silently.
        const QVariantList series{ curve("leadWristFlexExt"),
                                   curve("zzzNotAMetric"), curve("aaaAlsoNotAMetric") };
        const QVariantList g = cm.seriesGroups(series);
        checkStr("uncatalogued → Other", groupOf(g, "zzzNotAMetric"), "Other");
        checkStr("known key unaffected", groupOf(g, "leadWristFlexExt"), "Wrist & forearm");
        // Last, and internally sorted — the set it is collected from has no stable iteration
        // order, so without the sort this group's contents would vary between runs.
        const QVariantMap last = g.at(g.size() - 1).toMap();
        checkStr("Other is last", last.value(QStringLiteral("group")).toString(), "Other");
        checkStr("Other sorted", last.value(QStringLiteral("keys")).toStringList().join(','),
                 "aaaAlsoNotAMetric,zzzNotAMetric");
    }

    // ── shortUnit: one short token, so a value is longer than its unit ───────────
    {
        std::printf("shortUnit — the display token\n");
        // The four percent-of-a-body-dimension units all collapse. The denominator is what the
        // metric's own NAME carries on this panel, and the Metric Library still spells it out.
        checkStr("stance width",   cm.shortUnit(QStringLiteral("% stance width")),   "%");
        checkStr("shoulder width", cm.shortUnit(QStringLiteral("% shoulder width")), "%");
        checkStr("foot length",    cm.shortUnit(QStringLiteral("% foot length")),    "%");
        checkStr("arm length",     cm.shortUnit(QStringLiteral("% arm length")),     "%");

        // Everything else is already a token and passes through untouched. Shortening these would
        // be inventing an abbreviation nobody asked for, which is the opposite of the point.
        for (const char *u : { "°", "mph", "yd", "cm", "mm", "in", "ratio", "s", "ft", "°/s", ":1" })
            checkStr(u, cm.shortUnit(QString::fromUtf8(u)), u);

        // ⚠ THE CANONICAL UNIT IS UNCHANGED, and this is the assertion that says so. It still has
        // to match the norm's unit — the loader refuses a mismatch — and measureUnitMismatch still
        // compares it against the producer's. A display token that leaked back into the catalogue
        // would make six metrics claim the same unit as four others and grade against each other's
        // corridors.
        const pinpoint::analysis::MetricCatalogue cat = pinpoint::analysis::makeMetricCatalogue();
        const pinpoint::analysis::MetricDescriptor *d =
            cat.descriptor(QStringLiteral("pelvisSway"));
        checkTrue("the descriptor still carries the full phrase",
                  d != nullptr && d->unit == QLatin1String("% stance width"));
    }

    // ── formatValue / formatBare: ONE rule, and the only place it can be asserted ─
    {
        std::printf("formatValue / formatBare — the shared rule\n");
        // Degrees close up and carry a signed-deviation "+"; everything else takes a space and
        // does not. This used to live three times in QML — twice as a copy of itself and once as
        // a variant that concatenated with no separator, which is where "12mph" came from.
        checkStr("degrees, positive", cm.formatValue(12.4,  QStringLiteral("°")),  "+12°");
        checkStr("degrees, negative", cm.formatValue(-8.2,  QStringLiteral("°")),  "-8°");
        checkStr("percent",           cm.formatValue(12.4,  QStringLiteral("% stance width")), "12 %");
        checkStr("mph gets a space",  cm.formatValue(75.2,  QStringLiteral("mph")), "75 mph");
        checkStr("inches",            cm.formatValue(1.6,   QStringLiteral("in")),  "2 in");
        // Empty unit reads as degrees — the pre-multi-unit default, kept so an uncatalogued
        // series does not render a bare number where every neighbour carries a token.
        checkStr("no unit ⇒ degrees", cm.formatValue(3.0,   QString()),             "+3°");

        // The bare form, for a card or gutter that already names the unit. Same sign rule, so a
        // reading does not change shape between the summary card and the legend chip.
        checkStr("bare degrees",      cm.formatBare(12.4,   QStringLiteral("°")),  "+12");
        checkStr("bare percent",      cm.formatBare(12.4,   QStringLiteral("% stance width")), "12");
        checkStr("bare negative",     cm.formatBare(-8.2,   QStringLiteral("°")),  "-8");
        // ⚠ The "+" is degrees-ONLY and deliberately not generalised: degrees here are signed
        // deviations from a reference posture, where the sign IS the reading. "+75 mph" would be
        // decoration on a quantity whose sign nobody is asking about.
        checkStr("no + on a speed",   cm.formatBare(75.2,   QStringLiteral("mph")), "75");
    }

    // ── shortLabel reads the manifest, so a new metric is short-named on arrival ──
    {
        std::printf("shortLabel — served from the catalogue\n");
        // This was a hand-maintained QHash of seven keys duplicating the descriptors' own
        // shortLabel, and it went stale the moment Phase F added four wrist metrics: they had
        // short names authored in the manifest and still drew their full labels in the chart's
        // split-mode gutter. The four that exposed it, pinned here.
        checkStr("forearm rotation",  cm.shortLabel(QStringLiteral("forearmRotation")),     "Rotation");
        checkStr("hm bow/cup",        cm.shortLabel(QStringLiteral("hm.leadWristFlexExt")), "Bow/cup (HM)");
        checkStr("hm hinge",          cm.shortLabel(QStringLiteral("hm.leadWristRadUln")),  "Hinge (HM)");
        checkStr("hm rotation",       cm.shortLabel(QStringLiteral("hm.forearmRotation")),  "Rotation (HM)");

        // The seven the old table carried, unchanged — this was a de-duplication and must not
        // have moved a single word the chart already displayed.
        checkStr("bow/cup",     cm.shortLabel(QStringLiteral("leadWristFlexExt")), "Bow/cup");
        checkStr("hinge",       cm.shortLabel(QStringLiteral("leadWristRadUln")),  "Hinge");
        checkStr("roll",        cm.shortLabel(QStringLiteral("forearmPronation")), "Roll");
        checkStr("elbow",       cm.shortLabel(QStringLiteral("leadArmFlexion")),   "Elbow");
        checkStr("club speed",  cm.shortLabel(QStringLiteral("clubheadSpeed")),    "Club speed");
        checkStr("hand speed",  cm.shortLabel(QStringLiteral("handSpeed")),        "Hand speed");
        checkStr("lag",         cm.shortLabel(QStringLiteral("lagAngle")),         "Lag");

        // An uncatalogued key still returns "" — PpMetricChart._name falls back to series.label,
        // and a series the manifest has never heard of must keep the name its producer gave it.
        checkStr("uncatalogued → \"\"", cm.shortLabel(QStringLiteral("zzzNotAMetric")), "");
    }

    // ── summaryMasked: an invalid sample is not a measurement ─────────────────────
    //
    // The fixture is the shape of the defect this exists to stop: five samples of a slow, tidy
    // curve with ONE absurd value in the middle, marked invalid because it was bridged across a
    // gated run (the body line was foreshortened there and its tilt was measuring the camera).
    // Unmasked it owns the peak, the range AND the peak rate — the three tiles the screenshot
    // that started this design got wrong.
    {
        std::printf("summaryMasked — invalid samples are skipped\n");
        const QVariantList t{ qlonglong(0), qlonglong(1000), qlonglong(2000),
                              qlonglong(3000), qlonglong(4000) };
        const QVariantList v{ 1.0, 2.0, 99.0, 3.0, 4.0 };
        const QVariantList mask{ 1, 1, 0, 1, 1 };

        // Baseline: what the old code did, and still does with nothing marked.
        const QVariantMap bare = cm.summary(t, v, 0, 4000);
        checkEqD("unmasked peak is the outlier", bare.value(QStringLiteral("peak")).toDouble(), 99.0);
        checkEqD("unmasked rate is its slope",   bare.value(QStringLiteral("rate")).toDouble(), 9700.0);
        checkTrue("unmasked is not partial",    !bare.value(QStringLiteral("partial")).toBool());

        const QVariantMap m = cm.summaryMasked(t, v, mask, 0, 4000);
        checkEqD("min over valid only",  m.value(QStringLiteral("min")).toDouble(),   1.0);
        checkEqD("max over valid only",  m.value(QStringLiteral("max")).toDouble(),   4.0);
        checkEqD("peak over valid only", m.value(QStringLiteral("peak")).toDouble(),  4.0);
        checkEqD("range over valid only", m.value(QStringLiteral("range")).toDouble(), 3.0);
        // The rate is taken between consecutive VALID samples, so the 2→3 step spans the bridged
        // sample's 2 ms and reads 50/100 ms rather than the outlier's 9700. Crossing a gap makes a
        // rate SMALLER, which is the only safe direction: a gap must not invent the steepest slope.
        checkEqD("rate over valid only", m.value(QStringLiteral("rate")).toDouble(), 100.0);
        checkTrue("a bridged sample in the window ⇒ partial",
                  m.value(QStringLiteral("partial")).toBool());
    }

    // ── partial: the window's numbers do not rest on a continuous measurement ─────
    {
        std::printf("summaryMasked — partial\n");
        const QVariantList t{ qlonglong(0), qlonglong(1000), qlonglong(2000),
                              qlonglong(3000), qlonglong(4000) };
        const QVariantList v{ 1.0, 2.0, 99.0, 3.0, 4.0 };
        const QVariantList mask{ 1, 1, 0, 1, 1 };

        // An EDGE landing on the invalid sample. The edge value is interpolated from the valid
        // samples either side (2 at 1 ms, 3 at 3 ms ⇒ 2.5), which is the one place this reducer
        // may cross a bridged run — and it says so rather than presenting 2.5 as a reading.
        const QVariantMap edge = cm.summaryMasked(t, v, mask, 2000, 4000);
        checkEqD("edge read from the valid neighbours",
                 edge.value(QStringLiteral("start")).toDouble(), 2.5);
        checkTrue("edge on an invalid sample ⇒ partial",
                  edge.value(QStringLiteral("partial")).toBool());

        // The case the "invalid sample inside the window" rule alone would MISS: a window that
        // sits entirely between two samples, inside the bridged run, and so contains no sample at
        // all. Every number in it came from across the gap.
        const QVariantMap inside = cm.summaryMasked(t, v, mask, 1200, 1800);
        checkTrue("window inside a bridged run ⇒ partial",
                  inside.value(QStringLiteral("partial")).toBool());

        // …and a window with valid samples on both sides of nothing marked is NOT partial, so the
        // chip stays off on the swings that have nothing to declare.
        const QVariantMap clean = cm.summaryMasked(t, v, mask, 3000, 4000);
        checkTrue("a fully valid window is not partial",
                  !clean.value(QStringLiteral("partial")).toBool());
    }

    // ── An empty mask IS "every sample valid" ─────────────────────────────────────
    //
    // summary() delegates to summaryMasked() with {}, so this pins the delegation as an identity:
    // every series that predates the validity field, which is every series in every swing.json
    // written before 2026-09-04, must summarise to exactly the numbers it did before.
    {
        std::printf("summaryMasked — empty mask ≡ summary()\n");
        const QVariantList t{ qlonglong(0), qlonglong(1000), qlonglong(2000),
                              qlonglong(3000), qlonglong(4000) };
        const QVariantList v{ 1.0, 2.0, 99.0, 3.0, 4.0 };
        const QVariantMap a = cm.summary(t, v, 500, 3500);
        const QVariantMap b = cm.summaryMasked(t, v, QVariantList{}, 500, 3500);
        checkTrue("same key set", a.keys() == b.keys());
        bool same = true;
        for (auto it = a.constBegin(); it != a.constEnd(); ++it)
            if (it.value() != b.value(it.key())) same = false;
        checkTrue("identical for every key", same);

        // An all-ONES mask is the same thing said the long way — the persistence layer never
        // writes one, but a caller that builds a mask by hand must not get a different answer.
        const QVariantMap ones = cm.summaryMasked(t, v, QVariantList{ 1, 1, 1, 1, 1 }, 500, 3500);
        checkEqD("all-ones peak",  ones.value(QStringLiteral("peak")).toDouble(),
                                   a.value(QStringLiteral("peak")).toDouble());
        checkTrue("all-ones is not partial", !ones.value(QStringLiteral("partial")).toBool());
    }

    // ── domainFor: the manifest says where a metric means something ───────────────
    //
    // Reads the REAL catalogue for the same reason seriesGroups' test does: the domain is authored
    // in metric_catalogue_manifest.cpp and the chart is one of three consumers of it (the pack
    // validator and the diagnostics engine are the others). A domain quietly dropped from a
    // descriptor would silently un-dim two thirds of a curve, and nothing else would notice.
    {
        std::printf("domainFor — the metric's phase domain\n");
        const QVariantMap hip = cm.domainFor(QStringLiteral("hipLineTilt"));
        // Address(0) → Impact(5): past impact the pelvis has TURNED, so the tilt of the hip line
        // in the image plane is a reading of rotation, not of the frontal-plane geometry the
        // metric is defined as. See design §5.1's table.
        checkEqI("hip tilt first phase", hip.value(QStringLiteral("firstPhase")).toInt(), 0);
        checkEqI("hip tilt last phase",  hip.value(QStringLiteral("lastPhase")).toInt(),  5);

        // comOverLeadFoot is the counter-case in the same table — it is READ at the finish, so it
        // must NOT have been swept into the Address→Impact batch.
        const QVariantMap com = cm.domainFor(QStringLiteral("comOverLeadFoot"));
        checkEqI("com over lead foot last phase",
                 com.value(QStringLiteral("lastPhase")).toInt(), 7);

        // An uncatalogued key gets the descriptor DEFAULT — the whole swing, Address(0)→Finish(7).
        // Not knowing a metric is not a licence to hide part of its curve.
        const QVariantMap unknown = cm.domainFor(QStringLiteral("zzzNotAMetric"));
        checkEqI("unknown key first phase", unknown.value(QStringLiteral("firstPhase")).toInt(), 0);
        checkEqI("unknown key last phase",  unknown.value(QStringLiteral("lastPhase")).toInt(),  7);

        // ── THE NARROWED FLAGS, PER SIDE ────────────────────────────────────────────────────
        //
        // This is the flag that stops the domain clamp firing on metrics it was never meant for,
        // and the bug it prevents is invisible in a screenshot. The chart's AXIS is the PADDED
        // swing (Segmentation swingStart/End ± boundPadUs = 250 ms), so Address is 250 ms inside
        // the axis start and Finish 250 ms inside its end. A caller that clipped to the DEFAULT
        // Address..Finish domain would dash a quarter-second off both ends of every whole-swing
        // metric and move its Full-window PEAK/Δ/RATE on every swing — breaking the property this
        // phase rests on, that nothing changes where the design does not fire. Per side, because
        // hipLineTilt narrows only its END and its leading pre-address samples must be untouched.
        checkTrue("headSway not narrowed",
                  !cm.domainFor(QStringLiteral("headSway")).value(QStringLiteral("narrowed")).toBool());
        checkTrue("headSway neither side",
                  !cm.domainFor(QStringLiteral("headSway")).value(QStringLiteral("firstNarrowed")).toBool()
                  && !cm.domainFor(QStringLiteral("headSway")).value(QStringLiteral("lastNarrowed")).toBool());
        // The same for a club metric and an uncatalogued key — the whole-swing majority.
        checkTrue("clubheadSpeed not narrowed",
                  !cm.domainFor(QStringLiteral("clubheadSpeed")).value(QStringLiteral("narrowed")).toBool());
        checkTrue("unknown key not narrowed",
                  !unknown.value(QStringLiteral("narrowed")).toBool());

        // hipLineTilt: narrowed, and on the LAST side only. Its domain first phase IS Address, so
        // a whole-domain "narrowed" flag would have clipped its start as well.
        checkTrue("hip tilt narrowed",       hip.value(QStringLiteral("narrowed")).toBool());
        checkTrue("hip tilt last side only", hip.value(QStringLiteral("lastNarrowed")).toBool()
                                             && !hip.value(QStringLiteral("firstNarrowed")).toBool());
        // comOverLeadFoot is in the same family but deliberately NOT narrowed — it is read at the
        // finish. Pinned so a future sweep of the frontal-plane batch cannot take it along.
        checkTrue("com over lead foot not narrowed",
                  !com.value(QStringLiteral("narrowed")).toBool());
    }

    // ── The short-mask rule: a malformed mask is no mask, not half a mask ──────────
    //
    // Pinned because THREE places consult validity — this class, PpChartPlot/PpSegmentBrush's JS
    // index form, and measure_sample.cpp's buildPhaseGrid — and a truncated array is the one input
    // on which they could each plausibly do something different. Guessing which end a mask was
    // truncated from would invent validity nobody stated; the shared answer is to discard it.
    {
        std::printf("summaryMasked / measuredAt — the short-mask rule\n");
        const QVariantList t{ qlonglong(0), qlonglong(1000), qlonglong(2000),
                              qlonglong(3000), qlonglong(4000) };
        const QVariantList v{ 1.0, 2.0, 99.0, 3.0, 4.0 };
        const QVariantList shortMask{ 1, 1, 0 };          // covers 3 of 5 — malformed

        const QVariantMap full  = cm.summary(t, v, 0, 4000);
        const QVariantMap trunc = cm.summaryMasked(t, v, shortMask, 0, 4000);
        checkEqD("short mask ⇒ no mask (peak)", trunc.value(QStringLiteral("peak")).toDouble(),
                                                full.value(QStringLiteral("peak")).toDouble());
        checkEqD("short mask ⇒ no mask (rate)", trunc.value(QStringLiteral("rate")).toDouble(),
                                                full.value(QStringLiteral("rate")).toDouble());
        checkTrue("short mask ⇒ not partial", !trunc.value(QStringLiteral("partial")).toBool());
        // measuredAt agrees, rather than bounding its scan at qMin(sizes) and answering a
        // different question than the reducer did about the very same series.
        checkTrue("short mask ⇒ measuredAt says measured", cm.measuredAt(t, shortMask, 2000, 0, 0));
        // A mask LONGER than the curve is still honoured — over-length is not truncation.
        const QVariantList longMask{ 1, 1, 0, 1, 1, 1, 1 };
        checkEqD("over-length mask still masks",
                 cm.summaryMasked(t, v, longMask, 0, 4000).value(QStringLiteral("peak")).toDouble(), 4.0);
    }

    // ── bandAtNearest: no verdict rather than an invented pass ─────────────────────
    //
    // "nearest" over a whole swing means the Address sample is 900 ms away and still nearest, and
    // the old code additionally defaulted an EMPTY list to "good". Between them, a series whose
    // producer emitted no sample at impact — which is exactly what §5.1's gating arranges — had
    // its @impact reading tinted PASS GREEN off nothing at all.
    {
        std::printf("bandAtNearest — a missing verdict is not a good one\n");
        auto sample = [](qlonglong tUs, const char *band) {
            return QVariant(QVariantMap{ { QStringLiteral("t_us"), tUs },
                                         { QStringLiteral("value"), 0.0 },
                                         { QStringLiteral("band"), QString::fromLatin1(band) } });
        };
        checkStr("empty list ⇒ no verdict", cm.bandAtNearest({}, 3000000), "");
        // 900 ms away: nearest, and irrelevant. This is the case that tinted cards green.
        const QVariantList far{ sample(2100000, "good") };
        checkStr("far sample ⇒ no verdict", cm.bandAtNearest(far, 3000000), "");
        // Within a frame of the instant asked about ⇒ the verdict stands, unchanged.
        const QVariantList near{ sample(2995000, "warn"), sample(2100000, "good") };
        checkStr("near sample ⇒ its band", cm.bandAtNearest(near, 3000000), "warn");
        // Exactly on it, and an unscored producer's empty band still reads as no verdict — which
        // is what it is: the face-on batch is UNSCORED and stamps band "".
        const QVariantList unscored{ sample(3000000, "") };
        checkStr("unscored sample ⇒ no verdict", cm.bandAtNearest(unscored, 3000000), "");
    }

    // ── measuredAt: one predicate for the dashes, the dots and the "—" ────────────
    {
        std::printf("measuredAt — was there a reading here\n");
        const QVariantList t{ qlonglong(0), qlonglong(1000), qlonglong(2000),
                              qlonglong(3000), qlonglong(4000) };
        const QVariantList mask{ 1, 1, 0, 1, 1 };

        // No mask and no domain (fromUs == toUs disables the domain test) ⇒ everything measured,
        // which is how every pre-validity swing must still render.
        checkTrue("no mask ⇒ measured", cm.measuredAt(t, QVariantList{}, 2000, 0, 0));
        checkTrue("bridged sample ⇒ not measured", !cm.measuredAt(t, mask, 2000, 0, 0));
        checkTrue("valid sample ⇒ measured",        cm.measuredAt(t, mask, 3000, 0, 0));
        // Nearest, not exact: a crosshair between samples takes the nearer one's verdict.
        checkTrue("nearest is the bridged one",    !cm.measuredAt(t, mask, 2100, 0, 0));
        // Outside the domain nothing is measured, mask or no mask.
        checkTrue("past the domain ⇒ not measured", !cm.measuredAt(t, QVariantList{}, 3000, 0, 2000));
        checkTrue("inside the domain ⇒ measured",    cm.measuredAt(t, QVariantList{}, 1000, 0, 2000));
    }

    std::printf("\n%s — %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
