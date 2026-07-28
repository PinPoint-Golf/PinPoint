// NormBandProvider — the one reference-band provider, projecting the diagnostics norm set into the
// (DOF, position) bands the wrist grid renders.
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests -j4
//   ctest --test-dir build/analyzer-tests -R reference_bands_test --output-on-failure
//
// This file inherited the PERMANENT half of reference_bands_parity_test, which was deleted at stage
// 9 along with the compiled table it compared against. The distinction is the whole reason it was
// rewritten rather than dropped. Assertions that pinned norms.json to the old frozen numbers are
// GONE — they would fail the first time a corridor is legitimately re-seated from a corpus,
// reporting a difference nobody wanted reported — while the rules that must hold whatever the
// numbers are stayed:
//
//   - a DOF/position with no norm anywhere on its chain yields an INVALID band, never a zeroed one
//   - the SwingLab bands.* margin override widens amber and leaves the Ideal band alone
//   - the archetype contexts shift the face corridor and leave every other DOF alone
//   - contextId is the real key; an unknown context yields nothing rather than a silent fallback
//   - ragOf(grade(v)) == classifyDelta(v) on every shipped norm — two user-visible paths judging one
//     number, which is what ragOf() exists to keep from separating
//
// The first sections build their own small norm set, so the PROJECTION is asserted without
// depending on shipped content at all.

#include "../reference_bands.h"

#include <QSet>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static void checkNear(const char *label, double got, double want)
{
    const bool ok = std::fabs(got - want) <= 1e-9;
    std::printf("  [%s] %-46s got %8.3f  want %8.3f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

// ── A norm set of our own, for the projection rules ─────────────────────────
//
// resolve() is deliberately NON-virtual on INormProvider — every provider must resolve identically —
// so a fake supplies content and inherits the tree walk.
namespace {

class FakeNorms final : public INormProvider {
public:
    FakeNorms()
    {
        m_contexts = ContextTree(std::vector<ContextNode>{
            { QStringLiteral("any"),        QStringLiteral("Any"),  QString() },
            { QStringLiteral("full_swing"), QStringLiteral("Full"), QStringLiteral("any") },
            { QStringLiteral("iron"),       QStringLiteral("Iron"), QStringLiteral("full_swing") },
        });

        // P4 carries explicit monitor bounds (the migrated shape); P6 carries none (what the
        // corridor editor authors). Both on one DOF, so one lookup cannot satisfy both rules.
        Norm withMonitor;
        withMonitor.measureId = NormBandProvider::cellMeasureId(PpJointDof::LeadWristFlexExt,
                                                                PpSwingPosition::P4);
        withMonitor.contextId = QStringLiteral("full_swing");
        withMonitor.mu        = 5.0;
        withMonitor.sigmaLo   = 11.0;
        withMonitor.sigmaHi   = 11.0;
        withMonitor.monitorLo = -11.0;      // NOT a fixed multiple of sigma — that is the point
        withMonitor.monitorHi = 21.0;
        withMonitor.unit      = QStringLiteral("°");
        m_norms.upsert(withMonitor);

        Norm zDerived;
        zDerived.measureId = NormBandProvider::cellMeasureId(PpJointDof::LeadWristFlexExt,
                                                             PpSwingPosition::P6);
        zDerived.contextId = QStringLiteral("full_swing");
        zDerived.mu        = 10.0;
        zDerived.sigmaLo   = 4.0;
        zDerived.sigmaHi   = 2.0;           // asymmetric, so a symmetric projection cannot pass
        zDerived.unit      = QStringLiteral("°");
        m_norms.upsert(zDerived);
    }

    const NormPack         &norms() const override    { return m_norms; }
    const ContextTree      &contexts() const override { return m_contexts; }
    const ValidationReport &report() const override   { return m_report; }
    QString                 label() const override    { return QStringLiteral("fake"); }
    PackOrigin              origin() const override   { return PackOrigin::Core; }

private:
    NormPack         m_norms;
    ContextTree      m_contexts;
    ValidationReport m_report;
};

} // namespace

int main()
{
    // ── The projection, over a norm set of known shape ──────────────────────
    std::printf("=== NormBandProvider: Norm -> Band ===\n");
    {
        const NormBandProvider p(std::make_shared<FakeNorms>());

        // Explicit monitor bounds: green is mu ± sigma, amber is the monitor band verbatim. It is
        // deliberately not a multiple of the green half-width, because the migrated corridors were
        // not either — a projection deriving amber from a z policy could not have reproduced them.
        const Band m = p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4);
        check(m.valid, "a norm resolves to a valid band");
        checkNear("green lo = mu - sigmaLo", m.greenLo, -6.0);
        checkNear("green hi = mu + sigmaHi", m.greenHi, 16.0);
        checkNear("amber lo = monitorLo",    m.amberLo, -11.0);
        checkNear("amber hi = monitorHi",    m.amberHi, 21.0);

        // No monitor band: amber comes from the grade policy, PER SIDE, so an asymmetric norm
        // projects asymmetrically.
        const Band z = p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P6);
        check(z.valid, "a norm with no monitor band still bands");
        checkNear("green lo",                  z.greenLo,  6.0);
        checkNear("green hi",                  z.greenHi, 12.0);
        checkNear("amber lo = mu - 3*sigmaLo", z.amberLo, -2.0);
        checkNear("amber hi = mu + 3*sigmaHi", z.amberHi, 16.0);

        // A stricter policy pulls the z-derived edge in and leaves the monitored one alone: the
        // monitor bounds are the norm's own claim, not a function of the policy.
        const NormBandProvider strict(std::make_shared<FakeNorms>(), GradePolicy{ 0.75, 1.5, 2.25 });
        checkNear("strict tightens the z-derived amber",
                  strict.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P6).amberLo, 1.0);
        checkNear("…and leaves an explicit monitor edge alone",
                  strict.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4).amberLo, -11.0);
    }

    // ── Absence is absence ─────────────────────────────────────────────────
    std::printf("=== NormBandProvider: no norm -> no band ===\n");
    {
        const NormBandProvider p(std::make_shared<FakeNorms>());
        check(!p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P1).valid,
              "a position with no norm row is unbanded (not a 0..0 corridor)");
        check(!p.band(PpJointDof::TrailShoulderRotation, PpSwingPosition::P4).valid,
              "a DOF with no norm anywhere is unbanded");
        // No norm source at all — how "the shipped file failed to load" presents.
        const NormBandProvider none(nullptr);
        check(!none.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4).valid,
              "no norm source at all yields no bands, never invented ones");
    }

    // ── Inheritance, and the unknown context ───────────────────────────────
    std::printf("=== NormBandProvider: context resolution ===\n");
    {
        const NormBandProvider p(std::make_shared<FakeNorms>());

        BandContext iron;
        iron.contextId = QStringLiteral("iron");
        const Band inherited = p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, iron);
        const Band own = p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{});
        check(inherited.valid && inherited.greenLo == own.greenLo && inherited.greenHi == own.greenHi,
              "a context with no row of its own inherits its ancestor's corridor");

        BandContext bogus;
        bogus.contextId = QStringLiteral("hovercraft");
        check(!p.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, bogus).valid,
              "an unknown context yields no band, never a silent fallback to the default");
    }

    // ── The classifier, on a clean band ────────────────────────────────────
    //
    // Untouched by the migration, and asserted here because everything above feeds it.
    std::printf("=== classifyDelta ===\n");
    {
        Band invalid;                                   // valid == false
        check(classifyDelta(0.0, invalid) == PpRag::Grey, "invalid band → Grey");
        Band inverted{ 20.0, 10.0, 5.0, 25.0, true };    // greenLo > greenHi
        check(classifyDelta(15.0, inverted) == PpRag::Grey, "inverted green range → Grey");

        const Band b{ 10.0, 20.0, 5.0, 25.0, true };
        check(classifyDelta(15.0, b) == PpRag::Green, "centre → Green");
        check(classifyDelta(10.0, b) == PpRag::Green, "green lower edge → Green");
        check(classifyDelta(20.0, b) == PpRag::Green, "green upper edge → Green");
        check(classifyDelta(7.0,  b) == PpRag::Amber, "in amber margin → Amber");
        check(classifyDelta(25.0, b) == PpRag::Amber, "amber upper edge → Amber");
        check(classifyDelta(4.0,  b) == PpRag::Red,   "below amber → Red");
        check(classifyDelta(30.0, b) == PpRag::Red,   "above amber → Red");
    }

    // ── The shipped set actually bands ─────────────────────────────────────
    //
    // NormBandProvider reads content from a file at runtime where the table it replaced was compiled
    // in. If that read fails every band vanishes and the whole wrist grid greys — a failure that
    // looks like "no data" rather than like a bug, so it is asserted rather than assumed.
    std::printf("=== the shipped norm set bands the instrumented DOFs ===\n");
    {
        const NormBandProvider shipped;              // the shared, cached shipped set
        const PpJointDof kInstrumented[] = {
            PpJointDof::LeadWristFlexExt, PpJointDof::LeadWristRadUln, PpJointDof::LeadForearmRot,
            PpJointDof::LeadElbowFlex,    PpJointDof::TrailWristFlexExt,
        };

        int banded = 0, cells = 0;
        for (PpJointDof dof : kInstrumented)
            for (int i = 0; i < kNumPos; ++i) {
                ++cells;
                if (shipped.band(dof, static_cast<PpSwingPosition>(i)).valid) ++banded;
            }
        std::printf("      %d of %d instrumented cells carry a corridor\n", banded, cells);
        check(banded >= 39, "the shipped norm set loaded and bands every migrated cell");
        check(!shipped.band(PpJointDof::TrailWristFlexExt, PpSwingPosition::P8).valid,
              "trail wrist at P8 has no corridor — an absence the content states, not a gap");
        check(!shipped.band(PpJointDof::LeadShoulderRotation, PpSwingPosition::P4).valid,
              "an un-instrumented DOF has no corridor");
    }

    // ── The bands.* margin override still bites ────────────────────────────
    std::printf("=== the SwingLab bands.* margin override ===\n");
    {
        const NormBandProvider shipped;
        BandContext base;
        BandContext wide;
        wide.tuning.flexExtMargin = 20.0;

        const Band b0 = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, base);
        const Band b1 = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, wide);
        check(b1.valid && b1.amberLo < b0.amberLo && b1.amberHi > b0.amberHi,
              "a margin override widens the amber band");
        check(b1.greenLo == b0.greenLo && b1.greenHi == b0.greenHi,
              "…and leaves the Ideal band alone");
        checkNear("amber lo = green lo - margin", b1.amberLo, b0.greenLo - 20.0);
        check(shipped.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, wide).amberLo
                  == shipped.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, base).amberLo,
              "the override is per DOF — another DOF is untouched");
    }

    // ── The archetype shift is content, not code ───────────────────────────
    //
    // The MAGNITUDE is deliberately not asserted. It is a flat ±10° migrated from a compiled
    // constant and is expected to be re-seated per position from a corpus (ledger C1d); pinning it
    // here would rebuild the gate stage 9 removed. What must hold is the mechanism: the face DOF
    // moves with the archetype, the two archetypes move it opposite ways, and nothing else moves.
    std::printf("=== the archetype contexts shift the face corridor ===\n");
    {
        const NormBandProvider shipped;
        BandContext neutral, bowed, cupped;
        bowed.archetype  = 1;
        cupped.archetype = 2;

        const Band n = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, neutral);
        const Band b = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, bowed);
        const Band c = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, cupped);
        check(n.valid && b.valid && c.valid, "all three archetypes band the face DOF");
        check(b.greenLo > n.greenLo && b.greenHi > n.greenHi,
              "bowed shifts the face corridor toward more bow");
        check(c.greenLo < n.greenLo && c.greenHi < n.greenHi,
              "cupped shifts it the other way");
        checkNear("the two shifts are equal and opposite", (b.greenLo - n.greenLo),
                  -(c.greenLo - n.greenLo));

        const Band rn = shipped.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, neutral);
        const Band rb = shipped.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, bowed);
        check(rn.greenLo == rb.greenLo && rn.greenHi == rb.greenHi,
              "a non-face DOF inherits full_swing and is archetype-invariant");

        // The legacy archetype int and the context id are the same key.
        BandContext byId;
        byId.contextId = QStringLiteral("archetype_bowed");
        const Band bi = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, byId);
        check(bi.valid && bi.greenLo == b.greenLo && bi.greenHi == b.greenHi,
              "archetype 1 and contextId archetype_bowed resolve the same band");

        // A club context has no wrist rows of its own and must inherit rather than grey.
        BandContext driver;
        driver.contextId = QStringLiteral("driver");
        const Band d = shipped.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, driver);
        check(d.valid && d.greenLo == n.greenLo && d.greenHi == n.greenHi,
              "driver inherits the general corridor — nothing overrides the wrist DOFs per club");
    }

    // ── The grade and the RAG cannot drift ─────────────────────────────────
    //
    // Two paths judge the same number and both are user-visible: the wrist grid runs classifyDelta()
    // over a Band projected from a Norm, while the characteristic engine runs grade() over the Norm
    // itself. ragOf() is the single collapse between them, and this is what stops the two answers
    // separating — a value that reads amber in the grid while grading Ideal in a finding is a
    // contradiction nothing else would catch.
    //
    // The Band is built here from bandEdgesOf() rather than read back off NormBandProvider, so the
    // test states the specification rather than restating the implementation it is checking.
    std::printf("=== the grade and the RAG agree, over the whole shipped norm set ===\n");
    {
        const std::shared_ptr<const INormProvider> norms = sharedNormProvider();
        const GradePolicy                          policy;   // the shipped default

        // ── The DOMAIN of the claim, stated rather than assumed ────────────
        //
        // Every measure the wrist grid can ask for, built from the same DOF -> measure mapping the
        // provider itself uses. Until the seed conversion the sweep ran over ALL 149 rows and got
        // away with it, because every row was expressible as a Band. That is no longer true, and
        // an over-claiming parity test fails for a reason that is not a drift.
        QSet<QString> gridMeasures;
        for (int d = 0; d < kNumDof; ++d)
            for (int p = 0; p < kNumPos; ++p)
                gridMeasures.insert(NormBandProvider::cellMeasureId(static_cast<PpJointDof>(d),
                                                                    static_cast<PpSwingPosition>(p)));

        int     rows = 0, samples = 0, mismatch = 0, withMonitor = 0, withoutMonitor = 0;
        int     skipped = 0, skippedInGrid = 0;
        QString firstBad;

        for (const Norm &n : norms->norms().norms) {
            // A Band has four numbers and no way to say "not believed": a row carrying a
            // plausibility bound has a state the type cannot hold, so grade() answers Grey where
            // classifyDelta() answers Amber and neither is wrong. Skipped, and COUNTED, because a
            // silent skip is how a parity sweep quietly stops sweeping.
            //
            // The wrist grid is the only consumer of Band, so the skip is only safe while nothing
            // it renders is skipped — which is asserted below rather than assumed. That assertion
            // is also what would catch a wrist DOF acquiring a shape or a cap, which is the guard
            // the plan's non-goals ask NormBandProvider for, in a place that already runs.
            if (n.plausibleLo.has_value() || n.plausibleHi.has_value()) {
                ++skipped;
                if (gridMeasures.contains(n.measureId)) ++skippedInGrid;
                continue;
            }

            ++rows;
            n.hasExplicitMonitor() ? ++withMonitor : ++withoutMonitor;

            const NormBandEdges e = bandEdgesOf(n, policy);
            Band b;
            b.valid   = true;
            b.greenLo = e.idealLo;
            b.greenHi = e.idealHi;
            b.amberLo = e.watchLo;
            b.amberHi = e.watchHi;

            // Sweep the corridor and well past both edges, landing exactly ON every boundary —
            // that is where a one-sided comparison (>= versus >) diverges and nowhere else.
            std::vector<double> values;
            const double span = std::max({ n.sigmaLo, n.sigmaHi, 1.0 }) * 6.0;
            for (double v = n.mu - span; v <= n.mu + span; v += span / 200.0) values.push_back(v);
            for (double edge : { b.greenLo, b.greenHi, b.amberLo, b.amberHi })
                for (double eps : { -1e-9, 0.0, 1e-9, -0.001, 0.001 }) values.push_back(edge + eps);

            for (double v : values) {
                ++samples;
                const PpRag viaGrade = ragOf(grade(v, n, policy));
                const PpRag viaBand  = classifyDelta(v, b);
                if (viaGrade != viaBand) {
                    ++mismatch;
                    if (firstBad.isEmpty())
                        firstBad = QStringLiteral("%1 @ %2, value=%3: grade->%4 vs band->%5")
                                       .arg(n.measureId, n.contextId).arg(v)
                                       .arg(QLatin1String(ragName(viaGrade)))
                                       .arg(QLatin1String(ragName(viaBand)));
                }
            }
        }

        std::printf("      %d norm rows (%d with explicit monitor, %d z-derived), %d samples, "
                    "%d skipped as inexpressible as a Band\n",
                    rows, withMonitor, withoutMonitor, samples, skipped);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));

        check(rows > 0, "the shipped norm set loaded");
        check(mismatch == 0,
              "ragOf(grade(v)) equals classifyDelta(v) on every shipped norm a Band can express");
        // BOTH directions. The skip must be exercised — otherwise this branch is untested and the
        // seed conversion's caps are not actually reaching the pack — and it must not touch a
        // single cell the wrist grid renders, which is the whole reason skipping is safe.
        check(skipped > 0, "…and the exclusion is real: the shipped set does carry capped rows");
        check(skippedInGrid == 0,
              "…none of which is a cell the wrist grid renders — the grid stays entirely "
              "expressible as a Band, and this is what fails if a wrist DOF ever gains a cap or a "
              "shape");
        // BOTH branches of the precedence rule have to be exercised or the assertion is half a
        // gate: the monitor-dominated path is what migrated content uses, the z-derived path is
        // what everything authored in the corridor editor uses.
        check(withMonitor > 0 && withoutMonitor > 0,
              "both the monitor-dominated and the z-derived paths were covered");

        // Grey is reachable only through NotMeasured, and must never be produced by a real value.
        check(ragOf(Grade::NotMeasured) == PpRag::Grey && ragOf(Grade::Ideal) == PpRag::Green
                  && ragOf(Grade::Action) == PpRag::Red,
              "the collapse is Green iff Ideal, Red iff Action, Grey iff not measured");
        check(ragOf(Grade::Good) == PpRag::Amber && ragOf(Grade::Watch) == PpRag::Amber,
              "Good and Watch BOTH fall in the old amber — the mapping is not 2:1:1");
    }

    // ── The factory ────────────────────────────────────────────────────────
    std::printf("=== factory ===\n");
    {
        auto fp = makeReferenceBandProvider();
        check(fp != nullptr, "factory non-null");
        check(fp->band(PpJointDof::LeadWristRadUln, PpSwingPosition::P6).valid,
              "the factory's provider bands the shipped content");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
