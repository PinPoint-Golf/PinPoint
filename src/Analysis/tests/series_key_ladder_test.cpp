// SPDX-License-Identifier: GPL-3.0-or-later
// ---------------------------------------------------------------------------
// series_key_ladder_test — best instrument first, and the same anatomy either way.
// ---------------------------------------------------------------------------
//
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests --target series_key_ladder_test
//   ctest --test-dir build/analyzer-tests -R series_key_ladder --output-on-failure
//
// Two header-only functions, nine lines between them, and everything a wG3 swing
// shows a golfer depends on both:
//
//   findSeriesByLadder (swing_analysis.h)     "hm.<key>" if present, else "<key>"
//   dofForMetricKey    (wrist_assessment_types.h)  strip "hm." before the DOF join
//
// ⚠ THEY ARE PINNED BECAUSE THEIR FAILURE MODE IS SILENCE, NOT A WRONG ANSWER.
// Four files had each grown their own copy of a bare `m.key == key` loop. On a
// HackMotion swing the series arrive as `hm.leadWristFlexExt`, so all four found
// nothing: no wrist score, no resemblance, no uncertainty interval, no trace — not
// because the wrist was unmeasured but because it was measured by the BETTER
// instrument. Nothing errors in that state. The panels are simply empty, which
// reads as "this swing had no wrist data" and is the opposite of the truth.
//
// The same shape one layer down: a DOF is an anatomical axis, and which instrument
// measured it is not part of its identity. A grid that went blank because the
// series was named `hm.leadWristFlexExt` would be reporting a NAMING decision as
// missing anatomy.

#include "../swing_analysis.h"             // findSeriesByLadder, MetricSeries
#include "../wrist_assessment_types.h"     // dofForMetricKey, PpJointDof

#include <cstdio>
#include <vector>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

namespace {

MetricSeries series(const char *key, double v)
{
    MetricSeries m;
    m.key = QString::fromLatin1(key);
    m.value.push_back(v);
    return m;
}

double firstValue(const MetricSeries *m) { return m && !m->value.empty() ? m->value.front() : -1.0; }

// --- findSeriesByLadder ------------------------------------------------------

void test_the_measured_rung_wins_when_both_are_present()
{
    // The case that only exists in principle — one instrument per swing means both
    // keys are never emitted together today. Pinned anyway, because the whole
    // function is the statement of WHICH would win, and a future dual capture must
    // not discover the answer by experiment.
    const std::vector<MetricSeries> v = { series("leadWristFlexExt", 1.0),
                                          series("hm.leadWristFlexExt", 2.0) };
    check(firstValue(findSeriesByLadder(v, QStringLiteral("leadWristFlexExt"))) == 2.0,
          "hm. rung beats the bare key");
}

void test_order_in_the_vector_does_not_decide_it()
{
    // ⚠ THE ONE THAT CATCHES A REWRITE INTO A SINGLE LOOP. A single pass that takes
    // the first key matching EITHER spelling gives the right answer here and the
    // wrong one when the vector is built in the other order. Two passes, preferred
    // first, is the contract — so the same query is asked of both orderings.
    const std::vector<MetricSeries> hmFirst = { series("hm.leadWristRadUln", 2.0),
                                                series("leadWristRadUln", 1.0) };
    const std::vector<MetricSeries> hmLast  = { series("leadWristRadUln", 1.0),
                                                series("hm.leadWristRadUln", 2.0) };
    check(firstValue(findSeriesByLadder(hmFirst, QStringLiteral("leadWristRadUln"))) == 2.0,
          "hm. rung wins when it is first");
    check(firstValue(findSeriesByLadder(hmLast, QStringLiteral("leadWristRadUln"))) == 2.0,
          "hm. rung wins when it is last");
}

void test_a_bare_key_always_means_our_own_estimate()
{
    // On a wG3 capture the bare keys are not emitted at all, so a bare key found
    // here IS our estimate and is the correct answer — not a fallback that should
    // have been something better.
    const std::vector<MetricSeries> v = { series("leadWristFlexExt", 1.0) };
    check(firstValue(findSeriesByLadder(v, QStringLiteral("leadWristFlexExt"))) == 1.0,
          "the bare key is found when it is the only rung");
}

void test_a_wg3_swing_resolves_through_the_anatomical_name()
{
    // The real shape of a HackMotion swing: hm.* only. Callers ask for the
    // ANATOMICAL key — they never spell "hm." — and this is what makes the four
    // former copies' silence impossible.
    const std::vector<MetricSeries> v = { series("hm.leadWristFlexExt", 2.0),
                                          series("hm.leadWristRadUln", 3.0),
                                          series("hm.forearmRotation", 4.0) };
    check(firstValue(findSeriesByLadder(v, QStringLiteral("leadWristFlexExt"))) == 2.0,
          "bow/cup resolves on a wG3 swing");
    check(firstValue(findSeriesByLadder(v, QStringLiteral("leadWristRadUln"))) == 3.0,
          "hinge resolves on a wG3 swing");
    check(firstValue(findSeriesByLadder(v, QStringLiteral("forearmRotation"))) == 4.0,
          "forearm rotation resolves on a wG3 swing");
}

void test_a_missing_key_is_null_not_the_wrong_series()
{
    const std::vector<MetricSeries> v = { series("hm.leadWristFlexExt", 2.0) };
    check(findSeriesByLadder(v, QStringLiteral("forearmPronation")) == nullptr,
          "an unrelated key returns null");
    check(findSeriesByLadder({}, QStringLiteral("leadWristFlexExt")) == nullptr,
          "an empty vector returns null");
}

void test_the_prefix_is_not_matched_loosely()
{
    // Asking for "hm.leadWristFlexExt" would look for "hm.hm.leadWristFlexExt"
    // first and then find the literal key. Callers should not do this, but the
    // behaviour is stated rather than left to be discovered — a substring or
    // startsWith match here would collide `leadWristFlexExt` with a future
    // `leadWristFlexExtRate`.
    const std::vector<MetricSeries> v = { series("hm.leadWristFlexExt", 2.0),
                                          series("leadWristFlexExtRate", 9.0) };
    check(firstValue(findSeriesByLadder(v, QStringLiteral("hm.leadWristFlexExt"))) == 2.0,
          "an already-prefixed query still finds its literal key");
    check(findSeriesByLadder(v, QStringLiteral("leadWristFlexE")) == nullptr,
          "a partial key matches nothing — the compare is exact");
}

// --- dofForMetricKey ---------------------------------------------------------

bool dofIs(const char *key, PpJointDof want)
{
    const auto d = dofForMetricKey(QString::fromLatin1(key));
    return d.has_value() && *d == want;
}

void test_the_instrument_prefix_is_stripped_before_the_dof_join()
{
    // The line that keeps the whole DOF x P-position assessment grid working on a
    // HackMotion swing. Without it every hm.* series joins to nothing and the grid
    // renders empty — reporting a naming decision as missing anatomy.
    check(dofIs("hm.leadWristFlexExt", PpJointDof::LeadWristFlexExt), "hm. bow/cup -> DOF");
    check(dofIs("hm.leadWristRadUln", PpJointDof::LeadWristRadUln), "hm. hinge -> DOF");
    check(dofIs("leadWristFlexExt", PpJointDof::LeadWristFlexExt), "bare bow/cup -> the SAME DOF");
    check(dofIs("leadWristRadUln", PpJointDof::LeadWristRadUln), "bare hinge -> the SAME DOF");
}

void test_both_producer_spellings_still_reach_their_dof()
{
    // The mockup spellings predate the metric_extractor ones and both are live.
    check(dofIs("forearmPronation", PpJointDof::LeadForearmRot), "forearmPronation -> roll");
    check(dofIs("leadForearmRot", PpJointDof::LeadForearmRot), "leadForearmRot -> roll");
    check(dofIs("leadArmFlexion", PpJointDof::LeadElbowFlex), "leadArmFlexion -> elbow");
    check(dofIs("leadElbowFlex", PpJointDof::LeadElbowFlex), "leadElbowFlex -> elbow");
    check(dofIs("trailWristExt", PpJointDof::TrailWristFlexExt), "trailWristExt -> trail wrist");
    check(dofIs("trailWristFlexExt", PpJointDof::TrailWristFlexExt), "trailWristFlexExt -> trail wrist");
}

void test_forearm_rotation_deliberately_maps_to_no_dof()
{
    // ⚠ AN ASSERTION THAT SOMETHING DOES NOT WORK, AND IT IS LOAD-BEARING.
    // The grid's LeadForearmRot row is the ISB radioulnar angle, fed by
    // `forearmPronation`. `forearmRotation` is a different quantity that happens to
    // concern the same bone. Routing it there would put two measurements in one
    // cell on a three-sensor rig — last writer wins, silently — and would let a wG3
    // fill a row for an angle it cannot measure at all. An empty row is the honest
    // answer, and a future reader "fixing" this gap has to delete this test first.
    check(!dofForMetricKey(QStringLiteral("forearmRotation")).has_value(),
          "forearmRotation maps to NO dof");
    check(!dofForMetricKey(QStringLiteral("hm.forearmRotation")).has_value(),
          "and its measured rung maps to no dof either");
}

void test_an_unknown_key_is_nullopt()
{
    check(!dofForMetricKey(QStringLiteral("clubheadSpeed")).has_value(), "unrelated key -> nullopt");
    check(!dofForMetricKey(QString()).has_value(), "empty key -> nullopt");
    // Only the exact "hm." prefix is stripped, and only once.
    check(!dofForMetricKey(QStringLiteral("hm.hm.leadWristFlexExt")).has_value(),
          "a doubled prefix is not unwound twice");
    check(!dofForMetricKey(QStringLiteral("HM.leadWristFlexExt")).has_value(),
          "the prefix is case-sensitive");
}

void test_the_two_functions_agree_on_the_same_swing()
{
    // The join the assessment grid actually performs, end to end: find the series
    // by its anatomical name, then map the key it FOUND back to a DOF. Both halves
    // have to strip the prefix or the round trip breaks in the middle, which is
    // exactly how a wG3 swing lost its grid.
    const std::vector<MetricSeries> v = { series("hm.leadWristFlexExt", 2.0) };
    const MetricSeries *m = findSeriesByLadder(v, QStringLiteral("leadWristFlexExt"));
    check(m != nullptr, "wG3 series located by anatomical name");
    if (!m) return;
    const auto dof = dofForMetricKey(m->key);
    check(dof.has_value() && *dof == PpJointDof::LeadWristFlexExt,
          "and the key it returned still maps to the anatomical DOF");
}

} // namespace

int main()
{
    std::printf("=== series key ladder: best instrument first, same anatomy ===\n");

    test_the_measured_rung_wins_when_both_are_present();
    test_order_in_the_vector_does_not_decide_it();
    test_a_bare_key_always_means_our_own_estimate();
    test_a_wg3_swing_resolves_through_the_anatomical_name();
    test_a_missing_key_is_null_not_the_wrong_series();
    test_the_prefix_is_not_matched_loosely();

    test_the_instrument_prefix_is_stripped_before_the_dof_join();
    test_both_producer_spellings_still_reach_their_dof();
    test_forearm_rotation_deliberately_maps_to_no_dof();
    test_an_unknown_key_is_nullopt();
    test_the_two_functions_agree_on_the_same_swing();

    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
