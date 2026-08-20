// Standalone test for the Wrist Motion assessment engine — Phase 0 windowed-median sampler.
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.1/gcc_64
//   cmake --build build/analyzer-tests -j4
//   ctest --test-dir build/analyzer-tests -R wrist_angle_sampling_test --output-on-failure
//
// Verifies the sampling MACHINERY against deterministic synthetic fixtures: windowed median over
// noisy samples returns the Pn anchor; an outlier is rejected; a gap → unavailable (no fabricated
// value); a missing timeline entry → gap; a near-gimbal position → indeterminate (distinct from
// gap) with the threshold as the single tunable knob; a left-handed swing mirrors to its
// right-handed twin; and the whole thing is deterministic run-to-run.

#include "../wrist_assessment_contract.h"
#include "../wrist_angle_sampler.h"
#include "../wrist_assessment_fixtures.h"

#include <cmath>
#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;

static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static void checkNear(const char *label, double got, double want, double tol)
{
    const bool ok = std::fabs(got - want) <= tol;
    std::printf("  [%s] %-46s got %9.4f  want %9.4f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

// Build a single-position, single-DOF source for the micro-cases (outlier / gimbal).
static PpJointAngleSample mkSample(int64_t t, double v, double pitch = 0.0)
{
    PpJointAngleSample s;
    s.t_us          = t;
    s.valueDeg      = v;
    s.available     = true;
    s.confidence    = 1.0f;
    s.pitchProxyDeg = pitch;
    return s;
}

int main()
{
    std::printf("=== WristAngleSampler (Phase 0) ===\n");

    // 1. Windowed median over noisy samples returns the Pn anchor; gaps are Gap cells.
    {
        std::printf("-- 1. clean fixture: median recovers anchors --\n");
        const FixtureWristAngleSource clean = makeCleanSwing();
        const PpWristAngleSet set = WristAngleSampler::sample(clean);
        char lab[96];
        for (const DofTrace &tr : cleanTraces()) {
            for (int p = 0; p < kNumPos; ++p) {
                const auto pos = static_cast<PpSwingPosition>(p);
                const PpWristAngleCell &c = set.cell(tr.dof, pos);
                if (tr.present[p]) {
                    std::snprintf(lab, sizeof lab, "%s P%d Ok", dofName(tr.dof), p + 1);
                    check(c.status == PpCellStatus::Ok, lab);
                    std::snprintf(lab, sizeof lab, "%s P%d value", dofName(tr.dof), p + 1);
                    checkNear(lab, c.valueDeg, tr.anchor[p], 1e-9);
                } else {
                    std::snprintf(lab, sizeof lab, "%s P%d gap", dofName(tr.dof), p + 1);
                    check(c.status == PpCellStatus::Gap, lab);
                }
            }
        }
        // an Ok cell carries non-zero aggregated confidence.
        check(set.cell(PpJointDof::LeadWristRadUln, PpSwingPosition::P6).confidence > 0.0f,
              "Ok cell has non-zero confidence");
    }

    // 2. Median rejects an injected outlier (the wild sample does not move the cell).
    {
        std::printf("-- 2. outlier rejection --\n");
        FixtureWristAngleSource src;
        PpSwingPositionTimeline tl;
        tl.positions[idx(PpSwingPosition::P4)] = { true, 400000, 1.0f };
        src.setTimeline(tl);

        PpJointAngleSeries ser;
        ser.dof = PpJointDof::LeadWristFlexExt;
        ser.present = true;
        ser.baseConfidence = 1.0f;
        const double a = 10.0;
        ser.samples = { mkSample(390000, a + 0.75), mkSample(395000, a - 1.5),
                        mkSample(400000, a + 0.0),  mkSample(405000, a - 0.75),
                        mkSample(410000, a + 500.0) };   // one wild outlier
        src.setSeries(ser);

        const PpWristAngleSet set = WristAngleSampler::sample(src);
        const PpWristAngleCell &c = set.cell(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4);
        check(c.status == PpCellStatus::Ok, "outlier: cell Ok");
        checkNear("outlier: median rejects wild sample", c.valueDeg, a, 1e-9);
    }

    // 3. Gap at Pn → unavailable, no fabricated value, zero confidence.
    {
        std::printf("-- 3. gap → unavailable --\n");
        const FixtureWristAngleSource clean = makeCleanSwing();
        const PpWristAngleSet set = WristAngleSampler::sample(clean);
        const PpWristAngleCell &c = set.cell(PpJointDof::TrailWristFlexExt, PpSwingPosition::P8);
        check(c.status == PpCellStatus::Gap, "trail wrist P8 is Gap");
        check(!c.available(), "gap cell not available");
        check(c.valueDeg == 0.0 && c.confidence == 0.0f, "gap cell has no value/confidence");
    }

    // 4. Missing timeline entry → every DOF at that Pn is a Gap (the P5-has-no-event reality).
    {
        std::printf("-- 4. missing timeline entry → gap --\n");
        FixtureWristAngleSource s = makeCleanSwing();
        PpSwingPositionTimeline tl = s.timeline();
        tl.positions[idx(PpSwingPosition::P5)].present = false;
        s.setTimeline(tl);
        const PpWristAngleSet set = WristAngleSampler::sample(s);
        bool allGap = true;
        for (const DofTrace &tr : cleanTraces())
            if (set.cell(tr.dof, PpSwingPosition::P5).status != PpCellStatus::Gap)
                allGap = false;
        check(allGap, "all DOFs at P5 are Gap when the position is absent");
    }

    // 5. Near-gimbal → Indeterminate (distinct from Gap); raising the threshold flips it to Ok.
    {
        std::printf("-- 5. near-gimbal → indeterminate, single tunable knob --\n");
        FixtureWristAngleSource src;
        PpSwingPositionTimeline tl;
        tl.positions[idx(PpSwingPosition::P4)] = { true, 400000, 1.0f };
        src.setTimeline(tl);

        PpJointAngleSeries ser;
        ser.dof = PpJointDof::LeadForearmRot;
        ser.present = true;
        ser.baseConfidence = 1.0f;
        const double a = -20.0;
        ser.samples = { mkSample(390000, a + 0.75, 85.0), mkSample(395000, a - 1.5, 85.0),
                        mkSample(400000, a + 0.0,  85.0), mkSample(405000, a - 0.75, 85.0),
                        mkSample(410000, a + 1.5,  85.0) };   // pitch proxy 85° > default 75°
        src.setSeries(ser);

        const PpWristAngleSet set = WristAngleSampler::sample(src);
        const PpWristAngleCell &c = set.cell(PpJointDof::LeadForearmRot, PpSwingPosition::P4);
        check(c.status == PpCellStatus::Indeterminate, "near-gimbal cell is Indeterminate (not Gap)");
        check(!c.available(), "indeterminate cell not available");

        PpWristSamplingConfig cfg;
        cfg.gimbalThresholdDeg = 90.0;   // raise above the proxy
        const PpWristAngleSet set2 = WristAngleSampler::sample(src, cfg);
        const PpWristAngleCell &c2 = set2.cell(PpJointDof::LeadForearmRot, PpSwingPosition::P4);
        check(c2.status == PpCellStatus::Ok, "raising threshold makes the same cell Ok");
        checkNear("threshold-raised cell value", c2.valueDeg, a, 1e-9);
    }

    // 6. Left-handed swing mirrors to its right-handed twin, cell for cell.
    {
        std::printf("-- 6. handedness mirror: LH == RH twin --\n");
        const PpWristAngleSet rh = WristAngleSampler::sample(makeCleanSwing());
        const PpWristAngleSet lh = WristAngleSampler::sample(makeCleanSwingLeftHanded());
        bool match = true;
        char lab[96];
        for (int d = 0; d < kNumDof; ++d) {
            for (int p = 0; p < kNumPos; ++p) {
                const auto dof = static_cast<PpJointDof>(d);
                const auto pos = static_cast<PpSwingPosition>(p);
                const PpWristAngleCell &a = rh.cell(dof, pos);
                const PpWristAngleCell &b = lh.cell(dof, pos);
                if (a.status != b.status) { match = false; continue; }
                if (a.status == PpCellStatus::Ok) {
                    if (std::fabs(a.valueDeg - b.valueDeg) > 1e-9 ||
                        std::fabs(a.confidence - b.confidence) > 1e-6)
                        match = false;
                }
            }
        }
        check(match, "left-handed set is identical to the right-handed twin");
        // spot-check a sign-flipped DOF explicitly (radial-ulnar mirrors).
        std::snprintf(lab, sizeof lab, "radUln P3 RH==LH after mirror");
        checkNear(lab,
                  lh.cell(PpJointDof::LeadWristRadUln, PpSwingPosition::P3).valueDeg,
                  rh.cell(PpJointDof::LeadWristRadUln, PpSwingPosition::P3).valueDeg, 1e-9);
    }

    // 7. Determinism: sampling the same fixture twice yields identical cells.
    {
        std::printf("-- 7. determinism --\n");
        const PpWristAngleSet a = WristAngleSampler::sample(makeCleanSwing());
        const PpWristAngleSet b = WristAngleSampler::sample(makeCleanSwing());
        bool identical = true;
        for (int d = 0; d < kNumDof; ++d)
            for (int p = 0; p < kNumPos; ++p) {
                const auto dof = static_cast<PpJointDof>(d);
                const auto pos = static_cast<PpSwingPosition>(p);
                const PpWristAngleCell &ca = a.cell(dof, pos);
                const PpWristAngleCell &cb = b.cell(dof, pos);
                if (ca.status != cb.status || ca.valueDeg != cb.valueDeg ||
                    ca.confidence != cb.confidence)
                    identical = false;
            }
        check(identical, "two runs produce byte-identical cells");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
