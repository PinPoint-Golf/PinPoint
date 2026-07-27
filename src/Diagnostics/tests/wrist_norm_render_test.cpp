// The wrist assessment engine, rendered from the SHIPPED norm set.
//
// This was wrist_render_parity_test, the downstream half of the stage-2 migration gate: it ran the
// engine twice, once over the compiled band table and once over the norms, and asserted every cell
// and every finding was identical. The table was deleted at stage 9, so the comparison has no other
// side and the name would now be a lie.
//
// What it keeps is the half that was never about parity. NormBandProvider reads its content from a
// FILE at runtime where the old table was compiled in, so a failed read does not crash — every band
// silently vanishes, every cell greys, and the whole assessment view degrades into "no data". That
// is the one failure mode a band-level unit test cannot see, because a provider returning nothing is
// exactly what an un-instrumented DOF looks like. So the engine is driven over a spread of synthetic
// swings and the assertions are that it produced REAL bands, reached Green AND Amber AND Red, and
// produced findings — and, as the inverse, that a provider with no norm source greys everything
// (which is what proves the first assertion could fail).
//
//   cmake --build build/analyzer-tests --target wrist_norm_render_test
//   ctest --test-dir build/analyzer-tests -R wrist_norm_render --output-on-failure

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

int main()
{
    const NormBandProvider norms;

    // A spread of swings: flat, moderate, large, and offset ones that push cells out through amber
    // into red on both sides.
    struct Case { double amp; double bias; const char *name; };
    const std::vector<Case> cases = {
        { 0.0,  0.0,  "flat" },      { 10.0,  0.0, "moderate" },
        { 30.0, 0.0,  "large" },     { 50.0,  0.0, "very large" },
        { 10.0, 5.0,  "offset +" },  { 10.0, -5.0, "offset -" },
        { 60.0, 20.0, "extreme +" }, { 60.0, -20.0, "extreme -" },
    };

    std::printf("=== the shipped norms render a real wrist grid ===\n");
    {
        int cells = 0, banded = 0, graded = 0;
        int ragCounts[5] = { 0, 0, 0, 0, 0 };

        for (const Case &c : cases) {
            const SyntheticSource src(c.amp, c.bias);
            for (int arch = 0; arch <= 2; ++arch) {
                WristAssessmentConfig cfg;
                cfg.band.archetype = arch;

                const PpWristAssessmentResult r = WristAssessmentEngine::assess(src, norms, cfg);

                for (int d = 0; d < kNumDof; ++d) {
                    if (!r.rows[d].present) continue;
                    for (int p = 0; p < kNumPos; ++p) {
                        const PpRagCell &cell = r.rows[d].cells[p];
                        ++cells;
                        if (cell.banded) ++banded;
                        if (cell.rag != PpRag::Ref && cell.rag != PpRag::Grey) ++graded;
                        ragCounts[int(cell.rag)]++;
                    }
                }
            }
        }

        std::printf("      %d cells, %d banded, %d graded  (ref %d, green %d, amber %d, red %d, grey %d)\n",
                    cells, banded, graded, ragCounts[int(PpRag::Ref)], ragCounts[int(PpRag::Green)],
                    ragCounts[int(PpRag::Amber)], ragCounts[int(PpRag::Red)],
                    ragCounts[int(PpRag::Grey)]);

        check(banded > 0, "the norm path produced real bands (not a silently empty norm set)");
        check(graded > 0, "cells were actually assessed, not all Ref/Grey");
        check(ragCounts[int(PpRag::Green)] > 0 && ragCounts[int(PpRag::Amber)] > 0
                  && ragCounts[int(PpRag::Red)] > 0,
              "the fixture exercised Green, Amber AND Red");
    }

    // The inverse, which is what makes the assertion above a gate rather than an observation: with no
    // norm source every band is absent, and the engine greys the entire grid. That is precisely what
    // a failed content read looks like at runtime.
    std::printf("=== an empty norm set greys everything ===\n");
    {
        const NormBandProvider empty(nullptr);
        const SyntheticSource  src(30.0, 0.0);

        int banded = 0, assessed = 0;
        const PpWristAssessmentResult r = WristAssessmentEngine::assess(src, empty);
        for (int d = 0; d < kNumDof; ++d) {
            if (!r.rows[d].present) continue;
            for (int p = 0; p < kNumPos; ++p) {
                if (r.rows[d].cells[p].banded) ++banded;
                if (r.rows[d].cells[p].rag == PpRag::Green || r.rows[d].cells[p].rag == PpRag::Amber
                    || r.rows[d].cells[p].rag == PpRag::Red)
                    ++assessed;
            }
        }
        check(banded == 0 && assessed == 0,
              "no norm source ⇒ no banded cell and no colour, so the check above can fail");
    }

    std::printf("=== findings come out of the norm path ===\n");
    {
        int assessments = 0, totalFindings = 0, withFindings = 0;

        for (const Case &c : cases) {
            const SyntheticSource src(c.amp, c.bias);
            for (int arch = 0; arch <= 2; ++arch) {
                WristAssessmentConfig cfg;
                cfg.band.archetype = arch;

                const PpWristAssessmentResult r = WristAssessmentEngine::assess(src, norms, cfg);
                ++assessments;
                totalFindings += int(r.findings.size());
                if (!r.findings.empty()) ++withFindings;

                // Every finding a user reads has to carry the fields the view renders. A finding with
                // no id is a row with no name, which is worse than an absent finding.
                for (const auto &f : r.findings)
                    if (f.id.isEmpty()) {
                        check(false, "a finding carried no id");
                        break;
                    }
            }
        }

        std::printf("      %d assessments, %d findings total, %d assessments with findings\n",
                    assessments, totalFindings, withFindings);
        check(totalFindings > 0, "the rule base fires against norm-derived bands");
        check(withFindings > 0, "at least one synthetic swing produced findings");
    }

    std::printf("%s\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
    return g_fail == 0 ? 0 : 1;
}
