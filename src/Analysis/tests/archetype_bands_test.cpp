// Standalone test for ArchetypeBandProvider — style-aware face-DOF bands (design §6).
// Run: ctest --test-dir build/analyzer-tests -R archetype_bands_test --output-on-failure

#include "../reference_bands.h"
#include "../wrist_assessment_engine.h"
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
static void checkNear(const char *label, double got, double want)
{
    const bool ok = std::fabs(got - want) <= 1e-9;
    std::printf("  [%s] %-36s got %8.3f want %8.3f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}
static bool has(const std::vector<PpWristFinding> &v, const char *id)
{
    for (const PpWristFinding &f : v)
        if (f.id == QLatin1String(id)) return true;
    return false;
}

int main()
{
    std::printf("=== ArchetypeBandProvider ===\n");
    ArchetypeBandProvider arch;
    ConfigReferenceBandProvider config;

    // 1. Neutral archetype ≡ the config bands.
    {
        std::printf("-- 1. neutral ≡ config --\n");
        const Band a = arch.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{ .archetype = 0 });
        const Band c = config.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4);
        checkNear("neutral face greenLo", a.greenLo, c.greenLo);
        checkNear("neutral face greenHi", a.greenHi, c.greenHi);
    }

    // 2. Bowed/cupped shift the face corridor ±10°; non-face DOFs are unchanged.
    {
        std::printf("-- 2. archetype shifts the face corridor --\n");
        const Band c     = config.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4);
        const Band bowed = arch.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{ .archetype = 1 });
        const Band cupped= arch.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{ .archetype = 2 });
        checkNear("bowed face greenLo +10",  bowed.greenLo,  c.greenLo + 10.0);
        checkNear("bowed face greenHi +10",  bowed.greenHi,  c.greenHi + 10.0);
        checkNear("cupped face greenLo −10", cupped.greenLo, c.greenLo - 10.0);

        const Band ru  = config.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4);
        const Band ruA = arch.band(PpJointDof::LeadWristRadUln, PpSwingPosition::P4, BandContext{ .archetype = 1 });
        checkNear("radial-ulnar unchanged by archetype", ruA.greenLo, ru.greenLo);
    }

    // 3. Value test — a bowed-at-top swing is red-flagged by the neutral model but fits the bowed one.
    {
        std::printf("-- 3. bowed style not red-flagged under its own model --\n");
        std::vector<DofTrace> t = cleanTraces();
        DofTrace &fe = detail::traceFor(t, PpJointDof::LeadWristFlexExt);
        fe.anchor[idx(PpSwingPosition::P4)] = 24;        // strongly bowed at the top
        const FixtureWristAngleSource swing = buildFixtureSource(t, PpHandedness::Right);

        WristAssessmentConfig neutral;  neutral.band.archetype = 0;
        WristAssessmentConfig bowed;    bowed.band.archetype   = 1;
        const PpWristAssessmentResult rn = WristAssessmentEngine::assess(swing, arch, neutral);
        const PpWristAssessmentResult rb = WristAssessmentEngine::assess(swing, arch, bowed);

        check(rn.row(PpJointDof::LeadWristFlexExt).cells[idx(PpSwingPosition::P4)].rag == PpRag::Red,
              "neutral model: bowed top → Red");
        check(rb.row(PpJointDof::LeadWristFlexExt).cells[idx(PpSwingPosition::P4)].rag == PpRag::Green,
              "bowed model: bowed top → Green");
        check(has(rn.findings, "closed_face_top"),  "neutral model flags closed face");
        check(!has(rb.findings, "closed_face_top"), "bowed model does not flag it");
    }

    // 4. Auto-detect (archetype = −1) picks the band model from the swing's Top reading.
    {
        std::printf("-- 4. auto-detect from the Top --\n");
        WristAssessmentConfig autoCfg;
        autoCfg.band.archetype = -1;

        std::vector<DofTrace> bw = cleanTraces();
        detail::traceFor(bw, PpJointDof::LeadWristFlexExt).anchor[idx(PpSwingPosition::P4)] = 24;
        const PpWristAssessmentResult rb =
            WristAssessmentEngine::assess(buildFixtureSource(bw, PpHandedness::Right), arch, autoCfg);
        check(rb.archetype == 1, "bowed top → auto-detects Bowed");
        check(!has(rb.findings, "closed_face_top"), "auto-bowed: not flagged closed face");

        std::vector<DofTrace> cp = cleanTraces();
        detail::traceFor(cp, PpJointDof::LeadWristFlexExt).anchor[idx(PpSwingPosition::P4)] = -24;
        const PpWristAssessmentResult rc =
            WristAssessmentEngine::assess(buildFixtureSource(cp, PpHandedness::Right), arch, autoCfg);
        check(rc.archetype == 2, "cupped top → auto-detects Cupped");

        const PpWristAssessmentResult rn = WristAssessmentEngine::assess(makeCleanSwing(), arch, autoCfg);
        check(rn.archetype == 0, "clean top → auto-detects Neutral");

        FixtureWristAngleSource noTop = makeCleanSwing();   // no Top phase → neutral fallback
        PpSwingPositionTimeline tl = noTop.timeline();
        tl.positions[idx(PpSwingPosition::P4)].present = false;
        noTop.setTimeline(tl);
        const PpWristAssessmentResult rnt = WristAssessmentEngine::assess(noTop, arch, autoCfg);
        check(rnt.archetype == 0, "no Top reading → auto Neutral");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
