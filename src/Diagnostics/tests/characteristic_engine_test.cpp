// Standalone tests for the detection and explanation passes
// (src/Diagnostics/characteristic_engine.*, relation_resolver.*).
//
// The engine runs against a synthetic measure source ONLY — it is not wired into the live analysis
// path. The single most important behaviour under test is that an absent measure resolves as
// Unavailable and never as a pass: with most of the seed pack currently unproducible, that one
// mistake would make the whole library look like it works.
//
//   cmake --build build/analyzer-tests --target characteristic_engine_test
//   ctest --test-dir build/analyzer-tests -R characteristic_engine --output-on-failure

#include "../relation_resolver.h"

#include <cstdio>
#include <map>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// ── A synthetic measure source ──────────────────────────────────────────────
// Anything not explicitly added is absent, which is the interesting case.
class FakeSource final : public IMeasureSource {
public:
    void add(const QString &id, double value, double lo, double hi, float conf = 1.0f)
    {
        MeasureReading r;
        r.value       = value;
        r.hasCorridor = true;
        r.greenLo     = lo;
        r.greenHi     = hi;
        r.confidence  = conf;
        m[id]         = r;
    }
    // Present, but with no corridor to grade against — must be Unavailable, not a pass.
    void addWithoutCorridor(const QString &id, double value)
    {
        MeasureReading r;
        r.value       = value;
        r.hasCorridor = false;
        m[id]         = r;
    }
    std::optional<MeasureReading> read(const QString &id) const override
    {
        const auto it = m.find(id);
        return it == m.end() ? std::nullopt : std::optional<MeasureReading>(it->second);
    }

private:
    std::map<QString, MeasureReading> m;
};

// ── Fixture ─────────────────────────────────────────────────────────────────
// A chain that exercises the rules: a latent screened cause explains two characteristics, one of
// which causes a third. Plus an asserted cause and a measure nothing can produce.
static CharacteristicPack fixture()
{
    CharacteristicPack p;
    p.id            = QStringLiteral("t");
    p.schemaVersion = kPackSchemaVersion;

    auto measure = [&](const char *id) {
        Measure m;
        m.id             = QString::fromLatin1(id);
        m.kind           = MeasureKind::Composed;
        m.series         = Series{ AnatomyRole::PelvisCentre, Quantity::Distance, AnatomyRole::TrailAnkle };
        m.reducer.kind   = ReducerKind::At;
        m.reducer.anchor = Phase::Top;
        p.measures.push_back(m);
    };
    for (const char *id : { "mSway", "mSlide", "mBuckle", "mGhost" }) measure(id);

    auto signal = [&](const char *id, const char *mid, Direction d) {
        Signal s;
        s.id        = QString::fromLatin1(id);
        s.test      = SignalTest::OutsideCorridor;
        s.measures  = { QString::fromLatin1(mid) };
        s.direction = d;
        p.signalDefs.push_back(s);
    };
    signal("sigSway", "mSway", Direction::High);
    signal("sigSlide", "mSlide", Direction::High);
    signal("sigBuckle", "mBuckle", Direction::High);
    signal("sigGhost", "mGhost", Direction::High);

    auto observable = [&](const char *id, const char *sig) {
        Condition c;
        c.id            = QString::fromLatin1(id);
        c.label         = c.id;
        c.observability = Observability::Observable;
        c.confirmedBy   = ConfirmedBy::Measured;
        c.detectedBy    = { QString::fromLatin1(sig) };
        c.state         = ConditionState::Active;
        p.conditions.push_back(c);
    };
    observable("sway", "sigSway");
    observable("slide", "sigSlide");
    observable("lateBuckle", "sigBuckle");
    observable("ghost", "sigGhost");

    auto latent = [&](const char *id, ConfirmedBy by, const char *screen) {
        Condition c;
        c.id            = QString::fromLatin1(id);
        c.label         = c.id;
        c.observability = Observability::Latent;
        c.confirmedBy   = by;
        if (screen) c.screenRef = QString::fromLatin1(screen);
        c.state = ConditionState::Active;
        p.conditions.push_back(c);
    };
    latent("limitedHipIr", ConfirmedBy::Screened, "screen.hipIr");
    latent("poorBalance", ConfirmedBy::Screened, "screen.balance");
    latent("tempoHabit", ConfirmedBy::Asserted, nullptr);

    auto edge = [&](const char *from, const char *to, Strength s) {
        Edge e;
        e.from     = QString::fromLatin1(from);
        e.to       = QString::fromLatin1(to);
        e.type     = EdgeType::Causes;
        e.strength = s;
        p.edges.push_back(e);
    };
    edge("limitedHipIr", "sway", Strength::Strong);
    edge("limitedHipIr", "slide", Strength::Strong);
    edge("poorBalance", "lateBuckle", Strength::Moderate);
    edge("slide", "lateBuckle", Strength::Strong);   // observable causes observable
    edge("tempoHabit", "ghost", Strength::Moderate); // ghost's ONLY cause is asserted
    return p;
}

int main()
{
    std::printf("characteristic_engine_test\n");
    const CharacteristicPack pack = fixture();

    // ── An absent measure is Unavailable, never a pass ──────────────────────────
    {
        FakeSource src;   // nothing added at all
        const DetectionResult d = detect(pack, src);

        check(d.findings.size() == 4, "one finding per observable condition");
        bool allUnavailable = true;
        for (const Finding &f : d.findings)
            if (f.state != FindingState::Unavailable) allUnavailable = false;
        check(allUnavailable, "with no measures at all, every finding is Unavailable");
        check(d.fired().isEmpty(), "nothing fires when nothing can be measured");

        const Finding *f = d.find(QStringLiteral("sway"));
        check(f && f->missingMeasures.contains(QStringLiteral("mSway")),
              "the finding names exactly which measure was missing");
    }

    // ── Present but ungradeable is ALSO unavailable ─────────────────────────────
    // A value with no corridor cannot be assessed. Treating it as "within range" would be the same
    // false negative wearing a different hat.
    {
        FakeSource src;
        src.addWithoutCorridor(QStringLiteral("mSway"), 999.0);
        const DetectionResult d = detect(pack, src);
        const Finding        *f = d.find(QStringLiteral("sway"));
        check(f && f->state == FindingState::Unavailable,
              "a measure with no corridor is Unavailable, not a pass");
    }

    // ── Firing, direction, and NotFired ─────────────────────────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);    // above the corridor => fires High
        src.add(QStringLiteral("mSlide"), 5.0, 0.0, 10.0);    // inside => does not fire
        const DetectionResult d = detect(pack, src);

        const Finding *sway  = d.find(QStringLiteral("sway"));
        const Finding *slide = d.find(QStringLiteral("slide"));
        check(sway && sway->state == FindingState::Fired, "a value above the corridor fires");
        check(sway && sway->direction == Direction::High, "the finding carries which tail fired");
        check(slide && slide->state == FindingState::NotFired, "a value inside the corridor does not fire");
        check(d.fired() == QStringList{ QStringLiteral("sway") }, "fired() lists only what fired");

        // Low-tail signals fire on the other side of the corridor.
        CharacteristicPack low = pack;
        low.signalDefs.front().direction = Direction::Low;
        const DetectionResult dl = detect(low, src);
        check(dl.find(QStringLiteral("sway"))->state == FindingState::NotFired,
              "the same value does not fire the opposite tail");
    }

    // ── Confidence propagates ───────────────────────────────────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0, 0.4f);
        const Finding *f = detect(pack, src).find(QStringLiteral("sway"));
        check(f && f->confidence > 0.39f && f->confidence < 0.41f, "measure confidence reaches the finding");
    }

    // ── Explanation: greedy ranking, roots, and the chain rule ──────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mBuckle"), 50.0, 0.0, 10.0);
        const DetectionResult d  = detect(pack, src);
        const Explanation     ex = explain(pack, d);

        check(d.fired().size() == 3, "three characteristics fired");
        check(!ex.roots.empty(), "an explanation is produced");
        check(ex.roots.front().conditionId == QStringLiteral("limitedHipIr"),
              "the cause explaining most findings ranks first");
        check(ex.roots.front().coverage == 2, "it accounts for two of them");

        // RULE 1: `slide` fired and causes `lateBuckle`, but slide itself has a cause in the pack,
        // so it is a link in the chain — never offered as the root.
        const bool slideIsRoot = std::any_of(ex.roots.begin(), ex.roots.end(),
                                             [](const RankedCause &r) {
                                                 return r.conditionId == QStringLiteral("slide");
                                             });
        check(!slideIsRoot, "a fired characteristic with an in-pack cause is never a root");

        // Every fired finding ends up accounted for by something.
        check(ex.unexplained.isEmpty(), "everything fired is explained");
    }

    // ── RULE 2: Asserted causes are offered, never concluded ────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mGhost"), 50.0, 0.0, 10.0);   // only `ghost` fires
        const DetectionResult d  = detect(pack, src);
        const Explanation     ex = explain(pack, d);

        const bool tempoConcluded = std::any_of(ex.roots.begin(), ex.roots.end(),
                                                [](const RankedCause &r) {
                                                    return r.conditionId == QStringLiteral("tempoHabit");
                                                });
        check(!tempoConcluded, "an Asserted cause is never concluded as a root");

        check(ex.offered.size() == 1 && ex.offered.front().conditionId == QStringLiteral("tempoHabit"),
              "...but it IS offered, so the panel is not empty");
        check(ex.offered.front().offeredOnly, "the offered cause is flagged as offer-only");
        check(ex.offered.front().explains.contains(QStringLiteral("ghost")),
              "the offer says what it would explain");

        // The failure this guards against: dropping asserted causes entirely would leave a
        // characteristic whose only cause is habit with nothing shown at all.
        check(ex.unexplained.isEmpty(),
              "a habit-only characteristic is not reported as unexplained");
    }

    // ── Test recommendations ────────────────────────────────────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mBuckle"), 50.0, 0.0, 10.0);
        const DetectionResult d  = detect(pack, src);
        const Explanation     ex = explain(pack, d);

        check(!ex.recommendations.empty(), "an unanswered screen produces a recommendation");
        check(ex.recommendations.front().conditionId == QStringLiteral("limitedHipIr"),
              "the screen explaining the most findings is recommended first");
        check(ex.recommendations.front().coverage == 2, "the recommendation states its reach");
        check(ex.recommendations.front().screenRef == QStringLiteral("screen.hipIr"),
              "the recommendation names the screen to run");

        // A screen explaining only ONE finding is not worth sending someone for.
        const bool balanceRecommended = std::any_of(
            ex.recommendations.begin(), ex.recommendations.end(),
            [](const TestRecommendation &t) { return t.conditionId == QStringLiteral("poorBalance"); });
        check(!balanceRecommended, "a screen explaining only one finding is not recommended");
    }

    // ── A screen already answered NEGATIVE stops being an explanation ───────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);
        const DetectionResult d = detect(pack, src);

        QHash<QString, bool> screens;
        screens.insert(QStringLiteral("limitedHipIr"), false);   // tested, came back clear
        const Explanation ex = explain(pack, d, screens);

        const bool stillRoot = std::any_of(ex.roots.begin(), ex.roots.end(),
                                           [](const RankedCause &r) {
                                               return r.conditionId == QStringLiteral("limitedHipIr");
                                           });
        check(!stillRoot, "a screen that came back clear is dropped as an explanation");
        check(ex.recommendations.empty() || ex.recommendations.front().conditionId
                                                  != QStringLiteral("limitedHipIr"),
              "and it is not recommended again");
        check(!ex.unexplained.isEmpty(), "the findings it used to explain become unexplained");
    }

    // ── The UI can always interrogate a ranking ─────────────────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);
        const DetectionResult d = detect(pack, src);

        const QStringList covered = findingsCoveredBy(pack, d, QStringLiteral("limitedHipIr"));
        check(covered.size() == 2, "any candidate cause can enumerate what it would cover");
        check(findingsCoveredBy(pack, d, QStringLiteral("poorBalance")).isEmpty(),
              "a cause whose effects did not fire covers nothing");
    }

    // ── Nothing fired => nothing claimed ────────────────────────────────────────
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 5.0, 0.0, 10.0);   // inside the corridor
        const Explanation ex = explain(pack, detect(pack, src));
        check(ex.roots.empty() && ex.offered.empty() && ex.recommendations.empty(),
              "no findings means no explanation, no offers, no recommendations");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
