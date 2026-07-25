// Standalone test for SwingDocWriter (round-trip). Run via CTest (src/Analysis/tests/CMakeLists.txt):
//   cmake -S src/Analysis/tests -B build/analyzer-tests -DCMAKE_PREFIX_PATH=$HOME/Qt/6.11.0/gcc_64
//   cmake --build build/analyzer-tests && ctest --test-dir build/analyzer-tests --output-on-failure

#include "../swing_doc.h"
#include "../swing_paths.h"
#include "../../Analysis/swing_analysis.h"

// Stub — avoids linking swing_paths.cpp (which pulls in the PpLogStream logging deps).
// SwingDocReader::latestSessionDir() (the only sanitise() user) isn't exercised here.
QString pinpoint::SwingPaths::sanitise(const QString &raw) { return raw; }

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace pinpoint;
using namespace pinpoint::analysis;

static int g_fail = 0;
static void check(bool c, const char *label)
{
    std::printf("  [%s] %s\n", c ? "PASS" : "FAIL", label);
    if (!c) ++g_fail;
}

int main()
{
    const QString dir = QStringLiteral("/tmp/swingdoc_test");
    QDir().mkpath(dir);

    QJsonObject manifest;
    manifest[QStringLiteral("schema")]  = QStringLiteral("pinpoint.swing/1");
    manifest[QStringLiteral("swing")]   = QJsonObject{ {QStringLiteral("index"), 7},
                                                       {QStringLiteral("id"), QStringLiteral("swing_0007")} };
    manifest[QStringLiteral("streams")] = QJsonArray{
        QJsonObject{ {QStringLiteral("kind"), QStringLiteral("video")},
                     {QStringLiteral("alias"), QStringLiteral("face-on")},
                     {QStringLiteral("setup"), QJsonObject{ {QStringLiteral("perspective"), 2},
                                                            {QStringLiteral("perspectiveName"), QStringLiteral("FaceOn")},
                                                            {QStringLiteral("mirrored"), false},
                                                            {QStringLiteral("fixedInPlace"), true} }} },
        QJsonObject{ {QStringLiteral("kind"), QStringLiteral("imu")},
                     {QStringLiteral("alias"), QStringLiteral("Hand")},
                     {QStringLiteral("device"), QJsonObject{ {QStringLiteral("outputRateHz"), 200},
                                                             {QStringLiteral("fusionMode"), QStringLiteral("6axis")},
                                                             {QStringLiteral("orientationFilter"), QStringLiteral("Madgwick")},
                                                             {QStringLiteral("placementSlot"), QStringLiteral("B")} }} } };
    manifest[QStringLiteral("clock")]   = QJsonObject{ {QStringLiteral("wallclock"), QStringLiteral("2026-06-08T16:00:00.000")} };
    manifest[QStringLiteral("capture")] = QJsonObject{
        {QStringLiteral("sessionType"), 1},
        {QStringLiteral("shotSource"),  1},
        {QStringLiteral("swingDetectionSensitivity"), QStringLiteral("Medium")},
        {QStringLiteral("latencyUs"), QJsonObject{ {QStringLiteral("imuBle"), 30000},
                                                   {QStringLiteral("audioDevice"), 20000} }},
        {QStringLiteral("host"), QJsonObject{ {QStringLiteral("app"), QStringLiteral("PinPointStudio")},
                                              {QStringLiteral("gitSha"), QStringLiteral("abc1234")} }} };
    manifest[QStringLiteral("thumbnail")] = QJsonObject{ {QStringLiteral("file"), QStringLiteral("thumb.jpg")},
                                                         {QStringLiteral("t_us"), static_cast<qint64>(10000)} };

    SwingAnalysis a;
    a.tier          = int(ReconstructionTier::Mono3DPlusImu);
    a.score.overall = 82;
    MetricSeries m;
    m.key = QStringLiteral("leadWristFlexExt"); m.label = QStringLiteral("Lead-wrist flex/ext"); m.unit = QStringLiteral("°");
    m.t_us  = { 1000000, 1005000, 1010000 };
    m.value = { 0.0, 12.5, -8.0 };
    m.phaseSamples.push_back({ Phase::Impact, 1010000, -8.0, QStringLiteral("green") });
    a.series.push_back(m);
    // A Summary scalar carrying a measurement uncertainty: empty curve, one
    // Impact phaseSample, non-degree unit. Covers both additions at once — the
    // optional `sigma` field and the unit-aware flat-metric formatting (this
    // used to render as "3°" because the reader hardcoded degrees).
    MetricSeries tr;
    tr.key   = QStringLiteral("tempoRatio");
    tr.label = QStringLiteral("Tempo ratio");
    tr.unit  = QStringLiteral(":1");
    tr.sigma = 0.25;
    tr.phaseSamples.push_back({ Phase::Impact, 1010000, 3.0, QString() });
    a.series.push_back(tr);
    a.phases.push_back({ Phase::Impact, 1010000, 1.0f });
    a.phases.push_back({ Phase::Top, 700000, 0.9f, SegmentRole::LeadHand });

    // Segmentation block (v3 G2): swing bounds + ladder meta.
    a.segmentation.swingStartUs = 250000;
    a.segmentation.swingEndUs   = 1400000;
    a.segmentation.conf         = 0.84f;
    a.segmentation.version      = 2;

    // ShaftTracker blocks (S3): one pose frame + a valid 2-sample club track.
    PoseFrame2D pf;
    pf.t_us = 1000000;
    pf.kp[9]   = QPointF(0.40, 0.55); pf.conf[9]  = 0.9f;   // left wrist
    pf.kp[10]  = QPointF(0.42, 0.56); pf.conf[10] = 0.8f;   // right wrist
    pf.leadHand  = QPointF(0.41, 0.57);
    pf.trailHand = QPointF(0.43, 0.58);
    pf.handConf  = 0.85f;
    a.pose2d.camera = 3;
    a.pose2d.frames.push_back(pf);

    a.shaft.camera        = 3;
    a.shaft.valid         = true;
    a.shaft.coverage      = 0.95f;
    a.shaft.imuVisionCorr = 0.97f;
    a.shaft.frameWidth    = 1920;
    a.shaft.frameHeight   = 1080;
    a.shaft.samples.push_back({ 1000000, QPointF(960.0, 540.0), QPointF(960.0, 810.0),
                                1.5708, 0.0, 270.0, 0.9f, ShaftMeasured });
    a.shaft.samples.push_back({ 1010000, QPointF(950.0, 545.0), QPointF(700.0, 700.0),
                                2.2, 30.0, 268.0, 0.8f,
                                uint8_t(ShaftImuBridged | ShaftHeadProjected) });
    // R7 dual output: pure-model predicted series + model/vision residual.
    a.shaft.modelVisionResidualDeg = 4.2f;
    a.shaft.predicted.push_back({ 1000000, QPointF(960.0, 540.0), QPointF(965.0, 800.0),
                                  1.55, 0.0, 270.0, 0.7f, ShaftKinematicPredicted });
    a.shaft.predicted.push_back({ 1010000, QPointF(950.0, 545.0), QPointF(720.0, 690.0),
                                  2.15, 0.0, 268.0, 0.6f, ShaftKinematicPredicted });

    // Ball track (v3.4): one found sample (drawn) + one post-launch gap sample.
    a.ball.camera       = 3;
    a.ball.launchTUs    = 1010000;
    a.ball.launchCenter = QPointF(0.50, 0.80);
    a.ball.frames.push_back({ 1000000, true,  QPointF(0.50, 0.80), 0.02f, 0.9f });
    a.ball.frames.push_back({ 1010000, false, QPointF(0.50, 0.80), 0.0f,  0.0f });

    // IMU binding with the calibration-status snapshot (corpus provenance).
    BindingRecord bind;
    bind.serial = QStringLiteral("WT901-1234");
    bind.role   = SegmentRole::LeadHand;
    bind.alignA = QQuaternion(1.0f, 0.0f, 0.0f, 0.0f);
    bind.mountM = QQuaternion(0.5f, -0.5f, -0.5f, -0.5f);
    bind.anatCalibrated       = true;
    bind.calibrated           = true;
    bind.mountDeviationDeg    = 3.2;
    bind.mountGravityErrorDeg = 5.1;
    bind.calibratedAtUtc      = QStringLiteral("2026-06-11T09:12:00.123Z");
    bind.calibAgeSec          = 412.5;
    a.bindings.push_back(bind);

    std::printf("=== unified write (raw + analysis) ===\n");
    QString err;
    if (!SwingDocWriter::writeSwingJson(dir, manifest, &a, &err)) {
        std::printf("  [FAIL] write: %s\n", err.toUtf8().constData());
        return 1;
    }
    QFile f(dir + QStringLiteral("/swing.json"));
    if (!f.open(QIODevice::ReadOnly))  return 1;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    f.close();   // release the handle: on Windows an open reader blocks the
                 // QSaveFile atomic replace that updateReview()/writeSwingJson() do below

    check(root[QStringLiteral("schema")].toString() == QStringLiteral("pinpoint.swing/2"), "schema bumped to pinpoint.swing/2");
    check(root.contains(QStringLiteral("streams")), "raw streams preserved");
    check(root[QStringLiteral("swing")].toObject()[QStringLiteral("id")].toString() == QStringLiteral("swing_0007"), "raw swing block preserved");
    check(root.contains(QStringLiteral("analysis")), "inline analysis block present");
    const QJsonObject an = root[QStringLiteral("analysis")].toObject();
    check(an[QStringLiteral("schema")].toString() == QStringLiteral("pinpoint.analysis/3"), "analysis schema /3");
    // /3: "score" is now a ScoreBreakdown object (design §B.0a); overall preserved, default kind=adherence.
    const QJsonObject scoreObj = an[QStringLiteral("score")].toObject();
    check(scoreObj[QStringLiteral("overall")].toInt() == 82, "analysis score.overall = 82");
    check(scoreObj[QStringLiteral("kind")].toString() == QStringLiteral("adherence"), "analysis score.kind = adherence");
    check(an[QStringLiteral("tier")].toInt() == int(ReconstructionTier::Mono3DPlusImu), "tier");
    const QJsonArray mets = an[QStringLiteral("metrics")].toArray();
    check(mets.size() == 2, "two metric series");
    const QJsonObject m0 = mets.at(0).toObject();
    check(m0[QStringLiteral("key")].toString() == QStringLiteral("leadWristFlexExt"), "metric key");
    check(m0[QStringLiteral("t_us")].toArray().size() == 3 && m0[QStringLiteral("value")].toArray().size() == 3, "t_us + value arrays (len 3)");
    check(qFuzzyCompare(m0[QStringLiteral("value")].toArray().at(1).toDouble(), 12.5), "value[1] == 12.5");
    check(m0[QStringLiteral("phaseSamples")].toArray().size() == 1, "phaseSamples");
    // sigma is OPTIONAL and absent-by-default: a metric that never set one must
    // not gain the key, or every pre-existing swing.json stops round-tripping
    // byte-identically.
    check(!m0.contains(QStringLiteral("sigma")), "no sigma key when the producer set none");
    const QJsonObject m1 = mets.at(1).toObject();
    check(m1[QStringLiteral("key")].toString() == QStringLiteral("tempoRatio"), "summary metric key");
    check(m1[QStringLiteral("t_us")].toArray().isEmpty() && m1[QStringLiteral("value")].toArray().isEmpty(),
          "summary scalar has an empty curve");
    check(m1.contains(QStringLiteral("sigma"))
              && qFuzzyCompare(m1[QStringLiteral("sigma")].toDouble(), 0.25),
          "sigma serialized when present");
    check(an[QStringLiteral("phases")].toArray().size() == 2, "phases array");
    check(static_cast<qint64>(an[QStringLiteral("phases")].toArray().at(0).toObject()[QStringLiteral("t_us")].toDouble()) == 1010000, "phase t_us preserved");
    check(an[QStringLiteral("phases")].toArray().at(1).toObject()[QStringLiteral("segment")].toInt()
              == int(SegmentRole::LeadHand), "phase provenance preserved");

    std::printf("\n=== capture / setup / device passthrough ===\n");
    {
        check(root.contains(QStringLiteral("capture")), "capture block preserved");
        const QJsonObject cap = root[QStringLiteral("capture")].toObject();
        check(cap[QStringLiteral("sessionType")].toInt() == 1, "capture.sessionType");
        check(cap[QStringLiteral("latencyUs")].toObject()[QStringLiteral("imuBle")].toInt() == 30000,
              "capture.latencyUs.imuBle");
        check(cap[QStringLiteral("host")].toObject()[QStringLiteral("gitSha")].toString()
                  == QStringLiteral("abc1234"), "capture.host.gitSha");
        const QJsonArray strs = root[QStringLiteral("streams")].toArray();
        check(strs.at(0).toObject()[QStringLiteral("setup")].toObject()[QStringLiteral("perspective")].toInt() == 2,
              "video stream setup.perspective");
        check(strs.at(1).toObject()[QStringLiteral("device")].toObject()[QStringLiteral("outputRateHz")].toInt() == 200,
              "imu stream device.outputRateHz");
    }

    std::printf("\n=== bindings calibration status ===\n");
    {
        check(an.contains(QStringLiteral("bindings")), "bindings block present");
        const QJsonArray binds = an[QStringLiteral("bindings")].toArray();
        check(binds.size() == 1, "one binding");
        const QJsonObject b0 = binds.at(0).toObject();
        check(b0[QStringLiteral("serial")].toString() == QStringLiteral("WT901-1234"), "binding serial");
        check(b0[QStringLiteral("role")].toInt() == int(SegmentRole::LeadHand), "binding role");
        check(b0[QStringLiteral("calibrated")].toBool(), "binding calibrated");
        check(b0[QStringLiteral("anatCalibrated")].toBool(), "binding anatCalibrated");
        check(qFuzzyCompare(b0[QStringLiteral("mountDeviationDeg")].toDouble(), 3.2), "mountDeviationDeg");
        check(qFuzzyCompare(b0[QStringLiteral("mountGravityErrorDeg")].toDouble(), 5.1), "mountGravityErrorDeg");
        check(b0[QStringLiteral("calibratedAt")].toString() == QStringLiteral("2026-06-11T09:12:00.123Z"),
              "calibratedAt ISO string");
        check(qFuzzyCompare(b0[QStringLiteral("calibAgeSec")].toDouble(), 412.5), "calibAgeSec");
        check(qFuzzyCompare(b0[QStringLiteral("mountM")].toArray().at(0).toDouble(), 0.5), "mountM w");
    }

    std::printf("\n=== segmentation block (v3 G2) ===\n");
    {
        check(an.contains(QStringLiteral("segmentation")), "segmentation block present");
        const QJsonObject sg = an[QStringLiteral("segmentation")].toObject();
        check(static_cast<qint64>(sg[QStringLiteral("swingStartUs")].toDouble()) == 250000,
              "swingStartUs preserved");
        check(static_cast<qint64>(sg[QStringLiteral("swingEndUs")].toDouble()) == 1400000,
              "swingEndUs preserved");
        check(qFuzzyCompare(sg[QStringLiteral("conf")].toDouble(), double(0.84f)),
              "segmentation conf preserved");
        check(sg[QStringLiteral("version")].toInt() == 2, "segmentation version preserved");
    }

    std::printf("\n=== ShaftTracker blocks (pose2d + club) ===\n");
    {
        check(an.contains(QStringLiteral("pose2d")), "pose2d block present");
        const QJsonObject p2 = an[QStringLiteral("pose2d")].toObject();
        check(p2[QStringLiteral("camera")].toInt() == 3, "pose2d.camera");
        const QJsonArray pframes = p2[QStringLiteral("frames")].toArray();
        check(pframes.size() == 1, "pose2d one frame");
        const QJsonObject pf0 = pframes.at(0).toObject();
        check(p2[QStringLiteral("keypointCount")].toInt() == 133, "pose2d.keypointCount 133");
        check(pf0[QStringLiteral("kp")].toArray().size() == 133 * 3,
              "kp flat array 399 long (COCO-WholeBody)");
        check(qFuzzyCompare(pf0[QStringLiteral("kp")].toArray().at(9 * 3).toDouble(), 0.40),
              "left-wrist x at kp[27]");
        check(qFuzzyCompare(pf0[QStringLiteral("handConf")].toDouble(), double(0.85f)), "handConf");
        check(qFuzzyCompare(pf0[QStringLiteral("lead")].toArray().at(1).toDouble(), 0.57),
              "lead hand y");
        // Byte-additivity: an analysis with an empty smoothed vector must NOT emit
        // the "smoothed" key (Motion-overlay Phase 2 additive contract).
        check(!p2.contains(QStringLiteral("smoothed")),
              "pose2d without smoothed omits the block (byte-additive)");

        check(an.contains(QStringLiteral("club")), "club block present");
        const QJsonObject cb = an[QStringLiteral("club")].toObject();
        check(cb[QStringLiteral("valid")].toBool(), "club.valid");
        check(cb[QStringLiteral("frameWidth")].toInt() == 1920, "club.frameWidth");
        const QJsonArray cs = cb[QStringLiteral("samples")].toArray();
        check(cs.size() == 2, "club two samples");
        const QJsonObject c0 = cs.at(0).toObject();
        check(qFuzzyCompare(c0[QStringLiteral("grip")].toArray().at(0).toDouble(), 960.0 / 1920.0),
              "grip x normalized by frame width");
        check(qFuzzyCompare(c0[QStringLiteral("head")].toArray().at(1).toDouble(), 810.0 / 1080.0),
              "head y normalized by frame height");
        check(c0[QStringLiteral("flags")].toInt() == int(ShaftMeasured), "sample 0 flags");
        check(cs.at(1).toObject()[QStringLiteral("flags")].toInt()
                  == int(ShaftImuBridged | ShaftHeadProjected), "sample 1 flags");
        const double mvr = cb[QStringLiteral("modelVisionResidualDeg")].toDouble();
        check(mvr > 4.19 && mvr < 4.21, "club.modelVisionResidualDeg");
        const QJsonArray cp = cb[QStringLiteral("predicted")].toArray();
        check(cp.size() == 2, "club two predicted samples");
        check(cp.at(0).toObject()[QStringLiteral("flags")].toInt() == int(ShaftKinematicPredicted),
              "predicted flag ShaftKinematicPredicted");
        check(qFuzzyCompare(cp.at(0).toObject()[QStringLiteral("grip")].toArray().at(0).toDouble(),
                            960.0 / 1920.0), "predicted grip normalized by frame width");
    }

    std::printf("\n=== ball block (v3.4) ===\n");
    {
        check(an.contains(QStringLiteral("ball")), "ball block present");
        const QJsonObject bb = an[QStringLiteral("ball")].toObject();
        check(bb[QStringLiteral("camera")].toInt() == 3, "ball.camera");
        check(bb[QStringLiteral("valid")].toBool(), "ball.valid");
        const QJsonArray bs = bb[QStringLiteral("samples")].toArray();
        check(bs.size() == 2, "ball two samples");
        const QJsonObject b0 = bs.at(0).toObject();
        check(b0[QStringLiteral("found")].toBool(), "ball sample 0 found");
        check(qFuzzyCompare(b0[QStringLiteral("x")].toDouble(), 0.50), "ball sample 0 x normalized");
        check(qFuzzyCompare(b0[QStringLiteral("y")].toDouble(), 0.80), "ball sample 0 y normalized");
        check(qFuzzyCompare(b0[QStringLiteral("r")].toDouble(), double(0.02f)), "ball sample 0 radiusNorm");
        check(!bs.at(1).toObject()[QStringLiteral("found")].toBool(),
              "ball sample 1 post-launch not found");
    }

    std::printf("\n=== pose2d smoothed companion track (Motion overlay) ===\n");
    {
        // Absolute-domain manifest so we also verify smoothed t_us is written
        // window-relative (re-based by clock.t0_us) exactly like frames'.
        const qint64 T0 = 176400665083LL;
        QJsonObject mSm = manifest;
        mSm[QStringLiteral("clock")] = QJsonObject{ {QStringLiteral("t0_us"), double(T0)},
                                                    {QStringLiteral("wallclock"),
                                                     QStringLiteral("2026-06-08T16:00:00.000")} };
        SwingAnalysis aSm;
        aSm.tier = int(ReconstructionTier::Angles2D);
        aSm.pose2d.camera = 3;
        // Two RAW pose frames …
        PoseFrame2D r0; r0.t_us = T0 + 1000000; r0.kp[5] = QPointF(0.30, 0.40); r0.conf[5] = 0.9f;
        PoseFrame2D r1; r1.t_us = T0 + 1010000; r1.kp[5] = QPointF(0.31, 0.41); r1.conf[5] = 0.8f;
        aSm.pose2d.frames = { r0, r1 };
        // … and the parallel SMOOTHED companion + per-kp honesty aux.
        PoseFrame2D s0; s0.t_us = T0 + 1000000; s0.kp[5] = QPointF(0.305, 0.405); s0.conf[5] = 0.95f;
        PoseFrame2D s1; s1.t_us = T0 + 1010000; s1.kp[5] = QPointF(0.312, 0.412); s1.conf[5] = 0.85f;
        aSm.pose2d.smoothed = { s0, s1 };
        PoseKpAux x0; x0.tier[5] = uint8_t(PoseTier::Meas); x0.sigma[5] = 2.5f;
        PoseKpAux x1; x1.tier[5] = uint8_t(PoseTier::Pred); x1.sigma[5] = 4.0f;
        aSm.pose2d.smoothedAux = { x0, x1 };

        const QString dirS = dir + QStringLiteral("_smoothed");
        QDir().mkpath(dirS);
        QString serr;
        check(SwingDocWriter::writeSwingJson(dirS, mSm, &aSm, &serr), "smoothed write ok");

        QFile fs(dirS + QStringLiteral("/swing.json"));
        if (!fs.open(QIODevice::ReadOnly)) return 1;
        const QJsonObject rs = QJsonDocument::fromJson(fs.readAll()).object();
        fs.close();
        const QJsonObject p2 =
            rs[QStringLiteral("analysis")].toObject()[QStringLiteral("pose2d")].toObject();
        check(p2.contains(QStringLiteral("smoothed")), "smoothed block present when populated");
        check(p2[QStringLiteral("frames")].toArray().size() == 2,
              "raw frames still present alongside smoothed");
        const QJsonArray sm = p2[QStringLiteral("smoothed")].toArray();
        check(sm.size() == 2, "smoothed two frames");
        const QJsonObject sm0 = sm.at(0).toObject();
        // t_us re-based to window-relative like frames.
        check(static_cast<qint64>(sm0[QStringLiteral("t_us")].toDouble()) == 1000000,
              "smoothed[0] t_us window-relative");
        check(static_cast<qint64>(sm.at(1).toObject()[QStringLiteral("t_us")].toDouble()) == 1010000,
              "smoothed[1] t_us window-relative");
        check(sm0[QStringLiteral("kp")].toArray().size() == 133 * 3,
              "smoothed kp flat 399 long (COCO-WholeBody)");
        check(qFuzzyCompare(sm0[QStringLiteral("kp")].toArray().at(5 * 3).toDouble(), 0.305),
              "smoothed kp[5].x carried");
        check(qFuzzyCompare(sm0[QStringLiteral("kp")].toArray().at(5 * 3 + 2).toDouble(), double(0.95f)),
              "smoothed kp[5] conf carried");
        check(sm0[QStringLiteral("tier")].toArray().size() == 133, "smoothed tier 133 long");
        check(sm0[QStringLiteral("tier")].toArray().at(5).toInt() == int(PoseTier::Meas),
              "smoothed[0] tier[5] = Meas");
        check(sm.at(1).toObject()[QStringLiteral("tier")].toArray().at(5).toInt() == int(PoseTier::Pred),
              "smoothed[1] tier[5] = Pred");
        check(sm0[QStringLiteral("sigma")].toArray().size() == 133, "smoothed sigma 133 long");
        check(qFuzzyCompare(sm0[QStringLiteral("sigma")].toArray().at(5).toDouble(), double(2.5f)),
              "smoothed sigma[5] carried");
        // No hand fields on the smoothed frames (hands are not smoothed).
        check(!sm0.contains(QStringLiteral("lead")) && !sm0.contains(QStringLiteral("handConf")),
              "smoothed frame has no hand fields");

        // Reader passthrough: the whole-object toVariantMap carries smoothed for free.
        const PersistedShot ps = SwingDocReader::readSwingJson(dirS);
        check(ps.ok, "smoothed reader ok");
        const QVariantMap rp2 = ps.analysisDetail.value(QStringLiteral("pose2d")).toMap();
        const QVariantList rsm = rp2.value(QStringLiteral("smoothed")).toList();
        check(rsm.size() == 2, "reader analysisDetail.pose2d.smoothed len 2");
        const QVariantMap rsm0 = rsm.at(0).toMap();
        check(rsm0.value(QStringLiteral("kp")).toList().size() == 133 * 3,
              "reader smoothed kp flat 399");
        check(rsm0.value(QStringLiteral("tier")).toList().at(5).toInt() == int(PoseTier::Meas),
              "reader smoothed tier[5] = Meas");
        check(qFuzzyCompare(rsm0.value(QStringLiteral("sigma")).toList().at(5).toDouble(), double(2.5f)),
              "reader smoothed sigma[5] carried");
    }

    std::printf("\n=== reader round-trip ===\n");
    {
        const PersistedShot ps = SwingDocReader::readSwingJson(dir);
        check(ps.ok, "read ok");
        check(ps.ordinal == 7, "ordinal == 7");
        check(ps.hasVideo, "hasVideo true");
        check(!ps.thumbnailPath.isEmpty(), "thumbnail path resolved");
        check(ps.timestampLabel == QStringLiteral("16:00:00"), "timestamp from wallclock");
        check(ps.score == 82, "score == 82");
        const QVariantMap fe = ps.metrics.value(QStringLiteral("leadWristFlexExt")).toMap();
        check(fe.value(QStringLiteral("value")).toString() == QStringLiteral("-8°"), "flat metric value -8 deg");
        // Non-degree metrics format in their OWN unit — this reader used to
        // hardcode "°" and rendered every ratio/speed/×frame value as degrees.
        const QVariantMap tro = ps.metrics.value(QStringLiteral("tempoRatio")).toMap();
        check(tro.value(QStringLiteral("value")).toString() == QStringLiteral("3.00:1"),
              "flat metric formats in its own unit, not degrees");
        check(ps.analysisDetail.value(QStringLiteral("overall")).toInt() == 82, "analysisDetail.overall");
        check(ps.analysisDetail.value(QStringLiteral("series")).toList().size() == 2, "analysisDetail.series len 2");
        check(ps.analysisDetail.value(QStringLiteral("phases")).toList().size() == 2, "analysisDetail.phases len 2");
        const QVariantMap sg = ps.analysisDetail.value(QStringLiteral("segmentation")).toMap();
        check(sg.value(QStringLiteral("swingStartUs")).toLongLong() == 250000
                  && sg.value(QStringLiteral("swingEndUs")).toLongLong() == 1400000,
              "analysisDetail.segmentation bounds reload");
        const QVariantMap p2 = ps.analysisDetail.value(QStringLiteral("pose2d")).toMap();
        check(p2.value(QStringLiteral("frames")).toList().size() == 1, "reloaded pose2d frames");
        const QVariantMap cb = ps.analysisDetail.value(QStringLiteral("club")).toMap();
        check(cb.value(QStringLiteral("valid")).toBool(), "reloaded club.valid");
        check(cb.value(QStringLiteral("samples")).toList().size() == 2, "reloaded club samples");
        const QVariantMap cs1 = cb.value(QStringLiteral("samples")).toList().at(1).toMap();
        check(cs1.value(QStringLiteral("flags")).toInt()
                  == int(ShaftImuBridged | ShaftHeadProjected), "reloaded sample flags");
        check(cb.value(QStringLiteral("predicted")).toList().size() == 2, "reloaded predicted samples");
        const double mvr2 = cb.value(QStringLiteral("modelVisionResidualDeg")).toDouble();
        check(mvr2 > 4.19 && mvr2 < 4.21, "reloaded modelVisionResidualDeg");
        const QVariantMap rbb = ps.analysisDetail.value(QStringLiteral("ball")).toMap();
        check(rbb.value(QStringLiteral("samples")).toList().size() == 2, "reloaded ball samples");
        check(rbb.value(QStringLiteral("samples")).toList().at(0).toMap()
                  .value(QStringLiteral("found")).toBool(), "reloaded ball sample 0 found");
        const QVariantList rbinds = ps.analysisDetail.value(QStringLiteral("bindings")).toList();
        check(rbinds.size() == 1, "reloaded bindings len 1");
        const QVariantMap rb0 = rbinds.at(0).toMap();
        check(rb0.value(QStringLiteral("calibrated")).toBool(), "reloaded binding calibrated");
        check(qFuzzyCompare(rb0.value(QStringLiteral("calibAgeSec")).toDouble(), 412.5),
              "reloaded binding calibAgeSec");
    }

    std::printf("\n=== review write-through round-trip ===\n");
    {
        QString rerr;
        check(SwingDocWriter::updateReview(dir, 4, QStringLiteral("nice tempo"),
                                           QStringLiteral("7 IRON"), &rerr),
              "updateReview ok");
        // The review block lands without disturbing the raw/analysis blocks.
        QFile fr(dir + QStringLiteral("/swing.json"));
        if (!fr.open(QIODevice::ReadOnly)) return 1;
        const QJsonObject r = QJsonDocument::fromJson(fr.readAll()).object();
        fr.close();   // release before the updateReview() rewrite below (Windows replace)
        check(r.contains(QStringLiteral("analysis")), "analysis block survives review write");
        const QJsonObject rv = r[QStringLiteral("review")].toObject();
        check(rv[QStringLiteral("rating")].toInt() == 4, "review.rating == 4");
        check(rv[QStringLiteral("note")].toString() == QStringLiteral("nice tempo"), "review.note");
        check(rv[QStringLiteral("club")].toString() == QStringLiteral("7 IRON"), "review.club");

        const PersistedShot ps = SwingDocReader::readSwingJson(dir);
        check(ps.rating == 4, "reader rating == 4");
        check(ps.note == QStringLiteral("nice tempo"), "reader note");
        check(ps.club == QStringLiteral("7 IRON"), "reader club");

        // Rewriting replaces (does not append) the review block; clamps rating.
        check(SwingDocWriter::updateReview(dir, 9, QStringLiteral("re-rated"),
                                           QStringLiteral("PUTTER"), nullptr),
              "updateReview rewrite ok");
        const PersistedShot ps2 = SwingDocReader::readSwingJson(dir);
        check(ps2.rating == 5, "reader rating clamped to 5");
        check(ps2.note == QStringLiteral("re-rated"), "reader note rewritten");
        check(ps2.club == QStringLiteral("PUTTER"), "reader club rewritten");

        // No swing.json → updateReview fails harmlessly (returns false).
        check(!SwingDocWriter::updateReview(QStringLiteral("/tmp/swingdoc_test_nope"), 3, QString(),
                                            QStringLiteral("DRIVER"), nullptr),
              "updateReview on missing doc returns false");
    }

    std::printf("\n=== analysis t_us normalised to window-relative ===\n");
    {
        // Live capture: absolute analysis t_us (clock domain) → written relative.
        const qint64 T0 = 176400665083LL;
        QJsonObject mAbs = manifest;
        mAbs[QStringLiteral("clock")] = QJsonObject{ {QStringLiteral("t0_us"), double(T0)} };
        SwingAnalysis aAbs;
        aAbs.tier = int(ReconstructionTier::Angles2D);
        aAbs.phases.push_back({ Phase::Impact, T0 + 1010000, 1.0f });
        aAbs.segmentation.swingStartUs = T0 + 250000;
        aAbs.segmentation.swingEndUs   = T0 + 1400000;
        aAbs.segmentation.version = 2;
        aAbs.shaft.camera = 3; aAbs.shaft.valid = true; aAbs.shaft.coverage = 0.9f;
        aAbs.shaft.frameWidth = 1920; aAbs.shaft.frameHeight = 1080;
        aAbs.shaft.samples.push_back({ T0 + 1000000, QPointF(960, 540), QPointF(960, 810),
                                       1.57, 0.0, 270.0, 0.9f, ShaftMeasured });
        const QString dirN = dir + QStringLiteral("_norm");
        QDir().mkpath(dirN);
        SwingDocWriter::writeSwingJson(dirN, mAbs, &aAbs, nullptr);
        QFile fn(dirN + QStringLiteral("/swing.json"));
        if (fn.open(QIODevice::ReadOnly)) {
            const QJsonObject rn = QJsonDocument::fromJson(fn.readAll()).object();
            fn.close();
            const QJsonObject an = rn[QStringLiteral("analysis")].toObject();
            check(qint64(an[QStringLiteral("phases")].toArray().at(0).toObject()[QStringLiteral("t_us")].toDouble()) == 1010000,
                  "absolute phase t_us → window-relative");
            check(qint64(an[QStringLiteral("segmentation")].toObject()[QStringLiteral("swingStartUs")].toDouble()) == 250000,
                  "absolute swingStartUs → window-relative");
            check(qint64(an[QStringLiteral("club")].toObject()[QStringLiteral("samples")].toArray().at(0)
                            .toObject()[QStringLiteral("t_us")].toDouble()) == 1000000,
                  "absolute club sample t_us → window-relative");
        }

        // Re-analysis: already-relative t_us (≪ t0) pass through unchanged.
        SwingAnalysis aRel;
        aRel.phases.push_back({ Phase::Impact, 1010000, 1.0f });
        SwingDocWriter::writeSwingJson(dirN, mAbs, &aRel, nullptr);
        QFile fr(dirN + QStringLiteral("/swing.json"));
        if (fr.open(QIODevice::ReadOnly)) {
            const QJsonObject rr = QJsonDocument::fromJson(fr.readAll()).object();
            fr.close();
            check(qint64(rr[QStringLiteral("analysis")].toObject()[QStringLiteral("phases")].toArray()
                            .at(0).toObject()[QStringLiteral("t_us")].toDouble()) == 1010000,
                  "relative phase t_us passed through (idempotent)");
        }
    }

    std::printf("\n=== raw-only write (analysis == nullptr) ===\n");
    SwingDocWriter::writeSwingJson(dir, manifest, nullptr);
    QFile f2(dir + QStringLiteral("/swing.json"));
    if (!f2.open(QIODevice::ReadOnly)) return 1;
    const QJsonObject root2 = QJsonDocument::fromJson(f2.readAll()).object();
    f2.close();
    check(!root2.contains(QStringLiteral("analysis")), "no analysis block when null");
    check(root2[QStringLiteral("schema")].toString() == QStringLiteral("pinpoint.swing/2"), "schema still /2");

    // ── summary sidecar ─────────────────────────────────────────────────────
    // swing_summary.json is a regenerable cache of the scalars the session picker needs,
    // so it never has to parse a multi-MB swing.json on the GUI thread. Everything below
    // guards that it stays truthful — a sidecar that silently goes stale, or one that is
    // never actually read, reintroduces the freeze it exists to prevent.
    const QString sumPath = dir + QStringLiteral("/swing_summary.json");

    std::printf("\n=== summary sidecar: written on the happy path ===\n");
    {
        // updateReview() above rewrote swing.json last, so the sidecar must already be
        // present and stamped against that rewrite.
        check(QFile::exists(sumPath), "swing_summary.json exists after a write");

        QFile f(sumPath);
        if (!f.open(QIODevice::ReadOnly)) return 1;
        const QJsonObject s = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        check(s[QStringLiteral("schema")].toString() == QStringLiteral("pinpoint.swingsummary/1"),
              "sidecar schema tag");
        const QFileInfo srcInfo(dir + QStringLiteral("/swing.json"));
        const QJsonObject src = s[QStringLiteral("source")].toObject();
        check(qint64(src[QStringLiteral("size")].toDouble()) == srcInfo.size(),
              "sidecar stamps the COMMITTED source size");
        check(qint64(src[QStringLiteral("mtime_ms")].toDouble())
                  == srcInfo.lastModified().toMSecsSinceEpoch(),
              "sidecar stamps the COMMITTED source mtime");
    }

    std::printf("\n=== summary sidecar: PARITY with the full reader ===\n");
    {
        const PersistedShot fat  = SwingDocReader::readSwingJson(dir);
        const SwingSummary  lean = SwingDocReader::readSwingSummary(dir);
        // Without this the whole section could pass while every read silently fell back to
        // the fat parse — i.e. correct data, and the stall quietly back.
        check(lean.fromSidecar, "parity exercised the SIDECAR path, not a fat fallback");
        check(lean.ok == fat.ok,                         "parity ok");
        check(lean.ordinal == fat.ordinal,               "parity ordinal");
        check(lean.timestampLabel == fat.timestampLabel, "parity timestampLabel");
        check(lean.wallclockMs == fat.wallclockMs,       "parity wallclockMs");
        check(lean.club == fat.club,                     "parity club");
        check(lean.hasVideo == fat.hasVideo,             "parity hasVideo");
        check(lean.thumbnailPath == fat.thumbnailPath,   "parity thumbnailPath");
        check(lean.score == fat.score,                   "parity score");

        // Delete it: the fallback must produce identical values AND self-heal.
        QFile::remove(sumPath);
        const SwingSummary rebuilt = SwingDocReader::readSwingSummary(dir);
        check(!rebuilt.fromSidecar, "deleted sidecar → fat fallback");
        check(rebuilt.ok == fat.ok && rebuilt.ordinal == fat.ordinal
                  && rebuilt.timestampLabel == fat.timestampLabel
                  && rebuilt.wallclockMs == fat.wallclockMs && rebuilt.club == fat.club
                  && rebuilt.hasVideo == fat.hasVideo
                  && rebuilt.thumbnailPath == fat.thumbnailPath
                  && rebuilt.score == fat.score,
              "fat-fallback parity (identical to the sidecar path)");
        check(QFile::exists(sumPath), "fallback self-heals: sidecar rewritten");
        check(SwingDocReader::readSwingSummary(dir).fromSidecar, "healed sidecar is used next time");
    }

    std::printf("\n=== summary sidecar: stale detection ===\n");
    {
        const auto patchSidecar = [&](const char *key, const QJsonValue &v) {
            QFile f(sumPath);
            if (!f.open(QIODevice::ReadOnly)) return;
            QJsonObject s = QJsonDocument::fromJson(f.readAll()).object();
            f.close();
            if (QLatin1String(key) == QLatin1String("schema")) {
                s[QStringLiteral("schema")] = v;
            } else {
                QJsonObject src = s[QStringLiteral("source")].toObject();
                src[QString::fromLatin1(key)] = v;
                s[QStringLiteral("source")] = src;
            }
            QFile o(sumPath);
            if (!o.open(QIODevice::WriteOnly)) return;
            o.write(QJsonDocument(s).toJson(QJsonDocument::Compact));
        };

        patchSidecar("size", QJsonValue(1.0));
        check(!SwingDocReader::readSwingSummary(dir).fromSidecar, "wrong source.size → stale");
        check(SwingDocReader::readSwingSummary(dir).fromSidecar,  "…and is rewritten correctly");

        patchSidecar("mtime_ms", QJsonValue(1.0));
        check(!SwingDocReader::readSwingSummary(dir).fromSidecar, "wrong source.mtime_ms → stale");
        check(SwingDocReader::readSwingSummary(dir).fromSidecar,  "…and is rewritten correctly");

        // An unknown schema is a miss, not an error — forward compatibility is free.
        patchSidecar("schema", QJsonValue(QStringLiteral("pinpoint.swingsummary/2")));
        check(!SwingDocReader::readSwingSummary(dir).fromSidecar, "unknown schema → miss, not error");
        check(SwingDocReader::readSwingSummary(dir).ok,           "…and still returns good data");
    }

    std::printf("\n=== summary sidecar: review write-through keeps it fresh ===\n");
    {
        // updateReview rewrites swing.json, changing its size+mtime. If the sidecar were
        // not refreshed here — or were stamped BEFORE the commit — every later read would
        // fall back to the full parse and the picker would quietly get slow again.
        check(SwingDocWriter::updateReview(dir, 3, QStringLiteral("after-index"),
                                           QStringLiteral("5 WOOD"), nullptr),
              "updateReview ok");
        const SwingSummary s = SwingDocReader::readSwingSummary(dir);
        check(s.fromSidecar, "sidecar still fresh after updateReview (stamped post-commit)");
        check(s.club == QStringLiteral("5 WOOD"), "sidecar carries the new club");
    }

    std::printf("\n=== summary sidecar: missing and orphan documents ===\n");
    {
        const SwingSummary none = SwingDocReader::readSwingSummary(
            QStringLiteral("/tmp/swingdoc_test_nope"));
        check(!none.ok, "no swing.json → !ok");
        check(!QFile::exists(QStringLiteral("/tmp/swingdoc_test_nope/swing_summary.json")),
              "no sidecar created for a missing document");

        // An orphan sidecar (swing.json trashed under it) must never be trusted: a
        // recreated swing_NNNN dir would otherwise show the previous occupant's data.
        const QString orphanDir = QStringLiteral("/tmp/swingdoc_test_orphan");
        QDir().mkpath(orphanDir);
        QFile::copy(sumPath, orphanDir + QStringLiteral("/swing_summary.json"));
        check(!SwingDocReader::readSwingSummary(orphanDir).ok, "orphan sidecar → !ok (fail closed)");
        QDir(orphanDir).removeRecursively();
    }

    std::printf("\n=== summary sidecar: default parity (no review block) ===\n");
    {
        // A doc that never had a review block: both paths must land on the "DRIVER" stub.
        const QString d2 = QStringLiteral("/tmp/swingdoc_test_noreview");
        QDir().mkpath(d2);
        QString werr;
        check(SwingDocWriter::writeSwingJson(d2, manifest, nullptr, &werr), "write bare doc");
        const PersistedShot fat  = SwingDocReader::readSwingJson(d2);
        const SwingSummary  lean = SwingDocReader::readSwingSummary(d2);
        check(lean.fromSidecar, "bare doc indexed at write time");
        check(lean.club == QStringLiteral("DRIVER") && fat.club == lean.club,
              "no review block → DRIVER on both paths");
        check(lean.score == fat.score, "no analysis block → score parity");
        QDir(d2).removeRecursively();
    }

    std::printf("\n=== %s (%d failures) ===\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
