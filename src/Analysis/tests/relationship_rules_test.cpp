// Standalone test for the Tier-2 relationship post-pass: cast↔flip linkage, flip corroboration,
// over-rotation / holding-off mutual exclusion, open-face-at-top suppression (design §7.4).
// Run: ctest --test-dir build/analyzer-tests -R relationship_rules_test --output-on-failure

#include "../reference_bands.h"
#include "../wrist_assessment_engine.h"
#include "../wrist_assessment_fixtures.h"

#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}
static const PpWristFinding *find(const std::vector<PpWristFinding> &v, const char *id)
{
    for (const PpWristFinding &f : v)
        if (f.id == QLatin1String(id)) return &f;
    return nullptr;
}
static std::vector<PpWristFinding> assess(const IWristAngleSource &s)
{
    NormBandProvider provider;
    return WristAssessmentEngine::assess(s, provider).findings;
}

int main()
{
    std::printf("=== Tier-2 relationship rules ===\n");

    // 1. Cast ↔ flip linkage (the mockup demo is a cast+flip swing).
    {
        std::printf("-- 1. cast↔flip linkage --\n");
        const auto f = assess(makeMockupDemoSwing());
        const PpWristFinding *cast = find(f, "cast");
        const PpWristFinding *flip = find(f, "flip");
        check(cast && flip, "both cast and flip fire");
        check(cast && cast->linkedTo == QLatin1String("flip"), "cast links to flip");
        check(flip && flip->linkedTo == QLatin1String("cast"), "flip links to cast");
    }

    // 2. Flip corroboration: trail-wrist flattening raises the flip's confidence.
    {
        std::printf("-- 2. flip corroboration --\n");
        const auto withTrail = assess(makeFlipSwing());          // trail flattens P6→P7

        std::vector<DofTrace> t = cleanTraces();                 // flip, but trail held flat
        DofTrace &fe = detail::traceFor(t, PpJointDof::LeadWristFlexExt);
        fe.anchor[idx(PpSwingPosition::P6)] = 2;
        fe.anchor[idx(PpSwingPosition::P7)] = -7;
        fe.anchor[idx(PpSwingPosition::P8)] = -14;
        DofTrace &tw = detail::traceFor(t, PpJointDof::TrailWristFlexExt);
        tw.anchor[idx(PpSwingPosition::P6)] = 12;
        tw.anchor[idx(PpSwingPosition::P7)] = 12;                // no flattening
        const auto noTrail = assess(buildFixtureSource(t, PpHandedness::Right));

        const PpWristFinding *a = find(withTrail, "flip");
        const PpWristFinding *b = find(noTrail, "flip");
        check(a && !a->corroboratedBy.isEmpty(), "trail flattening recorded as corroboration");
        check(b && b->corroboratedBy.isEmpty(),  "no corroboration without trail flattening");
        check(a && b && a->confidence > b->confidence, "corroboration raises flip confidence");
    }

    // 3. Over-rotation and holding-off never co-fire (opposite ends of the forearm axis).
    {
        std::printf("-- 3. mutual exclusion --\n");
        const auto over = assess(makeOverRotationSwing());
        const auto hold = assess(makeHoldingOffSwing());
        check(find(over, "over_rotation") && !find(over, "holding_off"),
              "over-rotation swing → over-rotation only");
        check(find(hold, "holding_off") && !find(hold, "over_rotation"),
              "holding-off swing → holding-off only");
    }

    // 4. Suppression: open-face-at-top with clean downstream → Watch; uncompensated → Fault.
    {
        std::printf("-- 4. open-face suppression --\n");
        const auto supFindings = assess(makeOpenFaceTopSwing());
        const PpWristFinding *suppressed = find(supFindings, "open_face_top");
        check(suppressed && suppressed->severity == PpFindingSeverity::Watch,
              "clean downstream compensation downgrades to Watch");
        check(suppressed && !suppressed->corroboratedBy.isEmpty(), "compensation noted");

        std::vector<DofTrace> t = cleanTraces();                 // open at top AND still open at impact
        DofTrace &fe = detail::traceFor(t, PpJointDof::LeadWristFlexExt);
        fe.anchor[idx(PpSwingPosition::P4)] = -20;
        fe.anchor[idx(PpSwingPosition::P7)] = -10;               // face not recovered by impact
        const auto rawFindings = assess(buildFixtureSource(t, PpHandedness::Right));
        const PpWristFinding *raw = find(rawFindings, "open_face_top");
        check(raw && raw->severity == PpFindingSeverity::Fault, "uncompensated open face stays Fault");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
