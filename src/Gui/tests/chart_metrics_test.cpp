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
//
// AND, since Phase 2 of docs/design/metric_presentation_honesty.md, the summary reducers — which
// is now the larger half of this file. ChartMetrics::summaryMasked no longer computes anything
// itself: it marshals the QML bridge's arrays into a src/Analysis/series_reduce.h SeriesView and
// delegates to the four shared reducers the diagnostics engine grades with. What is at risk there
// is therefore not arithmetic in this class but the CONTRACT — which reducer answers which key,
// what happens when one of them cannot answer at all, and whether an invalid sample is really
// absent rather than merely down-weighted. The fixtures below are sized in real TIME (8 ms and
// 27 ms sampling) because every one of those reducers is defined by a time window; the pre-Phase-2
// fixtures were 1 ms apart and shorter end to end than the smallest window in the design.

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

// ── Fixtures for the reducer suite (Phase 2, design §5.2) ─────────────────────────────────────
//
// ⚠ REAL SAMPLING, AND IT IS NOT COSMETIC. The Phase 2 reducers are defined in TIME — a 40 ms
// centred extremum window, a 50 ms minimum rate window — so the fixtures these assertions grew
// from, five samples 1 ms apart, describe a curve 4 ms long: shorter than every window in the
// design, with every reduction degenerate (one extremum window covering the whole series, no rate
// window qualifying at all). They are rescaled here to the pipeline's own cadence, 8 ms per
// sample, which is the ~120 fps the corpus was shot at and the spacing design §5.2's window sizes
// were chosen against ("≈5 samples dense, ≈2 sparse").
static constexpr qlonglong kDtUs = 8000;

// t_us for `n` samples at `dtUs`, ascending from 0.
static QVariantList tAt(int n, qlonglong dtUs = kDtUs)
{
    QVariantList t;
    for (int i = 0; i < n; ++i) t.append(qlonglong(i) * dtUs);
    return t;
}
static QVariantList flatV(int n, double v)
{
    QVariantList out;
    for (int i = 0; i < n; ++i) out.append(v);
    return out;
}
// v0 + i*step — a clean ramp, whose slope and endpoints the reducers must reproduce exactly.
static QVariantList rampV(int n, double v0, double step)
{
    QVariantList out;
    for (int i = 0; i < n; ++i) out.append(v0 + step * i);
    return out;
}
static QVariantList onesMask(int n, int zeroFrom = -1, int zeroTo = -1)
{
    QVariantList out;
    for (int i = 0; i < n; ++i)
        out.append((zeroFrom >= 0 && i >= zeroFrom && i <= zeroTo) ? 0 : 1);
    return out;
}

// A "still" series: fixed-seed pseudo-noise of the given sd, uniform on ±sd·√3 (a uniform's sd is
// its half-range / √3). An address hold is exactly this — nothing is moving, so every wobble in it
// belongs to the pipeline — and design §7 item 2 is measured on such a window.
//
// FIXED SEED, deliberately: a test whose noise changes per run cannot be debugged, and a tolerance
// wide enough to cover an unseeded generator has stopped testing anything.
static QVariantList noiseV(int n, double sd)
{
    QVariantList out;
    unsigned long long x = 0x2545F4914F6CDD1Dull;          // any constant; this one is arbitrary
    for (int i = 0; i < n; ++i) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        const double u = double((x >> 33) & 0x7FFFFFFFull) / double(0x7FFFFFFFull);   // [0,1]
        out.append(sd * std::sqrt(3.0) * (2.0 * u - 1.0));
    }
    return out;
}

static double meanOf(const QVariantList &v)
{
    double s = 0.0;
    for (const QVariant &x : v) s += x.toDouble();
    return v.isEmpty() ? 0.0 : s / v.size();
}
static double sdOf(const QVariantList &v)
{
    if (v.size() < 2) return 0.0;
    const double m = meanOf(v);
    double s = 0.0;
    for (const QVariant &x : v) { const double d = x.toDouble() - m; s += d * d; }
    return std::sqrt(s / (v.size() - 1));
}

// ── The OLD reducers, kept HERE and nowhere else ──────────────────────────────────────────────
//
// Several assertions below are of the form "the new number is far below the old one", and that is
// the only form in which the point of Phase 2 can be pinned: a literal would rot the moment a
// fixture changed, and would say nothing about which definition produced it. So the two
// definitions chart_metrics.cpp deleted live on as test scaffolding — max adjacent |Δv/Δt| per
// 100 ms, and the raw argmax of |v| — and the suite asserts the ratio between them and the
// windowed forms that replaced them.
static double oldRate(const QVariantList &t, const QVariantList &v)
{
    double r = 0.0;
    for (int i = 1; i < t.size() && i < v.size(); ++i) {
        const double dt = double(t.at(i).toLongLong() - t.at(i - 1).toLongLong()) / 1.0e5;
        if (dt <= 0.0) continue;
        const double d = std::fabs(v.at(i).toDouble() - v.at(i - 1).toDouble()) / dt;
        if (d > r) r = d;
    }
    return r;
}
static double oldPeak(const QVariantList &v)
{
    double p = 0.0;
    for (const QVariant &x : v)
        if (std::fabs(x.toDouble()) > std::fabs(p)) p = x.toDouble();
    return p;
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

    // ── The spike fixture, under the Phase 2 reducers ──────────────────────────────
    //
    // The fixture is the shape of the defect this design exists to stop: a still, tidy curve of
    // ~4s with ONE absurd value in the middle, of the kind a foreshortened body line produces.
    // In Phase 1 the only thing that could stop it owning PEAK, RANGE and PK RATE was marking
    // that sample invalid. Phase 2's job is that it can no longer own them EVEN WHEN NOBODY MARKS
    // ANYTHING, because the reducers are windowed — which is what makes the honesty a property of
    // the maths rather than of the gate happening to fire.
    {
        std::printf("summaryMasked — the spike, unmasked (Phase 2 reducers)\n");
        const int  n      = 25;                        // 25 × 8 ms = 192 ms of curve
        const int  iSpike = 12;                        // t = 96 ms, clear of both edges
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[iSpike] = 99.0;
        const qlonglong tEnd = qlonglong(n - 1) * kDtUs;

        const QVariantMap bare = cm.summary(t, v, 0, tEnd);

        // ⚠ REPLACES "unmasked peak is the outlier" (which asserted peak == 99.0 exactly).
        // WHICH DEFINITION CHANGED: peak is the extremum of the 40 ms CENTRED-WINDOW MEAN, not
        // the raw argmax — a value has to hold for 40 ms to be a peak (design §5.2).
        // THE ARITHMETIC: at 8 ms sampling a ±20 ms window holds 5 samples, and every window that
        // contains the spike contains four baseline samples with it, so the best any of them can
        // mean is (4·4 + 99)/5 = 23.0. The bound is baseline + (99 − baseline)/5 = 4 + 95/5 = 23;
        // the tolerance is for a reducer whose window boundary is inclusive at one end, which can
        // only add baseline samples and read LOWER.
        const double peakBound = 4.0 + 95.0 / 5.0;
        checkTrue("unmasked peak is under the 40 ms bound",
                  bare.value(QStringLiteral("peak")).toDouble() <= peakBound + 1e-6);
        // …and it is still well above the baseline. The excursion is REAL and must not be erased,
        // only refused the right to be a 99: design §7 item 3 in miniature.
        checkTrue("…and still shows the excursion",
                  bare.value(QStringLiteral("peak")).toDouble() > 4.0 + 1e-6);
        checkEqD("the argmax it replaced, for scale", oldPeak(v), 99.0);
        // range follows peak: it was 95 (99 − 4) and cannot be more than 19 now.
        checkTrue("range is the windowed one too",
                  bare.value(QStringLiteral("range")).toDouble() <= peakBound - 4.0 + 1e-6);

        // ⚠ REPLACES "unmasked rate is its slope" (which asserted rate == 9700.0 — 97 units
        // across one 1 ms frame of the old fixture).
        // WHICH DEFINITION CHANGED: rate is the steepest LEAST-SQUARES slope over a window of at
        // least 50 ms holding at least 3 valid samples, so no single sample interval can set it.
        // THE ARITHMETIC: at 8 ms the qualifying window is 8 samples (the 7 inside 50 ms, extended
        // to the first sample at or past 50 ms so the span reaches it — 56 ms). For a flat
        // baseline plus one 95-unit outlier at offset x_j the fitted slope is
        // 95·(x_j − x̄)/Σ(x − x̄)², largest when the spike sits at an END of the window:
        // 95·28/2688 per ms = 0.99 per ms = 99 per 100 ms. A 7-sample window would give
        // 95·24/1792 = 127. Either is under 200, and 10–12× below what the same shape used to
        // report through the adjacent-frame form (95/0.08 = 1187 per 100 ms at this spacing).
        const double rate = bare.value(QStringLiteral("rate")).toDouble();
        checkTrue("unmasked rate is a fitted slope, not a frame difference",
                  std::fabs(rate) < 200.0);
        // ⚠ ON |rate|, NOT rate, and that is not laziness: the slope is SIGNED now, and the window
        // with the spike at its START (the curve falling away from it) fits exactly as steeply as
        // the one with it at the END. Which of the two ties wins is not something C8 promises, so
        // a signed assertion here would be pinning an implementation detail.
        checkTrue("…and the spike is still visible in it", std::fabs(rate) > 20.0);
        checkTrue("…many times below the adjacent-frame definition",
                  std::fabs(rate) < oldRate(t, v) / 5.0);
        checkTrue("a rate WAS fitted here", bare.value(QStringLiteral("rateOk")).toBool());
        checkTrue("unmasked is not partial", !bare.value(QStringLiteral("partial")).toBool());
    }

    // ── The same spike, MASKED, is not a measurement at all ───────────────────────
    {
        std::printf("summaryMasked — the spike masked ≡ the clean curve\n");
        const int n = 25, iSpike = 12;
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[iSpike] = 99.0;
        const qlonglong tEnd = qlonglong(n - 1) * kDtUs;

        const QVariantMap m     = cm.summaryMasked(t, v, onesMask(n, iSpike, iSpike), 0, tEnd);
        const QVariantMap clean = cm.summary(t, flatV(n, 4.0), 0, tEnd);

        // Every VALUE the two report is identical. A flat curve's windowed mean, windowed median
        // and least-squares slope do not care whether a sample equal to the baseline was present
        // or dropped — so this is the strongest available statement of "an invalid sample is not
        // a measurement": not down-weighted, ABSENT.
        //
        // tPeakUs / tRateUs are deliberately not compared: on a flat curve every window ties, so
        // which instant wins is an implementation detail. `partial` must DIFFER, and does below.
        for (const char *k : { "start", "end", "min", "max", "peak", "range", "delta",
                               "rate", "peakSigma", "rateSigma" })
            checkEqD(k, m.value(QLatin1String(k)).toDouble(),
                        clean.value(QLatin1String(k)).toDouble());

        // ⚠ REPLACES "peak over valid only == 4.0" and "rate over valid only == 100.0". The peak
        // is unchanged in VALUE but not in definition (it is now the 4.0 baseline as a windowed
        // mean, not as an argmax); the rate assertion was 100 per 100 ms because the old form
        // took the step ACROSS the bridged sample and divided by its 2 ms. A ≥50 ms fit over a
        // flat baseline is exactly 0 — there is nothing there to have a slope.
        checkEqD("masked peak is the baseline", m.value(QStringLiteral("peak")).toDouble(), 4.0);
        checkEqD("masked rate is flat",        m.value(QStringLiteral("rate")).toDouble(),  0.0);
        checkTrue("…and it was FITTED, not withheld", m.value(QStringLiteral("rateOk")).toBool());
        checkTrue("a bridged sample in the window ⇒ partial",
                  m.value(QStringLiteral("partial")).toBool());
        checkTrue("…while the clean curve declares nothing",
                  !clean.value(QStringLiteral("partial")).toBool());
    }

    // ── A still series: whatever the rate reads over it IS the noise floor ────────
    //
    // Design §7 item 2's window, in synthetic form: the golfer has not moved, so nothing in this
    // curve is a measurement of the athlete. The corpus baseline read 39 (% stance width) and 291
    // (°) per 100 ms over such a window with the adjacent-frame definition.
    {
        std::printf("summaryMasked — a still (noisy) series\n");
        const int    n  = 51;                          // 51 × 8 ms = 400 ms of holding still
        const double sd = 1.0;
        const QVariantList t = tAt(n);
        const QVariantList v = noiseV(n, sd);
        const QVariantMap  s = cm.summary(t, v, 0, qlonglong(n - 1) * kDtUs);

        const double mean   = meanOf(v);
        const double realSd = sdOf(v);
        const double peak   = s.value(QStringLiteral("peak")).toDouble();
        const double rate   = s.value(QStringLiteral("rate")).toDouble();

        // PEAK WITHIN ONE σ OF THE MEAN — design §7 item 3. A 5-sample window mean has standard
        // error σ/√5 = 0.45σ, and the largest of the ~45 overlapping ones (about ten of them
        // independent) sits near 1.6 of those, ≈0.7σ. The raw argmax of the same samples is
        // ~2.5σ out, and that is the number the tile used to print.
        checkTrue("peak is within 1σ of the mean", std::fabs(peak - mean) < realSd);
        checkTrue("…and inside the argmax it replaced",
                  std::fabs(peak - mean) < std::fabs(oldPeak(v) - mean));

        // THE RATE BOUND, AND WHY IT IS NOT 2. σ_slope = σ/√Σ(x−x̄)²; over the 8 samples of a
        // 56 ms window at 8 ms spacing Σ(x−x̄)² = 2688 ms², so the FIT'S OWN standard error is
        // σ·1.93 per 100 ms — 1.93 at σ = 1. The largest of ~40 overlapping windows (≈14 of them
        // independent) therefore lands near 2 of those, ≈4 per 100 ms. "Under 2 per 100 ms"
        // (design §7 item 2) is consequently NOT a property of the reducer at σ = 1: it is a
        // property of a series whose residual σ is ≲ 0.5, which is what Phase 1's gating and
        // Phase 4's longer smoother window are for. What the reducer does own, and what is
        // asserted here, is that the steepest slope in a still series is inside its own fitted
        // uncertainty and an order of magnitude below the adjacent-frame form.
        const double rateSigma = s.value(QStringLiteral("rateSigma")).toDouble();
        checkTrue("|rate| < 8 per 100 ms on σ=1 pseudo-noise", std::fabs(rate) < 8.0);
        checkTrue("…and not distinguishable from 0 at any usable threshold",
                  std::fabs(rate) < 6.0 * rateSigma);
        checkTrue("…and ≥5× below the adjacent-frame definition",
                  std::fabs(rate) < oldRate(t, v) / 5.0);
        checkTrue("rateSigma is non-zero on a noisy fit", rateSigma > 0.0);
        // The σ of the winning extremum window is reported too, and on noise it is of the order
        // of the series' own σ. This is the number design §5.3 puts a "±" in front of on PEAK,
        // and the reason a reader can tell that a 0.7σ excursion is not a finding.
        const double peakSigma = s.value(QStringLiteral("peakSigma")).toDouble();
        checkTrue("peakSigma is of the order of σ",
                  peakSigma > 0.2 * realSd && peakSigma < 3.0 * realSd);
    }

    // ── A clean ramp: the windowed forms must reproduce it EXACTLY ────────────────
    //
    // The counter-case to every assertion above, and the one that proves the reducers are not
    // smoothers: on a straight line a symmetric window's mean IS the value at its centre and a
    // least-squares fit IS the line, so the new definitions and the old raw ones agree to the
    // last digit. A windowed reducer that shrank a real excursion would fail here.
    {
        std::printf("summaryMasked — a clean ramp\n");
        const int n = 51;                              // 400 ms
        const QVariantList t = tAt(n);
        const QVariantList v = rampV(n, 10.0, 2.0);    // 2 per 8 ms = 25 per 100 ms
        // The window sits ON two samples and 100 ms inside both ends of the series, so every
        // window a reducer opens is symmetric and interior. An edge closer than 15 ms to the first
        // sample would take its median from a one-sided window and read high — a real property of
        // the reducer, but not the one this case pins.
        const qlonglong a = 13 * kDtUs;                // 104 ms, value 36
        const qlonglong b = 37 * kDtUs;                // 296 ms, value 84
        const QVariantMap s = cm.summary(t, v, a, b);

        checkEqD("start = the ramp at the edge", s.value(QStringLiteral("start")).toDouble(), 36.0);
        checkEqD("end   = the ramp at the edge", s.value(QStringLiteral("end")).toDouble(),   84.0);
        checkEqD("delta = the ramp's rise",      s.value(QStringLiteral("delta")).toDouble(), 48.0);
        checkEqD("min = the ramp at the start",  s.value(QStringLiteral("min")).toDouble(),   36.0);
        checkEqD("max = the ramp at the end",    s.value(QStringLiteral("max")).toDouble(),   84.0);
        checkEqD("peak = the larger magnitude",  s.value(QStringLiteral("peak")).toDouble(),  84.0);
        checkEqD("range",                        s.value(QStringLiteral("range")).toDouble(), 48.0);
        checkEqI("tPeakUs = the winning sample",
                 s.value(QStringLiteral("tPeakUs")).toLongLong(), b);
        // ⚠ REPLACES the old adjacent-difference rate expectation on a monotone fixture. On an
        // exact line every window fits perfectly, so the slope is the ramp's own and its standard
        // error is 0 — the least-squares form's own sanity check.
        checkEqD("rate = the ramp's slope per 100 ms",
                 s.value(QStringLiteral("rate")).toDouble(), 25.0);
        checkEqD("rateSigma = 0 on an exact fit",
                 s.value(QStringLiteral("rateSigma")).toDouble(), 0.0);
        checkTrue("rateOk",      s.value(QStringLiteral("rateOk")).toBool());
        checkTrue("not partial", !s.value(QStringLiteral("partial")).toBool());
        checkTrue("tRateUs lies inside the window",
                  s.value(QStringLiteral("tRateUs")).toLongLong() >= a
                  && s.value(QStringLiteral("tRateUs")).toLongLong() <= b);
        // peakSigma on a ramp is not noise at all — it is the SLOPE across the winning window:
        // 5 samples 2 apart ⇒ deviations ±4, ±2, 0 ⇒ √10 = 3.162 (n−1) or √8 = 2.828 (n).
        // Bounded rather than pinned because that denominator is C8's choice, not this test's.
        const double ps = s.value(QStringLiteral("peakSigma")).toDouble();
        checkTrue("peakSigma is the in-window spread", ps > 2.7 && ps < 3.3);
    }

    // ── Sparse sampling, and a series too short to have a rate at all ─────────────
    {
        std::printf("summaryMasked — sparse 27 ms sampling\n");
        // 27 ms is the sparse end of the corpus (a ~37 fps clip). The rate window is 50 ms, which
        // at this spacing holds THREE samples spanning 54 ms — exactly C8's minimum, and the
        // reason the window extends to the first sample at or past 50 ms instead of stopping
        // short of it: stopping short, a sparse series would have no rate anywhere.
        const int n = 21;
        const QVariantList t = tAt(n, 27000);
        const QVariantList v = rampV(n, 0.0, 2.7);     // 2.7 per 27 ms = 10 per 100 ms
        const QVariantMap  s = cm.summary(t, v, 2 * 27000, 18 * 27000);
        checkTrue("rateOk on sparse sampling", s.value(QStringLiteral("rateOk")).toBool());
        checkEqD("rate on a 27 ms ramp", s.value(QStringLiteral("rate")).toDouble(), 10.0);
        checkEqD("start", s.value(QStringLiteral("start")).toDouble(), 5.4);
        checkEqD("end",   s.value(QStringLiteral("end")).toDouble(),  48.6);
        // At 27 ms a ±20 ms extremum window holds ONE sample — itself — so the windowed mean is
        // the sample and its σ is 0. The "always ≥ 1, itself" clause is what keeps a sparse
        // series' extremes from vanishing.
        checkEqD("min = the window's first sample", s.value(QStringLiteral("min")).toDouble(),  5.4);
        checkEqD("max = the window's last sample",  s.value(QStringLiteral("max")).toDouble(), 48.6);
        checkEqD("peakSigma = 0 on a one-sample window",
                 s.value(QStringLiteral("peakSigma")).toDouble(), 0.0);

        std::printf("summaryMasked — a series with no rate to report\n");
        // TWO samples: no window can hold three, so there is no slope to fit.
        // ⚠ THE ANSWER IS ABSENCE, NOT ZERO — rateOk false, and the card prints "—" with its
        // "/100ms" token hidden. A bare 0 would read as a still, well-behaved curve, which is the
        // confident absurdity this design exists to remove. The old definition answered this case
        // with (2 − 1)/8 ms = 12.5 per 100 ms, a rate off one frame difference.
        const QVariantList t2{ qlonglong(0), qlonglong(kDtUs) };
        const QVariantList v2{ 1.0, 2.0 };
        const QVariantMap  two = cm.summary(t2, v2, 0, kDtUs);
        checkTrue("rateOk is false", !two.value(QStringLiteral("rateOk")).toBool());
        checkEqD("rate is 0",      two.value(QStringLiteral("rate")).toDouble(),      0.0);
        checkEqD("rateSigma is 0", two.value(QStringLiteral("rateSigma")).toDouble(), 0.0);
        checkEqI("tRateUs is 0",   two.value(QStringLiteral("tRateUs")).toLongLong(), 0);
        // The rest of the card still works: two samples 8 ms apart fall in one 40 ms window, so
        // both extremes are that window's mean and Δ between two identical medians is 0. Asserted
        // as a self-consistency rather than as literals, because the median of an EVEN count is
        // C8's convention to choose.
        checkEqD("min == max inside one window",
                 two.value(QStringLiteral("min")).toDouble(),
                 two.value(QStringLiteral("max")).toDouble());
        checkEqD("delta between two equal medians is 0",
                 two.value(QStringLiteral("delta")).toDouble(), 0.0);
        checkTrue("peak lies between the two samples",
                  two.value(QStringLiteral("peak")).toDouble() >= 1.0
                  && two.value(QStringLiteral("peak")).toDouble() <= 2.0);
    }

    // ── partial: the window's numbers do not rest on a continuous measurement ─────
    {
        std::printf("summaryMasked — partial, and the fallback edge\n");
        // A 64 ms bridged run (8 samples at 8 ms) in the middle of a ramp, WIDER than the ±15 ms
        // median window on purpose: that is the case Phase 2 introduces. With no valid sample
        // near the edge there is no median to take, so the summary falls back to interpolating
        // between the nearest measurements — and says so, every time, rather than presenting the
        // interpolation as a reading.
        const int n = 25;
        const QVariantList t    = tAt(n);
        const QVariantList v    = rampV(n, 0.0, 1.0);   // value == index, so an edge is legible
        const QVariantList mask = onesMask(n, 8, 15);   // t = 64 ms … 120 ms bridged

        const QVariantMap edge = cm.summaryMasked(t, v, mask, 12 * kDtUs, 24 * kDtUs);
        // The edge at 96 ms: nearest valid samples are (56 ms, 7) and (128 ms, 16), so
        // 7 + 9·(40/72) = 12.0. The SAME number the pre-Phase-2 code produced there, deliberately:
        // when there is nothing within ±15 ms, the value between the nearest measurements is
        // still the honest one. What changed is that it is no longer reported as measured.
        checkEqD("fallback edge = interpolated from the nearest valid samples",
                 edge.value(QStringLiteral("start")).toDouble(), 12.0);
        checkTrue("an edge with no valid sample within ±15 ms ⇒ partial",
                  edge.value(QStringLiteral("partial")).toBool());

        // ⚠ REPLACES the old "window inside a bridged run ⇒ partial" case, which was caught by
        // the interpolation's bracket test (whether the two samples it read were adjacent in the
        // original series). That test is gone: the FALLBACK ITSELF is now the signal, and it
        // catches strictly more — a window wholly inside the run has no valid sample within
        // ±15 ms of either edge.
        const QVariantMap inside = cm.summaryMasked(t, v, mask, 10 * kDtUs, 13 * kDtUs);
        checkTrue("window inside a bridged run ⇒ partial",
                  inside.value(QStringLiteral("partial")).toBool());
        // …and it still reports extremes, from the two fallback edges (10 and 13 on this ramp)
        // rather than from nothing. A 0 printed there would be a reading invented out of a gap.
        checkTrue("…extremes come from the edges, not from nothing",
                  inside.value(QStringLiteral("min")).toDouble() >= 7.0
                  && inside.value(QStringLiteral("max")).toDouble() <= 16.0);

        // A window entirely in the valid tail declares nothing, so the chip stays off on the
        // swings that have nothing to declare — and the ramp's slope comes through it intact
        // (1 per 8 ms = 12.5 per 100 ms).
        const QVariantMap clean = cm.summaryMasked(t, v, mask, 17 * kDtUs, 24 * kDtUs);
        checkTrue("a fully valid window is not partial",
                  !clean.value(QStringLiteral("partial")).toBool());
        checkEqD("…and it fits the ramp's slope",
                 clean.value(QStringLiteral("rate")).toDouble(), 12.5);
    }

    // ── An empty mask IS "every sample valid" ─────────────────────────────────────
    //
    // summary() delegates to summaryMasked() with {}, so this pins the delegation as an identity:
    // every series that predates the validity field — which is every series in every swing.json
    // written before 2026-09-04 — must summarise through exactly the same code path.
    {
        std::printf("summaryMasked — empty mask ≡ summary()\n");
        const int n = 25;
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[12] = 99.0;
        const QVariantMap a = cm.summary(t, v, 5 * kDtUs, 20 * kDtUs);
        const QVariantMap b = cm.summaryMasked(t, v, QVariantList{}, 5 * kDtUs, 20 * kDtUs);
        checkTrue("same key set", a.keys() == b.keys());
        bool same = true;
        for (auto it = a.constBegin(); it != a.constEnd(); ++it)
            if (it.value() != b.value(it.key())) same = false;
        checkTrue("identical for every key", same);

        // The four keys Phase 2 adds are PRESENT, not optional. A QML binding that reads
        // `st.rateOk` against a map without it gets `undefined` — falsy — and the PK RATE tile
        // would print "—" on every card of every swing, which is exactly as wrong as printing a
        // fabricated number.
        for (const char *k : { "peakSigma", "rateSigma", "rateOk", "tRateUs" })
            checkTrue(k, a.contains(QLatin1String(k)));
        // …and nothing that was there before was lost on the way: three QML files and one probe
        // read these by name.
        for (const char *k : { "start", "end", "min", "max", "peak", "range", "delta", "rate",
                               "tPeakUs", "partial" })
            checkTrue(k, a.contains(QLatin1String(k)));

        // An all-ONES mask is the same thing said the long way — the persistence layer never
        // writes one, but a caller that builds a mask by hand must not get a different answer.
        const QVariantMap ones = cm.summaryMasked(t, v, onesMask(n), 5 * kDtUs, 20 * kDtUs);
        checkEqD("all-ones peak", ones.value(QStringLiteral("peak")).toDouble(),
                                  a.value(QStringLiteral("peak")).toDouble());
        checkEqD("all-ones rate", ones.value(QStringLiteral("rate")).toDouble(),
                                  a.value(QStringLiteral("rate")).toDouble());
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
        const int n = 25;
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[12] = 99.0;                                   // the spike, at t = 96 ms
        const qlonglong tEnd = qlonglong(n - 1) * kDtUs;
        const QVariantList shortMask = onesMask(13, 12, 12);   // covers 13 of 25 — malformed

        const QVariantMap full  = cm.summary(t, v, 0, tEnd);
        const QVariantMap trunc = cm.summaryMasked(t, v, shortMask, 0, tEnd);
        // Asserted as an IDENTITY against the unmasked summary rather than against literals, so
        // this case survives the reducer definitions changing under it — as they just did.
        checkEqD("short mask ⇒ no mask (peak)", trunc.value(QStringLiteral("peak")).toDouble(),
                                                full.value(QStringLiteral("peak")).toDouble());
        checkEqD("short mask ⇒ no mask (rate)", trunc.value(QStringLiteral("rate")).toDouble(),
                                                full.value(QStringLiteral("rate")).toDouble());
        checkTrue("short mask ⇒ not partial", !trunc.value(QStringLiteral("partial")).toBool());
        // C8 puts this rule in the CALLER's hands — SeriesView takes a mask pointer that may be
        // null — for the same reason it lives in one function here: the shared reducers must not
        // carry a second opinion about a malformed document.
        checkTrue("short mask ⇒ measuredAt says measured",
                  cm.measuredAt(t, shortMask, 12 * kDtUs, 0, 0));
        // A mask LONGER than the curve is still honoured — over-length is not truncation — and
        // with the spike marked, the curve reduces to its baseline everywhere.
        const QVariantMap over = cm.summaryMasked(t, v, onesMask(n + 4, 12, 12), 0, tEnd);
        checkEqD("over-length mask still masks", over.value(QStringLiteral("peak")).toDouble(), 4.0);
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
