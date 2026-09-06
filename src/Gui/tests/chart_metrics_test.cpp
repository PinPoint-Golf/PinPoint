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
//
// AND, since Phase 3, the σ-GOVERNED DISPLAY — displayStep, the two formatters' step rounding, and
// the "± x" beside the PEAK and PK RATE tiles. Those are string rules and this is the only place
// they can be asserted at all: the alternative home is a QML binding, which no test can reach. Two
// of the assertions below are load-bearing beyond their own rule. One is that σ = 0 (which is what
// an ABSENT σ becomes at the display boundary, and what most series still carry) reproduces the
// pre-Phase-3 strings byte for byte, because that is what makes the whole phase safe to ship on a
// corpus whose producers have not propagated σ yet. The other is that the step is never finer than
// one unit, because a rule meant to remove false precision must not be able to manufacture it.
//
// AND, since Phase 6, ChartMetrics::windowedMean — the array the chart STROKES. Drawing anything
// other than the persisted samples is only defensible if it is the same reduction the tiles report
// (design §4 principle 1), so what is at risk is an identity rather than a value: PEAK must be an
// extremum of the drawn line inside the window, BIT-EXACTLY, on every fixture in this file. That is
// asserted by scanning the returned array — not by asking the reducer twice — with a checkExactD
// that has no epsilon, because a tolerance would pass exactly the state this design exists to end:
// two implementations of one window, drifting.

#include "chart_metrics.h"

#include "../../Metrics/metric_catalogue.h"

#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <cstdio>
#include <limits>

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
// ── BIT-EXACT, and it is a different question from checkEqD's 1e-6 ────────────────────────────
//
// Phase 6 (C17) draws the array the PEAK tile reduces, so "the tile is a point on the drawn line"
// is not an approximation to be toleranced — it is the SAME double, produced by the same code, or
// the two reductions have drifted apart and the whole claim is gone. A 1e-6 tolerance would pass a
// build where the chart re-implemented the window slightly differently, which is exactly the state
// this design exists to end.
static void checkExactD(const char *label, double got, double want)
{
    const bool ok = got == want;               // deliberate: no epsilon
    std::printf("  [%s] %-42s got %17.10g  want %17.10g\n", ok ? "PASS" : "FAIL", label, got, want);
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

// ── The C17 identity, restated INDEPENDENTLY of the reducer ───────────────────────────────────
//
// The extremes of ChartMetrics::windowedMean over the anchors a query may pick — every VALID sample
// whose t lies in [a, b]. This is the whole content of Phase 6's promise ("PEAK is the extremum of
// the drawn line inside the window") written as a plain scan over the array the chart draws, so the
// assertion is not "the reducer agrees with itself": it walks the returned array, not the series.
//
// The mask rule here is the SHORT-MASK RULE, spelled out a second time on purpose — a mask shorter
// than the curve is discarded wholesale (chart_metrics.h haveMask), and if this test applied it to
// the prefix instead it would be asserting a different question than the class answers. Non-finite
// values are excluded for the same reason SeriesView::isValid excludes them: they are not
// measurements, and no fixture here has one, which is exactly why it must not be assumed.
struct MeanExtremes {
    double mn = 0.0, mx = 0.0;
    int    iMin = -1, iMax = -1;
    bool   any = false;                 // false ⇒ NO anchor in the window; the identity is silent
};
static MeanExtremes meanExtremesOver(const QVariantList &t, const QVariantList &v,
                                     const QVariantList &mean, const QVariantList &valid,
                                     qlonglong a, qlonglong b)
{
    MeanExtremes r;
    const int n = qMin(qMin(t.size(), v.size()), mean.size());
    const bool haveMask = n > 0 && valid.size() >= n;
    for (int i = 0; i < n; ++i) {
        const qlonglong ti = t.at(i).toLongLong();
        if (ti < a || ti > b) continue;
        if (haveMask && valid.at(i).toInt() == 0) continue;
        if (!std::isfinite(v.at(i).toDouble())) continue;
        const double m = mean.at(i).toDouble();
        if (!r.any) { r.mn = r.mx = m; r.iMin = r.iMax = i; r.any = true; continue; }
        if (m < r.mn) { r.mn = m; r.iMin = i; }
        if (m > r.mx) { r.mx = m; r.iMax = i; }
    }
    return r;
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

    // ── σ GOVERNS THE DIGITS (design §5.3, principle 3) ───────────────────────────
    //
    // "A ±3° reading printed as 11° is honest; the same reading printed as 11.37° is not." The rule
    // has one number in it, displayStep, and two properties worth defending in a test rather than in
    // a comment: it can only ever COARSEN a reading (so no series gains precision from having been
    // characterised), and at σ = 0 — which is what an ABSENT σ becomes at the display boundary — it
    // is bit-for-bit the formatter that shipped before any of this existed. The second is what makes
    // Phase 3 safe on the hundreds of series whose producers do not propagate σ yet.
    {
        std::printf("displayStep — the nicest {1,2,5}×10ⁿ not below σ\n");
        const QString deg = QStringLiteral("°");
        // The table pinned in C12. The interesting entries are the ones that do NOT round to the
        // nearest nice number but to the nearest nice number AT OR ABOVE σ: 2.5 → 5 (not 2), 6 → 10
        // (not 5), 30 → 50 (not 20). A step below σ would print a digit the noise does not support,
        // which is the whole thing being prevented.
        checkEqD("σ 0   ⇒ 1  (absent, or uncharacterised)", cm.displayStep(0.0,  deg), 1.0);
        checkEqD("σ 0.3 ⇒ 1  (floored at one unit)",        cm.displayStep(0.3,  deg), 1.0);
        checkEqD("σ 1.4 ⇒ 2",                               cm.displayStep(1.4,  deg), 2.0);
        checkEqD("σ 2.5 ⇒ 5  (not 2 — 2 is below σ)",       cm.displayStep(2.5,  deg), 5.0);
        checkEqD("σ 6   ⇒ 10 (not 5)",                      cm.displayStep(6.0,  deg), 10.0);
        checkEqD("σ 12  ⇒ 20",                              cm.displayStep(12.0, deg), 20.0);
        checkEqD("σ 30  ⇒ 50 (not 20)",                     cm.displayStep(30.0, deg), 50.0);

        // On a boundary the step EQUALS σ — "not below" is inclusive, so a σ of exactly 2 does not
        // get promoted to 5. And the exact powers of ten are the float trap in the rule: without the
        // epsilon in the implementation, log10/pow round-trip 100 to a hair over 1.0×10² and promote
        // it to 200, doubling the coarseness of every reading of a σ = 100 series.
        checkEqD("σ 2   ⇒ 2   (inclusive)",  cm.displayStep(2.0,   deg), 2.0);
        checkEqD("σ 5   ⇒ 5   (inclusive)",  cm.displayStep(5.0,   deg), 5.0);
        checkEqD("σ 10  ⇒ 10  (power of 10)", cm.displayStep(10.0, deg), 10.0);
        checkEqD("σ 100 ⇒ 100 (power of 10)", cm.displayStep(100.0, deg), 100.0);

        // ⚠ NEVER BELOW ONE UNIT, whatever σ says, because the rule exists to coarsen a reading and
        // never to sharpen one. A plumb bob with σ = 0.08 in has NOT earned a decimal place: its
        // noise being small is not evidence that the projection, the calibration and the smoother
        // agree to a hundredth of an inch, and printing "1.63 in" because σ was 0.08 would be this
        // design manufacturing exactly the false precision it was written to remove.
        checkEqD("σ 0.08 in ⇒ 1  (no decimal earned)", cm.displayStep(0.08, QStringLiteral("in")), 1.0);
        checkEqD("σ 0.001  ⇒ 1",                       cm.displayStep(0.001, deg), 1.0);
        // A broken error budget is no claim at all, not a claim of zero: all three go to the floor.
        // +INFINITY is the one worth spelling out — it is NOT "infinitely coarse, print one digit for
        // the whole swing", it is a producer that divided by a zero span, and an unusable σ is
        // indistinguishable from an unstated one. A step of inf would also make llround undefined
        // behaviour in the formatter, so this clause is load-bearing and not merely tidy.
        checkEqD("σ negative ⇒ 1", cm.displayStep(-3.0, deg), 1.0);
        checkEqD("σ NaN      ⇒ 1", cm.displayStep(std::nan(""), deg), 1.0);
        checkEqD("σ +inf     ⇒ 1 (treated as absent)",
                 cm.displayStep(std::numeric_limits<double>::infinity(), deg), 1.0);
        // The floor is keyed on the DISPLAY token (unitStepFloor), so the four "% of something"
        // units cannot disagree with each other or with degrees about how coarse a step 1 is.
        checkEqD("percent, same rule", cm.displayStep(2.5, QStringLiteral("% stance width")), 5.0);
        checkEqD("empty unit, same rule", cm.displayStep(2.5, QString()), 5.0);
    }

    {
        std::printf("formatBare/formatValue with σ — the step in the printed string\n");
        const QString deg = QStringLiteral("°");
        // σ = 2.5° ⇒ step 5. ROUND TO THE NEAREST MULTIPLE OF THE STEP, TIES AWAY FROM ZERO (this is
        // llround's rule, not banker's rounding — 2.5 goes to 5, not to 0). This is the case design
        // §8 open question 1 is about: on the degrees scale it may read as coarse, and the fallback
        // if it does is whole units plus the ± chip. The rule is implemented so the probe can show
        // Mark what it looks like on the Plumb Bob preset.
        checkStr("step 5: 12.4 ⇒ +10", cm.formatBare(12.4,  deg, 2.5), "+10");
        checkStr("step 5: 12.6 ⇒ +15", cm.formatBare(12.6,  deg, 2.5), "+15");
        checkStr("step 5: −7.4 ⇒ -5",  cm.formatBare(-7.4,  deg, 2.5), "-5");
        checkStr("step 5: tie 2.5 away from zero",  cm.formatBare(2.5,  deg, 2.5), "+5");
        checkStr("step 5: tie −2.5 away from zero", cm.formatBare(-2.5, deg, 2.5), "-5");
        // σ = 1.4° ⇒ step 2: 3.1 goes to 4 and 2.9 to 2, because those are the nearest MULTIPLES OF
        // THE STEP — the step is what quantises a reading, not the digit count. The two assertions
        // above are the ones that separate ties-away-from-zero from banker's rounding: at step 5,
        // 2.5 is an exact half-step, and nearest-EVEN would send it to 0 and print "0" — a reading
        // suppressed by a rounding convention rather than by its own noise.
        checkStr("step 2: 3.1 ⇒ +4", cm.formatBare(3.1, deg, 1.4), "+4");
        checkStr("step 2: 2.9 ⇒ +2", cm.formatBare(2.9, deg, 1.4), "+2");
        // A value smaller than half a step reads as 0 — and prints "0", not "-0": that is the whole
        // point of the rule (a wobble inside the noise is not a reading) and "-0" would be a sign
        // claimed off rounding noise.
        checkStr("step 5: −0.4 ⇒ 0 (no -0)", cm.formatBare(-0.4, deg, 2.5), "0");
        // The unit-carrying form takes the same step and keeps its own spacing convention.
        checkStr("value form, step 5",      cm.formatValue(12.6, deg, 2.5), "+15°");
        checkStr("percent, step 5, spaced", cm.formatValue(12.4, QStringLiteral("% stance width"), 2.5), "10 %");

        // ── THE 2-ARG FORMS ARE UNCHANGED, BYTE FOR BYTE ──────────────────────────
        //
        // This is the assertion Phase 3 rests on: every series whose producer has not propagated a
        // σ (which is most of them, and every series in every swing.json written before W1's work)
        // must display EXACTLY as it did before. Both the explicit 0.0 and the defaulted call are
        // checked, because the two reach the formatter by different routes — the second through the
        // moc-generated CLONE of the method, which is also the route every QML caller that has not
        // been updated takes.
        // Every string the pre-Phase-3 block above asserts, re-asserted at σ = 0 — the whole
        // pre-σ surface, not a sample of it, because "unchanged" is a claim about all of it.
        checkStr("2-arg degrees",  cm.formatValue(12.4, deg),  "+12°");
        checkStr("σ=0 == 2-arg",   cm.formatValue(12.4, deg, 0.0), "+12°");
        checkStr("2-arg negative", cm.formatBare(-8.2, deg),   "-8");
        checkStr("σ=0 == 2-arg, bare", cm.formatBare(-8.2, deg, 0.0), "-8");
        checkStr("2-arg inches",   cm.formatValue(1.6, QStringLiteral("in")), "2 in");
        checkStr("σ=0 inches",     cm.formatValue(1.6, QStringLiteral("in"), 0.0), "2 in");
        // The three that exercise the OTHER branches of the sign and spacing rules, so a σ argument
        // cannot have quietly changed one of them: a speed (space, no "+"), a percent (short token),
        // and the empty unit that falls back to degrees and so DOES take a "+".
        checkStr("σ=0 mph, spaced, no +", cm.formatValue(75.2, QStringLiteral("mph"), 0.0), "75 mph");
        checkStr("σ=0 mph bare",          cm.formatBare(75.2,  QStringLiteral("mph"), 0.0), "75");
        checkStr("σ=0 percent short token",
                 cm.formatValue(12.4, QStringLiteral("% stance width"), 0.0), "12 %");
        checkStr("σ=0 percent bare",
                 cm.formatBare(12.4, QStringLiteral("% stance width"), 0.0), "12");
        checkStr("σ=0 empty unit ⇒ degrees", cm.formatValue(3.0, QString(), 0.0), "+3°");
        // σ present but under a unit changes nothing either: the floor is doing its job.
        checkStr("σ 0.3 ⇒ today's string", cm.formatBare(12.4, deg, 0.3), "+12");
        checkStr("σ 0.3 mph ⇒ today's string",
                 cm.formatValue(75.2, QStringLiteral("mph"), 0.3), "75 mph");
    }

    // ── READINGS ARE QUANTISED, UNCERTAINTIES ARE QUOTED ──────────────────────────
    //
    // Every "± x" on the panel goes through this one function: the series σ chip beside the card's
    // unit, the ± beside PEAK (peakSigma) and the ± beside PK RATE (rateSigma). It has NO σ
    // parameter, and that absence is the thing under test. displayStep governs READINGS; an
    // uncertainty is not a reading, it is the statement of how far the reading can be trusted, and it
    // is only ever read against the value beside it — so it is quoted at one fixed decimal.
    //
    // Three defects came of coupling the two, all found in review, and the assertions below are
    // written against them by name.
    {
        std::printf("formatUncertainty — quoted, never quantised\n");
        const QString deg = QStringLiteral("°");
        // One decimal, always. No step can round these to a whole number or to nothing.
        checkStr("0.4",  cm.formatUncertainty(0.4),  "± 0.4");
        checkStr("12.4", cm.formatUncertainty(12.4), "± 12.4");
        checkStr("1.5",  cm.formatUncertainty(1.5),  "± 1.5");
        checkStr("3.0",  cm.formatUncertainty(3.0),  "± 3.0");

        // ⚠ NO DISCONTINUITY ACROSS HALF A STEP — the assertion the review asked for. On a series
        // with σ = 2.5 (step 5) the old step-quantised rule sent 2.4 to "± 0" and 2.6 to "± 5": two
        // uncertainties 8 % apart printed as nothing and as a whole step, a cliff sitting exactly
        // where a reader is least able to see it. Quoted, they differ by what they differ by. There
        // is no σ argument to pass any more, which is what makes the cliff unreachable rather than
        // merely unused.
        checkStr("2.4 (σ 2.5, old step 5)", cm.formatUncertainty(2.4), "± 2.4");
        checkStr("2.6 (σ 2.5, old step 5)", cm.formatUncertainty(2.6), "± 2.6");
        // F1, by its numbers: a fitted-slope standard error of 3.0 on a series whose σ chose a
        // 5-unit step printed "± 5" — two thirds of inflation borrowed from a quantity in another
        // unit entirely (σ is in the metric's unit, a slope's error is per 100 ms).
        checkStr("PK RATE 3.0 is not ± 5", cm.formatUncertainty(3.0), "± 3.0");
        // F2: peakSigma is about σ/√k for a k-sample window, so it is SMALLER than σ by
        // construction — a step chosen from σ rounded it to zero on essentially every card, which is
        // why the step branch was effectively unreachable and only the fallback ever ran.
        checkStr("PEAK 0.6 on a step-5 card", cm.formatUncertainty(0.6), "± 0.6");

        // Below 0.05 one decimal can say nothing true, so the honest statement is a BOUND. "± 0.0"
        // is never printed: it reads as exactness, in the one place on the card whose whole job is to
        // deny it. Exactly 0 goes here too — a zero standard error is a degenerate window (one
        // sample, or an exact fit), not evidence of a perfect measurement, and "<0.1" is a bound
        // that is true of it.
        checkStr("0.04 ⇒ a bound",      cm.formatUncertainty(0.04), "± <0.1");
        checkStr("0.049 ⇒ a bound",     cm.formatUncertainty(0.049), "± <0.1");
        checkStr("0.05 ⇒ one decimal",  cm.formatUncertainty(0.05), "± 0.1");
        checkStr("exactly 0 ⇒ a bound", cm.formatUncertainty(0.0),  "± <0.1");

        // F3, the σ chip: the plumb bob's own σ is 0.03–0.06 in, and the local `.toFixed(1)` this
        // replaced printed "± 0.0in" — false exactness AND the unit jammed against the number, the
        // same defect formatValue exists to prevent. The unit token takes formatValue's spacing:
        // degrees close up, everything else spaced.
        checkStr("chip: plumb bob σ 0.04 in", cm.formatUncertainty(0.04, QStringLiteral("in")), "± <0.1 in");
        checkStr("chip: plumb bob σ 0.06 in", cm.formatUncertainty(0.06, QStringLiteral("in")), "± 0.1 in");
        checkStr("chip: degrees close up",    cm.formatUncertainty(2.5,  deg), "± 2.5°");
        checkStr("chip: percent short token", cm.formatUncertainty(1.2, QStringLiteral("% stance width")),
                 "± 1.2 %");
        // ⚠ AN EMPTY UNIT MEANS NO TOKEN HERE, the opposite of formatBare/formatValue's
        // empty-means-degrees — the two tiles want no token because the card names their unit above
        // them. Pinned because the inversion is surprising and a caller passing an unresolved unit
        // through would otherwise silently lose a "°".
        checkStr("empty unit ⇒ no token", cm.formatUncertainty(2.5), "± 2.5");

        // Direction is not a property of an uncertainty. (summaryMasked's `rate` is signed, and a
        // caller handing this its error must not be able to print "± -2".)
        checkStr("negative reads as magnitude", cm.formatUncertainty(-2.0), "± 2.0");
        // A degenerate fit renders as nothing at all — the caller draws an empty label rather than
        // "± nan" or "± inf". +inf is the one a real reducer can produce (a zero-variance basis).
        checkStr("NaN ⇒ nothing",  cm.formatUncertainty(std::nan("")), "");
        checkStr("+inf ⇒ nothing", cm.formatUncertainty(std::numeric_limits<double>::infinity()), "");
    }

    // ── seriesSigma: ONE implementation of "absent means absent" ──────────────────
    //
    // This was a four-clause guard copied into PpChartSummary, PpMetricChart and PpChartPlot — three
    // chances to disagree about what an absent σ means, in the one part of this design where the
    // distinction between "not characterised" and "zero" is the whole point. Now one C++ function,
    // which is also the only form that can be asserted.
    {
        std::printf("seriesSigma — the absent→0 display substitution, in one place\n");
        auto withSigma = [](const QVariant &s) {
            QVariantMap m{ { QStringLiteral("key"), QStringLiteral("hipLineTilt") } };
            if (s.isValid()) m.insert(QStringLiteral("sigma"), s);
            return m;
        };
        checkEqD("a characterised σ comes through", cm.seriesSigma(withSigma(2.5)), 2.5);
        // ABSENT ⇒ 0, which asks displayStep for no coarsening — the series then prints exactly as
        // it did before §5.3. The 0 means "no claim", never "exact", and nothing that touches DATA
        // may call this.
        checkEqD("no sigma key ⇒ 0", cm.seriesSigma(withSigma(QVariant())), 0.0);
        checkEqD("null sigma ⇒ 0",   cm.seriesSigma(withSigma(QVariant::fromValue(nullptr))), 0.0);
        // A broken budget is not a small one: none of these may become a step.
        checkEqD("NaN ⇒ 0",      cm.seriesSigma(withSigma(std::nan(""))), 0.0);
        checkEqD("+inf ⇒ 0",     cm.seriesSigma(withSigma(std::numeric_limits<double>::infinity())), 0.0);
        checkEqD("negative ⇒ 0", cm.seriesSigma(withSigma(-1.0)), 0.0);
        checkEqD("zero ⇒ 0",     cm.seriesSigma(withSigma(0.0)), 0.0);
        checkEqD("empty map ⇒ 0", cm.seriesSigma(QVariantMap{}), 0.0);
        // A σ that arrived as a JSON number in a string (which a hand-edited swing.json can produce)
        // still reads — toDouble's `ok` only rejects what is not a number at all.
        checkEqD("numeric string reads", cm.seriesSigma(withSigma(QStringLiteral("2.5"))), 2.5);
        checkEqD("non-numeric string ⇒ 0", cm.seriesSigma(withSigma(QStringLiteral("wide"))), 0.0);
        // And the round trip the whole rule rests on: an absent σ leaves the display untouched.
        checkEqD("absent ⇒ step 1",
                 cm.displayStep(cm.seriesSigma(withSigma(QVariant())), QStringLiteral("°")), 1.0);
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
        // The σ of the winning extremum window is reported too, and here it is NOT of the order of
        // the series' σ: it is the standard error of that window's MEAN about a local line, so
        // roughly σ_residual/√k — a fraction of σ (0.21 against σ = 0.90 on this fixture). That is
        // the number design §5.3 puts a "±" in front of on PEAK, and the reason a reader can tell
        // that a 0.7σ excursion is not a finding. Bounded loosely on purpose: the exact value
        // depends on which window won and how many samples it held, neither of which this case is
        // pinning — what it pins is that a still series reports a NON-ZERO but SUB-σ uncertainty.
        const double peakSigma = s.value(QStringLiteral("peakSigma")).toDouble();
        checkTrue("peakSigma is non-zero on noise", peakSigma > 0.05 * realSd);
        checkTrue("…and well inside σ (it is the mean's error, not the spread)",
                  peakSigma < 1.5 * realSd);
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
        // ⚠ WHICH SEMANTICS THESE ENCODE: the extremum support is NOT clamped to [from, to] — the
        // ANCHORS are inside the window, their ±20 ms support is not — and it widens symmetrically
        // only where it would otherwise hold fewer than 3 samples. At both window-edge anchors
        // (104 ms and 296 ms) the support is therefore the full symmetric five (88…120 ms and
        // 280…312 ms, all of which exist because the window sits 100 ms inside a 400 ms series),
        // and the mean of a symmetric support on a straight line IS the value at its centre: 36.0
        // and 84.0, the window's endpoints exactly.
        //
        // The clamp was tried and REVERTED: a span-cached engine cannot agree with a clamped card
        // (20 disagreements in 514 against 0), and the out-of-domain leak it was aimed at is closed
        // at the PRODUCER instead — samples outside a narrowed metric's phase domain are marked
        // valid = 0 in swing.json, so these reducers exclude them for the same reason they exclude
        // every other bridged sample, and no reducer needs a second notion of "outside". Under the
        // clamp these two read 38.0 and 82.0 (supports truncated to 104/112/120 and 280/288/296);
        // if this assertion ever fails with THOSE numbers, a clamp has come back — the fixture has
        // not gone stale.
        checkEqD("min = the ramp at the window start",
                 s.value(QStringLiteral("min")).toDouble(),  36.0);
        checkEqD("max = the ramp at the window end",
                 s.value(QStringLiteral("max")).toDouble(),  84.0);
        checkEqD("peak = the larger magnitude", s.value(QStringLiteral("peak")).toDouble(),  84.0);
        checkEqD("range",                       s.value(QStringLiteral("range")).toDouble(), 48.0);
        // range AND delta AGREE here — 48 both ways — and that is worth pinning as well: they rest
        // on different evidence (a span of windowed means at anchors inside the window, against a
        // difference of ±15 ms medians at its edges), and on a window several samples wider than
        // either support they must converge. Where they can openly disagree is a window NARROWER
        // than the support, and that case is documented in the header rather than papered over by
        // blending the two.
        checkEqD("…and delta agrees with it on a window this wide",
                 s.value(QStringLiteral("range")).toDouble(),
                 std::fabs(s.value(QStringLiteral("delta")).toDouble()));
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
        // ⚠ peakSigma IS ZERO HERE, and that is the assertion, not a rounding. It is the noise the
        // window MEAN carries — the scatter about a LOCAL STRAIGHT LINE through the window, over
        // √k — not the spread of the window's samples. A ramp IS a straight line, so it has no
        // residual and the mean carries no noise: "± 0" beside PEAK is the honest reading of a
        // noiseless curve, where the sample spread of that support (√10 = 3.16) would have printed
        // "± 3" and called the slope
        // measurement error.
        checkEqD("peakSigma = 0 on a noiseless line",
                 s.value(QStringLiteral("peakSigma")).toDouble(), 0.0);
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
        // At 27 ms a ±20 ms support holds exactly ONE sample, so C8's widen-to-≥3 rule fires on
        // every anchor here — and because the widening is SYMMETRIC and the support is not clamped
        // to [from, to], what it widens to is the anchor plus one neighbour each side (±27 ms).
        // On a straight line a symmetric support's mean is the value at its centre, so the extremes
        // come back EXACTLY the window's own end samples, sparse or not: the widening changed the σ
        // this reports, not the value it reports.
        checkEqD("min = the window's first sample", s.value(QStringLiteral("min")).toDouble(),  5.4);
        checkEqD("max = the window's last sample",  s.value(QStringLiteral("max")).toDouble(), 48.6);
        // …and the mean's σ is 0 for the same reason as on the dense ramp: a straight line leaves
        // no residual to speak from, however few samples the support ended up with. This is the
        // assertion that would have caught the FIRST version of the sparse path, which reported
        // peakSigma 0.000 for the wrong reason — because the support was ONE SAMPLE, not because
        // the curve was clean.
        checkEqD("peakSigma = 0 on a noiseless line, sparse too",
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

    // ── A COARSE series is not an incomplete one ──────────────────────────────────
    //
    // ⚠ THE CASE THAT MADE `partial` OVER-CLAIM. The fallback edge fires whenever no valid sample
    // lies within ±15 ms of the window edge, and that has nothing to do with bridging: a series
    // sampled at 100 ms strides (or any real timeline's larger gaps — a fifth of a rich_7iron
    // series' span is more than 15 ms from a sample, its worst gap 83 ms) puts most instants out
    // of reach of the median window. A chip on those swings would be claiming the producer left a
    // hole where it simply took fewer readings, and pre-Phase-2 the chip was unreachable without a
    // mask. So `partial` requires an honoured mask, and this is the pair that pins it.
    {
        std::printf("summaryMasked — a coarse series declares nothing\n");
        const QVariantList t{ qlonglong(0),      qlonglong(100000), qlonglong(200000),
                              qlonglong(300000), qlonglong(400000) };
        const QVariantList v{ 0.0, 1.0, 2.0, 3.0, 4.0 };
        // The window starts 40 ms past a sample and 60 ms before the next — every reducer's median
        // window over it is empty, so the edge is interpolated.
        const QVariantMap none = cm.summary(t, v, 140000, 300000);
        checkEqD("the edge is interpolated (1 + 1·0.4)",
                 none.value(QStringLiteral("start")).toDouble(), 1.4);
        checkTrue("…and an unmasked series is NOT partial",
                  !none.value(QStringLiteral("partial")).toBool());
        checkTrue("…and it does have readings", none.value(QStringLiteral("edgeOk")).toBool());
        // 100 ms strides: the 50 ms fit window reaches one further sample, which is two points —
        // below the 3-sample floor — so there is no rate, exactly as C8's note says.
        checkTrue("no rate at 100 ms strides", !none.value(QStringLiteral("rateOk")).toBool());

        // The same series with ONE bridged sample OUTSIDE the window: rule 2 cannot fire (no
        // invalid sample inside), so this isolates rule 1 — the fallback, on a masked series.
        // The interpolation now reaches from (0 ms, 0) to (200 ms, 2) and lands on the same 1.4,
        // which is the point: the VALUE did not change, the claim about it did.
        const QVariantList mask{ 1, 0, 1, 1, 1 };
        const QVariantMap some = cm.summaryMasked(t, v, mask, 140000, 300000);
        checkEqD("masked: the edge reads the same 1.4",
                 some.value(QStringLiteral("start")).toDouble(), 1.4);
        checkTrue("…but a masked fallback IS partial",
                  some.value(QStringLiteral("partial")).toBool());
    }

    // ── edgeOk: a series with nothing readable prints no numbers at all ───────────
    //
    // The counterpart to rateOk, for the other six. Every sample bridged means interpValid has
    // nothing to interpolate BETWEEN and returns 0.0, so PEAK, Δ and RANGE all come back 0 — a
    // card that reads as a still, well-behaved curve while wearing a PARTIAL chip that says
    // "mostly fine". `partial` qualifies a number; this says there is no number.
    {
        std::printf("summaryMasked — edgeOk\n");
        const int n = 25;
        const QVariantList t = tAt(n);
        const QVariantList v = rampV(n, 0.0, 1.0);

        // Every sample bridged. A mask of all zeros IS honoured (it covers the curve), so this is
        // a series that says, in full, "none of this was measured".
        QVariantList allBridged;
        for (int i = 0; i < n; ++i) allBridged.append(0);
        const QVariantMap dead = cm.summaryMasked(t, v, allBridged, 0, qlonglong(n - 1) * kDtUs);
        checkTrue("all-bridged ⇒ edgeOk false", !dead.value(QStringLiteral("edgeOk")).toBool());
        checkTrue("…and no rate either",        !dead.value(QStringLiteral("rateOk")).toBool());
        checkTrue("…and partial, which is true but not enough",
                  dead.value(QStringLiteral("partial")).toBool());
        // The zeros are still RETURNED — a caller mid-migration degrades rather than crashes — and
        // they are exactly why the flag has to exist. This asserts what the card must not print.
        checkEqD("peak is a zero from nothing",  dead.value(QStringLiteral("peak")).toDouble(),  0.0);
        checkEqD("delta is a zero from nothing", dead.value(QStringLiteral("delta")).toDouble(), 0.0);
        checkEqD("range is a zero from nothing", dead.value(QStringLiteral("range")).toDouble(), 0.0);

        // An EMPTY curve says the same thing with no mask at all, and must not be `partial` for it:
        // there is no bridged run here, there is no series.
        const QVariantMap empty = cm.summary(QVariantList{}, QVariantList{}, 0, 100000);
        checkTrue("empty curve ⇒ edgeOk false", !empty.value(QStringLiteral("edgeOk")).toBool());
        checkTrue("…and not partial (nothing was bridged; nothing exists)",
                  !empty.value(QStringLiteral("partial")).toBool());
        checkTrue("…and no rate", !empty.value(QStringLiteral("rateOk")).toBool());

        // One valid sample is enough to have an edge — the flag is about EXISTENCE, not sufficiency;
        // `partial` and `rateOk` are what qualify the rest.
        QVariantList oneGood;
        for (int i = 0; i < n; ++i) oneGood.append(i == 12 ? 1 : 0);
        const QVariantMap thin = cm.summaryMasked(t, v, oneGood, 0, qlonglong(n - 1) * kDtUs);
        checkTrue("one valid sample ⇒ edgeOk true", thin.value(QStringLiteral("edgeOk")).toBool());
        checkEqD("…and every edge reads that one sample",
                 thin.value(QStringLiteral("start")).toDouble(), 12.0);
        checkTrue("…still no rate from one sample", !thin.value(QStringLiteral("rateOk")).toBool());
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
        for (const char *k : { "peakSigma", "rateSigma", "edgeOk", "rateOk", "tRateUs", "extremumOk" })
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

    // ── C17 / Phase 6: THE DRAWN LINE AND THE PEAK TILE ARE ONE REDUCTION ────────
    //
    // The chart no longer strokes the persisted samples: it strokes ChartMetrics::windowedMean, the
    // per-sample 40 ms centred mean that reduceExtremum ranks. That is only defensible — design §4
    // principle 1 forbids display-only smoothing outright — if the line and the numbers are provably
    // THE SAME ARITHMETIC, so this suite asserts the identity BIT-EXACTLY, on every fixture in this
    // file, in the form the user can check by eye on the panel:
    //
    //     summaryMasked(win).min / .max / .peak  ==  the extremes of the drawn line inside win
    //
    // Bit-exact rather than toleranced because a 1e-6 agreement would still permit two
    // implementations of one window, which is the state Phase 2 removed for the tiles and Phase 6
    // removes for the stroke. If this suite fails, either the reducer stopped sharing its per-sample
    // mean (C16) or the chart started smoothing on its own; both are the same defect.
    //
    // ⚠ ONE KNOWN WAY THIS COULD FAIL WITHOUT EITHER BEING TRUE, stated so a failure is diagnosable
    // rather than mysterious: reduceExtremum breaks ties with a RELATIVE MARGIN (series_reduce.h
    // detail::improves, 1e-12·|value|) so the reported instant is not decided by float noise, while
    // the scan below takes the strict extremum. Two candidate means within that margin of each other
    // would therefore hand the two different WINNERS — same curve, same window, a difference in the
    // twelfth digit. No fixture here has one (the ties that exist are exact, and both rules keep the
    // EARLIEST of those), and a real failure of that kind would still be worth reading: it would mean
    // the PEAK tile is not quite the line's extremum, by a hair, on a curve that flat.
    {
        std::printf("windowedMean — the line the chart draws IS the reduction the tiles report\n");

        // Every fixture this file already exercises, in one table, so a case added to the reducer
        // suite is one line away from being covered here too. The windows are the ones each of those
        // cases uses, including the degenerate ones (inside a bridged run, all bridged, one valid
        // sample, a mask too short to honour) — those are where an identity most easily stops holding.
        struct Fixture { const char *name; QVariantList t, v, mask; qlonglong a, b; };

        const int          nSpike = 25, iSpike = 12;
        const QVariantList tSpike = tAt(nSpike);
        QVariantList       vSpike = flatV(nSpike, 4.0);
        vSpike[iSpike] = 99.0;
        const qlonglong    spikeEnd = qlonglong(nSpike - 1) * kDtUs;

        const QVariantList tRamp25 = tAt(25);
        const QVariantList vRamp25 = rampV(25, 0.0, 1.0);
        const QVariantList bridged = onesMask(25, 8, 15);        // t = 64…120 ms bridged

        const QVariantList tCoarse{ qlonglong(0),      qlonglong(100000), qlonglong(200000),
                                    qlonglong(300000), qlonglong(400000) };
        const QVariantList vCoarse{ 0.0, 1.0, 2.0, 3.0, 4.0 };

        QVariantList allBridged;
        for (int i = 0; i < 25; ++i) allBridged.append(0);
        QVariantList oneGood;
        for (int i = 0; i < 25; ++i) oneGood.append(i == 12 ? 1 : 0);

        const Fixture fixtures[] = {
            { "spike, whole series",      tSpike, vSpike, {},                 0, spikeEnd },
            { "spike, interior window",   tSpike, vSpike, {},                 5 * kDtUs, 20 * kDtUs },
            { "spike masked out",         tSpike, vSpike, onesMask(nSpike, iSpike, iSpike),
                                                                              0, spikeEnd },
            { "spike, short mask",        tSpike, vSpike, onesMask(13, 12, 12), 0, spikeEnd },
            { "spike, over-length mask",  tSpike, vSpike, onesMask(nSpike + 4, iSpike, iSpike),
                                                                              0, spikeEnd },
            { "still noise (σ=1)",        tAt(51), noiseV(51, 1.0), {},        0, 50 * kDtUs },
            { "clean ramp, interior",     tAt(51), rampV(51, 10.0, 2.0), {},   13 * kDtUs, 37 * kDtUs },
            { "sparse 27 ms ramp",        tAt(21, 27000), rampV(21, 0.0, 2.7), {},
                                                                              2 * 27000, 18 * 27000 },
            { "two samples",              QVariantList{ qlonglong(0), qlonglong(kDtUs) },
                                          QVariantList{ 1.0, 2.0 }, {},       0, kDtUs },
            { "bridged run, across it",   tRamp25, vRamp25, bridged,           12 * kDtUs, 24 * kDtUs },
            { "bridged run, inside it",   tRamp25, vRamp25, bridged,           10 * kDtUs, 13 * kDtUs },
            { "bridged run, clean tail",  tRamp25, vRamp25, bridged,           17 * kDtUs, 24 * kDtUs },
            { "coarse 100 ms strides",    tCoarse, vCoarse, {},                140000, 300000 },
            // ⚠ F2's CASE, AND IT IS UNMASKED: a window narrower than the sample spacing, on a
            // perfectly healthy series. No sample lies in [140, 190] ms, so there is no drawn point
            // in there to be the peak and `partial` cannot say so (it needs an honoured mask). This
            // is the fixture that pins `extremumOk` as the thing that does.
            { "coarse, tight unmasked win", tCoarse, vCoarse, {},                140000, 180000 },
            { "coarse, one bridged",      tCoarse, vCoarse, QVariantList{ 1, 0, 1, 1, 1 },
                                                                              140000, 300000 },
            { "all bridged",              tRamp25, vRamp25, allBridged,        0, 24 * kDtUs },
            { "one valid sample",         tRamp25, vRamp25, oneGood,           0, 24 * kDtUs },
        };

        for (const Fixture &f : fixtures) {
            const QVariantMap  st   = cm.summaryMasked(f.t, f.v, f.mask, f.a, f.b);
            const QVariantMap  wm   = cm.windowedMean(f.t, f.v, f.mask);
            const QVariantList mean = wm.value(QStringLiteral("mean")).toList();
            const QVariantList sig  = wm.value(QStringLiteral("sigma")).toList();

            std::printf("  · %s\n", f.name);
            // Parallel to the curve, always — a caller indexes mean[i] against value[i] and a
            // shorter array would silently draw a truncated line (PpChartPlot._meanOf falls back to
            // the raw values on a length mismatch, so this is what keeps the mean reachable at all).
            checkEqI("   length == the curve", mean.size(), qMin(f.t.size(), f.v.size()));
            checkEqI("   sigma is parallel too", sig.size(), mean.size());

            const MeanExtremes ex = meanExtremesOver(f.t, f.v, mean, f.mask, f.a, f.b);
            if (!ex.any) {
                // NO ANCHOR IN THE WINDOW — a window wholly inside a bridged run, an all-bridged
                // series, or (F2) a window narrower than the sample spacing on a series with nothing
                // wrong with it. summaryMasked then reports the two interpolated EDGES as its
                // extremes: there is no drawn measurement in there to be equal to, and asserting one
                // would be asserting that a gap has a peak.
                //
                // ⚠ WHAT IS ASSERTED IS THAT THE CARD SAYS SO — and it is `extremumOk`, not
                // `partial`. This assertion used to test `partial`, which ENCODED THE BUG: partial
                // requires an honoured mask, so it is true for the two masked cases here and
                // unreachable for the unmasked one, where the tile printed an interpolated edge as
                // PEAK wearing no chip at all. extremumOk is exactly this branch's condition
                // (loR.ok && hiR.ok), whatever the mask says.
                checkTrue("   no valid anchor ⇒ extremumOk false",
                          !st.value(QStringLiteral("extremumOk")).toBool());
                continue;
            }
            // …and the converse, on every fixture that HAS an anchor: the flag is not simply always
            // false, and a display gating PEAK on it does not lose the tile everywhere.
            checkTrue("   an anchor in the window ⇒ extremumOk true",
                      st.value(QStringLiteral("extremumOk")).toBool());
            checkExactD("   min == min of the drawn line",
                        st.value(QStringLiteral("min")).toDouble(), ex.mn);
            checkExactD("   max == max of the drawn line",
                        st.value(QStringLiteral("max")).toDouble(), ex.mx);
            // PEAK is the extremum of larger MAGNITUDE (chart_metrics.cpp's rule, unchanged), so the
            // tile a reader actually sees is pinned as well and not merely its two ingredients.
            const bool   maxWins = std::fabs(ex.mx) >= std::fabs(ex.mn);
            const double peak    = maxWins ? ex.mx : ex.mn;
            checkExactD("   peak == that value on the line",
                        st.value(QStringLiteral("peak")).toDouble(), peak);
            // …AT THE SAME INSTANT, which is what makes the PEAK marker land ON the stroke rather
            // than near it: tPeakUs is the winning anchor's own sample time.
            checkEqI("   tPeakUs == that sample's time",
                     st.value(QStringLiteral("tPeakUs")).toLongLong(),
                     f.t.at(maxWins ? ex.iMax : ex.iMin).toLongLong());
            // …and the ± beside the tile is that sample's own entry in the sigma array, so the chart
            // could draw the tile's uncertainty at the tile's point with no second reduction.
            if (sig.size() == mean.size())          // guarded: the length check above already failed
                checkExactD("   peakSigma == sigma at that sample",
                            st.value(QStringLiteral("peakSigma")).toDouble(),
                            sig.at(maxWins ? ex.iMax : ex.iMin).toDouble());
        }

        // ── F2 IN FULL: the window with nothing in it, on a series with nothing wrong ──────
        //
        // The loop above pins `extremumOk` false here. This pins the other half — WHY a new flag was
        // needed rather than reading one of the two that already existed — because that is the part a
        // future reader will want to argue with.
        std::printf("  · F2: a window narrower than the sample spacing, unmasked\n");
        const QVariantMap tight = cm.summary(tCoarse, vCoarse, 140000, 180000);
        checkTrue("   extremumOk false (no sample in the window)",
                  !tight.value(QStringLiteral("extremumOk")).toBool());
        // `edgeOk` is TRUE: the series has readings, just not in here. So it cannot report this.
        checkTrue("   …but edgeOk is true — the curve has readings",
                  tight.value(QStringLiteral("edgeOk")).toBool());
        // …and `partial` is FALSE and unreachable: it requires an honoured mask, and this series
        // declares nothing. Before extremumOk the card therefore printed an interpolated edge as PEAK
        // with no chip of any kind — the confident absurdity, with the tile's Phase 6 claim ("a point
        // on the drawn line") false at the same time.
        checkTrue("   …and `partial` cannot fire on an unmasked series",
                  !tight.value(QStringLiteral("partial")).toBool());
        // The proof that PEAK is not on the line here: it equals an EDGE (the interpolated value at
        // 140 ms or 180 ms, 1.4 and 1.8 on this ramp-shaped coarse series), not any sample's mean.
        // (180, not 190: an edge within ±15 ms of a sample is that sample's median, not a fallback.)
        checkExactD("   peak is the window's own edge, not a measurement",
                    tight.value(QStringLiteral("peak")).toDouble(),
                    tight.value(QStringLiteral("end")).toDouble());
        checkEqD("   …which is the interpolation at 180 ms",
                 tight.value(QStringLiteral("end")).toDouble(), 1.8);
    }

    // ── windowedMean: what an INVALID sample contributes, which is nothing ────────
    {
        std::printf("windowedMean — an invalid sample is absent, and keeps its raw value\n");
        const int n = 25, iSpike = 12;
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[iSpike] = 99.0;

        const QVariantList bare = cm.windowedMean(t, v, QVariantList{})
                                    .value(QStringLiteral("mean")).toList();
        // UNMASKED: at 8 ms a ±20 ms window holds five samples, so every anchor within 20 ms of the
        // spike means (4·4 + 99)/5 = 23 exactly — including the spike's own anchor. This is the
        // number the tile prints, and after Phase 6 it is also the height the line reaches: the
        // reader can no longer see a 99 on the curve beside a 23 on the card and conclude that one
        // of the two is lying.
        checkExactD("the spike's own sample draws as 23", bare.at(iSpike).toDouble(), 23.0);
        checkExactD("…and so does its neighbour",         bare.at(iSpike - 1).toDouble(), 23.0);
        checkExactD("…while a sample 40 ms away is untouched", bare.at(iSpike - 5).toDouble(), 4.0);
        checkExactD("the tile agrees with the line",
                    cm.summary(t, v, 0, qlonglong(n - 1) * kDtUs).value(QStringLiteral("peak")).toDouble(),
                    23.0);

        // MASKED: the invalid entry carries its RAW value — the chart draws the bridged run dashed at
        // the persisted numbers, exactly as it did before this phase — and it enters no neighbour's
        // window, so the baseline either side is 4.0 to the last bit rather than 4-point-something.
        const QVariantList masked = cm.windowedMean(t, v, onesMask(n, iSpike, iSpike))
                                      .value(QStringLiteral("mean")).toList();
        checkExactD("an invalid entry is the RAW value", masked.at(iSpike).toDouble(), 99.0);
        checkExactD("…and pulls no neighbour up",        masked.at(iSpike - 1).toDouble(), 4.0);
        checkExactD("…on either side",                   masked.at(iSpike + 1).toDouble(), 4.0);

        // ⚠ THE COUNTER-CASE, and it is the one that proves this is a reduction and not a smoother:
        // on a straight line a symmetric window's mean IS the value at its centre, so the drawn line
        // lies exactly on the samples and a real excursion is not shrunk. Interior anchors only —
        // the first two and last two have one-sided support, which is a real property of the window
        // and not what this pins.
        const QVariantList tr = tAt(51), vr = rampV(51, 10.0, 2.0);
        const QVariantList mr = cm.windowedMean(tr, vr, QVariantList{})
                                  .value(QStringLiteral("mean")).toList();
        bool   onTheLine = true;
        double maxSigma  = 0.0;
        const QVariantList sr = cm.windowedMean(tr, vr, QVariantList{})
                                  .value(QStringLiteral("sigma")).toList();
        for (int i = 2; i < 49; ++i) {
            if (mr.at(i).toDouble() != vr.at(i).toDouble()) onTheLine = false;
            const double si = std::fabs(sr.at(i).toDouble());
            if (si > maxSigma) maxSigma = si;      // no <algorithm> needed for one comparison
        }
        checkTrue("on a ramp the drawn line IS the samples", onTheLine);
        // …and it says so: a straight line leaves no residual, so the mean it carries has no noise.
        // "± 0" on a noiseless curve, never the slope dressed up as uncertainty.
        //
        // ⚠ A BOUND, NOT AN EXACT ZERO, and the distinction is the whole content of the assertion.
        // windowMeanSigma fits a local line and sums the squared residuals of a curve that IS that
        // line, so the residuals are cancellations of numbers of order 100 — they come back as a few
        // ulps rather than as 0.0, and on this ramp the SSE guard (`sse > 0.0`) does not always
        // catch them. That is arithmetic, not a claim. What WOULD be a claim is a σ big enough to
        // print: the smallest thing formatUncertainty renders is 0.05, and a σ that was really the
        // ramp's own slope (the failure this case exists to catch — the first version of this
        // reducer reported ±0.08 on a noiseless ramp) is tens of millions of times the bound. So the
        // measured maximum is PRINTED, and the gate is 1e-9: anything between that and a visible
        // number is a real bias and must be reported, never toleranced by widening this line.
        char sigLabel[128];
        std::snprintf(sigLabel, sizeof(sigLabel),
                      "…and its σ is 0 to float noise (max |σ| = %.3g)", maxSigma);
        checkTrue(sigLabel, maxSigma < 1e-9);
    }

    // ── windowedMean: the short-mask rule, and an empty curve ─────────────────────
    {
        std::printf("windowedMean — a malformed mask is no mask, and an empty curve is empty\n");
        const int n = 25;
        const QVariantList t = tAt(n);
        QVariantList v = flatV(n, 4.0);
        v[12] = 99.0;

        // A mask covering 13 of 25 samples is a malformed document, not a partial statement, and is
        // discarded WHOLESALE — the same rule summaryMasked, measuredAt and the QML index form apply,
        // asserted here as an element-wise identity with the unmasked line so the DRAWN CURVE cannot
        // become the fourth opinion about it.
        const QVariantList none  = cm.windowedMean(t, v, QVariantList{})
                                     .value(QStringLiteral("mean")).toList();
        const QVariantList trunc = cm.windowedMean(t, v, onesMask(13, 12, 12))
                                     .value(QStringLiteral("mean")).toList();
        bool identical = trunc.size() == none.size();
        for (int i = 0; identical && i < none.size(); ++i)
            identical = trunc.at(i).toDouble() == none.at(i).toDouble();
        checkTrue("short mask ⇒ the unmasked line, sample for sample", identical);

        // An EMPTY mask is "every sample valid" (C4) — the state of every series written before the
        // field existed — so it must be the same identity in the other direction.
        const QVariantList ones = cm.windowedMean(t, v, onesMask(n))
                                    .value(QStringLiteral("mean")).toList();
        bool sameAsOnes = ones.size() == none.size();
        for (int i = 0; sameAsOnes && i < none.size(); ++i)
            sameAsOnes = ones.at(i).toDouble() == none.at(i).toDouble();
        checkTrue("all-ones mask ⇒ the same line", sameAsOnes);

        // An empty curve returns two EMPTY lists rather than nothing at all: the QML side reads
        // `wm.mean` unconditionally in _plottable, and a missing key there would decorate `undefined`
        // onto the series and take every trace on the panel with it.
        const QVariantMap empty = cm.windowedMean(QVariantList{}, QVariantList{}, QVariantList{});
        checkTrue("empty curve ⇒ mean key present",  empty.contains(QStringLiteral("mean")));
        checkTrue("empty curve ⇒ sigma key present", empty.contains(QStringLiteral("sigma")));
        checkEqI("empty curve ⇒ empty mean",  empty.value(QStringLiteral("mean")).toList().size(),  0);
        checkEqI("empty curve ⇒ empty sigma", empty.value(QStringLiteral("sigma")).toList().size(), 0);

        // A ONE-SAMPLE curve is a point, not a trace (the chart refuses to plot it), but it must not
        // be a special case here: its own window holds itself, so it draws at its own value.
        const QVariantList one = cm.windowedMean(QVariantList{ qlonglong(0) }, QVariantList{ 7.0 },
                                                 QVariantList{}).value(QStringLiteral("mean")).toList();
        checkEqI("one sample ⇒ one entry", one.size(), 1);
        checkExactD("…at its own value",   one.at(0).toDouble(), 7.0);

        // ⚠ MISMATCHED ARRAYS: the length follows the CURVE (the shorter of the two), which is what
        // makes PpChartPlot._meanOf's `mean.length === t_us.length` guard fire and fall back to the
        // persisted values. A malformed pair draws the raw curve; it does not draw a truncated one.
        QVariantList vShort;
        for (int i = 0; i < 20; ++i) vShort.append(4.0);
        checkEqI("mismatched arrays ⇒ min length",
                 cm.windowedMean(t, vShort, QVariantList{}).value(QStringLiteral("mean")).toList().size(),
                 20);
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

    // ── segments: the chip list, and the SWING entry the chart opens on ───────────
    //
    // The window default moved off the whole recording (seconds of a golfer standing over the ball)
    // onto Address→Finish, and that segment is emitted here rather than derived in QML. What is at
    // risk is not the arithmetic but the three cases where it must NOT appear: no finish resolved,
    // nothing between the two landmarks (the adjacent-pair loop already emits it), and a phase list
    // with neither.
    {
        std::printf("segments — Full, the swing, then the adjacent pairs\n");
        auto ph = [](int phase, qlonglong tUs) {
            return QVariant(QVariantMap{ { QStringLiteral("phase"), phase },
                                         { QStringLiteral("t_us"),  tUs } });
        };
        auto swingOf = [](const QVariantList &segs) {
            for (const QVariant &sv : segs)
                if (sv.toMap().value(QStringLiteral("swing")).toBool()) return sv.toMap();
            return QVariantMap{};
        };

        // Address 200 ms in, Top, Impact, Finish at 1.4 s — inside a 2.5 s recording.
        const QVariantList full{ ph(0, 200000), ph(2, 900000), ph(5, 1200000), ph(7, 1400000) };
        const QVariantList segs = cm.segments(full, 2500000);
        checkEqI("[0] is the whole recording",
                 segs.at(0).toMap().value(QStringLiteral("endUs")).toLongLong(), 2500000);
        const QVariantMap sw = swingOf(segs);
        checkEqI("swing starts at Address", sw.value(QStringLiteral("startUs")).toLongLong(), 200000);
        checkEqI("swing ends at Finish",    sw.value(QStringLiteral("endUs")).toLongLong(), 1400000);
        checkEqI("swing is Address→…",      sw.value(QStringLiteral("phaseA")).toInt(), 0);
        checkEqI("…→Finish",                sw.value(QStringLiteral("phaseB")).toInt(), 7);
        // It sits directly after Full, so the chip lands beside it rather than at the end of a row
        // of pairs.
        checkTrue("swing is chip [1]",
                  segs.at(1).toMap().value(QStringLiteral("swing")).toBool());

        // No Finish resolved ⇒ no swing chip. A window running Address→<axis end> would claim a
        // finish this swing never found.
        const QVariantList noFinish{ ph(0, 200000), ph(2, 900000), ph(5, 1200000) };
        checkTrue("no Finish ⇒ no swing segment", swingOf(cm.segments(noFinish, 2500000)).isEmpty());

        // Address and Finish ADJACENT (nothing P-tagged between them) ⇒ the pair loop already emits
        // exactly this segment, and two identical chips is a bug, not a shortcut.
        const QVariantList bare{ ph(0, 200000), ph(7, 1400000) };
        const QVariantList bareSegs = cm.segments(bare, 2500000);
        checkTrue("adjacent pair ⇒ no duplicate chip", swingOf(bareSegs).isEmpty());
        checkEqI("…and the pair itself is still there", bareSegs.size(), 2);

        // Untagged phases can never be an endpoint — Takeaway (1) between them changes nothing.
        const QVariantList tkw{ ph(0, 200000), ph(1, 400000), ph(7, 1400000) };
        checkTrue("an untagged phase does not separate them",
                  swingOf(cm.segments(tkw, 2500000)).isEmpty());

        checkEqI("no phases ⇒ Full alone", cm.segments(QVariantList{}, 2500000).size(), 1);
    }

    std::printf("\n%s — %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
