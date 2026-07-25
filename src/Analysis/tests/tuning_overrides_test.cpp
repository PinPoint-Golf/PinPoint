// Verifies the dotted-key `tuningOverrides` path actually mutates the values a stage
// consumes, and that an empty map is a no-op (byte-identical to defaults). Covers the
// `score.*` namespace (SwingScorer); later phases extend this with sampler.*/rules.*/filter.*.
//
//   cmake -S src/Analysis/tests -B build/analyzer-tests && \
//   cmake --build build/analyzer-tests --target tuning_overrides_test && \
//   ctest --test-dir build/analyzer-tests -R tuning_overrides_test --output-on-failure

#include "../swing_scorer.h"
#include "../wrist_assessment_tuning.h"
#include "../reference_bands.h"
#include "../analysis_tuning.h"
#include "../ball_position.h"
#include "../event_refine.h"
#include "../kinematic_series.h"
#include "../tempo_metrics.h"
#include "../../Core/pp_tuned_constants.h"

#include <QVariantMap>
#include <cstdio>

using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

static MetricSeries metric(const char *key, double impactValue)
{
    MetricSeries m;
    m.key = QString::fromLatin1(key); m.label = m.key; m.unit = QStringLiteral("°");
    m.t_us = { 0, 1 }; m.value = { 0.0, impactValue };
    m.phaseSamples.push_back({ Phase::Impact, 1, impactValue, QString() });
    return m;
}

static const ScoredMetric *sm(const ScoreBreakdown &b, const char *key)
{
    for (const ScoredMetric &m : b.metrics)
        if (m.key == QString::fromLatin1(key)) return &m;
    return nullptr;
}

int main()
{
    std::printf("=== tuningOverrides (score.*) ===\n");

    // 1. Empty map == default path (the production no-op).
    {
        const std::vector<MetricSeries> s = { metric("leadWristFlexExt", 15.0) };
        const ScoreBreakdown def = SwingScorer::score(s, 1);
        const ScoreBreakdown ovr = SwingScorer::score(s, 1, QVariantMap{});
        check(def.overall == ovr.overall && sm(def, "leadWristFlexExt")->subScore
                                                == sm(ovr, "leadWristFlexExt")->subScore,
              "empty map == default (no-op)");
    }

    // 2. score.<key>.mu shifts the band centre → an on-centre value becomes penalised.
    {
        const std::vector<MetricSeries> s = { metric("leadWristFlexExt", 15.0) };  // default mu 15 → 100
        const ScoreBreakdown def = SwingScorer::score(s, 1);
        check(sm(def, "leadWristFlexExt")->subScore == 100.0, "value at default mu → 100");

        QVariantMap ov; ov["score.leadWristFlexExt.mu"] = 40.0;   // move centre far above
        const ScoreBreakdown b = SwingScorer::score(s, 1, ov);
        check(sm(b, "leadWristFlexExt")->mu == 40.0, "override mu reaches ScoredMetric.mu");
        check(sm(b, "leadWristFlexExt")->subScore < 100.0,
              "value now below shifted mu (one-sided) → penalised");
    }

    // 3. score.zIn widens the deadband → a previously-yellow value becomes green (100).
    {
        const std::vector<MetricSeries> s = { metric("leadWristFlexExt", -9.0) }; // z=-2 (penalised) → yellow
        const ScoreBreakdown def = SwingScorer::score(s, 1);
        check(sm(def, "leadWristFlexExt")->band == QStringLiteral("yellow")
              && sm(def, "leadWristFlexExt")->subScore < 100.0, "z≈2 → yellow, <100 by default");

        QVariantMap ov; ov["score.zIn"] = 2.5;   // deadband now covers |z|=2
        const ScoreBreakdown b = SwingScorer::score(s, 1, ov);
        check(sm(b, "leadWristFlexExt")->subScore == 100.0 && sm(b, "leadWristFlexExt")->band
                                                                  == QStringLiteral("green"),
              "widened deadband → green/100");
    }

    // 4. score.<key>.weight changes aggregation (sanity: a fault's weight matters).
    {
        const std::vector<MetricSeries> s = {
            metric("leadWristFlexExt", -30.0),  // catastrophic (one-sided, ~1)
            metric("leadArmFlexion",    5.0),   // 100
        };
        const ScoreBreakdown def = SwingScorer::score(s, 1);
        QVariantMap ov; ov["score.leadWristFlexExt.weight"] = 0.01;  // de-weight the fault
        const ScoreBreakdown b = SwingScorer::score(s, 1, ov);
        check(b.overall > def.overall, "de-weighting the fault raises overall");
    }

    std::printf("--- assessment engine: sampler.* / rules.* / bands.* ---\n");

    // 5. wristAssessmentConfigFor maps every key onto the config; empty map → defaults.
    {
        QVariantMap ov;
        ov["sampler.windowHalfUs"] = 9000;
        ov["sampler.gimbalThresholdDeg"] = 60.0;
        ov["sampler.minValidSamples"] = 3;
        ov["rules.confidenceFloor"] = 0.6;
        ov["rules.scoreScale"] = 22.0;
        ov["rules.severityWeightFault"] = 1.5;
        ov["rules.severityWeightWatch"] = 0.7;
        ov["rules.corroborationBoost"] = 0.4;
        ov["rules.strengthsRequireAdjacentFault"] = false;
        ov["bands.flexExtMargin"] = 8.0;
        const WristAssessmentConfig cfg = wristAssessmentConfigFor(ov);
        check(cfg.sampling.windowHalfUs == 9000 && cfg.sampling.gimbalThresholdDeg == 60.0
              && cfg.sampling.minValidSamples == 3, "sampler.* mapped");
        check(cfg.rules.confidenceFloor == 0.6f && cfg.rules.scoreScale == 22.0
              && cfg.rules.severityWeightFault == 1.5 && cfg.rules.severityWeightWatch == 0.7
              && cfg.rules.corroborationBoost == 0.4 && cfg.rules.strengthsRequireAdjacentFault == false,
              "rules.* mapped");
        check(cfg.band.tuning.flexExtMargin == 8.0, "bands.flexExtMargin mapped");

        const WristAssessmentConfig def = wristAssessmentConfigFor(QVariantMap{});
        check(def.sampling.gimbalThresholdDeg == 75.0 && def.rules.scoreScale == 18.0
              && def.band.tuning.flexExtMargin == -1.0, "empty map → frozen defaults");
    }

    // 6. The band provider honours a bands.* margin override (amber widens; green unchanged).
    {
        ConfigReferenceBandProvider prov;
        const Band b0 = prov.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, BandContext{});
        BandContext ctx; ctx.tuning.flexExtMargin = 20.0;
        const Band b1 = prov.band(PpJointDof::LeadWristFlexExt, PpSwingPosition::P4, ctx);
        check(b1.amberLo < b0.amberLo && b1.amberHi > b0.amberHi, "bands.flexExtMargin widens amber");
        check(b1.greenLo == b0.greenLo && b1.greenHi == b0.greenHi, "margin override leaves green unchanged");
    }

    std::printf("--- pose.* offline ORT intra-op thread count (PoseRunner seam) ---\n");

    // 7. pose.intraOpThreads resolves onto its frozen-constant-seeded local — the
    //    exact seam PoseRunner uses (seed from opt.intraOpThreads ==
    //    pose::kIntraOpThreads, then tuning::apply the override, then
    //    estimator.setIntraOpThreads before load() sizes the pool). Empty map ⇒
    //    frozen default 0, i.e. the legacy heuristic (thread-count-identical to
    //    history). > 0 pins; -1 is the opt-in physical-core topology sentinel.
    {
        int intraOp = pinpoint::tuned::pose::kIntraOpThreads;
        tuning::apply(QVariantMap{}, "pose.intraOpThreads", intraOp);
        check(intraOp == 0, "empty map → pose.intraOpThreads frozen default 0 (legacy auto)");

        QVariantMap ov; ov["pose.intraOpThreads"] = 6;
        tuning::apply(ov, "pose.intraOpThreads", intraOp);
        check(intraOp == 6, "pose.intraOpThreads override reaches the load() seam");

        ov["pose.intraOpThreads"] = -1;   // topology-auto sentinel (physicalCoreCount clamped [1,16])
        tuning::apply(ov, "pose.intraOpThreads", intraOp);
        check(intraOp == -1, "pose.intraOpThreads = -1 (topology auto) resolves");
    }

    std::printf("--- shaft.* onset no-return veto + bridging + Takeaway event (segmentPhases seam) ---\n");

    // 8. The four camera-only Address/Takeaway knobs resolve onto their frozen-
    //    constant-seeded locals — the exact seam ShaftV3Config::fromOverrides uses
    //    (seed from tuned::shaft::k*, then tuning::apply the override). Empty map ⇒
    //    the 2026-07-17 FROZEN-ON defaults (box 7 / gap 15 / bridge 10 / Takeaway
    //    on — 17-swing truth: Address-error median 0.564 → 0.060 s); 0 keys still
    //    disable each (the legacy soak baseline). (The retired anchor-box veto's
    //    shaft.onsetReturnPhiDeg / shaft.onsetReturnStillFrames keys are GONE —
    //    the departure-referenced revisit scan needs neither.)
    {
        double boxPx  = pinpoint::tuned::shaft::kOnsetReturnBoxPx;
        int    gapF   = pinpoint::tuned::shaft::kOnsetReturnGapFrames;
        int    bridge = pinpoint::tuned::shaft::kOnsetRunBridgeFrames;
        double m3gate = pinpoint::tuned::shaft::kOnsetBridgeMinNetFrac;
        bool   emitTk = pinpoint::tuned::shaft::kEmitTakeaway;
        tuning::apply(QVariantMap{}, "shaft.onsetReturnBoxPx", boxPx);
        tuning::apply(QVariantMap{}, "shaft.onsetReturnGapFrames", gapF);
        tuning::apply(QVariantMap{}, "shaft.onsetRunBridgeFrames", bridge);
        tuning::apply(QVariantMap{}, "shaft.onsetBridgeMinNetFrac", m3gate);
        tuning::apply(QVariantMap{}, "shaft.emitTakeaway", emitTk);
        check(boxPx == 7.0 && gapF == 15 && bridge == 10 && emitTk == true,
              "empty map → shaft onset/bridge/Takeaway frozen ON defaults (2026-07-17 freeze)");
        check(m3gate == 0.2, "empty map → shaft.onsetBridgeMinNetFrac frozen ON 0.2 (2026-07-18 freeze)");

        QVariantMap ov;                            // distinct non-default values
        ov["shaft.onsetReturnBoxPx"] = 0.0;        // 0 = the legacy dark baseline
        ov["shaft.onsetReturnGapFrames"] = 20;
        ov["shaft.onsetRunBridgeFrames"] = 0;
        ov["shaft.onsetBridgeMinNetFrac"] = 0.0;   // 0 = m3gate dark-out
        ov["shaft.emitTakeaway"] = false;
        tuning::apply(ov, "shaft.onsetReturnBoxPx", boxPx);
        tuning::apply(ov, "shaft.onsetReturnGapFrames", gapF);
        tuning::apply(ov, "shaft.onsetRunBridgeFrames", bridge);
        tuning::apply(ov, "shaft.onsetBridgeMinNetFrac", m3gate);
        tuning::apply(ov, "shaft.emitTakeaway", emitTk);
        check(boxPx == 0.0 && gapF == 20 && bridge == 0 && m3gate == 0.0 && emitTk == false,
              "shaft onset/bridge/m3gate/Takeaway overrides (incl. dark-out) reach their fromOverrides seam");
    }

    std::printf("--- ball.* club-activity + positions.p1ClubQuietSigma + ball.tk0AddressOverride (W3/W4) ---\n");

    // 9. The W3 club-activity producer knobs (BallActivityConfig::fromOverrides),
    //    the W3 club-quiet consumer gate (PositionsConfig via ShaftV3Config::
    //    fromOverrides) and the W4 tk0 A/B key (applyBallAnchor) resolve onto their
    //    frozen-constant-seeded locals — the exact seam each fromOverrides uses.
    //    Empty map ⇒ activity ON (FROZEN 2026-07-18 with refine.enabled — the
    //    load-bearing Tier-B input; a false key still darks it out, the soak
    //    baseline) and tk0 override OFF (FROZEN 2026-07-17: the earliest-departure
    //    tk0 fires on the first fidget departure and overwrote a good hold-end
    //    Address); true restores the old overwrite for A/B.
    {
        bool   act    = pinpoint::tuned::ball::activity::kClubActivity;
        int    refF   = pinpoint::tuned::ball::activity::kActivityRefFrames;
        double innerR = pinpoint::tuned::ball::activity::kActivityInnerR;
        double outerR = pinpoint::tuned::ball::activity::kActivityOuterR;
        double quietS = pinpoint::tuned::positions::kP1ClubQuietSigma;
        bool   tk0Ov  = pinpoint::tuned::ball::kTk0AddressOverride;
        tuning::apply(QVariantMap{}, "ball.clubActivity", act);
        tuning::apply(QVariantMap{}, "ball.activityRefFrames", refF);
        tuning::apply(QVariantMap{}, "ball.activityInnerR", innerR);
        tuning::apply(QVariantMap{}, "ball.activityOuterR", outerR);
        tuning::apply(QVariantMap{}, "positions.p1ClubQuietSigma", quietS);
        tuning::apply(QVariantMap{}, "ball.tk0AddressOverride", tk0Ov);
        check(act == true && refF == 9 && innerR == 1.5 && outerR == 5.0
              && quietS == 3.0 && tk0Ov == false,
              "empty map → W3/W4 frozen defaults (activity ON 2026-07-18, tk0 override OFF 2026-07-17)");

        QVariantMap ov;
        ov["ball.clubActivity"] = false;           // dark-out: the byte-identical soak baseline
        ov["ball.activityRefFrames"] = 5;
        ov["ball.activityInnerR"] = 2.0;
        ov["ball.activityOuterR"] = 6.0;
        ov["positions.p1ClubQuietSigma"] = 2.0;
        ov["ball.tk0AddressOverride"] = true;      // the A/B: restore the old overwrite
        tuning::apply(ov, "ball.clubActivity", act);
        tuning::apply(ov, "ball.activityRefFrames", refF);
        tuning::apply(ov, "ball.activityInnerR", innerR);
        tuning::apply(ov, "ball.activityOuterR", outerR);
        tuning::apply(ov, "positions.p1ClubQuietSigma", quietS);
        tuning::apply(ov, "ball.tk0AddressOverride", tk0Ov);
        check(act == false && refF == 5 && innerR == 2.0 && outerR == 6.0
              && quietS == 2.0 && tk0Ov == true,
              "W3/W4 overrides (incl. activity dark-out) reach their fromOverrides seams");
    }

    std::printf("--- refine.* late-pipeline event refinement (EventRefineConfig::fromOverrides) ---\n");

    // 10. Every refine.* key maps onto EventRefineConfig; empty map ⇒ the frozen
    //     2026-07-18 ON defaults (enabled true, minConf 0.8 — 17-swing truth: median
    //     held 0.052 s, max 0.577 → 0.145 s, within-100ms 12 → 14, zero regressions).
    //     The override direction is DARK-OUT: refine.enabled = false restores the
    //     byte- and code-path-identical pre-refine pipeline (the soak baseline).
    {
        const EventRefineConfig def = EventRefineConfig::fromOverrides(QVariantMap{});
        check(def.enabled == true && def.takeaway == true && def.address == true
              && def.impactResidual == true && def.departThetaDeg == 25.0
              && def.activityQuietSigma == 3.0 && def.returnHoldMs == 200
              && def.minConf == 0.8 && def.maxShiftS == 3.0,
              "empty map → refine.* frozen ON defaults (2026-07-18 freeze)");

        QVariantMap ov;
        ov["refine.enabled"]           = false;    // dark-out: the pre-refine soak baseline
        ov["refine.takeaway"]          = false;
        ov["refine.address"]           = false;
        ov["refine.impactResidual"]    = false;
        ov["refine.departThetaDeg"]    = 30.0;
        ov["refine.activityQuietSigma"] = 2.5;
        ov["refine.returnHoldMs"]      = 150;
        ov["refine.minConf"]           = 0.4;
        ov["refine.maxShiftS"]         = 2.0;
        const EventRefineConfig c = EventRefineConfig::fromOverrides(ov);
        check(c.enabled == false && c.takeaway == false && c.address == false
              && c.impactResidual == false && c.departThetaDeg == 30.0
              && c.activityQuietSigma == 2.5 && c.returnHoldMs == 150
              && c.minConf == 0.4 && c.maxShiftS == 2.0,
              "refine.* overrides (incl. enabled dark-out) reach EventRefineConfig::fromOverrides");
    }

    std::printf("--- kinematics.* club-kinematics display series (KinematicSeriesConfig::fromOverrides) ---\n");

    // 11. kinematics.enabled maps onto KinematicSeriesConfig; empty map ⇒ the ON default
    //     (2026-07-18 — the clubhead/hand-speed + lag review-chart series live on Wrist and
    //     the Swing/GRF/Coach camera-kinematics analyzer). Override direction is DARK-OUT:
    //     kinematics.enabled = false skips the stage (Wrist byte-identical, others stub).
    {
        const KinematicSeriesConfig def = KinematicSeriesConfig::fromOverrides(QVariantMap{});
        check(def.enabled == true, "empty map → kinematics.enabled ON default (2026-07-18)");

        QVariantMap ov;
        ov["kinematics.enabled"] = false;   // dark-out: restores the pre-kinematics pipeline
        const KinematicSeriesConfig c = KinematicSeriesConfig::fromOverrides(ov);
        check(c.enabled == false,
              "kinematics.enabled dark-out override reaches KinematicSeriesConfig::fromOverrides");
    }

    std::printf("--- tempo.* tempo metrics (TempoConfig::fromOverrides) ---\n");

    // 12. Every tempo.* key maps onto TempoConfig; empty map ⇒ the frozen ON
    //     defaults. Override direction is DARK-OUT: tempo.enabled = false emits
    //     no series, which is the OFF-parity path for the corpus gate.
    {
        const TempoConfig def = TempoConfig::fromOverrides(QVariantMap{});
        check(def.enabled == true && def.minConf == pinpoint::tuned::tempo::kMinConf
              && def.baseSigmaS == pinpoint::tuned::tempo::kBaseSigmaS
              && def.confInflate == pinpoint::tuned::tempo::kConfInflate,
              "empty map → tempo.* frozen defaults");

        QVariantMap ov;
        ov["tempo.enabled"]     = false;   // dark-out: no tempo series at all
        ov["tempo.minConf"]     = 0.75;
        ov["tempo.baseSigmaS"]  = 0.05;
        ov["tempo.confInflate"] = 2.0;
        const TempoConfig c = TempoConfig::fromOverrides(ov);
        check(c.enabled == false && c.minConf == 0.75 && c.baseSigmaS == 0.05
              && c.confInflate == 2.0,
              "tempo.* overrides (incl. enabled dark-out) reach TempoConfig::fromOverrides");
    }

    std::printf("--- ballpos.* ball position at address (BallPositionConfig::fromOverrides) ---\n");

    // 13. Every ballpos.* key maps onto BallPositionConfig; empty map ⇒ the frozen
    //     ON defaults. Dark-out: ballpos.enabled = false emits no ballPosition
    //     series AND no px→mm ruler, so stanceWidth reverts to ×frame — i.e. the
    //     single flag restores the whole pre-change foot-metrics output.
    {
        const BallPositionConfig def = BallPositionConfig::fromOverrides(QVariantMap{});
        check(def.enabled == true
              && def.addrWindowUs == pinpoint::tuned::ballpos::kAddrWindowUs
              && def.minSamples == pinpoint::tuned::ballpos::kMinSamples
              && def.maxJumpPx == pinpoint::tuned::ballpos::kMaxJumpPx
              && def.fracLo == pinpoint::tuned::ballpos::kFracLo
              && def.fracHi == pinpoint::tuned::ballpos::kFracHi,
              "empty map → ballpos.* frozen defaults");

        QVariantMap ov;
        ov["ballpos.enabled"]      = false;   // dark-out: no ball position, no mm ruler
        ov["ballpos.addrWindowUs"] = qint64(500000);
        ov["ballpos.minSamples"]   = 7;
        ov["ballpos.maxJumpPx"]    = 12.5;
        ov["ballpos.fracLo"]       = -0.25;
        ov["ballpos.fracHi"]       = 1.25;
        const BallPositionConfig c = BallPositionConfig::fromOverrides(ov);
        check(c.enabled == false && c.addrWindowUs == 500000 && c.minSamples == 7
              && c.maxJumpPx == 12.5 && c.fracLo == -0.25 && c.fracHi == 1.25,
              "ballpos.* overrides (incl. enabled dark-out) reach BallPositionConfig::fromOverrides");
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
