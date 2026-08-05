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
        // fromCorridor, not hand-built: a corridor with an unset grade never fires, which reads as
        // "nothing was wrong" and is precisely the false negative this engine exists to avoid.
        m[id] = MeasureReading::fromCorridor(value, lo, hi, conf);
    }
    // Present, but with no corridor to grade against — must be Unavailable, not a pass.
    void addWithoutCorridor(const QString &id, double value)
    {
        MeasureReading r;
        r.value       = value;
        r.hasCorridor = false;
        m[id]         = r;
    }
    // A one-sided corridor, as NormMeasureSource builds one: the open side's edge is mu.
    void addOneSided(const QString &id, double value, double mu, double sigma, Shape shape)
    {
        Norm n;
        n.mu = mu;
        n.sigmaLo = n.sigmaHi = sigma;
        const NormBandEdges e = bandEdgesOf(n, shape);

        MeasureReading r;
        r.value       = value;
        r.hasCorridor = true;
        r.greenLo     = e.idealLo;
        r.greenHi     = e.idealHi;
        r.lowOpen     = e.lowOpen;
        r.highOpen    = e.highOpen;
        r.grade       = grade(value, n, shape);
        m[id]         = r;
    }
    // A reading outside what its norm is willing to believe.
    void addImplausible(const QString &id, double value, double lo, double hi)
    {
        MeasureReading r = MeasureReading::fromCorridor(value, lo, hi);
        r.grade          = Grade::NotMeasured;
        r.implausible    = true;
        m[id]            = r;
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
        // Bind the result: find() returns a pointer INTO its findings vector, so calling it on the
        // temporary leaves f dangling the moment this statement ends. It read plausible confidences
        // off freed memory on x86 and garbage on arm64.
        const DetectionResult d = detect(pack, src);
        const Finding        *f = d.find(QStringLiteral("sway"));
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

    // ── Withdrawn content does not diagnose ─────────────────────────────────────
    //
    // Retired and Superseded are the only two states that mean "do not use this any more". The
    // engine read all six identically until now, so retiring a characteristic changed a badge and
    // nothing else — it went on firing exactly as it did the day it was sound.
    //
    // The negative half of this test is the more important one. Draft and Candidate are editorial
    // confidence, NOT withdrawal, and most of the shipped pack is Draft; a reading of the field
    // that treats "not finished" as "not in use" would dark more than half the library while every
    // count and census still said it was there.
    {
        FakeSource src;
        src.add(QStringLiteral("mSway"), 99.0, 0.0, 10.0);   // way outside — sway must fire

        for (const ConditionState st : { ConditionState::Draft, ConditionState::Candidate,
                                         ConditionState::NeedsRevalidation }) {
            CharacteristicPack p = pack;
            for (Condition &c : p.conditions)
                if (c.id == QStringLiteral("sway")) c.state = st;
            check(detect(p, src).fired().contains(QStringLiteral("sway")),
                  QByteArray("a ").append(conditionStateName(st).toLatin1())
                      .append(" condition still detects — it is confidence, not withdrawal")
                      .constData());
        }

        for (const ConditionState st : { ConditionState::Retired, ConditionState::Superseded }) {
            CharacteristicPack p = pack;
            for (Condition &c : p.conditions)
                if (c.id == QStringLiteral("sway")) {
                    c.state        = st;
                    c.supersededBy = QStringLiteral("slide");   // the validator requires a successor
                }
            const DetectionResult d = detect(p, src);
            check(!d.fired().contains(QStringLiteral("sway")),
                  QByteArray("a ").append(conditionStateName(st).toLatin1())
                      .append(" condition does not fire").constData());
            // OMITTED, not NotFired: it was never asked, and NotFired would claim it was assessed
            // and found absent — the same distinction the binding rule turns on.
            check(d.find(QStringLiteral("sway")) == nullptr,
                  QByteArray("a ").append(conditionStateName(st).toLatin1())
                      .append(" condition is omitted from the result, not reported as absent")
                      .constData());
            check(d.findings.size() == 3, "and the rest of the library is untouched");
        }
    }

    // ── Context bindings: what does not apply here is never asked ───────────────
    //
    // The load-bearing distinction is that an inapplicable condition is ABSENT from the result, not
    // NotFired (which would claim it was assessed and found absent) and not Unavailable (which
    // would claim the app tried and could not). Both would be wrong in a way a coach could read.
    {
        const ContextTree tree(std::vector<ContextNode>{
            { QStringLiteral("any"),        QStringLiteral("Any shot"),     QString() },
            { QStringLiteral("full_swing"), QStringLiteral("Full swing"),   QStringLiteral("any") },
            { QStringLiteral("partial"),    QStringLiteral("Partial"),      QStringLiteral("any") },
            { QStringLiteral("chip"),       QStringLiteral("Chip"),         QStringLiteral("partial") },
        });

        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);    // fires
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);   // fires

        // Unbound: passing a context changes nothing for a pack with no binding rows, which is the
        // shipped pack and therefore the case that must not move.
        const DetectionResult base  = detect(pack, src);
        const DetectionResult ctx   = detect(pack, src, &tree, QStringLiteral("chip"));
        check(base.findings.size() == ctx.findings.size(),
              "a pack with no bindings is unaffected by the shot's context");

        CharacteristicPack narrowed = pack;
        for (Condition &c : narrowed.conditions)
            if (c.id == QLatin1String("sway"))
                c.bindings.push_back(ContextBinding{ QStringLiteral("partial"), false, true, {} });

        const DetectionResult onChip = detect(narrowed, src, &tree, QStringLiteral("chip"));
        check(onChip.find(QStringLiteral("sway")) == nullptr,
              "a condition switched off at a parent context is omitted, not reported as absent");
        check(onChip.find(QStringLiteral("slide")) != nullptr,
              "…and the rest of the pack is evaluated as usual");
        check(!onChip.fired().contains(QStringLiteral("sway")),
              "an omitted condition cannot fire");

        const DetectionResult onFull = detect(narrowed, src, &tree, QStringLiteral("full_swing"));
        check(onFull.find(QStringLiteral("sway")) != nullptr
                  && onFull.find(QStringLiteral("sway"))->state == FindingState::Fired,
              "the same condition still fires in a context it does apply to");
        check(detect(narrowed, src).find(QStringLiteral("sway")) != nullptr,
              "with no context given, nothing is filtered — bindings need a context to mean anything");
    }

    // ── Materiality is a ranking weight and nothing else ────────────────────────
    {
        const ContextTree tree(std::vector<ContextNode>{
            { QStringLiteral("any"),     QStringLiteral("Any shot"), QString() },
            { QStringLiteral("partial"), QStringLiteral("Partial"),  QStringLiteral("any") },
        });

        FakeSource src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);

        CharacteristicPack immaterial = pack;
        for (Condition &c : immaterial.conditions)
            if (c.id == QLatin1String("sway"))
                c.bindings.push_back(ContextBinding{ QStringLiteral("partial"), true, false, {} });

        const DetectionResult d = detect(immaterial, src, &tree, QStringLiteral("partial"));
        const Finding *f = d.find(QStringLiteral("sway"));
        check(f && f->state == FindingState::Fired && !f->material,
              "an immaterial condition still fires and is still reported — it is only unweighted");

        // limitedHipIr causes BOTH sway and slide, so its coverage is unchanged while its score
        // loses exactly the immaterial finding's edge.
        const Explanation exM = explain(pack, detect(pack, src, &tree, QStringLiteral("partial")));
        const Explanation exI = explain(immaterial, d);
        check(!exM.roots.empty() && !exI.roots.empty(), "both rank something");
        check(exI.roots.front().coverage == exM.roots.front().coverage,
              "an immaterial finding is still explained and still counted in coverage");
        check(exI.roots.front().score < exM.roots.front().score,
              "…but it carries no weight in the ranking score");
    }

    // ── Prominence: the base rate, in the ranking ───────────────────────────────
    //
    // SHAPES ONLY. Nothing here pins 0.35 or any condition's rung — those are editorial figures
    // awaiting a corpus re-seat, and trap 5 says a test must not freeze them. What is asserted is
    // that the term is present, that it is a tie-break of last resort rather than a coverage
    // override, and that a uniform pack reproduces the pre-change ordering — which is the property
    // `rank_shift_report` measures against and would be worthless without.
    std::printf("=== prominence weights the ranking ===\n");
    {
        CharacteristicPack pack = fixture();
        FakeSource         src;
        src.add(QStringLiteral("mSway"), 50.0, 0.0, 10.0);   // sway and slide both fire, so each
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);  // cause below covers two findings
        const DetectionResult d = detect(pack, src);

        // Two causes, identical in every way the ranking reads except the rung. Give poorBalance
        // the same two effects at the same strength so coverage and edge weights cannot separate
        // them, and the ONLY difference left is how common each is.
        for (const char *to : { "sway", "slide" }) {
            Edge e;
            e.from     = QStringLiteral("poorBalance");
            e.to       = QString::fromLatin1(to);
            e.type     = EdgeType::Causes;
            e.strength = Strength::Strong;               // matches limitedHipIr's
            pack.edges.push_back(e);
        }
        for (Condition &c : pack.conditions) {
            if (c.id == QLatin1String("limitedHipIr")) c.prominence = Prominence::Ubiquitous;
            if (c.id == QLatin1String("poorBalance"))  c.prominence = Prominence::Rare;
        }

        const Explanation ex = explain(pack, d);
        check(!ex.roots.empty(), "the tied pair ranks");
        check(ex.roots.front().conditionId == QLatin1String("limitedHipIr"),
              "of two causes alike in coverage and strength, the commoner one is put first");
        check(ex.roots.front().prominence == Prominence::Ubiquitous,
              "and the rung it was weighted by is carried out, so a UI can say why");

        // Reversing the rungs must reverse the answer. Without this the check above passes on an
        // alphabetical tie-break, which is what it would have done before the change.
        for (Condition &c : pack.conditions) {
            if (c.id == QLatin1String("limitedHipIr")) c.prominence = Prominence::Rare;
            if (c.id == QLatin1String("poorBalance"))  c.prominence = Prominence::Ubiquitous;
        }
        const Explanation flipped = explain(pack, d);
        check(!flipped.roots.empty()
                  && flipped.roots.front().conditionId == QLatin1String("poorBalance"),
              "…and swapping the rungs swaps the order, so it is the rung deciding and not the id");

        // Coverage is a COUNT of findings explained and prominence must never touch it. A base rate
        // that changed what a cause accounts for would be prevalence rewriting the evidence.
        check(flipped.roots.front().coverage == ex.roots.front().coverage,
              "prominence reorders causes; it never changes what one explains");

        // A uniform pack is a positive scalar multiple of the pre-change score, so it must produce
        // the pre-change ORDER exactly — here, the alphabetical tie-break the two rungs were
        // overriding. This is the control `rank_shift_report` measures against.
        CharacteristicPack flat = pack;
        for (Condition &c : flat.conditions) c.prominence = Prominence::Occasional;
        const Explanation control = explain(flat, d);
        check(!control.roots.empty()
                  && control.roots.front().conditionId == QLatin1String("limitedHipIr"),
              "a pack at one rung ranks by the id again — the control reproduces the old ordering");
    }

    // ── Excludes: two findings that cannot both describe one swing ──────────────
    //
    // The pair is resolved BEFORE ranking, because leaving both in would put a contradiction in
    // front of a coach and let one cause be credited twice for two versions of one event.
    std::printf("=== excludes ===\n");
    {
        CharacteristicPack p = pack;
        p.edges.push_back(Edge{ QStringLiteral("sway"), QStringLiteral("slide"),
                                EdgeType::Excludes, Strength::Strong, {} });

        FakeSource src;
        src.add(QStringLiteral("mSway"),  50.0, 0.0, 10.0, 0.9f);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0, 0.4f);   // the less confident reading
        const DetectionResult d  = detect(p, src);
        const Explanation     ex = explain(p, d);

        check(d.fired().contains(QStringLiteral("slide")),
              "detection still reports both — suppression is the EXPLANATION's job, not the engine's");
        check(ex.suppressed.size() == 1, "one finding was ruled out");
        check(ex.suppressed.front().conditionId == QStringLiteral("slide")
                  && ex.suppressed.front().excludedBy == QStringLiteral("sway"),
              "the LESS confident reading is the one dropped");
        check(!ex.suppressed.front().reason.isEmpty(),
              "…and it says so, rather than the finding silently vanishing");
        check(!ex.unexplained.contains(QStringLiteral("slide")),
              "a suppressed finding is not ALSO reported as unexplained — one event, one heading");

        // Symmetry: an author may write the edge from either end and must get the same answer.
        CharacteristicPack q = pack;
        q.edges.push_back(Edge{ QStringLiteral("slide"), QStringLiteral("sway"),
                                EdgeType::Excludes, Strength::Strong, {} });
        const Explanation exq = explain(q, detect(q, src));
        check(exq.suppressed.size() == 1
                  && exq.suppressed.front().conditionId == QStringLiteral("slide"),
              "the edge means the same written from either end");

        // With no exclusion authored, nothing is suppressed — the negative case, without which the
        // check above would pass on a resolver that suppressed everything.
        const Explanation plain = explain(pack, detect(pack, src));
        check(plain.suppressed.empty(), "no exclusion, no suppression");
    }

    // ── Corroborates: reported, and a tie-break, never a fabricated weight ──────
    std::printf("=== corroborates ===\n");
    {
        CharacteristicPack p = pack;
        p.edges.push_back(Edge{ QStringLiteral("sway"), QStringLiteral("slide"),
                                EdgeType::Corroborates, Strength::Strong, {} });

        FakeSource src;
        src.add(QStringLiteral("mSway"),  50.0, 0.0, 10.0);
        src.add(QStringLiteral("mSlide"), 50.0, 0.0, 10.0);
        const Explanation ex = explain(p, detect(p, src));

        check(ex.corroborations.size() == 2,
              "both ends of a corroborating pair are reported — it is symmetric in meaning");
        check(ex.corroborations.front().corroboratedBy.size() == 1,
              "each names what confirmed it");

        // The scores must be IDENTICAL to the un-corroborated run. Corroboration is evidence a coach
        // reads, and turning it into a multiplier would mean inventing a number nobody could defend
        // when asked why one cause outranked another.
        const Explanation plain = explain(pack, detect(pack, src));
        check(plain.corroborations.empty(), "no edge, no corroboration reported");
        check(!ex.roots.empty() && !plain.roots.empty()
                  && ex.roots.front().score == plain.roots.front().score,
              "corroboration changes no score");
    }

    // ── A one-sided corridor: the open tail can never fire ──────────────────
    std::printf("\none-sided corridors\n");
    {
        CharacteristicPack p;
        Measure m;
        m.id     = QStringLiteral("m_smash");
        m.status = MeasureStatus::Live;
        m.shape  = Shape::Floor;
        p.measures.push_back(m);

        Signal high;                       // the misunderstanding: a floor has no upper fault
        high.id        = QStringLiteral("sigHigh");
        high.measures  = { m.id };
        high.direction = Direction::High;
        p.signalDefs.push_back(high);

        Signal low;                        // the graded tail
        low.id        = QStringLiteral("sigLow");
        low.measures  = { m.id };
        low.direction = Direction::Low;
        p.signalDefs.push_back(low);

        Condition tooHigh;
        tooHigh.id         = QStringLiteral("c_high");
        tooHigh.detectedBy = { high.id };
        tooHigh.state      = ConditionState::Active;
        p.conditions.push_back(tooHigh);

        Condition tooLow;
        tooLow.id         = QStringLiteral("c_low");
        tooLow.detectedBy = { low.id };
        tooLow.state      = ConditionState::Active;
        p.conditions.push_back(tooLow);

        // Far above the aspiration: the good side of a floor, however far out.
        FakeSource above;
        above.addOneSided(m.id, 2.20, 1.48, 0.05, Shape::Floor);
        const DetectionResult ra = detect(p, above);
        check(ra.find("c_high") && ra.find("c_high")->state == FindingState::NotFired,
              "a High signal on a floor does not fire, however far above the aspiration");
        check(ra.find("c_low") && ra.find("c_low")->state == FindingState::NotFired,
              "…and the graded tail is quiet too, because nothing is wrong");

        // Well below it: the graded tail, and only that one, fires.
        FakeSource below;
        below.addOneSided(m.id, 1.28, 1.48, 0.05, Shape::Floor);
        const DetectionResult rb = detect(p, below);
        check(rb.find("c_low") && rb.find("c_low")->state == FindingState::Fired,
              "the graded tail fires exactly as a target norm's low tail would");
        check(rb.find("c_high") && rb.find("c_high")->state == FindingState::NotFired,
              "…and the open tail stays silent on the same swing");

        // The authoring mistake is ALSO reported by diagnosticsHealth as `signalOnOpenTail` —
        // gated in diagnostics_health_test, which is where that module is linked. Both guards
        // exist deliberately: the runtime one makes the answer right, the health one makes the
        // mistake visible.
    }

    // ── A reading nobody believes was not assessed ──────────────────────────
    std::printf("\nimplausible readings\n");
    {
        const CharacteristicPack p = fixture();

        FakeSource src;
        src.addImplausible(QStringLiteral("mSway"), 9999.0, 0.0, 10.0);

        const DetectionResult r = detect(p, src);
        const Finding *f = r.find(QStringLiteral("sway"));
        check(f && f->state == FindingState::Unavailable,
              "an implausible reading is UNAVAILABLE, not NotFired — it was never assessed");
        check(f && !f->missingMeasures.isEmpty(),
              "…and the finding names the measure, so the UI can say which reading was wrong");
    }

    // ── A threshold makes the same two refusals a corridor does ────────────
    //
    // The number being authored rather than inherited changes what the signal COMPARES against and
    // nothing else: it is the same reading, off the same norm-joined source, and the norm's two
    // veto powers over it are unaffected. This branch skipped both, which made it the one place in
    // the engine where a typed-in figure was trusted further than the norm that said the reading
    // was not real.
    std::printf("\nthreshold signals\n");
    {
        CharacteristicPack p;
        Measure            m;
        m.id     = QStringLiteral("m_speed");
        m.status = MeasureStatus::Live;
        p.measures.push_back(m);

        Signal fast;
        fast.id        = QStringLiteral("sigFast");
        fast.test      = SignalTest::Threshold;
        fast.measures  = { m.id };
        fast.direction = Direction::High;
        fast.threshold = 100.0;
        p.signalDefs.push_back(fast);

        Condition c;
        c.id         = QStringLiteral("c_fast");
        c.detectedBy = { fast.id };
        c.state      = ConditionState::Active;
        p.conditions.push_back(c);

        // The positive case first, and deliberately WITHOUT a corridor: a threshold test exists to
        // work where no norm has been authored, so a guard that demanded one would dark exactly the
        // signals this test kind is for.
        {
            FakeSource over;
            over.addWithoutCorridor(m.id, 120.0);
            check(detect(p, over).find("c_fast")->state == FindingState::Fired,
                  "a threshold fires above its number with no corridor in sight");

            FakeSource under;
            under.addWithoutCorridor(m.id, 80.0);
            check(detect(p, under).find("c_fast")->state == FindingState::NotFired,
                  "…and does not fire below it");
        }

        // An implausible reading is not a big reading. It was NOT ASSESSED, and answering "yes,
        // above 100" about a number the norm refuses to believe launders a capture fault into a
        // confident diagnosis.
        {
            FakeSource src;
            src.addImplausible(m.id, 9999.0, 0.0, 200.0);
            const DetectionResult r = detect(p, src);
            const Finding        *f = r.find("c_fast");
            check(f && f->state == FindingState::Unavailable,
                  "an implausible reading does not fire a threshold — it was never assessed");
            check(f && f->missingMeasures.contains(m.id),
                  "…and the finding names the measure, exactly as the corridor branch does");
        }
    }

    // A threshold watching the tail a one-sided measure does not grade cannot fire, whoever wrote
    // the number. Same authoring mistake as on a corridor signal, and now the same answer.
    {
        CharacteristicPack p;
        Measure            m;
        m.id     = QStringLiteral("m_smash");
        m.status = MeasureStatus::Live;
        m.shape  = Shape::Floor;
        p.measures.push_back(m);

        Signal high;                       // the misunderstanding: a floor has no upper fault
        high.id        = QStringLiteral("sigHigh");
        high.test      = SignalTest::Threshold;
        high.measures  = { m.id };
        high.direction = Direction::High;
        high.threshold = 1.00;
        p.signalDefs.push_back(high);

        Signal low;                        // the graded tail, with an authored number on it
        low.id        = QStringLiteral("sigLow");
        low.test      = SignalTest::Threshold;
        low.measures  = { m.id };
        low.direction = Direction::Low;
        low.threshold = 1.40;
        p.signalDefs.push_back(low);

        auto condition = [&](const char *id, const QString &sig) {
            Condition c;
            c.id         = QString::fromLatin1(id);
            c.detectedBy = { sig };
            c.state      = ConditionState::Active;
            p.conditions.push_back(c);
        };
        condition("c_high", high.id);
        condition("c_low", low.id);

        FakeSource above;
        above.addOneSided(m.id, 2.20, 1.48, 0.05, Shape::Floor);   // way past 1.00, on the open side
        const DetectionResult ra = detect(p, above);
        check(ra.find("c_high") && ra.find("c_high")->state == FindingState::NotFired,
              "a High threshold on a floor does not fire, however far past its number the value is");
        check(ra.find("c_low") && ra.find("c_low")->state == FindingState::NotFired,
              "…and the graded tail is quiet, because nothing is wrong");

        FakeSource below;
        below.addOneSided(m.id, 1.28, 1.48, 0.05, Shape::Floor);
        const DetectionResult rb = detect(p, below);
        check(rb.find("c_low") && rb.find("c_low")->state == FindingState::Fired,
              "the graded tail fires on its authored number as usual");
        check(rb.find("c_high") && rb.find("c_high")->state == FindingState::NotFired,
              "…and the open tail stays silent on the same swing");
    }

    // ── The ratio contract ─────────────────────────────────────────────────
    //
    // A quotient is DIMENSIONLESS and it is not a measure, so no norm can key on it. The branch used
    // to grade it against the NUMERATOR's corridor — a band in the numerator's own unit — which is
    // a category error rather than a mis-tuned test. It now grades against the signal's authored
    // number, the way Threshold does, and the validator requires both that number and a direction.
    //
    // The load-bearing check below is the last one: a numerator sitting comfortably inside its own
    // corridor whose quotient is still past the authored figure. Under the old branch that swing
    // could not fire; under this one the corridor is not consulted at all.
    std::printf("\nratio signals\n");
    {
        CharacteristicPack p;
        for (const char *id : { "m_backTime", "m_downTime" }) {
            Measure m;
            m.id     = QString::fromLatin1(id);
            m.unit   = QStringLiteral("s");
            m.status = MeasureStatus::Live;
            p.measures.push_back(m);
        }

        Signal tempo;
        tempo.id        = QStringLiteral("sigTempo");
        tempo.test      = SignalTest::Ratio;
        tempo.measures  = { QStringLiteral("m_backTime"), QStringLiteral("m_downTime") };
        tempo.direction = Direction::High;
        tempo.threshold = 3.0;
        p.signalDefs.push_back(tempo);

        Condition c;
        c.id         = QStringLiteral("c_slowTransition");
        c.detectedBy = { tempo.id };
        c.state      = ConditionState::Active;
        p.conditions.push_back(c);

        // No corridor anywhere, and the signal still works: the number came from the author.
        FakeSource slow;
        slow.addWithoutCorridor(QStringLiteral("m_backTime"), 1.5);
        slow.addWithoutCorridor(QStringLiteral("m_downTime"), 0.4);      // 3.75
        check(detect(p, slow).find("c_slowTransition")->state == FindingState::Fired,
              "a ratio past its authored number fires, with no corridor on either measure");

        FakeSource brisk;
        brisk.addWithoutCorridor(QStringLiteral("m_backTime"), 1.0);
        brisk.addWithoutCorridor(QStringLiteral("m_downTime"), 0.4);     // 2.5
        check(detect(p, brisk).find("c_slowTransition")->state == FindingState::NotFired,
              "…and a ratio short of it does not");

        // The other tail is a different fault and must be authorable as one.
        CharacteristicPack quick = p;
        quick.signalDefs.front().direction = Direction::Low;
        check(detect(quick, brisk).find("c_slowTransition")->state == FindingState::Fired,
              "the low tail of a ratio fires on the other side of the same number");
        check(detect(quick, slow).find("c_slowTransition")->state == FindingState::NotFired,
              "…and not on this one");

        // A zero denominator is not a very large ratio, it is no ratio at all.
        FakeSource zero;
        zero.addWithoutCorridor(QStringLiteral("m_backTime"), 1.5);
        zero.addWithoutCorridor(QStringLiteral("m_downTime"), 0.0);
        check(detect(p, zero).find("c_slowTransition")->state == FindingState::Unavailable,
              "a zero denominator is Unavailable, never a very large ratio");

        // Either half being unbelievable makes the whole quotient unassessed.
        for (const char *bad : { "m_backTime", "m_downTime" }) {
            FakeSource src;
            src.addWithoutCorridor(QStringLiteral("m_backTime"), 1.5);
            src.addWithoutCorridor(QStringLiteral("m_downTime"), 0.4);
            src.addImplausible(QString::fromLatin1(bad), 9999.0, 0.0, 10.0);
            const DetectionResult r = detect(p, src);
            check(r.find("c_slowTransition")->state == FindingState::Unavailable,
                  QByteArray("an implausible ").append(bad)
                      .append(" makes the ratio Unavailable, whichever half was wrong").constData());
        }

        // THE REGRESSION THIS CONTRACT EXISTS FOR. The numerator is dead centre of its own corridor
        // — a wholly ordinary backswing — and the quotient is still 3.75. The old branch compared
        // 3.75 against greenHi = 2.0 SECONDS and answered by accident; this one compares it against
        // the 3.0 the author wrote, and the corridor is not read.
        FakeSource graded;
        graded.add(QStringLiteral("m_backTime"), 1.5, 1.0, 2.0);        // inside, grades Ideal
        graded.addWithoutCorridor(QStringLiteral("m_downTime"), 0.4);
        check(detect(p, graded).find("c_slowTransition")->state == FindingState::Fired,
              "the ratio is graded against its authored number, not the numerator's corridor");

        // …and the mirror: a numerator OUTSIDE its corridor whose quotient is fine says nothing.
        FakeSource wide;
        wide.add(QStringLiteral("m_backTime"), 1.0, 0.1, 0.2);          // far above its band
        wide.addWithoutCorridor(QStringLiteral("m_downTime"), 0.5);     // 2.0, short of 3.0
        check(detect(p, wide).find("c_slowTransition")->state == FindingState::NotFired,
              "…and a numerator outside its own corridor does not fire a ratio whose quotient is fine");
    }

    std::printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "OK", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
