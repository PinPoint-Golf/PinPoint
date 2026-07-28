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

// A cohort, terse enough to write a probe table with. `std::nullopt` on either axis is "no answer",
// which is a real state and not a placeholder — see cohortProbeOrder().
static Cohort coh(std::optional<Sex> s = std::nullopt, std::optional<AgeBand> a = std::nullopt)
{
    Cohort c;
    c.sex = s;
    c.age = a;
    return c;
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

        // monitorExcludesIdeal has MOVED to validateNormsAgainst for the same reason
        // partialMonitor did, and the reason is worth stating: it was gated on
        // hasExplicitMonitor(), which without a shape demands BOTH bounds — so on a one-sided row,
        // where one bound is the whole legal monitor band, the check silently did not run at all.
        // It refused nothing and checked nothing on exactly the rows shapes introduced.
        NormPack excl;
        Norm     en  = makeNorm("m_a", "full_swing", 0.0, 10.0);
        en.monitorLo = -2.0;
        en.monitorHi = 2.0;
        excl.norms.push_back(en);
        check(!hasCode(validateNormPack(excl), "monitorExcludesIdeal"),
              "containment is not decidable without the measure's shape either");

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

        // ── monitorExcludesIdeal, likewise now that the shape is in hand ────
        //
        // A monitor band that does not contain its own tolerance means a value sitting inside that
        // tolerance grades Action: it reads as a detection and is a typo. On a one-sided row the
        // check exists on ONE side only, and the side it does not exist on must stay silent — that
        // is the case the standalone validator could not see and therefore never tested at all.
        NormPack exclTarget;
        exclTarget.norms.push_back(makeNorm("m_ballPosition", "driver", 12.0, 4.0, "%"));
        exclTarget.norms[0].monitorLo = 13.0;      // inside mu - sigma (8)
        exclTarget.norms[0].monitorHi = 30.0;
        check(hasCode(validateNormsAgainst(exclTarget, pack, tree), "monitorExcludesIdeal"),
              "a fault edge set inside the tolerance is an error on a target");

        NormPack exclFloor;
        exclFloor.norms.push_back(makeNorm("m_smash", "driver", 1.48, 0.05, "ratio"));
        exclFloor.norms[0].monitorLo = 1.46;       // inside mu - sigma (1.43)
        check(hasCode(validateNormsAgainst(exclFloor, pack, tree), "monitorExcludesIdeal"),
              "…and on a FLOOR's graded side, which the standalone validator could not reach");

        NormPack okFloor;
        okFloor.norms.push_back(makeNorm("m_smash", "driver", 1.48, 0.05, "ratio"));
        okFloor.norms[0].monitorLo = 1.30;         // outside the tolerance, as it should be
        check(!hasCode(validateNormsAgainst(okFloor, pack, tree), "monitorExcludesIdeal"),
              "…while a fault edge outside it is fine, so the check has not become noise");

        // The message names the two EDGES, not two bands. "A monitor band of at least 1.46 does
        // not contain a tolerance of at least 1.48" would read as false on its face.
        QString exclMsg;
        for (const ValidationIssue &i : validateNormsAgainst(exclFloor, pack, tree).issues)
            if (i.code == QLatin1String("monitorExcludesIdeal")) exclMsg = i.message;
        check(exclMsg.contains(QLatin1String("below 1.46")),
              "the message names the fault edge and which way it faces");
        check(exclMsg.contains(QLatin1String("1.43")),
              "…and the tolerance edge it cuts into");
        check(!exclMsg.contains(QLatin1String("above")),
              "…and never names the open tail, which has no fault edge at all");

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

    // ── Cohort: the optional third term of the key ──────────────────────────
    //
    // Every assertion below is paired against an UNQUALIFIED control wherever one exists, because
    // the gate for this stage is that nothing an unqualified set does has changed — and all 149
    // shipped rows are unqualified.

    std::printf("=== cohort: the key ===\n");
    {
        NormPack p;
        Norm men = makeNorm("m_thoraxRotation", "full_swing", 50.0, 8.0);
        men.cohort = coh(Sex::Male);
        Norm seniors = makeNorm("m_thoraxRotation", "full_swing", 42.0, 8.0);
        seniors.cohort = coh(std::nullopt, AgeBand::Adult55_64);

        p.upsert(makeNorm("m_thoraxRotation", "full_swing", 46.0, 8.0));   // unqualified
        p.upsert(men);
        p.upsert(seniors);
        check(p.norms.size() == 3,
              "a qualified row and an unqualified one at the same (measure, context) are two rows");

        check(p.find(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing")) != nullptr
                  && near(p.find(QStringLiteral("m_thoraxRotation"),
                                 QStringLiteral("full_swing"))->mu, 46.0),
              "find with no cohort answers the UNQUALIFIED row, not merely the first one");
        check(near(p.find(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                          coh(Sex::Male))->mu, 50.0),
              "…and find with a cohort answers that cohort's row");
        check(p.find(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                     coh(Sex::Female)) == nullptr,
              "a cohort with no row resolves to nothing here — the probe order is the provider's job");

        // The upsert hazard: replacing by (measure, context) alone would have silently eaten one of
        // the two cohort rows the moment a second one was authored.
        Norm menMoved = men;
        menMoved.mu   = 51.0;
        p.upsert(menMoved);
        check(p.norms.size() == 3 && near(p.find(QStringLiteral("m_thoraxRotation"),
                                                 QStringLiteral("full_swing"),
                                                 coh(Sex::Male))->mu, 51.0),
              "upsert replaces within one cohort and leaves the others standing");

        check(p.contextsFor(QStringLiteral("m_thoraxRotation"))
                  == QStringList({ QStringLiteral("full_swing") }),
              "contextsFor names a context ONCE however many cohort rows sit at it");

        check(p.remove(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                       coh(Sex::Male)),
              "remove takes the cohort's own row");
        check(p.norms.size() == 2
                  && p.contains(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing")),
              "…and leaves the unqualified row alone");
    }

    std::printf("=== cohort: persistence ===\n");
    {
        NormPack in;
        in.id = QStringLiteral("core");
        Norm qualified = makeNorm("m_thoraxRotation", "full_swing", 42.0, 8.0);
        qualified.cohort = coh(Sex::Female, AgeBand::Adult55_64);
        in.norms.push_back(qualified);
        in.norms.push_back(makeNorm("m_stanceWidth", "full_swing", 100.0, 10.0, "%"));

        const QJsonObject root = saveNormPack(in);
        check(root.value(QStringLiteral("schemaVersion")).toInt() == 2,
              "a set carrying a cohort declares schema 2");

        const QJsonArray rows = root.value(QStringLiteral("norms")).toArray();
        check(rows.at(0).toObject().contains(QStringLiteral("cohort")),
              "the qualified row writes its cohort");
        check(!rows.at(1).toObject().contains(QStringLiteral("cohort")),
              "…and the unqualified row writes no cohort key at all, so it round-trips unchanged");

        const NormPackLoadResult res = loadNormPack(root);
        check(res.loaded, "it loads clean");
        const Norm *back = res.pack.find(QStringLiteral("m_thoraxRotation"),
                                         QStringLiteral("full_swing"),
                                         coh(Sex::Female, AgeBand::Adult55_64));
        check(back != nullptr, "the cohort survives the round-trip as part of the key");

        // Content-driven, not a flat bump: a set nobody has qualified stays readable by an older
        // build, and a set that GAINS a cohort row stops being.
        NormPack plain;
        plain.norms.push_back(makeNorm("m_stanceWidth", "full_swing", 100.0, 10.0, "%"));
        check(requiredNormSchemaVersion(plain) == 1
                  && saveNormPack(plain).value(QStringLiteral("schemaVersion")).toInt() == 1,
              "a set with no cohort still declares schema 1");
        check(requiredNormSchemaVersion(in) == 2, "…and one with a cohort declares 2");

        // Single-axis rows: each field is written only when set, so "sex only" and "sex plus band"
        // stay distinguishable in the file.
        NormPack oneAxis;
        Norm sexOnly = makeNorm("m_a", "any", 1.0, 1.0);
        sexOnly.cohort = coh(Sex::Male);
        oneAxis.norms.push_back(sexOnly);
        const QJsonObject co =
            saveNormPack(oneAxis).value(QStringLiteral("norms")).toArray().at(0).toObject()
                .value(QStringLiteral("cohort")).toObject();
        check(co.contains(QStringLiteral("sex")) && !co.contains(QStringLiteral("age")),
              "an unset axis is absent rather than written as a null");
    }

    std::printf("=== cohort: an unknown token drops the row ===\n");
    {
        // The asymmetry with `unknownShape` is the point: falling back there means Target, which
        // grades both tails and is conservative. Falling back here would mean UNQUALIFIED, which
        // would grade EVERYONE against a row meant for one segment.
        const NormPackLoadResult res = loadNormPack(QByteArray(R"({
            "id": "x", "schemaVersion": 2, "norms": [
              { "measure": "m_a", "context": "any", "mu": 1.0, "sigmaLo": 1.0,
                "cohort": { "sex": "mail" } },
              { "measure": "m_b", "context": "any", "mu": 1.0, "sigmaLo": 1.0 }
            ] })"));
        check(hasCode(res.report, "unknownCohort"), "an unreadable sex is a named load error");
        check(!res.loaded, "…and the set does not load clean");
        check(res.pack.norms.size() == 1
                  && res.pack.contains(QStringLiteral("m_b"), QStringLiteral("any")),
              "the row is DROPPED, not silently promoted to matching everyone");

        for (const ValidationIssue &i : res.report.issues) {
            if (i.code != QLatin1String("unknownCohort")) continue;
            check(i.message.contains(QLatin1String("mail"))
                      && i.message.contains(QLatin1String("female")),
                  "…and the message names both the token and the vocabulary it was measured against");
        }

        const NormPackLoadResult ages = loadNormPack(QByteArray(R"({
            "id": "x", "schemaVersion": 2, "norms": [
              { "measure": "m_a", "context": "any", "mu": 1.0, "sigmaLo": 1.0,
                "cohort": { "age": "veteran" } }
            ] })"));
        check(hasCode(ages.report, "unknownCohort") && ages.pack.norms.empty(),
              "the same for an age band outside the closed vocabulary");
    }

    std::printf("=== cohort: the key as one string ===\n");
    {
        check(normKeyLabel(QStringLiteral("m_a"), QStringLiteral("driver"))
                  == QLatin1String("m_a @ driver"),
              "an unqualified key reads exactly as it always did");
        check(normKeyLabel(QStringLiteral("m_a"), QStringLiteral("driver"),
                           coh(Sex::Female, AgeBand::Adult55_64))
                  .startsWith(QLatin1String("m_a @ driver")),
              "a qualified key extends it rather than reshaping it");

        // Every character in the key must be the character it was authored as. The label tables and
        // the separator carry an en dash and a middle dot, and reading either as Latin-1 turns one
        // authored character into two or three mojibake ones — which nothing else here would notice,
        // because the key would still split and still round-trip.
        check(ageBandLabel(AgeBand::Adult55_64).size() == 5
                  && ageBandLabel(AgeBand::Adult55_64).contains(QChar(0x2013)),
              "the age labels decode as UTF-8, en dash included");
        check(normKeyLabel(QStringLiteral("m_a"), QStringLiteral("driver"), coh(Sex::Male))
                  .endsWith(cohortLabel(coh(Sex::Male))),
              "…and the key ends with exactly the cohort label, not a re-encoding of it");

        // The half that had been a guess: the health view splits the subject back apart to build a
        // deep-link, and the spaced spelling used to leave a leading space on the context id.
        QString m, c;
        splitNormKey(normKeyLabel(QStringLiteral("m_a"), QStringLiteral("driver")), m, c);
        check(m == QLatin1String("m_a") && c == QLatin1String("driver"),
              "splitNormKey is the exact inverse — no stray whitespace on the context");

        splitNormKey(normKeyLabel(QStringLiteral("m_a"), QStringLiteral("driver"),
                                  coh(Sex::Male, AgeBand::Adult65Plus)), m, c);
        check(m == QLatin1String("m_a") && c == QLatin1String("driver"),
              "…and the cohort term does not leak into the context id");

        splitNormKey(QStringLiteral("sig_something"), m, c);
        check(m.isEmpty() && c.isEmpty(),
              "a subject that is a plain id yields no measure and no context, rather than half a key");
    }

    std::printf("=== cohort: across the C++/QML boundary ===\n");
    {
        // ONE spelling, the JSON's, so a cohort cannot mean one thing in the file and another in a
        // façade. An unset axis is an ABSENT key, not an empty string — otherwise "no answer" and
        // "the empty answer" would be two spellings of one state.
        const QVariantMap m = cohortToMap(coh(Sex::Female, AgeBand::Adult55_64));
        check(m.value(QStringLiteral("sex")).toString() == QLatin1String("female")
                  && m.value(QStringLiteral("age")).toString() == QLatin1String("adult_55_64"),
              "a cohort maps to the tokens the JSON uses");

        Cohort back;
        check(cohortFromMap(m, back) && back == coh(Sex::Female, AgeBand::Adult55_64),
              "…and round-trips exactly");

        check(cohortToMap(coh()).isEmpty(),
              "the unqualified cohort is an EMPTY map, so an unset axis is an absent key");
        check(cohortFromMap(QVariantMap{}, back) && back.isUnqualified(),
              "…and an empty map reads back as unqualified");

        const QVariantMap sexOnly = cohortToMap(coh(Sex::Male));
        check(sexOnly.size() == 1 && !sexOnly.contains(QStringLiteral("age")),
              "an unset axis is absent rather than written empty");

        // REFUSED, not dropped. Dropping the axis would broaden the key — a caller asking about one
        // segment silently answered about everyone.
        QVariantMap bad;
        bad.insert(QStringLiteral("sex"), QStringLiteral("mail"));
        check(!cohortFromMap(bad, back), "an unreadable token is refused, never dropped to unqualified");
        bad.clear();
        bad.insert(QStringLiteral("age"), QStringLiteral("veteran"));
        check(!cohortFromMap(bad, back), "…on either axis");
    }

    std::printf("=== cohort: the segmented rows at one key ===\n");
    {
        NormPack p;
        p.upsert(makeNorm("m_a", "any", 1.0, 1.0));                       // unqualified
        Norm men = makeNorm("m_a", "any", 2.0, 1.0);
        men.cohort = coh(Sex::Male);
        p.upsert(men);
        Norm seniorWomen = makeNorm("m_a", "any", 3.0, 1.0);
        seniorWomen.cohort = coh(Sex::Female, AgeBand::Adult55_64);
        p.upsert(seniorWomen);
        p.upsert(makeNorm("m_a", "driver", 4.0, 1.0));

        const std::vector<Cohort> qualified = p.cohortsFor(QStringLiteral("m_a"), QStringLiteral("any"));
        check(qualified.size() == 2 && qualified[0] == coh(Sex::Male)
                  && qualified[1] == coh(Sex::Female, AgeBand::Adult55_64),
              "cohortsFor lists the SEGMENTED rows at that key, in pack order");
        check(p.cohortsFor(QStringLiteral("m_a"), QStringLiteral("driver")).empty(),
              "…and says nothing about a context carrying only the universal row");

        // The exclusion is the point: this exists so a surface can list the segmented rows BESIDE
        // the universal one it already shows, not instead of it.
        for (const Cohort &c : qualified)
            check(!c.isUnqualified(), "the unqualified row is never in the list");
    }

    std::printf("=== cohort: the probe order ===\n");
    {
        const auto names = [](const std::vector<Cohort> &v) {
            QStringList out;
            for (const Cohort &c : v)
                out << (c.isUnqualified() ? QStringLiteral("-") : cohortLabel(c));
            return out.join(QStringLiteral(" | "));
        };

        // The full six, most specific first. Age ahead of sex at equal specificity.
        const std::vector<Cohort> full = cohortProbeOrder(coh(Sex::Female, AgeBand::Adult55_64));
        check(full.size() == 6, "a fully-known athlete probes six keys");
        check(full[0] == coh(Sex::Female, AgeBand::Adult55_64)
                  && full[1] == coh(Sex::Female, AgeBand::Adult)
                  && full[2] == coh(std::nullopt, AgeBand::Adult55_64)
                  && full[3] == coh(std::nullopt, AgeBand::Adult)
                  && full[4] == coh(Sex::Female)
                  && full[5] == coh(),
              "…in the fixed order: sex+band, sex+adult, band, adult, sex, unqualified");
        if (full.size() != 6) std::printf("    (order was: %s)\n", qPrintable(names(full)));

        // A junior is not an adult, and `adult` is an 18+ corridor.
        const std::vector<Cohort> junior = cohortProbeOrder(coh(Sex::Male, AgeBand::Junior));
        check(junior.size() == 4, "a junior skips both `adult` probes");
        for (const Cohort &c : junior)
            check(!(c.age.has_value() && *c.age == AgeBand::Adult),
                  "…so no probe of a junior's ever names the adult band");

        // An axis with no answer is skipped, never guessed.
        const std::vector<Cohort> noSex = cohortProbeOrder(coh(std::nullopt, AgeBand::Adult65Plus));
        check(noSex.size() == 3 && noSex[0] == coh(std::nullopt, AgeBand::Adult65Plus)
                  && noSex[1] == coh(std::nullopt, AgeBand::Adult) && noSex[2] == coh(),
              "an athlete who declined to say their sex probes only the age-qualified keys");

        const std::vector<Cohort> noAge = cohortProbeOrder(coh(Sex::Male));
        check(noAge.size() == 2 && noAge[0] == coh(Sex::Male) && noAge[1] == coh(),
              "an athlete with no date of birth probes only the sex-qualified keys");

        // The control, and the reason resolution costs what it always did.
        const std::vector<Cohort> unknown = cohortProbeOrder(coh());
        check(unknown.size() == 1 && unknown[0] == coh(),
              "an athlete we know nothing about probes exactly ONE key — the unqualified one");

        // `adult` is not a band a birthday produces, but a caller can pass it; the collapsed probes
        // must not be repeated.
        const std::vector<Cohort> parent = cohortProbeOrder(coh(Sex::Male, AgeBand::Adult));
        check(parent.size() == 4, "an athlete passed the parent band probes each key once");
    }

    std::printf("=== cohort: the band a birthday puts you in ===\n");
    {
        const QDate dob(1970, 6, 15);

        // The band is read ON A DAY, and the day is the SWING's. An athlete ages across their own
        // history, so the same date of birth answers differently either side of a boundary — which
        // is the whole reason this is derived rather than stored.
        check(ageBandFor(dob, QDate(1988, 6, 14)) == AgeBand::Junior,
              "the day before an 18th birthday is still a junior");
        check(ageBandFor(dob, QDate(1988, 6, 15)) == AgeBand::Adult18_54,
              "…and the birthday itself crosses the boundary");
        check(ageBandFor(dob, QDate(2025, 6, 14)) == AgeBand::Adult18_54,
              "the day before a 55th birthday is still 18–54");
        check(ageBandFor(dob, QDate(2025, 6, 15)) == AgeBand::Adult55_64,
              "…and the birthday itself moves them up a band");
        check(ageBandFor(dob, QDate(2035, 6, 14)) == AgeBand::Adult55_64, "the day before 65");
        check(ageBandFor(dob, QDate(2035, 6, 15)) == AgeBand::Adult65Plus, "…and 65 itself");

        // ONE date of birth, TWO swings, two different rows. This is the assertion the whole
        // derive-at-the-swing-date rule exists for.
        check(ageBandFor(dob, QDate(2025, 6, 14)) != ageBandFor(dob, QDate(2025, 6, 15)),
              "two swings a day apart across a boundary resolve DIFFERENT bands");

        // It never produces the parent band. cohortProbeOrder depends on that: it probes `adult`
        // only for somebody already in one of its sub-bands, so a derived `Adult` would make probes
        // 1 and 2 collapse and quietly change the order.
        for (int y = 1930; y <= 2030; y += 1) {
            const std::optional<AgeBand> b = ageBandFor(QDate(y, 3, 1), QDate(2026, 7, 28));
            if (b.has_value() && *b == AgeBand::Adult)
                check(false, "a birthday never derives the parent band");
        }
        check(true, "no birthday over a century of years derives the parent `adult` band");

        // The three ways it declines to answer, each of which means the universal corridor grades.
        check(!ageBandFor(QDate(), QDate(2026, 7, 28)).has_value(),
              "no date of birth, no band");
        check(!ageBandFor(dob, QDate()).has_value(),
              "no swing date, no band — never silently substituting today");
        check(!ageBandFor(QDate(2030, 1, 1), QDate(2026, 7, 28)).has_value(),
              "born after the swing is nonsense, and resolves nothing rather than a junior");

        // A leap-day birthday falls on 28 February in a non-leap year, which is QDate::addYears'
        // clamping and one of the two readings jurisdictions actually use (the other is 1 March).
        // Pinned so the behaviour is a decision rather than an accident — the disagreement is one
        // day, once every four years, at a band boundary that is itself a round number.
        check(ageBandFor(QDate(2008, 2, 29), QDate(2026, 2, 27)) == AgeBand::Junior,
              "a leap-day birthday has not occurred on 27 February");
        check(ageBandFor(QDate(2008, 2, 29), QDate(2026, 2, 28)) == AgeBand::Adult18_54,
              "…and is taken to occur on the 28th in a non-leap year");
    }

    std::printf("=== cohort: from the two fields an athlete record holds ===\n");
    {
        const QDate dob(1960, 1, 1);
        const QDate on(2026, 7, 28);

        check(cohortFor(dob, QStringLiteral("female"), on) == coh(Sex::Female, AgeBand::Adult65Plus),
              "both fields known gives both axes");

        // Every way of not answering lands on the same place: the axis is unset, and unset RESOLVES
        // — against the corridor for everyone. It is never "not measured".
        check(cohortFor(dob, QString(), on) == coh(std::nullopt, AgeBand::Adult65Plus),
              "an unanswered sex leaves that axis unset and keeps the age");
        check(cohortFor(dob, QStringLiteral("declined"), on)
                  == cohortFor(dob, QString(), on),
              "…and a DECLINED answer resolves identically to an unanswered one");
        check(cohortFor(dob, QStringLiteral("from-a-newer-build"), on)
                  == cohortFor(dob, QString(), on),
              "…as does a token this build does not know: unknown means unknown, not wrong");
        check(cohortFor(QDate(), QStringLiteral("male"), on) == coh(Sex::Male),
              "no date of birth leaves the age unset and keeps the sex");
        check(cohortFor(QDate(), QString(), on).isUnqualified(),
              "neither answered is the unqualified cohort — which matches everyone");

        // And that unqualified cohort is exactly the one probe order that costs what resolution
        // always cost, which is what makes "we know nothing about you" free rather than a penalty.
        check(cohortProbeOrder(cohortFor(QDate(), QString(), on)).size() == 1,
              "…and it probes one key, so knowing nothing costs nothing");
    }

    std::printf("=== cohort: resolution, exhaustively at one node ===\n");
    {
        const Cohort athlete = coh(Sex::Female, AgeBand::Adult55_64);

        // Six rows, one per probe, each with a distinguishing mu. Removing them one at a time from
        // the most specific end must walk the probe order exactly.
        struct Row { Cohort who; double mu; };
        const Row rows[] = {
            { coh(Sex::Female, AgeBand::Adult55_64), 1.0 },
            { coh(Sex::Female, AgeBand::Adult),      2.0 },
            { coh(std::nullopt, AgeBand::Adult55_64), 3.0 },
            { coh(std::nullopt, AgeBand::Adult),      4.0 },
            { coh(Sex::Female),                       5.0 },
            { coh(),                                  6.0 },
        };

        for (size_t drop = 0; drop <= 6; ++drop) {
            NormPack core;
            for (size_t i = drop; i < 6; ++i) {
                Norm n  = makeNorm("m_thoraxRotation", "full_swing", rows[i].mu, 5.0);
                n.cohort = rows[i].who;
                core.upsert(n);
            }
            const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), {});
            const NormResolution r =
                prov->resolve(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                              athlete);
            if (drop == 6)
                check(!r.found(), "with no row at all, nothing resolves");
            else
                check(r.found() && near(r.norm->mu, rows[drop].mu),
                      "the first present probe wins, and only it");
        }

        // A male golfer must never resolve a female row, whatever else is missing.
        {
            NormPack core;
            Norm f  = makeNorm("m_thoraxRotation", "full_swing", 1.0, 5.0);
            f.cohort = coh(Sex::Female, AgeBand::Adult55_64);
            core.upsert(f);
            const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), {});
            check(!prov->resolve(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                                 coh(Sex::Male, AgeBand::Adult55_64)).found(),
                  "a cohort row is never a fallback for a different cohort");
        }

        // A junior against an adult-banded set: both `adult` probes are skipped, so only the
        // unqualified row can answer — and it DOES answer, because an unknown or unmatched cohort
        // degrades to the universal corridor rather than to NotMeasured.
        {
            NormPack core;
            Norm adult  = makeNorm("m_thoraxRotation", "full_swing", 1.0, 5.0);
            adult.cohort = coh(std::nullopt, AgeBand::Adult);
            core.upsert(adult);
            core.upsert(makeNorm("m_thoraxRotation", "full_swing", 9.0, 5.0));
            const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), {});
            const NormResolution r =
                prov->resolve(QStringLiteral("m_thoraxRotation"), QStringLiteral("full_swing"),
                              coh(std::nullopt, AgeBand::Junior));
            check(r.found() && near(r.norm->mu, 9.0),
                  "a junior falls past the adult row to the unqualified one, and still GRADES");
        }
    }

    std::printf("=== cohort: context-major, not cohort-major ===\n");
    {
        // The consequence to hold on to: stance width at the driver is club-mechanical, and a senior
        // corridor at `any` must not displace it. If senior-driver matters, it gets authored.
        NormPack core;
        Norm senior  = makeNorm("m_stanceWidth", "any", 90.0, 8.0, "%");
        senior.cohort = coh(std::nullopt, AgeBand::Adult55_64);
        core.upsert(senior);
        core.upsert(makeNorm("m_stanceWidth", "driver", 105.0, 8.0, "%"));   // unqualified, narrow

        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), {});
        const NormResolution r =
            prov->resolve(QStringLiteral("m_stanceWidth"), QStringLiteral("driver"),
                          coh(std::nullopt, AgeBand::Adult55_64));
        check(r.found() && near(r.norm->mu, 105.0) && r.contextId == QLatin1String("driver"),
              "an unqualified narrow-context row beats a cohort-qualified broad-context one");

        // The other half of the same rule, and the reason it is worth having: where no club row
        // exists, the senior row DOES answer — which is the ROM family, exactly where cohorts matter.
        const NormResolution wedge =
            prov->resolve(QStringLiteral("m_stanceWidth"), QStringLiteral("wedge"),
                          coh(std::nullopt, AgeBand::Adult55_64));
        check(wedge.found() && near(wedge.norm->mu, 90.0) && wedge.inherited,
              "…and the cohort row answers wherever no narrower row exists");
    }

    std::printf("=== cohort: layering and duplicates ===\n");
    {
        // A user row qualified to one cohort overrides the shipped row for THAT cohort, and marks
        // nothing else as edited.
        NormPack core;
        core.upsert(makeNorm("m_a", "full_swing", 10.0, 1.0));
        Norm coreMen  = makeNorm("m_a", "full_swing", 12.0, 1.0);
        coreMen.cohort = coh(Sex::Male);
        core.upsert(coreMen);

        NormPack user;
        Norm mine  = makeNorm("m_a", "full_swing", 13.0, 1.0);
        mine.cohort = coh(Sex::Male);
        user.upsert(mine);

        std::vector<std::unique_ptr<INormProvider>> layers;
        layers.push_back(fake(user, PackOrigin::LocalUser));
        const auto prov = makeMergedNormProvider(fake(core, PackOrigin::Core), std::move(layers));

        check(prov->isOverridden(QStringLiteral("m_a"), QStringLiteral("full_swing"),
                                 coh(Sex::Male)),
              "the cohort row is marked overridden");
        check(!prov->isOverridden(QStringLiteral("m_a"), QStringLiteral("full_swing")),
              "…and the unqualified row beside it is NOT — nobody touched it");
        check(prov->shippedNorm(QStringLiteral("m_a"), QStringLiteral("full_swing"),
                                coh(Sex::Male)) != nullptr
                  && near(prov->shippedNorm(QStringLiteral("m_a"), QStringLiteral("full_swing"),
                                            coh(Sex::Male))->mu, 12.0),
              "…and 'reset to shipped' finds that cohort's shipped row, not the unqualified one");

        // Duplicate detection is on the FULL key.
        NormPack twoCohorts;
        Norm men = makeNorm("m_a", "any", 1.0, 1.0);
        men.cohort = coh(Sex::Male);
        Norm women = makeNorm("m_a", "any", 2.0, 1.0);
        women.cohort = coh(Sex::Female);
        twoCohorts.norms.push_back(men);
        twoCohorts.norms.push_back(women);
        twoCohorts.norms.push_back(makeNorm("m_a", "any", 3.0, 1.0));
        check(!hasCode(validateNormPack(twoCohorts), "duplicateNorm"),
              "three cohorts at one (measure, context) are three rows, not a duplicate");

        twoCohorts.norms.push_back(women);
        check(hasCode(validateNormPack(twoCohorts), "duplicateNorm"),
              "…and two rows on the SAME cohort still collide");
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

        // THE REGRESSION GATE for cohort keying, stated over real content rather than a fixture:
        // every shipped row is unqualified, so every resolution must answer identically for a golfer
        // we know everything about and one we know nothing about. If this ever fails, a cohort has
        // reached shipped content — which is a decision, not an accident, and it should fail here.
        bool     allSame     = true;
        bool     allUnqual   = true;
        for (const Norm &n : prov->norms().norms) {
            if (!n.cohort.isUnqualified()) { allUnqual = false; continue; }
            const NormResolution plain = prov->resolve(n.measureId, n.contextId);
            for (const Cohort &who : { coh(Sex::Female, AgeBand::Adult65Plus),
                                       coh(Sex::Male, AgeBand::Junior),
                                       coh(Sex::Male),
                                       coh(std::nullopt, AgeBand::Adult18_54) }) {
                const NormResolution seg = prov->resolve(n.measureId, n.contextId, who);
                if (seg.norm != plain.norm || seg.contextId != plain.contextId) allSame = false;
            }
        }
        check(allUnqual, "every shipped row is unqualified — this stage adds no content");
        check(allSame, "…so every shipped resolution answers identically for any athlete");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
