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

    std::printf("\n%s — %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
