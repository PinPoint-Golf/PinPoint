// THE MIGRATION GATE. Asserts that NormBandProvider — bands projected from the diagnostics norm
// set — reproduces ArchetypeBandProvider (the compiled table in reference_bands.cpp) EXACTLY.
//
// This test is what licenses deleting that table. It is not a smoke test and it is not
// approximate: every (DOF, position, archetype) cell must agree on all four band edges to the bit,
// and the resulting PpRag must agree across a swept delta range that deliberately lands ON the
// band edges, where an off-by-an-epsilon conversion would show up as a flipped colour.
//
// If this fails, the CONVERSION is wrong — not the target. The old numbers are the specification.
//
// ⚠ THIS IS A MIGRATION GATE, NOT A PERMANENT ONE. It pins norms.json to a frozen compiled table,
// so the moment anyone legitimately re-seats a wrist corridor from a corpus it will fail — correctly
// reporting a difference nobody wants reported. Delete it together with ConfigReferenceBandProvider,
// ArchetypeBandProvider and the table itself at stage 9. Until then the table is unreachable from
// the app (makeReferenceBandProvider defaults to Norm) and exists only to be compared against.
//
//   cmake --build build/analyzer-tests --target reference_bands_parity_test
//   ctest --test-dir build/analyzer-tests -R reference_bands_parity --output-on-failure

#include "../../Analysis/reference_bands.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// The five DOFs the table covers. Everything else has no band in either provider.
static const PpJointDof kDofs[] = {
    PpJointDof::LeadWristFlexExt, PpJointDof::LeadWristRadUln, PpJointDof::LeadForearmRot,
    PpJointDof::LeadElbowFlex,    PpJointDof::TrailWristFlexExt,
};

// A DOF with no producer and no band, to prove both providers agree on ABSENCE too — a provider
// that invented a band where the old one had none would be just as wrong as one that got a number
// wrong, and far harder to notice.
static const PpJointDof kUnbandedDofs[] = {
    PpJointDof::TrailWristRadUln, PpJointDof::LeadShoulderRotation, PpJointDof::TrailElbowFlex,
};

int main()
{
    const ArchetypeBandProvider oldProv;
    const NormBandProvider      newProv;

    // ── The band edges, cell by cell ────────────────────────────────────────
    std::printf("=== parity: band edges over every (DOF, position, archetype) ===\n");
    {
        int cells = 0, banded = 0, mismatch = 0, absentAgree = 0;
        QString firstBad;

        for (int arch = 0; arch <= 2; ++arch) {
            BandContext ctx;
            ctx.archetype = arch;

            for (PpJointDof dof : kDofs) {
                for (int p = 0; p < kNumPos; ++p) {
                    const auto pos = static_cast<PpSwingPosition>(p);
                    const Band a = oldProv.band(dof, pos, ctx);
                    const Band b = newProv.band(dof, pos, ctx);
                    ++cells;

                    if (a.valid != b.valid) {
                        ++mismatch;
                        if (firstBad.isEmpty())
                            firstBad = QStringLiteral("%1 P%2 arch%3: validity %4 vs %5")
                                           .arg(QLatin1String(dofName(dof))).arg(p + 1).arg(arch)
                                           .arg(a.valid).arg(b.valid);
                        continue;
                    }
                    if (!a.valid) { ++absentAgree; continue; }
                    ++banded;

                    // Bit-for-bit. Every mu/sigma in the migrated set is a half-integer, so the
                    // projection mu±sigma recovers the original endpoints exactly; a tolerance
                    // here would hide precisely the drift this test exists to catch.
                    const bool same = (a.greenLo == b.greenLo) && (a.greenHi == b.greenHi)
                                      && (a.amberLo == b.amberLo) && (a.amberHi == b.amberHi);
                    if (!same) {
                        ++mismatch;
                        if (firstBad.isEmpty())
                            firstBad = QStringLiteral("%1 P%2 arch%3: green %4..%5 vs %6..%7, "
                                                      "amber %8..%9 vs %10..%11")
                                           .arg(QLatin1String(dofName(dof))).arg(p + 1).arg(arch)
                                           .arg(a.greenLo).arg(a.greenHi)
                                           .arg(b.greenLo).arg(b.greenHi)
                                           .arg(a.amberLo).arg(a.amberHi)
                                           .arg(b.amberLo).arg(b.amberHi);
                    }
                }
            }
        }

        std::printf("      %d cells, %d banded, %d absent-and-agreed\n", cells, banded, absentAgree);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));

        check(mismatch == 0, "every band edge is bit-identical across all three archetypes");
        // 5 DOFs x 8 positions x 3 archetypes = 120, less trail wrist P8 x 3 = 117.
        check(banded == 117, "the expected 117 banded cells resolved (39 rows x 3 archetypes)");
        check(absentAgree == 3, "trail wrist P8 has no band in either provider, in all 3 archetypes");
    }

    // ── Absence, for DOFs the table never covered ───────────────────────────
    std::printf("=== parity: DOFs with no band stay unbanded ===\n");
    {
        bool ok = true;
        for (int arch = 0; arch <= 2; ++arch) {
            BandContext ctx;
            ctx.archetype = arch;
            for (PpJointDof dof : kUnbandedDofs)
                for (int p = 0; p < kNumPos; ++p) {
                    const auto pos = static_cast<PpSwingPosition>(p);
                    ok = ok && !oldProv.band(dof, pos, ctx).valid
                            && !newProv.band(dof, pos, ctx).valid;
                }
        }
        check(ok, "a DOF the table never covered is unbanded in both providers");
    }

    // ── The classification, swept ──────────────────────────────────────────
    std::printf("=== parity: PpRag over a swept delta range ===\n");
    {
        int    samples = 0, mismatch = 0, cells = 0;
        QString firstBad;

        for (int arch = 0; arch <= 2; ++arch) {
            BandContext ctx;
            ctx.archetype = arch;

            for (PpJointDof dof : kDofs) {
                for (int p = 0; p < kNumPos; ++p) {
                    const auto pos = static_cast<PpSwingPosition>(p);
                    const Band a = oldProv.band(dof, pos, ctx);
                    const Band b = newProv.band(dof, pos, ctx);
                    if (!a.valid) continue;
                    ++cells;

                    // The interesting deltas are the EDGES and their immediate neighbours: a
                    // uniform sweep would very likely step straight over a one-sided boundary
                    // error. Sweep both.
                    std::vector<double> deltas;
                    for (double d = -80.0; d <= 80.0; d += 0.25) deltas.push_back(d);
                    for (double e : { a.greenLo, a.greenHi, a.amberLo, a.amberHi })
                        for (double eps : { -1e-9, 0.0, 1e-9, -0.001, 0.001 })
                            deltas.push_back(e + eps);

                    for (double d : deltas) {
                        ++samples;
                        const PpRag ra = classifyDelta(d, a);
                        const PpRag rb = classifyDelta(d, b);
                        if (ra != rb) {
                            ++mismatch;
                            if (firstBad.isEmpty())
                                firstBad = QStringLiteral("%1 P%2 arch%3 delta=%4: %5 vs %6")
                                               .arg(QLatin1String(dofName(dof))).arg(p + 1)
                                               .arg(arch).arg(d)
                                               .arg(QLatin1String(ragName(ra)))
                                               .arg(QLatin1String(ragName(rb)));
                        }
                    }
                }
            }
        }

        // 641 uniform steps (-80..80 by 0.25) + 4 band edges x 5 epsilon offsets = 661 per cell.
        // Asserted exactly rather than as "enough": a silently truncated sweep would let this test
        // pass while covering a fraction of the range, which is the one way a parity gate fails
        // without failing.
        const int kPerCell = 641 + 4 * 5;
        std::printf("      %d classified samples over %d cells\n", samples, cells);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));
        check(mismatch == 0, "every classified delta yields an identical PpRag");
        check(cells == 117 && samples == 117 * kPerCell,
              "every banded cell got the full sweep, edges included");
    }

    // ── The SwingLab margin override still bites ───────────────────────────
    std::printf("=== parity: bands.* margin override ===\n");
    {
        // tuning_overrides_test pins that the override widens the amber band. It has to keep
        // working through the norm path, or a sweep would silently do nothing.
        BandContext base;
        BandContext wide;
        wide.tuning.flexExtMargin = 20.0;

        const Band b0 = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, base);
        const Band b1 = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, wide);
        check(b1.valid && b1.amberLo < b0.amberLo && b1.amberHi > b0.amberHi,
              "a margin override widens the amber band through the norm path");
        check(b1.greenLo == b0.greenLo && b1.greenHi == b0.greenHi,
              "…and leaves the green corridor alone");

        const Band o0 = oldProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, wide);
        check(o0.amberLo == b1.amberLo && o0.amberHi == b1.amberHi,
              "the overridden band matches the old provider exactly");
    }

    // ── The archetype shift is now content, not code ────────────────────────
    std::printf("=== parity: archetype shift comes from norm rows ===\n");
    {
        BandContext neutral, bowed, cupped;
        bowed.archetype  = 1;
        cupped.archetype = 2;

        const Band n = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, neutral);
        const Band b = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, bowed);
        const Band c = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, cupped);

        check(b.greenLo == n.greenLo + 10.0 && b.greenHi == n.greenHi + 10.0,
              "bowed shifts the face corridor +10°, from a norm row rather than a compiled offset");
        check(c.greenLo == n.greenLo - 10.0 && c.greenHi == n.greenHi - 10.0,
              "cupped shifts it -10°");

        // Other DOFs are archetype-invariant: they have no archetype rows and inherit full_swing.
        const Band rn = newProv.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, neutral);
        const Band rb = newProv.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, bowed);
        check(rn.greenLo == rb.greenLo && rn.greenHi == rb.greenHi,
              "a non-face DOF inherits full_swing and is archetype-invariant");
    }

    // ── contextId supersedes the legacy archetype int ───────────────────────
    std::printf("=== parity: contextId is the real key ===\n");
    {
        BandContext byInt;
        byInt.archetype = 1;
        BandContext byId;
        byId.contextId = QStringLiteral("archetype_bowed");

        const Band a = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, byInt);
        const Band b = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, byId);
        check(a.valid && b.valid && a.greenLo == b.greenLo && a.greenHi == b.greenHi,
              "archetype 1 and contextId archetype_bowed resolve the same band");

        // A club context inherits full swing, because nothing overrides the wrist DOFs per club.
        BandContext driver;
        driver.contextId = QStringLiteral("driver");
        const Band d = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, driver);
        const Band f = newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{});
        check(d.valid && d.greenLo == f.greenLo && d.greenHi == f.greenHi,
              "driver inherits the full-swing corridor — nothing overrides the wrist DOFs per club");

        // An unrecognised context resolves to NOTHING, so the cell greys rather than being graded
        // against a corridor that was never meant for it.
        BandContext bogus;
        bogus.contextId = QStringLiteral("hovercraft");
        check(!newProv.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, bogus).valid,
              "an unknown context yields no band, never a silent fallback");
    }

    // ── The grade and the RAG cannot drift ──────────────────────────────────
    //
    // Two paths judge the same number and both are user-visible: the wrist grid runs
    // classifyDelta() over a Band projected from a Norm, while the characteristic engine runs
    // grade() over the Norm itself. ragOf() is the single collapse between them, and this is what
    // stops the two answers separating — a value that greys amber in the grid while grading Ideal
    // in a finding is a contradiction nothing else would catch.
    //
    // The Band is projected here from the norm's own fields rather than read back off
    // NormBandProvider, so the test states the specification independently of the implementation
    // it is checking.
    std::printf("=== the grade and the RAG agree, over the whole shipped norm set ===\n");
    {
        const std::shared_ptr<const INormProvider> norms = sharedNormProvider();
        const GradePolicy                          policy;   // the shipped default

        int rows = 0, samples = 0, mismatch = 0, withMonitor = 0, withoutMonitor = 0;
        QString firstBad;

        for (const Norm &n : norms->norms().norms) {
            ++rows;
            n.hasExplicitMonitor() ? ++withMonitor : ++withoutMonitor;

            Band b;
            b.valid   = true;
            b.greenLo = n.idealLo();
            b.greenHi = n.idealHi();
            if (n.hasExplicitMonitor()) {
                b.amberLo = *n.monitorLo;
                b.amberHi = *n.monitorHi;
            } else {
                b.amberLo = n.mu - policy.watchMaxZ * n.sigmaLo;
                b.amberHi = n.mu + policy.watchMaxZ * n.sigmaHi;
            }

            // Sweep the corridor and well past both edges, landing exactly ON every boundary —
            // that is where a one-sided comparison (>= versus >) diverges and nowhere else.
            std::vector<double> values;
            const double span = std::max({ n.sigmaLo, n.sigmaHi, 1.0 }) * 6.0;
            for (double v = n.mu - span; v <= n.mu + span; v += span / 200.0) values.push_back(v);
            for (double e : { b.greenLo, b.greenHi, b.amberLo, b.amberHi })
                for (double eps : { -1e-9, 0.0, 1e-9, -0.001, 0.001 }) values.push_back(e + eps);

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

        std::printf("      %d norm rows (%d with explicit monitor, %d z-derived), %d samples\n",
                    rows, withMonitor, withoutMonitor, samples);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));

        check(rows > 0, "the shipped norm set loaded");
        check(mismatch == 0, "ragOf(grade(v)) equals classifyDelta(v) on every shipped norm");
        // BOTH branches of the precedence rule have to be exercised or the assertion is half a
        // gate: the monitor-dominated path is what migrated content uses, the z-derived path is
        // what everything authored in the corridor editor will use.
        check(withMonitor > 0 && withoutMonitor > 0,
              "both the monitor-dominated and the z-derived paths were covered");

        // Grey is reachable only through NotMeasured, and must never be produced by a real value.
        check(ragOf(Grade::NotMeasured) == PpRag::Grey && ragOf(Grade::Ideal) == PpRag::Green
                  && ragOf(Grade::Action) == PpRag::Red,
              "the collapse is Green iff Ideal, Red iff Action, Grey iff not measured");
        check(ragOf(Grade::Good) == PpRag::Amber && ragOf(Grade::Watch) == PpRag::Amber,
              "Good and Watch BOTH fall in the old amber — the mapping is not 2:1:1");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
