// Standalone test for the live analysisDetail → engine adapter (Wrist diagnostics).
// Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake --build build/analyzer-tests -j4
//   ctest --test-dir build/analyzer-tests -R wrist_analysis_adapter_test --output-on-failure
//
// Feeds a synthetic shotReplay.analysisDetail QVariantMap (the wrist DOF series + a phase ladder)
// through parseAnalysisDetail() + WristAssessmentEngine::assess(), and asserts real per-checkpoint
// values/RAG, that a checkpoint whose phase is absent greys out, and that an un-supplied DOF row is
// not present.

#include "../reference_bands.h"
#include "../wrist_analysis_adapter.h"
#include "../wrist_angle_sampler.h"
#include "../wrist_assessment_engine.h"

#include <QVariantList>
#include <QVariantMap>

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
    const bool ok = std::fabs(got - want) <= 1e-6;
    std::printf("  [%s] %-36s got %8.3f want %8.3f\n", ok ? "PASS" : "FAIL", label, got, want);
    if (!ok) ++g_fail;
}

// Checkpoint timestamps + the Phase int each maps to (must match wristCheckpoints()).
static const qint64 P_T[8]  = { 1000000, 1100000, 1200000, 1300000, 1400000, 1500000, 1600000, 1700000 };
static const int    P_PH[8] = { 0, 1, 8, 2, 4, 9, 5, 11 };

// A metric series with `a[c]` at each checkpoint timestamp (±10 ms cloud so the median is exact).
static QVariantMap makeSeries(const char *key, const double a[8])
{
    QVariantList tUs, val;
    for (int c = 0; c < 8; ++c)
        for (int k = -2; k <= 2; ++k) { tUs.append(P_T[c] + k * 5000); val.append(a[c]); }
    QVariantMap m;
    m[QStringLiteral("key")]   = QString::fromLatin1(key);
    m[QStringLiteral("t_us")]  = tUs;
    m[QStringLiteral("value")] = val;
    return m;
}

static PpRag rag(const PpWristAssessmentResult &r, PpJointDof d, PpSwingPosition p)
{
    return r.row(d).cells[static_cast<int>(p)].rag;
}

int main()
{
    std::printf("=== Wrist analysisDetail adapter ===\n");

    const double radUln[8]  = { 0, 10, 28, 38, 38, 30, 8, 0 };
    const double flexExt[8] = { 0, 1, 4, 9, 11, 12, 9, 4 };
    const double forearm[8] = { 0, -8, -16, -20, -12, -5, 1, 3 };

    QVariantList series;
    series.append(makeSeries("leadWristRadUln", radUln));
    series.append(makeSeries("leadWristFlexExt", flexExt));
    series.append(makeSeries("forearmPronation", forearm));
    // NB: leadArmFlexion (lead elbow) intentionally omitted.

    // Phase ladder — omit Delivery (P6) to exercise a missing checkpoint.
    QVariantList phases;
    for (int c = 0; c < 8; ++c) {
        if (c == 5) continue;             // skip Delivery → P6
        QVariantMap pm;
        pm[QStringLiteral("phase")] = P_PH[c];
        pm[QStringLiteral("t_us")]  = P_T[c];
        pm[QStringLiteral("conf")]  = 0.9;
        phases.append(pm);
    }

    QVariantMap analysisDetail;
    analysisDetail[QStringLiteral("series")] = series;
    analysisDetail[QStringLiteral("phases")] = phases;

    const InMemoryWristAngleSource src = parseAnalysisDetail(analysisDetail);
    NormBandProvider provider;
    const PpWristAssessmentResult r = WristAssessmentEngine::assess(src, provider);

    // 1. Real values flow through to the cells.
    std::printf("-- 1. real values + RAG --\n");
    check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P1) == PpRag::Ref, "radUln P1 → Ref");
    checkNear("radUln P4 delta", r.row(PpJointDof::LeadWristRadUln).cells[3].deltaDeg, 38.0);
    check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P3) == PpRag::Green, "radUln P3 → Green");
    check(rag(r, PpJointDof::LeadForearmRot, PpSwingPosition::P4) == PpRag::Green, "forearm P4 → Green");

    // 2. A checkpoint whose phase is absent → Grey (no fabricated value).
    std::printf("-- 2. missing phase → Grey --\n");
    check(rag(r, PpJointDof::LeadWristRadUln, PpSwingPosition::P6) == PpRag::Grey,
          "radUln P6 (Delivery absent) → Grey");

    // 3. DOF rows: supplied series present, omitted DOF absent.
    std::printf("-- 3. row presence --\n");
    check(r.row(PpJointDof::LeadWristRadUln).present, "radUln row present");
    check(r.row(PpJointDof::LeadForearmRot).present, "forearm row present");
    check(!r.row(PpJointDof::LeadElbowFlex).present, "lead elbow row absent (no series)");
    check(!r.row(PpJointDof::TrailWristFlexExt).present, "trail wrist row absent (no producer)");

    // 4. Gimbal proxy is now wired and the sampler.gimbalThresholdDeg knob is OBSERVABLE (A.1 #1 /
    //    finding B4): the proxy = |RUD| (the FE/RUD middle axis). At P4 RUD = 38°, so the FE cell is
    //    Ok at the default 75° threshold but flips to Indeterminate once the threshold drops below 38.
    std::printf("-- 4. gimbal proxy wired + knob observable --\n");
    {
        using P = PpSwingPosition;
        const PpJointDof FE = PpJointDof::LeadWristFlexExt;
        PpWristSamplingConfig def;                       // gimbalThresholdDeg = 75
        const PpWristAngleSet s75 = WristAngleSampler::sample(src, def);
        check(s75.cell(FE, P::P4).status != PpCellStatus::Indeterminate,
              "RUD 38 < 75 ⇒ FE P4 not Indeterminate");
        PpWristSamplingConfig low; low.gimbalThresholdDeg = 20.0;
        const PpWristAngleSet s20 = WristAngleSampler::sample(src, low);
        check(s20.cell(FE, P::P4).status == PpCellStatus::Indeterminate,
              "RUD 38 ≥ 20 ⇒ FE P4 Indeterminate (knob now observable)");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
