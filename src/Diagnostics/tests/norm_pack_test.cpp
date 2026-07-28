// Standalone tests for norm persistence, validation and layering
// (src/Diagnostics/norm_pack.*, *_norm_provider.cpp).
//
// The rules under test are the ones that would otherwise fail silently:
//
//   * A unit mismatch is a LOAD ERROR naming both sides. A norm authored in degrees against a
//     measure that later became a percentage still loads, still grades, and is wrong every time.
//   * A user row REPLACES a core row at the same key, but CONTEXT SPECIFICITY BEATS LAYER
//     PRECEDENCE — a user adjusting the general full-swing norm must not silently override every
//     club-specific shipped norm beneath it.
//   * Reverting an override restores the shipped norm exactly.
//
//   cmake --build build/analyzer-tests --target norm_pack_test
//   ctest --test-dir build/analyzer-tests -R norm_pack_test --output-on-failure

#include "../norm_provider.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cstdio>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }

static bool hasCode(const ValidationReport &r, const char *code)
{
    for (const ValidationIssue &i : r.issues)
        if (i.code == QLatin1String(code)) return true;
    return false;
}

static Norm makeNorm(const char *measure, const char *context, double mu, double sigma,
                     const char *unit = "°")
{
    Norm n;
    n.measureId = QLatin1String(measure);
    n.contextId = QLatin1String(context);
    n.mu        = mu;
    n.sigmaLo   = sigma;
    n.sigmaHi   = sigma;
    n.unit      = QLatin1String(unit);
    return n;
}

static ContextTree sampleTree()
{
    return ContextTree(std::vector<ContextNode>{
        { QStringLiteral("any"),        QStringLiteral("Any"),        QString() },
        { QStringLiteral("full_swing"), QStringLiteral("Full swing"), QStringLiteral("any") },
        { QStringLiteral("driver"),     QStringLiteral("Driver"),     QStringLiteral("full_swing") },
        { QStringLiteral("wedge"),      QStringLiteral("Wedge"),      QStringLiteral("full_swing") },
    });
}

// A provider over an in-memory set, so layering can be tested without touching disk.
class FakeNormProvider final : public INormProvider {
public:
    FakeNormProvider(NormPack pack, ContextTree tree, PackOrigin origin)
        : m_norms(std::move(pack)), m_contexts(std::move(tree)), m_origin(origin) {}

    const NormPack         &norms() const override { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override { return m_report; }
    QString                 label() const override { return QStringLiteral("fake"); }
    PackOrigin              origin() const override { return m_origin; }

private:
    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
    PackOrigin       m_origin;
};

static std::unique_ptr<INormProvider> fake(NormPack p, PackOrigin origin)
{
    return std::make_unique<FakeNormProvider>(std::move(p), sampleTree(), origin);
}

int main()
{
    std::printf("=== norm pack: JSON round-trip ===\n");
    {
        NormPack in;
        in.id      = QStringLiteral("core");
        in.version = QStringLiteral("1.0.0");

        Norm a      = makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%");
        a.sigmaHi   = 9.0;                    // asymmetric: forward is tolerated further
        a.monitorLo = -6.0;
        a.monitorHi = 30.0;
        a.n         = 42;
        a.source    = NormSource::Seated;
        a.author    = QStringLiteral("ML");
        a.citation  = QStringLiteral("10.1000/example");
        a.setOn     = QDate(2026, 7, 25);
        in.norms.push_back(a);
        in.norms.push_back(makeNorm("m_stanceWidth", "full_swing", 100.0, 10.0, "%"));

        const NormPackLoadResult res = loadNormPack(saveNormPack(in));
        check(res.loaded, "a saved norm set loads clean");
        check(res.pack.norms.size() == 2, "both rows survive");

        const Norm *back = res.pack.find(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(back != nullptr, "the row is found by (measure, context)");
        if (back) {
            check(near(back->mu, 12.0) && near(back->sigmaLo, 4.0) && near(back->sigmaHi, 9.0),
                  "asymmetric tolerances survive the round-trip");
            check(back->hasExplicitMonitor() && near(*back->monitorLo, -6.0)
                      && near(*back->monitorHi, 30.0),
                  "explicit monitor bounds survive the round-trip");
            check(back->n == 42 && back->source == NormSource::Seated,
                  "provenance survives the round-trip");
            check(back->author == QLatin1String("ML")
                      && back->citation == QLatin1String("10.1000/example")
                      && back->setOn == QDate(2026, 7, 25),
                  "author, citation and date survive the round-trip");
        }

        // A symmetric norm is written once, not twice — sigmaHi defaults to sigmaLo on read.
        const Norm *sym = res.pack.find(QStringLiteral("m_stanceWidth"),
                                        QStringLiteral("full_swing"));
        check(sym != nullptr && near(sym->sigmaLo, 10.0) && near(sym->sigmaHi, 10.0),
              "an omitted sigmaHi defaults to sigmaLo");
        const QJsonObject obj = saveNormPack(in);
        const QJsonArray  arr = obj.value(QStringLiteral("norms")).toArray();
        check(!arr.at(1).toObject().contains(QStringLiteral("sigmaHi")),
              "a symmetric norm omits sigmaHi on write");
    }

    std::printf("=== norm pack: standalone validation ===\n");
    {
        NormPack p;
        p.norms.push_back(makeNorm("m_a", "full_swing", 0.0, 1.0));
        p.norms.push_back(makeNorm("m_a", "full_swing", 5.0, 1.0));
        check(hasCode(validateNormPack(p), "duplicateNorm"), "duplicate (measure, context) is an error");

        NormPack neg;
        Norm     n = makeNorm("m_a", "full_swing", 0.0, 1.0);
        n.sigmaLo  = -1.0;
        neg.norms.push_back(n);
        check(hasCode(validateNormPack(neg), "negativeSigma"), "a negative tolerance is an error");

        // partialMonitor is NOT decidable here any more and has moved to validateNormsAgainst —
        // one bound is a COMPLETE monitor band on a one-sided measure, and this validator cannot
        // see the measure. Asserted silent in both directions so the move cannot be undone by
        // accident.
        NormPack partial;
        Norm     pn  = makeNorm("m_a", "full_swing", 0.0, 1.0);
        pn.monitorLo = -5.0;                 // no monitorHi
        partial.norms.push_back(pn);
        check(!hasCode(validateNormPack(partial), "partialMonitor"),
              "half a monitor band is not decidable without the measure's shape");

        // Plausibility order needs no shape — it is a claim about capture, not about grading.
        NormPack badPlaus;
        Norm     bp    = makeNorm("m_a", "full_swing", 0.0, 1.0);
        bp.plausibleLo = 9.0;
        bp.plausibleHi = 1.0;
        badPlaus.norms.push_back(bp);
        check(hasCode(validateNormPack(badPlaus), "plausibleOrder"),
              "plausibleLo above plausibleHi is an error");

        NormPack onePlaus;
        Norm     op    = makeNorm("m_a", "full_swing", 0.0, 1.0);
        op.plausibleHi = 9.0;                // a cap above and nothing below
        onePlaus.norms.push_back(op);
        check(validateNormPack(onePlaus).ok(), "…but a bound stated singly is legal");

        NormPack ordered;
        Norm     on  = makeNorm("m_a", "full_swing", 0.0, 1.0);
        on.monitorLo = 5.0;
        on.monitorHi = -5.0;
        ordered.norms.push_back(on);
        check(hasCode(validateNormPack(ordered), "monitorOrder"),
              "monitorLo above monitorHi is an error");

        // The subtle one: a monitor band that does not contain its own ideal band means a value
        // sitting inside its tolerance grades Action. That reads as a detection but is a typo.
        NormPack excl;
        Norm     en  = makeNorm("m_a", "full_swing", 0.0, 10.0);
        en.monitorLo = -2.0;
        en.monitorHi = 2.0;
        excl.norms.push_back(en);
        check(hasCode(validateNormPack(excl), "monitorExcludesIdeal"),
              "a monitor band inside the ideal band is an error");

        NormPack empty;
        empty.norms.push_back(makeNorm("", "full_swing", 0.0, 1.0));
        check(hasCode(validateNormPack(empty), "emptyNormKey"), "a keyless norm is an error");

        NormPack zero;
        zero.norms.push_back(makeNorm("m_a", "full_swing", 0.0, 0.0));
        check(hasCode(validateNormPack(zero), "zeroSigma"), "a zero tolerance warns");
        check(validateNormPack(zero).ok(), "…but only warns; the set still loads");
    }

    std::printf("=== norm pack: referential validation ===\n");
    {
        CharacteristicPack pack;
        Measure m;
        m.id     = QStringLiteral("m_ballPosition");
        m.unit   = QStringLiteral("%");
        m.status = MeasureStatus::Live;
        pack.measures.push_back(m);

        Measure gap;
        gap.id     = QStringLiteral("m_ungettable");
        gap.unit   = QStringLiteral("°");
        gap.status = MeasureStatus::NotCapturable;
        pack.measures.push_back(gap);

        const ContextTree tree = sampleTree();

        NormPack good;
        good.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        check(validateNormsAgainst(good, pack, tree).ok(), "a well-formed norm validates clean");

        NormPack unknownMeasure;
        unknownMeasure.norms.push_back(makeNorm("m_nope", "driver", 0.0, 1.0, "%"));
        check(hasCode(validateNormsAgainst(unknownMeasure, pack, tree), "unknownNormMeasure"),
              "a norm on a measure the library lacks is an error");

        NormPack unknownContext;
        unknownContext.norms.push_back(makeNorm("m_ballPosition", "hovercraft", 0.0, 1.0, "%"));
        check(hasCode(validateNormsAgainst(unknownContext, pack, tree), "unknownNormContext"),
              "a norm on a context the tree lacks is an error");

        // Unit drift: the quiet failure this check exists for.
        NormPack wrongUnit;
        wrongUnit.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "°"));
        const ValidationReport ur = validateNormsAgainst(wrongUnit, pack, tree);
        check(hasCode(ur, "normUnitMismatch"), "a unit mismatch is an ERROR, not a warning");
        bool namesBoth = false;
        for (const ValidationIssue &i : ur.issues)
            if (i.code == QLatin1String("normUnitMismatch"))
                namesBoth = i.message.contains(QLatin1String("°"))
                            && i.message.contains(QLatin1String("%"));
        check(namesBoth, "the message names BOTH units, so the fix is obvious");

        NormPack onGap;
        onGap.norms.push_back(makeNorm("m_ungettable", "driver", 0.0, 1.0, "°"));
        check(hasCode(validateNormsAgainst(onGap, pack, tree), "normNotCapturable"),
              "a norm on a not-capturable measure is refused");
    }

    std::printf("=== norm pack: a norm's numbers against the measure's SHAPE ===\n");
    {
        // Shape is on the MEASURE and the norm carries only numbers, so this join is the only
        // place the two can be checked against each other — exactly like normUnitMismatch.
        CharacteristicPack pack;
        Measure target;
        target.id     = QStringLiteral("m_ballPosition");
        target.unit   = QStringLiteral("%");
        target.status = MeasureStatus::Live;
        pack.measures.push_back(target);

        Measure floor;
        floor.id     = QStringLiteral("m_smash");
        floor.unit   = QStringLiteral("ratio");
        floor.status = MeasureStatus::Live;
        floor.shape  = Shape::Floor;
        pack.measures.push_back(floor);

        Measure ceiling;
        ceiling.id     = QStringLiteral("m_heelLift");
        ceiling.unit   = QStringLiteral("cm");
        ceiling.status = MeasureStatus::Live;
        ceiling.shape  = Shape::Ceiling;
        pack.measures.push_back(ceiling);

        const ContextTree tree = sampleTree();

        // ── STAYS SILENT ────────────────────────────────────────────────────
        // Half the value of this test is the negative cases: a check that cannot stay quiet is
        // worse than no check.
        NormPack terseFloor;
        terseFloor.norms.push_back(makeNorm("m_smash", "driver", 1.48, 0.05, "ratio"));
        check(validateNormsAgainst(terseFloor, pack, tree).ok(),
              "a floor stating ONE tolerance validates clean");

        // An explicitly-equal sigmaHi is indistinguishable from the parse default (readNorm mirrors
        // sigmaLo when sigmaHi is absent), so it must not be an error — it is the terse form.
        NormPack equalSigmas = terseFloor;
        equalSigmas.norms[0].sigmaHi = equalSigmas.norms[0].sigmaLo;
        check(validateNormsAgainst(equalSigmas, pack, tree).ok(),
              "…and so does one stating the same value twice");

        // The GRADED side's monitor is legal on a floor: it is the low tail that grades.
        NormPack gradedMonitor = terseFloor;
        gradedMonitor.norms[0].monitorLo = 1.20;
        check(!hasCode(validateNormsAgainst(gradedMonitor, pack, tree), "normShapeMonitor"),
              "a monitorLo on a FLOOR is the graded tail and is legal");

        NormPack asymTarget;
        asymTarget.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        asymTarget.norms[0].sigmaHi = 9.0;
        check(validateNormsAgainst(asymTarget, pack, tree).ok(),
              "an asymmetric TARGET is normal, not exotic — nothing fires");

        // ── FIRES ───────────────────────────────────────────────────────────
        NormPack asymFloor = terseFloor;
        asymFloor.norms[0].sigmaHi = 0.30;
        check(hasCode(validateNormsAgainst(asymFloor, pack, tree), "normShapeTolerance"),
              "a floor with two DIFFERENT tolerances is refused");

        NormPack floorHiMonitor = terseFloor;
        floorHiMonitor.norms[0].monitorHi = 1.90;
        check(hasCode(validateNormsAgainst(floorHiMonitor, pack, tree), "normShapeMonitor"),
              "a monitorHi on a floor names a tail nothing grades");

        NormPack ceilLoMonitor;
        ceilLoMonitor.norms.push_back(makeNorm("m_heelLift", "driver", 0.0, 2.0, "cm"));
        ceilLoMonitor.norms[0].monitorLo = -5.0;
        check(hasCode(validateNormsAgainst(ceilLoMonitor, pack, tree), "normShapeMonitor"),
              "…and the ceiling mirrors it: monitorLo is the open tail there");

        // The message has to name the measure AND what its shape means, because the author is
        // reading it without the pack open beside them.
        const ValidationReport sr = validateNormsAgainst(asymFloor, pack, tree);
        bool namesBoth = false;
        for (const ValidationIssue &i : sr.issues)
            if (i.code == QLatin1String("normShapeTolerance"))
                namesBoth = i.message.contains(QLatin1String("m_smash"))
                            && i.message.contains(shapeLabel(Shape::Floor));
        check(namesBoth, "the message names the measure and what its shape means");

        // ── partialMonitor, now that the shape is in hand ───────────────────
        //
        // The same row is an error on a Target and correct on a Floor, which is exactly why the
        // check could not stay in the standalone validator.
        NormPack halfOnTarget;
        halfOnTarget.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        halfOnTarget.norms[0].monitorLo = 0.0;
        check(hasCode(validateNormsAgainst(halfOnTarget, pack, tree), "partialMonitor"),
              "half a monitor band on a TARGET is half a rule");
        check(!hasCode(validateNormsAgainst(gradedMonitor, pack, tree), "partialMonitor"),
              "…and the same row on a FLOOR is complete");

        // ── Plausibility must not cut into the corridor ─────────────────────
        //
        // Otherwise a reading is graded Action and disbelieved at the same time, and which answer
        // surfaces depends on the order two checks happen to run in.
        NormPack biting;
        biting.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        biting.norms[0].plausibleHi = 13.0;      // well inside mu + 3.5 sigma
        check(hasCode(validateNormsAgainst(biting, pack, tree), "plausibleInsideCorridor"),
              "a plausible cap inside the Watch edge is refused");

        NormPack roomy;
        roomy.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        roomy.norms[0].plausibleHi = 100.0;
        roomy.norms[0].plausibleLo = -100.0;
        check(validateNormsAgainst(roomy, pack, tree).ok(),
              "…and one outside it is fine");

        // Measured against the WIDEST preset, so a pack cannot be valid for one reader and not
        // another. mu + 3.5 sigma is 26 under `lenient`; 3 sigma is 24 under `standard`.
        NormPack betweenPresets;
        betweenPresets.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        betweenPresets.norms[0].plausibleHi = 25.0;
        check(hasCode(validateNormsAgainst(betweenPresets, pack, tree), "plausibleInsideCorridor"),
              "a cap outside `standard` but inside `lenient` is still refused");

        // The OPEN tail has no Watch edge to cut into, so a cap there is always fine — which is
        // the whole point of plausibility on a floor.
        NormPack floorCap;
        floorCap.norms.push_back(makeNorm("m_smash", "driver", 1.48, 0.05, "ratio"));
        floorCap.norms[0].plausibleHi = 1.56;
        check(validateNormsAgainst(floorCap, pack, tree).ok(),
              "a cap on a floor's OPEN tail is always fine — that is what it is for");
    }

    std::printf("=== norm pack: schema gate ===\n");
    {
        const NormPackLoadResult res =
            loadNormPack(QByteArray(R"({"id":"x","schemaVersion":99,"norms":[]})"));
        check(!res.parsed && hasCode(res.report, "schemaTooNew"),
              "a set from a newer build is refused rather than partially read");

        const NormPackLoadResult bad = loadNormPack(QByteArray("{ not json"));
        check(!bad.parsed && hasCode(bad.report, "badNormFile"),
              "unparseable JSON is reported, not thrown");
    }

    std::printf("=== norm pack: upsert and remove ===\n");
    {
        NormPack p;
        p.norms.push_back(makeNorm("m_a", "full_swing", 0.0, 1.0));
        p.norms.push_back(makeNorm("m_b", "full_swing", 0.0, 1.0));

        p.upsert(makeNorm("m_a", "full_swing", 99.0, 2.0));
        check(p.norms.size() == 2, "upsert of an existing key replaces rather than appends");
        check(near(p.norms[0].mu, 99.0), "…in place, preserving pack order");

        p.upsert(makeNorm("m_a", "driver", 5.0, 1.0));
        check(p.norms.size() == 3, "upsert at a new context appends");
        check(p.contextsFor(QStringLiteral("m_a")) ==
                  QStringList({ QStringLiteral("full_swing"), QStringLiteral("driver") }),
              "contextsFor lists every context the measure has a row for");

        check(p.remove(QStringLiteral("m_a"), QStringLiteral("driver")), "remove finds the row");
        check(!p.remove(QStringLiteral("m_a"), QStringLiteral("driver")), "…and is idempotent");
        check(p.norms.size() == 2, "remove drops exactly one row");
    }

    std::printf("=== norm provider: resolution walks up ===\n");
    {
        NormPack core;
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));
        core.norms.push_back(makeNorm("m_ballPosition", "driver", 10.0, 5.0, "%"));

        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), {});

        const NormResolution driver =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(driver.found() && near(driver.norm->mu, 10.0), "driver resolves its own row");
        check(!driver.inherited && driver.contextId == QLatin1String("driver"),
              "…and is not marked inherited");

        const NormResolution wedge =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("wedge"));
        check(wedge.found() && near(wedge.norm->mu, 50.0), "wedge inherits the full-swing row");
        check(wedge.inherited && wedge.contextId == QLatin1String("full_swing"),
              "…and says WHERE it inherited from, so the UI need not guess");

        const NormResolution none =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("hovercraft"));
        check(!none.found(), "an unknown context resolves to nothing, never to the default");
        check(none.grade(50.0) == Grade::NotMeasured, "…and grades NotMeasured, never a pass");

        const NormResolution defaulted = prov->resolve(QStringLiteral("m_ballPosition"), QString());
        check(defaulted.found() && defaulted.contextId == QLatin1String("full_swing"),
              "an UNDECLARED context falls back to full_swing (the caller marks it inferred)");

        check(!prov->resolve(QStringLiteral("m_missing"), QStringLiteral("driver")).found(),
              "a measure with no norm anywhere resolves to nothing");
    }

    std::printf("=== norm provider: layering and copy-on-write ===\n");
    {
        NormPack core;
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));
        core.norms.push_back(makeNorm("m_ballPosition", "driver", 10.0, 5.0, "%"));

        // The user overrides only the FULL-SWING row.
        NormPack user;
        user.norms.push_back(makeNorm("m_ballPosition", "full_swing", 44.0, 6.0, "%"));

        std::vector<std::unique_ptr<INormProvider>> layers;
        layers.push_back(fake(user, PackOrigin::LocalUser));
        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));

        const NormResolution fs =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
        check(fs.found() && near(fs.norm->mu, 44.0), "a LocalUser row replaces the core row at the same key");

        // The rule that matters: the user said something about full swings, and nothing about
        // drivers. Core's driver row is the more specific statement and must survive.
        const NormResolution drv =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(drv.found() && near(drv.norm->mu, 10.0),
              "context specificity BEATS layer precedence — the shipped driver row survives");

        // A sibling with no row of its own picks up the user's override by inheritance.
        const NormResolution wdg =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("wedge"));
        check(wdg.found() && near(wdg.norm->mu, 44.0) && wdg.inherited,
              "a context with no row of its own inherits the user's override");

        check(prov->overriddenContextsFor(QStringLiteral("m_ballPosition")) ==
                  QStringList({ QStringLiteral("full_swing"), QStringLiteral("driver") }),
              "overriddenContextsFor lists rows in TREE order, for an indented list");
    }

    std::printf("=== norm provider: shipped vs yours ===\n");
    {
        // What "reset to shipped" and the "edited" markers both rest on. Neither can be derived by
        // comparing numbers: a user row holding exactly the shipped values is still a user row.
        //
        // Tree: any -> full_swing -> { driver, wedge }.
        NormPack core;
        core.id = QStringLiteral("core");
        core.norms.push_back(makeNorm("m_ballPosition", "any",        60.0, 20.0, "%"));
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));
        // A user row holding EXACTLY the shipped numbers. Still the user's.
        core.norms.push_back(makeNorm("m_stanceWidth",  "full_swing", 102.0, 12.0, "%"));

        NormPack user;
        user.id = QStringLiteral("user");
        user.norms.push_back(makeNorm("m_ballPosition", "full_swing", 44.0,  6.0, "%"));
        // A user row at a key core does NOT carry — dropping this one means "inherit from the
        // parent", not "go back to what shipped", and a button has to promise the right one.
        user.norms.push_back(makeNorm("m_ballPosition", "wedge",      70.0,  8.0, "%"));
        user.norms.push_back(makeNorm("m_stanceWidth",  "full_swing", 102.0, 12.0, "%"));

        std::vector<std::unique_ptr<INormProvider>> layers;
        layers.push_back(fake(user, PackOrigin::LocalUser));
        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));

        // isOverridden is about ONE KEY: does a user layer supply a row exactly here?
        check(prov->isOverridden(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing")),
              "a key a user layer supplied reports as overridden");
        check(!prov->isOverridden(QStringLiteral("m_ballPosition"), QStringLiteral("any")),
              "a key only core supplies does not");
        check(!prov->isOverridden(QStringLiteral("m_ballPosition"), QStringLiteral("driver")),
              "a key NOBODY supplies does not — driver has no row of its own");
        check(prov->isOverridden(QStringLiteral("m_stanceWidth"), QStringLiteral("full_swing")),
              "a user row with the SHIPPED numbers is still overridden — tracked, not compared");

        const Norm *shippedFs = prov->shippedNorm(QStringLiteral("m_ballPosition"),
                                                  QStringLiteral("full_swing"));
        check(shippedFs && near(shippedFs->mu, 50.0),
              "shippedNorm reports what CORE says, ignoring the override on top of it");
        check(prov->shippedNorm(QStringLiteral("m_ballPosition"), QStringLiteral("wedge")) == nullptr,
              "a key core does not carry has NO shipped row — the reset there means inherit");
        check(prov->shippedNorm(QStringLiteral("m_ballPosition"), QStringLiteral("driver")) == nullptr,
              "shippedNorm is about ONE key and does not walk the tree");

        // resolve().overridden is about the RESOLUTION: is the row that won the user's? The two
        // differ exactly where inheritance does, and that difference is the point — a driver with
        // no row of its own is still graded by the user's corridor, and must say so.
        const NormResolution fs =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
        check(fs.found() && fs.overridden && near(fs.norm->mu, 44.0),
              "resolve() marks an overridden row");

        const NormResolution drv =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("driver"));
        check(drv.found() && drv.inherited && drv.overridden && near(drv.norm->mu, 44.0),
              "INHERITING the user's override is still being graded by the user's number");

        const NormResolution any =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("any"));
        check(any.found() && !any.overridden && near(any.norm->mu, 60.0),
              "a context resolving ABOVE the override is untouched by it");
    }

    std::printf("=== norm provider: a layer can be switched OFF ===\n");
    {
        // Ledger C2. Until makeMergedNormProvider() could skip a layer, the norm-set strip was a
        // census with nothing to switch. The behaviour that has to hold is that a disabled layer is
        // ABSENT, not present-and-ignored: the shipped corridor is what grades, and the layer does
        // not appear in layers() either, because a set you cannot see is a set you cannot turn on.
        NormPack core;
        core.id = QStringLiteral("core");
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));

        NormPack user;
        user.id = QStringLiteral("user");
        user.norms.push_back(makeNorm("m_ballPosition", "full_swing", 44.0, 6.0, "%"));

        {
            std::vector<std::unique_ptr<INormProvider>> layers;
            layers.push_back(fake(user, PackOrigin::LocalUser));
            const auto on = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));
            const NormResolution r =
                on->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
            check(r.found() && near(r.norm->mu, 44.0), "with both layers on, the user row wins");
            check(on->layers().size() == 2, "…and both layers are reported");
        }
        {
            std::vector<std::unique_ptr<INormProvider>> layers;
            layers.push_back(fake(user, PackOrigin::LocalUser));
            const auto off = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers),
                                                    QStringList{ QStringLiteral("user") });
            const NormResolution r =
                off->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
            check(r.found() && near(r.norm->mu, 50.0),
                  "switching the user set OFF grades against the SHIPPED corridor");
            check(off->layers().size() == 1, "…and the disabled layer is absent from the census");
        }
        {
            // The core layer is switchable like any other, and an assembly with nothing left
            // resolves NOTHING rather than pretending it has an answer.
            std::vector<std::unique_ptr<INormProvider>> layers;
            layers.push_back(fake(user, PackOrigin::LocalUser));
            const auto none = makeMergedNormProvider(
                fake(core, PackOrigin::Core), std::move(layers),
                QStringList{ QStringLiteral("core"), QStringLiteral("user") });
            check(!none->resolve(QStringLiteral("m_ballPosition"),
                                 QStringLiteral("full_swing")).found(),
                  "every layer off resolves nothing, honestly");
            check(none->layers().empty(), "…and reports no layers");
        }
    }

    std::printf("=== norm provider: community never wins over core ===\n");
    {
        NormPack core;
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));

        NormPack community;
        community.norms.push_back(makeNorm("m_ballPosition", "full_swing", 1.0, 1.0, "%"));

        std::vector<std::unique_ptr<INormProvider>> layers;
        layers.push_back(fake(community, PackOrigin::Community));
        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));

        const NormResolution r =
            prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
        check(r.found() && near(r.norm->mu, 50.0), "the shipped norm wins over a community set");
        check(hasCode(prov->report(), "duplicateNorm"),
              "…and the loser is reported, so the pack author can see it happened");
    }

    std::printf("=== norm provider: revert restores the shipped norm ===\n");
    {
        NormPack core;
        core.norms.push_back(makeNorm("m_ballPosition", "full_swing", 50.0, 10.0, "%"));

        NormPack user;
        user.upsert(makeNorm("m_ballPosition", "full_swing", 44.0, 6.0, "%"));

        {
            std::vector<std::unique_ptr<INormProvider>> layers;
            layers.push_back(fake(user, PackOrigin::LocalUser));
            const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));
            check(near(prov->resolve(QStringLiteral("m_ballPosition"),
                                     QStringLiteral("full_swing")).norm->mu, 44.0),
                  "the override is live");
        }

        // Reverting is removing the row from the user layer — the shipped pack was never touched.
        user.remove(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
        {
            std::vector<std::unique_ptr<INormProvider>> layers;
            layers.push_back(fake(user, PackOrigin::LocalUser));
            const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));
            const NormResolution r =
                prov->resolve(QStringLiteral("m_ballPosition"), QStringLiteral("full_swing"));
            check(r.found() && near(r.norm->mu, 50.0), "reverting restores the shipped norm exactly");
        }
    }

    std::printf("=== norm provider: the SHIPPED norm set ===\n");
    {
        // Reached via PINPOINT_CORE_NORMS / PINPOINT_CORE_CONTEXTS — the Qt resource exists only
        // inside the app binary.
        const auto prov = makeResourceNormProvider();
        check(prov->report().ok(), "the shipped norm set and context tree load clean");
        check(prov->contexts().contains(kDefaultContextId()),
              "the shipped tree carries the default context");
        check(prov->norms().readOnly, "the shipped norm set is marked read-only");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
