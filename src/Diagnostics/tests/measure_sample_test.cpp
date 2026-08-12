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
//
//   cmake --build build/analyzer-tests --target measure_sample_test
//   ctest --test-dir build/analyzer-tests -R measure_sample --output-on-failure

#include "../measure_sample.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <cstdio>

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
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Min)).value_or(-999),
         -20.0, "extremum finds a trough NO endpoint sees");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Top)).value_or(-999), 10.0,
         "…and P4 really does read 10, so the trough was not free");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"), at(Phase::Impact)).value_or(-999), 10.0,
         "…nor does P7 give it away");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Max)).value_or(-999),
         30.0, "the max over the same window is the crest, equally invisible to the endpoints");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Address, Phase::Top, ExtremumSense::Min)).value_or(-999),
         10.0, "a window that ENDS before the dip does not see it");

    // Anchored: signed deviation from the anchor's value, NOT |value - anchor|. Under an absolute
    // reading the Min below would be 0 (the flat stretch is exactly at the anchor); signed, it is
    // the trough's distance beneath address.
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Min, Phase::Address))
             .value_or(-999),
         -30.0, "anchored extremum is the SIGNED deviation (-20 - 10), not |.|");
    near(reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                        extremum(Phase::Top, Phase::Impact, ExtremumSense::Max, Phase::Address))
             .value_or(-999),
         20.0, "…and the Max tail of the same anchored window");
    check(!reduceOverGrid(grid, QStringLiteral("pelvisSway"),
                          extremum(Phase::Top, Phase::Finish, ExtremumSense::Min)).has_value(),
          "extremum over a window whose end was never segmented is unavailable");

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
             -20.0, "the trough survives the round-trip — spans are persisted, not recomputed");

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
