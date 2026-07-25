// The second half of the migration gate: the WristAssessmentEngine's whole output — every cell's
// rag, band and delta, plus every finding's id, severity and confidence — must be unchanged by
// swapping the compiled table for norms.
//
// reference_bands_parity_test proves the BANDS are identical. This proves nothing downstream of
// them moved: the assessment view is what a user actually looks at, and a difference in a finding's
// severity or a suppressed fault would be invisible to a band-level comparison.
//
// It also covers a failure mode the band test cannot: NormBandProvider reads its content from a
// file at runtime where the old provider compiled it in. If that read fails, every band silently
// vanishes and the whole grid greys. The "the norm path produced real bands" assertion is there so
// that shows up as a failure rather than as a suspiciously clean diff.
//
//   cmake --build build/analyzer-tests --target wrist_render_parity_test
//   ctest --test-dir build/analyzer-tests -R wrist_render_parity --output-on-failure

#include "../../Analysis/reference_bands.h"
#include "../../Analysis/wrist_assessment_engine.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int  g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

// ── A synthetic angle source ────────────────────────────────────────────────
// Deterministic curves per DOF, shaped so that different swings land in different bands: the point
// is to sweep the assessment across Green/Amber/Red rather than to be biomechanically plausible.
class SyntheticSource final : public IWristAngleSource {
public:
    SyntheticSource(double amplitude, double bias, PpHandedness hand = PpHandedness::Right)
        : m_hand(hand)
    {
        for (int d = 0; d < kNumDof; ++d) {
            PpJointAngleSeries &s = m_series[d];
            s.dof     = static_cast<PpJointDof>(d);
            // Only the five instrumented DOFs are present; the rest are absent, which is itself
            // part of what must not change.
            s.present = (d == int(PpJointDof::LeadWristFlexExt)
                         || d == int(PpJointDof::LeadWristRadUln)
                         || d == int(PpJointDof::LeadForearmRot)
                         || d == int(PpJointDof::LeadElbowFlex)
                         || d == int(PpJointDof::TrailWristFlexExt));
            s.baseConfidence = 0.9f;
            if (!s.present) continue;

            // One sample per position, walking a smooth arc scaled by the caller's amplitude.
            for (int p = 0; p < kNumPos; ++p) {
                const double t     = double(p) / double(kNumPos - 1);
                const double shape = std::sin(t * 3.14159265358979) * amplitude
                                     + bias * double(d + 1) * 0.5;
                PpJointAngleSample smp;
                smp.t_us       = int64_t(p) * 100000;
                smp.valueDeg   = shape;
                smp.available  = true;
                smp.confidence = 0.9f;
                s.samples.push_back(smp);
            }
        }

        for (int p = 0; p < kNumPos; ++p) {
            m_timeline.positions[p].present = true;
            m_timeline.positions[p].t_us    = int64_t(p) * 100000;
            m_timeline.positions[p].conf    = 1.0f;
        }
    }

    PpHandedness handedness() const override { return m_hand; }
    const PpJointAngleSeries *series(PpJointDof dof) const override
    {
        const int d = static_cast<int>(dof);
        if (d < 0 || d >= kNumDof) return nullptr;
        return &m_series[d];
    }
    PpSwingPositionTimeline timeline() const override { return m_timeline; }

private:
    PpJointAngleSeries      m_series[kNumDof];
    PpSwingPositionTimeline m_timeline;
    PpHandedness            m_hand;
};

static bool sameCell(const PpRagCell &a, const PpRagCell &b)
{
    // Bit-for-bit on the numbers: the deltas come from the same source and the bands are proven
    // identical upstream, so any drift here is a real behaviour change, not rounding.
    return a.rag == b.rag && a.status == b.status && a.banded == b.banded
           && a.valueDeg == b.valueDeg && a.deltaDeg == b.deltaDeg
           && a.bandLo == b.bandLo && a.bandHi == b.bandHi
           && a.confidence == b.confidence;
}

int main()
{
    const ArchetypeBandProvider oldProv;
    const NormBandProvider      newProv;

    // A spread of swings: flat, moderate, large, and offset ones that push cells out through amber
    // into red on both sides.
    struct Case { double amp; double bias; const char *name; };
    const std::vector<Case> cases = {
        { 0.0,  0.0,  "flat" },      { 10.0,  0.0, "moderate" },
        { 30.0, 0.0,  "large" },     { 50.0,  0.0, "very large" },
        { 10.0, 5.0,  "offset +" },  { 10.0, -5.0, "offset -" },
        { 60.0, 20.0, "extreme +" }, { 60.0, -20.0, "extreme -" },
    };

    std::printf("=== wrist render parity: cells ===\n");
    {
        int cells = 0, banded = 0, mismatch = 0, graded = 0;
        int ragCounts[5] = { 0, 0, 0, 0, 0 };
        QString firstBad;

        for (const Case &c : cases) {
            const SyntheticSource src(c.amp, c.bias);
            for (int arch = 0; arch <= 2; ++arch) {
                WristAssessmentConfig cfg;
                cfg.band.archetype = arch;

                const PpWristAssessmentResult a = WristAssessmentEngine::assess(src, oldProv, cfg);
                const PpWristAssessmentResult b = WristAssessmentEngine::assess(src, newProv, cfg);

                if (a.archetype != b.archetype && firstBad.isEmpty())
                    firstBad = QStringLiteral("%1 arch%2: archetype resolved differently")
                                   .arg(QLatin1String(c.name)).arg(arch);

                for (int d = 0; d < kNumDof; ++d) {
                    if (a.rows[d].present != b.rows[d].present) { ++mismatch; continue; }
                    for (int p = 0; p < kNumPos; ++p) {
                        const PpRagCell &ca = a.rows[d].cells[p];
                        const PpRagCell &cb = b.rows[d].cells[p];
                        ++cells;
                        // Counted off the NEW provider on purpose. Counting the old one would make
                        // the "not a silently empty norm set" guard below vacuous — it would report
                        // healthy bands in exactly the scenario it exists to catch.
                        if (cb.banded) ++banded;
                        if (ca.rag != PpRag::Ref && ca.rag != PpRag::Grey) ++graded;
                        ragCounts[int(ca.rag)]++;

                        if (!sameCell(ca, cb)) {
                            ++mismatch;
                            if (firstBad.isEmpty())
                                firstBad = QStringLiteral("%1 arch%2 %3 P%4: rag %5 vs %6, "
                                                          "band %7..%8 vs %9..%10")
                                               .arg(QLatin1String(c.name)).arg(arch)
                                               .arg(QLatin1String(dofName(static_cast<PpJointDof>(d))))
                                               .arg(p + 1)
                                               .arg(QLatin1String(ragName(ca.rag)))
                                               .arg(QLatin1String(ragName(cb.rag)))
                                               .arg(ca.bandLo).arg(ca.bandHi)
                                               .arg(cb.bandLo).arg(cb.bandHi);
                        }
                    }
                }
            }
        }

        std::printf("      %d cells, %d banded, %d graded  (ref %d, green %d, amber %d, red %d, grey %d)\n",
                    cells, banded, graded, ragCounts[int(PpRag::Ref)], ragCounts[int(PpRag::Green)],
                    ragCounts[int(PpRag::Amber)], ragCounts[int(PpRag::Red)],
                    ragCounts[int(PpRag::Grey)]);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));

        check(mismatch == 0, "every assessed cell is identical — rag, band, delta and confidence");

        // The guard against a vacuous pass: if the norm file failed to load, every band would be
        // absent and both providers would agree on a grid of grey.
        check(banded > 0, "the norm path produced real bands (not a silently empty norm set)");
        check(ragCounts[int(PpRag::Green)] > 0 && ragCounts[int(PpRag::Amber)] > 0
                  && ragCounts[int(PpRag::Red)] > 0,
              "the fixture actually exercised Green, Amber AND Red");
    }

    std::printf("=== wrist render parity: findings ===\n");
    {
        int compared = 0, mismatch = 0, totalFindings = 0;
        QString firstBad;

        for (const Case &c : cases) {
            const SyntheticSource src(c.amp, c.bias);
            for (int arch = 0; arch <= 2; ++arch) {
                WristAssessmentConfig cfg;
                cfg.band.archetype = arch;

                const PpWristAssessmentResult a = WristAssessmentEngine::assess(src, oldProv, cfg);
                const PpWristAssessmentResult b = WristAssessmentEngine::assess(src, newProv, cfg);
                ++compared;
                totalFindings += int(a.findings.size());

                if (a.findings.size() != b.findings.size()) {
                    ++mismatch;
                    if (firstBad.isEmpty())
                        firstBad = QStringLiteral("%1 arch%2: %3 findings vs %4")
                                       .arg(QLatin1String(c.name)).arg(arch)
                                       .arg(a.findings.size()).arg(b.findings.size());
                    continue;
                }

                for (size_t i = 0; i < a.findings.size(); ++i) {
                    const auto &fa = a.findings[i];
                    const auto &fb = b.findings[i];
                    const bool same = fa.id == fb.id && fa.severity == fb.severity
                                      && fa.confidence == fb.confidence
                                      && fa.lowConfidence == fb.lowConfidence
                                      && fa.magnitudeDeg == fb.magnitudeDeg
                                      && fa.weight == fb.weight
                                      && fa.corroboratedBy == fb.corroboratedBy
                                      && fa.linkedTo == fb.linkedTo;
                    if (!same) {
                        ++mismatch;
                        if (firstBad.isEmpty())
                            firstBad = QStringLiteral("%1 arch%2 finding %3: '%4' vs '%5'")
                                           .arg(QLatin1String(c.name)).arg(arch).arg(i)
                                           .arg(fa.id, fb.id);
                    }
                }
            }
        }

        std::printf("      %d assessments compared, %d findings total\n", compared, totalFindings);
        if (!firstBad.isEmpty())
            std::printf("      first mismatch: %s\n", qPrintable(firstBad));
        check(mismatch == 0, "every finding is identical — id, severity, magnitude, confidence, corroboration");
        check(totalFindings > 0, "the fixture actually produced findings to compare");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
