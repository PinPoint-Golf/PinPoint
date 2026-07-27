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
        if (mine) m_mine.insert(QLatin1String(measure) + QLatin1Char('@') + QLatin1String(context));
        else      m_shipped.upsert(norm);
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

    const NormPack         &norms() const override    { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override   { return m_report; }
    QString                 label() const override    { return QStringLiteral("fake"); }
    PackOrigin              origin() const override   { return PackOrigin::Core; }

    const Norm *shippedNorm(const QString &measureId, const QString &contextId) const override
    {
        return m_shipped.find(measureId, contextId);
    }
    bool isOverridden(const QString &measureId, const QString &contextId) const override
    {
        return m_mine.contains(measureId + QLatin1Char('@') + contextId);
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

    // ── The single-tail axis must stay silent ───────────────────────────────
    std::printf("=== the unread edge of a single-tail axis is NOT reported ===\n");
    {
        // c_bothTails is the only condition on axis_single, and its measure has a two-sided norm.
        // Nothing in the health list may argue for the tail nobody authored.
        const CharacteristicPack pack = fakePack();
        FakeNorms norms;
        norms.add("m_normed",   "full_swing", 10.0, 2.0);
        norms.add("m_unnormed", "full_swing", 10.0, 2.0);

        const auto issues = diagnosticsHealth(pack, norms, cat);
        for (const ValidationIssue &i : issues)
            if (i.message.contains(QStringLiteral("tail"), Qt::CaseInsensitive))
                std::printf("      unexpected tail row: %s\n", qPrintable(i.message));
        check(countCode(issues, "singleTailAxis") == 0,
              "diagnosticsHealth does not duplicate the pack validator's axis check");
        check(countCode(issues, "signalNoNorm") == 0,
              "and with both measures normed, nothing claims a missing corridor");
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
        check(hasSubject(issues, "personalNormNoSample", QStringLiteral("m_normed@full_swing")),
              "…and it names the norm, both halves of the key");

        // The load-bearing one: a shipped row at n = 0 is normal and must be silent.
        check(!hasSubject(issues, "personalNormNoSample", QStringLiteral("m_unnormed@full_swing")),
              "a SHIPPED n = 0 row is not reported — 39 of those exist and were fine yesterday");
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
        check(hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_normed@full_swing")),
              "the shipped row moved away from the base → reported");
        check(!hasSubject(issues, "overrideCoreChanged", QStringLiteral("m_unnormed@full_swing")),
              "the shipped row still matches the base → not reported, however much yours differs");
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
        check(hasSubject(issues, "oneBandCorpus", QStringLiteral("m_stanceWidth@full_swing")),
              "everything Action is reported");
        check(hasSubject(issues, "oneBandCorpus", QStringLiteral("m_wide@full_swing")),
              "everything Ideal is reported too — a corridor that can never report a deviation");
        check(!hasSubject(issues, "oneBandCorpus", QStringLiteral("m_ok@full_swing")),
              "a spread distribution is not reported");
        check(!hasSubject(issues, "oneBandCorpus", QStringLiteral("m_tiny@full_swing")),
              "too few readings to mean anything is not reported");
        check(countCode(issues, "oneBandCorpus") == 2, "one row per corridor, not one per band");

        // The two failures have opposite fixes, so the message has to distinguish them.
        for (const ValidationIssue &i : issues) {
            if (i.subject != QLatin1String("m_wide@full_swing")) continue;
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
