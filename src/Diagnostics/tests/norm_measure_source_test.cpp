// The join that lights the pack up: values + norms -> a graded MeasureReading
// (src/Diagnostics/norm_measure_source.h) and the engine's corridor test on top of it.
//
// Three behaviours carry it, and each exists because the failure it prevents is silent:
//
//   1. A signal fires on a DEVIATION (Watch/Action), not merely on leaving the Ideal band. Ideal is
//      |z| <= 1, so firing there would trip about a third of any normal population on every
//      characteristic in the library.
//   2. An unknown context resolves to NOTHING, so the characteristic reports Unavailable. Grading
//      a bunker shot against a full-swing norm produces a confident, plausible, wrong finding.
//   3. An undeclared context resolves to the default but is MARKED, demoting confidence. The
//      deviation is real; what is uncertain is whether the right norm judged it.
//
//   cmake --build build/analyzer-tests --target norm_measure_source_test
//   ctest --test-dir build/analyzer-tests -R norm_measure_source --output-on-failure

#include "../norm_measure_source.h"

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

class FakeValues final : public IMeasureValueSource {
public:
    void set(const QString &id, double v, float conf = 1.0f) { m[id] = { v, conf }; }
    std::optional<Value> value(const QString &id) const override
    {
        const auto it = m.find(id);
        return it == m.end() ? std::nullopt : std::optional<Value>(it->second);
    }
private:
    std::map<QString, Value> m;
};

class FakeNorms final : public INormProvider {
public:
    FakeNorms()
    {
        m_contexts = ContextTree(std::vector<ContextNode>{
            { QStringLiteral("any"),        QStringLiteral("Any"),   QString() },
            { QStringLiteral("full_swing"), QStringLiteral("Full"),  QStringLiteral("any") },
            { QStringLiteral("driver"),     QStringLiteral("Drv"),   QStringLiteral("full_swing") },
            { QStringLiteral("wedge"),      QStringLiteral("Wdg"),   QStringLiteral("full_swing") },
        });
    }
    void add(const char *measure, const char *context, double mu, double sigma)
    {
        Norm n;
        n.measureId = QLatin1String(measure);
        n.contextId = QLatin1String(context);
        n.mu        = mu;
        n.sigmaLo = n.sigmaHi = sigma;
        m_norms.upsert(n);
    }
    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return QStringLiteral("fake"); }
    PackOrigin              origin() const override { return PackOrigin::Core; }
private:
    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
};

int main()
{
    auto norms = std::make_shared<FakeNorms>();
    norms->add("m_ballPosition", "full_swing", 50.0, 10.0);   // ideal 40..60
    norms->add("m_ballPosition", "driver",     10.0, 5.0);    // ideal  5..15

    std::printf("=== the corridor finally exists ===\n");
    {
        FakeValues v;
        v.set(QStringLiteral("m_ballPosition"), 50.0);
        const NormMeasureSource src(v, norms, QStringLiteral("full_swing"));

        const auto r = src.read(QStringLiteral("m_ballPosition"));
        check(r.has_value(), "a measure with a value reads");
        check(r && r->hasCorridor, "hasCorridor is TRUE — the thing that was never true before");
        check(r && r->greenLo == 40.0 && r->greenHi == 60.0, "the ideal band is the norm's");
        check(r && r->grade == Grade::Ideal, "a centred value grades Ideal");
        check(r && r->normContextId == QLatin1String("full_swing"), "the resolved context is reported");
        check(r && !r->contextInferred, "a declared context is not marked inferred");
    }

    std::printf("=== grades across the bands ===\n");
    {
        struct Case { double v; Grade want; const char *label; };
        const Case cases[] = {
            { 50.0, Grade::Ideal,  "centre -> Ideal" },
            { 60.0, Grade::Ideal,  "1 sigma -> Ideal" },
            { 65.0, Grade::Good,   "1.5 sigma -> Good" },
            { 75.0, Grade::Watch,  "2.5 sigma -> Watch" },
            { 90.0, Grade::Action, "4 sigma -> Action" },
        };
        bool ok = true;
        for (const Case &c : cases) {
            FakeValues v;
            v.set(QStringLiteral("m_ballPosition"), c.v);
            const NormMeasureSource src(v, norms, QStringLiteral("full_swing"));
            const auto r = src.read(QStringLiteral("m_ballPosition"));
            if (!r || r->grade != c.want) { ok = false; std::printf("      %s\n", c.label); }
        }
        check(ok, "every band boundary grades as expected through the source");
    }

    std::printf("=== resolution walks up the tree ===\n");
    {
        FakeValues v;
        v.set(QStringLiteral("m_ballPosition"), 10.0);

        const NormMeasureSource drv(v, norms, QStringLiteral("driver"));
        const auto rd = drv.read(QStringLiteral("m_ballPosition"));
        check(rd && rd->normContextId == QLatin1String("driver") && rd->grade == Grade::Ideal,
              "a driver shot at 10% grades Ideal against the DRIVER norm");

        // The same value, judged as a wedge: it inherits full swing (no wedge row) and 10% is a
        // long way forward of a wedge's 50% centre. Same swing, different verdict, correctly.
        const NormMeasureSource wdg(v, norms, QStringLiteral("wedge"));
        const auto rw = wdg.read(QStringLiteral("m_ballPosition"));
        check(rw && rw->normContextId == QLatin1String("full_swing"),
              "a wedge inherits the full-swing norm");
        check(rw && rw->grade == Grade::Action,
              "…and the SAME value grades Action there — which is the whole point of contexts");
    }

    std::printf("=== an unknown context is never silently defaulted ===\n");
    {
        FakeValues v;
        v.set(QStringLiteral("m_ballPosition"), 10.0);
        const NormMeasureSource src(v, norms, QStringLiteral("hovercraft"));
        const auto r = src.read(QStringLiteral("m_ballPosition"));
        check(r.has_value(), "the VALUE still reads");
        check(r && !r->hasCorridor && r->grade == Grade::NotMeasured,
              "but no corridor resolves, so it is NotMeasured — not graded against full swing");
    }

    std::printf("=== an undeclared context is marked, not hidden ===\n");
    {
        FakeValues v;
        v.set(QStringLiteral("m_ballPosition"), 90.0);
        const NormMeasureSource src(v, norms, QString());
        const auto r = src.read(QStringLiteral("m_ballPosition"));
        check(r && r->hasCorridor && r->normContextId == QLatin1String("full_swing"),
              "an undeclared context falls back to full swing");
        check(r && r->contextInferred, "…and says so, so the engine can demote confidence");
        check(r && r->grade == Grade::Action, "the deviation itself is still reported");
    }

    std::printf("=== absent value vs absent norm ===\n");
    {
        FakeValues v;                                   // nothing set
        const NormMeasureSource src(v, norms, QStringLiteral("full_swing"));
        check(!src.read(QStringLiteral("m_ballPosition")).has_value(),
              "no value at all -> nullopt, which the engine reports Unavailable");

        FakeValues v2;
        v2.set(QStringLiteral("m_noNorm"), 42.0);
        const NormMeasureSource src2(v2, norms, QStringLiteral("full_swing"));
        const auto r = src2.read(QStringLiteral("m_noNorm"));
        check(r && !r->hasCorridor,
              "a value with no norm reads, but carries no corridor — also Unavailable, never a pass");

        const NormMeasureSource src3(v2, nullptr, QStringLiteral("full_swing"));
        const auto r3 = src3.read(QStringLiteral("m_noNorm"));
        check(r3 && !r3->hasCorridor, "no norm provider at all degrades, it does not crash");
    }

    std::printf("=== the engine fires on a deviation, not on leaving Ideal ===\n");
    {
        // A minimal pack: one axis, two tails, on one measure.
        CharacteristicPack pack;
        Measure m;
        m.id     = QStringLiteral("m_ballPosition");
        m.unit   = QStringLiteral("%");
        m.status = MeasureStatus::Live;
        pack.measures.push_back(m);

        Signal hi;
        hi.id        = QStringLiteral("sig_back");
        hi.test      = SignalTest::OutsideCorridor;
        hi.measures  = { QStringLiteral("m_ballPosition") };
        hi.direction = Direction::High;
        pack.signalDefs.push_back(hi);

        Signal lo = hi;
        lo.id        = QStringLiteral("sig_forward");
        lo.direction = Direction::Low;
        pack.signalDefs.push_back(lo);

        Condition back;
        back.id         = QStringLiteral("ball_back");
        back.label      = QStringLiteral("Ball back");
        back.axis       = QStringLiteral("ball_position");
        back.detectedBy = { QStringLiteral("sig_back") };
        back.state      = ConditionState::Active;
        pack.conditions.push_back(back);

        Condition fwd = back;
        fwd.id         = QStringLiteral("ball_forward");
        fwd.label      = QStringLiteral("Ball forward");
        fwd.detectedBy = { QStringLiteral("sig_forward") };
        pack.conditions.push_back(fwd);

        auto detectAt = [&](double value, const QString &ctx) {
            FakeValues v;
            v.set(QStringLiteral("m_ballPosition"), value);
            const NormMeasureSource src(v, norms, ctx);
            return detect(pack, src);
        };

        const QString FS = QStringLiteral("full_swing");

        // 65 is 1.5 sigma out — outside the Ideal band, but ordinary variation. It must NOT fire.
        const DetectionResult good = detectAt(65.0, FS);
        check(good.fired().isEmpty(),
              "1.5 sigma outside Ideal does NOT fire — Good is ordinary variation");
        const Finding *gf = good.find(QStringLiteral("ball_back"));
        check(gf && gf->state == FindingState::NotFired,
              "…and it is reported NotFired, which is an assessment, not a gap");

        // 75 is 2.5 sigma — a Watch deviation on the high tail.
        const DetectionResult watch = detectAt(75.0, FS);
        check(watch.fired() == QStringList({ QStringLiteral("ball_back") }),
              "2.5 sigma fires the HIGH tail only");

        // 25 is 2.5 sigma the other way — the same axis, the other condition.
        const DetectionResult low = detectAt(25.0, FS);
        check(low.fired() == QStringList({ QStringLiteral("ball_forward") }),
              "2.5 sigma low fires the LOW tail only — one norm, two tails, never both");

        // No norm for this measure at all: Unavailable, and emphatically not NotFired.
        CharacteristicPack orphan = pack;
        orphan.measures[0].id = QStringLiteral("m_orphan");
        orphan.signalDefs[0].measures = { QStringLiteral("m_orphan") };
        orphan.signalDefs[1].measures = { QStringLiteral("m_orphan") };
        FakeValues ov;
        ov.set(QStringLiteral("m_orphan"), 999.0);
        const NormMeasureSource osrc(ov, norms, FS);
        const DetectionResult ores = detect(orphan, osrc);
        const Finding *of = ores.find(QStringLiteral("ball_back"));
        check(of && of->state == FindingState::Unavailable,
              "a measure with no norm is UNAVAILABLE, never a clean bill of health");

        // The inferred-context demotion reaches the finding.
        const DetectionResult inferred = detectAt(75.0, QString());
        const Finding *inf = inferred.find(QStringLiteral("ball_back"));
        const Finding *dec = watch.find(QStringLiteral("ball_back"));
        check(inf && dec && inf->state == FindingState::Fired
                  && inf->confidence < dec->confidence,
              "an inferred context fires the same finding at LOWER confidence");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
