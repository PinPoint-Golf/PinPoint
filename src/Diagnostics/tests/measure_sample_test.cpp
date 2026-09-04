// Reading a MEASURE off a stored swing (src/Diagnostics/measure_sample.h).
//
// This is the half of norm_measure_source.h that nothing implemented until stage 6: the corridor
// editor's live histogram is the safety mechanism against a corridor that grades everyone Action,
// and it only works if there are real numbers under the band.
//
// Four behaviours carry it, each because its failure is silent:
//
//   1. A phase the segmenter never found yields NOTHING, not a nearest-sample guess. A measure
//      graded off a fabricated phase is a confident wrong answer, and the sidecar would cache it.
//   2. Extremum finds a peak the endpoints cannot see. That is the entire reason Extremum is
//      first-class (metric_reducer.h) — a pelvis that sways and recovers before the next phase is
//      invisible to At() and Delta().
//   3. An anchored Extremum is the SIGNED deviation, not |value - anchor|. An absolute deviation
//      cannot carry a `sense`, and every anchored Extremum in the shipped pack means the signed
//      reading.
//   4. The sidecar round-trips exactly AND refuses a stale guard. A cache that answered from a
//      superseded swing.json would be wrong in a way nothing on screen could reveal.
//   5. A span extreme is the extreme of the WINDOWED MEAN, so one wild sample cannot be a peak
//      (schema 3, design §5.2) — and the engine's answer is the SAME NUMBER series_reduce.h gives
//      the review chart, checked here by calling reduceExtremum directly on the same fixture. The
//      corridor is authored by looking at the chart, so if those two ever drifted the band would be
//      drawn around a number the engine never grades against, and both surfaces would look fine.
//
//   cmake --build build/analyzer-tests --target measure_sample_test
//   ctest --test-dir build/analyzer-tests -R measure_sample --output-on-failure

#include "../measure_sample.h"

#include "../../Analysis/series_reduce.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void near(double a, double b, const char *label, double tol = 1e-9)
{
    const bool ok = std::fabs(a - b) <= tol;
    std::printf("  [%s] %s (got %.6f, want %.6f)\n", ok ? "PASS" : "FAIL", label, a, b);
    if (!ok) ++g_fail;
}

namespace {

QJsonObject phaseEvent(Phase p, qint64 tUs)
{
    QJsonObject o;
    o.insert(QStringLiteral("phase"), int(p));
    o.insert(QStringLiteral("t_us"),  tUs);
    o.insert(QStringLiteral("conf"),  0.9);
    return o;
}

// A metric curve sampled every 1 ms over [0, spanUs], value = f(t_us).
QJsonObject metricCurve(const QString &key, const QString &unit, qint64 spanUs,
                        double (*f)(qint64))
{
    QJsonArray t, v;
    for (qint64 x = 0; x <= spanUs; x += 1000) {
        t.append(qint64(x));
        v.append(f(x));
    }
    QJsonObject o;
    o.insert(QStringLiteral("key"),   key);
    o.insert(QStringLiteral("unit"),  unit);
    o.insert(QStringLiteral("t_us"),  t);
    o.insert(QStringLiteral("value"), v);
    return o;
}

// Flat at 10 everywhere a phase is read, with a trough to -20 and a crest to +30 buried strictly
// BETWEEN P4 and P7. Both excursions are invisible to At() and Delta() at every phase — which is
// the whole point of the Extremum assertions below.
double dipCurve(qint64 t)
{
    if (t <   500'000) return  10.0;
    if (t <   600'000) return  10.0 - 30.0 * double(t -   500'000) / 100'000.0;   // -> -20
    if (t <   700'000) return -20.0 + 30.0 * double(t -   600'000) / 100'000.0;   // -> +10
    if (t <   800'000) return  10.0 + 20.0 * double(t -   700'000) / 100'000.0;   // -> +30
    if (t <   900'000) return  30.0 - 20.0 * double(t -   800'000) / 100'000.0;   // -> +10
    return 10.0;
}

double rampCurve(qint64 t) { return double(t) / 100'000.0; }   // 1 at 100 ms, 11 at 1100 ms

// ── The dip fixture's P4→P7 span extremes under the WINDOWED-MEAN rule (schema 3) ────────────────
//
// Derived, not observed, because a number copied out of a failing run pins nothing. The window is
// tuned::reduce::kExtremumWindowUs = 40 ms centred, so ±20 ms, and the curve is sampled every 1 ms:
// 41 samples per window.
//
// The trough is a symmetric tent apexing at −20 at 600 ms with slope 0.3 per ms, so the mean is
// lowest with the window centred exactly on the apex: the 20 samples either side run −14.0 … −19.7
// in 0.3 steps, giving 2·(20·−14 − 0.3·190) + (−20) = −694 over 41 samples.
//   BEFORE (raw-sample extreme): −20.0 exactly, the apex itself.
constexpr double kDipSpanMin = -694.0 / 41.0;    // −16.926829…
// The crest is the same shape apexing at +30 at 800 ms with slope 0.2 per ms:
// 2·(20·26 + 0.2·190) + 30 = 1146 over 41.
//   BEFORE: +30.0 exactly.
constexpr double kDipSpanMax = 1146.0 / 41.0;    //  27.951220…
// With 595…605 ms masked the apex is gone and the deepest window is the one centred on the nearest
// surviving sample (594 ms, symmetrically 606 ms): 30 valid samples summing to −472.2.
//   BEFORE: −18.2, the deepest surviving SAMPLE.
constexpr double kDipSpanMinMasked = -472.2 / 30.0;   // −15.74

// A borrowed view of one metric object's curve, so a test can call the SHARED reducer on exactly the
// samples buildPhaseGrid fed it. The vectors must outlive the view, hence the out-parameters.
SeriesView viewOfMetric(const QJsonObject &metric, std::vector<int64_t> &t,
                        std::vector<double> &v)
{
    const QJsonArray tArr = metric.value(QStringLiteral("t_us")).toArray();
    const QJsonArray vArr = metric.value(QStringLiteral("value")).toArray();
    const int        n    = std::min(tArr.size(), vArr.size());
    t.clear();
    v.clear();
    for (int i = 0; i < n; ++i) {
        t.push_back(tArr.at(i).toVariant().toLongLong());
        v.push_back(vArr.at(i).toDouble());
    }
    SeriesView s;
    s.t = t.data();
    s.v = v.data();
    s.n = t.size();
    return s;
}

QJsonObject metricNamed(const QJsonObject &analysis, const QString &key)
{
    for (const QJsonValue &mv : analysis.value(QStringLiteral("metrics")).toArray())
        if (mv.toObject().value(QStringLiteral("key")).toString() == key)
            return mv.toObject();
    return {};
}

// The same curve plus the parallel `valid` mask of design 5.1: 0 on every sample inside
// [fromUs, toUs] (inclusive), 1 elsewhere. A 0 marks a sample the producer BRIDGED across a gated
// or absent run — the value is interpolation, not measurement, so no reducer may read it.
//
// Synthetic on purpose. The fixtures under tests/data are recorded swings and must keep saying what
// they said; a mask nobody recorded has to be built here.
QJsonObject metricCurveMasked(const QString &key, const QString &unit, qint64 spanUs,
                              double (*f)(qint64), qint64 fromUs, qint64 toUs)
{
    QJsonObject o = metricCurve(key, unit, spanUs, f);
    QJsonArray  valid;
    for (qint64 x = 0; x <= spanUs; x += 1000)
        valid.append((x >= fromUs && x <= toUs) ? 0 : 1);
    o.insert(QStringLiteral("valid"), valid);
    return o;
}

// A flat curve at 4.0 sampled every 8 ms (the pose grid's DENSE spacing) with exactly ONE sample
// replaced by 99 — the Phase 1 spike, the shape the design was written about. 696 ms is chosen
// because it is on the 8 ms grid, sits strictly inside the P4→P7 span, and is more than a phase
// window away from every phase instant, so the spike can only ever reach the SPANS. If it also
// moved a phase median the test would be proving two things and pinning neither.
QJsonObject metricSpike(const QString &key, const QString &unit)
{
    QJsonArray t, v;
    for (qint64 x = 0; x <= 1'200'000; x += 8'000) {
        t.append(qint64(x));
        v.append(x == 696'000 ? 99.0 : 4.0);
    }
    QJsonObject o;
    o.insert(QStringLiteral("key"),   key);
    o.insert(QStringLiteral("unit"),  unit);
    o.insert(QStringLiteral("t_us"),  t);
    o.insert(QStringLiteral("value"), v);
    return o;
}

// Replace one metric object in an analysis by key. The fixture is shared, so every masked case below
// is "the same swing with one channel masked" rather than a second fixture that could drift.
QJsonObject withMetric(QJsonObject an, const QJsonObject &replacement)
{
    const QString key = replacement.value(QStringLiteral("key")).toString();
    QJsonArray    ms  = an.value(QStringLiteral("metrics")).toArray();
    for (int i = 0; i < ms.size(); ++i)
        if (ms.at(i).toObject().value(QStringLiteral("key")).toString() == key)
            ms.replace(i, replacement);
    an.insert(QStringLiteral("metrics"), ms);
    return an;
}

// P1 / P4 / P7 — the three phases the local corpus actually carries on every swing, so the fixture
// has the shape real data has.
//
// The curve deliberately extends 100 ms PAST P7 and starts 100 ms BEFORE P1, so every phase gets a
// SYMMETRIC ±15 ms median window. Sampling at a curve's own first or last point truncates the
// window to one side and biases the median inward — real behaviour, asserted on its own below, but
// mixing it into the reducer arithmetic would test two things at once and pin neither.
QJsonObject fixtureAnalysis()
{
    QJsonArray phases;
    phases.append(phaseEvent(Phase::Address,   100'000));
    phases.append(phaseEvent(Phase::Top,       400'000));
    phases.append(phaseEvent(Phase::Impact,  1'100'000));

    QJsonArray metrics;
    metrics.append(metricCurve(QStringLiteral("pelvisSway"),     QStringLiteral("mm"), 1'200'000, dipCurve));
    metrics.append(metricCurve(QStringLiteral("thoraxRotation"), QStringLiteral("°"),  1'200'000, rampCurve));

    QJsonObject an;
    an.insert(QStringLiteral("phases"),  phases);
    an.insert(QStringLiteral("metrics"), metrics);
    return an;
}

// A DEGENERATE metric: no curve at all, just one labelled scalar. This is the
// representation every point-in-time producer uses (foot_metrics.h, tempo_metrics.cpp,
// and shaft_plane's ShaftPlaneStage), and it reduces by a different code path from a
// sampled curve — buildPhaseGrid's `labelled` fallback rather than a windowed median.
QJsonObject metricScalar(const QString &key, const QString &unit,
                         Phase phase, qint64 tUs, double value)
{
    QJsonObject ps;
    ps.insert(QStringLiteral("phase"), int(phase));
    ps.insert(QStringLiteral("t_us"),  tUs);
    ps.insert(QStringLiteral("value"), value);

    QJsonObject o;
    o.insert(QStringLiteral("key"),          key);
    o.insert(QStringLiteral("unit"),         unit);
    o.insert(QStringLiteral("t_us"),         QJsonArray{});
    o.insert(QStringLiteral("value"),        QJsonArray{});
    o.insert(QStringLiteral("phaseSamples"), QJsonArray{ ps });
    return o;
}

Reducer at(Phase p)
{
    Reducer r;
    r.kind   = ReducerKind::At;
    r.anchor = p;
    return r;
}

Reducer delta(Phase from, Phase to)
{
    Reducer r;
    r.kind   = ReducerKind::Delta;
    r.anchor = from;
    r.window = { from, to };
    return r;
}

Reducer rate(Phase from, Phase to)
{
    Reducer r = delta(from, to);
    r.kind    = ReducerKind::Rate;
    return r;
}

Reducer extremum(Phase from, Phase to, ExtremumSense s, std::optional<Phase> anchor = std::nullopt)
{
    Reducer r;
    r.kind   = ReducerKind::Extremum;
    r.window = { from, to };
    r.sense  = s;
    r.anchor = anchor;
    return r;
}

} // namespace

int main()
{
    std::printf("measure_sample_test\n");

    const SwingPhaseGrid grid = buildPhaseGrid(fixtureAnalysis());

    // ── The grid itself ─────────────────────────────────────────────────────
    std::printf("\ngrid construction\n");
    check(grid.metrics.size() == 2, "both metrics present");
    const MetricPhaseGrid *sway = grid.metric(QStringLiteral("pelvisSway"));
    check(sway != nullptr, "pelvisSway resolves by key");
    check(grid.metric(QStringLiteral("noSuchMetric")) == nullptr, "unknown key resolves to nothing");
    if (sway) {
        check(sway->values.size() == 3, "three segmented phases yield three values");
        check(sway->spans.size() == 2, "three values yield two adjacent spans");
        check(sway->unit == QStringLiteral("mm"), "unit carried through");
        check(sway->at(Phase::Release) == nullptr, "an unsegmented phase has no value");
    }

    // ── At ──────────────────────────────────────────────────────────────────
    std::printf("\nat\n");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Address)).value_or(-999), 10.0,
         "at P1 reads the windowed median");
    near(reduceOverGrid(grid, QStringLiteral("thoraxRotation"), at(Phase::Top)).value_or(-999), 4.0,
         "at P4 on the ramp");
    check(!reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Finish)).has_value(),
          "at an UNSEGMENTED phase is unavailable, not a nearest-sample guess");
    check(!reduceOverGrid(grid, QStringLiteral("nope"), at(Phase::Address)).has_value(),
          "at on a metric the swing does not carry is unavailable");

    // ── A labelled scalar at a phase the LADDER does not carry ──────────────
    // m_transitionPlaneDelta reduces `at` the `transition` anchor, and real swings
    // do not reliably emit a Transition event — the observed ladder on a corpus
    // swing runs Address/Takeaway/ShaftParallelBack/MidBackswing/Top/… with no
    // Transition at all. What binds the reducer is therefore the PHASE LABEL the
    // producer stamps on its own phaseSample, not anything the segmenter found.
    // Pin both halves: the label resolves, and the timestamp alone does not.
    // Stamping Phase::Top here (the same instant) would silently resolve nothing.
    {
        QJsonObject an = fixtureAnalysis();
        QJsonArray metrics = an.value(QStringLiteral("metrics")).toArray();
        metrics.append(metricScalar(QStringLiteral("transitionPlaneDelta"),
                                    QStringLiteral("°"), Phase::Transition, 400'000, 13.76));
        an.insert(QStringLiteral("metrics"), metrics);
        const SwingPhaseGrid g = buildPhaseGrid(an);

        near(reduceOverGrid(g, QStringLiteral("transitionPlaneDelta"),
                            at(Phase::Transition)).value_or(-999), 13.76,
             "a labelled scalar resolves `at` its own phase, with no ladder event there");
        check(!reduceOverGrid(g, QStringLiteral("transitionPlaneDelta"),
                              at(Phase::Top)).has_value(),
              "...and NOT at the segmented phase sharing its timestamp — the label binds");
    }

    // ── Delta and Rate ──────────────────────────────────────────────────────
    std::printf("\ndelta / rate\n");
    near(reduceOverGrid(grid, QStringLiteral("thoraxRotation"),
                        delta(Phase::Address, Phase::Impact)).value_or(-999), 10.0,
         "delta P1->P7 on the ramp");
    near(reduceOverGrid(grid, QStringLiteral("thoraxRotation"),
                        delta(Phase::Top, Phase::Impact)).value_or(-999), 7.0,
         "delta runs from the ANCHOR, not the window start");
    near(reduceOverGrid(grid, QStringLiteral("thoraxRotation"),
                        rate(Phase::Address, Phase::Impact)).value_or(-999), 10.0,
         "rate divides by elapsed SECONDS (10 deg over 1.0 s)");
    check(!reduceOverGrid(grid, QStringLiteral("thoraxRotation"),
                          delta(Phase::Address, Phase::Release)).has_value(),
          "delta to an unsegmented end is unavailable");

    // The median window is TRUNCATED at a curve's own endpoints — a real property of the
    // convention, pinned here rather than left to surprise someone. A phase landing on the last
    // sample sees only the preceding half-window, so the median is biased inward: on a ramp
    // reaching 12.0 at 1200 ms, the last 15 ms average to 11.925, not 12.0.
    std::printf("\nendpoint truncation\n");
    {
        QJsonObject an = fixtureAnalysis();
        QJsonArray  ph;
        ph.append(phaseEvent(Phase::Impact, 1'200'000));   // exactly the curve's last sample
        an.insert(QStringLiteral("phases"), ph);
        const SwingPhaseGrid g = buildPhaseGrid(an);
        near(reduceOverGrid(g, QStringLiteral("thoraxRotation"), at(Phase::Impact)).value_or(-999),
             11.925, "a one-sided window biases the median inward, and says so");
    }

    // ── Extremum ────────────────────────────────────────────────────────────
    std::printf("\nextremum\n");
    // The trough bottoms at -20 and the crest tops at +30, both strictly between P4 (400 ms) and
    // P7 (1100 ms). Every phase reads 10, so ONLY the span aggregation can see either.
    //
    // The REPORTED extreme is the windowed mean at the apex, not the apex sample (schema 3): -16.93
    // rather than -20, and +27.95 rather than +30, both derived above. A tent 100 ms wide loses
    // about 3 of its 30 to a 40 ms window, which is the honest cost of the rule — a real excursion
    // is still nearly all there, while a one-sample spike (below) loses four fifths of itself.
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
         kDipSpanMin, "extremum finds a trough NO endpoint sees");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Top)).value_or(-999), 10.0,
         "…and P4 really does read 10, so the trough was not free");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Impact)).value_or(-999), 10.0,
         "…nor does P7 give it away");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Max)).value_or(-999),
         kDipSpanMax, "the max over the same window is the crest, equally invisible to the endpoints");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Address, Phase::Top, ExtremumSense::Min)).value_or(-999),
         10.0, "a window that ENDS before the dip does not see it");

    // Anchored: signed deviation from the anchor's value, NOT |value - anchor|. Under an absolute
    // reading the Min below would be 0 (the flat stretch is exactly at the anchor); signed, it is
    // the trough's distance beneath address.
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Min, Phase::Address))
             .value_or(-999),
         kDipSpanMin - 10.0, "anchored extremum is the SIGNED deviation (trough - 10), not |.|");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Max, Phase::Address))
             .value_or(-999),
         kDipSpanMax - 10.0, "…and the Max tail of the same anchored window");
    check(!reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                          extremum(Phase::Top, Phase::Finish, ExtremumSense::Min)).has_value(),
          "extremum over a window whose end was never segmented is unavailable");

    // ── ONE WILD SAMPLE IS NOT A PEAK ───────────────────────────────────────
    //
    // The case the whole of design §5.2 exists for. Before this, a span's extreme was the extreme
    // RAW SAMPLE, so a single frame of pose noise became the cached peak, got graded against a
    // corridor, and printed on the card as the golfer's number. Nothing about a max says how long
    // its value was held, so nothing could tell that reading from a real excursion.
    //
    // Arithmetic, so this pins a rule and not a run: the curve is flat at 4.0 every 8 ms with one
    // sample at 99. A ±20 ms window on an 8 ms grid holds offsets 0, ±8, ±16 — FIVE samples — so the
    // best any window can do with one spike in it is (4·4 + 99) / 5 = 4 + 95/5 = 23. It is attained:
    // every centre within 16 ms of the spike sees exactly 5 samples, one of them 99.
    std::printf("\nthe spike: a peak has to be there for 40 ms\n");
    {
        QJsonObject an      = fixtureAnalysis();
        QJsonArray  metrics = an.value(QStringLiteral("metrics")).toArray();
        metrics.append(metricSpike(QStringLiteral("spiked"), QStringLiteral("%")));
        an.insert(QStringLiteral("metrics"), metrics);
        const SwingPhaseGrid   g = buildPhaseGrid(an);
        const MetricPhaseGrid *m = g.metric(QStringLiteral("spiked"));

        check(m != nullptr && m->values.size() == 3 && m->spans.size() == 2,
              "the spiked metric grids over the same three phases");
        if (m) {
            near(m->values[0].value, 4.0, "the spike is far from every phase window: P1 reads 4");
            near(m->values[2].value, 4.0, "…and so does P7");
            // spans[1] is P4->P7, the span the spike sits inside.
            near(m->spans[1].max, 23.0, "the span max is the windowed-mean bound, 4 + 95/5");
            check(m->spans[1].max < 30.0,
                  "…which is WELL below the 99 the raw-sample rule would have cached");
            near(m->spans[1].min, 4.0, "…and the min is the flat curve, untouched by the spike");
            near(m->spans[0].max, 4.0, "the span the spike is NOT in stays flat");
        }
        near(reduceOverGrid(g, QStringLiteral("spiked"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Max)).value_or(-999),
             23.0, "…and that is the number a Measure reduces to, not 99");
    }

    // ── CARD-VS-ENGINE AGREEMENT, BY CONSTRUCTION ───────────────────────────
    //
    // Design §7 item 5: for every authored extremum measure the chart summary over the same window
    // must report the same number. The chart calls reduceExtremum on the series; the engine calls it
    // on the same series and caches the answer in a span. So call it BOTH ways here on one fixture
    // and require equality — the useful failure is not a wrong value, it is the two paths drifting
    // apart later while each stays plausible on its own.
    //
    // The engine additionally admits the two endpoint MEDIANS into its search (a peak sitting
    // exactly on a phase is still the peak), so equality is exact precisely when the span extreme
    // dominates them. That is the case for any genuine excursion, and it is the case here: the dip
    // reaches -16.93 and the crest +27.95 either side of endpoints that both read 10.
    std::printf("\nagreement with the shared reducer\n");
    {
        const QJsonObject an = fixtureAnalysis();
        std::vector<int64_t> ts;
        std::vector<double>  vs;
        const SeriesView     view = viewOfMetric(metricNamed(an, QStringLiteral("pelvisSway")),
                                                 ts, vs);
        ReduceConfig rc;                       // the engine's defaults, unmodified
        const int64_t p4 = 400'000, p7 = 1'100'000;

        // (from, to] — the same half-open span the grid stores, spelled the same way.
        const Reduced mn = reduceExtremum(view, p4 + 1, p7, /*wantMax=*/false, rc);
        const Reduced mx = reduceExtremum(view, p4 + 1, p7, /*wantMax=*/true,  rc);
        check(mn.ok && mx.ok, "the shared reducer answers over the span directly");

        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
             mn.value, "engine Min == reduceExtremum(min) on the same samples");
        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Max)).value_or(-999),
             mx.value, "engine Max == reduceExtremum(max) on the same samples");

        // The ANCHORED (signed-deviation) form too, since that is what the shipped pack authors.
        const Reduced anchor = reduceAt(view, 100'000, rc);
        check(anchor.ok, "…and the anchor's own median comes from reduceAt");
        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min, Phase::Address))
                 .value_or(-999),
             mn.value - anchor.value, "anchored Min == reduceExtremum(min) - reduceAt(anchor)");
        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Max, Phase::Address))
                 .value_or(-999),
             mx.value - anchor.value, "anchored Max == reduceExtremum(max) - reduceAt(anchor)");

        // And the phase values are the shared reducer's too, not a second median convention.
        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Top)).value_or(-999),
             reduceAt(view, p4, rc).value, "engine At == reduceAt at the same instant");
    }

    // ── phaseSamples: the only data a whole class of metric has ─────────────
    //
    // Every setup metric in a real swing.json (stanceWidth, ballPosition, tempoRatio, foot flare,
    // toe line) ships with an EMPTY curve and nothing but phaseSamples. A builder that read only
    // `value[]` produced nothing for all of them, and the symptom is indistinguishable from "no
    // swing carries this measure" — which is exactly how it was missed.
    std::printf("\nphaseSamples fallback\n");
    {
        QJsonArray ps;
        QJsonObject s1;
        s1.insert(QStringLiteral("phase"), int(Phase::Address));
        s1.insert(QStringLiteral("t_us"),  100'000);
        s1.insert(QStringLiteral("value"), 41.5);
        ps.append(s1);
        // A phase the LADDER does not carry, labelled by the metric's own producer.
        QJsonObject s2;
        s2.insert(QStringLiteral("phase"), int(Phase::Delivery));
        s2.insert(QStringLiteral("t_us"),  900'000);
        s2.insert(QStringLiteral("value"), 12.0);
        ps.append(s2);

        QJsonObject curveless;
        curveless.insert(QStringLiteral("key"),          QStringLiteral("stanceWidth"));
        curveless.insert(QStringLiteral("unit"),         QStringLiteral("%"));
        curveless.insert(QStringLiteral("t_us"),         QJsonArray{});
        curveless.insert(QStringLiteral("value"),        QJsonArray{});
        curveless.insert(QStringLiteral("phaseSamples"), ps);

        QJsonObject an      = fixtureAnalysis();
        QJsonArray  metrics = an.value(QStringLiteral("metrics")).toArray();
        metrics.append(curveless);
        an.insert(QStringLiteral("metrics"), metrics);

        const SwingPhaseGrid g = buildPhaseGrid(an);
        check(g.metric(QStringLiteral("stanceWidth")) != nullptr,
              "a metric with an EMPTY curve is still in the grid");
        near(reduceOverGrid(g, QStringLiteral("stanceWidth"), at(Phase::Address)).value_or(-999),
             41.5, "…and reads its phaseSample");
        near(reduceOverGrid(g, QStringLiteral("stanceWidth"), at(Phase::Delivery)).value_or(-999),
             12.0, "a phaseSample at a phase the LADDER lacks is still answerable");
        check(!reduceOverGrid(g, QStringLiteral("stanceWidth"), at(Phase::Top)).has_value(),
              "…but a phase with neither curve nor phaseSample stays unavailable");

        // The curve WINS where it has samples: one convention, not two. thoraxRotation carries a
        // real curve, so a contradictory phaseSample must not displace the median.
        QJsonArray  lie;
        QJsonObject l;
        l.insert(QStringLiteral("phase"), int(Phase::Top));
        l.insert(QStringLiteral("t_us"),  400'000);
        l.insert(QStringLiteral("value"), -999.0);
        lie.append(l);
        QJsonArray m2 = an.value(QStringLiteral("metrics")).toArray();
        for (int i = 0; i < m2.size(); ++i) {
            QJsonObject o = m2.at(i).toObject();
            if (o.value(QStringLiteral("key")).toString() == QLatin1String("thoraxRotation")) {
                o.insert(QStringLiteral("phaseSamples"), lie);
                m2.replace(i, o);
            }
        }
        an.insert(QStringLiteral("metrics"), m2);
        const SwingPhaseGrid g2 = buildPhaseGrid(an);
        near(reduceOverGrid(g2, QStringLiteral("thoraxRotation"), at(Phase::Top)).value_or(-999),
             4.0, "the windowed median WINS over a phaseSample where the curve has samples");
    }

    // ── The validity mask ───────────────────────────────────────────────────
    //
    // A `valid` 0 says the sample was BRIDGED across a gated or absent run, not measured (design
    // 5.1 / metric_channel.h). The curve keeps its continuity for the renderer; the number does not
    // get to be graded. Three things follow, and each fails silently if it is wrong: the median
    // must skip masked samples, a span's extremes must skip them, and a phase whose whole window is
    // masked must take the existing "no entry" path rather than reporting a bridged number.
    std::printf("\nvalidity mask\n");
    {
        // (a) The MEDIAN. P4 sits at 400 ms on a 1-ms ramp reading t/100 ms, so its +-15 ms window
        // spans 385..415 ms and medians to exactly 4.00. Mask 400..415 ms — the phase instant and
        // everything after it — and only 385..399 survive, 15 samples medianing to 3.92. Two
        // different numbers, so the skip cannot be a no-op that happens to agree.
        const SwingPhaseGrid masked = buildPhaseGrid(withMetric(
            fixtureAnalysis(),
            metricCurveMasked(QStringLiteral("thoraxRotation"), QStringLiteral("°"),
                              1'200'000, rampCurve, 400'000, 415'000)));
        near(reduceOverGrid(grid,   QStringLiteral("thoraxRotation"), at(Phase::Top)).value_or(-999),
             4.00, "unmasked, P4 medians the whole +-15 ms window");
        near(reduceOverGrid(masked, QStringLiteral("thoraxRotation"), at(Phase::Top)).value_or(-999),
             3.92, "masked, the bridged half of the window is not in the median");

        // (b) The SPAN EXTREMES. The dip bottoms at -20 at 600 ms, strictly between P4 and P7 and
        // invisible to both endpoints — it is the only thing the span aggregation can see. Mask
        // 595..605 ms and both the apex and its whole neighbourhood are gone: the deepest window is
        // the one centred on the nearest surviving sample, 594 ms (or 606 ms, symmetrically), and it
        // averages -15.74 over the 30 samples that remain inside it. A reducer that still answered
        // the unmasked -16.93 would be reading bridged samples and nothing else would show it.
        const SwingPhaseGrid dipMasked = buildPhaseGrid(withMetric(
            fixtureAnalysis(),
            metricCurveMasked(QStringLiteral("pelvisSway"), QStringLiteral("mm"),
                              1'200'000, dipCurve, 595'000, 605'000)));
        near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
             kDipSpanMin, "unmasked, the trough reads the windowed mean at its apex");
        near(reduceOverGrid(dipMasked, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
             kDipSpanMinMasked,
             "masked, the extreme is the deepest VALID window, not one holding bridged samples");
        near(reduceOverGrid(dipMasked, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Max)).value_or(-999),
             kDipSpanMax, "…and the crest, far from the mask, is untouched");
        near(reduceOverGrid(dipMasked, QStringLiteral("pelvisSway"), at(Phase::Top)).value_or(-999),
             10.0, "…as are the phase medians either side of it");

        // (c) A PHASE WHOSE WHOLE WINDOW IS MASKED gets NO ENTRY. Not a zero, not the nearest valid
        // sample: "assessed and fine" and "not assessed" are different statements. P1 is at 100 ms,
        // so masking 85..115 ms empties its window entirely, and pelvisSway carries no phaseSamples
        // to fall back on.
        const SwingPhaseGrid noP1 = buildPhaseGrid(withMetric(
            fixtureAnalysis(),
            metricCurveMasked(QStringLiteral("pelvisSway"), QStringLiteral("mm"),
                              1'200'000, dipCurve, 85'000, 115'000)));
        const MetricPhaseGrid *ms = noP1.metric(QStringLiteral("pelvisSway"));
        check(ms != nullptr && ms->at(Phase::Address) == nullptr,
              "a phase whose whole +-15 ms window is bridged has NO grid entry");
        check(!reduceOverGrid(noP1, QStringLiteral("pelvisSway"), at(Phase::Address)).has_value(),
              "…so reading it is unavailable, not a bridged number");
        check(ms != nullptr && ms->values.size() == 2 && ms->spans.size() == 1,
              "…and the grid re-spans over the two phases that remain");
        // The channel is not written off: the phases with valid windows still answer.
        near(reduceOverGrid(noP1, QStringLiteral("pelvisSway"), at(Phase::Impact)).value_or(-999),
             10.0, "…while P7, whose window is clean, still reads");

        // (d) ABSENT MEANS EVERY SAMPLE COUNTS. The key is written only when something is invalid
        // (the `sigma` discipline), so reading a swing that carries no mask must give EXACTLY the
        // grid an all-ones mask gives: that equivalence is what makes the mask additive, and it is
        // the reason a `valid` array appearing in a swing.json does not by itself change a number.
        // (It is no longer a claim about the previous RELEASE — schema 3 moved every span. The
        // `values` are untouched, and those are what the mask can reach.) An all-ones mask is never
        // written, but reading one has to be harmless, and comparing the serialised sidecars proves
        // it field by field rather than value by value.
        const SwingPhaseGrid allOnes = buildPhaseGrid(withMetric(
            fixtureAnalysis(),
            metricCurveMasked(QStringLiteral("pelvisSway"), QStringLiteral("mm"),
                              1'200'000, dipCurve, -2, -1)));   // no sample in [-2,-1] => all valid
        check(savePhaseGrid(allOnes, 1, 1) == savePhaseGrid(grid, 1, 1),
              "an all-valid mask grids byte-identically to no mask at all");

        // (e) A SHORT MASK IS NO MASK. `valid` is contractually parallel to `t_us`, so an array
        // that does not reach the end of the curve is a malformed document rather than a partial
        // statement — and there is no honest way to read one. Treating the missing tail as VALID
        // invents measurements nobody vouched for; treating it as INVALID throws away readings
        // nobody impeached; and either guess depends on assuming which end was truncated. So the
        // whole mask is ignored and the metric grids exactly as an unmasked one, which is the
        // answer that claims nothing. Pinned because the alternative fails silently: a mask short
        // by one would otherwise shift every sample's validity by one position.
        QJsonObject shortMask = metricCurveMasked(QStringLiteral("pelvisSway"),
                                                  QStringLiteral("mm"), 1'200'000, dipCurve,
                                                  595'000, 605'000);
        QJsonArray  trimmed = shortMask.value(QStringLiteral("valid")).toArray();
        trimmed.removeLast();                      // n - 1: one short of t_us
        shortMask.insert(QStringLiteral("valid"), trimmed);
        const SwingPhaseGrid shorted = buildPhaseGrid(withMetric(fixtureAnalysis(), shortMask));
        check(savePhaseGrid(shorted, 1, 1) == savePhaseGrid(grid, 1, 1),
              "a `valid` array one entry short of t_us is ignored entirely — the grid is the "
              "unmasked one, not the masked one shifted by a sample");
        // And it really is the UNMASKED answer, not a coincidence: the same mask at full length
        // moves the trough, which (b) above proved.
        near(reduceOverGrid(shorted, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
             kDipSpanMin, "…so the trough it would have masked is still there");
    }

    // ── A Measure, not just a reducer ───────────────────────────────────────
    std::printf("\nmeasure binding\n");
    {
        Measure m;
        m.id        = QStringLiteral("m_test");
        m.kind      = MeasureKind::Provided;
        m.metricKey = QStringLiteral("thoraxRotation");
        m.reducer   = at(Phase::Impact);
        near(reduceOverGrid(grid, m).value_or(-999), 11.0, "a Provided measure reduces by metricKey");

        Measure composed;
        composed.id      = QStringLiteral("m_composed");
        composed.kind    = MeasureKind::Composed;
        composed.reducer = at(Phase::Impact);
        check(!reduceOverGrid(grid, composed).has_value(),
              "a Composed measure names facets, not a catalogue key — unavailable here");
    }

    // ── Unsegmented swing ───────────────────────────────────────────────────
    std::printf("\nunsegmented swing\n");
    {
        QJsonObject an = fixtureAnalysis();
        an.insert(QStringLiteral("phases"), QJsonArray{});
        const SwingPhaseGrid g = buildPhaseGrid(an);
        check(g.isEmpty(), "no phases means nothing is producible — not a grid of zeroes");
    }

    // A phase int that is not a real enumerator must be dropped, never cast into its neighbour.
    std::printf("\nbad phase int\n");
    {
        QJsonObject an = fixtureAnalysis();
        QJsonArray  ph = an.value(QStringLiteral("phases")).toArray();
        QJsonObject bogus;
        bogus.insert(QStringLiteral("phase"), 99);
        bogus.insert(QStringLiteral("t_us"),  500'000);
        ph.append(bogus);
        an.insert(QStringLiteral("phases"), ph);
        const SwingPhaseGrid g = buildPhaseGrid(an);
        const MetricPhaseGrid *m = g.metric(QStringLiteral("pelvisSway"));
        check(m && m->values.size() == 3, "an unknown phase int is dropped, not admitted as a phase");
    }

    // ── Sidecar round-trip and its guard ────────────────────────────────────
    std::printf("\nsidecar\n");
    {
        const qint64 size = 12345, mtime = 1700000000000LL;
        SwingPhaseGrid src = grid;
        src.sessionId   = QStringLiteral("2026-07-10_Session_01");
        src.club        = QStringLiteral("7 IRON");
        src.ordinal     = 4;
        src.wallclockMs = 1700000001234LL;

        const QJsonObject doc = savePhaseGrid(src, size, mtime);

        bool ok = false;
        const SwingPhaseGrid back = loadPhaseGrid(doc, size, mtime, &ok);
        check(ok, "a matching guard loads");
        check(back.sessionId == src.sessionId && back.club == src.club
                  && back.ordinal == src.ordinal && back.wallclockMs == src.wallclockMs,
              "identity fields round-trip");
        check(back.metrics.size() == src.metrics.size(), "metric count round-trips");
        near(reduceOverGrid(back, QStringLiteral("pelvisSway"),
                            extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
             kDipSpanMin, "the trough survives the round-trip — spans are persisted, not recomputed");

        bool stale = true;
        const SwingPhaseGrid s1 = loadPhaseGrid(doc, size + 1, mtime, &stale);
        check(!stale && s1.isEmpty(), "a changed SIZE invalidates the cache");
        stale = true;
        const SwingPhaseGrid s2 = loadPhaseGrid(doc, size, mtime + 1, &stale);
        check(!stale && s2.isEmpty(), "a changed MTIME invalidates the cache");

        QJsonObject future = doc;
        future.insert(QStringLiteral("schema"), kPhaseGridSchemaVersion + 1);
        stale = true;
        const SwingPhaseGrid s3 = loadPhaseGrid(future, size, mtime, &stale);
        check(!stale && s3.isEmpty(), "a NEWER schema is rebuilt, never partially read");

        // THE SCHEMA BUMP IS THE ONLY THING THAT RETIRES A v2 SIDECAR, and that is why it is
        // asserted rather than left to a code review. Schema 3 changed every span (the windowed-mean
        // extreme), but it rewrote no swing.json — so a v2 sidecar's size+mtime guard still MATCHES,
        // and without the bump every cached grid in the library would keep serving raw-sample peaks
        // to the corridor editor with nothing anywhere saying so.
        check(kPhaseGridSchemaVersion == 3,
              "the sidecar schema is 3: spans are windowed-mean extremes (design 5.2)");
        QJsonObject v2 = doc;
        v2.insert(QStringLiteral("schema"), 2);
        stale = true;
        const SwingPhaseGrid s4 = loadPhaseGrid(v2, size, mtime, &stale);
        check(!stale && s4.isEmpty(),
              "…and a v2 sidecar is discarded even though its size+mtime guard still matches");
    }

    // ── The instrument ladder: Measure::preferKeys ──────────────────────────────
    //
    // A measure may name better instruments to try before its own key. The rule that carries the
    // weight is that the ladder walks until one ANSWERS, not until one EXISTS — a launch monitor
    // can report a shot and omit a column, and a preferred key that is present but silent at this
    // phase has to fall through rather than dark the measure.
    {
        const auto ladderMeasure = [](const char *own, std::initializer_list<const char *> prefer) {
            Measure m;
            m.id             = QStringLiteral("m_test");
            m.kind           = MeasureKind::Provided;
            m.metricKey      = QString::fromLatin1(own);
            for (const char *k : prefer) m.preferKeys << QString::fromLatin1(k);
            m.reducer.kind   = ReducerKind::At;
            m.reducer.anchor = Phase::Address;
            return m;
        };

        // pelvisSway reads 10.0 at Address in the fixture; thoraxRotation is present but has no
        // Address sample, and `nope` is absent entirely.
        near(reduceOverGrid(grid, ladderMeasure("pelvisSway", {})).value_or(-999), 10.0,
             "no ladder: the measure's own key, exactly as before");
        near(reduceOverGrid(grid, ladderMeasure("pelvisSway", { "nope" })).value_or(-999), 10.0,
             "a preferred key that does not exist falls through to our own");

        // THE CASE THE WHOLE DESIGN TURNS ON. `partial` is in the grid, but it carries only a
        // Transition sample — the shape of a device that reported the shot and omitted this column
        // at the phase we want. A lookup that stopped at "the preferred metric exists" would take
        // it, find nothing at Address, and report the measure unavailable while a perfectly good
        // reading sat on the very next rung.
        {
            QJsonObject an      = fixtureAnalysis();
            QJsonArray  metrics = an.value(QStringLiteral("metrics")).toArray();
            metrics.append(metricScalar(QStringLiteral("partial"), QStringLiteral("mm"),
                                        Phase::Transition, 400'000, 99.0));
            an.insert(QStringLiteral("metrics"), metrics);
            const SwingPhaseGrid g = buildPhaseGrid(an);

            check(g.metric(QStringLiteral("partial")) != nullptr,
                  "the preferred metric really is present in the grid");
            near(reduceOverGrid(g, ladderMeasure("pelvisSway", { "partial" })).value_or(-999), 10.0,
                 "a preferred key present but silent at this phase falls through too");
        }

        // And when the preferred rung does answer, it wins — that is the point of preferring it.
        Measure top = ladderMeasure("pelvisSway", { "thoraxRotation" });
        top.reducer.anchor = Phase::Top;
        near(reduceOverGrid(grid, top).value_or(-999), 4.0,
             "…and where the preferred key answers, it is the one that answers");

        check(!reduceOverGrid(grid, ladderMeasure("nope", { "alsoNope" })).has_value(),
              "no rung answers, no value");
    }

    std::printf("\n%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
