// The assembled-library health checks (diagnostics_health.h) — the ones no single validator can make.
//
// Every check here is a statement about content, and content is exactly what a test can stop being
// silently wrong. Two of them are easy to get backwards and are pinned in BOTH directions:
//
//   * `personalNormNoSample` must fire on the user's own n = 0 rows and NEVER on the 39 shipped
//     migrated ones. Unscoped, the health list opens with 39 items of noise about content that was
//     fine yesterday — the plan says so outright, and this is where that is enforced.
//   * the unread edge of a single-tail axis must NOT be reported. `s_posture` reads one end of
//     `lumbar_curve`; the norm is a single two-sided row, so the "missing" tail is a condition nobody
//     authored, not a corridor anybody is missing.
//
// It also runs the whole set over the SHIPPED library, because a check that cannot fire on real
// content is a check nobody will notice is broken.
//
//   cmake --build build/analyzer-tests --target diagnostics_health_test
//   ctest --test-dir build/analyzer-tests -R diagnostics_health --output-on-failure

#include "../diagnostics_health.h"
#include "../pack_provider.h"

#include <QFile>

#include <cstdio>
#include <memory>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static int countCode(const std::vector<ValidationIssue> &issues, const char *code)
{
    int n = 0;
    for (const ValidationIssue &i : issues)
        if (i.code == QLatin1String(code)) ++n;
    return n;
}

static bool hasSubject(const std::vector<ValidationIssue> &issues, const char *code,
                       const QString &subject)
{
    for (const ValidationIssue &i : issues)
        if (i.code == QLatin1String(code) && i.subject == subject) return true;
    return false;
}

// ── A library of our own ────────────────────────────────────────────────────
namespace {

class FakeNorms final : public INormProvider {
public:
    FakeNorms()
    {
        m_contexts = ContextTree(std::vector<ContextNode>{
            { QStringLiteral("any"),        QStringLiteral("Any"),    QString() },
            { QStringLiteral("full_swing"), QStringLiteral("Full"),   QStringLiteral("any") },
            { QStringLiteral("driver"),     QStringLiteral("Driver"), QStringLiteral("full_swing") },
            { QStringLiteral("wedge"),      QStringLiteral("Wedge"),  QStringLiteral("full_swing") },
        });
    }

    // `mine` marks the row as coming from a non-core layer, which is what scopes the personal-layer
    // check. Tracked, exactly as the merged provider tracks it — never derived by comparing values.
    void add(const char *measure, const char *context, double mu, double sigma, int n = 0,
             bool mine = false, const char *unit = "°")
    {
        Norm norm;
        norm.measureId = QLatin1String(measure);
        norm.contextId = QLatin1String(context);
        norm.mu        = mu;
        norm.sigmaLo = norm.sigmaHi = sigma;
        norm.n         = n;
        norm.unit      = QLatin1String(unit);
        m_norms.upsert(norm);
        if (mine) m_mine.insert(normKeyLabel(norm));
        else      m_shipped.upsert(norm);
    }

    // The same, qualified to a cohort. Separate rather than a defaulted argument on add(), because
    // the cohort is part of the KEY and a caller passing it should be seen to be doing so.
    void addFor(const Cohort &cohort, const char *measure, const char *context, double mu,
                double sigma)
    {
        Norm norm;
        norm.measureId = QLatin1String(measure);
        norm.contextId = QLatin1String(context);
        norm.cohort    = cohort;
        norm.mu        = mu;
        norm.sigmaLo = norm.sigmaHi = sigma;
        norm.unit      = QStringLiteral("°");
        m_norms.upsert(norm);
        m_shipped.upsert(norm);
    }

    // Give a user row a base, and move the shipped row out from under it.
    void rebase(const char *measure, const char *context, double baseMu, double shippedMu)
    {
        const QString mid = QLatin1String(measure), cid = QLatin1String(context);
        for (Norm &n : m_norms.norms) {
            if (n.measureId != mid || n.contextId != cid) continue;
            NormBasis b;
            b.mu = baseMu; b.sigmaLo = b.sigmaHi = 1.0;
            n.basedOn = b;
        }
        Norm shipped;
        shipped.measureId = mid;
        shipped.contextId = cid;
        shipped.mu        = shippedMu;
        shipped.sigmaLo = shipped.sigmaHi = 1.0;
        shipped.unit      = QStringLiteral("°");
        m_shipped.upsert(shipped);
    }

    // The corridor is left EXACTLY where it was; only the plausibility cap moves. That is the
    // case the message split exists for: a notice reading "was 10 to 12, has since been revised to
    // 10 to 12" is a notice nobody can act on.
    void rebaseCapOnly(const char *measure, const char *context,
                       std::optional<double> baseCap, std::optional<double> shippedCap)
    {
        const QString mid = QLatin1String(measure), cid = QLatin1String(context);
        for (Norm &n : m_norms.norms) {
            if (n.measureId != mid || n.contextId != cid) continue;
            NormBasis b;
            b.mu = 10.0; b.sigmaLo = b.sigmaHi = 1.0;
            b.plausibleHi = baseCap;
            n.basedOn = b;
        }
        Norm shipped;
        shipped.measureId   = mid;
        shipped.contextId   = cid;
        shipped.mu          = 10.0;
        shipped.sigmaLo     = shipped.sigmaHi = 1.0;
        shipped.plausibleHi = shippedCap;
        shipped.unit        = QStringLiteral("°");
        m_shipped.upsert(shipped);
    }

    const NormPack         &norms() const override    { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override   { return m_report; }
    QString                 label() const override    { return QStringLiteral("fake"); }
    PackOrigin              origin() const override   { return PackOrigin::Core; }

    const Norm *shippedNorm(const QString &measureId, const QString &contextId,
                            const Cohort &cohort) const override
    {
        return m_shipped.find(measureId, contextId, cohort);
    }
    bool isOverridden(const QString &measureId, const QString &contextId,
                      const Cohort &cohort) const override
    {
        return m_mine.contains(normKeyLabel(measureId, contextId, cohort));
    }

private:
    NormPack         m_norms;
    NormPack         m_shipped;
    QSet<QString>    m_mine;
    ContextTree      m_contexts;
    ValidationReport m_report;
};

// A minimal pack: one live measure with a corridor signal, one live measure whose signal has no
// norm, one measure with no producer, and a single-tail axis.
CharacteristicPack fakePack()
{
    CharacteristicPack p;

    auto measure = [&](const char *id, MeasureStatus st, const char *metricKey = "") {
        Measure m;
        m.id        = QLatin1String(id);
        m.kind      = MeasureKind::Provided;
        m.metricKey = QLatin1String(metricKey);
        m.label     = QLatin1String(id);
        m.status    = st;
        m.unit      = QStringLiteral("°");
        m.reducer   = Reducer{};
        m.highMeans = QStringLiteral("more of it");
        p.measures.push_back(m);
    };
    measure("m_normed",     MeasureStatus::Live);
    measure("m_unnormed",   MeasureStatus::Live);
    measure("m_noProducer", MeasureStatus::NoProducer);

    auto signalDef = [&](const char *id, const char *measureId, Direction d) {
        Signal s;
        s.id        = QLatin1String(id);
        s.test      = SignalTest::OutsideCorridor;
        s.measures  = QStringList{ QLatin1String(measureId) };
        s.direction = d;
        p.signalDefs.push_back(s);
    };
    signalDef("sig_normed",     "m_normed",     Direction::High);
    signalDef("sig_unnormed",   "m_unnormed",   Direction::High);
    signalDef("sig_noProducer", "m_noProducer", Direction::High);
    signalDef("sig_bothA",      "m_normed",     Direction::High);
    signalDef("sig_bothB",      "m_normed",     Direction::Low);

    // NB `signalIds`, not `signals` — Qt #defines that to `public`, and a parameter named for it
    // expands into a syntax error two lines later that says nothing about the cause.
    auto condition = [&](const char *id, const QStringList &signalIds, const char *axis = "") {
        Condition c;
        c.id            = QLatin1String(id);
        c.label         = QLatin1String(id);
        c.observability = Observability::Observable;
        c.detectedBy    = signalIds;
        c.axis          = QLatin1String(axis);
        p.conditions.push_back(c);
    };
    condition("c_ok",         { QStringLiteral("sig_normed") });
    condition("c_noNorm",     { QStringLiteral("sig_unnormed") });
    condition("c_noProducer", { QStringLiteral("sig_noProducer") });
    condition("c_bothTails",  { QStringLiteral("sig_bothA"), QStringLiteral("sig_bothB") },
              "axis_single");
    return p;
}

} // namespace

int main()
{
    const MetricCatalogue cat = makeMetricCatalogue();

    // ── A corridor signal that cannot fire ──────────────────────────────────
    std::printf("=== a corridor signal with no norm cannot fire, and says so ===\n");
    {
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed", "full_swing", 10.0, 2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);

        check(hasSubject(issues, "signalNoNorm", QStringLiteral("sig_unnormed")),
              "a LIVE measure with no norm anywhere is reported");
        check(!hasSubject(issues, "signalNoNorm", QStringLiteral("sig_normed")),
              "a measure that has a norm is not");
        check(!hasSubject(issues, "signalNoNorm", QStringLiteral("sig_noProducer")),
              "a measure with NO PRODUCER is not — the roadmap owns that gap, and authoring a "
              "corridor would not fix it");
    }

    // ── A signal watching a tail that never grades ──────────────────────────
    std::printf("=== a signal on the OPEN tail of a one-sided measure is reported ===\n");
    {
        // Distinct in kind from signalNoNorm, and the difference decides which queue it belongs
        // to: a signal with no norm is waiting for work somebody could do, and this one can never
        // fire however much gets built, because the measure has no fault on that side. So this is
        // NOT scoped to Live — it is an author's misreading, visible the moment it is written.
        CharacteristicPack pack = fakePack();

        Measure floorM;
        floorM.id     = QStringLiteral("m_floor");
        floorM.label  = QStringLiteral("Smash factor");
        floorM.status = MeasureStatus::Live;
        floorM.shape  = Shape::Floor;
        pack.measures.push_back(floorM);

        Measure ceilM;
        ceilM.id     = QStringLiteral("m_ceiling");
        ceilM.status = MeasureStatus::Planned;          // deliberately NOT live
        ceilM.shape  = Shape::Ceiling;
        pack.measures.push_back(ceilM);

        auto sig = [&](const char *id, const char *mid, Direction d) {
            Signal s;
            s.id        = QString::fromLatin1(id);
            s.test      = SignalTest::OutsideCorridor;
            s.measures  = { QString::fromLatin1(mid) };
            s.direction = d;
            pack.signalDefs.push_back(s);
        };
        sig("sig_floorHigh", "m_floor",   Direction::High);   // the open tail — reported
        sig("sig_floorLow",  "m_floor",   Direction::Low);    // the graded tail — silent
        sig("sig_ceilLow",   "m_ceiling", Direction::Low);    // the open tail — reported
        sig("sig_ceilHigh",  "m_ceiling", Direction::High);   // the graded tail — silent

        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0);
        norms.add("m_unnormed", "full_swing", 10.0, 2.0);
        norms.add("m_floor",    "full_swing", 1.48, 0.05);
        norms.add("m_ceiling",  "full_swing", 0.0,  2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);

        check(hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_floorHigh")),
              "a High signal on a FLOOR is reported");
        check(hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_ceilLow")),
              "…and a Low signal on a CEILING, which is the mirror");
        check(!hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_floorLow")),
              "the tail that DOES grade is not reported");
        check(!hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_ceilHigh")),
              "…on either shape");
        check(!hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_normed")),
              "and a two-sided measure is never reported — both its tails grade");
        check(countCode(issues, "signalOnOpenTail") == 2, "exactly the two, and no others");

        // Not scoped to Live: m_ceiling is Planned and still reported, because no producer will
        // ever give a ceiling an upper... a lower fault.
        check(hasSubject(issues, "signalOnOpenTail", QStringLiteral("sig_ceilLow")),
              "a planned measure is reported too — this is not a producer backlog");
    }

    // ── A tail that grades with nothing behind it ───────────────────────────
    std::printf("=== the ungraded tail of a two-sided measure is reported ===\n");
    {
        // The exact mirror of signalOnOpenTail above. A Target measure with a norm grades BOTH
        // tails — sigmaHi defaults to sigmaLo — so a corridor carrying one condition still puts a
        // colour on the dashboard for the other side with no fault behind it.
        CharacteristicPack pack = fakePack();

        // Fields are set BEFORE the push_back: holding a reference into the vector across a later
        // push_back would dangle the moment it reallocates.
        auto measure = [&](const char *id, MeasureStatus st, Shape sh,
                           std::optional<Direction> unwatched = {},
                           const char              *reason    = "") {
            Measure m;
            m.id              = QString::fromLatin1(id);
            m.label           = QString::fromLatin1(id);
            m.kind            = MeasureKind::Provided;
            m.status          = st;
            m.shape           = sh;
            m.unwatchedTail   = unwatched;
            m.unwatchedReason = QString::fromLatin1(reason);
            pack.measures.push_back(m);
        };
        auto sig = [&](const char *id, const char *mid, Direction d) {
            Signal s;
            s.id        = QString::fromLatin1(id);
            s.test      = SignalTest::OutsideCorridor;
            s.measures  = { QString::fromLatin1(mid) };
            s.direction = d;
            pack.signalDefs.push_back(s);
        };

        measure("m_oneTail",    MeasureStatus::Planned, Shape::Target);  // reported
        measure("m_shaped",     MeasureStatus::Live,    Shape::Floor);   // shape answers it
        measure("m_noCorridor", MeasureStatus::Live,    Shape::Target);  // no norm — nothing grades
        measure("m_declared",   MeasureStatus::Live,    Shape::Target, Direction::Low,
                "The low tail is bounded by arm length.");
        measure("m_wrongTail",  MeasureStatus::Live,    Shape::Target, Direction::High,
                "Declared on the tail a signal already watches.");

        sig("sig_oneTail",     "m_oneTail",     Direction::High);   // low tail ungraded
        sig("sig_shaped",      "m_shaped",      Direction::Low);    // high tail does not grade
        sig("sig_noCorridor",  "m_noCorridor",  Direction::High);   // neither tail grades
        sig("sig_declared",    "m_declared",    Direction::High);   // low tail declared unwatched
        sig("sig_wrongTail",   "m_wrongTail",   Direction::High);   // declared the WATCHED tail

        FakeNorms norms;
        norms.add("m_normed",    "full_swing", 10.0, 2.0);
        norms.add("m_unnormed",  "full_swing", 10.0, 2.0);
        norms.add("m_oneTail",   "full_swing", 10.0, 2.0);
        norms.add("m_shaped",    "full_swing", 1.48, 0.05);
        norms.add("m_declared",  "full_swing", 10.0, 2.0);
        norms.add("m_wrongTail", "full_swing", 10.0, 2.0);
        // m_noCorridor deliberately gets none.

        const auto issues = diagnosticsHealth(pack, norms, cat);

        check(hasSubject(issues, "ungradedTail", QStringLiteral("m_oneTail")),
              "a Target measure with a norm and ONE direction is reported");
        check(!hasSubject(issues, "ungradedTail", QStringLiteral("m_normed")),
              "a measure with both tails authored is not");
        check(!hasSubject(issues, "ungradedTail", QStringLiteral("m_shaped")),
              "a FLOOR is not — the shape already stopped that tail grading");
        check(countCode(issues, "signalOnOpenTail") == 0,
              "…and the mirror check is untouched by any of this");
        check(!hasSubject(issues, "ungradedTail", QStringLiteral("m_noCorridor")),
              "a measure with no norm anywhere is not — NEITHER tail grades, so there is no "
              "colour on the dashboard to explain");
        check(!hasSubject(issues, "ungradedTail", QStringLiteral("m_noProducer")),
              "…which is why the producer-less fixture measure is silent too");
        check(!hasSubject(issues, "ungradedTail", QStringLiteral("m_declared")),
              "a tail declared deliberately unwatched, with a reason, is silenced");
        check(hasSubject(issues, "ungradedTail", QStringLiteral("m_wrongTail")),
              "…but declaring the tail a signal ALREADY watches silences nothing — the ungraded "
              "tail is still ungraded, and validatePack refuses the declaration separately");

        // m_unnormed carries a norm here and one direction, so it is the fourth legitimate row.
        check(countCode(issues, "ungradedTail") == 3,
              "exactly m_oneTail, m_wrongTail and the fixture's own single-tail measure");
    }

    // ── The single-tail axis must stay silent ───────────────────────────────
    std::printf("=== the unread edge of a single-tail axis is NOT reported ===\n");
    {
        // c_bothTails is the only condition on axis_single, and its measure has a two-sided norm.
        // Nothing in the health list may argue for the AXIS nobody finished.
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0);
        norms.add("m_unnormed", "full_swing", 10.0, 2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        for (const ValidationIssue &i : issues)
            if (i.code != QLatin1String("ungradedTail")
                && i.message.contains(QStringLiteral("tail"), Qt::CaseInsensitive))
                std::printf("      unexpected tail row: %s\n", qPrintable(i.message));
        check(countCode(issues, "singleTailAxis") == 0,
              "diagnosticsHealth does not duplicate the pack validator's axis check");
        check(countCode(issues, "signalNoNorm") == 0,
              "and with both measures normed, nothing claims a missing corridor");

        // The two are about different things and both are correct on this fixture: axis_single is
        // an AXIS with one tail declared (the pack validator's business, and c_bothTails is not on
        // it twice), while m_unnormed is a MEASURE whose second tail grades unexplained. Keying the
        // new check on the measure rather than the axis is what keeps them from colliding — most
        // ungraded tails belong to measures nobody ever gave an axis.
        check(hasSubject(issues, "ungradedTail", QStringLiteral("m_unnormed")),
              "…while the ungraded tail of a MEASURE is reported, which is a different claim");
    }

    // ── Your own corridors, seated on nothing ───────────────────────────────
    std::printf("=== n = 0 is reported in the PERSONAL layer only ===\n");
    {
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0, /*n*/ 0, /*mine*/ true);
        norms.add("m_unnormed", "full_swing", 10.0, 2.0, /*n*/ 0, /*mine*/ false);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(countCode(issues, "personalNormNoSample") == 1,
              "exactly one row — the user's own");
        check(hasSubject(issues, "personalNormNoSample", QStringLiteral("m_normed @ full_swing")),
              "…and it names the norm, both halves of the key");

        // The load-bearing one: a shipped row at n = 0 is normal and must be silent.
        check(!hasSubject(issues, "personalNormNoSample", QStringLiteral("m_unnormed @ full_swing")),
              "a SHIPPED n = 0 row is not reported — 39 of those exist and were fine yesterday");
    }

    // ── Cohort coverage ─────────────────────────────────────────────────────
    //
    // Both nudges must be SILENT on an unqualified set, which is every set that exists today. A
    // health list that opened with rows about a feature nobody had used would be the same mistake
    // `observableNoSignal` made.
    std::printf("=== cohort coverage: sex and age authored apart ===\n");
    {
        const auto cohortOf = [](std::optional<Sex> s, std::optional<AgeBand> a) {
            Cohort c;
            c.sex = s;
            c.age = a;
            return c;
        };

        {
            const CharacteristicPack pack = fakePack();
            FakeNorms norms;
            norms.add("m_normed",   "full_swing", 10.0, 2.0);
            norms.add("m_unnormed", "full_swing", 10.0, 2.0);
            check(countCode(diagnosticsHealth(pack, norms, cat), "cohortGap") == 0,
                  "an unqualified set says nothing about cohorts");
        }

        {
            const CharacteristicPack pack = fakePack();
            FakeNorms norms;
            norms.add("m_unnormed", "full_swing", 10.0, 2.0);
            norms.add("m_normed",   "full_swing", 10.0, 2.0);
            norms.addFor(cohortOf(Sex::Female, std::nullopt), "m_normed", "full_swing", 9.0, 2.0);
            norms.addFor(cohortOf(std::nullopt, AgeBand::Adult55_64), "m_normed", "full_swing",
                         8.0, 2.0);

            const auto issues = diagnosticsHealth(pack, norms, cat);
            check(countCode(issues, "cohortGap") == 1,
                  "a sex-only row beside an age-only row, with no combination, is one nudge");
            check(hasSubject(issues, "cohortGap", QStringLiteral("m_normed @ full_swing")),
                  "…named at the (measure, context) the probe order runs at");

            // The negative case, and the one that decides whether this check is worth having: adding
            // the combined row must silence it.
            norms.addFor(cohortOf(Sex::Female, AgeBand::Adult55_64), "m_normed", "full_swing",
                         7.0, 2.0);
            check(countCode(diagnosticsHealth(pack, norms, cat), "cohortGap") == 0,
                  "…and authoring the combination silences it");
        }

        // A parent band under a complete set of its own sub-bands can never resolve.
        {
            const CharacteristicPack pack = fakePack();
            FakeNorms norms;
            norms.add("m_unnormed", "full_swing", 10.0, 2.0);
            norms.add("m_normed",   "full_swing", 10.0, 2.0);
            norms.addFor(cohortOf(std::nullopt, AgeBand::Adult), "m_normed", "full_swing", 9.0, 2.0);
            norms.addFor(cohortOf(std::nullopt, AgeBand::Adult18_54), "m_normed", "full_swing",
                         8.0, 2.0);
            norms.addFor(cohortOf(std::nullopt, AgeBand::Adult55_64), "m_normed", "full_swing",
                         7.0, 2.0);
            check(countCode(diagnosticsHealth(pack, norms, cat), "shadowedCohort") == 0,
                  "with one sub-band still unauthored the parent row is reachable, and silent");

            norms.addFor(cohortOf(std::nullopt, AgeBand::Adult65Plus), "m_normed", "full_swing",
                         6.0, 2.0);
            check(countCode(diagnosticsHealth(pack, norms, cat), "shadowedCohort") == 1,
                  "…and once all three are authored the parent can never resolve, so it is reported");
        }
    }

    // ── Contexts nothing resolves for ───────────────────────────────────────
    //
    // Two findings that must not be confused. Inheritance is the POINT of the tree, so a context with
    // nothing of its own is fine — its parent grades it. A context with nothing anywhere up its chain
    // is not fine at all: a shot there is graded by nothing.
    std::printf("=== a context with nothing of its own vs one nothing grades ===\n");
    {
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0);
        norms.add("m_unnormed", "driver",     10.0, 2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(hasSubject(issues, "emptyContext", QStringLiteral("wedge")),
              "wedge carries nothing of its own, but full_swing above it does — a control with no "
              "effect, not a hole");
        check(!hasSubject(issues, "ungradedContext", QStringLiteral("wedge")),
              "…so it is NOT reported as ungraded");
        check(!hasSubject(issues, "emptyContext", QStringLiteral("full_swing")),
              "full_swing carries a row of its own");
        check(!hasSubject(issues, "emptyContext", QStringLiteral("any")),
              "nor is the root reported — full_swing sits beneath it");
    }
    {
        // Nothing at full_swing or above: now every context under it is graded by nothing at all.
        // This is the shipped tree's actual shape for partial / pitch / chip / bunker / specialty,
        // which hang off `any` where no norm is authored.
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed", "driver", 10.0, 2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(hasSubject(issues, "ungradedContext", QStringLiteral("wedge")),
              "with nothing anywhere up its chain, a shot in wedge is graded by NOTHING");
        check(!hasSubject(issues, "emptyContext", QStringLiteral("wedge")),
              "…and it is not softened into the harmless message");
        check(!hasSubject(issues, "ungradedContext", QStringLiteral("driver")),
              "driver has its own row and is neither");
    }

    // ── An override made against numbers since revised ──────────────────────
    std::printf("=== 'core has been revised since you overrode it' needs a base ===\n");
    {
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 20.0, 1.0, 0, /*mine*/ true);
        norms.add("m_unnormed", "full_swing", 20.0, 1.0, 0, /*mine*/ true);

        // Before any base exists, the check must be SILENT rather than reporting every override:
        // "yours differs from theirs" is what an override IS.
        check(countCode(diagnosticsHealth(pack, norms, cat), "overrideCoreChanged") == 0,
              "an override with no recorded base is silent — we do not know, so we do not say");

        norms.rebase("m_normed", "full_swing", /*base*/ 10.0, /*shipped now*/ 12.0);
        norms.rebase("m_unnormed", "full_swing", /*base*/ 10.0, /*shipped now*/ 10.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_normed @ full_swing")),
              "the shipped row moved away from the base → reported");
        check(!hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_unnormed @ full_swing")),
              "the shipped row still matches the base → not reported, however much yours differs");
    }
    {
        // ── A shipped row that GAINED a cap, with its corridor untouched ────
        //
        // NormBasis has carried the plausibility pair since A2, but nothing compared it and the
        // editor never wrote it — so acquiring a cap, which turns readings from Action into
        // NotMeasured, compared as unmoved and this notice stayed silent. A field complete on both
        // sides, reaching nothing.
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 20.0, 1.0, 0, /*mine*/ true);
        norms.add("m_unnormed", "full_swing", 20.0, 1.0, 0, /*mine*/ true);

        norms.rebaseCapOnly("m_normed",   "full_swing", std::nullopt, 15.0);   // gained a cap
        norms.rebaseCapOnly("m_unnormed", "full_swing", 15.0,         15.0);   // same cap as before

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_normed @ full_swing")),
              "a shipped row that gained a cap is reported, corridor unchanged or not");
        check(!hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_unnormed @ full_swing")),
              "…and an unchanged cap is still silent, so the check has not become noise");

        // …and it must not CALL it a corridor revision, because it is not one and the two numbers
        // it would quote are identical.
        QString msg;
        for (const ValidationIssue &i : issues)
            if (i.code == QLatin1String("overrideCoreChanged")
                && i.subject == QLatin1String("m_normed @ full_swing"))
                msg = i.message;
        check(!msg.isEmpty(), "the notice has a message");
        check(msg.contains(QLatin1String("BELIEVE")),
              "…which says what actually changed: what the row will believe");
        check(!msg.contains(QLatin1String("has since been revised")),
              "…and never claims a corridor revision that did not happen");
        check(msg.contains(QLatin1String("15")),
              "…naming the new cap");
        check(msg.contains(QLatin1String("any reading")),
              "…and saying there was none before, rather than printing a zero for the absence");
    }

    // ── The corpus-share check ──────────────────────────────────────────────
    std::printf("=== a corridor grading almost everything into one band ===\n");
    {
        std::vector<CorpusGradeCounts> counts;

        CorpusGradeCounts allAction;                       // the stage-6 screen: 11 Action out of 11
        allAction.measureId = QStringLiteral("m_stanceWidth");
        allAction.contextId = QStringLiteral("full_swing");
        allAction.action    = 11;
        counts.push_back(allAction);

        CorpusGradeCounts allIdeal;                        // the opposite failure: too wide to speak
        allIdeal.measureId = QStringLiteral("m_wide");
        allIdeal.contextId = QStringLiteral("full_swing");
        allIdeal.ideal     = 40;
        counts.push_back(allIdeal);

        CorpusGradeCounts healthy;
        healthy.measureId = QStringLiteral("m_ok");
        healthy.contextId = QStringLiteral("full_swing");
        healthy.ideal = 20; healthy.good = 10; healthy.watch = 4; healthy.action = 2;
        counts.push_back(healthy);

        CorpusGradeCounts tiny;                            // three in one band is a Tuesday
        tiny.measureId = QStringLiteral("m_tiny");
        tiny.contextId = QStringLiteral("full_swing");
        tiny.action    = 3;
        counts.push_back(tiny);

        const auto issues = corpusShareHealth(counts);
        check(hasSubject(issues, "oneBandCorpus", QStringLiteral("m_stanceWidth @ full_swing")),
              "everything Action is reported");
        check(hasSubject(issues, "oneBandCorpus", QStringLiteral("m_wide @ full_swing")),
              "everything Ideal is reported too — a corridor that can never report a deviation");
        check(!hasSubject(issues, "oneBandCorpus", QStringLiteral("m_ok @ full_swing")),
              "a spread distribution is not reported");
        check(!hasSubject(issues, "oneBandCorpus", QStringLiteral("m_tiny @ full_swing")),
              "too few readings to mean anything is not reported");
        check(countCode(issues, "oneBandCorpus") == 2, "one row per corridor, not one per band");

        // The two failures have opposite fixes, so the message has to distinguish them.
        for (const ValidationIssue &i : issues) {
            if (i.subject != QLatin1String("m_wide @ full_swing")) continue;
            check(i.message.contains(QStringLiteral("cannot report a deviation")),
                  "the too-wide case says what is wrong with it, not just which band won");
        }
    }

    // ── The referential norm checks, which nothing used to run ──────────────
    std::printf("=== the referential norm validation is actually run ===\n");
    {
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0, 0, false, "%");   // measure is in °
        norms.add("m_unnormed", "full_swing", 10.0, 2.0);
        norms.add("m_ghost",    "full_swing", 10.0, 2.0);                  // no such measure

        const auto issues = diagnosticsHealth(pack, norms, cat);
        check(countCode(issues, "normUnitMismatch") >= 1,
              "a norm in % against a measure in ° is reported — the check existed and nothing ran it");
        check(countCode(issues, "unknownNormMeasure") >= 1,
              "a norm keyed on a measure the library does not have is reported");
    }

    // ── The shipped library ─────────────────────────────────────────────────
    //
    // Run for real. This is not a "no issues" assertion — the shipped set has known content debts
    // (the plan's ledger) and asserting zero would be asserting the debts away. What must hold is
    // that the checks RUN over real content, and that the two rules with a documented failure mode
    // hold there too.
    std::printf("=== over the shipped library ===\n");
    {
        const std::unique_ptr<ICharacteristicPackProvider> packProv = makeCharacteristicPackProvider();
        const std::shared_ptr<const INormProvider>         norms    = sharedNormProvider();
        const CharacteristicPack                          &pack     = packProv->pack();

        check(!pack.measures.empty(), "the shipped pack loaded");
        check(!norms->norms().norms.empty(), "the shipped norm set loaded");

        const auto issues = diagnosticsHealth(pack, *norms, cat);
        std::printf("      %zu health rows over the shipped library\n", issues.size());
        for (const ValidationIssue &i : issues)
            std::printf("        [%s] %s\n", qPrintable(i.code), qPrintable(i.subject));

        // THE GATE for "shot type informs a diagnosis, it never gates one". Every context must
        // resolve corridors: the general rows live at `any`, the root of the tree, so every chain
        // reaches them. Before that content change they sat at `full_swing` — a SIBLING of partial /
        // bunker / specialty — and a pitch or bunker shot resolved nothing at all, so every reading
        // came back not measured and no corridor signal could fire for a whole class of shot.
        check(countCode(issues, "ungradedContext") == 0,
              "NO shipped context is graded by nothing — shot type informs, it does not gate");

        // THE GATE for "every tail that grades has a fault behind it, or a stated reason why it
        // does not". This one shipped at 37 the day the check was written: 37 corridors were
        // putting a Watch or an Action on the dashboard for a tail with no fault name, no
        // consequence and no drill behind it, and nothing reported it because a half-authored
        // corridor looks exactly like a finished one. Unlike the debts printed above, a new row
        // here is not a backlog item — it is a colour a golfer can already see with nothing to say
        // about it, so it is gated rather than listed.
        check(countCode(issues, "ungradedTail") == 0,
              "NO shipped corridor grades a tail that nothing explains");
        check(countCode(issues, "signalOnOpenTail") == 0,
              "…and its mirror stays clean: nothing watches a tail that never grades");

        // No shipped row is anybody's override, so neither personal-layer check may fire.
        check(countCode(issues, "personalNormNoSample") == 0,
              "NOTHING shipped is reported as a personal n = 0 row — this is the 39-item noise gate");
        check(countCode(issues, "overrideCoreChanged") == 0,
              "and nothing shipped claims to override a revised core row");

        // The referential checks must come back CLEAN on shipped content: a unit mismatch or a norm
        // keyed on a measure that does not exist is a content error, not a debt.
        check(countCode(issues, "normUnitMismatch") == 0, "no shipped norm is in the wrong unit");
        check(countCode(issues, "unknownNormMeasure") == 0, "every shipped norm keys on a real measure");
        check(countCode(issues, "unknownNormContext") == 0, "every shipped norm keys on a real context");
        check(countCode(issues, "normNotCapturable") == 0,
              "no shipped norm sits on a measure no sensor can produce");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
